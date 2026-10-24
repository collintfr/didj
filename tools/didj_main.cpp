// didj: the Didj in a window. Boots the Didj's home screen with a cartridge
// inserted; keyboard and game controllers are the Didj's buttons, and its
// sound plays on the host.
//
//   didj [--cart SNC | --no-cart] [--rootfs IMAGE] [--os IMAGE] [--data DIR] [--scale N] [--fullscreen]
//
// Without --cart it shows a menu of the cartridges didj-fetch downloaded. If
// the dumps are not there yet, it offers to download them.
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
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "emu/machine.h"
#include "frontend/text.h"
#include "pkg/dumps.h"
#include "pkg/fetch.h"
#include "pkg/paths.h"
#include "util/png.h"

namespace {

using didj::emu::DidjButton;

int usage() {
    std::fprintf(stderr,
                 "usage: didj [--cart IMAGE|NAME | --no-cart] [--rootfs IMAGE] [--os IMAGE] [--data DIR]\n"
                 "            [--scale N] [--fullscreen]\n"
                 "Without --cart, didj lets you choose among the downloaded cartridges.\n"
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

struct Game {
    std::string title;
    std::string cart;       // empty: no cartridge
    bool download = false;  // not a game: download the missing dumps
};

// The dumps not downloaded yet, the system and JetPack Heroes first;
// os_only leaves out the cartridges.
std::vector<const didj::pkg::DumpFile*> missing_dumps(bool os_only) {
    std::vector<const didj::pkg::DumpFile*> out;
    for (const auto& d : didj::pkg::kDumps)
        if ((!os_only || !d.cartridge()) && didj::emu::Machine::fetched(d.name).empty()) out.push_back(&d);
    std::stable_partition(out.begin(), out.end(), [](const didj::pkg::DumpFile* d) { return !d->cartridge(); });
    return out;
}

// The fetched cartridges by title, then the home screen without one (which
// still has JetPack Heroes, built into the Didj), then a way to get the rest.
std::vector<Game> available_games() {
    std::vector<Game> games;
    for (const auto& d : didj::pkg::kDumps)
        if (d.cartridge() && !didj::emu::Machine::fetched(d.name).empty()) games.push_back({d.title, d.name});
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return a.title < b.title; });
    games.push_back({"No cartridge (Didj home, JetPack Heroes)", ""});
    if (!missing_dumps(false).empty()) games.push_back({"Download more games...", "", true});
    return games;
}

void draw_backdrop(didj::frontend::Frame& f, const std::string& title, const std::string& hints) {
    using namespace didj::frontend;
    for (int y = 0; y < f.height; ++y) fill_rect(f, 0, y, f.width, 1, 0x0a2a6e + uint32_t(y / 16) * 0x000203);
    fill_rect(f, 0, 0, f.width, 28, 0x06205a);
    draw_text(f, 10, 6, title, 0xffffff);
    fill_rect(f, 0, f.height - 22, f.width, 22, 0x06205a);
    draw_text(f, 10, f.height - 19, hints, 0xc8d8ff);
}

void present(SDL_Renderer* ren, SDL_Texture* tex, const std::vector<uint32_t>& px, int width, bool vsync) {
    SDL_UpdateTexture(tex, nullptr, px.data(), width * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
    if (!vsync) SDL_Delay(16);
}

std::string megabytes(uintmax_t bytes) { return std::to_string(bytes / 1000000) + " MB"; }

// Downloads the missing dumps from archive.org, after asking. first_run words the question
// for a Didj with nothing on it yet. Returns false if the window was closed.
bool run_download(SDL_Window* win, SDL_Renderer* ren, SDL_Texture* tex, bool vsync,
                  std::vector<SDL_GameController*>& pads, bool first_run) {
    using namespace didj::frontend;
    namespace fs = std::filesystem;
    constexpr int kW = int(didj::emu::Lf1000Display::kWidth), kH = int(didj::emu::Lf1000Display::kHeight);
    std::vector<uint32_t> px(size_t(kW) * kH);
    Frame f{px.data(), kW, kH};
    bool fullscreen = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    const fs::path dir = didj::pkg::default_dump_dir();

    // The worker downloads one file after another; the window shows how far
    // it is and can stop it (curl resumes the .part file next time).
    enum class State { Ask, Busy, Failed, Done } state = State::Ask;
    std::atomic<bool> cancel{false};
    std::mutex mu;
    std::vector<const didj::pkg::DumpFile*> files;
    size_t index = 0;
    bool checking = false, finished = false, ok = false;
    std::thread worker;
    auto start = [&] {
        files = missing_dumps(false);
        index = 0, finished = false, ok = false, cancel = false;
        state = files.empty() ? State::Done : State::Busy;
        if (files.empty()) return;
        worker = std::thread([&] {
            std::error_code ec;
            fs::create_directories(dir, ec);
            bool all_ok = !dir.empty() && !ec;
            std::string why = dir.empty() ? "No folder to download into." : ec ? ec.message() : "";
            for (size_t i = 0; all_ok && i < files.size() && !cancel; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mu);
                    index = i, checking = true;
                }
                didj::pkg::FetchOptions opt;
                opt.show_progress = false;
                opt.cancel = &cancel;
                opt.log = [&](const std::string& s) {
                    std::fprintf(stderr, "didj: %s\n", s.c_str());
                    std::lock_guard<std::mutex> lock(mu);
                    checking = s.rfind("download", 0) != 0;
                };
                if (!didj::pkg::fetch_dump(*files[i], dir, opt, &why)) all_ok = false;
            }
            if (!why.empty()) std::fprintf(stderr, "didj: %s\n", why.c_str());
            std::lock_guard<std::mutex> lock(mu);
            finished = true, ok = all_ok && !cancel;
        });
    };
    auto stop = [&] {
        cancel = true;
        if (worker.joinable()) worker.join();
    };

    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            bool yes = false, no = false;
            switch (e.type) {
                case SDL_QUIT: stop(); return false;
                case SDL_KEYDOWN:
                    switch (e.key.keysym.sym) {
                        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: yes = true; break;
                        case SDLK_ESCAPE: no = true; break;
                        case SDLK_F11:
                            fullscreen = !fullscreen;
                            SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            break;
                        default: break;
                    }
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(c);
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: yes = true; break;
                        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: no = true; break;
                        default: break;
                    }
                    break;
                default: break;
            }
            if (state == State::Busy) {
                if (no) stop(), state = State::Ask;
            } else if (no) {
                return true;
            } else if (yes) {
                start();
            }
        }
        if (state == State::Busy) {
            std::lock_guard<std::mutex> lock(mu);
            if (finished) {
                worker.join();
                state = ok ? State::Done : cancel ? State::Ask : State::Failed;
            }
        }
        if (state == State::Done) return true;

        const int x = 14, y0 = 46, line = text_height() + 4;
        switch (state) {
            case State::Ask:
                draw_backdrop(f, first_run ? "Welcome!" : "Download games",
                              first_run ? "X/Enter: download   Esc: quit" : "X/Enter: download   Esc: back");
                if (first_run) {
                    draw_text(f, x, y0, "The Didj's software and games", 0xffffff);
                    draw_text(f, x, y0 + line, "are downloaded from archive.org", 0xffffff);
                    draw_text(f, x, y0 + 2 * line, "the first time (about 1.2 GB).", 0xffffff);
                } else {
                    draw_text(f, x, y0, "Download the games that are", 0xffffff);
                    draw_text(f, x, y0 + line, "not here yet from archive.org.", 0xffffff);
                }
                draw_text(f, x, y0 + 4 * line, "A stopped download carries on", 0xc8d8ff);
                draw_text(f, x, y0 + 5 * line, "where it left off next time.", 0xc8d8ff);
                break;
            case State::Busy: {
                draw_backdrop(f, "Downloading", "Esc: stop");
                std::lock_guard<std::mutex> lock(mu);
                const auto& d = *files[std::min(index, files.size() - 1)];
                std::string what = d.kind == didj::pkg::DumpKind::System ? "Didj system software" : d.title;
                if (what.empty()) what = d.name;
                while (what.size() > 3 && text_width(what) > kW - 2 * x) what = what.substr(0, what.size() - 4) + "...";
                draw_text(f, x, y0, std::to_string(index + 1) + " of " + std::to_string(files.size()), 0xffffff);
                draw_text(f, x, y0 + line, what, 0xffd860);
                std::error_code ec;
                fs::path part = dir / fs::u8path(std::string(d.name) + ".part");
                uintmax_t size = fs::file_size(part, ec);
                draw_text(f, x, y0 + 2 * line, checking ? "Checking..." : ec ? "Starting..." : megabytes(size),
                          0xffffff);
                // A bar for the whole set, file by file.
                fill_rect(f, x, y0 + 4 * line, kW - 2 * x, 10, 0x06205a);
                fill_rect(f, x, y0 + 4 * line, int((kW - 2 * x) * index / files.size()), 10, 0xff9a1a);
                break;
            }
            case State::Failed:
                draw_backdrop(f, "Download failed", "X/Enter: try again   Esc: back");
                draw_text(f, x, y0, "The download did not finish.", 0xffffff);
                draw_text(f, x, y0 + line, "Check the internet connection", 0xffffff);
                draw_text(f, x, y0 + 2 * line, "and try again.", 0xffffff);
                break;
            case State::Done: break;
        }
        present(ren, tex, px, kW, vsync);
    }
}

// A menu of games in the window. Returns the index chosen, or -1 to quit.
// The last choice is remembered in the data directory.
int run_picker(SDL_Window* win, SDL_Renderer* ren, SDL_Texture* tex, bool vsync,
               std::vector<SDL_GameController*>& pads, const std::vector<Game>& games,
               const std::filesystem::path& data) {
    using namespace didj::frontend;
    constexpr int kW = int(didj::emu::Lf1000Display::kWidth), kH = int(didj::emu::Lf1000Display::kHeight);
    const auto last_file = data / "last-game.txt";
    int sel = 0;
    {
        std::ifstream in(last_file);
        std::string last;
        if (in && std::getline(in, last))
            for (size_t i = 0; i < games.size(); ++i)
                if (games[i].cart == last) sel = int(i);
    }
    std::vector<uint32_t> px(size_t(kW) * kH);
    Frame f{px.data(), kW, kH};
    const int row_h = 20, list_top = 36, rows = (kH - list_top - 28) / row_h;
    int first = 0;
    bool fullscreen = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    const bool none_fetched = std::none_of(games.begin(), games.end(), [](const Game& g) { return !g.cart.empty(); });
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int move = 0;
            bool choose = false;
            switch (e.type) {
                case SDL_QUIT: return -1;
                case SDL_KEYDOWN:
                    switch (e.key.keysym.sym) {
                        case SDLK_UP: move = -1; break;
                        case SDLK_DOWN: move = 1; break;
                        case SDLK_PAGEUP: move = -rows; break;
                        case SDLK_PAGEDOWN: move = rows; break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: choose = true; break;
                        case SDLK_ESCAPE: return -1;
                        case SDLK_F11:
                            fullscreen = !fullscreen;
                            SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                            break;
                        default: break;
                    }
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) pads.push_back(c);
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP: move = -1; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move = 1; break;
                        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: choose = true; break;
                        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: return -1;
                        default: break;
                    }
                    break;
                default: break;
            }
            sel = std::clamp(sel + move, 0, int(games.size()) - 1);
            if (choose) {
                if (games[size_t(sel)].download) return sel;
                std::error_code ec;
                std::filesystem::create_directories(data, ec);
                std::ofstream(last_file) << games[size_t(sel)].cart << "\n";
                return sel;
            }
        }
        if (sel < first) first = sel;
        if (sel >= first + rows) first = sel - rows + 1;

        draw_backdrop(f, "Choose a game", "Up/Down: choose   X/Enter: play   Esc: quit");
        if (none_fetched) draw_text(f, 10, list_top + 2 * row_h + 2, "No cartridges downloaded yet.", 0xffd860);
        for (int i = first; i < std::min(int(games.size()), first + rows); ++i) {
            const int y = list_top + (i - first) * row_h;
            if (i == sel) fill_rect(f, 6, y, kW - 12, row_h - 2, 0xff9a1a);
            std::string t = games[size_t(i)].title;
            while (t.size() > 3 && text_width(t) > kW - 28) t = t.substr(0, t.size() - 4) + "...";
            draw_text(f, 14, y + 1, t, i == sel ? 0x1a1a1a : 0xffffff);
        }
        if (first > 0) draw_text(f, kW - 16, list_top - 16, "^", 0xffffff);
        if (first + rows < int(games.size())) draw_text(f, kW - 16, kH - 40, "v", 0xffffff);
        present(ren, tex, px, kW, vsync);
    }
}

}  // namespace

int main(int argc, char** argv) {
    didj::emu::MachineConfig config;
    config.data = didj::pkg::default_data_dir();
    int scale = 3;
    bool fullscreen = false, no_cart = false;
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
        else if (a == "--no-cart") no_cart = true;
        else return usage();
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "didj: SDL: %s\n", SDL_GetError());
        return 1;
    }
    auto audio = std::make_shared<SdlAudio>();
    config.audio = audio;

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
    std::vector<SDL_GameController*> pads;

    auto quit_now = [&] {
        for (auto* c : pads) SDL_GameControllerClose(c);
        SDL_Quit();
        return 0;
    };
    // A Didj with nothing on it yet: offer to download the dumps.
    if (config.rootfs.empty() && config.os.empty() && !missing_dumps(true).empty()) {
        if (!run_download(win, ren, tex, vsync, pads, true) || !missing_dumps(true).empty()) return quit_now();
    }

    // Without --cart, choose among the cartridges didj-fetch downloaded.
    while (config.cart.empty() && !no_cart) {
        auto games = available_games();
        int pick = run_picker(win, ren, tex, vsync, pads, games, config.data);
        if (pick < 0) return quit_now();
        const Game& g = games[size_t(pick)];
        if (g.download) {
            if (!run_download(win, ren, tex, vsync, pads, false)) return quit_now();
            continue;
        }
        std::fprintf(stderr, "didj: %s\n", g.title.c_str());
        if (g.cart.empty()) break;
        config.cart = g.cart;
    }

    didj::emu::Machine machine(config);
    std::string err;
    std::vector<std::string> env = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/", "TERM=vt100", "USER=root"};
    if (!machine.start({}, env, &err)) {
        std::fprintf(stderr, "didj: %s\n", err.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "didj", err.c_str(), win);
        SDL_Quit();
        return 1;
    }

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
