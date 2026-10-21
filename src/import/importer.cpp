#include "importer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "../common/csv.h"
#include "../common/db.h"
#include "../gtfs/inspect.h"

namespace fs = std::filesystem;

namespace metro {
namespace import {

namespace {

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

[[noreturn]] void fail(const std::string& message) { throw ImportError(message); }

std::string pathIn(const std::string& dir, const char* file) { return (fs::path(dir) / file).string(); }

std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseLong(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 10);
    return end && *end == '\0';
}

bool parseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

// Opens a CSV and checks that it has the columns we need.
std::unique_ptr<CsvReader> openCsv(const std::string& path, const std::vector<std::string>& required) {
    auto r = std::make_unique<CsvReader>(path);
    if (!r->ok()) fail("cannot read " + path + " (missing, empty, or not a CSV file)");
    for (const auto& c : required)
        if (r->column(c) < 0) fail(path + " is missing the column '" + c + "'");
    return r;
}

std::string where(const std::string& file, const CsvReader& r) {
    return file + " row " + std::to_string(r.rowNumber());
}

// Median, rounded half up to a whole number (so 140.5 -> 141 on every platform).
template <typename T>
int medianRounded(std::vector<T> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    double med = n % 2 ? static_cast<double>(v[n / 2])
                       : (static_cast<double>(v[n / 2 - 1]) + static_cast<double>(v[n / 2])) / 2.0;
    return static_cast<int>(std::floor(med + 0.5));
}

// Today as YYYYMMDD (UTC), without using any platform-specific date functions.
std::string todayYyyymmdd() {
    using namespace std::chrono;
    long long days = duration_cast<hours>(system_clock::now().time_since_epoch()).count() / 24;
    days += 719468;   // civil-from-days algorithm (H. Hinnant)
    long long era = (days >= 0 ? days : days - 146096) / 146097;
    long long doe = days - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long mth = mp < 10 ? mp + 3 : mp - 9;
    if (mth <= 2) ++y;
    std::ostringstream o;
    o << std::setfill('0') << std::setw(4) << y << std::setw(2) << mth << std::setw(2) << d;
    return o.str();
}

// ---------------------------------------------------------------------------------------
// Config files
// ---------------------------------------------------------------------------------------

struct FamilyConfig {
    std::string prefix, display, colour, op;
    bool join = false;   // join two services of this family that meet end to end
};

std::vector<FamilyConfig> loadFamilies(const std::string& configDir) {
    const std::string path = pathIn(configDir, "line_map.csv");
    auto r = openCsv(path, {"prefix", "display_name", "colour", "operator", "join_end_to_end"});
    int cP = r->column("prefix"), cD = r->column("display_name"), cC = r->column("colour"),
        cO = r->column("operator"), cJ = r->column("join_end_to_end");
    std::vector<FamilyConfig> out;
    std::set<std::string> seen;
    std::vector<std::string> row;
    while (r->next(row)) {
        FamilyConfig f;
        f.prefix = CsvReader::get(row, cP);
        f.display = CsvReader::get(row, cD);
        f.colour = CsvReader::get(row, cC);
        f.op = CsvReader::get(row, cO);
        std::string j = lowerCase(CsvReader::get(row, cJ));
        if (f.prefix.empty() || f.display.empty() || f.op.empty())
            fail(where(path, *r) + ": prefix, display_name and operator must not be empty");
        if (j != "yes" && j != "no") fail(where(path, *r) + ": join_end_to_end must be 'yes' or 'no'");
        f.join = (j == "yes");
        if (!seen.insert(f.prefix).second) fail(where(path, *r) + ": duplicate prefix '" + f.prefix + "'");
        out.push_back(f);
    }
    if (out.empty()) fail(path + " has no rows");
    return out;
}

int loadInterchangeWalk(const std::string& configDir) {
    const std::string path = pathIn(configDir, "settings.csv");
    auto r = openCsv(path, {"key", "value"});
    int cK = r->column("key"), cV = r->column("value");
    std::vector<std::string> row;
    while (r->next(row)) {
        if (CsvReader::get(row, cK) != "interchange_walk_seconds") continue;
        long v;
        if (!parseLong(CsvReader::get(row, cV), v) || v < 0 || v > 3600)
            fail(where(path, *r) + ": interchange_walk_seconds must be a whole number from 0 to 3600");
        return static_cast<int>(v);
    }
    fail(path + " has no 'interchange_walk_seconds' row");
}

// The journey planner's settings are optional in settings.csv; each key falls back to its default.
void loadPlannerSettings(const std::string& configDir, int& similar, int& maxRoutes, int& slower) {
    const std::string path = pathIn(configDir, "settings.csv");
    auto r = openCsv(path, {"key", "value"});
    int cK = r->column("key"), cV = r->column("value");
    std::vector<std::string> row;
    while (r->next(row)) {
        const std::string key = CsvReader::get(row, cK);
        long v;
        if (key != "similar_time_seconds" && key != "max_routes_per_group" && key != "max_slower_percent") continue;
        if (!parseLong(CsvReader::get(row, cV), v)) fail(where(path, *r) + ": " + key + " must be a whole number");
        if (key == "similar_time_seconds") {
            if (v < 0 || v > 1800) fail(where(path, *r) + ": similar_time_seconds must be from 0 to 1800");
            similar = static_cast<int>(v);
        } else if (key == "max_routes_per_group") {
            if (v < 1 || v > 10) fail(where(path, *r) + ": max_routes_per_group must be from 1 to 10");
            maxRoutes = static_cast<int>(v);
        } else {
            if (v < 0 || v > 300) fail(where(path, *r) + ": max_slower_percent must be from 0 to 300");
            slower = static_cast<int>(v);
        }
    }
}

struct SeedRow {
    std::string a, b;
    int walk = 0;
    bool confirmed = false;
};

std::vector<SeedRow> loadSeeds(const std::string& configDir) {
    const std::string path = pathIn(configDir, "transfers_seed.csv");
    std::vector<SeedRow> out;
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;   // the file is optional
    auto r = openCsv(path, {"stop_id_a", "stop_id_b", "walk_seconds", "confirmed"});
    int cA = r->column("stop_id_a"), cB = r->column("stop_id_b"), cW = r->column("walk_seconds"),
        cC = r->column("confirmed");
    std::vector<std::string> row;
    while (r->next(row)) {
        SeedRow s;
        s.a = CsvReader::get(row, cA);
        s.b = CsvReader::get(row, cB);
        long w;
        if (!parseLong(CsvReader::get(row, cW), w) || w < 0 || w > 7200)
            fail(where(path, *r) + ": walk_seconds must be a whole number from 0 to 7200");
        s.walk = static_cast<int>(w);
        std::string c = lowerCase(CsvReader::get(row, cC));
        if (c != "yes" && c != "no") fail(where(path, *r) + ": confirmed must be 'yes' or 'no'");
        s.confirmed = (c == "yes");
        if (s.a.empty() || s.b.empty() || s.a == s.b) fail(where(path, *r) + ": needs two different stop ids");
        out.push_back(s);
    }
    return out;
}


// ---------------------------------------------------------------------------------------
// Fare slabs and distance corrections (both optional config files)
// ---------------------------------------------------------------------------------------

std::vector<FareSlabRow> loadFareSlabs(const std::string& configDir, std::vector<std::string>& warnings) {
    const std::string path = pathIn(configDir, "fare_slabs.csv");
    std::vector<FareSlabRow> out;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        warnings.push_back("config/fare_slabs.csv was not found, so no fares can be computed.");
        return out;
    }
    auto r = openCsv(path, {"operator", "day_type", "ticket", "min_km", "max_km", "fare_inr", "valid_from", "source"});
    int cO = r->column("operator"), cD = r->column("day_type"), cT = r->column("ticket"), cMin = r->column("min_km"),
        cMax = r->column("max_km"), cF = r->column("fare_inr"), cV = r->column("valid_from"), cS = r->column("source");
    std::vector<std::string> row;
    while (r->next(row)) {
        FareSlabRow f;
        f.op = CsvReader::get(row, cO);
        f.dayType = CsvReader::get(row, cD);
        f.ticket = CsvReader::get(row, cT);
        f.validFrom = CsvReader::get(row, cV);
        f.source = CsvReader::get(row, cS);
        long fare;
        if (f.op.empty()) fail(where(path, *r) + ": operator must not be empty");
        if (f.dayType != "weekday" && f.dayType != "sunday_holiday") fail(where(path, *r) + ": day_type must be 'weekday' or 'sunday_holiday'");
        if (f.ticket != "token" && f.ticket != "smartcard") fail(where(path, *r) + ": ticket must be 'token' or 'smartcard'");
        if (!parseDouble(CsvReader::get(row, cMin), f.minKm) || !parseDouble(CsvReader::get(row, cMax), f.maxKm) || f.minKm < 0 || f.maxKm <= f.minKm)
            fail(where(path, *r) + ": min_km and max_km must be numbers with 0 <= min_km < max_km");
        if (!parseLong(CsvReader::get(row, cF), fare) || fare < 0 || fare > 100000) fail(where(path, *r) + ": fare_inr must be a whole number of rupees");
        f.fareInr = static_cast<int>(fare);
        if (f.validFrom.empty()) fail(where(path, *r) + ": valid_from must not be empty");
        if (f.source.empty()) fail(where(path, *r) + ": source must not be empty (say where the number came from)");
        out.push_back(f);
    }
    // Slabs of each (operator, day, ticket) must start at 0, follow each other with no gap or overlap, and share one valid_from.
    std::map<std::array<std::string, 3>, std::vector<const FareSlabRow*>> groups;
    for (const auto& f : out) groups[{f.op, f.dayType, f.ticket}].push_back(&f);
    for (auto& g : groups) {
        auto& v = g.second;
        std::sort(v.begin(), v.end(), [](const FareSlabRow* a, const FareSlabRow* b) { return a->minKm < b->minKm; });
        const std::string label = g.first[0] + "/" + g.first[1] + "/" + g.first[2];
        if (v.front()->minKm != 0) fail("config/fare_slabs.csv: the slabs for " + label + " must start at 0 km");
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i]->minKm != v[i - 1]->maxKm) fail("config/fare_slabs.csv: the slabs for " + label + " have a gap or an overlap near " + std::to_string(v[i]->minKm) + " km");
        for (const auto* f : v)
            if (f->validFrom != v.front()->validFrom) fail("config/fare_slabs.csv: " + label + " has several valid_from dates; only one set is supported yet");
    }
    return out;
}


struct StationOverride {
    std::string stop, source;
    double lat = 0, lon = 0;
};

std::vector<StationOverride> loadStationOverrides(const std::string& configDir) {
    const std::string path = pathIn(configDir, "station_overrides.csv");
    std::vector<StationOverride> out;
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;   // optional
    auto r = openCsv(path, {"stop_id", "lat", "lon", "source"});
    int cS = r->column("stop_id"), cLa = r->column("lat"), cLo = r->column("lon"), cSrc = r->column("source");
    std::vector<std::string> row;
    while (r->next(row)) {
        StationOverride o;
        o.stop = CsvReader::get(row, cS);
        o.source = CsvReader::get(row, cSrc);
        if (o.stop.empty()) fail(where(path, *r) + ": stop_id must not be empty");
        if (!parseDouble(CsvReader::get(row, cLa), o.lat) || !parseDouble(CsvReader::get(row, cLo), o.lon) || o.lat < -90 || o.lat > 90 || o.lon < -180 || o.lon > 180)
            fail(where(path, *r) + ": lat and lon must be valid coordinates");
        if (o.source.empty()) fail(where(path, *r) + ": source must not be empty (say where the coordinates came from)");
        out.push_back(o);
    }
    return out;
}

std::vector<FarePairRow> loadFarePairs(const std::string& configDir, const std::unordered_map<std::string, int>& stationOf) {
    const std::string path = pathIn(configDir, "fare_pairs.csv");
    std::vector<FarePairRow> out;
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;   // optional
    auto r = openCsv(path, {"operator", "day_type", "ticket", "stop_id_a", "stop_id_b", "fare_inr", "valid_from", "source"});
    int cO = r->column("operator"), cD = r->column("day_type"), cT = r->column("ticket"), cA = r->column("stop_id_a"), cB = r->column("stop_id_b"),
        cF = r->column("fare_inr"), cV = r->column("valid_from"), cS = r->column("source");
    std::set<std::array<std::string, 5>> seen;
    std::vector<std::string> row;
    while (r->next(row)) {
        FarePairRow f;
        f.op = CsvReader::get(row, cO);
        f.dayType = CsvReader::get(row, cD);
        f.ticket = CsvReader::get(row, cT);
        f.stopA = CsvReader::get(row, cA);
        f.stopB = CsvReader::get(row, cB);
        f.validFrom = CsvReader::get(row, cV);
        f.source = CsvReader::get(row, cS);
        long fare;
        if (f.op.empty()) fail(where(path, *r) + ": operator must not be empty");
        if (f.dayType != "any" && f.dayType != "weekday" && f.dayType != "sunday_holiday") fail(where(path, *r) + ": day_type must be 'any', 'weekday' or 'sunday_holiday'");
        if (f.ticket != "token" && f.ticket != "smartcard") fail(where(path, *r) + ": ticket must be 'token' or 'smartcard'");
        if (!stationOf.count(f.stopA) || !stationOf.count(f.stopB)) fail(where(path, *r) + ": names a stop that is not in the feed ('" + (stationOf.count(f.stopA) ? f.stopB : f.stopA) + "')");
        if (f.stopA == f.stopB) fail(where(path, *r) + ": needs two different stops");
        if (!parseLong(CsvReader::get(row, cF), fare) || fare < 0 || fare > 100000) fail(where(path, *r) + ": fare_inr must be a whole number of rupees");
        f.fareInr = static_cast<int>(fare);
        if (f.validFrom.empty()) fail(where(path, *r) + ": valid_from must not be empty");
        if (f.source.empty()) fail(where(path, *r) + ": source must not be empty (say where the fare came from)");
        if (f.stopB < f.stopA) std::swap(f.stopA, f.stopB);   // store each pair once, in a fixed order
        if (!seen.insert({f.op, f.dayType, f.ticket, f.stopA, f.stopB}).second)
            fail(where(path, *r) + ": the pair '" + f.stopA + "' and '" + f.stopB + "' is listed twice");
        out.push_back(f);
    }
    return out;
}

struct DistanceOverride {
    std::string a, b, source;
    int metres = 0;
};

std::vector<DistanceOverride> loadDistanceOverrides(const std::string& configDir) {
    const std::string path = pathIn(configDir, "distance_overrides.csv");
    std::vector<DistanceOverride> out;
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;   // optional
    auto r = openCsv(path, {"stop_id_a", "stop_id_b", "distance_m", "source"});
    int cA = r->column("stop_id_a"), cB = r->column("stop_id_b"), cD = r->column("distance_m"), cS = r->column("source");
    std::vector<std::string> row;
    while (r->next(row)) {
        DistanceOverride o;
        o.a = CsvReader::get(row, cA);
        o.b = CsvReader::get(row, cB);
        o.source = CsvReader::get(row, cS);
        long d;
        if (o.a.empty() || o.b.empty() || o.a == o.b) fail(where(path, *r) + ": needs two different stop ids");
        if (!parseLong(CsvReader::get(row, cD), d) || d < 1 || d > 200000) fail(where(path, *r) + ": distance_m must be a whole number of metres from 1 to 200000");
        if (o.source.empty()) fail(where(path, *r) + ": source must not be empty (say where the distance came from)");
        o.metres = static_cast<int>(d);
        out.push_back(o);
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Sequence helpers
// ---------------------------------------------------------------------------------------

using Seq = std::vector<int>;

// True if A appears as a contiguous run inside B, read either forwards or backwards.
bool containsRun(const Seq& b, const Seq& a) {
    if (a.empty() || a.size() > b.size()) return false;
    const Seq rb(b.rbegin(), b.rend());
    for (const Seq* x : {&b, &rb})
        for (size_t i = 0; i + a.size() <= x->size(); ++i)
            if (std::equal(a.begin(), a.end(), x->begin() + static_cast<std::ptrdiff_t>(i))) return true;
    return false;
}

Seq reversed(const Seq& s) { return Seq(s.rbegin(), s.rend()); }

struct Candidate {
    Seq seq;
    int first = 0;           // order of first appearance (routes.txt order, then trip order)
    std::string routeId;
};

struct ServiceDraft {
    Seq seq;
    int first = 0;
    int family = 0;
};

// Union-find for connected pieces
struct DisjointSets {
    std::vector<int> parent;
    explicit DisjointSets(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    }
    void unite(int a, int b) { parent[static_cast<size_t>(find(a))] = find(b); }
};

std::string joinStrings(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

}  // namespace

// =======================================================================================
// buildModel
// =======================================================================================

Model buildModel(const std::string& gtfsDir, const std::string& configDir) {
    Model m;

    // ---- config ---------------------------------------------------------------------
    const std::vector<FamilyConfig> families = loadFamilies(configDir);
    std::unordered_map<std::string, int> familyIndex;
    for (size_t i = 0; i < families.size(); ++i) familyIndex[families[i].prefix] = static_cast<int>(i);
    m.interchangeWalkSeconds = loadInterchangeWalk(configDir);
    loadPlannerSettings(configDir, m.similarTimeSeconds, m.maxRoutesPerGroup, m.maxSlowerPercent);
    const std::vector<SeedRow> seeds = loadSeeds(configDir);
    m.fareSlabs = loadFareSlabs(configDir, m.warnings);
    const std::vector<DistanceOverride> overrides = loadDistanceOverrides(configDir);

    // ---- stops ----------------------------------------------------------------------
    std::unordered_map<std::string, int> stationOf;   // gtfs stop_id -> station id
    {
        const std::string path = pathIn(gtfsDir, "stops.txt");
        auto r = openCsv(path, {"stop_id", "stop_name", "stop_lat", "stop_lon"});
        int cI = r->column("stop_id"), cN = r->column("stop_name"), cLa = r->column("stop_lat"),
            cLo = r->column("stop_lon");
        std::vector<std::string> row;
        while (r->next(row)) {
            Station s;
            s.gtfsId = CsvReader::get(row, cI);
            s.name = CsvReader::get(row, cN);
            if (s.gtfsId.empty()) fail(where(path, *r) + ": empty stop_id");
            if (s.name.empty()) fail(where(path, *r) + ": empty stop_name");
            if (!parseDouble(CsvReader::get(row, cLa), s.lat) || !parseDouble(CsvReader::get(row, cLo), s.lon) ||
                s.lat < -90 || s.lat > 90 || s.lon < -180 || s.lon > 180)
                fail(where(path, *r) + ": missing or invalid coordinates for stop '" + s.gtfsId + "'");
            if (stationOf.count(s.gtfsId)) fail(where(path, *r) + ": duplicate stop_id '" + s.gtfsId + "'");
            s.id = static_cast<int>(m.stations.size()) + 1;
            stationOf[s.gtfsId] = s.id;
            m.stations.push_back(s);
        }
        if (m.stations.empty()) fail(path + " has no stops");
    }
    auto stationName = [&](int id) -> const std::string& { return m.stations[static_cast<size_t>(id - 1)].name; };

    // ---- coordinate corrections (config/station_overrides.csv) ----------------------------
    for (const auto& o : loadStationOverrides(configDir)) {
        auto it = stationOf.find(o.stop);
        if (it == stationOf.end()) fail("config/station_overrides.csv names a stop that is not in the feed: '" + o.stop + "'");
        Station& st = m.stations[static_cast<size_t>(it->second - 1)];
        m.notes.push_back("Coordinate correction: " + st.name + " moved to " + std::to_string(o.lat) + ", " + std::to_string(o.lon) + ". Source: " + o.source);
        st.lat = o.lat;
        st.lon = o.lon;
        ++m.stationOverridesApplied;
    }
    m.farePairs = loadFarePairs(configDir, stationOf);

    // ---- routes ---------------------------------------------------------------------
    struct RouteInfo {
        std::string id;
        int family = -1;
    };
    std::vector<RouteInfo> routes;
    std::unordered_map<std::string, int> routeIdx;
    {
        const std::string path = pathIn(gtfsDir, "routes.txt");
        auto r = openCsv(path, {"route_id", "route_long_name"});
        int cI = r->column("route_id"), cL = r->column("route_long_name");
        std::vector<std::string> row;
        while (r->next(row)) {
            RouteInfo ri;
            ri.id = CsvReader::get(row, cI);
            std::string longName = CsvReader::get(row, cL);
            if (ri.id.empty()) fail(where(path, *r) + ": empty route_id");
            if (routeIdx.count(ri.id)) fail(where(path, *r) + ": duplicate route_id '" + ri.id + "'");
            std::string prefix = longName.substr(0, longName.find('_'));
            auto it = familyIndex.find(prefix);
            if (it == familyIndex.end())
                fail(where(path, *r) + ": route '" + ri.id + "' (" + longName + ") has line prefix '" + prefix +
                     "' which is not listed in config/line_map.csv. Add it there, or fix the feed.");
            ri.family = it->second;
            routeIdx[ri.id] = static_cast<int>(routes.size());
            routes.push_back(ri);
        }
        if (routes.empty()) fail(path + " has no routes");
    }

    // ---- trips ----------------------------------------------------------------------
    std::vector<int> tripRoute;                       // trip index -> route index
    std::unordered_map<std::string, int> tripIdx;
    {
        const std::string path = pathIn(gtfsDir, "trips.txt");
        auto r = openCsv(path, {"trip_id", "route_id"});
        int cT = r->column("trip_id"), cR = r->column("route_id");
        std::vector<std::string> row;
        while (r->next(row)) {
            std::string tid = CsvReader::get(row, cT), rid = CsvReader::get(row, cR);
            if (tid.empty()) fail(where(path, *r) + ": empty trip_id");
            auto ri = routeIdx.find(rid);
            if (ri == routeIdx.end()) fail(where(path, *r) + ": trip '" + tid + "' names unknown route '" + rid + "'");
            if (tripIdx.count(tid)) fail(where(path, *r) + ": duplicate trip_id '" + tid + "'");
            tripIdx[tid] = static_cast<int>(tripRoute.size());
            tripRoute.push_back(ri->second);
        }
        if (tripRoute.empty()) fail(path + " has no trips");
    }

    // ---- stop_times -----------------------------------------------------------------
    struct TimeRow {
        int trip, station, seq, arr, dep;
        double dist;
    };
    std::vector<TimeRow> rows;
    {
        const std::string path = pathIn(gtfsDir, "stop_times.txt");
        auto r = openCsv(path, {"trip_id", "stop_id", "stop_sequence", "arrival_time", "departure_time"});
        int cT = r->column("trip_id"), cS = r->column("stop_id"), cQ = r->column("stop_sequence"),
            cA = r->column("arrival_time"), cD = r->column("departure_time"), cH = r->column("shape_dist_traveled");
        std::vector<std::string> row;
        while (r->next(row)) {
            TimeRow tr;
            auto ti = tripIdx.find(CsvReader::get(row, cT));
            if (ti == tripIdx.end()) fail(where(path, *r) + ": unknown trip '" + CsvReader::get(row, cT) + "'");
            auto si = stationOf.find(CsvReader::get(row, cS));
            if (si == stationOf.end()) fail(where(path, *r) + ": unknown stop '" + CsvReader::get(row, cS) + "'");
            long seq;
            if (!parseLong(CsvReader::get(row, cQ), seq)) fail(where(path, *r) + ": invalid stop_sequence");
            tr.trip = ti->second;
            tr.station = si->second;
            tr.seq = static_cast<int>(seq);
            tr.arr = gtfs::parseGtfsTime(CsvReader::get(row, cA));
            tr.dep = gtfs::parseGtfsTime(CsvReader::get(row, cD));
            if (tr.arr < 0 && tr.dep >= 0) tr.arr = tr.dep;
            if (tr.dep < 0 && tr.arr >= 0) tr.dep = tr.arr;
            if (tr.arr < 0) fail(where(path, *r) + ": no usable arrival or departure time");
            double d;
            tr.dist = parseDouble(CsvReader::get(row, cH), d) ? d : std::nan("");
            rows.push_back(tr);
        }
        if (rows.empty()) fail(path + " has no rows");
    }
    std::stable_sort(rows.begin(), rows.end(), [](const TimeRow& a, const TimeRow& b) {
        return a.trip != b.trip ? a.trip < b.trip : a.seq < b.seq;
    });

    // ---- per-route patterns and per-(family, pair) statistics ---------------------------
    std::vector<std::vector<Seq>> routePatterns(routes.size());
    std::map<std::array<int, 3>, std::vector<int>> runStats;       // (family, a, b) -> run seconds
    std::map<std::array<int, 3>, std::vector<double>> distStats;   // (family, a, b) -> metres
    std::vector<int> dwells;
    long long skippedTrips = 0;
    {
        // rows are sorted by trip index, which follows trips.txt order
        size_t i = 0;
        while (i < rows.size()) {
            size_t j = i;
            while (j < rows.size() && rows[j].trip == rows[i].trip) ++j;
            int route = tripRoute[static_cast<size_t>(rows[i].trip)];
            int fam = routes[static_cast<size_t>(route)].family;
            if (j - i < 2) {
                ++skippedTrips;
                i = j;
                continue;
            }
            Seq seq;
            std::set<int> distinct;
            for (size_t k = i; k < j; ++k) {
                seq.push_back(rows[k].station);
                if (!distinct.insert(rows[k].station).second)
                    fail("trip index " + std::to_string(rows[i].trip) + " visits '" + stationName(rows[k].station) +
                         "' twice. Loop services are not supported yet.");
                if (rows[k].dep >= rows[k].arr) dwells.push_back(rows[k].dep - rows[k].arr);
            }
            auto& pats = routePatterns[static_cast<size_t>(route)];
            if (std::find(pats.begin(), pats.end(), seq) == pats.end()) pats.push_back(seq);
            for (size_t k = i; k + 1 < j; ++k) {
                int a = rows[k].station, b = rows[k + 1].station;
                std::array<int, 3> key = {fam, std::min(a, b), std::max(a, b)};
                int run = rows[k + 1].arr - rows[k].dep;
                if (run > 0) runStats[key].push_back(run);
                if (std::isfinite(rows[k].dist) && std::isfinite(rows[k + 1].dist)) {
                    double dd = std::fabs(rows[k + 1].dist - rows[k].dist);
                    if (dd > 0) distStats[key].push_back(dd);
                }
            }
            i = j;
        }
    }
    if (skippedTrips > 0)
        m.warnings.push_back(std::to_string(skippedTrips) + " trip(s) with fewer than 2 stops were ignored.");
    if (dwells.empty()) fail("no usable dwell times found in stop_times.txt");
    m.dwellSeconds = medianRounded(dwells);

    // ---- merge routes into services, family by family ----------------------------------
    std::vector<std::vector<Candidate>> candidates(families.size());
    {
        int counter = 0;
        for (size_t r = 0; r < routes.size(); ++r)
            for (const Seq& p : routePatterns[r])
                candidates[static_cast<size_t>(routes[r].family)].push_back({p, counter++, routes[r].id});
    }

    std::vector<ServiceDraft> drafts;
    for (size_t f = 0; f < families.size(); ++f) {
        const auto& cl = candidates[f];
        if (cl.empty()) continue;

        // 1. keep only the maximal sequences (longest first; anything contained in a kept one is dropped)
        std::vector<size_t> order(cl.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t x, size_t y) { return cl[x].seq.size() > cl[y].seq.size(); });
        std::vector<size_t> keep;
        for (size_t i : order) {
            bool contained = false;
            for (size_t k : keep)
                if (containsRun(cl[k].seq, cl[i].seq)) {
                    contained = true;
                    break;
                }
            if (!contained) keep.push_back(i);
        }
        std::sort(keep.begin(), keep.end(), [&](size_t x, size_t y) { return cl[x].first < cl[y].first; });

        std::vector<ServiceDraft> fam;
        for (size_t k : keep) fam.push_back({cl[k].seq, cl[k].first, static_cast<int>(f)});

        const size_t patternsBefore = cl.size();
        if (patternsBefore > fam.size())
            m.notes.push_back(families[f].prefix + ": " + std::to_string(patternsBefore) + " route pattern(s) merged into " +
                              std::to_string(fam.size()) + " service(s) (shorter and reversed patterns are the same service).");

        // 2. optional end-to-end join (only for families flagged in line_map.csv)
        if (families[f].join) {
            bool changed = true;
            while (changed) {
                changed = false;
                for (size_t i = 0; i < fam.size() && !changed; ++i)
                    for (size_t j = i + 1; j < fam.size() && !changed; ++j) {
                        const Seq& p = fam[i].seq;
                        const Seq& q = fam[j].seq;
                        const Seq rp = reversed(p), rq = reversed(q);
                        const std::array<std::pair<const Seq*, const Seq*>, 4> tries = {
                            {{&p, &q}, {&p, &rq}, {&rp, &q}, {&rp, &rq}}};
                        for (const auto& t : tries) {
                            const Seq& left = *t.first;
                            const Seq& right = *t.second;
                            if (left.back() != right.front()) continue;
                            std::set<int> a(left.begin(), left.end());
                            size_t common = 0;
                            for (int s : right) common += a.count(s);
                            if (common != 1) continue;
                            Seq merged = left;
                            merged.insert(merged.end(), right.begin() + 1, right.end());
                            m.notes.push_back(families[f].prefix + ": joined two services end to end at '" +
                                              stationName(left.back()) + "' (" + stationName(left.front()) + " to " +
                                              stationName(right.back()) +
                                              "). ASSUMPTION from config/line_map.csv: trains run through.");
                            fam[i].seq = merged;
                            fam.erase(fam.begin() + static_cast<std::ptrdiff_t>(j));
                            changed = true;
                            break;
                        }
                    }
            }
        }
        for (auto& d : fam) drafts.push_back(d);
    }
    std::stable_sort(drafts.begin(), drafts.end(), [](const ServiceDraft& a, const ServiceDraft& b) {
        return a.family != b.family ? a.family < b.family : a.first < b.first;
    });

    // ---- build services (names, ids, merged routes) -------------------------------------
    std::vector<int> servicesInFamily(families.size(), 0);
    for (const auto& d : drafts) servicesInFamily[static_cast<size_t>(d.family)]++;
    for (const auto& d : drafts) {
        const FamilyConfig& fc = families[static_cast<size_t>(d.family)];
        Service s;
        s.id = static_cast<int>(m.services.size()) + 1;
        s.family = fc.prefix;
        s.colour = fc.colour;
        s.op = fc.op;
        s.stations = d.seq;
        s.name = servicesInFamily[static_cast<size_t>(d.family)] == 1
                     ? fc.display
                     : fc.display + ": " + stationName(d.seq.front()) + " to " + stationName(d.seq.back());
        m.services.push_back(s);
    }
    // record which GTFS routes ended up in which service (first service that contains the pattern)
    for (size_t r = 0; r < routes.size(); ++r)
        for (const Seq& p : routePatterns[r])
            for (auto& s : m.services) {
                if (s.family != families[static_cast<size_t>(routes[r].family)].prefix) continue;
                if (!containsRun(s.stations, p)) continue;
                if (std::find(s.routeIds.begin(), s.routeIds.end(), routes[r].id) == s.routeIds.end())
                    s.routeIds.push_back(routes[r].id);
                break;
            }
    for (const auto& s : m.services)
        if (s.routeIds.empty()) fail("internal error: service '" + s.name + "' has no source routes");

    // ---- segments ---------------------------------------------------------------------
    for (const auto& s : m.services) {
        const int fam = familyIndex[s.family];
        for (size_t i = 0; i + 1 < s.stations.size(); ++i) {
            int a = s.stations[i], b = s.stations[i + 1];
            std::array<int, 3> key = {fam, std::min(a, b), std::max(a, b)};
            auto rs = runStats.find(key);
            if (rs == runStats.end() || rs->second.empty())
                fail("no usable run time between '" + stationName(a) + "' and '" + stationName(b) + "' on " + s.name);
            Segment seg;
            seg.service = s.id;
            seg.from = a;
            seg.to = b;
            seg.runSeconds = medianRounded(rs->second);
            auto ds = distStats.find(key);
            seg.distanceM = (ds == distStats.end() || ds->second.empty()) ? 0 : medianRounded(ds->second);
            if (seg.runSeconds <= 0) fail("non-positive run time between '" + stationName(a) + "' and '" + stationName(b) + "'");
            m.segments.push_back(seg);
        }
    }

    // ---- distance corrections (config/distance_overrides.csv) ---------------------------
    for (const auto& o : overrides) {
        auto ia = stationOf.find(o.a), ib = stationOf.find(o.b);
        if (ia == stationOf.end() || ib == stationOf.end())
            fail("config/distance_overrides.csv names a stop that is not in the feed: '" + (ia == stationOf.end() ? o.a : o.b) + "'");
        int changed = 0;
        for (auto& sg : m.segments)
            if ((sg.from == ia->second && sg.to == ib->second) || (sg.from == ib->second && sg.to == ia->second)) {
                sg.distanceM = o.metres;
                ++changed;
            }
        if (changed == 0)
            fail("config/distance_overrides.csv: no hop joins '" + o.a + "' and '" + o.b + "', so the correction cannot be applied");
        m.distanceOverridesApplied += changed;
        m.notes.push_back("Distance correction: " + stationName(ia->second) + " - " + stationName(ib->second) + " set to " + std::to_string(o.metres) +
                          " m on " + std::to_string(changed) + " segment(s). Source: " + o.source);
    }

    // ---- transfers --------------------------------------------------------------------
    std::vector<std::vector<int>> servicesAt(m.stations.size() + 1);
    for (const auto& s : m.services)
        for (int st : s.stations) servicesAt[static_cast<size_t>(st)].push_back(s.id);
    for (size_t st = 1; st < servicesAt.size(); ++st)
        for (int a : servicesAt[st])
            for (int b : servicesAt[st])
                if (a != b) m.transfers.push_back({static_cast<int>(st), a, static_cast<int>(st), b, m.interchangeWalkSeconds});

    std::set<std::pair<int, int>> seedPairs;
    for (const auto& sd : seeds) {
        if (!sd.confirmed) {
            ++m.seedsSkipped;
            continue;
        }
        auto ia = stationOf.find(sd.a), ib = stationOf.find(sd.b);
        if (ia == stationOf.end() || ib == stationOf.end())
            fail("config/transfers_seed.csv names a stop that is not in the feed: '" +
                 (ia == stationOf.end() ? sd.a : sd.b) + "'");
        const int a = ia->second, b = ib->second;
        if (servicesAt[static_cast<size_t>(a)].empty() || servicesAt[static_cast<size_t>(b)].empty())
            fail("config/transfers_seed.csv links a stop that no service calls at ('" + sd.a + "' or '" + sd.b + "')");
        if (!seedPairs.insert({std::min(a, b), std::max(a, b)}).second)
            fail("config/transfers_seed.csv lists the pair '" + sd.a + "' and '" + sd.b + "' twice");
        for (int la : servicesAt[static_cast<size_t>(a)])
            for (int lb : servicesAt[static_cast<size_t>(b)]) {
                m.transfers.push_back({a, la, b, lb, sd.walk});
                m.transfers.push_back({b, lb, a, la, sd.walk});
            }
        ++m.seedsImported;
    }

    // ---- calendar (feed freshness) -----------------------------------------------------
    {
        CsvReader cal(pathIn(gtfsDir, "calendar.txt"));
        if (cal.ok()) {
            int cE = cal.column("end_date");
            std::vector<std::string> row;
            while (cal.next(row)) {
                std::string e = CsvReader::get(row, cE);
                if (e.size() == 8 && (m.feedValidUntil.empty() || e > m.feedValidUntil)) m.feedValidUntil = e;
            }
        }
    }

    // ---- validation and warnings --------------------------------------------------------
    {
        // unserved stations
        std::vector<std::string> unserved;
        for (const auto& st : m.stations)
            if (servicesAt[static_cast<size_t>(st.id)].empty()) unserved.push_back(st.name);
        if (!unserved.empty()) {
            if (unserved.size() > 5) unserved.resize(5);
            m.warnings.push_back("Some stations are not served by any service (first few: " + joinStrings(unserved, ", ") + ").");
        }

        // connected pieces
        DisjointSets ds(m.stations.size() + 1);
        for (const auto& sg : m.segments) ds.unite(sg.from, sg.to);
        for (const auto& t : m.transfers) ds.unite(t.fromStation, t.toStation);
        std::map<int, int> sizes;
        std::map<int, std::set<std::string>> famsIn;
        for (const auto& s : m.services)
            for (int st : s.stations) {
                sizes[ds.find(st)] += 0;
                famsIn[ds.find(st)].insert(s.family);
            }
        std::set<int> counted;
        for (const auto& s : m.services)
            for (int st : s.stations)
                if (counted.insert(st).second) sizes[ds.find(st)]++;
        std::vector<std::pair<int, int>> bySize;
        for (auto& kv : sizes) bySize.push_back({kv.second, kv.first});
        std::sort(bySize.begin(), bySize.end(), std::greater<>());
        for (auto& p : bySize) m.componentSizes.push_back(p.first);
        if (bySize.size() > 1)
            for (size_t i = 1; i < bySize.size(); ++i) {
                std::vector<std::string> names(famsIn[bySize[i].second].begin(), famsIn[bySize[i].second].end());
                m.warnings.push_back("Disconnected piece of the network: " + std::to_string(bySize[i].first) + " station(s) on " +
                                     joinStrings(names, ", ") +
                                     ". Passengers cannot route between it and the rest until a transfer links them.");
            }

        // long hops
        std::vector<const Segment*> longHops;
        for (const auto& sg : m.segments)
            if (sg.runSeconds > 600) longHops.push_back(&sg);
        std::sort(longHops.begin(), longHops.end(), [](const Segment* a, const Segment* b) { return a->runSeconds > b->runSeconds; });
        if (!longHops.empty()) {
            std::vector<std::string> ex;
            for (size_t i = 0; i < longHops.size() && i < 4; ++i)
                ex.push_back(stationName(longHops[i]->from) + " - " + stationName(longHops[i]->to) + " (" +
                             std::to_string(longHops[i]->runSeconds / 60) + " min)");
            m.warnings.push_back(std::to_string(longHops.size()) + " hop(s) take over 10 minutes, which looks like a data artefact: " +
                                 joinStrings(ex, "; ") + ".");
        }

        // hops that take real time but have almost no distance (a sign of wrong coordinates in the feed)
        {
            std::vector<std::string> ex;
            long long count = 0;
            for (const auto& sg : m.segments)
                if (sg.distanceM > 0 && sg.distanceM < 100 && sg.runSeconds >= 60) {
                    ++count;
                    if (ex.size() < 4)
                        ex.push_back(stationName(sg.from) + " - " + stationName(sg.to) + " (" + std::to_string(sg.distanceM) + " m, run time " +
                                     std::to_string(sg.runSeconds) + " s)");
                }
            if (count > 0)
                m.warnings.push_back(std::to_string(count) + " hop(s) have almost no distance but take real time, so their distance is probably wrong: " +
                                     joinStrings(ex, "; ") + ". Fares across them are unreliable. Add a sourced correction to config/distance_overrides.csv.");
        }

        // distances
        long long noDist = 0;
        std::vector<std::pair<double, std::string>> odd;
        for (const auto& sg : m.segments) {
            if (sg.distanceM <= 0) {
                ++noDist;
                continue;
            }
            const Station& a = m.stations[static_cast<size_t>(sg.from - 1)];
            const Station& b = m.stations[static_cast<size_t>(sg.to - 1)];
            double straight = gtfs::haversineMeters(a.lat, a.lon, b.lat, b.lon);
            if (straight < 50) continue;
            double ratio = sg.distanceM / straight;
            if (ratio < 0.95 || ratio > 1.5)
                odd.push_back({std::fabs(ratio - 1.0), a.name + " - " + b.name + " (feed " + std::to_string(sg.distanceM) +
                                                           " m, straight line " + std::to_string(static_cast<int>(straight)) + " m)"});
        }
        if (noDist > 0) m.warnings.push_back(std::to_string(noDist) + " segment(s) have no distance. Fares cannot be computed for them.");
        if (!odd.empty()) {
            std::sort(odd.begin(), odd.end(), std::greater<>());
            std::vector<std::string> ex;
            for (size_t i = 0; i < odd.size() && i < 3; ++i) ex.push_back(odd[i].second);
            m.warnings.push_back(std::to_string(odd.size()) + " segment(s) have a feed distance far from the straight-line distance: " +
                                 joinStrings(ex, "; ") + ".");
        }

        if (m.feedValidUntil.empty())
            m.warnings.push_back("calendar.txt is missing, so the feed's validity period is unknown.");
        else if (m.feedValidUntil < todayYyyymmdd())
            m.warnings.push_back("The feed's calendar ended on " + m.feedValidUntil +
                                 ". Newer stations and extensions are probably missing.");
        if (m.seedsSkipped > 0)
            m.notes.push_back(std::to_string(m.seedsSkipped) +
                              " candidate transfer(s) in config/transfers_seed.csv were skipped because confirmed is 'no'.");
    }
    return m;
}

// =======================================================================================
// describe
// =======================================================================================

std::string describe(const Model& m) {
    std::ostringstream o;
    o << "IMPORT REPORT\n\n";
    o << "Stations: " << m.stations.size() << "   Services: " << m.services.size() << "   Segments: " << m.segments.size()
      << "   Transfers: " << m.transfers.size() << "\n";
    o << "Dwell per stop (feed median): " << m.dwellSeconds << " s   Interchange walk (PLACEHOLDER): " << m.interchangeWalkSeconds
      << " s\n";
    o << "Journey planner: routes within " << m.similarTimeSeconds << " s count as equally fast; at most " << m.maxRoutesPerGroup << " routes per need; drop routes over "
      << m.maxSlowerPercent << "% slower\n";
    o << "Feed calendar valid until: " << (m.feedValidUntil.empty() ? "unknown" : m.feedValidUntil) << "\n";
    {
        std::set<std::string> ops;
        int unverified = 0;
        for (const auto& f : m.fareSlabs) {
            ops.insert(f.op);
            if (f.source.rfind("UNVERIFIED", 0) == 0) ++unverified;
        }
        o << "Fare slabs: " << m.fareSlabs.size() << " row(s) for operator(s):";
        for (const auto& op : ops) o << " " << op;
        o << (m.fareSlabs.empty() ? " (none)" : "") << "   not yet verified: " << unverified << "\n";
        o << "Station-pair fares: " << m.farePairs.size() << " row(s)   Distance corrections applied: " << m.distanceOverridesApplied
          << " segment(s)   Coordinate corrections applied: " << m.stationOverridesApplied << "\n";
    }
    o << "Cross-station transfers imported: " << m.seedsImported << "   skipped (not confirmed): " << m.seedsSkipped << "\n";
    o << "Connected pieces: " << m.componentSizes.size() << " (station counts:";
    for (int s : m.componentSizes) o << " " << s;
    o << ")\n\nServices:\n";
    for (const auto& s : m.services)
        o << "  " << std::setw(2) << s.id << "  " << s.name << "  |  " << s.op << "  |  " << s.stations.size() << " stops  |  routes: "
          << joinStrings(s.routeIds, " ") << "\n";
    o << "\nDecisions made automatically:\n";
    if (m.notes.empty()) o << "  (none)\n";
    for (const auto& n : m.notes) o << "  - " << n << "\n";
    o << "\nWarnings (worth a human look):\n";
    if (m.warnings.empty()) o << "  (none)\n";
    for (const auto& w : m.warnings) o << "  - " << w << "\n";
    o << "\nAssumptions to remember:\n"
         "  - Line colours in config/line_map.csv are cosmetic; the feed has none.\n"
         "  - The interchange walking time is a placeholder, not a measured value.\n"
         "  - Run times come from the feed and are estimates, not a real timetable.\n"
         "  - Distances come from the feed's shape_dist_traveled (they look like metres).\n";
    return o.str();
}

// =======================================================================================
// importInto
// =======================================================================================

namespace {

std::string num(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(10) << v;
    return o.str();
}

// Runs "INSERT INTO <head> VALUES <rows>" in batches so statements stay small.
void insertRows(Database& db, const std::string& head, const std::vector<std::string>& rows) {
    const size_t batch = 200;
    for (size_t i = 0; i < rows.size(); i += batch) {
        std::string sql = "INSERT INTO " + head + " VALUES ";
        for (size_t j = i; j < rows.size() && j < i + batch; ++j) {
            if (j > i) sql += ",";
            sql += rows[j];
        }
        db.exec(sql + ";");
    }
}

}  // namespace

int importInto(Database& db, const Model& m) {
    std::string version;
    try {
        version = db.scalarText("SELECT value FROM meta WHERE key='schema_version';");
    } catch (const std::exception&) {
        throw ImportError("this database has no schema version. Create it with: metro_db init --force");
    }
    if (version != "5")
        throw ImportError("database schema version is " + version + " but this importer needs 5. Recreate it with: metro_db init --force");

    int newVersion = 1;
    {
        std::string old;
        try {
            old = db.scalarText("SELECT value FROM meta WHERE key='data_version';");
        } catch (const std::exception&) {
        }
        long v = 0;
        if (!old.empty()) v = std::strtol(old.c_str(), nullptr, 10);
        newVersion = static_cast<int>(v) + 1;
    }

    Transaction tx(db);   // everything below is undone if anything throws
    db.exec("DELETE FROM transfers; DELETE FROM segments; DELETE FROM line_stops; DELETE FROM lines; DELETE FROM stations; DELETE FROM fare_slabs; DELETE FROM fare_pairs;");

    std::vector<std::string> rows;
    for (const auto& s : m.stations)
        rows.push_back("(" + std::to_string(s.id) + "," + sqlQuote(s.gtfsId) + "," + sqlQuote(s.name) + "," + num(s.lat) + "," + num(s.lon) + ")");
    insertRows(db, "stations(station_id,gtfs_stop_id,name,lat,lon)", rows);

    rows.clear();
    for (const auto& s : m.services) {
        std::vector<std::string> ids = s.routeIds;
        rows.push_back("(" + std::to_string(s.id) + "," + sqlQuote(s.family) + "," + sqlQuote(s.name) + "," +
                       (s.colour.empty() ? std::string("NULL") : sqlQuote(s.colour)) + "," + sqlQuote(s.op) + "," +
                       sqlQuote(joinStrings(ids, " ")) + ")");
    }
    insertRows(db, "lines(line_id,family,name,colour,operator,gtfs_route_ids)", rows);

    rows.clear();
    for (const auto& s : m.services)
        for (size_t i = 0; i < s.stations.size(); ++i)
            rows.push_back("(" + std::to_string(s.id) + "," + std::to_string(i) + "," + std::to_string(s.stations[i]) + ")");
    insertRows(db, "line_stops(line_id,seq,station_id)", rows);

    rows.clear();
    for (const auto& s : m.segments)
        rows.push_back("(" + std::to_string(s.service) + "," + std::to_string(s.from) + "," + std::to_string(s.to) + "," +
                       std::to_string(s.runSeconds) + "," + (s.distanceM > 0 ? std::to_string(s.distanceM) : std::string("NULL")) + ")");
    insertRows(db, "segments(line_id,from_station,to_station,run_seconds,distance_m)", rows);

    rows.clear();
    for (const auto& t : m.transfers)
        rows.push_back("(" + std::to_string(t.fromStation) + "," + std::to_string(t.fromService) + "," + std::to_string(t.toStation) + "," +
                       std::to_string(t.toService) + "," + std::to_string(t.walkSeconds) + ")");
    insertRows(db, "transfers(from_station,from_line,to_station,to_line,walk_seconds)", rows);

    rows.clear();
    for (const auto& f : m.fareSlabs)
        rows.push_back("(" + sqlQuote(f.op) + "," + sqlQuote(f.dayType) + "," + sqlQuote(f.ticket) + "," + num(f.minKm) + "," + num(f.maxKm) + "," +
                       std::to_string(f.fareInr) + "," + sqlQuote(f.validFrom) + "," + sqlQuote(f.source) + ")");
    insertRows(db, "fare_slabs(operator,day_type,ticket,min_km,max_km,fare_inr,valid_from,source)", rows);

    rows.clear();
    for (const auto& f : m.farePairs)
        rows.push_back("(" + sqlQuote(f.op) + "," + sqlQuote(f.dayType) + "," + sqlQuote(f.ticket) + "," + sqlQuote(f.stopA) + "," + sqlQuote(f.stopB) + "," +
                       std::to_string(f.fareInr) + "," + sqlQuote(f.validFrom) + "," + sqlQuote(f.source) + ")");
    insertRows(db, "fare_pairs(operator,day_type,ticket,stop_a,stop_b,fare_inr,valid_from,source)", rows);

    auto setMeta = [&](const std::string& key, const std::string& valueSql) {
        db.exec("INSERT OR REPLACE INTO meta(key,value) VALUES (" + sqlQuote(key) + "," + valueSql + ");");
    };
    setMeta("data_version", sqlQuote(std::to_string(newVersion)));
    setMeta("last_import_at", "datetime('now')");
    setMeta("dwell_seconds", sqlQuote(std::to_string(m.dwellSeconds)));
    setMeta("interchange_walk_seconds", sqlQuote(std::to_string(m.interchangeWalkSeconds)));
    setMeta("similar_time_seconds", sqlQuote(std::to_string(m.similarTimeSeconds)));
    setMeta("max_routes_per_group", sqlQuote(std::to_string(m.maxRoutesPerGroup)));
    setMeta("max_slower_percent", sqlQuote(std::to_string(m.maxSlowerPercent)));
    setMeta("feed_valid_until", sqlQuote(m.feedValidUntil.empty() ? "unknown" : m.feedValidUntil));

    if (!db.columnText("PRAGMA foreign_key_check;").empty())
        throw ImportError("foreign key check failed after import; nothing was saved");
    tx.commit();
    return newVersion;
}

}  // namespace import
}  // namespace metro
