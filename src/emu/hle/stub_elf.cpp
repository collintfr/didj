#include "emu/hle/stub_elf.h"

#include <cstring>
#include <stdexcept>

namespace didj::emu::hle {

namespace {

constexpr uint32_t kPage = 0x1000;

uint32_t elf_hash(const std::string& name) {
    uint32_t h = 0;
    for (unsigned char c : name) {
        h = (h << 4) + c;
        uint32_t g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

class Writer {
public:
    std::vector<uint8_t> b;
    uint32_t pos() const { return uint32_t(b.size()); }
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint32_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(v & 0xffff), u16(v >> 16); }
    void align(uint32_t a) {
        while (b.size() % a) b.push_back(0);
    }
    void put32(uint32_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
    }
};

}  // namespace

StubLibrary build_stub_library(const std::string& soname, const std::vector<std::string>& exports,
                               const std::vector<std::string>& imports) {
    if (exports.size() > kHleMaxExports) throw std::length_error("too many HLE exports");

    // String table.
    std::string strtab(1, '\0');
    auto add_str = [&](const std::string& s) {
        uint32_t off = uint32_t(strtab.size());
        strtab += s;
        strtab += '\0';
        return off;
    };
    const uint32_t so_name = add_str(soname), libc_name = add_str("libc.so.0");
    std::vector<std::string> names;  // dynsym order after the null symbol
    for (const auto& s : imports) names.push_back(s);
    for (const auto& s : exports) names.push_back(s);
    std::vector<uint32_t> name_off;
    for (const auto& s : names) name_off.push_back(add_str(s));
    const uint32_t nsyms = uint32_t(names.size()) + 1;

    Writer w;
    w.b.resize(52 + 3 * 32);  // ELF header and program headers, filled in below

    // .hash
    w.align(4);
    const uint32_t hash = w.pos();
    const uint32_t nbucket = nsyms < 8 ? 1 : nsyms / 2 + 1;
    std::vector<uint32_t> bucket(nbucket, 0), chain(nsyms, 0);
    for (uint32_t i = 1; i < nsyms; ++i) {
        uint32_t h = elf_hash(names[i - 1]) % nbucket;
        chain[i] = bucket[h];
        bucket[h] = i;
    }
    w.u32(nbucket);
    w.u32(nsyms);
    for (uint32_t v : bucket) w.u32(v);
    for (uint32_t v : chain) w.u32(v);

    // .dynsym; export values are patched once .text is placed.
    const uint32_t dynsym = w.pos();
    for (int i = 0; i < 16; ++i) w.u8(0);
    std::vector<uint32_t> sym_value_at;
    for (uint32_t i = 0; i < names.size(); ++i) {
        const bool imp = i < imports.size();
        w.u32(name_off[i]);
        sym_value_at.push_back(w.pos());
        w.u32(0);                                  // st_value
        w.u32(imp ? 0 : 12);                       // st_size
        w.u8(imp ? (2 << 4 | 2) : (1 << 4 | 2));   // STB_WEAK/STB_GLOBAL, STT_FUNC
        w.u8(0);
        w.u16(imp ? 0 : 1);                        // SHN_UNDEF or "defined"
    }

    // .dynstr
    const uint32_t dynstr = w.pos();
    for (char c : strtab) w.u8(uint8_t(c));
    w.align(4);

    // .rel.dyn: one GLOB_DAT per import, patched once the table is placed.
    const uint32_t rel = w.pos();
    for (uint32_t i = 0; i < imports.size(); ++i) {
        w.u32(0);
        w.u32((i + 1) << 8 | 21);  // R_ARM_GLOB_DAT
    }
    const uint32_t relsz = w.pos() - rel;

    // .text: the thunks, then the shared loop.
    w.align(8);
    const uint32_t text = w.pos();
    const uint32_t loop = text + 12 * uint32_t(exports.size());
    auto branch = [](uint32_t from, uint32_t to) { return 0xea000000u | (((to - from - 8) >> 2) & 0xffffff); };
    for (uint32_t n = 0; n < exports.size(); ++n) {
        const uint32_t at = w.pos();
        w.u32(0xe92d4010);                    // push {r4, lr}
        w.u32(0xef000000 | (kHleSvcBase + n)); // svc #base+n
        w.u32(branch(at + 8, loop));          // b loop
        w.put32(sym_value_at[imports.size() + n], at);
    }
    w.u32(0xe35c0000);                     // cmp ip, #0
    w.u32(0x08bd8010);                     // popeq {r4, pc}
    w.u32(0xe12fff3c);                     // blx ip
    w.u32(0xef000000 | kHleSvcReturn);     // svc #return
    w.u32(branch(loop + 16, loop));        // b loop
    const uint32_t text_end = w.pos();

    // Writable segment on its own page: .dynamic then the import table.
    w.align(kPage);
    const uint32_t data = w.pos();
    const uint32_t dyn_entries[][2] = {
        {1, libc_name},  {14, so_name}, {4, hash},  {5, dynstr}, {6, dynsym}, {10, uint32_t(strtab.size())},
        {11, 16},        {17, rel},     {18, relsz}, {19, 8},     {0, 0}};
    for (const auto& e : dyn_entries) w.u32(e[0]), w.u32(e[1]);
    const uint32_t dynamic_size = w.pos() - data;
    const uint32_t table = w.pos();
    for (uint32_t i = 0; i < imports.size(); ++i) {
        w.put32(rel + 8 * i, table + 4 * i);
        w.u32(0);
    }
    w.u32(0);  // keeps the segment non-empty
    const uint32_t end = w.pos();

    // ELF header.
    std::vector<uint8_t>& b = w.b;
    const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0};
    std::memcpy(b.data(), ident, 16);
    Writer h;
    h.u16(3);           // ET_DYN
    h.u16(40);          // EM_ARM
    h.u32(1);           // EV_CURRENT
    h.u32(0);           // e_entry
    h.u32(52);          // e_phoff
    h.u32(0);           // e_shoff
    h.u32(0x04000002);  // EABI version 4, as the original
    h.u16(52), h.u16(32), h.u16(3), h.u16(40), h.u16(0), h.u16(0);
    // PT_LOAD r-x, PT_LOAD rw-, PT_DYNAMIC
    const uint32_t ph[3][8] = {{1, 0, 0, 0, text_end, text_end, 5, kPage},
                               {1, data, data, data, end - data, end - data, 6, kPage},
                               {2, data, data, data, dynamic_size, dynamic_size, 6, 4}};
    for (const auto& p : ph)
        for (uint32_t v : p) h.u32(v);
    std::memcpy(b.data() + 16, h.b.data(), h.b.size());

    StubLibrary lib;
    lib.image = std::move(b);
    lib.thunk_offset = text;
    lib.import_offset = table;
    return lib;
}

}  // namespace didj::emu::hle
