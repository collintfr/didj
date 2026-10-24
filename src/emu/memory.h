// Guest address space: host-backed regions mapped into Unicorn with
// uc_mem_map_ptr, so syscalls and HLE code can use direct host pointers.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct uc_struct;

namespace didj::emu {

constexpr uint32_t kPageSize = 0x1000;
inline uint32_t page_down(uint32_t v) { return v & ~(kPageSize - 1); }
inline uint64_t page_up(uint64_t v) { return (v + kPageSize - 1) & ~uint64_t(kPageSize - 1); }

// Linux/ARM 2.6 user layout: executables at 0x8000, mmap from 0x40000000,
// stack below TASK_SIZE (0xbf000000); the kuser helper page sits at 0xffff0000.
constexpr uint32_t kMmapBase = 0x40000000;
constexpr uint32_t kTaskSize = 0xbf000000;
constexpr uint32_t kKuserPage = 0xffff0000;

class GuestMemory {
public:
    explicit GuestMemory(uc_struct* uc) : uc_(uc) {}
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Maps zeroed memory at a fixed, page-aligned address, replacing any
    // existing mapping in the range (MAP_FIXED semantics).
    bool map_fixed(uint32_t addr, uint32_t len, const std::string& tag);
    // Maps zeroed memory at a free address at or above hint (0 = kMmapBase).
    // Returns 0 on failure.
    uint32_t map_anywhere(uint32_t len, uint32_t hint, const std::string& tag);
    // Maps caller-owned host memory (device framebuffers); owner keeps it alive.
    bool map_host(uint32_t addr, uint32_t len, uint8_t* host, std::shared_ptr<void> owner, const std::string& tag);
    void unmap(uint32_t addr, uint32_t len);
    bool is_mapped(uint32_t addr, uint32_t len) const;
    uint32_t find_free(uint32_t len, uint32_t hint) const;

    // Host pointer for [addr, addr+len) if it lies within one region, else nullptr.
    uint8_t* ptr(uint32_t addr, uint32_t len = 1) const;
    // Copies that may span regions; return false on unmapped memory.
    bool read(uint32_t addr, void* out, size_t len) const;
    bool write(uint32_t addr, const void* in, size_t len);
    bool read_u32(uint32_t addr, uint32_t& v) const { return read(addr, &v, 4); }
    bool write_u32(uint32_t addr, uint32_t v) { return write(addr, &v, 4); }
    // Reads a NUL-terminated string of at most max bytes.
    bool read_string(uint32_t addr, std::string& out, size_t max = 4096) const;

    // Drops Unicorn's translated code for a range (after code was replaced).
    void invalidate_code(uint32_t addr, uint32_t len);

    struct RegionInfo {
        uint32_t base, size;
        std::string tag;
    };
    std::vector<RegionInfo> regions() const;

private:
    struct Region {
        uint32_t size;
        uint8_t* host;
        std::shared_ptr<void> block;  // keeps the host allocation alive
        std::string tag;
    };
    bool insert(uint32_t addr, uint32_t len, uint8_t* host, std::shared_ptr<void> block, const std::string& tag);

    uc_struct* uc_;
    std::map<uint32_t, Region> regions_;  // keyed by base address
};

}  // namespace didj::emu
