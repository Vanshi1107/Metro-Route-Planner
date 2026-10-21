#include "json.h"

#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>

#include "../common/text.h"

namespace metro {
namespace web {

std::string JsonWriter::quote(const std::string& raw) {
    const std::string text = sanitizeUtf8(raw);
    std::string out = "\"";
    char buf[8];
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<': out += "\\u003c"; break;
            case '>': out += "\\u003e"; break;
            case '&': out += "\\u0026"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                           (static_cast<unsigned char>(text[i + 2]) == 0xA8 || static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
                    out += static_cast<unsigned char>(text[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
                    i += 2;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

void JsonWriter::beforeValue() {
    if (afterKey_) {          // the key already wrote the comma and the colon
        afterKey_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
}

JsonWriter& JsonWriter::beginObject() {
    beforeValue();
    out_ += '{';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    out_ += '}';
    if (!first_.empty()) first_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    beforeValue();
    out_ += '[';
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    out_ += ']';
    if (!first_.empty()) first_.pop_back();
    return *this;
}

JsonWriter& JsonWriter::key(const std::string& name) {
    if (!first_.empty()) {
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
    out_ += quote(name);
    out_ += ':';
    afterKey_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(const std::string& text) {
    beforeValue();
    out_ += quote(text);
    return *this;
}

JsonWriter& JsonWriter::value(const char* text) { return value(std::string(text)); }

JsonWriter& JsonWriter::value(long long number) {
    beforeValue();
    out_ += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::value(int number) { return value(static_cast<long long>(number)); }

JsonWriter& JsonWriter::value(double number) {
    beforeValue();
    if (!std::isfinite(number)) {
        out_ += "null";   // JSON has no NaN or infinity
        return *this;
    }
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(10);
    o << number;
    out_ += o.str();
    return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
    beforeValue();
    out_ += flag ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null() {
    beforeValue();
    out_ += "null";
    return *this;
}

}  // namespace web
}  // namespace metro
