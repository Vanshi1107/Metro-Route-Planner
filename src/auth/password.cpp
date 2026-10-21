#include "password.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "crypto.h"

namespace metro {
namespace auth {

namespace {

const char* kAlgorithm = "pbkdf2-sha256";
const size_t kSaltBytes = 16;
const size_t kHashBytes = 32;
const unsigned long kMaxAcceptedIterations = 10000000;   // refuse absurd values read from a damaged database

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

struct Parsed {
    bool ok = false;
    unsigned long iterations = 0;
    Bytes salt, hash;
};

Parsed parse(const std::string& stored) {
    Parsed p;
    const auto parts = split(stored, '$');
    if (parts.size() != 4 || parts[0] != kAlgorithm) return p;
    char* end = nullptr;
    const unsigned long it = std::strtoul(parts[1].c_str(), &end, 10);
    if (parts[1].empty() || !end || *end != '\0' || it == 0 || it > kMaxAcceptedIterations) return p;
    try {
        p.salt = fromHex(parts[2]);
        p.hash = fromHex(parts[3]);
    } catch (const std::invalid_argument&) {
        return p;
    }
    if (p.salt.size() != kSaltBytes || p.hash.size() != kHashBytes) return p;
    p.iterations = it;
    p.ok = true;
    return p;
}

}  // namespace

std::string hashPassword(const std::string& password, unsigned long iterations) {
    const Bytes salt = randomBytes(kSaltBytes);
    const Bytes hash = pbkdf2Sha256(password, salt, iterations, kHashBytes);
    return std::string(kAlgorithm) + "$" + std::to_string(iterations) + "$" + toHex(salt) + "$" + toHex(hash);
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    const Parsed p = parse(stored);
    if (!p.ok) return false;
    return constantTimeEqual(pbkdf2Sha256(password, p.salt, p.iterations, kHashBytes), p.hash);
}

bool needsRehash(const std::string& stored, unsigned long iterations) {
    const Parsed p = parse(stored);
    return !p.ok || p.iterations < iterations;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

Check checkUsername(const std::string& username) {
    Check c;
    if (username.size() < 3 || username.size() > 20) {
        c.ok = false;
        c.message = "The username must be 3 to 20 characters long.";
        return c;
    }
    for (unsigned char ch : username)
        if (!std::isalnum(ch) || ch >= 0x80) {
            c.ok = false;
            c.message = "The username may contain only the letters A-Z and the digits 0-9.";
            return c;
        }
    return c;
}

Check checkPassword(const std::string& password, const std::string& username) {
    Check c;
    const size_t chars = utf8Length(password);
    if (chars < 8) {
        c.ok = false;
        c.message = "The password must be at least 8 characters long.";
    } else if (chars > 128 || password.size() > 512) {
        c.ok = false;
        c.message = "The password must be at most 128 characters long.";
    } else {
        std::string a, b;
        for (char ch : password) a += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (char ch : username) b += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!b.empty() && a == b) {
            c.ok = false;
            c.message = "The password must not be the same as the username.";
        }
    }
    return c;
}

}  // namespace auth
}  // namespace metro
