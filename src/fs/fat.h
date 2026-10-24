// Read-only FAT12/16/32 reader for raw Didj dumps.
//
// The "Didj OS" dump is a bare FAT16 volume and cartridge dumps are bare FAT32
// volumes (no partition table), but images with an MBR are accepted too.
// Names match case-insensitively, by long name or 8.3 alias, as on the Didj's
// vfat mounts. Not thread-safe: callers must serialize access.
#pragma once

#include <fstream>
#include <unordered_map>

#include "fs/filesystem.h"

namespace didj::fs {

enum class FatType { Fat12, Fat16, Fat32 };

const char* fat_type_name(FatType t);

class FatImage : public FileSystem {
public:
    // Opens an image file. On failure returns nullptr and sets *err.
    static std::unique_ptr<FatImage> open(const std::string& path, std::string* err);

    FatType type() const { return type_; }
    const std::string& label() const { return label_; }
    uint32_t cluster_size() const { return bytes_per_cluster_; }
    uint32_t cluster_count() const { return cluster_count_; }

    std::string describe() const override;
    Entry root() override;
    std::vector<Entry> list(const Entry& dir) override;
    size_t read(const Entry& file, uint64_t offset, void* buf, size_t len) override;
    bool case_insensitive() const override { return true; }
    using FileSystem::list;

private:
    FatImage() = default;
    bool init(std::string* err);
    bool read_raw(uint64_t offset, void* buf, size_t len);
    uint32_t next_cluster(uint32_t cluster);
    bool is_eoc(uint32_t cluster) const;
    const std::vector<uint32_t>& chain(uint32_t first_cluster);
    uint64_t cluster_offset(uint32_t cluster) const;
    std::vector<uint8_t> read_dir_bytes(const Entry& dir);

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
