#include "network.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>

#include "../common/csv.h"
#include "../common/db.h"
#include "../gtfs/inspect.h"

namespace metro {
namespace route {

namespace {

[[noreturn]] void bad(const std::string& message) { throw std::invalid_argument("invalid network: " + message); }

int toInt(const std::string& s, const char* what) {
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || !end || *end != '\0') throw std::runtime_error(std::string("database has a bad number in ") + what + ": '" + s + "'");
    return static_cast<int>(v);
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// Name key used for matching: lower-case, punctuation removed, and "sec" treated as "sector" because the
// feed spells the same word both ways (for example "Noida Sec-52" and "Noida Sector 51").
std::string matchKey(const std::string& name) {
    const std::string n = gtfs::normaliseName(name);
    std::string out, word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (!out.empty()) out += ' ';
        out += (word == "sec") ? std::string("sector") : word;
        word.clear();
    };
    for (char c : n) {
        if (c == ' ') flush();
        else word += c;
    }
    flush();
    return out;
}

}  // namespace

void validateNetwork(const Network& n) {
    if (n.stations.empty()) bad("no stations");
    if (n.services.empty()) bad("no services");
    for (size_t i = 0; i < n.stations.size(); ++i)
        if (n.stations[i].id != static_cast<int>(i) + 1) bad("station ids must be 1..N in order");
    for (size_t i = 0; i < n.services.size(); ++i)
        if (n.services[i].id != static_cast<int>(i) + 1) bad("service ids must be 1..K in order");
    if (n.dwellSeconds < 0) bad("negative dwell time");

    // position of each station within each service
    std::vector<std::vector<int>> pos(n.services.size(), std::vector<int>(n.stations.size() + 1, -1));
    for (const auto& s : n.services) {
        if (s.stations.size() < 2) bad("service '" + s.name + "' has fewer than 2 stations");
        for (size_t i = 0; i < s.stations.size(); ++i) {
            int st = s.stations[i];
            if (st < 1 || st > static_cast<int>(n.stations.size())) bad("service '" + s.name + "' names an unknown station");
            int& p = pos[static_cast<size_t>(s.id - 1)][static_cast<size_t>(st)];
            if (p != -1) bad("service '" + s.name + "' visits a station twice");
            p = static_cast<int>(i);
        }
    }
    for (const auto& g : n.segments) {
        if (g.service < 1 || g.service > static_cast<int>(n.services.size())) bad("segment names an unknown service");
        if (g.from < 1 || g.from > static_cast<int>(n.stations.size()) || g.to < 1 || g.to > static_cast<int>(n.stations.size()))
            bad("segment names an unknown station");
        const auto& p = pos[static_cast<size_t>(g.service - 1)];
        int a = p[static_cast<size_t>(g.from)], b = p[static_cast<size_t>(g.to)];
        if (a < 0 || b < 0) bad("segment uses a station that its service does not call at");
        if (std::abs(a - b) != 1) bad("segment joins stations that are not neighbours on its service");
        if (g.runSeconds <= 0) bad("segment with a non-positive run time");
    }
    for (const auto& f : n.fareSlabs) {
        if (f.op.empty() || f.minKm < 0 || f.maxKm <= f.minKm || f.fareInr < 0) bad("a fare slab has impossible values");
        if (f.dayType != "weekday" && f.dayType != "sunday_holiday") bad("a fare slab has an unknown day type");
    }
    for (const auto& f : n.farePairs)
        if (f.op.empty() || f.fareInr < 0 || f.stopA.empty() || f.stopB.empty() || !(f.stopA < f.stopB)) bad("a fare pair has impossible values");
    for (const auto& t : n.transfers) {
        if (t.fromService < 1 || t.fromService > static_cast<int>(n.services.size()) || t.toService < 1 ||
            t.toService > static_cast<int>(n.services.size()))
            bad("transfer names an unknown service");
        if (t.fromStation < 1 || t.fromStation > static_cast<int>(n.stations.size()) || t.toStation < 1 ||
            t.toStation > static_cast<int>(n.stations.size()))
            bad("transfer names an unknown station");
        if (pos[static_cast<size_t>(t.fromService - 1)][static_cast<size_t>(t.fromStation)] < 0 ||
            pos[static_cast<size_t>(t.toService - 1)][static_cast<size_t>(t.toStation)] < 0)
            bad("transfer uses a station that its service does not call at");
        if (t.walkSeconds < 0) bad("transfer with a negative walking time");
    }
}

Network loadNetwork(Database& db) {
    std::string version;
    try {
        version = db.scalarText("SELECT value FROM meta WHERE key='schema_version';");
    } catch (const std::exception&) {
        throw std::runtime_error("this is not a metro database. Create one with: metro_db init");
    }
    if (version != "5")
        throw std::runtime_error("database schema version is " + version + " but 5 is needed. Recreate it with: metro_db init --force, then metro_import");

    Network n;
    for (const auto& r : db.rows("SELECT station_id,gtfs_stop_id,name,lat,lon FROM stations ORDER BY station_id")) {
        Station s;
        s.id = toInt(r[0], "stations");
        s.gtfsId = r[1];
        s.name = r[2];
        s.lat = std::strtod(r[3].c_str(), nullptr);
        s.lon = std::strtod(r[4].c_str(), nullptr);
        n.stations.push_back(s);
    }
    if (n.stations.empty()) throw std::runtime_error("the database has no network yet. Run: metro_import --gtfs data\\raw");

    for (const auto& r : db.rows("SELECT line_id,family,name,ifnull(colour,''),operator FROM lines ORDER BY line_id")) {
        Service s;
        s.id = toInt(r[0], "lines");
        s.family = r[1];
        s.name = r[2];
        s.colour = r[3];
        s.op = r[4];
        n.services.push_back(s);
    }
    for (const auto& r : db.rows("SELECT line_id,station_id FROM line_stops ORDER BY line_id,seq")) {
        int lid = toInt(r[0], "line_stops");
        if (lid < 1 || lid > static_cast<int>(n.services.size())) throw std::runtime_error("line_stops names an unknown line");
        n.services[static_cast<size_t>(lid - 1)].stations.push_back(toInt(r[1], "line_stops"));
    }
    for (const auto& r : db.rows("SELECT line_id,from_station,to_station,run_seconds,ifnull(distance_m,0) FROM segments ORDER BY segment_id")) {
        Segment g;
        g.service = toInt(r[0], "segments");
        g.from = toInt(r[1], "segments");
        g.to = toInt(r[2], "segments");
        g.runSeconds = toInt(r[3], "segments");
        g.distanceM = toInt(r[4], "segments");
        n.segments.push_back(g);
    }
    for (const auto& r : db.rows("SELECT from_station,from_line,to_station,to_line,walk_seconds FROM transfers "
                                 "ORDER BY from_station,from_line,to_station,to_line")) {
        Transfer t;
        t.fromStation = toInt(r[0], "transfers");
        t.fromService = toInt(r[1], "transfers");
        t.toStation = toInt(r[2], "transfers");
        t.toService = toInt(r[3], "transfers");
        t.walkSeconds = toInt(r[4], "transfers");
        n.transfers.push_back(t);
    }
    for (const auto& r : db.rows("SELECT operator,day_type,ticket,min_km,max_km,fare_inr,valid_from,source FROM fare_slabs ORDER BY slab_id")) {
        FareSlab f;
        f.op = r[0];
        f.dayType = r[1];
        f.ticket = r[2];
        f.minKm = std::strtod(r[3].c_str(), nullptr);
        f.maxKm = std::strtod(r[4].c_str(), nullptr);
        f.fareInr = toInt(r[5], "fare_slabs");
        f.validFrom = r[6];
        f.source = r[7];
        n.fareSlabs.push_back(f);
    }
    for (const auto& r : db.rows("SELECT operator,day_type,ticket,stop_a,stop_b,fare_inr,valid_from,source FROM fare_pairs ORDER BY pair_id")) {
        FarePair f;
        f.op = r[0];
        f.dayType = r[1];
        f.ticket = r[2];
        f.stopA = r[3];
        f.stopB = r[4];
        f.fareInr = toInt(r[5], "fare_pairs");
        f.validFrom = r[6];
        f.source = r[7];
        n.farePairs.push_back(f);
    }
    try {
        n.dwellSeconds = toInt(db.scalarText("SELECT value FROM meta WHERE key='dwell_seconds';"), "meta");
        n.dataVersion = toInt(db.scalarText("SELECT value FROM meta WHERE key='data_version';"), "meta");
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception&) {
        throw std::runtime_error("the database is missing import information. Run: metro_import --gtfs data\\raw");
    }
    // planner settings are optional in the database; the defaults apply if a key is missing
    auto optionalInt = [&](const char* key, int& target) {
        try {
            target = toInt(db.scalarText(std::string("SELECT value FROM meta WHERE key='") + key + "';"), "meta");
        } catch (const std::runtime_error&) {
            throw;
        } catch (const std::exception&) {
        }
    };
    optionalInt("similar_time_seconds", n.plannerSimilarSeconds);
    optionalInt("max_routes_per_group", n.plannerMaxRoutes);
    optionalInt("max_slower_percent", n.plannerMaxSlowerPercent);
    if (n.plannerSimilarSeconds < 0 || n.plannerMaxRoutes < 1 || n.plannerMaxSlowerPercent < 0) throw std::runtime_error("the database has impossible journey planner settings");
    validateNetwork(n);
    return n;
}

Resolution resolveStation(const Network& n, const std::string& rawQuery) {
    Resolution res;
    const std::string query = metro::trim(rawQuery);

    if (query.rfind("id:", 0) == 0) {
        const std::string id = metro::trim(query.substr(3));
        for (const auto& s : n.stations)
            if (s.gtfsId == id) {
                res.status = Resolution::Status::Found;
                res.station = s.id;
                return res;
            }
        return res;   // NotFound, no suggestions
    }

    const std::string q = matchKey(query);
    if (q.empty()) return res;

    std::vector<std::string> norm;
    norm.reserve(n.stations.size());
    for (const auto& s : n.stations) norm.push_back(matchKey(s.name));

    std::vector<int> exact, contains;
    for (size_t i = 0; i < n.stations.size(); ++i) {
        if (norm[i] == q) exact.push_back(n.stations[i].id);
        else if (norm[i].find(q) != std::string::npos) contains.push_back(n.stations[i].id);
    }
    auto settle = [&](const std::vector<int>& found) {
        if (found.size() == 1) {
            res.status = Resolution::Status::Found;
            res.station = found[0];
        } else {
            res.status = Resolution::Status::Ambiguous;
            res.candidates = found;
        }
    };
    if (!exact.empty()) {
        settle(exact);
        return res;
    }
    if (!contains.empty()) {
        settle(contains);
        return res;
    }

    std::vector<std::pair<int, int>> scored;   // (distance, station id)
    for (size_t i = 0; i < n.stations.size(); ++i) scored.push_back({editDistance(q, norm[i]), n.stations[i].id});
    std::sort(scored.begin(), scored.end());
    for (size_t i = 0; i < scored.size() && i < 5; ++i) res.candidates.push_back(scored[i].second);
    return res;   // NotFound with suggestions
}

}  // namespace route
}  // namespace metro
