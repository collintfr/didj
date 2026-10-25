// Unit tests. Images are synthesized here so the tests need no Didj dumps.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "elf/dynamic.h"
#include "fs/fat.h"
#include "fs/jffs2.h"
#include "pkg/metainf.h"
#include "pkg/zip.h"
#include "util/inflate.h"
#include "util/md5.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

void put16(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = uint8_t(v);
    b[off + 1] = uint8_t(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    put16(b, off, v & 0xffff);
    put16(b, off + 2, v >> 16);
}

// Builds a small FAT volume in memory, laid out as mkdosfs would.
class FatBuilder {
public:
    FatBuilder(didj::fs::FatType type, uint32_t clusters, uint32_t lead_in = 0)
        : type_(type), lead_in_(lead_in) {
        reserved_ = type == didj::fs::FatType::Fat32 ? 32 : 1;
        root_entries_ = type == didj::fs::FatType::Fat32 ? 0 : 512;
        uint32_t bits = type == didj::fs::FatType::Fat12 ? 12 : type == didj::fs::FatType::Fat16 ? 16 : 32;
        fat_sectors_ = ((clusters + 2) * bits / 8 + 511) / 512 + 1;
        uint32_t root_sectors = root_entries_ * 32 / 512;
        data_sector_ = reserved_ + 2 * fat_sectors_ + root_sectors;
        total_ = data_sector_ + clusters;
        img_.assign(size_t(lead_in_) + size_t(total_) * 512, 0);
        next_free_ = 2;
        set_fat(0, 0x0FFFFFF8);
        set_fat(1, 0x0FFFFFFF);
        if (type == didj::fs::FatType::Fat32) {
            root_cluster_ = alloc_chain(1)[0];
        }
    }

    // Allocates n clusters; if gap, leaves a free cluster between each to fragment the chain.
    std::vector<uint32_t> alloc_chain(uint32_t n, bool gap = false) {
        std::vector<uint32_t> c;
        for (uint32_t i = 0; i < n; ++i) {
            c.push_back(next_free_);
            next_free_ += gap ? 2 : 1;
        }
        for (size_t i = 0; i < c.size(); ++i) set_fat(c[i], i + 1 < c.size() ? c[i + 1] : 0x0FFFFFFF);
        return c;
    }

    uint8_t* cluster(uint32_t c) { return &img_[lead_in_ + (size_t(data_sector_) + (c - 2)) * 512]; }

    // Directory handle: first cluster (0 = fixed root on FAT12/16) and next slot.
    struct Dir {
        uint32_t first = 0;
        size_t slot = 0;
    };
    Dir root() { return {type_ == didj::fs::FatType::Fat32 ? root_cluster_ : 0, 0}; }

    void add_raw_entry(Dir& d, const uint8_t* e) {
        uint8_t* base = d.first ? cluster(d.first) : &img_[lead_in_ + size_t(reserved_ + 2 * fat_sectors_) * 512];
        std::memcpy(base + d.slot * 32, e, 32);
        ++d.slot;
    }

    void add_entry(Dir& d, const char* name83, uint8_t attr, uint32_t first, uint32_t size,
                   const std::string& long_name = "", uint8_t nt_flags = 0) {
        if (!long_name.empty()) {
            uint8_t sum = 0;
            for (int i = 0; i < 11; ++i) sum = uint8_t(((sum & 1) << 7) + (sum >> 1) + uint8_t(name83[i]));
            size_t n = (long_name.size() + 1 + 12) / 13;
            for (size_t seq = n; seq >= 1; --seq) {
                uint8_t e[32] = {};
                e[0] = uint8_t(seq | (seq == n ? 0x40 : 0));
                e[11] = 0x0F;
                e[13] = sum;
                static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13; ++k) {
                    size_t ci = (seq - 1) * 13 + k;
                    uint16_t ch = ci < long_name.size() ? uint8_t(long_name[ci]) : ci == long_name.size() ? 0 : 0xFFFF;
                    e[pos[k]] = uint8_t(ch);
                    e[pos[k] + 1] = uint8_t(ch >> 8);
                }
                add_raw_entry(d, e);
            }
        }
        uint8_t e[32] = {};
        std::memcpy(e, name83, 11);
        e[11] = attr;
        e[12] = nt_flags;
        e[20] = uint8_t(first >> 16);
        e[21] = uint8_t(first >> 24);
        e[26] = uint8_t(first);
        e[27] = uint8_t(first >> 8);
        std::vector<uint8_t> tmp(e, e + 32);
        put32(tmp, 28, size);
        add_raw_entry(d, tmp.data());
    }

    Dir add_dir(Dir& parent, const char* name83) {
        uint32_t c = alloc_chain(1)[0];
        add_entry(parent, name83, 0x10, c, 0);
        Dir d{c, 0};
        add_entry(d, ".          ", 0x10, c, 0);
        add_entry(d, "..         ", 0x10, parent.first, 0);
        return d;
    }

    void add_file(Dir& d, const char* name83, const std::vector<uint8_t>& data, const std::string& long_name = "",
                  uint8_t nt_flags = 0, bool fragmented = false) {
        uint32_t n = uint32_t((data.size() + 511) / 512);
        auto c = alloc_chain(n, fragmented);
        for (uint32_t i = 0; i < n; ++i)
            std::memcpy(cluster(c[i]), data.data() + i * 512, std::min<size_t>(512, data.size() - i * 512));
        add_entry(d, name83, 0x20, n ? c[0] : 0, uint32_t(data.size()), long_name, nt_flags);
    }

    std::vector<uint8_t> finish(const char* label) {
        std::vector<uint8_t>& b = img_;
        size_t o = lead_in_;
        b[o] = 0xEB;
        b[o + 1] = 0x58;
        b[o + 2] = 0x90;
        std::memcpy(&b[o + 3], "mkdosfs ", 8);
        put16(b, o + 11, 512);
        b[o + 13] = 1;
        put16(b, o + 14, reserved_);
        b[o + 16] = 2;
        put16(b, o + 17, root_entries_);
        b[o + 21] = 0xF8;
        if (total_ < 65536) put16(b, o + 19, total_);
        else put32(b, o + 32, total_);
        if (type_ == didj::fs::FatType::Fat32) {
            put32(b, o + 36, fat_sectors_);
            put32(b, o + 44, root_cluster_);
            std::memcpy(&b[o + 71], label, 11);
            std::memcpy(&b[o + 82], "FAT32   ", 8);
        } else {
            put16(b, o + 22, fat_sectors_);
            std::memcpy(&b[o + 43], label, 11);
        }
        b[o + 510] = 0x55;
        b[o + 511] = 0xAA;
        // Second FAT copy.
        std::memcpy(&b[o + size_t(reserved_ + fat_sectors_) * 512], &b[o + size_t(reserved_) * 512],
                    size_t(fat_sectors_) * 512);
        if (lead_in_) {
            b[446 + 4] = type_ == didj::fs::FatType::Fat32 ? 0x0C : 0x06;
            put32(b, 446 + 8, lead_in_ / 512);
            put32(b, 446 + 12, total_);
            b[510] = 0x55;
            b[511] = 0xAA;
        }
        return b;
    }

private:
    void set_fat(uint32_t c, uint32_t v) {
        size_t fat = lead_in_ + size_t(reserved_) * 512;
        switch (type_) {
            case didj::fs::FatType::Fat12: {
                v &= 0xFFF;
                size_t off = fat + c + c / 2;
                uint16_t cur = uint16_t(img_[off] | (img_[off + 1] << 8));
                cur = (c & 1) ? uint16_t((cur & 0x000F) | (v << 4)) : uint16_t((cur & 0xF000) | v);
                put16(img_, off, cur);
                break;
            }
            case didj::fs::FatType::Fat16: put16(img_, fat + c * 2, v & 0xFFFF); break;
            case didj::fs::FatType::Fat32: put32(img_, fat + c * 4, v & 0x0FFFFFFF); break;
        }
    }

    didj::fs::FatType type_;
    uint32_t lead_in_;
    uint32_t reserved_, root_entries_, fat_sectors_, data_sector_, total_;
    uint32_t root_cluster_ = 0, next_free_ = 2;
    std::vector<uint8_t> img_;
};

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 9));
    return v;
}

std::string write_temp(const std::string& name, const std::vector<uint8_t>& data) {
    auto p = std::filesystem::temp_directory_path() / ("didj_test_" + name);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return p.string();
}

void test_fat(didj::fs::FatType type, uint32_t clusters, uint32_t lead_in) {
    FatBuilder b(type, clusters, lead_in);
    auto root = b.root();
    b.add_entry(root, "Cartridge  ", 0x08, 0, 0);
    auto readme = pattern(100, 1);
    b.add_file(root, "README  TXT", readme);
    auto sub = b.add_dir(root, "SNC        ");
    auto app = pattern(5000, 3);
    b.add_file(sub, "APP     SO ", app, "App.so", 0, /*fragmented=*/true);
    auto meta = pattern(20, 9);
    b.add_file(sub, "META    INF", meta, "", 0x18);
    auto longf = pattern(1500, 5);
    b.add_file(sub, "LONGFI~1TXT", longf, "A Rather Long File Name.txt");
    b.add_file(sub, "EMPTY   BIN", {});

    std::string path = write_temp(std::string(didj::fs::fat_type_name(type)) + (lead_in ? "_mbr" : "") + ".img",
                                  b.finish("NO NAME    "));
    std::string err;
    auto img = didj::fs::FatImage::open(path, &err);
    CHECK(img != nullptr);
    if (!img) {
        std::fprintf(stderr, "  open failed: %s\n", err.c_str());
        return;
    }
    CHECK(img->type() == type);
    CHECK(img->label() == "Cartridge");

    auto top = img->list("/");
    CHECK(top && top->size() == 2);
    CHECK(img->lookup("/snc") && img->lookup("/snc")->is_dir());

    auto entries = img->list("/SNC");
    CHECK(entries && entries->size() == 4);  // "." and ".." are hidden

    CHECK(img->read_file("/README.TXT") == readme);
    CHECK(img->read_file("/SNC/App.so") == app);
    CHECK(img->read_file("snc/app.so") == app);
    CHECK(img->read_file("/SNC/APP.SO") == app);  // 8.3 alias
    CHECK(img->lookup("/SNC/meta.inf") && img->lookup("/SNC/meta.inf")->name == "meta.inf");
    CHECK(img->lookup("/SNC/../SNC/./App.so") && img->lookup("/SNC/../SNC/./App.so")->size == app.size());
    CHECK(img->read_file("/SNC/meta.inf") == meta);
    CHECK(img->read_file("/SNC/A Rather Long File Name.txt") == longf);
    CHECK(img->read_file("/SNC/EMPTY.BIN") == std::vector<uint8_t>{});
    CHECK(!img->read_file("/SNC/missing"));
    CHECK(!img->read_file("/README.TXT/x"));

    // Partial read across a fragmented cluster boundary.
    auto e = img->lookup("/SNC/App.so");
    std::vector<uint8_t> part(1000);
    CHECK(img->read(*e, 300, part.data(), part.size()) == part.size());
    CHECK(std::equal(part.begin(), part.end(), app.begin() + 300));
    CHECK(img->read(*e, 4990, part.data(), part.size()) == 10);

    img.reset();
    std::filesystem::remove(path);
}

// --- inflate ---

#include "inflate_vectors.inc"

void test_inflate() {
    std::string fixed_text;
    for (int i = 0; i < 6; ++i) fixed_text += "The LeapFrog Didj ran Linux 2.6.20 on an LF1000. ";
    for (int i = 0; i < 256; ++i) fixed_text += char(i);

    std::vector<uint8_t> out;
    CHECK(didj::util::inflate_zlib(kZlibFixed, sizeof kZlibFixed, out, 1 << 20));
    CHECK(std::string(out.begin(), out.end()) == fixed_text);

    out.clear();
    CHECK(didj::util::inflate_zlib(kZlibDynamic, sizeof kZlibDynamic, out, 1 << 20));  // Adler-32 checked inside
    CHECK(out.size() == 3000);

    out.clear();
    CHECK(!didj::util::inflate_zlib(kZlibDynamic, sizeof kZlibDynamic, out, 100));  // output limit enforced
    out.clear();
    CHECK(!didj::util::inflate_zlib(kZlibDynamic, sizeof kZlibDynamic - 10, out, 1 << 20));  // truncated

    // Stored block, raw deflate.
    const uint8_t stored[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'};
    out.clear();
    CHECK(didj::util::inflate_raw(stored, sizeof stored, out, 16));
    CHECK(std::string(out.begin(), out.end()) == "abc");

    // Linux crc32_le(0, "123456789") as used by JFFS2 (no pre/post inversion).
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(~didj::util::crc32_raw(~0u, digits, 9) == 0xCBF43926u);  // standard CRC-32 check value
}

// --- JFFS2 ---

class Jffs2Builder {
public:
    void dirent(uint32_t pino, uint32_t version, uint32_t ino, uint8_t type, const std::string& name) {
        std::vector<uint8_t> n(40 + name.size(), 0);
        header(n, 0xE001);
        put32(n, 12, pino);
        put32(n, 16, version);
        put32(n, 20, ino);
        n[28] = uint8_t(name.size());
        n[29] = type;
        put32(n, 32, didj::util::crc32_raw(0, n.data(), 32));
        std::memcpy(&n[40], name.data(), name.size());
        put32(n, 36, didj::util::crc32_raw(0, &n[40], name.size()));
        append(n);
    }

    void inode(uint32_t ino, uint32_t version, uint32_t mode, uint32_t isize, uint32_t offset,
               const std::vector<uint8_t>& payload, uint32_t dsize, uint8_t compr) {
        std::vector<uint8_t> n(68 + payload.size(), 0);
        header(n, 0xE002);
        put32(n, 12, ino);
        put32(n, 16, version);
        put32(n, 20, mode);
        put32(n, 28, isize);
        put32(n, 44, offset);
        put32(n, 48, uint32_t(payload.size()));
        put32(n, 52, dsize);
        n[56] = compr;
        std::memcpy(&n[68], payload.data(), payload.size());
        put32(n, 60, didj::util::crc32_raw(0, payload.data(), payload.size()));
        put32(n, 64, didj::util::crc32_raw(0, n.data(), 60));
        append(n);
    }

    void raw(const std::vector<uint8_t>& bytes) { img_.insert(img_.end(), bytes.begin(), bytes.end()); }
    std::vector<uint8_t> take() { return std::move(img_); }

private:
    void header(std::vector<uint8_t>& n, uint16_t type) {
        put16(n, 0, 0x1985);
        put16(n, 2, type);
        put32(n, 4, uint32_t(n.size()));
        put32(n, 8, didj::util::crc32_raw(0, n.data(), 8));
    }
    void append(std::vector<uint8_t> n) {
        while (n.size() % 4) n.push_back(0xFF);
        raw(n);
    }
    std::vector<uint8_t> img_;
};

std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

void test_jffs2() {
    Jffs2Builder b;
    b.raw(std::vector<uint8_t>(16, 0xFF));  // erased flash before the first node
    b.inode(1, 1, 040755, 0, 0, {}, 0, 0);
    b.dirent(1, 1, 2, 4, "lib");
    b.inode(2, 1, 040755, 0, 0, {}, 0, 0);

    // A file written in two nodes, the second one zlib-compressed.
    b.dirent(2, 1, 3, 8, "libz.so.1.2.3");
    b.inode(3, 1, 0100755, 4, 0, bytes_of("\x7f" "ELF"), 4, 0);
    std::vector<uint8_t> fixed_text(kZlibFixed, kZlibFixed + sizeof kZlibFixed);
    b.inode(3, 2, 0100755, 4 + 550, 4, fixed_text, 550, 6);

    // Symlinks, relative and absolute.
    b.dirent(2, 2, 4, 10, "libz.so");
    b.inode(4, 1, 0120777, 13, 0, bytes_of("libz.so.1.2.3"), 13, 0);
    b.dirent(1, 2, 5, 10, "usrlib");
    b.inode(5, 1, 0120777, 4, 0, bytes_of("/lib"), 4, 0);

    // A file rewritten, then truncated by a newer data-less node, then deleted
    // and recreated under the same name with a new inode.
    b.dirent(1, 3, 6, 8, "settings.cfg");
    b.inode(6, 1, 0100644, 11, 0, bytes_of("hello world"), 11, 0);
    b.inode(6, 2, 0100644, 5, 0, {}, 0, 0);
    b.dirent(1, 4, 0, 8, "settings.cfg");
    b.dirent(1, 5, 7, 8, "settings.cfg");
    b.inode(7, 1, 0100600, 3, 0, {}, 0, 0);
    b.inode(7, 2, 0100600, 3, 0, std::vector<uint8_t>{0}, 3, 1);  // ZERO compression

    // An entry deleted for good, and a node with a corrupt header CRC.
    b.dirent(1, 6, 8, 8, "gone");
    b.dirent(1, 7, 0, 8, "gone");
    std::vector<uint8_t> bad = {0x85, 0x19, 0x01, 0xE0, 0x0c, 0, 0, 0, 0xde, 0xad, 0xbe, 0xef};
    b.raw(bad);

    // A character device: /dev/fb0 is (29, 0), stored old-style in 2 bytes.
    b.dirent(1, 8, 9, 2, "fb0");
    b.inode(9, 1, 020660, 2, 0, std::vector<uint8_t>{0x00, 29}, 2, 0);

    std::string err;
    auto fs = didj::fs::Jffs2Image::from_bytes(b.take(), &err);
    CHECK(fs != nullptr);
    if (!fs) return;
    CHECK(fs->bad_nodes() == 1);

    auto lib = fs->read_file("/lib/libz.so.1.2.3");
    CHECK(lib && lib->size() == 554);
    CHECK(lib && std::memcmp(lib->data(), "\x7f" "ELF" "The LeapFrog", 16) == 0);
    CHECK(fs->read_file("/lib/libz.so") == lib);
    CHECK(fs->read_file("/usrlib/libz.so") == lib);
    CHECK(fs->read_file("usrlib/../lib/libz.so") == lib);
    auto link = fs->lookup("/lib/libz.so", /*follow_last=*/false);
    CHECK(link && link->type == didj::fs::NodeType::Symlink && fs->readlink(*link) == "libz.so.1.2.3");
    CHECK(!fs->lookup("/LIB/libz.so"));  // JFFS2 is case-sensitive

    auto cfg = fs->read_file("/settings.cfg");
    CHECK(cfg && *cfg == std::vector<uint8_t>(3, 0));
    CHECK(fs->lookup("/settings.cfg")->mode == 0100600);
    CHECK(!fs->lookup("/gone"));

    auto fb = fs->lookup("/fb0");
    CHECK(fb && fb->type == didj::fs::NodeType::CharDevice && fb->rdev == (29u << 8));

    auto root = fs->list("/");
    CHECK(root && root->size() == 4);  // lib, usrlib, settings.cfg, fb0
}

void test_metainf() {
    auto m = didj::pkg::MetaInf::parse(
        "MetaVersion=\"1.0\"\r\nProductID=0x000F0004\r\nName=\"Sonic the Hedgehog\"\r\n# comment\r\nHidden=0\r\n");
    CHECK(m.get_or("Name", "") == "Sonic the Hedgehog");
    CHECK(m.get_or("ProductID", "") == "0x000F0004");
    CHECK(m.get_or("Hidden", "") == "0");
    CHECK(!m.get("Missing"));
    CHECK(m.entries().size() == 4);
}

// Minimal ARM ET_DYN: one PT_LOAD covering the file, PT_DYNAMIC, DT_HASH,
// three symbols (an import with a version, an export, a weak import).
void test_elf() {
    std::vector<uint8_t> b(0x400, 0);
    std::memcpy(b.data(), "\x7f" "ELF\x01\x01\x01", 7);
    put16(b, 16, 3);
    put16(b, 18, 40);
    put32(b, 28, 52);       // phoff
    put32(b, 36, 0x05000002);
    put16(b, 42, 32);
    put16(b, 44, 2);
    // PT_LOAD
    put32(b, 52, 1);
    put32(b, 56, 0);
    put32(b, 60, 0);
    put32(b, 68, uint32_t(b.size()));
    // PT_DYNAMIC at 0x100
    put32(b, 84, 2);
    put32(b, 88, 0x100);
    put32(b, 100, 0x80);

    const char strtab[] = "\0libc.so.0\0libfoo.so\0puts\0foo_init\0weak_fn\0GLIBC_X\0";
    size_t str = 0x200;
    std::memcpy(&b[str], strtab, sizeof strtab);
    auto so = [&](const char* s) {
        return uint32_t(std::search(strtab, strtab + sizeof strtab, s, s + std::strlen(s) + 1) - strtab);
    };
    size_t sym = 0x280, hash = 0x300, versym = 0x340, verneed = 0x360;
    auto add_sym = [&](int i, const char* name, uint32_t value, uint8_t info, uint16_t shndx) {
        size_t s = sym + size_t(i) * 16;
        put32(b, s, so(name));
        put32(b, s + 4, value);
        b[s + 12] = info;
        put16(b, s + 14, shndx);
    };
    add_sym(1, "puts", 0, 0x12, 0);
    add_sym(2, "foo_init", 0x1234, 0x12, 5);
    add_sym(3, "weak_fn", 0, 0x22, 0);
    put32(b, hash, 1);
    put32(b, hash + 4, 4);  // nchain = symbol count
    put16(b, versym + 2, 2);
    // Verneed: libc.so.0 needs GLIBC_X as version index 2.
    put16(b, verneed, 1);
    put16(b, verneed + 2, 1);
    put32(b, verneed + 4, so("libc.so.0"));
    put32(b, verneed + 8, 16);
    put16(b, verneed + 16 + 6, 2);
    put32(b, verneed + 16 + 8, so("GLIBC_X"));

    uint32_t dyn[][2] = {{1, so("libc.so.0")}, {14, so("libfoo.so")}, {5, uint32_t(str)}, {6, uint32_t(sym)},
                         {4, uint32_t(hash)}, {0x6ffffff0, uint32_t(versym)}, {0x6ffffffe, uint32_t(verneed)},
                         {0x6fffffff, 1}, {0, 0}};
    for (size_t i = 0; i < sizeof dyn / sizeof dyn[0]; ++i) {
        put32(b, 0x100 + i * 8, dyn[i][0]);
        put32(b, 0x100 + i * 8 + 4, dyn[i][1]);
    }

    std::string err;
    auto info = didj::elf::parse_dynamic(b, &err);
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->machine == 40);
    CHECK(info->soname == "libfoo.so");
    CHECK(info->needed == std::vector<std::string>{"libc.so.0"});
    CHECK(info->symbols.size() == 3);
    CHECK(info->symbols[0].name == "puts" && !info->symbols[0].defined && info->symbols[0].version == "GLIBC_X");
    CHECK(info->symbols[1].name == "foo_init" && info->symbols[1].defined && info->symbols[1].value == 0x1234);
    CHECK(info->symbols[2].weak && info->symbols[2].version.empty());
    CHECK(!didj::elf::parse_dynamic(std::vector<uint8_t>(10), &err));
}

void test_md5() {
    auto md5 = [](const std::string& s) {
        didj::util::Md5 m;
        m.update(s.data(), s.size());
        return m.hex();
    };
    // RFC 1321 appendix A.5.
    CHECK(md5("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(md5("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(md5("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    const std::string digits =
        "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    CHECK(md5(digits) == "57edf4a22be3c955ac49da2e2107b67a");
    // Split updates give the same digest.
    didj::util::Md5 m;
    for (char c : digits) m.update(&c, 1);
    CHECK(m.hex() == "57edf4a22be3c955ac49da2e2107b67a");
}

void test_zip() {
    // One stored file, as LeapFrog's .lfp packages store most of theirs.
    const std::string name = "POW/meta.inf", body = "Name=\"x\"\n";
    const uint32_t crc = ~didj::util::crc32_raw(~0u, reinterpret_cast<const uint8_t*>(body.data()), body.size());
    std::vector<uint8_t> z;
    auto u16 = [&](uint32_t v) { z.push_back(uint8_t(v)), z.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xffff), u16(v >> 16); };
    auto str = [&](const std::string& t) { z.insert(z.end(), t.begin(), t.end()); };
    u32(0x04034b50), u16(10), u16(0), u16(0), u16(0), u16(0), u32(crc), u32(uint32_t(body.size())),
        u32(uint32_t(body.size())), u16(uint32_t(name.size())), u16(0), str(name), str(body);
    const uint32_t cd = uint32_t(z.size());
    u32(0x02014b50), u16(20), u16(10), u16(0), u16(0), u16(0), u16(0), u32(crc), u32(uint32_t(body.size())),
        u32(uint32_t(body.size())), u16(uint32_t(name.size())), u16(0), u16(0), u16(0), u16(0), u32(0), u32(0),
        str(name);
    const uint32_t cd_size = uint32_t(z.size()) - cd;
    u32(0x06054b50), u16(0), u16(0), u16(1), u16(1), u32(cd_size), u32(cd), u16(0);

    didj::pkg::ZipArchive zip;
    std::string err;
    CHECK(zip.open(z, &err));
    CHECK(zip.entries().size() == 1);
    if (zip.entries().size() != 1) return;
    std::vector<uint8_t> out;
    CHECK(zip.read(zip.entries()[0], out, &err) && std::string(out.begin(), out.end()) == body);
    z[30 + name.size()] ^= 1;  // corrupt the data: the CRC check catches it
    CHECK(zip.open(z, &err) && !zip.read(zip.entries()[0], out, &err));
    CHECK(!zip.open(std::vector<uint8_t>(30), &err));
}

void test_zip_extract_names() {
    // Entries that could land outside the target directory are skipped (open()
    // turns '\\' into '/', so Windows-style "..\\" is caught too).
    const std::vector<std::string> names = {"POW/meta.inf", "POW/../up", "POW\\..\\..\\up", "C:up", "/up"};
    std::vector<uint8_t> z;
    auto u16 = [&](uint32_t v) { z.push_back(uint8_t(v)), z.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xffff), u16(v >> 16); };
    auto str = [&](const std::string& t) { z.insert(z.end(), t.begin(), t.end()); };
    std::vector<uint32_t> offsets;
    for (const auto& n : names) {
        offsets.push_back(uint32_t(z.size()));
        u32(0x04034b50), u16(10), u16(0), u16(0), u16(0), u16(0), u32(0), u32(0), u32(0),
            u16(uint32_t(n.size())), u16(0), str(n);
    }
    const uint32_t cd = uint32_t(z.size());
    for (size_t i = 0; i < names.size(); ++i)
        u32(0x02014b50), u16(20), u16(10), u16(0), u16(0), u16(0), u16(0), u32(0), u32(0), u32(0),
            u16(uint32_t(names[i].size())), u16(0), u16(0), u16(0), u16(0), u32(0), u32(offsets[i]), str(names[i]);
    const uint32_t cd_size = uint32_t(z.size()) - cd;
    u32(0x06054b50), u16(0), u16(0), u16(uint32_t(names.size())), u16(uint32_t(names.size())), u32(cd_size), u32(cd),
        u16(0);

    didj::pkg::ZipArchive zip;
    std::string err;
    CHECK(zip.open(z, &err));
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "didj_zip_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path dir = root / "a" / "b";
    CHECK(zip.extract_all(dir, &err));
    CHECK(fs::is_regular_file(dir / "POW" / "meta.inf"));
    size_t files = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        files += it->is_regular_file();
    CHECK(files == 1);
    fs::remove_all(root, ec);
}

}  // namespace

int main() {
    test_fat(didj::fs::FatType::Fat12, 2000, 0);
    test_fat(didj::fs::FatType::Fat16, 5000, 0);
    test_fat(didj::fs::FatType::Fat32, 70000, 0);
    test_fat(didj::fs::FatType::Fat16, 5000, 63 * 512);
    test_inflate();
    test_jffs2();
    test_metainf();
    test_elf();
    test_md5();
    test_zip();
    test_zip_extract_names();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
