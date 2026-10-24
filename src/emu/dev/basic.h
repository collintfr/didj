// Standard character devices and /proc files every Linux program expects.
#pragma once

#include <memory>
#include <string>

#include "emu/vfs.h"

namespace didj::emu {

// Registers /dev/null, zero, (u)random, console/tty, and a few /proc files.
void register_basic_devices(Vfs& vfs);

// Host console streams for descriptors 0, 1 and 2.
std::shared_ptr<OpenFile> make_console(int host_stream);

// A read-only file whose contents are fixed at open time.
std::shared_ptr<OpenFile> make_text_file(std::string text);

}  // namespace didj::emu
