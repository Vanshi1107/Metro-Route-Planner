#include "sha256.h"

#include <cstring>
#include <stdexcept>

namespace metro {
namespace auth {

namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256() : bufLen_(0), totalLen_(0) {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof(h_));
    std::memset(buf_, 0, sizeof(buf_));
}

void Sha256::compress(const unsigned char* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) | (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(block[4 * i + 2]) << 8) | static_cast<uint32_t>(block[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const unsigned char* data, size_t len) {
    totalLen_ += len;
    while (len > 0) {
        size_t take = 64 - bufLen_;
        if (take > len) take = len;
        std::memcpy(buf_ + bufLen_, data, take);
        bufLen_ += take;
        data += take;
        len -= take;
        if (bufLen_ == 64) {
            compress(buf_);
            bufLen_ = 0;
        }
    }
}

Bytes Sha256::finish() {
    const uint64_t bits = totalLen_ * 8;
    const unsigned char one = 0x80, zero = 0;
    update(&one, 1);
    while (bufLen_ != 56) update(&zero, 1);
    unsigned char lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update(lenBytes, 8);
    Bytes out(32);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<size_t>(4 * i + j)] = static_cast<unsigned char>(h_[i] >> (24 - 8 * j));
    return out;
}

// ---- public helpers ---------------------------------------------------------------------

Bytes sha256Bytes(const Bytes& data) {
    Sha256 h;
    h.update(data.data(), data.size());
    return h.finish();
}

Bytes sha256(const std::string& data) {
    Sha256 h;
    h.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return h.finish();
}

std::string sha256Hex(const std::string& data) { return toHex(sha256(data)); }

std::string toHex(const Bytes& b) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (unsigned char c : b) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

Bytes fromHex(const std::string& hex) {
    if (hex.size() % 2 != 0) throw std::invalid_argument("hex string has an odd length");
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("hex string has a bad digit");
    };
    Bytes out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) out.push_back(static_cast<unsigned char>((val(hex[i]) << 4) | val(hex[i + 1])));
    return out;
}

Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

bool constantTimeEqual(const Bytes& a, const Bytes& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

}  // namespace auth
}  // namespace metro
