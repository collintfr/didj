#include "util/inflate.h"

#include <cstring>

namespace didj::util {

namespace {

constexpr int kMaxBits = 15;

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[288];
};

class Inflater {
public:
    Inflater(const uint8_t* in, size_t len, std::vector<uint8_t>& out, size_t max_out)
        : in_(in), len_(len), out_(out), max_out_(max_out) {}

    bool run() {
        int last;
        do {
            last = bits(1);
            int type = bits(2);
            bool ok = type == 0 ? stored() : type == 1 ? fixed() : type == 2 ? dynamic() : false;
            if (!ok || error_) return false;
        } while (!last);
        return true;
    }

    size_t consumed() const { return pos_; }  // whole bytes, after aligning

private:
    int bits(int need) {
        uint32_t val = bitbuf_;
        while (bitcnt_ < need) {
            if (pos_ >= len_) {
                error_ = true;
                return 0;
            }
            val |= uint32_t(in_[pos_++]) << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = val >> need;
        bitcnt_ -= need;
        return int(val & ((1u << need) - 1));
    }

    bool emit(uint8_t b) {
        if (out_.size() >= max_out_) return false;
        out_.push_back(b);
        return true;
    }

    bool stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        if (pos_ + 4 > len_) return false;
        unsigned n = in_[pos_] | (in_[pos_ + 1] << 8);
        unsigned nn = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if (n != (~nn & 0xffff) || pos_ + n > len_ || out_.size() + n > max_out_) return false;
        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + n);
        pos_ += n;
        return true;
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (error_) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    // Returns <0 if over-subscribed, 0 if complete, >0 if incomplete.
    static int construct(Huffman& h, const short* length, int n) {
        std::memset(h.count, 0, sizeof h.count);
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        short offs[kMaxBits + 1];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = short(offs[len] + h.count[len]);
        for (int s = 0; s < n; ++s)
            if (length[s]) h.symbol[offs[length[s]]++] = short(s);
        return left;
    }

    bool codes(const Huffman& lencode, const Huffman& distcode) {
        static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lencode);
            if (sym < 0) return false;
            if (sym < 256) {
                if (!emit(uint8_t(sym))) return false;
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                size_t len = size_t(lbase[sym] + bits(lext[sym]));
                int dsym = decode(distcode);
                if (dsym < 0 || dsym >= 30) return false;
                size_t dist = size_t(dbase[dsym] + bits(dext[dsym]));
                if (error_ || dist > out_.size() || out_.size() + len > max_out_) return false;
                size_t from = out_.size() - dist;
                for (size_t i = 0; i < len; ++i) out_.push_back(out_[from + i]);
            }
        }
    }

    bool fixed() {
        struct Tables {
            Huffman lencode, distcode;
        };
        static const Tables t = [] {
            Tables r;
            short lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            construct(r.lencode, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            construct(r.distcode, lengths, 30);
            return r;
        }();
        return codes(t.lencode, t.distcode);
    }

    bool dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (error_ || nlen > 286 || ndist > 30) return false;
        int idx = 0;
        for (; idx < ncode; ++idx) lengths[order[idx]] = short(bits(3));
        for (; idx < 19; ++idx) lengths[order[idx]] = 0;
        Huffman lencode, distcode;
        if (construct(lencode, lengths, 19) != 0) return false;

        idx = 0;
        while (idx < nlen + ndist) {
            int sym = decode(lencode);
            if (sym < 0) return false;
            if (sym < 16) {
                lengths[idx++] = short(sym);
                continue;
            }
            short len = 0;
            int rep;
            if (sym == 16) {
                if (idx == 0) return false;
                len = lengths[idx - 1];
                rep = 3 + bits(2);
            } else if (sym == 17) {
                rep = 3 + bits(3);
            } else {
                rep = 11 + bits(7);
            }
            if (error_ || idx + rep > nlen + ndist) return false;
            while (rep--) lengths[idx++] = len;
        }
        if (lengths[256] == 0) return false;
        int err = construct(lencode, lengths, nlen);
        if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
        err = construct(distcode, lengths + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
        return codes(lencode, distcode);
    }

    const uint8_t* in_;
    size_t len_;
    size_t pos_ = 0;
    uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;
    bool error_ = false;
    std::vector<uint8_t>& out_;
    size_t max_out_;
};

}  // namespace

bool inflate_raw(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out, size_t max_out) {
    Inflater inf(in, in_len, out, max_out);
    return inf.run();
}

bool inflate_zlib(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out, size_t max_out) {
    if (in_len < 6) return false;
    uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
    size_t start = out.size();
    Inflater inf(in + 2, in_len - 2, out, max_out);
    if (!inf.run()) return false;
    size_t trailer = 2 + inf.consumed();
    if (trailer + 4 > in_len) return false;
    uint32_t a = 1, b = 0;
    for (size_t i = start; i < out.size(); ++i) {
        a = (a + out[i]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t want = (uint32_t(in[trailer]) << 24) | (uint32_t(in[trailer + 1]) << 16) |
                    (uint32_t(in[trailer + 2]) << 8) | in[trailer + 3];
    return ((b << 16) | a) == want;
}

uint32_t crc32_raw(uint32_t crc, const uint8_t* data, size_t len) {
    struct Table {
        uint32_t v[256];
    };
    static const Table table = [] {
        Table t;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            t.v[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < len; ++i) crc = table.v[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

}  // namespace didj::util
