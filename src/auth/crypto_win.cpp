// Windows implementation: the operating system's own cryptography (CNG).
#include <stdexcept>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include "crypto.h"

#pragma comment(lib, "bcrypt.lib")

namespace metro {
namespace auth {

namespace {
std::string statusText(long status) { return "status 0x" + std::to_string(static_cast<unsigned long>(status)); }
}  // namespace

Bytes randomBytes(size_t n) {
    Bytes out(n);
    if (n == 0) return out;
    const NTSTATUS st = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) throw std::runtime_error("BCryptGenRandom failed, " + statusText(st));
    return out;
}

Bytes pbkdf2Sha256(const std::string& password, const Bytes& salt, unsigned long iterations, size_t outLen) {
    if (iterations == 0 || outLen == 0) throw std::invalid_argument("pbkdf2 needs iterations >= 1 and a non-empty output");
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(st)) throw std::runtime_error("BCryptOpenAlgorithmProvider failed, " + statusText(st));
    Bytes out(outLen);
    st = BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
                               const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), static_cast<ULONGLONG>(iterations), out.data(),
                               static_cast<ULONG>(outLen), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) throw std::runtime_error("BCryptDeriveKeyPBKDF2 failed, " + statusText(st));
    return out;
}

}  // namespace auth
}  // namespace metro
