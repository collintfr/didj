// Reads ZIP archives: LeapFrog's .lfp packages (games and Micromods
// installed into the Didj's internal storage) are ZIP files.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace didj::pkg {

class ZipArchive {
public:
    struct Entry {
        std::string name;  // '/'-separated; directories end in '/'
        uint16_t method = 0;  // 0 stored, 8 deflated
        uint32_t crc = 0, compressed = 0, size = 0;
        uint32_t local_offset = 0;
    };

    // False with a message if the file is not a readable ZIP archive.
    bool open(const std::filesystem::path& path, std::string* err);
    // Also usable on an archive already in memory.
    bool open(std::vector<uint8_t> data, std::string* err);

    const std::vector<Entry>& entries() const { return entries_; }
    // Decompresses an entry and checks its CRC.
    bool read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const;
    // Writes every file under dir, refusing names that would leave it.
    bool extract_all(const std::filesystem::path& dir, std::string* err) const;

private:
    std::vector<uint8_t> data_;
    std::vector<Entry> entries_;
};

}  // namespace didj::pkg
