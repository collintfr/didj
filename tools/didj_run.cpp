// didj-run: runs a program from the Didj's root filesystem, headless.
//
//   didj-run --rootfs erootfs.jffs2 [--os "Didj OS"] [--cart SNC] [--data DIR]
//            [--strace] [--stub-devices] [--env NAME=VALUE]... [--cwd DIR] -- PROGRAM [ARGS...]
//
// --stub-devices answers ioctls on device nodes that have no driver yet and
// logs them, which shows what a program expects from the hardware.
//
// The images are mounted read-only at /, /Didj and /Cart; writes land in
// DIR (default ./didj-data). /tmp is a host directory under DIR.
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "emu/dev/basic.h"
#include "emu/dev/board.h"
#include "emu/dev/input.h"
#include "emu/dev/lf1000_display.h"
#include "emu/dev/oss.h"
#include "emu/process.h"
#include "emu/vfs.h"
#include "fs/filesystem.h"
#include "util/png.h"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: didj-run --rootfs IMAGE [--os IMAGE] [--cart IMAGE] [--data DIR] [--strace] [--stub-devices]\n"
                 "                [--run-seconds N] [--screenshot FILE.png]\n"
                 "                [--env NAME=VALUE]... [--cwd DIR] -- PROGRAM [ARGS...]\n");
    return 2;
}

std::shared_ptr<didj::fs::FileSystem> open_or_die(const std::string& path) {
    std::string err;
    auto fs = didj::fs::open_filesystem(path, &err);
    if (!fs) {
        std::fprintf(stderr, "didj-run: %s: %s\n", path.c_str(), err.c_str());
        std::exit(1);
    }
    return std::shared_ptr<didj::fs::FileSystem>(std::move(fs));
}

}  // namespace

int main(int argc, char** argv) {
    std::string rootfs, os, cart, data = "didj-data", cwd = "/";
    bool stub_devices = false;
    std::string screenshot;
    double run_seconds = 0;
    std::vector<std::string> env = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/", "TERM=vt100", "USER=root"};
    didj::emu::ProcessOptions opts;
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
        if (a == "--rootfs") rootfs = next();
        else if (a == "--os") os = next();
        else if (a == "--cart") cart = next();
        else if (a == "--data") data = next();
        else if (a == "--cwd") cwd = next();
        else if (a == "--env") env.push_back(next());
        else if (a == "--strace") opts.strace = true;
        else if (a == "--stub-devices") stub_devices = true;
        else if (a == "--slice-us") opts.slice_us = uint32_t(std::atoi(next().c_str()));
        else if (a == "--screenshot") screenshot = next();
        else if (a == "--run-seconds") run_seconds = std::atof(next().c_str());
        else return usage();
    }
    if (rootfs.empty() || i >= argc) return usage();
    std::vector<std::string> args(argv + i, argv + argc);

    didj::emu::Vfs vfs;
    std::filesystem::path dir(data);
    vfs.mount("/", open_or_die(rootfs), dir / "rootfs");
    if (!os.empty()) vfs.mount("/Didj", open_or_die(os), dir / "Didj");
    if (!cart.empty()) vfs.mount("/Cart", open_or_die(cart), dir / "Cart");
    vfs.mount_host("/tmp", dir / "tmp");
    didj::emu::register_basic_devices(vfs);
    if (stub_devices) vfs.set_fallback_device(didj::emu::make_stub_device);
    didj::emu::BoardState board;
    board.register_sysfs(vfs, !cart.empty());
    didj::emu::InputHub input;
    input.register_device(vfs);
    didj::emu::register_oss_device(vfs, std::make_shared<didj::emu::NullAudioOutput>());
    didj::emu::Lf1000Display display;
    display.register_devices(vfs);

    didj::emu::Process proc(vfs, opts);
    std::string err;
    if (!proc.exec(args[0], args, env, &err)) {
        std::fprintf(stderr, "didj-run: %s\n", err.c_str());
        return 1;
    }
    (void)cwd;
    if (run_seconds > 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(run_seconds);
        proc.on_idle = [&] {
            if (std::chrono::steady_clock::now() < deadline) return;
            proc.request_exit(0);
        };
    }
    int status = proc.run();
    if (!screenshot.empty()) {
        std::vector<uint32_t> px(didj::emu::Lf1000Display::kWidth * didj::emu::Lf1000Display::kHeight);
        display.compose(px.data());
        if (!didj::util::write_png(screenshot, didj::emu::Lf1000Display::kWidth, didj::emu::Lf1000Display::kHeight,
                                   px.data()))
            std::fprintf(stderr, "didj-run: cannot write %s\n", screenshot.c_str());
    }
    return status;
}
