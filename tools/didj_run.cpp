// didj-run: runs a program from the Didj's root filesystem, headless.
//
//   didj-run [--rootfs erootfs.jffs2] [--os "Didj OS"] [--cart SNC] [--data DIR]
//            [--strace] [--stub-devices] [--env NAME=VALUE]... [--cwd DIR] [-- PROGRAM [ARGS...]]
//
// The images default to the ones didj-fetch downloads, and --cart also takes
// a bare cartridge name (SNC) from that directory. With no PROGRAM it starts
// the Didj's own launcher, RealAppManager, with the environment /etc/profile
// gives it.
//
// --stub-devices answers ioctls on device nodes that have no driver yet and
// logs them, which shows what a program expects from the hardware.
//
// The images are mounted read-only at /, /Didj and /Cart; writes land in
// DIR (default ./didj-data). /tmp is a host directory under DIR.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "emu/machine.h"
#include "pkg/paths.h"
#include "util/png.h"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: didj-run [--rootfs IMAGE] [--os IMAGE] [--cart IMAGE|NAME] [--data DIR] [--strace]\n"
                 "                [--stub-devices] [--gl-trace] [--real-gles] [--run-seconds N] [--screenshot FILE.png]\n"
                 "                [--gl-screenshot FILE.png] [--input T:BUTTON,...] [--screenshot-at T:FILE.png,...]\n"
                 "Buttons: up down left right a b l r home hint pause brightness.\n"
                 "                [--env NAME=VALUE]... [--cwd DIR] [-- PROGRAM [ARGS...]]\n"
                 "Images default to the didj-fetch directory (%s).\n",
                 didj::pkg::default_dump_dir().u8string().c_str());
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    didj::emu::MachineConfig config;
    std::string cwd = "/";
    std::string screenshot, gl_screenshot;
    // Scripted input and timed screenshots, in seconds from the start.
    struct Timed {
        double at;
        std::string what;
    };
    std::vector<Timed> presses, shots;
    double run_seconds = 0;
    std::vector<std::string> env = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/", "TERM=vt100", "USER=root"};
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--") {
            ++i;
            break;
        }
        if (a == "--rootfs") config.rootfs = next();
        else if (a == "--os") config.os = next();
        else if (a == "--cart") config.cart = next();
        else if (a == "--data") config.data = std::filesystem::u8path(next());
        else if (a == "--cwd") cwd = next();
        else if (a == "--env") env.push_back(next());
        else if (a == "--strace") config.process.strace = true;
        else if (a == "--stub-devices") config.stub_devices = true;
        else if (a == "--real-gles") config.real_gles = true;
        else if (a == "--gl-trace") config.gl_trace = true;
        else if (a == "--slice-us") config.process.slice_us = uint32_t(std::atoi(next().c_str()));
        else if (a == "--screenshot") screenshot = next();
        else if (a == "--gl-screenshot") gl_screenshot = next();
        else if (a == "--input" || a == "--screenshot-at") {
            // --input 3:a,4.5:right   --screenshot-at 10:menu.png
            std::string spec = next();
            for (size_t pos = 0; pos <= spec.size();) {
                size_t end = spec.find(',', pos);
                if (end == std::string::npos) end = spec.size();
                std::string item = spec.substr(pos, end - pos);
                size_t colon = item.find(':');
                if (colon == std::string::npos) return usage();
                (a == "--input" ? presses : shots).push_back({std::atof(item.c_str()), item.substr(colon + 1)});
                pos = end + 1;
            }
        }
        else if (a == "--run-seconds") run_seconds = std::atof(next().c_str());
        else return usage();
    }
    std::vector<std::string> args(argv + i, argv + argc);
    didj::emu::Machine machine(config);
    std::string err;
    if (!machine.start(args, env, &err)) {
        std::fprintf(stderr, "didj-run: %s\n", err.c_str());
        return 1;
    }
    auto& proc = machine.process();
    auto& display = machine.display();
    auto& input = machine.input();
    auto& gles = machine.gles();
    (void)cwd;
    auto dump_layers = [&] {
        std::lock_guard<std::mutex> lock(display.mutex());
        std::fprintf(stderr, "[lf1000] priority %u\n", display.priority());
        for (int l = 0; l < didj::emu::Lf1000Display::kLayers; ++l) {
            const auto& y = display.layer(l);
            std::fprintf(stderr,
                         "[lf1000] layer%d %s%s addr %x fmt %x stride %u/%u pos t%d l%d r%d b%d alpha %u%s%s "
                         "uv %x %x scale %d %d %d %d\n",
                         l, y.enabled ? "on" : "off", y.is3d ? " 3d" : "", y.address, y.format, y.hstride, y.vstride,
                         y.top, y.left, y.right, y.bottom, y.alpha, y.blend ? " blend" : "",
                         y.transparent ? " transparent" : "", y.address_u, y.address_v, y.scale[0], y.scale[1],
                         y.scale[2], y.scale[3]);
        }
    };
    auto take_screenshot = [&](const std::string& file) {
        if (config.gl_trace) dump_layers();
        std::vector<uint32_t> px(didj::emu::Lf1000Display::kWidth * didj::emu::Lf1000Display::kHeight);
        display.compose(px.data());
        if (!didj::util::write_png(file, didj::emu::Lf1000Display::kWidth, didj::emu::Lf1000Display::kHeight,
                                   px.data()))
            std::fprintf(stderr, "didj-run: cannot write %s\n", file.c_str());
    };
    // A scripted press holds the button for 150 ms.
    struct Held {
        double until;
        didj::emu::DidjButton button;
    };
    std::vector<Held> held;
    const auto start = std::chrono::steady_clock::now();
    proc.on_idle = [&] {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (auto it = presses.begin(); it != presses.end();) {
            if (it->at > now) {
                ++it;
                continue;
            }
            didj::emu::DidjButton b;
            if (didj::emu::button_from_name(it->what, b)) {
                input.press(b, true);
                held.push_back({now + 0.15, b});
            } else {
                std::fprintf(stderr, "didj-run: unknown button '%s'\n", it->what.c_str());
            }
            it = presses.erase(it);
        }
        for (auto it = held.begin(); it != held.end();) {
            if (it->until > now) {
                ++it;
                continue;
            }
            input.press(it->button, false);
            it = held.erase(it);
        }
        for (auto it = shots.begin(); it != shots.end();) {
            if (it->at > now) {
                ++it;
                continue;
            }
            take_screenshot(it->what);
            it = shots.erase(it);
        }
        if (run_seconds > 0 && now >= run_seconds) proc.request_exit(0);
    };
    int status = proc.run();
    if (!screenshot.empty()) take_screenshot(screenshot);
    if (config.gl_trace) dump_layers();
    if (!gl_screenshot.empty()) {
        // The OpenGL color buffer itself, before it reaches the display.
        const auto& r = gles.raster();
        std::vector<uint32_t> px(size_t(r.width()) * size_t(r.height()));
        for (size_t k = 0; k < px.size(); ++k) {
            uint32_t c = r.pixels()[k];
            px[k] = (c & 0xff) << 16 | (c & 0xff00) | ((c >> 16) & 0xff);
        }
        if (!didj::util::write_png(gl_screenshot, uint32_t(r.width()), uint32_t(r.height()), px.data()))
            std::fprintf(stderr, "didj-run: cannot write %s\n", gl_screenshot.c_str());
    }
    return status;
}
