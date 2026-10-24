#include "emu/memory.h"

#include <cstdlib>
#include <cstring>
#include <unicorn/unicorn.h>

namespace didj::emu {

namespace {

std::shared_ptr<void> alloc_zeroed(size_t len) {
    // calloc lets the OS hand out lazily-zeroed pages, so large sparse
    // mappings (stacks, heaps) cost nothing until touched.
    void* p = std::calloc(len, 1);
    if (!p) return nullptr;
    return std::shared_ptr<void>(p, [](void* q) { std::free(q); });
}

}  // namespace

bool GuestMemory::insert(uint32_t addr, uint32_t len, uint8_t* host, std::shared_ptr<void> block,
                         const std::string& tag) {
    if (uc_mem_map_ptr(uc_, addr, len, UC_PROT_ALL, host) != UC_ERR_OK) return false;
    regions_[addr] = Region{len, host, std::move(block), tag};
    invalidate_code(addr, len);
    return true;
}

bool GuestMemory::map_fixed(uint32_t addr, uint32_t len, const std::string& tag) {
    if (addr % kPageSize || !len) return false;
    uint64_t size = page_up(len);
    if (uint64_t(addr) + size > 0x100000000ull) return false;
    unmap(addr, uint32_t(size));
    auto block = alloc_zeroed(size_t(size));
    if (!block) return false;
    return insert(addr, uint32_t(size), static_cast<uint8_t*>(block.get()), block, tag);
}

bool GuestMemory::map_host(uint32_t addr, uint32_t len, uint8_t* host, std::shared_ptr<void> owner,
                           const std::string& tag) {
    if (addr % kPageSize || len % kPageSize || !len) return false;
    unmap(addr, len);
    return insert(addr, len, host, std::move(owner), tag);
}

uint32_t GuestMemory::find_free(uint32_t len, uint32_t hint) const {
    const uint64_t size = page_up(len);
    auto scan = [&](uint64_t cand) -> uint64_t {
        for (const auto& [base, r] : regions_) {  // ascending
            uint64_t rb = base, re = rb + r.size;
            if (re <= cand) continue;
            if (rb >= cand + size) break;  // the gap before this region fits
            cand = page_up(re);
        }
        return cand + size <= kTaskSize ? cand : 0;
    };
    uint64_t addr = scan(hint ? page_down(hint) : kMmapBase);
    if (!addr && hint) addr = scan(kMmapBase);
    return uint32_t(addr);
}

uint32_t GuestMemory::map_anywhere(uint32_t len, uint32_t hint, const std::string& tag) {
    uint32_t addr = find_free(len, hint);
    if (!addr || !map_fixed(addr, len, tag)) return 0;
    return addr;
}

void GuestMemory::unmap(uint32_t addr, uint32_t len) {
    uint64_t start = page_down(addr), end = page_up(uint64_t(addr) + len);
    auto it = regions_.upper_bound(uint32_t(start));
    if (it != regions_.begin()) --it;
    while (it != regions_.end() && it->first < end) {
        uint64_t rbase = it->first, rend = rbase + it->second.size;
        if (rend <= start) {
            ++it;
            continue;
        }
        Region r = it->second;
        it = regions_.erase(it);
        uc_mem_unmap(uc_, rbase, size_t(r.size));
        // Re-map the pieces that survive on either side, sharing the block.
        if (rbase < start) {
            uc_mem_map_ptr(uc_, rbase, size_t(start - rbase), UC_PROT_ALL, r.host);
            regions_[uint32_t(rbase)] = Region{uint32_t(start - rbase), r.host, r.block, r.tag};
        }
        if (rend > end) {
            uint8_t* h = r.host + (end - rbase);
            uc_mem_map_ptr(uc_, end, size_t(rend - end), UC_PROT_ALL, h);
            regions_[uint32_t(end)] = Region{uint32_t(rend - end), h, r.block, r.tag};
        }
        it = regions_.lower_bound(uint32_t(std::min<uint64_t>(rend, 0xffffffffull)));
        if (rend > 0xffffffffull) break;
    }
    invalidate_code(uint32_t(start), uint32_t(end - start));
}

bool GuestMemory::is_mapped(uint32_t addr, uint32_t len) const {
    uint64_t cur = addr, end = uint64_t(addr) + len;
    while (cur < end) {
        auto it = regions_.upper_bound(uint32_t(cur));
        if (it == regions_.begin()) return false;
        --it;
        uint64_t rend = uint64_t(it->first) + it->second.size;
        if (rend <= cur) return false;
        cur = rend;
    }
    return true;
}

uint8_t* GuestMemory::ptr(uint32_t addr, uint32_t len) const {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return nullptr;
    --it;
    uint64_t off = uint64_t(addr) - it->first;
    if (off + len > it->second.size) return nullptr;
    return it->second.host + off;
}

bool GuestMemory::read(uint32_t addr, void* out, size_t len) const {
    auto* o = static_cast<uint8_t*>(out);
    while (len) {
        auto it = regions_.upper_bound(addr);
        if (it == regions_.begin()) return false;
        --it;
        uint64_t off = uint64_t(addr) - it->first;
        if (off >= it->second.size) return false;
        size_t n = std::min<size_t>(len, size_t(it->second.size - off));
        std::memcpy(o, it->second.host + off, n);
        o += n;
        len -= n;
        addr += uint32_t(n);
    }
    return true;
}

bool GuestMemory::write(uint32_t addr, const void* in, size_t len) {
    const auto* s = static_cast<const uint8_t*>(in);
    while (len) {
        auto it = regions_.upper_bound(addr);
        if (it == regions_.begin()) return false;
        --it;
        uint64_t off = uint64_t(addr) - it->first;
        if (off >= it->second.size) return false;
        size_t n = std::min<size_t>(len, size_t(it->second.size - off));
        std::memcpy(it->second.host + off, s, n);
        s += n;
        len -= n;
        addr += uint32_t(n);
    }
    return true;
}

bool GuestMemory::read_string(uint32_t addr, std::string& out, size_t max) const {
    out.clear();
    while (out.size() < max) {
        const uint8_t* p = ptr(addr);
        if (!p) return false;
        if (!*p) return true;
        out += char(*p);
        ++addr;
    }
    return false;
}

void GuestMemory::invalidate_code(uint32_t addr, uint32_t len) {
    if (len) uc_ctl_remove_cache(uc_, addr, uint64_t(addr) + len);
}

std::vector<GuestMemory::RegionInfo> GuestMemory::regions() const {
    std::vector<RegionInfo> out;
    for (const auto& [base, r] : regions_) out.push_back({base, r.size, r.tag});
    return out;
}

}  // namespace didj::emu
