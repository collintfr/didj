#include "elf/dynamic.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace didj::elf {

namespace {

constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3;
constexpr uint32_t DT_NULL = 0, DT_NEEDED = 1, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6,
                   DT_SONAME = 14, DT_VERSYM = 0x6ffffff0, DT_VERDEF = 0x6ffffffc,
                   DT_VERDEFNUM = 0x6ffffffd, DT_VERNEED = 0x6ffffffe, DT_VERNEEDNUM = 0x6fffffff,
                   DT_GNU_HASH = 0x6ffffef5;
constexpr uint32_t SHT_DYNSYM = 11;

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}
    bool ok(uint64_t off, uint64_t n) const { return off + n <= d_.size(); }
    uint16_t u16(uint64_t off) const { return ok(off, 2) ? uint16_t(d_[off] | (d_[off + 1] << 8)) : 0; }
    uint32_t u32(uint64_t off) const {
        if (!ok(off, 4)) return 0;
        return uint32_t(d_[off]) | (uint32_t(d_[off + 1]) << 8) | (uint32_t(d_[off + 2]) << 16) |
               (uint32_t(d_[off + 3]) << 24);
    }
    std::string str(uint64_t off) const {
        std::string s;
        while (off < d_.size() && d_[off]) s += char(d_[off++]);
        return s;
    }

private:
    const std::vector<uint8_t>& d_;
};

struct Segment {
    uint32_t offset, vaddr, filesz;
};

}  // namespace

bool is_elf(const std::vector<uint8_t>& image) {
    return image.size() >= 52 && std::memcmp(image.data(), "\x7f" "ELF", 4) == 0;
}

std::optional<DynamicInfo> parse_dynamic(const std::vector<uint8_t>& image, std::string* err) {
    auto fail = [&](const char* msg) -> std::optional<DynamicInfo> {
        if (err) *err = msg;
        return std::nullopt;
    };
    if (!is_elf(image)) return fail("not an ELF file");
    if (image[4] != 1 || image[5] != 1) return fail("not a 32-bit little-endian ELF");

    Reader r(image);
    DynamicInfo info;
    info.elf_type = r.u16(16);
    info.machine = r.u16(18);
    info.entry = r.u32(24);
    info.flags = r.u32(36);
    uint32_t phoff = r.u32(28), shoff = r.u32(32);
    uint16_t phentsize = r.u16(42), phnum = r.u16(44);
    uint16_t shentsize = r.u16(46), shnum = r.u16(48);

    std::vector<Segment> loads;
    uint32_t dyn_off = 0, dyn_size = 0;
    for (uint32_t i = 0; i < phnum; ++i) {
        uint64_t ph = phoff + uint64_t(i) * phentsize;
        if (!r.ok(ph, 32)) return fail("truncated program headers");
        uint32_t type = r.u32(ph);
        if (type == PT_LOAD) loads.push_back({r.u32(ph + 4), r.u32(ph + 8), r.u32(ph + 16)});
        if (type == PT_DYNAMIC) {
            dyn_off = r.u32(ph + 4);
            dyn_size = r.u32(ph + 16);
        }
        if (type == PT_INTERP) info.interpreter = r.str(r.u32(ph + 4));
    }
    if (!dyn_off) return info;  // statically linked: nothing more to report

    auto v2o = [&](uint32_t vaddr) -> std::optional<uint32_t> {
        for (const auto& s : loads)
            if (vaddr >= s.vaddr && vaddr < s.vaddr + s.filesz) return s.offset + (vaddr - s.vaddr);
        return std::nullopt;
    };

    std::map<uint32_t, uint32_t> dyn;  // single-valued tags
    std::vector<uint32_t> needed_offs;
    uint32_t soname_off = UINT32_MAX;
    for (uint32_t off = dyn_off; off + 8 <= dyn_off + dyn_size; off += 8) {
        uint32_t tag = r.u32(off), val = r.u32(off + 4);
        if (tag == DT_NULL) break;
        if (tag == DT_NEEDED) needed_offs.push_back(val);
        else if (tag == DT_SONAME) soname_off = val;
        else dyn[tag] = val;
    }

    auto strtab_off = dyn.count(DT_STRTAB) ? v2o(dyn[DT_STRTAB]) : std::nullopt;
    auto symtab_off = dyn.count(DT_SYMTAB) ? v2o(dyn[DT_SYMTAB]) : std::nullopt;
    if (!strtab_off || !symtab_off) return fail("dynamic section lacks a string or symbol table");
    const uint32_t strtab = strtab_off.value_or(0), symtab = symtab_off.value_or(0);
    for (uint32_t n : needed_offs) info.needed.push_back(r.str(strtab + n));
    if (soname_off != UINT32_MAX) info.soname = r.str(strtab + soname_off);

    // Symbol count: DT_HASH's nchain, else the .dynsym section size, else
    // the highest index reachable through DT_GNU_HASH.
    uint32_t nsyms = 0;
    if (dyn.count(DT_HASH)) {
        if (auto h = v2o(dyn[DT_HASH])) nsyms = r.u32(*h + 4);
    }
    if (!nsyms && shoff && shentsize >= 40) {
        for (uint32_t i = 0; i < shnum; ++i) {
            uint64_t sh = shoff + uint64_t(i) * shentsize;
            if (r.u32(sh + 4) == SHT_DYNSYM) nsyms = r.u32(sh + 20) / 16;
        }
    }
    if (!nsyms && dyn.count(DT_GNU_HASH)) {
        if (auto g = v2o(dyn[DT_GNU_HASH])) {
            uint32_t nbuckets = r.u32(*g), symoffset = r.u32(*g + 4), bloom = r.u32(*g + 8);
            uint64_t buckets = *g + 16 + uint64_t(bloom) * 4;
            uint64_t chains = buckets + uint64_t(nbuckets) * 4;
            uint32_t last = 0;
            for (uint32_t b = 0; b < nbuckets; ++b) last = std::max(last, r.u32(buckets + b * 4));
            if (last >= symoffset) {
                while (r.ok(chains + uint64_t(last - symoffset) * 4, 4) &&
                       !(r.u32(chains + uint64_t(last - symoffset) * 4) & 1))
                    ++last;
                nsyms = last + 1;
            } else {
                nsyms = symoffset;
            }
        }
    }

    // Version index -> name, from both needed (imported) and defined versions.
    std::map<uint16_t, std::string> versions;
    if (dyn.count(DT_VERNEED)) {
        if (auto vn = v2o(dyn[DT_VERNEED])) {
            uint32_t off = *vn;
            for (uint32_t i = 0; i < dyn[DT_VERNEEDNUM] && r.ok(off, 16); ++i) {
                uint32_t aux = off + r.u32(off + 8);
                for (uint16_t j = 0; j < r.u16(off + 2) && r.ok(aux, 16); ++j) {
                    versions[r.u16(aux + 6) & 0x7fff] = r.str(strtab + r.u32(aux + 8));
                    uint32_t next = r.u32(aux + 12);
                    if (!next) break;
                    aux += next;
                }
                uint32_t next = r.u32(off + 12);
                if (!next) break;
                off += next;
            }
        }
    }
    if (dyn.count(DT_VERDEF)) {
        if (auto vd = v2o(dyn[DT_VERDEF])) {
            uint32_t off = *vd;
            for (uint32_t i = 0; i < dyn[DT_VERDEFNUM] && r.ok(off, 20); ++i) {
                uint16_t flags = r.u16(off + 2), ndx = r.u16(off + 4);
                uint32_t aux = off + r.u32(off + 12);
                if (!(flags & 1)) versions[ndx] = r.str(strtab + r.u32(aux));  // skip VER_FLG_BASE
                uint32_t next = r.u32(off + 16);
                if (!next) break;
                off += next;
            }
        }
    }
    std::optional<uint32_t> versym;
    if (dyn.count(DT_VERSYM)) versym = v2o(dyn[DT_VERSYM]);

    for (uint32_t i = 1; i < nsyms; ++i) {
        uint64_t s = symtab + uint64_t(i) * 16;
        if (!r.ok(s, 16)) break;
        DynSymbol sym;
        sym.name = r.str(strtab + r.u32(s));
        if (sym.name.empty()) continue;  // section symbols play no part in linking
        sym.value = r.u32(s + 4);
        sym.size = r.u32(s + 8);
        uint8_t st_info = image[s + 12];
        sym.type = st_info & 0xf;
        sym.weak = (st_info >> 4) == 2;
        sym.defined = r.u16(s + 14) != 0;
        if (versym) {
            auto it = versions.find(r.u16(*versym + uint64_t(i) * 2) & 0x7fff);
            if (it != versions.end()) sym.version = it->second;
        }
        info.symbols.push_back(std::move(sym));
    }
    return info;
}

}  // namespace didj::elf
