// Reads the dynamic-linking view of a 32-bit little-endian ELF file:
// NEEDED libraries, SONAME, interpreter, and the dynamic symbol table with
// symbol versions. This is what the Didj's ld-uClibc sees, and what the
// symbol-coverage check compares across libraries.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace didj::elf {

struct DynSymbol {
    std::string name;
    std::string version;   // "" if unversioned, e.g. "GLIBCXX_3.4"
    bool defined = false;  // st_shndx != SHN_UNDEF
    bool weak = false;
    uint8_t type = 0;      // STT_*
    uint32_t value = 0;
    uint32_t size = 0;
};

struct DynamicInfo {
    uint16_t machine = 0;     // e_machine (40 = ARM)
    uint32_t flags = 0;       // e_flags (EABI version in the top byte on ARM)
    uint16_t elf_type = 0;    // ET_EXEC = 2, ET_DYN = 3
    uint32_t entry = 0;
    std::string interpreter;  // PT_INTERP, empty for shared libraries
    std::string soname;
    std::vector<std::string> needed;
    std::vector<DynSymbol> symbols;  // index 0 (the null symbol) is omitted
};

// Parses an in-memory ELF image. On failure returns nullopt and sets *err.
std::optional<DynamicInfo> parse_dynamic(const std::vector<uint8_t>& image, std::string* err);

bool is_elf(const std::vector<uint8_t>& image);

}  // namespace didj::elf
