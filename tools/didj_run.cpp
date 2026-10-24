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
#include <cctype>
#include <chrono>
#include <cstdio>
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
#include "pkg/dumps.h"
#include "pkg/paths.h"
#include "util/png.h"

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: didj-run [--rootfs IMAGE] [--os IMAGE] [--cart IMAGE|NAME] [--data DIR] [--strace]\n"
                 "                [--stub-devices] [--run-seconds N] [--screenshot FILE.png]\n"
                 "                [--env NAME=VALUE]... [--cwd DIR] [-- PROGRAM [ARGS...]]\n"
                 "Images default to the didj-fetch directory (%s).\n",
                 didj::pkg::default_dump_dir().u8string().c_str());
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

// A dump in the didj-fetch directory, or an empty string if it isn't there.
std::string fetched(const std::string& name) {
    auto dir = didj::pkg::default_dump_dir();
    if (dir.empty()) return {};
    auto p = dir / std::filesystem::u8path(name);
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) ? p.u8string() : std::string();
}

// --cart accepts a path or the name of a fetched cartridge.
std::string resolve_cart(const std::string& arg) {
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::u8path(arg), ec)) return arg;
    for (const auto& d : didj::pkg::kDumps) {
        if (!d.cartridge) continue;
        std::string n = d.name;
        bool same = n.size() == arg.size();
        for (size_t k = 0; same && k < n.size(); ++k) same = std::toupper((unsigned char)arg[k]) == n[k];
        if (same) {
            std::string p = fetched(n);
            if (!p.empty()) return p;
        }
    }
    return arg;
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
    if (rootfs.empty()) rootfs = fetched(didj::pkg::kRootfsName);
    if (os.empty()) os = fetched(didj::pkg::kOsName);
    if (!cart.empty()) cart = resolve_cart(cart);
    if (rootfs.empty()) {
        std::fprintf(stderr, "didj-run: no root filesystem; run didj-fetch or pass --rootfs\n");
        return usage();
    }
    std::vector<std::string> args(argv + i, argv + argc);
    if (args.empty()) {
        if (os.empty()) {
            std::fprintf(stderr, "didj-run: no Didj OS image; run didj-fetch or pass --os\n");
            return usage();
        }
        args.push_back("/Didj/Base/bin/RealAppManager");
        bool have_path = false;
        for (const auto& e : env) have_path |= e.rfind("LD_LIBRARY_PATH=", 0) == 0;
        if (!have_path) env.push_back("LD_LIBRARY_PATH=/Didj/Base/Brio/lib/:/Didj/Base/lib/");
    }

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
