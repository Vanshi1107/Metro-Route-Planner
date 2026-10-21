#include "api.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "../auth/journeys.h"
#include "../common/text.h"
#include "../route/fare.h"
#include "../route/network.h"
#include "../route/planner.h"
#include "../route/present.h"
#include "../route/router.h"
#include "json.h"

namespace fs = std::filesystem;
using namespace metro::route;
using namespace metro::auth;

namespace metro {
namespace web {

namespace {

const char* kCsp =
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data: https://tile.openstreetmap.org https://*.tile.openstreetmap.org; "
    "connect-src 'self'; font-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'";

void addSecurityHeaders(Response& r, bool api) {
    r.headers.push_back({"X-Content-Type-Options", "nosniff"});
    r.headers.push_back({"X-Frame-Options", "DENY"});
    r.headers.push_back({"Referrer-Policy", "strict-origin-when-cross-origin"});   // OpenStreetMap's tile servers want a Referer
    r.headers.push_back({"Content-Security-Policy", kCsp});
    r.headers.push_back({"Permissions-Policy", "geolocation=(), microphone=(), camera=()"});
    r.headers.push_back({"Cross-Origin-Opener-Policy", "same-origin"});
    r.headers.push_back({"Cache-Control", api ? "no-store" : "no-cache"});
}

Response json(int status, const JsonWriter& w) {
    Response r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = w.str();
    return r;
}

Response errorJson(int status, const std::string& message) {
    JsonWriter w;
    w.beginObject().field("error", message).endObject();
    return json(status, w);
}

Response methodNotAllowed(const std::string& allow) {
    Response r = errorJson(405, "That is not allowed here.");
    r.headers.push_back({"Allow", allow});
    return r;
}

bool parseInt(const std::string& s, int lo, int hi, int& out) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    const long v = std::strtol(s.c_str(), nullptr, 10);
    if (v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string cookieValue(const std::string& header, const std::string& name) {
    size_t pos = 0;
    while (pos <= header.size()) {
        size_t semi = header.find(';', pos);
        if (semi == std::string::npos) semi = header.size();
        const std::string part = trimmed(header.substr(pos, semi - pos));
        const size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == name) return part.substr(eq + 1);
        if (semi == header.size()) break;
        pos = semi + 1;
    }
    return std::string();
}

int minutesOf(int seconds) { return (seconds + 30) / 60; }

const char* contentTypeFor(const std::string& ext) {
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return nullptr;   // anything else is not served
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// The loaded network and everything built on it
// ---------------------------------------------------------------------------------------------------

struct Api::World {
    Network net;
    Router router;
    FareCalculator fares;
    Planner planner;
    JourneyService journeys;
    std::unordered_map<std::string, int> byGtfs;
    std::string stationsJson, linesJson;

    World(Network n, Database& db, const std::function<long long()>& clock) : net(std::move(n)), router(net), fares(net), planner(net, router, fares), journeys(db, net, planner, fares, clock) {
        for (const auto& s : net.stations) byGtfs[s.gtfsId] = s.id;

        std::vector<std::vector<int>> linesAt(net.stations.size() + 1);
        for (const auto& sv : net.services)
            for (int st : sv.stations) linesAt[static_cast<size_t>(st)].push_back(sv.id);

        JsonWriter a;
        a.beginArray();
        for (const auto& s : net.stations) {
            a.beginObject().field("id", s.id).field("name", s.name).field("lat", s.lat).field("lon", s.lon);
            a.key("lines").beginArray();
            for (int l : linesAt[static_cast<size_t>(s.id)]) a.value(l);
            a.endArray().endObject();
        }
        a.endArray();
        stationsJson = a.str();

        JsonWriter b;
        b.beginArray();
        for (const auto& sv : net.services) {
            b.beginObject().field("id", sv.id).field("name", sv.name).field("family", sv.family).field("colour", sv.colour).field("operator", sv.op);
            b.key("stations").beginArray();
            for (int st : sv.stations) b.value(st);
            b.endArray().endObject();
        }
        b.endArray();
        linesJson = b.str();
    }

    void stationRef(JsonWriter& w, int id) const { w.beginObject().field("id", id).field("name", net.station(id).name).endObject(); }

    void serviceRef(JsonWriter& w, int id) const {
        const Service& s = net.service(id);
        w.beginObject().field("id", id).field("name", s.name).field("colour", s.colour).field("operator", s.op).endObject();
    }

    void fare(JsonWriter& w, const FareQuote& q) const {
        w.beginObject().field("available", q.available);
        if (q.available) {
            w.field("inr", q.fareInr).field("basis", q.basis).field("distanceM", q.distanceM).field("operator", q.op).field("ticket", q.ticket).field("verified", q.verified);
        } else {
            w.field("reason", q.reason);
        }
        w.endObject();
    }

    void route(JsonWriter& w, const Route& r, DayType day, const std::vector<Preference>* badges = nullptr) const {
        w.beginObject().field("seconds", r.totalSeconds).field("minutes", minutesOf(r.totalSeconds)).field("changes", r.changes).field("stops", r.stops);
        w.key("fare");
        fare(w, fares.quote(r, day));
        if (badges) {
            w.key("badges").beginArray();
            for (Preference p : *badges) w.value(preferenceName(p));
            w.endArray();
        }
        w.key("steps").beginArray();
        for (const auto& s : r.steps) {
            w.beginObject();
            if (s.kind == Step::Kind::Ride) {
                w.field("kind", "ride");
                w.key("service");
                serviceRef(w, s.service);
                w.key("from");
                stationRef(w, s.fromStation);
                w.key("to");
                stationRef(w, s.toStation);
                w.key("towards");
                stationRef(w, s.towardsStation);
                w.field("hops", s.hops).field("seconds", s.seconds).field("minutes", minutesOf(s.seconds));
                w.key("via").beginArray();
                for (int v : s.via) stationRef(w, v);
                w.endArray();
                w.key("path").beginArray();
                w.value(s.fromStation);
                for (int v : s.via) w.value(v);
                w.value(s.toStation);
                w.endArray();
            } else if (s.kind == Step::Kind::Change) {
                w.field("kind", "change");
                w.key("fromService");
                serviceRef(w, s.fromService);
                w.key("toService");
                serviceRef(w, s.service);
                w.key("at");
                stationRef(w, s.fromStation);
                w.key("to");
                stationRef(w, s.toStation);
                w.field("walkSeconds", s.seconds).field("walkMinutes", minutesOf(s.seconds));
            } else {
                w.field("kind", "walk");
                w.key("from");
                stationRef(w, s.fromStation);
                w.key("to");
                stationRef(w, s.toStation);
                w.field("walkSeconds", s.seconds).field("walkMinutes", minutesOf(s.seconds));
            }
            w.endObject();
        }
        w.endArray().endObject();
    }

    void gtfsRef(JsonWriter& w, const std::string& gtfsId) const {
        w.beginObject().field("gtfsId", gtfsId);
        auto it = byGtfs.find(gtfsId);
        w.key("name");
        if (it == byGtfs.end()) w.null();
        else w.value(net.station(it->second).name);
        w.endObject();
    }

    void journey(JsonWriter& w, const SavedJourney& j) const {
        w.beginObject().field("id", j.id);
        w.key("from");
        gtfsRef(w, j.fromGtfsId);
        w.key("to");
        gtfsRef(w, j.toGtfsId);
        w.field("mode", j.mode).field("day", j.dayType).field("label", j.label).field("minutes", minutesOf(j.totalSeconds)).field("changes", j.changes);
        w.key("fareInr");
        if (j.fareInr < 0) w.null();
        else w.value(j.fareInr);
        w.field("favourite", j.favourite).field("savedAt", j.updatedAt).field("dataVersion", j.dataVersion).field("current", j.dataVersion == net.dataVersion);
        w.endObject();
    }
};

// ---------------------------------------------------------------------------------------------------
// Construction, reloading
// ---------------------------------------------------------------------------------------------------

Api::Api(Database& db, ApiConfig config) : db_(db), cfg_(std::move(config)), accounts_(db, cfg_.account) {
    world_ = std::make_unique<World>(loadNetwork(db_), db_, cfg_.account.clock);
    loadedVersion_ = std::to_string(world_->net.dataVersion);
}

Api::~Api() = default;

int Api::dataVersion() {
    std::lock_guard<std::mutex> lock(mutex_);
    return world_->net.dataVersion;
}

void Api::reloadIfDataChanged() {
    const std::string now = db_.scalarText("SELECT value FROM meta WHERE key='data_version';");
    if (now == loadedVersion_) return;
    world_ = std::make_unique<World>(loadNetwork(db_), db_, cfg_.account.clock);   // metro_import has run since we started
    loadedVersion_ = now;
}

std::optional<Account> Api::currentAccount(const Request& req) {
    return accounts_.validateSession(cookieValue(req.header("cookie"), "session"));
}

// ---------------------------------------------------------------------------------------------------
// handle
// ---------------------------------------------------------------------------------------------------

Response Api::handle(const Request& req) {
    std::lock_guard<std::mutex> lock(mutex_);
    Response r;
    bool api = req.path.rfind("/api/", 0) == 0 || req.path == "/api";
    try {
        reloadIfDataChanged();
        r = dispatch(req);
    } catch (const std::exception& e) {
        if (cfg_.log) cfg_.log(std::string("internal error handling ") + req.method + " " + req.path + ": " + e.what());
        r = errorJson(500, "Something went wrong on the server.");   // nothing about the cause is sent to the browser
        r.headers.clear();
    }
    addSecurityHeaders(r, api);
    return r;
}

Response Api::dispatch(const Request& req) {
    if (req.badEncoding) return errorJson(400, "The request could not be understood.");
    if (req.rawQuery.size() > 2048 || req.path.size() > 512) return errorJson(414, "The request is too long.");
    if (req.body.size() > cfg_.maxBodyBytes) return errorJson(413, "The request is too large.");
    if (req.path == "/api" || req.path.rfind("/api/", 0) == 0) return handleApi(req);
    if (req.method != "GET" && req.method != "HEAD") {
        Response r;
        r.status = 405;
        r.body = "Method not allowed.";
        r.headers.push_back({"Allow", "GET, HEAD"});
        return r;
    }
    return serveStatic(req.path);
}

// ---------------------------------------------------------------------------------------------------
// static files
// ---------------------------------------------------------------------------------------------------

Response Api::serveStatic(const std::string& rawPath) {
    Response notFound;
    notFound.status = 404;
    notFound.body = "Not found.";

    // Exactly one spelling per file: no doubled or trailing slashes.
    if (rawPath.find("//") != std::string::npos || (rawPath.size() > 1 && rawPath.back() == '/')) return notFound;
    std::string path = rawPath == "/" ? "/index.html" : rawPath;
    const auto parts = segments(path);
    if (path.empty() || path[0] != '/' || parts.empty() || parts.size() > 6) return notFound;
    for (const auto& seg : parts) {
        if (seg.empty() || seg.size() > 64 || seg[0] == '.') return notFound;   // no "..", no hidden files
        for (unsigned char c : seg)
            if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-') || c >= 0x80) return notFound;
    }
    const std::string name = parts.back();
    const size_t dot = name.rfind('.');
    const char* type = dot == std::string::npos ? nullptr : contentTypeFor(name.substr(dot));
    if (!type) return notFound;

    std::error_code ec;
    fs::path file = fs::path(cfg_.webRoot);
    for (const auto& seg : parts) file /= seg;
    const fs::path root = fs::canonical(cfg_.webRoot, ec);
    const fs::path real = fs::canonical(file, ec);
    if (ec || root.empty() || !fs::is_regular_file(real, ec)) return notFound;
    // a symbolic link must not lead out of the web folder
    const std::string rs = real.generic_string(), ss = root.generic_string();
    if (rs.size() <= ss.size() || rs.compare(0, ss.size(), ss) != 0 || rs[ss.size()] != '/') return notFound;
    const auto size = fs::file_size(real, ec);
    if (ec || size > 2u * 1024 * 1024) return notFound;

    std::ifstream f(real, std::ios::binary);
    if (!f) return notFound;
    std::stringstream ss2;
    ss2 << f.rdbuf();
    Response r;
    r.contentType = type;
    r.body = ss2.str();
    return r;
}

// ---------------------------------------------------------------------------------------------------
// the API
// ---------------------------------------------------------------------------------------------------

Response Api::handleApi(const Request& req) {
    const auto seg = segments(req.path);   // seg[0] == "api"
    const bool isGet = req.method == "GET" || req.method == "HEAD";
    const bool isPost = req.method == "POST";
    const bool isDelete = req.method == "DELETE";
    if (!isGet && !isPost && !isDelete) return methodNotAllowed("GET, POST, DELETE");

    // CSRF defence for everything that changes something
    if (!isGet) {
        if (req.header("x-requested-with") != "metro-web") return errorJson(403, "This request was not allowed.");
        const std::string origin = req.header("origin");
        if (!origin.empty()) {
            const std::string host = req.header("host");
            if (host.empty() || (origin != "http://" + host && origin != "https://" + host)) return errorJson(403, "This request was not allowed.");
        }
        if (isPost && !req.body.empty() && req.header("content-type").rfind("application/x-www-form-urlencoded", 0) != 0)
            return errorJson(415, "Unsupported content type.");
    }

    const std::string& a = seg.size() > 1 ? seg[1] : std::string();
    World& w = *world_;

    // ---- read-only data ----------------------------------------------------------------------------
    if (a == "status" && seg.size() == 2) {
        if (!isGet) return methodNotAllowed("GET, HEAD");
        std::string validUntil = "unknown";
        try {
            validUntil = db_.scalarText("SELECT value FROM meta WHERE key='feed_valid_until';");
        } catch (const std::exception&) {
        }
        JsonWriter j;
        j.beginObject().field("dataVersion", w.net.dataVersion).field("algorithm", "bellman-ford").field("stations", static_cast<int>(w.net.stations.size()));
        j.field("services", static_cast<int>(w.net.services.size())).field("feedValidUntil", validUntil).field("changeWalkSeconds", w.net.transfers.empty() ? 0 : w.net.transfers.front().walkSeconds);
        j.endObject();
        return json(200, j);
    }
    if (a == "stations" && seg.size() == 2) {
        if (!isGet) return methodNotAllowed("GET, HEAD");
        Response r;
        r.contentType = "application/json; charset=utf-8";
        r.body = w.stationsJson;
        return r;
    }
    if (a == "lines" && seg.size() == 2) {
        if (!isGet) return methodNotAllowed("GET, HEAD");
        Response r;
        r.contentType = "application/json; charset=utf-8";
        r.body = w.linesJson;
        return r;
    }
    if (a == "search" && seg.size() == 2) {
        if (!isGet) return methodNotAllowed("GET, HEAD");
        const std::string q = req.queryParam("q");
        if (q.size() > 60 || !isValidUtf8(q)) return errorJson(400, "That search text is not valid.");
        // {"status": "found" | "ambiguous" | "suggestions" | "none", "stations": [...]}
        //   found: exactly one station matches   ambiguous: several match   suggestions: nothing matches, these are close
        JsonWriter j;
        std::string status = "none";
        std::vector<int> ids;
        if (!trimmed(q).empty()) {
            const Resolution res = resolveStation(w.net, q);
            if (res.status == Resolution::Status::Found) {
                status = "found";
                ids.push_back(res.station);
            } else {
                status = res.status == Resolution::Status::Ambiguous ? "ambiguous" : (res.candidates.empty() ? "none" : "suggestions");
                ids = res.candidates;
            }
            if (ids.size() > 8) ids.resize(8);
        }
        j.beginObject().field("status", status);
        j.key("stations").beginArray();
        for (int id : ids) w.stationRef(j, id);
        j.endArray().endObject();
        return json(200, j);
    }
    if (a == "route" && seg.size() == 2) {
        if (!isGet) return methodNotAllowed("GET, HEAD");
        int from = 0, to = 0;
        const int n = static_cast<int>(w.net.stations.size());
        if (!parseInt(req.queryParam("from"), 1, n, from) || !parseInt(req.queryParam("to"), 1, n, to)) return errorJson(400, "Choose two valid stations.");
        if (from == to) return errorJson(400, "Choose two different stations.");
        const std::string dayName = req.queryParam("day").empty() ? "weekday" : req.queryParam("day");
        if (dayName != "weekday" && dayName != "sunday") return errorJson(400, "The day must be weekday or sunday.");
        const DayType day = dayName == "weekday" ? DayType::Weekday : DayType::SundayHoliday;
        Preference prefer = Preference::Fastest;
        if (!req.queryParam("prefer").empty() && !parsePreference(req.queryParam("prefer"), prefer)) return errorJson(400, "Choose fastest, cheapest or fewest changes.");

        const PlanResult plan = w.planner.plan(from, to, day, prefer);
        JsonWriter j;
        j.beginObject().field("found", plan.found);
        j.key("from");
        w.stationRef(j, from);
        j.key("to");
        w.stationRef(j, to);
        j.field("day", dayName).field("dataVersion", w.net.dataVersion).field("prefer", preferenceName(prefer));
        if (!plan.found) {
            j.field("message", "There is no route between these two stations.").endObject();
            return json(200, j);
        }
        auto writeGroup = [&](const Group& g) {
            j.beginObject().field("criterion", preferenceName(g.criterion)).field("summary", g.summary).field("fellBack", g.fellBack);
            j.key("routes").beginArray();
            for (const auto& c : g.routes) w.route(j, c.route, day, &c.badges);
            j.endArray().endObject();
        };
        j.key("primary");
        writeGroup(plan.primary);
        j.key("others").beginArray();
        for (const auto& g : plan.others) writeGroup(g);
        j.endArray();
        j.key("notes").beginArray();
        for (const auto& note : plan.notes) j.value(note);
        j.endArray();
        j.key("notices").beginArray();
        j.value("Times are estimates from the timetable data and do not include waiting for trains.");
        j.value("Time to change trains is an estimate, not a measured value.");
        j.endArray().endObject();
        return json(200, j);
    }

    // ---- accounts --------------------------------------------------------------------------------------
    if (a == "me" && seg.size() == 2) {
        if (!isGet) return methodNotAllowed("GET, HEAD");
        const auto acct = currentAccount(req);
        JsonWriter j;
        j.beginObject().field("loggedIn", acct.has_value());
        if (acct) j.field("username", acct->username);
        j.endObject();
        return json(200, j);
    }
    if (a == "register" && seg.size() == 2) {
        if (!isPost) return methodNotAllowed("POST");
        const std::string user = req.formParam("username"), pass = req.formParam("password");
        const RegisterResult r = accounts_.registerUser(user, pass);
        JsonWriter j;
        if (r.status == Status::Ok) {
            j.beginObject().field("ok", true).field("username", user).endObject();
            return json(201, j);
        }
        return errorJson(r.status == Status::UsernameTaken ? 409 : 400, r.message);
    }
    if (a == "login" && seg.size() == 2) {
        if (!isPost) return methodNotAllowed("POST");
        const LoginResult r = accounts_.login(req.formParam("username"), req.formParam("password"));
        if (r.status == Status::LockedOut) {
            Response e = errorJson(429, r.message);
            e.headers.push_back({"Retry-After", std::to_string(r.retryAfterSeconds)});
            return e;
        }
        if (r.status != Status::Ok) return errorJson(401, r.message);
        const auto acct = accounts_.validateSession(r.token);
        JsonWriter j;
        j.beginObject().field("ok", true).field("username", acct ? acct->username : std::string()).endObject();
        Response resp = json(200, j);
        std::string cookie = "session=" + r.token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(cfg_.account.sessionSeconds);
        if (cfg_.secureCookies) cookie += "; Secure";
        resp.headers.push_back({"Set-Cookie", cookie});
        return resp;
    }
    if (a == "logout" && seg.size() == 2) {
        if (!isPost) return methodNotAllowed("POST");
        accounts_.logout(cookieValue(req.header("cookie"), "session"));
        JsonWriter j;
        j.beginObject().field("ok", true).endObject();
        Response resp = json(200, j);
        std::string cookie = "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
        if (cfg_.secureCookies) cookie += "; Secure";
        resp.headers.push_back({"Set-Cookie", cookie});
        return resp;
    }

    // ---- saved journeys (log in first) -----------------------------------------------------------------
    if (a == "journeys") {
        const auto acct = currentAccount(req);
        if (!acct) return errorJson(401, "Please log in first.");
        const int uid = acct->id;

        if (seg.size() == 2) {
            if (isGet) {
                JsonWriter j;
                j.beginArray();
                for (const auto& sj : w.journeys.list(uid)) w.journey(j, sj);
                j.endArray();
                return json(200, j);
            }
            if (!isPost) return methodNotAllowed("GET, HEAD, POST");
            int from = 0, to = 0;
            const int n = static_cast<int>(w.net.stations.size());
            if (!parseInt(req.formParam("from"), 1, n, from) || !parseInt(req.formParam("to"), 1, n, to)) return errorJson(400, "Choose two valid stations.");
            const std::string modeName = req.formParam("mode"), dayName = req.formParam("day");
            Preference savePrefer = Preference::Fastest;
            if (!parsePreference(modeName, savePrefer) || (dayName != "weekday" && dayName != "sunday")) return errorJson(400, "Choose an option and a day.");
            const SaveResult sr = w.journeys.save(uid, from, to, savePrefer,
                                                  dayName == "weekday" ? DayType::Weekday : DayType::SundayHoliday, req.formParam("label"));
            if (!sr.ok) return errorJson(400, sr.message);
            JsonWriter j;
            j.beginObject().field("id", sr.id).field("updated", sr.updatedExisting).endObject();
            return json(sr.updatedExisting ? 200 : 201, j);
        }

        int id = 0;
        if (!parseInt(seg[2], 1, 2000000000, id)) return errorJson(404, "No such saved journey.");
        if (seg.size() == 3) {
            if (isGet) {
                const auto re = w.journeys.reopen(uid, id);
                if (!re) return errorJson(404, "No such saved journey.");
                static const char* names[] = {"current", "recomputed", "stationMissing", "noRoute"};
                JsonWriter j;
                j.beginObject().field("status", names[static_cast<int>(re->status)]).field("changed", re->changed).field("note", re->note).field("text", re->text);
                j.key("journey");
                w.journey(j, re->journey);
                j.endObject();
                return json(200, j);
            }
            if (isDelete) {
                if (!w.journeys.remove(uid, id)) return errorJson(404, "No such saved journey.");
                JsonWriter j;
                j.beginObject().field("ok", true).endObject();
                return json(200, j);
            }
            return methodNotAllowed("GET, HEAD, DELETE");
        }
        if (seg.size() == 4 && seg[3] == "favourite") {
            if (!isPost) return methodNotAllowed("POST");
            const std::string v = req.formParam("value");
            if (v != "1" && v != "0") return errorJson(400, "Say whether it is a favourite (1 or 0).");
            if (!w.journeys.setFavourite(uid, id, v == "1")) return errorJson(404, "No such saved journey.");
            JsonWriter j;
            j.beginObject().field("ok", true).endObject();
            return json(200, j);
        }
    }
    return errorJson(404, "No such page.");
}

}  // namespace web
}  // namespace metro
