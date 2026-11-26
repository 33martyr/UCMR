// sha1.h - SHA-1 (FIPS 180-4) for the Linux builds of the launcher core and the importers, which have no bcrypt.
// Portable; the Windows builds keep their bcrypt hash. Same interface as sha256.h.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace sha1 {

class Hash {
public:
    Hash() { reset(); }

    void reset() {
        static const uint32_t start[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
        std::memcpy(h_, start, sizeof h_);
        bits_ = 0;
        have_ = 0;
    }

    void update(const void *data, size_t n) {
        const uint8_t *p = (const uint8_t *)data;
        bits_ += (uint64_t)n * 8;
        if (have_) {
            const size_t take = std::min(n, sizeof buf_ - have_);
            std::memcpy(buf_ + have_, p, take);
            have_ += take;
            p += take;
            n -= take;
            if (have_ == sizeof buf_) {
                block(buf_);
                have_ = 0;
            }
        }
        while (n >= sizeof buf_) {
            block(p);
            p += sizeof buf_;
            n -= sizeof buf_;
        }
        if (n) {
            std::memcpy(buf_, p, n);
            have_ = n;
        }
    }

    // the 20 digest bytes; the hash is reset for the next message
    void finish(uint8_t out[20]) {
        const uint64_t bits = bits_;
        const size_t rem = (size_t)((bits / 8) % sizeof buf_);
        uint8_t pad[sizeof buf_ * 2];
        size_t n = 0;
        pad[n++] = 0x80;
        const size_t zeros = (rem < sizeof buf_ - 8 ? sizeof buf_ - 8 - rem : 2 * sizeof buf_ - 8 - rem) - 1;
        std::memset(pad + n, 0, zeros);
        n += zeros;
        for (int i = 7; i >= 0; --i) pad[n++] = (uint8_t)(bits >> (8 * i));
        update(pad, n);
        for (int i = 0; i < 5; ++i) {
            out[4 * i] = (uint8_t)(h_[i] >> 24);
            out[4 * i + 1] = (uint8_t)(h_[i] >> 16);
            out[4 * i + 2] = (uint8_t)(h_[i] >> 8);
            out[4 * i + 3] = (uint8_t)h_[i];
        }
        reset();
    }

    // the digest as 20 raw bytes in a string; the hash is reset for the next message
    std::string bytes() {
        uint8_t d[20];
        finish(d);
        return std::string((const char *)d, sizeof d);
    }

    // the digest as 40 lowercase hex digits; the hash is reset for the next message
    std::string hex() {
        uint8_t d[20];
        finish(d);
        static const char digits[] = "0123456789abcdef";
        std::string s;
        s.reserve(40);
        for (uint8_t b : d) {
            s += digits[b >> 4];
            s += digits[b & 15];
        }
        return s;
    }

private:
    static uint32_t rotl(uint32_t v, int by) { return (v << by) | (v >> (32 - by)); }

    void block(const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5];
    uint64_t bits_;
    uint8_t buf_[64];
    size_t have_;
};

// the SHA-1 of `data` as 40 lowercase hex digits
inline std::string hex(const void *data, size_t n) {
    Hash h;
    h.update(data, n);
    return h.hex();
}

}  // namespace sha1
