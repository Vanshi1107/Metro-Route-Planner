// The web layer's own, library-independent request and response types, plus parsing of the pieces of a URL.
// The real HTTP server library only has to convert to and from these (see src/tools/metro_web.cpp), which is what
// lets the whole API be tested without a network.
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace metro {
namespace web {

struct Param {
    std::string name, value;
};

struct Request {
    std::string method;                         // upper-case
    std::string path;                           // percent-decoded, without the query string
    std::string rawQuery;
    std::vector<Param> query;
    std::map<std::string, std::string> headers; // names in lower case
    std::string body;
    std::vector<Param> form;                    // from the body when it is application/x-www-form-urlencoded
    bool badEncoding = false;                   // the path, query or form was not valid percent-encoding

    std::string header(const std::string& lowerName) const;
    std::string queryParam(const std::string& name) const;   // "" if absent
    std::string formParam(const std::string& name) const;    // "" if absent
};

struct Response {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;   // extra headers (Set-Cookie, Allow, ...)
};

// Builds a Request from the raw pieces of an HTTP request. Never throws.
Request makeRequest(const std::string& method, const std::string& target, const std::map<std::string, std::string>& headersAnyCase,
                    const std::string& body);

// Percent-decoding. Returns false for a malformed %xx or a decoded NUL byte.
bool urlDecode(const std::string& in, bool plusIsSpace, std::string& out);

// "a=1&b=two%20words" -> pairs. Returns false if malformed or there are more than 50 pairs.
bool parseUrlEncoded(const std::string& in, std::vector<Param>& out);

}  // namespace web
}  // namespace metro
