// Portable PBKDF2-HMAC-SHA256 and system random, used on non-Windows systems (mainly so the code can be
// tested on Linux). On Windows the operating system's own implementation is used instead: crypto_win.cpp.
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "crypto.h"
#include "sha256.h"

namespace metro {
namespace auth {

Bytes randomBytes(size_t n) {
    Bytes out(n);
    if (n == 0) return out;
    std::ifstream f("/dev/urandom", std::ios::binary);
    if (!f) throw std::runtime_error("cannot open the system random generator");
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
    if (static_cast<size_t>(f.gcount()) != n) throw std::runtime_error("the system random generator returned too little data");
    return out;
}

Bytes pbkdf2Sha256(const std::string& password, const Bytes& salt, unsigned long iterations, size_t outLen) {
    if (iterations == 0 || outLen == 0) throw std::invalid_argument("pbkdf2 needs iterations >= 1 and a non-empty output");

    // HMAC key: hashed first if longer than the 64-byte block
    Bytes key = toBytes(password);
    if (key.size() > 64) key = sha256Bytes(key);
    unsigned char ipad[64], opad[64];
    std::memset(ipad, 0x36, 64);
    std::memset(opad, 0x5c, 64);
    for (size_t i = 0; i < key.size(); ++i) {
        ipad[i] = static_cast<unsigned char>(ipad[i] ^ key[i]);
        opad[i] = static_cast<unsigned char>(opad[i] ^ key[i]);
    }
    Sha256 innerBase, outerBase;   // state after absorbing the padded key, reused for every HMAC call
    innerBase.update(ipad, 64);
    outerBase.update(opad, 64);
    auto prf = [&](const unsigned char* msg, size_t len) {
        Sha256 in = innerBase;
        in.update(msg, len);
        const Bytes ih = in.finish();
        Sha256 out = outerBase;
        out.update(ih.data(), ih.size());
        return out.finish();
    };

    Bytes result;
    result.reserve(outLen + 32);
    for (unsigned long block = 1; result.size() < outLen; ++block) {
        Bytes msg = salt;
        msg.push_back(static_cast<unsigned char>(block >> 24));
        msg.push_back(static_cast<unsigned char>(block >> 16));
        msg.push_back(static_cast<unsigned char>(block >> 8));
        msg.push_back(static_cast<unsigned char>(block));
        Bytes u = prf(msg.data(), msg.size());
        Bytes t = u;
        for (unsigned long i = 1; i < iterations; ++i) {
            u = prf(u.data(), u.size());
            for (size_t k = 0; k < 32; ++k) t[k] = static_cast<unsigned char>(t[k] ^ u[k]);
        }
        result.insert(result.end(), t.begin(), t.end());
    }
    result.resize(outLen);
    return result;
}

}  // namespace auth
}  // namespace metro
