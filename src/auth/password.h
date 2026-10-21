// Password hashing and the rules for usernames and passwords.
//
// A stored hash looks like:   pbkdf2-sha256$600000$<salt as hex>$<hash as hex>
// The algorithm and the iteration count are stored WITH the hash (OWASP's advice), so the cost can be raised
// later, or the algorithm replaced, without locking anyone out: old hashes keep verifying, and needsRehash()
// tells the caller to re-hash them the next time the user logs in.
#pragma once
#include <string>

namespace metro {
namespace auth {

// OWASP's Password Storage Cheat Sheet: PBKDF2-HMAC-SHA256 with 600,000 or more iterations.
// (OWASP prefers Argon2id; PBKDF2 is the listed choice where operating-system-provided, FIPS-validated
// implementations are wanted, which is why it is used here.)
constexpr unsigned long kDefaultIterations = 600000;

std::string hashPassword(const std::string& password, unsigned long iterations = kDefaultIterations);

// False for a wrong password and also for any malformed stored string.
bool verifyPassword(const std::string& password, const std::string& stored);

// True if the stored hash uses a different algorithm or fewer iterations than 'iterations'.
bool needsRehash(const std::string& stored, unsigned long iterations = kDefaultIterations);

struct Check {
    bool ok = true;
    std::string message;   // why it was rejected (safe to show to the person registering)
};

// 3 to 20 characters, ASCII letters and digits only.
Check checkUsername(const std::string& username);

// At least 8 and at most 128 characters (Unicode characters, not bytes), at most 512 bytes, and not the same as the username.
// Any characters are allowed, as OWASP advises, so that people can use their own language.
Check checkPassword(const std::string& password, const std::string& username);

// Number of Unicode characters in a UTF-8 string (counts bytes that are not continuation bytes).
size_t utf8Length(const std::string& s);

}  // namespace auth
}  // namespace metro
