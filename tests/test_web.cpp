#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "../src/common/db.h"
#include "../src/common/text.h"
#include "../src/import/importer.h"
#include "../src/web/api.h"
#include "../src/web/http.h"
#include "../src/web/json.h"
#include "check.h"

namespace fs = std::filesystem;
using namespace metro;
using namespace metro::web;

// ---- a strict JSON syntax checker, so every response can be proven to be valid JSON --------------------
struct JsonChecker {
    const std::string& s;
    size_t i = 0;
    explicit JsonChecker(const std::string& text) : s(text) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool lit(const char* w) {
        const size_t n = std::string(w).size();
        if (s.compare(i, n, w) != 0) return false;
        i += n;
        return true;
    }
    bool str() {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') { ++i; return true; }
            if (c < 0x20) return false;
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return false;
                const char e = s[i];
                if (e == 'u') {
                    for (int k = 1; k <= 4; ++k)
                        if (i + static_cast<size_t>(k) >= s.size() || !std::isxdigit(static_cast<unsigned char>(s[i + static_cast<size_t>(k)]))) return false;
                    i += 4;
                } else if (std::string("\"\\/bfnrt").find(e) == std::string::npos) {
                    return false;
                }
            }
            ++i;
        }
        return false;
    }
    bool num() {
        const size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        if (s[i] == '0') ++i; else while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i < s.size() && s[i] == '.') { ++i; if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) { ++i; if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i; if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i; }
        return i > start;
    }
    bool value(int depth) {
        if (depth > 100) return false;
        ws();
        if (i >= s.size()) return false;
        if (s[i] == '{') {
            ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                if (!str()) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                if (!value(depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (s[i] == '[') {
            ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                if (!value(depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (s[i] == '"') return str();
        if (s[i] == 't') return lit("true");
        if (s[i] == 'f') return lit("false");
        if (s[i] == 'n') return lit("null");
        return num();
    }
    bool valid() {
        if (!isValidUtf8(s)) return false;
        if (!value(0)) return false;
        ws();
        return i == s.size();
    }
};
static bool validJson(const std::string& text) { return JsonChecker(text).valid(); }

static std::string headerOf(const Response& r, const std::string& name) {
    for (const auto& h : r.headers)
        if (h.first == name) return h.second;
    return std::string();
}
static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static long long g_now = 1700000000;

// ---- the test world -------------------------------------------------------------------------------------------
struct World {
    fs::path dir;
    Database db{":memory:", true};
    std::unique_ptr<Api> api;
    std::vector<std::string> logged;

    World(const std::string& schemaPath, const std::string& gtfs, const std::string& config, bool secureCookies = false, size_t maxBody = 8 * 1024) {
        std::error_code ec;
        dir = fs::temp_directory_path(ec);
        if (ec || dir.empty()) dir = fs::current_path();
        dir /= "metro_test_web_root";
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "web" / "vendor" / "leaflet", ec);
        fs::create_directories(dir / "web" / "sub", ec);
        writeFile(dir / "web" / "index.html", "<!doctype html><title>test</title><script src=\"/app.js\"></script>");
        writeFile(dir / "web" / "app.js", "console.log('hi');");
        writeFile(dir / "web" / "style.css", "body{margin:0}");
        writeFile(dir / "web" / "vendor" / "leaflet" / "leaflet.js", "/* leaflet */");
        writeFile(dir / "web" / ".hidden.html", "hidden");
        writeFile(dir / "web" / "tool.exe", "binary");
        writeFile(dir / "secret.txt", "TOP SECRET");

        db.exec(readFile(schemaPath));
        import::importInto(db, import::buildModel(gtfs, config));
        ApiConfig cfg;
        cfg.webRoot = (dir / "web").string();
        cfg.secureCookies = secureCookies;
        cfg.maxBodyBytes = maxBody;
        cfg.account.hashIterations = 1000;
        cfg.account.clock = [] { return g_now; };
        cfg.log = [this](const std::string& m) { logged.push_back(m); };
        api = std::make_unique<Api>(db, cfg);
    }
    ~World() {
        api.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    Response get(const std::string& target, const std::string& cookie = "", const std::map<std::string, std::string>& extra = {}) {
        std::map<std::string, std::string> h = extra;
        h["Host"] = "localhost:8080";
        if (!cookie.empty()) h["Cookie"] = cookie;
        return api->handle(makeRequest("GET", target, h, ""));
    }
    Response post(const std::string& target, const std::string& form, const std::string& cookie = "", const std::string& method = "POST") {
        std::map<std::string, std::string> h = {{"Host", "localhost:8080"}, {"X-Requested-With", "metro-web"}, {"Content-Type", "application/x-www-form-urlencoded"}};
        if (!cookie.empty()) h["Cookie"] = cookie;
        return api->handle(makeRequest(method, target, h, form));
    }
    std::string loginCookie(const std::string& user, const std::string& pass) {
        Response r = post("/api/login", "username=" + user + "&password=" + pass);
        const std::string set = headerOf(r, "Set-Cookie");
        const size_t semi = set.find(';');
        return r.status == 200 && semi != std::string::npos ? set.substr(0, semi) : std::string();
    }
};

// ---- the tests --------------------------------------------------------------------------------------------------
static void testJsonWriter() {
    JsonWriter w;
    w.beginObject().field("a", 1).field("b", "x").key("list").beginArray().value(1).value("two").value(true).null().beginObject().field("k", 2.5).endObject().endArray().endObject();
    CHECK_EQ(w.str(), std::string("{\"a\":1,\"b\":\"x\",\"list\":[1,\"two\",true,null,{\"k\":2.5}]}"));
    CHECK(validJson(w.str()));
    CHECK_EQ(JsonWriter::quote("a\"b\\c\n\t"), std::string("\"a\\\"b\\\\c\\n\\t\""));
    CHECK_EQ(JsonWriter::quote("<script>&"), std::string("\"\\u003cscript\\u003e\\u0026\""));
    CHECK_EQ(JsonWriter::quote(std::string("\x01\x1f", 2)), std::string("\"\\u0001\\u001f\""));
    CHECK_EQ(JsonWriter::quote("\xE0\xA4\xA8"), std::string("\"\xE0\xA4\xA8\""));              // Hindi passes through untouched
    CHECK_EQ(JsonWriter::quote("\xE2\x80\xA8"), std::string("\"\\u2028\""));                   // line separator is escaped
    CHECK_EQ(JsonWriter::quote("ok\xFF" "bad"), std::string("\"ok\xEF\xBF\xBD" "bad\""));      // invalid bytes become U+FFFD
    CHECK(validJson(JsonWriter::quote(std::string("\xC0\xAF\xED\xA0\x80\xF5", 6))));            // overlong, surrogate, too large: still valid output
    JsonWriter n;
    n.beginArray().value(0.5).value(-3).value(1e10).endArray();
    CHECK(validJson(n.str()));
    JsonWriter empty;
    empty.beginObject().endObject();
    CHECK_EQ(empty.str(), std::string("{}"));

    CHECK(isValidUtf8("plain") && isValidUtf8("\xE0\xA4\xA8") && isValidUtf8(""));
    CHECK(!isValidUtf8("\xC0\xAF") && !isValidUtf8("\xED\xA0\x80") && !isValidUtf8("\xF4\x90\x80\x80") && !isValidUtf8("\xE0\xA4"));
    CHECK(validJson("{\"a\":[1,2,{\"b\":null}],\"c\":\"\\u00e9\"}"));
    CHECK(!validJson("{\"a\":1,}") && !validJson("{a:1}") && !validJson("[1,2") && !validJson("\"\x01\""));   // the checker itself is strict
}

static void testHttpParsing() {
    std::string out;
    CHECK(urlDecode("a%20b+c", true, out));
    CHECK_EQ(out, std::string("a b c"));
    CHECK(urlDecode("a+b", false, out));
    CHECK_EQ(out, std::string("a+b"));
    CHECK(urlDecode("%E0%A4%A8", true, out));
    CHECK_EQ(out, std::string("\xE0\xA4\xA8"));
    CHECK(!urlDecode("%", true, out));
    CHECK(!urlDecode("%4", true, out));
    CHECK(!urlDecode("%zz", true, out));
    CHECK(!urlDecode("%00", true, out));                    // a NUL byte is never accepted

    std::vector<Param> p;
    CHECK(parseUrlEncoded("a=1&b=two%20words&c&=x&&d=", p));
    CHECK_EQ(p.size(), static_cast<size_t>(5));
    CHECK_EQ(p[1].value, std::string("two words"));
    CHECK(!parseUrlEncoded("a=%zz", p));
    std::string many;
    for (int i = 0; i < 60; ++i) many += "k" + std::to_string(i) + "=1&";
    CHECK(!parseUrlEncoded(many, p));                       // too many parameters

    Request r = makeRequest("get", "/api/route?from=1&to=5&day=sunday", {{"HOST", "x"}, {"Cookie", "session=abc"}}, "");
    CHECK_EQ(r.method, std::string("GET"));
    CHECK_EQ(r.path, std::string("/api/route"));
    CHECK_EQ(r.queryParam("to"), std::string("5"));
    CHECK_EQ(r.header("host"), std::string("x"));           // header names are matched in lower case
    CHECK_EQ(r.header("cookie"), std::string("session=abc"));
    CHECK(!r.badEncoding);
    Request f = makeRequest("POST", "/api/login", {{"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"}}, "username=a&password=b%26c");
    CHECK_EQ(f.formParam("password"), std::string("b&c"));
    CHECK(makeRequest("GET", "/a%2", {}, "").badEncoding);
    CHECK(makeRequest("GET", "/a?x=%zz", {}, "").badEncoding);
    CHECK(makeRequest("GET", "/a%00b", {}, "").badEncoding);
    CHECK_EQ(makeRequest("GET", "/a%2e%2e/b", {}, "").path, std::string("/a../b"));   // decoded exactly once
}

static void testStaticFiles(const std::string& schema, const std::string& gtfs, const std::string& config) {
    World w(schema, gtfs, config);
    Response root = w.get("/");
    CHECK_EQ(root.status, 200);
    CHECK(contains(root.body, "<title>test</title>"));
    CHECK(contains(root.contentType, "text/html"));
    CHECK(contains(w.get("/app.js").contentType, "javascript"));
    CHECK(contains(w.get("/style.css").contentType, "text/css"));
    CHECK_EQ(w.get("/vendor/leaflet/leaflet.js").status, 200);
    CHECK_EQ(w.get("/index.html").status, 200);

    for (const char* bad : {"/../secret.txt", "/%2e%2e/secret.txt", "/..%2fsecret.txt", "/%2e%2e%2fsecret.txt", "/a/../../secret.txt", "/sub/../../secret.txt",
                            "/vendor/../../secret.txt", "//secret.txt", "/..\\secret.txt", "/.hidden.html", "/sub", "/sub/", "/tool.exe", "/missing.js",
                            "/vendor/leaflet", "/app.js/", "/%2e", "/.", "/..", "/app.js%00.png"}) {
        Response r = w.get(bad);
        if (r.status != 404 && r.status != 400) std::cerr << "unexpected status " << r.status << " for " << bad << "\n";
        CHECK(r.status == 404 || r.status == 400);
        CHECK(!contains(r.body, "SECRET"));
        CHECK(!contains(r.body, "hidden"));
    }
    CHECK_EQ(w.get("/" + std::string(70, 'a') + ".js").status, 404);                 // over-long names are refused

    // a symbolic link inside the web folder must not lead out of it
    std::error_code ec;
    fs::create_symlink(w.dir / "secret.txt", w.dir / "web" / "link.txt", ec);
    if (!ec) {
        CHECK_EQ(w.get("/link.txt").status, 404);
        CHECK(!contains(w.get("/link.txt").body, "SECRET"));
    }

    Response head = w.api->handle(makeRequest("HEAD", "/app.js", {{"Host", "x"}}, ""));
    CHECK_EQ(head.status, 200);
    Response postStatic = w.post("/app.js", "");
    CHECK(postStatic.status == 405 || postStatic.status == 404);

    // security headers on every kind of response
    for (const Response& r : {root, w.get("/missing.js"), w.get("/api/status")}) {
        CHECK(contains(headerOf(r, "Content-Security-Policy"), "script-src 'self'"));
        CHECK(contains(headerOf(r, "Content-Security-Policy"), "https://tile.openstreetmap.org"));
        CHECK(contains(headerOf(r, "Content-Security-Policy"), "default-src 'none'"));
        CHECK(contains(headerOf(r, "Content-Security-Policy"), "frame-ancestors 'none'"));
        CHECK_EQ(headerOf(r, "X-Content-Type-Options"), std::string("nosniff"));
        CHECK_EQ(headerOf(r, "X-Frame-Options"), std::string("DENY"));
        CHECK(!headerOf(r, "Referrer-Policy").empty());
    }
    CHECK_EQ(headerOf(w.get("/api/status"), "Cache-Control"), std::string("no-store"));
    CHECK(!contains(headerOf(root, "Content-Security-Policy"), "unsafe-inline"));
    CHECK(!contains(headerOf(root, "Content-Security-Policy"), "unsafe-eval"));
}

static void testReadOnlyApi(const std::string& schema, const std::string& gtfs, const std::string& config) {
    World w(schema, gtfs, config);
    Response st = w.get("/api/status");
    CHECK_EQ(st.status, 200);
    CHECK(validJson(st.body));
    CHECK(contains(st.body, "\"dataVersion\":1"));
    CHECK(contains(st.body, "\"algorithm\":\"bellman-ford\""));
    CHECK(contains(st.body, "\"stations\":17"));

    Response stations = w.get("/api/stations");
    CHECK(validJson(stations.body));
    CHECK(contains(stations.body, "\"name\":\"Station A\""));
    CHECK(contains(stations.body, "\"gtfsId\":\"A\""));                         // lets the page tell which journeys are already saved
    CHECK(contains(stations.body, "\"lat\":28.5"));
    CHECK(contains(stations.contentType, "application/json"));
    Response lines = w.get("/api/lines");
    CHECK(validJson(lines.body));
    CHECK(contains(lines.body, "\"name\":\"Red Line\""));
    CHECK(contains(lines.body, "\"colour\":\"FF0000\""));

    // search: the answer says what kind it is
    Response exact = w.get("/api/search?q=station%20a");
    CHECK(validJson(exact.body));
    CHECK(contains(exact.body, "\"status\":\"found\""));
    CHECK(contains(exact.body, "\"name\":\"Station A\""));
    CHECK(!contains(exact.body, "Station B"));
    Response several = w.get("/api/search?q=station");
    CHECK(validJson(several.body));
    CHECK(contains(several.body, "\"status\":\"ambiguous\""));
    Response typo = w.get("/api/search?q=statoin%20a");                           // a typo: nothing matches, but there are close names
    CHECK(validJson(typo.body));
    CHECK(contains(typo.body, "\"status\":\"suggestions\""));
    CHECK(contains(typo.body, "Station A"));
    CHECK_EQ(w.get("/api/search?q=").body, std::string("{\"status\":\"none\",\"stations\":[]}"));
    CHECK_EQ(w.get("/api/search").body, std::string("{\"status\":\"none\",\"stations\":[]}"));
    CHECK_EQ(w.get("/api/search?q=" + std::string(61, 'a')).status, 400);
    CHECK_EQ(w.get("/api/search?q=%FF%FE").status, 400);                           // invalid UTF-8
    CHECK(!contains(w.get("/api/search?q=%3Cscript%3E").body, "<script>"));

    // routes: A(1) to E(5) on the Red line
    Response r = w.get("/api/route?from=1&to=5");
    CHECK_EQ(r.status, 200);
    CHECK(validJson(r.body));
    CHECK(contains(r.body, "\"found\":true"));
    CHECK(contains(r.body, "\"prefer\":\"fastest\""));                           // the default need
    CHECK(contains(r.body, "\"primary\":{\"criterion\":\"fastest\""));
    CHECK(contains(r.body, "\"minutes\":9"));                                     // 560 s
    CHECK(contains(r.body, "\"changes\":0"));
    CHECK(contains(r.body, "\"kind\":\"ride\""));
    CHECK(contains(r.body, "\"inr\":20"));
    CHECK(contains(r.body, "\"badges\":[\"fastest\""));                          // this route is best on every measure
    CHECK(contains(r.body, "\"others\":[{\"criterion\":\"cheapest\""));            // the other needs are always answered
    CHECK(contains(r.body, "\"criterion\":\"fewest\""));
    CHECK(contains(r.body, "\"notes\""));
    CHECK(contains(r.body, "\"notices\""));
    for (const char* need : {"cheapest", "fewest", "fastest"}) {
        Response pr = w.get(std::string("/api/route?from=1&to=5&prefer=") + need);
        CHECK_EQ(pr.status, 200);
        CHECK(validJson(pr.body));
        CHECK(contains(pr.body, std::string("\"prefer\":\"") + need + "\""));
        CHECK(contains(pr.body, std::string("\"criterion\":\"") + need + "\""));
    }
    CHECK_EQ(w.get("/api/route?from=1&to=5&prefer=cheap").status, 400);              // unknown need
    CHECK_EQ(w.get("/api/route?from=1&to=5&prefer=").status, 200);                   // empty means the default
    Response multi = w.get("/api/route?from=9&to=10&prefer=cheapest");
    CHECK(validJson(multi.body));
    CHECK(contains(multi.body, "owest fare"));                                        // one route, or several that cost the same
    CHECK(contains(w.get("/api/route?from=1&to=5&day=sunday").body, "\"inr\":10"));

    Response change = w.get("/api/route?from=9&to=10");                           // L to M: change at the Blue junction
    CHECK(validJson(change.body));
    CHECK(contains(change.body, "\"kind\":\"change\""));
    CHECK(contains(change.body, "\"changes\":1"));
    Response mixed = w.get("/api/route?from=1&to=17");                            // Red then Rapid
    CHECK(validJson(mixed.body));
    CHECK(contains(mixed.body, "\"available\":false"));
    CHECK(contains(mixed.body, "mixes operators"));
    Response walkOnly = w.get("/api/route?from=5&to=16");                         // the confirmed link E - S1
    CHECK(validJson(walkOnly.body));
    CHECK(contains(walkOnly.body, "\"kind\":\"walk\""));
    Response none = w.get("/api/route?from=1&to=11");                             // Green is disconnected
    CHECK_EQ(none.status, 200);
    CHECK(validJson(none.body));
    CHECK(contains(none.body, "\"found\":false"));

    for (const char* bad : {"/api/route", "/api/route?from=1", "/api/route?from=a&to=2", "/api/route?from=0&to=2", "/api/route?from=1&to=99",
                            "/api/route?from=1&to=1", "/api/route?from=-1&to=2", "/api/route?from=1&to=2&day=monday", "/api/route?from=1e3&to=2",
                            "/api/route?from=%20&to=2", "/api/route?from=1&to=99999999999"}) {
        Response e = w.get(bad);
        if (e.status != 400) std::cerr << "expected 400 for " << bad << " got " << e.status << "\n";
        CHECK_EQ(e.status, 400);
        CHECK(validJson(e.body));
        CHECK(contains(e.body, "\"error\""));
    }
}

static void testProtection(const std::string& schema, const std::string& gtfs, const std::string& config) {
    World w(schema, gtfs, config);
    // changes must carry the custom header
    Response noHeader = w.api->handle(makeRequest("POST", "/api/login", {{"Host", "localhost:8080"}, {"Content-Type", "application/x-www-form-urlencoded"}}, "username=a&password=b"));
    CHECK_EQ(noHeader.status, 403);
    // ... and a foreign Origin is refused even with the header
    Response foreign = w.api->handle(makeRequest("POST", "/api/login", {{"Host", "localhost:8080"}, {"Origin", "http://evil.example"}, {"X-Requested-With", "metro-web"},
                                                                       {"Content-Type", "application/x-www-form-urlencoded"}}, "username=a&password=b"));
    CHECK_EQ(foreign.status, 403);
    Response lookalike = w.api->handle(makeRequest("POST", "/api/login", {{"Host", "localhost:8080"}, {"Origin", "http://localhost:8080.evil.example"}, {"X-Requested-With", "metro-web"},
                                                                         {"Content-Type", "application/x-www-form-urlencoded"}}, "username=a&password=b"));
    CHECK_EQ(lookalike.status, 403);
    Response same = w.api->handle(makeRequest("POST", "/api/login", {{"Host", "localhost:8080"}, {"Origin", "http://localhost:8080"}, {"X-Requested-With", "metro-web"},
                                                                    {"Content-Type", "application/x-www-form-urlencoded"}}, "username=a&password=b"));
    CHECK_EQ(same.status, 401);                                                  // allowed through, then simply a wrong login
    Response wrongType = w.api->handle(makeRequest("POST", "/api/login", {{"Host", "localhost:8080"}, {"X-Requested-With", "metro-web"}, {"Content-Type", "text/plain"}}, "username=a&password=b"));
    CHECK_EQ(wrongType.status, 415);
    Response json = w.api->handle(makeRequest("POST", "/api/login", {{"Host", "localhost:8080"}, {"X-Requested-With", "metro-web"}, {"Content-Type", "application/json"}}, "{\"username\":\"a\"}"));
    CHECK_EQ(json.status, 415);
    CHECK_EQ(w.api->handle(makeRequest("DELETE", "/api/journeys/1", {{"Host", "x"}}, "")).status, 403);   // DELETE needs the header too
    CHECK_EQ(w.get("/api/login").status, 405);                                   // reading a change-only address
    CHECK(!headerOf(w.get("/api/login"), "Allow").empty());
    CHECK_EQ(w.post("/api/status", "").status, 405);
    CHECK_EQ(w.post("/api/stations", "").status, 405);
    CHECK_EQ(w.post("/api/route", "").status, 405);
    CHECK_EQ(w.post("/api/x", "", "", "PUT").status, 405);
    CHECK_EQ(w.post("/api/x", "", "", "PATCH").status, 405);
    Response unknown = w.get("/api/nothing-here");
    CHECK_EQ(unknown.status, 404);
    CHECK(validJson(unknown.body));

    // size limits
    CHECK_EQ(w.post("/api/login", std::string(9000, 'x')).status, 413);
    CHECK_EQ(w.get("/api/search?q=" + std::string(3000, 'a')).status, 414);
    CHECK_EQ(w.get("/api/search?q=%zz").status, 400);
    CHECK_EQ(w.get("/api/%zz").status, 400);
}

static void testAccountsOverHttp(const std::string& schema, const std::string& gtfs, const std::string& config) {
    World w(schema, gtfs, config);
    CHECK_EQ(w.post("/api/register", "username=ab&password=correct%20horse").status, 400);       // username too short
    CHECK_EQ(w.post("/api/register", "username=alice&password=short").status, 400);
    Response ok = w.post("/api/register", "username=alice&password=correct%20horse");
    CHECK_EQ(ok.status, 201);
    CHECK(validJson(ok.body));
    CHECK(!contains(ok.body, "correct"));
    CHECK_EQ(w.post("/api/register", "username=ALICE&password=another%20pass").status, 409);
    CHECK_EQ(w.post("/api/register", "username=bobby&password=%D0%BF%D0%B0%D1%80%D0%BE%D0%BB%D1%8C123").status, 201);   // a Cyrillic password is fine

    Response wrong = w.post("/api/login", "username=alice&password=wrong%20guess");
    Response unknownUser = w.post("/api/login", "username=nobodyhere&password=wrong%20guess");
    CHECK_EQ(wrong.status, 401);
    CHECK_EQ(unknownUser.status, 401);
    CHECK_EQ(wrong.body, unknownUser.body);                                                     // no hint which names exist
    CHECK(headerOf(wrong, "Set-Cookie").empty());

    Response in = w.post("/api/login", "username=alice&password=correct%20horse");
    CHECK_EQ(in.status, 200);
    CHECK(validJson(in.body));
    const std::string set = headerOf(in, "Set-Cookie");
    CHECK(set.rfind("session=", 0) == 0);
    CHECK(contains(set, "HttpOnly"));
    CHECK(contains(set, "SameSite=Strict"));
    CHECK(contains(set, "Path=/"));
    CHECK(contains(set, "Max-Age="));
    CHECK(!contains(set, "Secure"));                                                            // plain http on localhost
    const std::string token = set.substr(8, 64);
    CHECK(!contains(in.body, token));                                                           // the token is only ever in the cookie

    const std::string cookie = "session=" + token;
    Response me = w.get("/api/me", cookie);
    CHECK(contains(me.body, "\"loggedIn\":true"));
    CHECK(contains(me.body, "\"username\":\"alice\""));
    CHECK(contains(w.get("/api/me").body, "\"loggedIn\":false"));
    CHECK(contains(w.get("/api/me", "session=" + std::string(64, 'a')).body, "\"loggedIn\":false"));
    CHECK(contains(w.get("/api/me", "session=garbage; other=1").body, "\"loggedIn\":false"));
    CHECK(contains(w.get("/api/me", "x=1; session=" + token).body, "\"loggedIn\":true"));      // found among other cookies

    Response out = w.post("/api/logout", "", cookie);
    CHECK_EQ(out.status, 200);
    CHECK(contains(headerOf(out, "Set-Cookie"), "Max-Age=0"));
    CHECK(contains(w.get("/api/me", cookie).body, "\"loggedIn\":false"));                      // the old cookie no longer works
    CHECK_EQ(w.post("/api/logout", "").status, 200);                                            // logging out twice is harmless

    // lockout after repeated failures
    for (int i = 0; i < 5; ++i) w.post("/api/login", "username=bobby&password=wrong%20guess1");
    Response locked = w.post("/api/login", "username=bobby&password=" + std::string("%D0%BF%D0%B0%D1%80%D0%BE%D0%BB%D1%8C123"));
    CHECK_EQ(locked.status, 429);
    CHECK(!headerOf(locked, "Retry-After").empty());
    CHECK(validJson(locked.body));

    // SQL-looking input is just text
    CHECK_EQ(w.post("/api/login", "username=x%27%20OR%20%271%27%3D%271&password=anything").status, 401);
    CHECK_EQ(w.post("/api/register", "username=a%27%3B%20DROP%20TABLE%20users%3B--&password=correct%20horse").status, 400);
    CHECK_EQ(w.post("/api/login", "username=alice&password=correct%20horse").status, 200);

    // cookies are marked Secure when the site is served over HTTPS
    World secure(schema, gtfs, config, true);
    secure.post("/api/register", "username=alice&password=correct%20horse");
    CHECK(contains(headerOf(secure.post("/api/login", "username=alice&password=correct%20horse"), "Set-Cookie"), "Secure"));
}

static void testJourneysOverHttp(const std::string& schema, const std::string& gtfs, const std::string& config) {
    World w(schema, gtfs, config);
    CHECK_EQ(w.get("/api/journeys").status, 401);                                               // not logged in
    CHECK_EQ(w.post("/api/journeys", "from=1&to=5&mode=fastest&day=weekday").status, 401);
    CHECK_EQ(w.get("/api/journeys/1").status, 401);
    w.post("/api/register", "username=alice&password=correct%20horse");
    w.post("/api/register", "username=bobby&password=another%20pass");
    const std::string alice = w.loginCookie("alice", "correct%20horse"), bobby = w.loginCookie("bobby", "another%20pass");
    CHECK(!alice.empty() && !bobby.empty());

    Response saved = w.post("/api/journeys", "from=1&to=5&mode=fastest&day=weekday&label=to%20work", alice);
    CHECK_EQ(saved.status, 201);
    CHECK(validJson(saved.body));
    CHECK(contains(saved.body, "\"updated\":false"));
    Response again = w.post("/api/journeys", "from=1&to=5&mode=fastest&day=weekday&label=renamed", alice);
    CHECK_EQ(again.status, 200);
    CHECK(contains(again.body, "\"updated\":true"));
    CHECK_EQ(w.post("/api/journeys", "from=1&to=9&mode=fewest&day=sunday", alice).status, 201);
    CHECK_EQ(w.post("/api/journeys", "from=1&to=5&mode=cheapest&day=weekday&label=cheap", alice).status, 201);   // 'cheapest' can be saved

    Response list = w.get("/api/journeys", alice);
    CHECK_EQ(list.status, 200);
    CHECK(validJson(list.body));
    CHECK(contains(list.body, "\"label\":\"renamed\""));
    CHECK(contains(list.body, "\"name\":\"Station A\""));
    CHECK(contains(list.body, "\"fareInr\":20"));
    CHECK(contains(list.body, "\"current\":true"));
    CHECK(contains(list.body, "\"mode\":\"cheapest\""));
    CHECK_EQ(w.get("/api/journeys", bobby).body, std::string("[]"));                            // each person sees only their own

    Response open = w.get("/api/journeys/1", alice);
    CHECK_EQ(open.status, 200);
    CHECK(validJson(open.body));
    CHECK(contains(open.body, "\"status\":\"current\""));
    CHECK(contains(open.body, "FASTEST"));
    CHECK(contains(open.body, "\"route\":{\"seconds\":560"));                    // the route as data too, for drawing the coloured bar
    CHECK(contains(open.body, "\"kind\":\"ride\""));
    CHECK_EQ(w.get("/api/journeys/1", bobby).status, 404);                                      // someone else's journey looks like "not found"
    CHECK_EQ(w.post("/api/journeys/1/favourite", "value=1", bobby).status, 404);
    CHECK_EQ(w.post("/api/journeys/1", "", bobby, "DELETE").status, 404);
    CHECK_EQ(w.get("/api/journeys/999", alice).status, 404);
    CHECK_EQ(w.get("/api/journeys/abc", alice).status, 404);
    CHECK_EQ(w.get("/api/journeys/0", alice).status, 404);
    CHECK_EQ(w.get("/api/journeys/-1", alice).status, 404);

    CHECK_EQ(w.post("/api/journeys/1/favourite", "value=1", alice).status, 200);
    CHECK(contains(w.get("/api/journeys", alice).body, "\"favourite\":true"));
    CHECK_EQ(w.post("/api/journeys/1/favourite", "value=2", alice).status, 400);
    CHECK_EQ(w.post("/api/journeys/1/favourite", "", alice).status, 400);

    for (const char* bad : {"from=1&to=5&mode=quickest&day=weekday", "from=1&to=5&mode=fastest&day=monday", "from=1&to=1&mode=fastest&day=weekday",
                            "from=x&to=5&mode=fastest&day=weekday", "from=1&to=99&mode=fastest&day=weekday", "mode=fastest&day=weekday", "from=1&to=11&mode=fastest&day=weekday"}) {
        Response e = w.post("/api/journeys", bad, alice);
        if (e.status != 400) std::cerr << "expected 400 for " << bad << " got " << e.status << "\n";
        CHECK_EQ(e.status, 400);
        CHECK(validJson(e.body));
    }
    CHECK_EQ(w.post("/api/journeys", "from=2&to=5&mode=fastest&day=weekday&label=" + std::string(61, 'x'), alice).status, 400);
    CHECK_EQ(w.post("/api/journeys", "from=2&to=5&mode=fastest&day=weekday&label=bad%0Aline", alice).status, 400);
    CHECK_EQ(w.post("/api/journeys", "from=2&to=5&mode=fastest&day=weekday&label=%FF%FE", alice).status, 400);   // not valid UTF-8

    // hostile text is stored as it is but can never become markup: the JSON escapes < > &
    Response xss = w.post("/api/journeys", "from=2&to=5&mode=fastest&day=weekday&label=%3Cimg%20src%3Dx%20onerror%3Dalert(1)%3E%26", alice);
    CHECK_EQ(xss.status, 201);
    Response xssList = w.get("/api/journeys", alice);
    CHECK(validJson(xssList.body));
    CHECK(!contains(xssList.body, "<img"));
    CHECK(contains(xssList.body, "\\u003cimg"));

    // The network data is re-imported (here: edited directly, then the version bumped). The server notices on its
    // next request, with no restart, and the saved journey is recomputed with an explanation.
    w.db.exec("UPDATE segments SET run_seconds = run_seconds + 60 WHERE line_id = 1 AND from_station = 2 AND to_station = 3;");
    w.db.exec("UPDATE meta SET value = '2' WHERE key = 'data_version';");
    CHECK(contains(w.get("/api/status").body, "\"dataVersion\":2"));
    Response re = w.get("/api/journeys/1", alice);
    CHECK(validJson(re.body));
    CHECK(contains(re.body, "\"status\":\"recomputed\""));
    CHECK(contains(re.body, "\"changed\":true"));
    CHECK(contains(re.body, "version 1 to 2"));
    CHECK(contains(re.body, "\"current\":true"));
    CHECK(contains(w.get("/api/journeys/1", alice).body, "\"status\":\"current\""));

    CHECK_EQ(w.post("/api/journeys/1", "", alice, "DELETE").status, 200);
    CHECK_EQ(w.get("/api/journeys/1", alice).status, 404);
}

// A few thousand random requests: whatever arrives, the handler must not crash, must answer with a sensible
// status, and every /api answer must be valid JSON.
static void testFuzz(const std::string& schema, const std::string& gtfs, const std::string& config) {
    World w(schema, gtfs, config);
    w.post("/api/register", "username=alice&password=correct%20horse");
    const std::string cookie = w.loginCookie("alice", "correct%20horse");
    std::mt19937 rng(12345);
    auto pick = [&](unsigned k) { return static_cast<size_t>(rng() % k); };
    const std::vector<std::string> paths = {"/", "/api", "/api/", "/api/status", "/api/route", "/api/search", "/api/journeys", "/api/journeys/1", "/api/journeys/1/favourite",
                                            "/api/login", "/api/register", "/api/me", "/app.js", "/../x", "/%00", "/api/journeys/99999999999999", "/api/stations", "/api/lines",
                                            "/index.html", "//", "/api//status", "/api/journeys//1"};
    const std::vector<std::string> methods = {"GET", "POST", "DELETE", "PUT", "HEAD", "OPTIONS", "PATCH", "TRACE", "get", ""};
    const std::vector<std::string> junk = {"", "a", "from=1&to=2", "from=%", "%00", "username=%FF&password=%FE", "\x01\x02\xff", "from=1&to=2&day=sunday",
                                           "label=%3Cscript%3E", "value=1", "x=y=z&&&", std::string(500, 'z'), "from[]=1&from[]=2", "from=1;to=2", "from=1&from=2&to=3"};
    int checked = 0;
    for (int i = 0; i < 3000; ++i) {
        std::string target = paths[pick(static_cast<unsigned>(paths.size()))];
        if (pick(2) == 0) target += "?" + junk[pick(static_cast<unsigned>(junk.size()))];
        std::map<std::string, std::string> h;
        if (pick(3) != 0) h["Host"] = "localhost:8080";
        if (pick(2) == 0) h["X-Requested-With"] = pick(4) == 0 ? "other" : "metro-web";
        if (pick(2) == 0) h["Content-Type"] = pick(3) == 0 ? "text/plain" : "application/x-www-form-urlencoded";
        if (pick(3) == 0) h["Origin"] = pick(2) == 0 ? "http://localhost:8080" : "http://evil";
        if (pick(2) == 0) h["Cookie"] = pick(2) == 0 ? cookie : "session=" + junk[pick(static_cast<unsigned>(junk.size()))];
        Request req = makeRequest(methods[pick(static_cast<unsigned>(methods.size()))], target, h, junk[pick(static_cast<unsigned>(junk.size()))]);
        Response r = w.api->handle(req);
        CHECK(r.status >= 200 && r.status <= 599);
        CHECK(!headerOf(r, "Content-Security-Policy").empty());
        if (req.path.rfind("/api", 0) == 0 && !(req.method == "HEAD")) {
            if (!validJson(r.body)) std::cerr << "invalid JSON for " << req.method << " " << target << ": " << r.body.substr(0, 80) << "\n";
            CHECK(validJson(r.body));
        }
        CHECK(r.status != 500);
        ++checked;
    }
    CHECK_EQ(checked, 3000);
    CHECK(w.logged.empty());                       // and nothing was reported as an internal error
}

static void testConcurrency(const std::string& schema, const std::string& gtfs, const std::string& config) {
    World w(schema, gtfs, config);
    w.post("/api/register", "username=alice&password=correct%20horse");
    std::vector<std::thread> threads;
    std::vector<int> failures(8, 0);
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 40; ++i) {
                Response r = w.get("/api/route?from=1&to=5");
                if (r.status != 200 || r.body.find("\"found\":true") == std::string::npos) ++failures[static_cast<size_t>(t)];
                Response l = w.post("/api/login", "username=ghost" + std::to_string(t) + "&password=wrong%20guess");
                if (l.status != 401 && l.status != 429) ++failures[static_cast<size_t>(t)];
                if (w.get("/api/stations").status != 200) ++failures[static_cast<size_t>(t)];
            }
        });
    for (auto& th : threads) th.join();
    for (int f : failures) CHECK_EQ(f, 0);
}

static int run(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_web <schema.sql> <import_gtfs> <import_config>\n";
        return 2;
    }
    const std::string schema = argv[1], gtfs = argv[2], config = argv[3];
    testJsonWriter();
    testHttpParsing();
    testStaticFiles(schema, gtfs, config);
    testReadOnlyApi(schema, gtfs, config);
    testProtection(schema, gtfs, config);
    testAccountsOverHttp(schema, gtfs, config);
    testJourneysOverHttp(schema, gtfs, config);
    testFuzz(schema, gtfs, config);
    testConcurrency(schema, gtfs, config);
    return finish("test_web");
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "test_web: unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
