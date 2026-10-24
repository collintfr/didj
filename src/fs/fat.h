// Read-only FAT12/16/32 reader for raw Didj dumps.
//
// The "Didj OS" dump is a bare FAT16 volume and cartridge dumps are bare FAT32
// volumes (no partition table), but images with an MBR are accepted too.
// Not thread-safe: callers must serialize access to one FatImage.
#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace didj::fs {

enum class FatType { Fat12, Fat16, Fat32 };

const char* fat_type_name(FatType t);

struct DirEntry {
    std::string name;            // long name if present, else 8.3 name
    std::string short_name;      // 8.3 name as stored on disk ("APP.SO")
    bool is_dir = false;
    uint32_t first_cluster = 0;  // 0 = FAT12/16 fixed root directory (or empty file)
    uint32_t size = 0;
};

class FatImage {
public:
    // Opens an image file. On failure returns nullptr and sets *err.
    static std::unique_ptr<FatImage> open(const std::string& path, std::string* err);

    FatType type() const { return type_; }
    const std::string& label() const { return label_; }
    uint32_t cluster_size() const { return bytes_per_cluster_; }
    uint32_t cluster_count() const { return cluster_count_; }

    // Paths are '/'-separated and matched case-insensitively, like the Didj
    // kernel's vfat mount. "" and "/" denote the root directory.
    std::optional<DirEntry> stat(std::string_view path);
    std::optional<std::vector<DirEntry>> list(std::string_view path);
    std::vector<DirEntry> list(const DirEntry& dir);

    // Reads up to len bytes at offset; returns the number of bytes read.
    size_t read(const DirEntry& file, uint64_t offset, void* buf, size_t len);
    std::optional<std::vector<uint8_t>> read_file(std::string_view path);

private:
    FatImage() = default;
    bool init(std::string* err);
    bool read_raw(uint64_t offset, void* buf, size_t len);
    uint32_t next_cluster(uint32_t cluster);
    bool is_eoc(uint32_t cluster) const;
    const std::vector<uint32_t>& chain(uint32_t first_cluster);
    uint64_t cluster_offset(uint32_t cluster) const;
    std::vector<uint8_t> read_dir_bytes(const DirEntry& dir);
    DirEntry root_entry() const;

    std::ifstream file_;
    uint64_t volume_offset_ = 0;
    FatType type_ = FatType::Fat16;
    std::string label_;
    uint32_t bytes_per_sector_ = 0;
    uint32_t bytes_per_cluster_ = 0;
    uint64_t fat_offset_ = 0;
    uint64_t root_dir_offset_ = 0;  // FAT12/16 only
    uint32_t root_dir_bytes_ = 0;   // FAT12/16 only
    uint32_t root_cluster_ = 0;     // FAT32 only
    uint64_t data_offset_ = 0;
    uint32_t cluster_count_ = 0;
    std::vector<uint8_t> fat_;      // first FAT, loaded whole (at most a few MB)
    std::unordered_map<uint32_t, std::vector<uint32_t>> chains_;
};

}  // namespace didj::fs
