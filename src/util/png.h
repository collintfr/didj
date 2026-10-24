// Minimal PNG writer (uncompressed deflate blocks) for screenshots.
#pragma once

#include <cstdint>
#include <string>

namespace didj::util {

// Writes an RGB PNG from XRGB8888 pixels. Returns false on I/O failure.
bool write_png(const std::string& path, uint32_t width, uint32_t height, const uint32_t* xrgb);

}  // namespace didj::util
