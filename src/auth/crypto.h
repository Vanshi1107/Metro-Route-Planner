// Cryptographic building blocks for passwords and sessions.
//
//   sha256()        portable SHA-256, identical on every platform (checked against published test vectors)
//   randomBytes()   the operating system's secure random generator
//   pbkdf2Sha256()  PBKDF2-HMAC-SHA256. On Windows this is the operating system's own implementation
//                   (CNG, BCryptDeriveKeyPBKDF2). On other systems a portable version is used, which exists
//                   so the code can be tested there; the tests prove both give the same answers.
//
// Nothing here invents a cryptographic scheme. The aim is to use standard, reviewed pieces.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace metro {
namespace auth {

using Bytes = std::vector<unsigned char>;

std::string toHex(const Bytes& b);                 // lower-case hex
Bytes fromHex(const std::string& hex);             // throws std::invalid_argument on odd length or bad digits
Bytes toBytes(const std::string& s);

Bytes sha256(const std::string& data);             // 32 bytes
Bytes sha256Bytes(const Bytes& data);
std::string sha256Hex(const std::string& data);

Bytes randomBytes(size_t n);                       // throws std::runtime_error if the system RNG fails

// iterations >= 1, outLen >= 1. 'password' is used as raw bytes (UTF-8), including any NUL bytes.
Bytes pbkdf2Sha256(const std::string& password, const Bytes& salt, unsigned long iterations, size_t outLen);

// Compares in time that does not depend on where the first difference is.
bool constantTimeEqual(const Bytes& a, const Bytes& b);

}  // namespace auth
}  // namespace metro
