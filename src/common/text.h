// Small text helpers shared by the web code and the saved-journey code.
#pragma once
#include <string>

namespace metro {

// True if the whole string is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool isValidUtf8(const std::string& s);

// Returns the string with every invalid byte replaced by U+FFFD, so the result is always valid UTF-8.
std::string sanitizeUtf8(const std::string& s);

}  // namespace metro
