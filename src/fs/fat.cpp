#include "fs/fat.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace didj::fs {

namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

bool looks_like_bpb(const uint8_t* s) {
    if (s[0] != 0xEB && s[0] != 0xE9) return false;
    uint32_t bps = le16(s + 11);
    uint32_t spc = s[13];
    return bps >= 512 && bps <= 4096 && is_pow2(bps) && is_pow2(spc) && le16(s + 14) != 0 && s[16] != 0;
}

std::string trim_right(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Appends one UCS-2 code unit as UTF-8.
void append_utf8(std::string& out, uint16_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

uint8_t lfn_checksum(const uint8_t* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) sum = uint8_t(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

}  // namespace

const char* fat_type_name(FatType t) {
    switch (t) {
        case FatType::Fat12: return "FAT12";
        case FatType::Fat16: return "FAT16";
        case FatType::Fat32: return "FAT32";
    }
    return "?";
}

std::unique_ptr<FatImage> FatImage::open(const std::string& path, std::string* err) {
    std::unique_ptr<FatImage> img(new FatImage());
    img->file_.open(path, std::ios::binary);
    if (!img->file_) {
        if (err) *err = "cannot open " + path;
        return nullptr;
    }
    if (!img->init(err)) return nullptr;
    return img;
}

bool FatImage::read_raw(uint64_t offset, void* buf, size_t len) {
    file_.clear();
    file_.seekg(std::streamoff(offset));
    file_.read(static_cast<char*>(buf), std::streamsize(len));
    return size_t(file_.gcount()) == len;
}

bool FatImage::init(std::string* err) {
    auto fail = [&](const char* msg) {
        if (err) *err = msg;
        return false;
    };
    uint8_t bs[512];
    if (!read_raw(0, bs, sizeof bs)) return fail("image too small");

    if (!looks_like_bpb(bs)) {
        // Try the first MBR partition.
        if (bs[510] != 0x55 || bs[511] != 0xAA) return fail("no FAT boot sector or MBR found");
        uint32_t lba = le32(bs + 446 + 8);
        if (!lba) return fail("MBR has no first partition");
        volume_offset_ = uint64_t(lba) * 512;
        if (!read_raw(volume_offset_, bs, sizeof bs) || !looks_like_bpb(bs))
            return fail("first partition is not a FAT volume");
    }

    bytes_per_sector_ = le16(bs + 11);
    uint32_t sec_per_clus = bs[13];
    uint32_t reserved = le16(bs + 14);
    uint32_t num_fats = bs[16];
    uint32_t root_entries = le16(bs + 17);
    uint32_t total = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
    uint32_t fat_sectors = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
    if (!fat_sectors || !total) return fail("invalid BPB");

    bytes_per_cluster_ = bytes_per_sector_ * sec_per_clus;
    uint32_t root_sectors = (root_entries * 32 + bytes_per_sector_ - 1) / bytes_per_sector_;
    uint64_t first_data_sector = reserved + uint64_t(num_fats) * fat_sectors + root_sectors;
    if (first_data_sector >= total) return fail("invalid BPB geometry");
    cluster_count_ = uint32_t((total - first_data_sector) / sec_per_clus);

    // Microsoft's rule: the type is decided by cluster count alone.
    if (cluster_count_ < 4085) type_ = FatType::Fat12;
    else if (cluster_count_ < 65525) type_ = FatType::Fat16;
    else type_ = FatType::Fat32;

    fat_offset_ = volume_offset_ + uint64_t(reserved) * bytes_per_sector_;
    root_dir_offset_ = fat_offset_ + uint64_t(num_fats) * fat_sectors * bytes_per_sector_;
    root_dir_bytes_ = root_sectors * bytes_per_sector_;
    data_offset_ = volume_offset_ + first_data_sector * bytes_per_sector_;

    const uint8_t* label_field;
    if (type_ == FatType::Fat32) {
        root_cluster_ = le32(bs + 44);
        label_field = bs + 71;
    } else {
        label_field = bs + 43;
    }
    label_ = trim_right(std::string(reinterpret_cast<const char*>(label_field), 11));

    fat_.resize(size_t(fat_sectors) * bytes_per_sector_);
    if (!read_raw(fat_offset_, fat_.data(), fat_.size())) return fail("cannot read FAT");

    // Prefer the volume label stored in the root directory, as mlabel/dosfslabel do.
    std::vector<uint8_t> root_dir = read_dir_bytes(root());
    for (size_t off = 0; off + 32 <= root_dir.size(); off += 32) {
        const uint8_t* e = &root_dir[off];
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == 0x0F) continue;
        if (e[11] & 0x08) {
            label_ = trim_right(std::string(reinterpret_cast<const char*>(e), 11));
            break;
        }
    }
    return true;
}

bool FatImage::is_eoc(uint32_t c) const {
    switch (type_) {
        case FatType::Fat12: return c >= 0xFF8;
        case FatType::Fat16: return c >= 0xFFF8;
        case FatType::Fat32: return c >= 0x0FFFFFF8;
    }
    return true;
}

uint32_t FatImage::next_cluster(uint32_t c) {
    switch (type_) {
        case FatType::Fat12: {
            size_t off = c + c / 2;
            if (off + 1 >= fat_.size()) return 0xFFF;
            uint16_t v = le16(&fat_[off]);
            return (c & 1) ? (v >> 4) : (v & 0xFFF);
        }
        case FatType::Fat16: {
            size_t off = size_t(c) * 2;
            if (off + 2 > fat_.size()) return 0xFFFF;
            return le16(&fat_[off]);
        }
        case FatType::Fat32: {
            size_t off = size_t(c) * 4;
            if (off + 4 > fat_.size()) return 0x0FFFFFFF;
            return le32(&fat_[off]) & 0x0FFFFFFF;
        }
    }
    return 0;
}

const std::vector<uint32_t>& FatImage::chain(uint32_t first) {
    auto it = chains_.find(first);
    if (it != chains_.end()) return it->second;
    std::vector<uint32_t> c;
    uint32_t cur = first;
    // Bounded by cluster_count_ so a corrupt (cyclic) FAT cannot loop forever.
    while (cur >= 2 && cur < cluster_count_ + 2 && !is_eoc(cur) && c.size() <= cluster_count_) {
        c.push_back(cur);
        cur = next_cluster(cur);
    }
    return chains_.emplace(first, std::move(c)).first->second;
}

uint64_t FatImage::cluster_offset(uint32_t cluster) const {
    return data_offset_ + uint64_t(cluster - 2) * bytes_per_cluster_;
}

std::string FatImage::describe() const {
    return std::string(fat_type_name(type_)) + " volume '" + label_ + "'";
}

Entry FatImage::root() {
    Entry d;
    d.name = "/";
    d.type = NodeType::Dir;
    d.mode = 040755;
    d.id = type_ == FatType::Fat32 ? root_cluster_ : 0;
    return d;
}

std::vector<uint8_t> FatImage::read_dir_bytes(const Entry& dir) {
    std::vector<uint8_t> out;
    if (dir.id == 0) {
        if (type_ == FatType::Fat32) return out;
        out.resize(root_dir_bytes_);
        if (!read_raw(root_dir_offset_, out.data(), out.size())) out.clear();
        return out;
    }
    const auto& c = chain(uint32_t(dir.id));
    out.resize(c.size() * size_t(bytes_per_cluster_));
    for (size_t i = 0; i < c.size(); ++i) {
        if (!read_raw(cluster_offset(c[i]), &out[i * bytes_per_cluster_], bytes_per_cluster_)) {
            out.resize(i * bytes_per_cluster_);
            break;
        }
    }
    return out;
}

std::vector<Entry> FatImage::list(const Entry& dir) {
    std::vector<Entry> entries;
    std::vector<uint8_t> raw = read_dir_bytes(dir);

    std::vector<uint16_t> lfn;  // accumulated long-name code units, in order
    int lfn_expected = 0;       // next sequence number we expect (counts down)
    uint8_t lfn_sum = 0;

    for (size_t off = 0; off + 32 <= raw.size(); off += 32) {
        const uint8_t* e = &raw[off];
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) {
            lfn_expected = 0;
            continue;
        }
        uint8_t attr = e[11];
        if (attr == 0x0F) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) {
                lfn.assign(size_t(seq) * 13, 0xFFFF);
                lfn_expected = seq;
                lfn_sum = e[13];
            }
            if (seq != lfn_expected || seq == 0 || e[13] != lfn_sum) {
                lfn_expected = 0;
                continue;
            }
            static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; ++k) lfn[size_t(seq - 1) * 13 + k] = le16(e + pos[k]);
            --lfn_expected;
            continue;
        }
        if (attr & 0x08) {  // volume label
            lfn_expected = 0;
            continue;
        }

        Entry d;
        uint8_t name83[11];
        std::memcpy(name83, e, 11);
        if (name83[0] == 0x05) name83[0] = 0xE5;
        std::string base = trim_right(std::string(reinterpret_cast<char*>(name83), 8));
        std::string ext = trim_right(std::string(reinterpret_cast<char*>(name83) + 8, 3));
        std::string short_name = ext.empty() ? base : base + "." + ext;

        bool have_lfn = !lfn.empty() && lfn_expected == 0 && lfn_sum == lfn_checksum(e);
        if (have_lfn) {
            for (uint16_t c : lfn) {
                if (c == 0x0000 || c == 0xFFFF) break;
                append_utf8(d.name, c);
            }
        } else {
            // Windows NT case flags: 0x08 = lowercase base, 0x10 = lowercase extension.
            uint8_t nt = e[12];
            if (nt & 0x08) for (auto& ch : base) ch = char(std::tolower((unsigned char)ch));
            if (nt & 0x10) for (auto& ch : ext) ch = char(std::tolower((unsigned char)ch));
            d.name = ext.empty() ? base : base + "." + ext;
        }
        lfn.clear();
        lfn_expected = 0;

        if (d.name != short_name) d.alt_name = std::move(short_name);
        bool is_dir = attr & 0x10;
        d.type = is_dir ? NodeType::Dir : NodeType::File;
        d.mode = is_dir ? 040755 : 0100644;
        d.id = (type_ == FatType::Fat32 ? uint32_t(le16(e + 20)) << 16 : 0) | le16(e + 26);
        d.size = is_dir ? 0 : le32(e + 28);
        if (d.name == "." || d.name == "..") continue;
        entries.push_back(std::move(d));
    }
    return entries;
}

size_t FatImage::read(const Entry& file, uint64_t offset, void* buf, size_t len) {
    if (file.type != NodeType::File || offset >= file.size || !file.id) return 0;
    len = size_t(std::min<uint64_t>(len, file.size - offset));
    const auto& c = chain(uint32_t(file.id));
    auto* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        size_t idx = size_t(pos / bytes_per_cluster_);
        if (idx >= c.size()) break;
        uint32_t in_clus = uint32_t(pos % bytes_per_cluster_);
        size_t n = std::min<size_t>(len - done, bytes_per_cluster_ - in_clus);
        // Merge runs of contiguous clusters into one read.
        while (done + n < len && idx + 1 < c.size() && c[idx + 1] == c[idx] + 1) {
            ++idx;
            n = std::min<size_t>(len - done, n + bytes_per_cluster_);
        }
        uint64_t start = cluster_offset(c[size_t(pos / bytes_per_cluster_)]) + in_clus;
        if (!read_raw(start, out + done, n)) break;
        done += n;
    }
    return done;
}

}  // namespace didj::fs
