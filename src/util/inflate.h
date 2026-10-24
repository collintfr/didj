// Small DEFLATE (RFC 1951) and zlib (RFC 1950) decoder, after Mark Adler's
// puff.c. Used for JFFS2 nodes, so the host build needs no zlib.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace didj::util {

// Decodes a raw DEFLATE stream. Returns false on malformed input or if the
// output would exceed max_out bytes.
bool inflate_raw(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out, size_t max_out);

// Decodes a zlib-wrapped stream (2-byte header, DEFLATE data, Adler-32).
bool inflate_zlib(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out, size_t max_out);

// CRC-32 (IEEE 802.3, reflected) without pre/post inversion, matching the
// Linux kernel's crc32_le(seed, ...) that JFFS2 uses with seed 0.
uint32_t crc32_raw(uint32_t seed, const uint8_t* data, size_t len);

}  // namespace didj::util
