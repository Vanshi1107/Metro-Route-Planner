#include "http.h"

#include <algorithm>
#include <cctype>

namespace metro {
namespace web {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool urlDecode(const std::string& in, bool plusIsSpace, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%') {
            const int hi = i + 1 < in.size() ? hexValue(in[i + 1]) : -1;
            const int lo = i + 2 < in.size() ? hexValue(in[i + 2]) : -1;
            if (hi < 0 || lo < 0) return false;
            const char decoded = static_cast<char>((hi << 4) | lo);
            if (decoded == '\0') return false;
            out += decoded;
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return true;
}

bool parseUrlEncoded(const std::string& in, std::vector<Param>& out) {
    out.clear();
    size_t pos = 0;
    while (pos <= in.size()) {
        size_t amp = in.find('&', pos);
        if (amp == std::string::npos) amp = in.size();
        const std::string part = in.substr(pos, amp - pos);
        if (!part.empty()) {
            if (out.size() >= 50) return false;
            const size_t eq = part.find('=');
            Param p;
            if (!urlDecode(part.substr(0, eq), true, p.name)) return false;
            if (eq != std::string::npos && !urlDecode(part.substr(eq + 1), true, p.value)) return false;
            out.push_back(p);
        }
        if (amp == in.size()) break;
        pos = amp + 1;
    }
    return true;
}

std::string Request::header(const std::string& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? std::string() : it->second;
}

std::string Request::queryParam(const std::string& name) const {
    for (const auto& p : query)
        if (p.name == name) return p.value;
    return std::string();
}

std::string Request::formParam(const std::string& name) const {
    for (const auto& p : form)
        if (p.name == name) return p.value;
    return std::string();
}

Request makeRequest(const std::string& method, const std::string& target, const std::map<std::string, std::string>& headersAnyCase,
                    const std::string& body) {
    Request r;
    r.method = method;
    for (char& c : r.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& h : headersAnyCase) r.headers[lower(h.first)] = h.second;
    r.body = body;

    const size_t q = target.find('?');
    const std::string rawPath = target.substr(0, q);
    r.rawQuery = q == std::string::npos ? std::string() : target.substr(q + 1);
    if (!urlDecode(rawPath, false, r.path)) r.badEncoding = true;
    if (!r.rawQuery.empty() && !parseUrlEncoded(r.rawQuery, r.query)) r.badEncoding = true;
    if (lower(r.header("content-type")).rfind("application/x-www-form-urlencoded", 0) == 0 && !body.empty())
        if (!parseUrlEncoded(body, r.form)) r.badEncoding = true;
    return r;
}

}  // namespace web
}  // namespace metro
