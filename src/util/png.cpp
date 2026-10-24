#include "util/png.h"

#include <cstdio>
#include <vector>

#include "util/inflate.h"

namespace didj::util {

namespace {

void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put_be32(out, uint32_t(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, ~crc32_raw(~0u, body.data(), body.size()));
}

}  // namespace

bool write_png(const std::string& path, uint32_t width, uint32_t height, const uint32_t* xrgb) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (width * 3 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t p = xrgb[y * width + x];
            raw.push_back(uint8_t(p >> 16));
            raw.push_back(uint8_t(p >> 8));
            raw.push_back(uint8_t(p));
        }
    }
    // zlib stream of stored blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t off = 0; off < raw.size() || off == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(off), raw.begin() + std::ptrdiff_t(off + n));
        off += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace didj::util
