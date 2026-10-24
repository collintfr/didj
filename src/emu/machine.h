// A whole emulated Didj: the dumps mounted where the Didj mounts them, the
// LF1000 devices, OpenGL ES on the host, and the guest process. Shared by
// the headless didj-run and the windowed didj.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "emu/dev/board.h"
#include "emu/dev/input.h"
#include "emu/dev/lf1000_display.h"
#include "emu/dev/oss.h"
#include "emu/hle/gles.h"
#include "emu/process.h"
#include "emu/vfs.h"

namespace didj::emu {

struct MachineConfig {
    // Images; empty rootfs/os mean the ones didj-fetch downloaded. cart may be
    // a path or the name of a fetched cartridge ("SNC"); empty means none.
    std::string rootfs, os, cart;
    std::filesystem::path data = "didj-data";  // writes to the images land here
    // .lfp game packages to install into /Didj/ProgramFiles (kept in data).
    // JetPack Heroes, built into every Didj, is added when it was fetched.
    std::vector<std::string> packages;
    bool builtin_game = true;
    bool stub_devices = false;  // answer and log ioctls on unknown devices
    bool real_gles = false;     // LeapFrog's own GLES library (needs the 3D core)
    bool gl_trace = false;
    ProcessOptions process;
    std::shared_ptr<AudioOutput> audio;  // default: silent, drained in real time
};

class Machine {
public:
    explicit Machine(MachineConfig config);
    ~Machine();

    // Mounts the images and starts program (default: RealAppManager with the
    // environment /etc/profile gives it). False with a message on failure.
    bool start(std::vector<std::string> args, std::vector<std::string> env, std::string* err);
    // Runs until the guest exits; returns its exit status.
    int run() { return proc_->run(); }

    Process& process() { return *proc_; }
    Lf1000Display& display() { return display_; }
    InputHub& input() { return input_; }
    hle::GlesHle& gles() { return *gles_; }
    const MachineConfig& config() const { return config_; }

    // The fetched image with this name, or an empty string.
    static std::string fetched(const std::string& name);

private:
    MachineConfig config_;
    Vfs vfs_;
    BoardState board_;
    InputHub input_;
    Lf1000Display display_;
    std::unique_ptr<Process> proc_;
    std::unique_ptr<hle::GlesHle> gles_;
};

}  // namespace didj::emu
