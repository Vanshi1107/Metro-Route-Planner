#include "text.h"

namespace metro {

namespace {

// Length of the valid UTF-8 sequence starting at s[i], or 0 if there is none.
size_t sequenceLength(const std::string& s, size_t i) {
    const size_t n = s.size();
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k, unsigned char lo, unsigned char hi) {
        if (i + k >= n) return false;
        const unsigned char b = static_cast<unsigned char>(s[i + k]);
        return b >= lo && b <= hi;
    };
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(1, 0x80, 0xBF) ? 2 : 0;
    if (c == 0xE0) return cont(1, 0xA0, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
    if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) return cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
    if (c == 0xED) return cont(1, 0x80, 0x9F) && cont(2, 0x80, 0xBF) ? 3 : 0;   // excludes surrogates
    if (c == 0xF0) return cont(1, 0x90, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
    if (c >= 0xF1 && c <= 0xF3) return cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
    if (c == 0xF4) return cont(1, 0x80, 0x8F) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
    return 0;
}

}  // namespace

bool isValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const size_t len = sequenceLength(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

std::string sanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t len = sequenceLength(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s, i, len);
            i += len;
        }
    }
    return out;
}

}  // namespace metro
