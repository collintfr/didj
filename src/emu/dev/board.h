// LF1000 board state exposed through sysfs: power, cartridge and USB
// status files that the Brio modules and system scripts read.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "emu/vfs.h"

namespace didj::emu {

class BoardState {
public:
    // Registers the /sys/devices/platform/lf1000-* files.
    void register_sysfs(Vfs& vfs, bool cartridge_inserted);

    std::string get(const std::string& path);
    void set(const std::string& path, const std::string& value);

private:
    std::mutex mu_;
    std::map<std::string, std::string> values_;
};

}  // namespace didj::emu
