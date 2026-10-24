#include "pkg/zip.h"

#include <cstdio>
#include <fstream>
#include <system_error>

#include "util/inflate.h"

namespace didj::pkg {

namespace {

uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

}  // namespace

bool ZipArchive::open(const std::filesystem::path& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *err = "cannot open";
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return open(std::move(data), err);
}

bool ZipArchive::open(std::vector<uint8_t> data, std::string* err) {
    data_ = std::move(data);
    entries_.clear();
    const auto& d = data_;
    // End of central directory: the last "PK\5\6" within 64 KB of the end.
    if (d.size() < 22) {
        *err = "not a ZIP archive";
        return false;
    }
    size_t eocd = d.size() - 22;
    const size_t stop = d.size() > 22 + 0xffff ? d.size() - 22 - 0xffff : 0;
    while (le32(&d[eocd]) != 0x06054b50) {
        if (eocd == stop) {
            *err = "not a ZIP archive";
            return false;
        }
        --eocd;
    }
    const uint32_t count = le16(&d[eocd + 10]);
    size_t p = le32(&d[eocd + 16]);
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > d.size() || le32(&d[p]) != 0x02014b50) {
            *err = "damaged central directory";
            return false;
        }
        Entry e;
        e.method = uint16_t(le16(&d[p + 10]));
        e.crc = le32(&d[p + 16]);
        e.compressed = le32(&d[p + 20]);
        e.size = le32(&d[p + 24]);
        const uint32_t name_len = le16(&d[p + 28]), extra_len = le16(&d[p + 30]), comment_len = le16(&d[p + 32]);
        e.local_offset = le32(&d[p + 42]);
        if (p + 46 + name_len > d.size()) {
            *err = "damaged central directory";
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(&d[p + 46]), name_len);
        for (char& c : e.name)
            if (c == '\\') c = '/';
        entries_.push_back(std::move(e));
        p += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

bool ZipArchive::read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const {
    const auto& d = data_;
    const size_t h = e.local_offset;
    if (h + 30 > d.size() || le32(&d[h]) != 0x04034b50) {
        *err = e.name + ": bad local header";
        return false;
    }
    const size_t start = h + 30 + le16(&d[h + 26]) + le16(&d[h + 28]);
    if (start + e.compressed > d.size()) {
        *err = e.name + ": truncated";
        return false;
    }
    out.clear();
    if (e.method == 0) {
        out.assign(d.begin() + std::ptrdiff_t(start), d.begin() + std::ptrdiff_t(start + e.compressed));
    } else if (e.method == 8) {
        if (!util::inflate_raw(&d[start], e.compressed, out, e.size) || out.size() != e.size) {
            *err = e.name + ": bad deflate data";
            return false;
        }
    } else {
        *err = e.name + ": unsupported compression method " + std::to_string(e.method);
        return false;
    }
    if (~util::crc32_raw(~0u, out.data(), out.size()) != e.crc) {
        *err = e.name + ": CRC mismatch";
        return false;
    }
    return true;
}

bool ZipArchive::extract_all(const std::filesystem::path& dir, std::string* err) const {
    std::error_code ec;
    for (const auto& e : entries_) {
        // Relative paths only, without "..".
        if (e.name.empty() || e.name[0] == '/' || e.name.find(':') != std::string::npos) continue;
        bool bad = false;
        for (size_t pos = 0; pos <= e.name.size();) {
            size_t end = e.name.find('/', pos);
            if (end == std::string::npos) end = e.name.size();
            if (e.name.compare(pos, end - pos, "..") == 0) bad = true;
            pos = end + 1;
        }
        if (bad) continue;
        const auto target = dir / std::filesystem::u8path(e.name);
        if (e.name.back() == '/') {
            std::filesystem::create_directories(target, ec);
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!read(e, bytes, err)) return false;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) {
            *err = target.u8string() + ": cannot write";
            return false;
        }
    }
    return true;
}

}  // namespace didj::pkg
