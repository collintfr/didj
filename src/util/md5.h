// MD5 (RFC 1321), used to verify downloaded dumps against known checksums.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace didj::util {

class Md5 {
public:
    Md5();
    void update(const void* data, size_t len);
    // Finishes the digest and returns it as 32 lowercase hex digits.
    std::string hex();

private:
    void block(const uint8_t* p);

    uint32_t h_[4];
    uint64_t len_ = 0;
    uint8_t buf_[64];
    size_t used_ = 0;
};

// MD5 of a whole file as hex, or an empty string if it can't be read.
std::string md5_file(const std::string& path);

}  // namespace didj::util
