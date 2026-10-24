// didj: the Didj in a window. Boots the Didj's home screen with a cartridge
// inserted; keyboard and game controllers are the Didj's buttons, and its
// sound plays on the host.
//
//   didj [--cart SNC] [--rootfs IMAGE] [--os IMAGE] [--data DIR] [--scale N] [--fullscreen]
//
// The images default to the ones didj-fetch downloads. What the Didj writes
// (profiles, game saves) is kept in DIR, by default next to the dumps.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "emu/machine.h"
#include "pkg/paths.h"
#include "util/png.h"

namespace {

using didj::emu::DidjButton;

int usage() {
    std::fprintf(stderr,
                 "usage: didj [--cart IMAGE|NAME] [--rootfs IMAGE] [--os IMAGE] [--data DIR] [--scale N]\n"
                 "            [--fullscreen]\n"
                 "Images default to the didj-fetch directory (%s).\n"
                 "Keys: arrows = pad, X = A, Z = B, A/S = shoulders, Enter = pause, H = hint,\n"
                 "      Esc = home, F11 = fullscreen, F12 = screenshot.\n",
                 didj::pkg::default_dump_dir().u8string().c_str());
    return 2;
}

// Plays the guest's PCM through SDL. Until a device is open (or if none can
// be opened), audio is drained in real time like NullAudioOutput, so the
// guest's audio thread never stalls.
class SdlAudio : public didj::emu::AudioOutput {
public:
    ~SdlAudio() override { close(); }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        if (dev_) SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }

    void configure(int rate, int channels) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (dev_ && rate == rate_ && channels == channels_) return;
        if (dev_) SDL_CloseAudioDevice(dev_);
        dev_ = 0;
        rate_ = rate, channels_ = channels;
        buf_.clear();
        fallback_.configure(rate, channels);
        SDL_AudioSpec want{}, have{};
        want.freq = rate;
        want.format = AUDIO_S16SYS;
        want.channels = uint8_t(channels);
        want.samples = 512;
        want.callback = &SdlAudio::callback;
        want.userdata = this;
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!dev_) {
            std::fprintf(stderr, "didj: no audio (%s)\n", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(dev_, 0);
    }

    void write(const int16_t* samples, size_t frames) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!dev_) return fallback_.write(samples, frames);
        buf_.insert(buf_.end(), samples, samples + frames * size_t(channels_));
    }

    size_t queued_frames() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!dev_) return fallback_.queued_frames();
        return buf_.size() / size_t(channels_);
    }

private:
    static void callback(void* self, Uint8* stream, int len) {
        auto* a = static_cast<SdlAudio*>(self);
        auto* out = reinterpret_cast<int16_t*>(stream);
        size_t n = size_t(len) / 2;
        std::lock_guard<std::mutex> lock(a->mu_);
        size_t take = std::min(n, a->buf_.size());
        std::copy(a->buf_.begin(), a->buf_.begin() + std::ptrdiff_t(take), out);
        a->buf_.erase(a->buf_.begin(), a->buf_.begin() + std::ptrdiff_t(take));
        std::fill(out + take, out + n, int16_t(0));  // underrun: silence
    }

    std::mutex mu_;
    SDL_AudioDeviceID dev_ = 0;
    int rate_ = 0, channels_ = 1;
    std::deque<int16_t> buf_;
    didj::emu::NullAudioOutput fallback_;
};

bool key_button(SDL_Keycode k, DidjButton& b) {
    switch (k) {
        case SDLK_UP: b = DidjButton::Up; return true;
        case SDLK_DOWN: b = DidjButton::Down; return true;
        case SDLK_LEFT: b = DidjButton::Left; return true;
        case SDLK_RIGHT: b = DidjButton::Right; return true;
        case SDLK_x: b = DidjButton::A; return true;
        case SDLK_z: b = DidjButton::B; return true;
        case SDLK_a: b = DidjButton::LeftShoulder; return true;
        case SDLK_s: b = DidjButton::RightShoulder; return true;
        case SDLK_RETURN: case SDLK_p: b = DidjButton::Pause; return true;
        case SDLK_h: b = DidjButton::Hint; return true;
        case SDLK_ESCAPE: case SDLK_HOME: b = DidjButton::Home; return true;
        default: return false;
    }
}

bool pad_button(Uint8 button, DidjButton& b) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: b = DidjButton::Up; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: b = DidjButton::Down; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: b = DidjButton::Left; return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: b = DidjButton::Right; return true;
        case SDL_CONTROLLER_BUTTON_A: b = DidjButton::A; return true;
        case SDL_CONTROLLER_BUTTON_B: b = DidjButton::B; return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: b = DidjButton::LeftShoulder; return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: b = DidjButton::RightShoulder; return true;
        case SDL_CONTROLLER_BUTTON_START: b = DidjButton::Pause; return true;
        case SDL_CONTROLLER_BUTTON_Y: b = DidjButton::Hint; return true;
        case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_GUIDE: b = DidjButton::Home; return true;
        default: return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    didj::emu::MachineConfig config;
    config.data = didj::pkg::default_data_dir();
    int scale = 3;
    bool fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) std::exit(usage());
            return argv[++i];
        };
        if (a == "--cart") config.cart = next();
        else if (a == "--rootfs") config.rootfs = next();
        else if (a == "--os") config.os = next();
        else if (a == "--data") config.data = std::filesystem::u8path(next());
        else if (a == "--scale") scale = std::max(1, std::atoi(next().c_str()));
        else if (a == "--fullscreen") fullscreen = true;
        else return usage();
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "didj: SDL: %s\n", SDL_GetError());
        return 1;
    }
    auto audio = std::make_shared<SdlAudio>();
    config.audio = audio;

    didj::emu::Machine machine(config);
    std::string err;
    std::vector<std::string> env = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/", "TERM=vt100", "USER=root"};
    if (!machine.start({}, env, &err)) {
        std::fprintf(stderr, "didj: %s\n", err.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "didj", err.c_str(), nullptr);
        SDL_Quit();
        return 1;
    }

    constexpr int kW = int(didj::emu::Lf1000Display::kWidth), kH = int(didj::emu::Lf1000Display::kHeight);
    SDL_Window* win = SDL_CreateWindow("didj", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kW * scale, kH * scale,
                                       SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) {
        std::fprintf(stderr, "didj: cannot open a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(ren, kW, kH);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    SDL_RendererInfo info{};
    const bool vsync = SDL_GetRendererInfo(ren, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);

    // The guest runs on its own thread; closing the window asks it to stop.
    std::atomic<bool> quit{false}, guest_done{false};
    int status = 0;
    machine.process().on_idle = [&] {
        if (quit) machine.process().request_exit(0);
    };
    std::thread guest([&] {
        status = machine.run();
        guest_done = true;
    });

    std::vector<SDL_GameController*> pads;
    std::vector<uint32_t> px(size_t(kW) * kH);
    int shot = 0;
    while (!guest_done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            DidjButton b;
            switch (e.type) {
                case SDL_QUIT: quit = true; break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    if (e.key.repeat) break;
                    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
                        std::string name = "didj-" + std::to_string(++shot) + ".png";
                        machine.display().compose(px.data());
                        if (didj::util::write_png(name, uint32_t(kW), uint32_t(kH), px.data()))
                            std::fprintf(stderr, "didj: saved %s\n", name.c_str());
                    } else if (key_button(e.key.keysym.sym, b)) {
                        machine.input().press(b, e.type == SDL_KEYDOWN);
                    }
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(c);
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP:
                    if (pad_button(e.cbutton.button, b))
                        machine.input().press(b, e.type == SDL_CONTROLLERBUTTONDOWN);
                    break;
                default: break;
            }
        }
        machine.display().compose(px.data());
        SDL_UpdateTexture(tex, nullptr, px.data(), kW * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
        if (!vsync) SDL_Delay(16);
    }
    quit = true;
    guest.join();
    for (auto* c : pads) SDL_GameControllerClose(c);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    audio->close();
    SDL_Quit();
    return status;
}
