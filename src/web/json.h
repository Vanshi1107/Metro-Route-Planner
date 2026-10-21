// A small JSON writer. It always produces valid JSON:
//  * text is made valid UTF-8 first (bad bytes become U+FFFD)
//  * quotes, backslashes and control characters are escaped
//  * < > & and the line separators U+2028/U+2029 are escaped too, so the output is safe even if it is ever
//    placed inside an HTML page
#pragma once
#include <string>
#include <vector>

namespace metro {
namespace web {

class JsonWriter {
public:
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();
    JsonWriter& key(const std::string& name);    // inside an object, before its value
    JsonWriter& value(const std::string& text);
    JsonWriter& value(const char* text);
    JsonWriter& value(long long number);
    JsonWriter& value(int number);
    JsonWriter& value(double number);
    JsonWriter& value(bool flag);
    JsonWriter& null();

    // key + value in one call
    template <typename T>
    JsonWriter& field(const std::string& name, const T& v) {
        key(name);
        return value(v);
    }

    const std::string& str() const { return out_; }

    static std::string quote(const std::string& text);   // the escaped, double-quoted form of a string

private:
    void beforeValue();
    std::string out_;
    std::vector<bool> first_;   // per open container: nothing written inside it yet?
    bool afterKey_ = false;
};

}  // namespace web
}  // namespace metro
