#include "emu/machine.h"

#include <cctype>
#include <system_error>

#include "emu/dev/basic.h"
#include "fs/filesystem.h"
#include "pkg/dumps.h"
#include "pkg/paths.h"

namespace didj::emu {

namespace {

// --cart accepts a path or the name of a fetched cartridge.
std::string resolve_cart(const std::string& arg) {
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::u8path(arg), ec)) return arg;
    for (const auto& d : pkg::kDumps) {
        if (!d.cartridge) continue;
        std::string n = d.name;
        bool same = n.size() == arg.size();
        for (size_t k = 0; same && k < n.size(); ++k) same = std::toupper((unsigned char)arg[k]) == n[k];
        if (same) {
            std::string p = Machine::fetched(n);
            if (!p.empty()) return p;
        }
    }
    return arg;
}

}  // namespace

std::string Machine::fetched(const std::string& name) {
    auto dir = pkg::default_dump_dir();
    if (dir.empty()) return {};
    auto p = dir / std::filesystem::u8path(name);
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) ? p.u8string() : std::string();
}

Machine::Machine(MachineConfig config) : config_(std::move(config)) {
    if (config_.rootfs.empty()) config_.rootfs = fetched(pkg::kRootfsName);
    if (config_.os.empty()) config_.os = fetched(pkg::kOsName);
    if (!config_.cart.empty()) config_.cart = resolve_cart(config_.cart);
    if (!config_.audio) config_.audio = std::make_shared<NullAudioOutput>();
}

Machine::~Machine() = default;

bool Machine::start(std::vector<std::string> args, std::vector<std::string> env, std::string* err) {
    auto open = [&](const std::string& path, std::shared_ptr<fs::FileSystem>& out) {
        std::string e;
        auto f = fs::open_filesystem(path, &e);
        if (!f) {
            *err = path + ": " + e;
            return false;
        }
        out = std::shared_ptr<fs::FileSystem>(std::move(f));
        return true;
    };
    if (config_.rootfs.empty()) {
        *err = "no root filesystem; run didj-fetch or pass --rootfs";
        return false;
    }
    if (args.empty()) {
        if (config_.os.empty()) {
            *err = "no Didj OS image; run didj-fetch or pass --os";
            return false;
        }
        args.push_back("/Didj/Base/bin/RealAppManager");
        bool have_path = false;
        for (const auto& e : env) have_path |= e.rfind("LD_LIBRARY_PATH=", 0) == 0;
        if (!have_path) env.push_back("LD_LIBRARY_PATH=/Didj/Base/Brio/lib/:/Didj/Base/lib/");
    }

    std::shared_ptr<fs::FileSystem> rootfs, os, cart;
    if (!open(config_.rootfs, rootfs)) return false;
    if (!config_.os.empty() && !open(config_.os, os)) return false;
    if (!config_.cart.empty() && !open(config_.cart, cart)) return false;
    const auto& dir = config_.data;
    vfs_.mount("/", rootfs, dir / "rootfs");
    if (os) vfs_.mount("/Didj", os, dir / "Didj");
    if (cart) vfs_.mount("/Cart", cart, dir / "Cart");
    vfs_.mount_host("/tmp", dir / "tmp");

    register_basic_devices(vfs_);
    if (config_.stub_devices) vfs_.set_fallback_device(make_stub_device);
    board_.register_sysfs(vfs_, cart != nullptr);
    input_.register_device(vfs_);
    input_.set_switch(1, cart != nullptr);  // the cartridge switch
    register_oss_device(vfs_, config_.audio);
    display_.register_devices(vfs_);

    proc_ = std::make_unique<Process>(vfs_, config_.process);
    // OpenGL ES runs on the host unless LeapFrog's own library is asked for,
    // which needs the (unemulated) 3D core.
    gles_ = std::make_unique<hle::GlesHle>(*proc_, display_);
    gles_->trace = config_.gl_trace;
    if (!config_.real_gles) {
        gles_->install(vfs_);
        proc_->on_hle_svc = [this](uint32_t imm) { gles_->on_svc(imm); };
    }
    return proc_->exec(args[0], args, env, err);
}

}  // namespace didj::emu
