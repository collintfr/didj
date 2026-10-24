// Loads an ARM ELF executable and its interpreter (ld-uClibc.so.0) into
// guest memory and builds the initial stack, as the Linux kernel's
// binfmt_elf does. The guest's own dynamic loader does the linking.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace didj::emu {

class GuestMemory;

struct ElfImage {
    uint32_t bias = 0;       // load address minus link address (0 for ET_EXEC)
    uint32_t entry = 0;      // relocated entry point
    uint32_t phdr = 0;       // guest address of the program headers
    uint32_t phnum = 0, phent = 0;
    uint32_t end = 0;        // highest mapped address (page aligned), start of brk
    std::string interpreter; // PT_INTERP, empty if static
};

// Maps PT_LOAD segments. ET_DYN images go at a free address (from the mmap
// base); ET_EXEC images at their link address.
bool load_elf(GuestMemory& mem, const std::vector<uint8_t>& file, const std::string& name, ElfImage& out,
              std::string* err);

struct StartInfo {
    uint32_t sp = 0;
    uint32_t pc = 0;
};

// Maps the stack below kTaskSize and writes argc/argv/envp/auxv.
bool setup_stack(GuestMemory& mem, const ElfImage& exe, const ElfImage* interp, const std::vector<std::string>& argv,
                 const std::vector<std::string>& envp, StartInfo& out, std::string* err);

constexpr uint32_t kStackSize = 8u << 20;

}  // namespace didj::emu
