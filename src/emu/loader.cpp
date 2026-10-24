#include "emu/loader.h"

#include <algorithm>
#include <cstring>

#include "emu/memory.h"

namespace didj::emu {

namespace {

constexpr uint32_t PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6;
constexpr uint16_t ET_EXEC = 2, ET_DYN = 3, EM_ARM = 40;

// auxv tags
constexpr uint32_t AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8,
                   AT_ENTRY = 9, AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_PLATFORM = 15,
                   AT_HWCAP = 16, AT_CLKTCK = 17;
// ARM926EJ-S: SWP, HALF, THUMB, FAST_MULT, EDSP, JAVA
constexpr uint32_t kHwcap = 1 | 2 | 4 | 16 | 128 | 256;

uint16_t rd16(const std::vector<uint8_t>& d, size_t o) { return uint16_t(d[o] | (d[o + 1] << 8)); }
uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
}

struct Phdr {
    uint32_t type, offset, vaddr, filesz, memsz;
};

}  // namespace

bool load_elf(GuestMemory& mem, const std::vector<uint8_t>& f, const std::string& name, ElfImage& out,
              std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = name + ": " + msg;
        return false;
    };
    if (f.size() < 52 || std::memcmp(f.data(), "\x7f" "ELF", 4) != 0) return fail("not an ELF file");
    if (f[4] != 1 || f[5] != 1) return fail("not a 32-bit little-endian ELF");
    uint16_t type = rd16(f, 16);
    if (rd16(f, 18) != EM_ARM) return fail("not an ARM ELF");
    if (type != ET_EXEC && type != ET_DYN) return fail("not an executable or shared object");
    uint32_t phoff = rd32(f, 28);
    uint16_t phent = rd16(f, 42), phnum = rd16(f, 44);
    if (phent < 32 || uint64_t(phoff) + uint64_t(phent) * phnum > f.size()) return fail("bad program headers");

    std::vector<Phdr> loads;
    uint32_t phdr_vaddr = 0;
    out = ElfImage();
    for (uint16_t i = 0; i < phnum; ++i) {
        size_t o = phoff + size_t(i) * phent;
        Phdr p{rd32(f, o), rd32(f, o + 4), rd32(f, o + 8), rd32(f, o + 16), rd32(f, o + 20)};
        if (p.type == PT_LOAD) loads.push_back(p);
        if (p.type == PT_PHDR) phdr_vaddr = p.vaddr;
        if (p.type == PT_INTERP && p.offset + p.filesz <= f.size()) {
            out.interpreter.assign(reinterpret_cast<const char*>(&f[p.offset]), p.filesz);
            while (!out.interpreter.empty() && out.interpreter.back() == '\0') out.interpreter.pop_back();
        }
    }
    if (loads.empty()) return fail("no loadable segments");

    uint32_t lo = page_down(loads.front().vaddr);
    uint64_t hi = 0;
    for (const auto& p : loads) hi = std::max<uint64_t>(hi, uint64_t(p.vaddr) + p.memsz);
    hi = page_up(hi);

    uint32_t bias = 0;
    if (type == ET_DYN) {
        uint32_t base = mem.find_free(uint32_t(hi - lo), kMmapBase);
        if (!base) return fail("no room to load");
        bias = base - lo;
    }
    // Map the whole span at once so the gaps between segments stay reserved.
    if (!mem.map_fixed(lo + bias, uint32_t(hi - lo), name)) return fail("cannot map segments");
    for (const auto& p : loads) {
        if (uint64_t(p.offset) + p.filesz > f.size() || p.filesz > p.memsz) return fail("segment exceeds file");
        if (!mem.write(p.vaddr + bias, &f[p.offset], p.filesz)) return fail("cannot write segment");
        // .bss is already zero: map_fixed hands out zeroed memory.
    }

    out.bias = bias;
    out.entry = rd32(f, 24) + bias;
    out.phnum = phnum;
    out.phent = phent;
    if (phdr_vaddr) {
        out.phdr = phdr_vaddr + bias;
    } else {
        // Without PT_PHDR, the headers are wherever the first segment maps phoff.
        const Phdr& first = loads.front();
        out.phdr = first.vaddr - first.offset + phoff + bias;
    }
    out.end = uint32_t(hi) + bias;
    return true;
}

bool setup_stack(GuestMemory& mem, const ElfImage& exe, const ElfImage* interp, const std::vector<std::string>& argv,
                 const std::vector<std::string>& envp, StartInfo& out, std::string* err) {
    const uint32_t top = kTaskSize;
    if (!mem.map_fixed(top - kStackSize, kStackSize, "[stack]")) {
        if (err) *err = "cannot map stack";
        return false;
    }
    uint32_t sp = top;
    auto push_str = [&](const std::string& s) {
        sp -= uint32_t(s.size() + 1);
        mem.write(sp, s.c_str(), s.size() + 1);
        return sp;
    };
    // Strings at the very top, as the kernel lays them out.
    std::vector<uint32_t> env_ptrs, arg_ptrs;
    for (auto it = envp.rbegin(); it != envp.rend(); ++it) env_ptrs.insert(env_ptrs.begin(), push_str(*it));
    for (auto it = argv.rbegin(); it != argv.rend(); ++it) arg_ptrs.insert(arg_ptrs.begin(), push_str(*it));
    uint32_t platform = push_str("v5l");

    std::vector<uint32_t> aux = {
        AT_HWCAP,  kHwcap,
        AT_PAGESZ, kPageSize,
        AT_CLKTCK, 100,
        AT_PHDR,   exe.phdr,
        AT_PHENT,  exe.phent,
        AT_PHNUM,  exe.phnum,
        AT_BASE,   interp ? interp->bias : 0,
        AT_FLAGS,  0,
        AT_ENTRY,  exe.entry,
        AT_UID,    0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0,
        AT_PLATFORM, platform,
        AT_NULL,   0,
    };
    std::vector<uint32_t> words;
    words.push_back(uint32_t(argv.size()));
    words.insert(words.end(), arg_ptrs.begin(), arg_ptrs.end());
    words.push_back(0);
    words.insert(words.end(), env_ptrs.begin(), env_ptrs.end());
    words.push_back(0);
    words.insert(words.end(), aux.begin(), aux.end());

    sp = (sp - uint32_t(words.size() * 4)) & ~7u;  // AAPCS: 8-byte aligned at entry
    mem.write(sp, words.data(), words.size() * 4);
    out.sp = sp;
    out.pc = interp ? interp->entry : exe.entry;
    return true;
}

}  // namespace didj::emu
