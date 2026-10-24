// Builds a small ARM shared object whose exported functions trap into the
// emulator. It stands in for a guest library that is implemented on the host
// (high-level emulation): the guest's own dynamic linker loads it like any
// other library, so callers are linked to it normally.
//
// Export n is a thunk:
//
//     push {r4, lr}           @ keeps the stack 8-byte aligned
//     svc  #(kHleSvcBase + n)
//     b    loop
//   loop:
//     cmp  ip, #0
//     popeq {r4, pc}          @ ip == 0: return r0 to the caller
//     blx  ip                 @ otherwise call guest function ip(r0..r3)
//     svc  #kHleSvcReturn     @ ...and hand its result (r0) back to the host
//     b    loop
//
// so the host can call guest code (callbacks into other libraries) while it
// handles a call, without running the CPU re-entrantly. Stack arguments of
// export n are at sp + 8 when its svc traps.
//
// Imports are weak GLOB_DAT relocations into a table in the library, which
// the dynamic linker fills with the resolved addresses (0 if absent).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace didj::emu::hle {

constexpr uint32_t kHleSvcBase = 0x0d1000;    // + export index
constexpr uint32_t kHleSvcReturn = 0x0d1fff;  // a guest call made for the host returned
constexpr uint32_t kHleMaxExports = 0xfff;

struct StubLibrary {
    std::vector<uint8_t> image;
    uint32_t thunk_offset = 0;  // offset of export 0's thunk; each is 12 bytes
    uint32_t svc_offset(uint32_t n) const { return thunk_offset + 12 * n + 4; }
    uint32_t import_offset = 0;  // offset of the import table; one word each
};

StubLibrary build_stub_library(const std::string& soname, const std::vector<std::string>& exports,
                               const std::vector<std::string>& imports);

}  // namespace didj::emu::hle
