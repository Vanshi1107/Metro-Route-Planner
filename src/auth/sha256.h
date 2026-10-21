// Incremental SHA-256 (internal; see crypto.h for the public functions).
#pragma once
#include <cstddef>
#include <cstdint>

#include "crypto.h"

namespace metro {
namespace auth {

class Sha256 {
public:
    Sha256();
    void update(const unsigned char* data, size_t len);
    Bytes finish();   // returns the 32-byte digest; do not use the object afterwards

private:
    void compress(const unsigned char* block);
    uint32_t h_[8];
    unsigned char buf_[64];
    size_t bufLen_;
    uint64_t totalLen_;
};

}  // namespace auth
}  // namespace metro
