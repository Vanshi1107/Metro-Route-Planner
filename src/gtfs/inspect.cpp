#include "inspect.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common/csv.h"

namespace fs = std::filesystem;

namespace metro {
namespace gtfs {

// ---------------------------------------------------------------------------------------
// Small public helpers (unit-tested)
// ---------------------------------------------------------------------------------------

int parseGtfsTime(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) return -1;
    size_t c1 = s.find(':');
    if (c1 == std::string::npos) return -1;
    size_t c2 = s.find(':', c1 + 1);
    if (c2 == std::string::npos) return -1;
    auto allDigits = [](const std::string& t) {
        if (t.empty()) return false;
        for (char ch : t)
            if (ch < '0' || ch > '9') return false;
        return true;
    };
    std::string h = s.substr(0, c1), m = s.substr(c1 + 1, c2 - c1 - 1), sec = s.substr(c2 + 1);
    if (!allDigits(h) || !allDigits(m) || !allDigits(sec)) return -1;
    if (h.size() > 3 || m.size() != 2 || sec.size() != 2) return -1;
    int hh = std::atoi(h.c_str()), mm = std::atoi(m.c_str()), ss = std::atoi(sec.c_str());
    if (mm > 59 || ss > 59) return -1;
    return hh * 3600 + mm * 60 + ss;
}

std::string normaliseName(const std::string& s) {
    std::string out;
    bool lastSpace = true;
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            lastSpace = false;
        } else if (ch >= 0x80) {          // keep non-ASCII (e.g. Hindi) bytes as they are
            out.push_back(static_cast<char>(ch));
            lastSpace = false;
        } else if (!lastSpace) {
            out.push_back(' ');
            lastSpace = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
    const double kPi = 3.14159265358979323846;
    const double R = 6371000.0;
    auto rad = [&](double d) { return d * kPi / 180.0; };
    double dLat = rad(lat2 - lat1), dLon = rad(lon2 - lon1);
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
}

// ---------------------------------------------------------------------------------------
// Internal types and helpers
// ---------------------------------------------------------------------------------------
namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

bool parseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

struct Stop {
    std::string id, name, norm;
    double lat = kNaN, lon = kNaN;
};

struct Route {
    std::string id, shortName, longName, type, colour;
    std::string family;                          // text before the first '_' in route_long_name
    long long trips = 0;
    std::set<std::string> headsigns;
    std::map<std::string, long long> patterns;   // stop sequence -> number of trips
    std::vector<int> longest;                    // longest stop sequence seen
};

struct StRow {
    int trip, stop, seq, arr, dep;
    double dist;
};

struct PairStat {
    long long hops = 0;
    long long validRuns = 0;
    int minRun = INT_MAX;
    int maxRun = INT_MIN;
    double distDelta = kNaN;   // shape_dist_traveled difference, first seen
};

std::string joinStrings(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::string mmss(long long sec) {
    std::ostringstream o;
    o << sec / 60 << ":" << std::setw(2) << std::setfill('0') << sec % 60;
    return o.str();
}

template <typename T>
double medianOf(std::vector<T> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? static_cast<double>(v[n / 2]) : (static_cast<double>(v[n / 2 - 1]) + static_cast<double>(v[n / 2])) / 2.0;
}

uint64_t pairKey(int a, int b) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint32_t>(b);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// The analysis
// ---------------------------------------------------------------------------------------

std::string analyze(const std::string& dir, size_t limit, InspectStats* statsOut) {
    InspectStats st;
    std::ostringstream out;
    out << std::fixed;

    auto heading = [&](const std::string& t) { out << "\n==== " << t << " ====\n"; };
    auto path = [&](const char* f) { return (fs::path(dir) / f).string(); };
    auto more = [&](size_t total) {
        if (total > limit) out << "  ... and " << (total - limit) << " more (use --limit N to see more)\n";
    };

    out << "GTFS INSPECTION REPORT\nFolder: " << dir << "\n";

    std::error_code ec;
    if (fs::is_regular_file(dir, ec)) {
        out << "\nERROR: that is a file, not a folder. If it is a .zip, unzip it first and pass the folder.\n";
        if (statsOut) *statsOut = st;
        return out.str();
    }
    if (!fs::is_directory(dir, ec)) {
        out << "\nERROR: folder not found.\n";
        if (statsOut) *statsOut = st;
        return out.str();
    }

    // ---- 1. Files ---------------------------------------------------------------------
    heading("1. FILES IN THE FEED");
    {
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".txt") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        if (files.empty()) out << "No .txt files found. Is this the right folder (it should contain stops.txt)?\n";
        for (auto& f : files) {
            CsvReader r(f.string());
            long long rows = 0;
            std::vector<std::string> row;
            while (r.next(row)) ++rows;
            out << f.filename().string() << "  |  " << rows << " rows  |  columns: "
                << (r.ok() ? joinStrings(r.header(), ", ") : std::string("(unreadable or empty)")) << "\n";
        }
    }

    // ---- read stops -------------------------------------------------------------------
    std::vector<Stop> stops;
    std::unordered_map<std::string, int> stopIdx;
    std::map<std::string, long long> locTypes;
    long long duplicateStopIds = 0, emptyNames = 0;
    double minLat = 1e9, maxLat = -1e9, minLon = 1e9, maxLon = -1e9;
    bool hasLocType = false, hasParent = false;
    {
        CsvReader r(path("stops.txt"));
        st.foundStops = r.ok();
        if (r.ok()) {
            int cId = r.column("stop_id"), cName = r.column("stop_name"), cLat = r.column("stop_lat"),
                cLon = r.column("stop_lon"), cLoc = r.column("location_type"), cPar = r.column("parent_station");
            hasLocType = cLoc >= 0;
            hasParent = cPar >= 0;
            std::vector<std::string> row;
            while (r.next(row)) {
                ++st.stopsRows;
                Stop s;
                s.id = CsvReader::get(row, cId);
                s.name = CsvReader::get(row, cName);
                s.norm = normaliseName(s.name);
                if (s.name.empty()) ++emptyNames;
                double la = 0, lo = 0;
                if (parseDouble(CsvReader::get(row, cLat), la) && parseDouble(CsvReader::get(row, cLon), lo) &&
                    la >= -90 && la <= 90 && lo >= -180 && lo <= 180) {
                    s.lat = la;
                    s.lon = lo;
                    minLat = std::min(minLat, la);
                    maxLat = std::max(maxLat, la);
                    minLon = std::min(minLon, lo);
                    maxLon = std::max(maxLon, lo);
                } else {
                    ++st.invalidCoords;
                }
                std::string lt = CsvReader::get(row, cLoc);
                locTypes[lt.empty() ? "0" : lt]++;
                if (!CsvReader::get(row, cPar).empty()) ++st.parentStationRows;
                if (s.id.empty() || stopIdx.count(s.id)) {
                    ++duplicateStopIds;
                    continue;
                }
                stopIdx[s.id] = static_cast<int>(stops.size());
                stops.push_back(std::move(s));
            }
        }
    }

    // ---- read routes ------------------------------------------------------------------
    std::vector<Route> routes;
    std::unordered_map<std::string, int> routeIdx;
    {
        CsvReader r(path("routes.txt"));
        if (r.ok()) {
            int cId = r.column("route_id"), cShort = r.column("route_short_name"),
                cLong = r.column("route_long_name"), cType = r.column("route_type"), cCol = r.column("route_color");
            std::vector<std::string> row;
            while (r.next(row)) {
                ++st.routesRows;
                Route rt;
                rt.id = CsvReader::get(row, cId);
                rt.shortName = CsvReader::get(row, cShort);
                rt.longName = CsvReader::get(row, cLong);
                rt.type = CsvReader::get(row, cType);
                rt.colour = CsvReader::get(row, cCol);
                rt.family = rt.longName.substr(0, rt.longName.find('_'));
                if (rt.id.empty() || routeIdx.count(rt.id)) continue;
                routeIdx[rt.id] = static_cast<int>(routes.size());
                routes.push_back(std::move(rt));
            }
        }
    }

    // ---- read trips -------------------------------------------------------------------
    std::unordered_map<std::string, int> tripIdx;
    std::vector<int> tripRoute;
    long long headsignRows = 0, tripsUnknownRoute = 0;
    std::map<std::string, long long> directions;
    {
        CsvReader r(path("trips.txt"));
        if (r.ok()) {
            int cTrip = r.column("trip_id"), cRoute = r.column("route_id"), cHead = r.column("trip_headsign"),
                cDir = r.column("direction_id");
            st.hasHeadsign = cHead >= 0;
            std::vector<std::string> row;
            while (r.next(row)) {
                ++st.tripsRows;
                std::string tid = CsvReader::get(row, cTrip);
                auto ri = routeIdx.find(CsvReader::get(row, cRoute));
                int route = ri == routeIdx.end() ? -1 : ri->second;
                if (route < 0) ++tripsUnknownRoute;
                if (tid.empty() || tripIdx.count(tid)) continue;
                tripIdx[tid] = static_cast<int>(tripRoute.size());
                tripRoute.push_back(route);
                if (route >= 0) {
                    routes[static_cast<size_t>(route)].trips++;
                    std::string hs = CsvReader::get(row, cHead);
                    if (!hs.empty()) {
                        ++headsignRows;
                        routes[static_cast<size_t>(route)].headsigns.insert(hs);
                    }
                }
                if (cDir >= 0) directions[CsvReader::get(row, cDir)]++;
            }
        }
    }

    // ---- read stop_times --------------------------------------------------------------
    std::vector<StRow> rows;
    std::unordered_set<std::string> orphanStopIds, orphanTripIds;
    long long rowsMissingTimes = 0;
    {
        CsvReader r(path("stop_times.txt"));
        if (r.ok()) {
            int cTrip = r.column("trip_id"), cStop = r.column("stop_id"), cSeq = r.column("stop_sequence"),
                cArr = r.column("arrival_time"), cDep = r.column("departure_time"),
                cDist = r.column("shape_dist_traveled");
            st.hasShapeDistColumn = cDist >= 0;
            std::vector<std::string> row;
            while (r.next(row)) {
                ++st.stopTimesRows;
                std::string tid = CsvReader::get(row, cTrip), sid = CsvReader::get(row, cStop);
                auto ti = tripIdx.find(tid);
                if (ti == tripIdx.end()) {
                    ++st.orphanTripRows;
                    orphanTripIds.insert(tid);
                    continue;
                }
                auto si = stopIdx.find(sid);
                if (si == stopIdx.end()) {
                    ++st.orphanStopTimeRows;
                    orphanStopIds.insert(sid);
                    continue;
                }
                StRow sr;
                sr.trip = ti->second;
                sr.stop = si->second;
                double seqD;
                sr.seq = parseDouble(CsvReader::get(row, cSeq), seqD) ? static_cast<int>(seqD)
                                                                      : static_cast<int>(rows.size());
                sr.arr = parseGtfsTime(CsvReader::get(row, cArr));
                sr.dep = parseGtfsTime(CsvReader::get(row, cDep));
                if (sr.arr < 0 && sr.dep >= 0) sr.arr = sr.dep;
                if (sr.dep < 0 && sr.arr >= 0) sr.dep = sr.arr;
                if (sr.arr < 0) ++rowsMissingTimes;
                double d;
                if (parseDouble(CsvReader::get(row, cDist), d)) {
                    sr.dist = d;
                    ++st.shapeDistRows;
                } else {
                    sr.dist = kNaN;
                }
                rows.push_back(sr);
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const StRow& a, const StRow& b) {
        return a.trip != b.trip ? a.trip < b.trip : a.seq < b.seq;
    });

    // ---- analyse trips: hops, patterns, usage -----------------------------------------
    std::unordered_map<uint64_t, PairStat> pairs;
    std::vector<int> runs, dwells;
    std::vector<std::set<int>> routesServing(stops.size());
    std::vector<std::set<std::string>> familiesServing(stops.size());
    std::vector<char> stopUsed(stops.size(), 0);
    long long tripsInStopTimes = 0, hopsMissingTimes = 0, selfHops = 0, totalHops = 0;
    {
        size_t i = 0;
        while (i < rows.size()) {
            size_t j = i;
            while (j < rows.size() && rows[j].trip == rows[i].trip) ++j;
            ++tripsInStopTimes;
            int route = tripRoute[static_cast<size_t>(rows[i].trip)];
            std::string key;
            std::vector<int> seqStops;
            for (size_t k = i; k < j; ++k) {
                size_t s = static_cast<size_t>(rows[k].stop);
                stopUsed[s] = 1;
                if (route >= 0) {
                    routesServing[s].insert(route);
                    familiesServing[s].insert(routes[static_cast<size_t>(route)].family);
                }
                key += std::to_string(rows[k].stop);
                key += ',';
                seqStops.push_back(rows[k].stop);
                if (rows[k].arr >= 0 && rows[k].dep >= rows[k].arr) dwells.push_back(rows[k].dep - rows[k].arr);
            }
            if (route >= 0) {
                Route& rt = routes[static_cast<size_t>(route)];
                rt.patterns[key]++;
                if (seqStops.size() > rt.longest.size()) rt.longest = seqStops;
            }
            for (size_t k = i; k + 1 < j; ++k) {
                ++totalHops;
                int from = rows[k].stop, to = rows[k + 1].stop;
                if (from == to) {
                    ++selfHops;
                    continue;
                }
                PairStat& ps = pairs[pairKey(from, to)];
                ++ps.hops;
                if (rows[k].dep < 0 || rows[k + 1].arr < 0) {
                    ++hopsMissingTimes;
                } else {
                    int run = rows[k + 1].arr - rows[k].dep;
                    if (run <= 0) {
                        ++st.nonPositiveRuns;
                    } else {
                        runs.push_back(run);
                        ++ps.validRuns;
                        ps.minRun = std::min(ps.minRun, run);
                        ps.maxRun = std::max(ps.maxRun, run);
                    }
                }
                if (std::isfinite(rows[k].dist) && std::isfinite(rows[k + 1].dist)) {
                    double dd = rows[k + 1].dist - rows[k].dist;
                    if (dd > 0 && std::isnan(ps.distDelta)) ps.distDelta = dd;
                }
            }
            i = j;
        }
    }
    st.directedPairs = static_cast<long long>(pairs.size());
    {
        std::set<uint64_t> und;
        for (auto& kv : pairs) {
            int a = static_cast<int>(kv.first >> 32), b = static_cast<int>(kv.first & 0xFFFFFFFFu);
            und.insert(pairKey(std::min(a, b), std::max(a, b)));
        }
        st.undirectedPairs = static_cast<long long>(und.size());
    }
    for (auto& kv : pairs)
        if (kv.second.validRuns > 1 && kv.second.maxRun - kv.second.minRun > 60) ++st.variablePairs;
    for (size_t s = 0; s < stops.size(); ++s)
        if (!stopUsed[s]) ++st.unusedStops;
    for (size_t s = 0; s < stops.size(); ++s)
        if (familiesServing[s].size() >= 2) ++st.sharedStops;

    // =====================================================================================
    // PRINT SECTIONS
    // =====================================================================================

    heading("2. STOPS (stops.txt)");
    if (!st.foundStops) {
        out << "stops.txt not found or unreadable. Nothing else can be analysed without it.\n";
    } else {
        out << "Rows: " << st.stopsRows << "   usable stops: " << stops.size() << "   duplicate/empty stop_id rows: "
            << duplicateStopIds << "   empty names: " << emptyNames << "\n";
        out << "Rows with missing or invalid coordinates: " << st.invalidCoords << "\n";
        if (minLat < 1e8)
            out << std::setprecision(4) << "Bounding box: lat " << minLat << " .. " << maxLat << ", lon " << minLon
                << " .. " << maxLon << "\n";
        out << "location_type column: " << (hasLocType ? "present" : "absent") << "  ";
        if (hasLocType) {
            out << "(";
            bool first = true;
            for (auto& kv : locTypes) {
                out << (first ? "" : ", ") << "type " << kv.first << ": " << kv.second;
                first = false;
            }
            out << ")  [0=stop/platform, 1=station, 2=entrance, 3=generic, 4=boarding area]";
        }
        out << "\nparent_station column: " << (hasParent ? "present" : "absent") << ", stops with a parent: "
            << st.parentStationRows << "\n";
        out << "First stops:\n";
        for (size_t i = 0; i < stops.size() && i < std::min<size_t>(limit, 8); ++i)
            out << "  " << stops[i].id << " | " << stops[i].name << " | " << std::setprecision(5) << stops[i].lat
                << ", " << stops[i].lon << "\n";
    }

    heading("3. ROUTES / LINES (routes.txt + trips.txt)");
    out << "Routes: " << st.routesRows << "   trips: " << st.tripsRows << "   trips naming an unknown route: "
        << tripsUnknownRoute << "\n";
    out << "trip_headsign column: " << (st.hasHeadsign ? "present" : "ABSENT") << " (non-empty on " << headsignRows
        << " trips)   direction_id values: ";
    if (directions.empty()) out << "(none)";
    for (auto& kv : directions) out << "[" << (kv.first.empty() ? "empty" : kv.first) << ": " << kv.second << "] ";
    out << "\n";
    {
        size_t shown = 0;
        for (size_t ri = 0; ri < routes.size(); ++ri) {
            if (shown++ >= limit) break;
            const Route& rt = routes[ri];
            out << "  " << rt.id << " | " << rt.shortName << " | " << rt.longName << " | type " << rt.type << " | #"
                << rt.colour << " | " << rt.trips << " trips | " << rt.patterns.size() << " distinct stop patterns";
            if (!rt.longest.empty())
                out << " | longest " << rt.longest.size() << " stops: " << stops[static_cast<size_t>(rt.longest.front())].name
                    << " -> " << stops[static_cast<size_t>(rt.longest.back())].name;
            if (!rt.headsigns.empty()) {
                std::vector<std::string> hs(rt.headsigns.begin(), rt.headsigns.end());
                if (hs.size() > 4) hs.resize(4);
                out << " | headsigns: " << joinStrings(hs, " / ");
            }
            out << "\n";
        }
        more(routes.size());
    }
    out << "(Several distinct stop patterns on one route usually means branches or short-turn trips.)\n";

    heading("4. STOP_TIMES: TRIPS AND DATA QUALITY");
    out << "Rows: " << st.stopTimesRows << "   usable rows: " << rows.size() << "   trips with stop times: "
        << tripsInStopTimes << " (trips.txt has " << st.tripsRows << ")\n";
    out << "Rows pointing to an UNKNOWN stop: " << st.orphanStopTimeRows << " (" << orphanStopIds.size()
        << " distinct ids)   rows pointing to an UNKNOWN trip: " << st.orphanTripRows << " (" << orphanTripIds.size()
        << " distinct ids)\n";
    out << "Rows with no usable arrival/departure time: " << rowsMissingTimes << "\n";
    out << "Stops that no trip calls at: " << st.unusedStops << " of " << stops.size() << "\n";
    if (!dwells.empty())
        out << "Dwell time at stops (departure - arrival): median " << mmss(static_cast<long long>(medianOf(dwells)))
            << ", max " << mmss(*std::max_element(dwells.begin(), dwells.end())) << "\n";
    {
        size_t shown = 0;
        out << "Examples of unknown stop ids:";
        for (auto& id : orphanStopIds) {
            if (shown++ >= std::min<size_t>(limit, 5)) break;
            out << " " << id;
        }
        if (orphanStopIds.empty()) out << " (none)";
        out << "\n";
    }

    heading("5. GRAPH PREVIEW (what the importer would build)");
    out << "Hops between consecutive stops in trips: " << totalHops << " (self-hops skipped: " << selfHops << ")\n";
    out << "Distinct DIRECTED adjacent pairs: " << st.directedPairs << "   UNDIRECTED (edges of the graph): "
        << st.undirectedPairs << "\n";
    if (!runs.empty()) {
        std::vector<int> sorted = runs;
        std::sort(sorted.begin(), sorted.end());
        out << "Run time between stops (arrival at next - departure from this): min " << mmss(sorted.front())
            << ", median " << mmss(static_cast<long long>(medianOf(sorted))) << ", 95th pct "
            << mmss(sorted[static_cast<size_t>(0.95 * static_cast<double>(sorted.size() - 1))]) << ", max "
            << mmss(sorted.back()) << "\n";
    } else {
        out << "No usable run times could be computed.\n";
    }
    {
        std::vector<std::tuple<int, int, int>> longest;   // run, from, to
        for (auto& kv : pairs)
            if (kv.second.validRuns > 0)
                longest.emplace_back(kv.second.maxRun, static_cast<int>(kv.first >> 32), static_cast<int>(kv.first & 0xFFFFFFFFu));
        std::sort(longest.begin(), longest.end(), std::greater<std::tuple<int, int, int>>());
        out << "Longest hops between consecutive stops:\n";
        for (size_t i = 0; i < longest.size() && i < std::min<size_t>(limit, 5); ++i)
            out << "  " << mmss(std::get<0>(longest[i])) << "  " << stops[static_cast<size_t>(std::get<1>(longest[i]))].name << " -> "
                << stops[static_cast<size_t>(std::get<2>(longest[i]))].name << "\n";
    }
    out << "Hops with missing times: " << hopsMissingTimes << "   hops with zero/negative run time: "
        << st.nonPositiveRuns << "\n";
    out << "Pairs whose run time differs by more than 60 s between trips (peak vs off-peak?): " << st.variablePairs
        << "\n";

    heading("6. DISTANCES (matters for fares)");
    out << "shape_dist_traveled column: " << (st.hasShapeDistColumn ? "present" : "ABSENT") << "   rows with a value: "
        << st.shapeDistRows << "\n";
    {
        std::vector<double> ratios, deltas;
        for (auto& kv : pairs) {
            if (!std::isfinite(kv.second.distDelta)) continue;
            const Stop& a = stops[static_cast<size_t>(kv.first >> 32)];
            const Stop& b = stops[static_cast<size_t>(kv.first & 0xFFFFFFFFu)];
            if (!std::isfinite(a.lat) || !std::isfinite(b.lat)) continue;
            double hv = haversineMeters(a.lat, a.lon, b.lat, b.lon);
            deltas.push_back(kv.second.distDelta);
            if (hv > 50) ratios.push_back(kv.second.distDelta / hv);
        }
        if (ratios.empty()) {
            out << "No pairs with both a feed distance and coordinates, so units cannot be checked.\n";
        } else {
            double med = medianOf(ratios);
            out << std::setprecision(3) << "Feed distance between adjacent stops: median " << medianOf(deltas)
                << " (raw units).  Ratio feed distance / straight-line distance: median " << med << "\n";
            if (med > 0.5 && med < 5)
                out << "=> The values look like METRES (track length is a little longer than the straight line).\n";
            else if (med > 0.0005 && med < 0.005)
                out << "=> The values look like KILOMETRES.\n";
            else
                out << "=> Units are unclear. Do not trust these distances without checking.\n";
        }
    }

    heading("7. INTERCHANGES (how the feed represents them)");
    {
        std::map<std::string, std::vector<int>> byName;
        for (size_t i = 0; i < stops.size(); ++i)
            if (!stops[i].norm.empty()) byName[stops[i].norm].push_back(static_cast<int>(i));
        std::vector<const std::vector<int>*> groups;
        for (auto& kv : byName)
            if (kv.second.size() > 1) groups.push_back(&kv.second);
        st.interchangeNameGroups = static_cast<long long>(groups.size());

        out << "A) Names shared by several different stop_ids: " << groups.size() << "\n";
        for (size_t g = 0; g < groups.size() && g < limit; ++g) {
            const auto& ids = *groups[g];
            double maxD = 0;
            for (size_t a = 0; a < ids.size(); ++a)
                for (size_t b = a + 1; b < ids.size(); ++b) {
                    const Stop &sa = stops[static_cast<size_t>(ids[a])], &sb = stops[static_cast<size_t>(ids[b])];
                    if (std::isfinite(sa.lat) && std::isfinite(sb.lat))
                        maxD = std::max(maxD, haversineMeters(sa.lat, sa.lon, sb.lat, sb.lon));
                }
            out << "  \"" << stops[static_cast<size_t>(ids[0])].name << "\"  max distance apart " << std::setprecision(0)
                << maxD << " m\n";
            for (int s : ids) {
                std::vector<std::string> rl(familiesServing[static_cast<size_t>(s)].begin(),
                                            familiesServing[static_cast<size_t>(s)].end());
                out << "      stop_id " << stops[static_cast<size_t>(s)].id << "  lines: "
                    << (rl.empty() ? "(none)" : joinStrings(rl, ", ")) << "\n";
            }
        }
        more(groups.size());

        out << "B) Single stop_ids served by 2+ LINES (colour families, not directional routes): " << st.sharedStops << "\n";
        size_t shown = 0;
        for (size_t s = 0; s < stops.size() && shown < limit; ++s) {
            if (familiesServing[s].size() < 2) continue;
            ++shown;
            std::vector<std::string> rl(familiesServing[s].begin(), familiesServing[s].end());
            out << "  " << stops[s].id << " | " << stops[s].name << " | lines: " << joinStrings(rl, ", ") << "\n";
        }
        more(static_cast<size_t>(st.sharedStops));

        CsvReader tr(path("transfers.txt"));
        out << "C) transfers.txt: ";
        if (!tr.ok()) {
            out << "absent (walking times between lines would have to be estimated)\n";
        } else {
            int cType = tr.column("transfer_type"), cMin = tr.column("min_transfer_time");
            std::vector<std::string> row;
            long long n = 0;
            std::vector<double> mins;
            while (tr.next(row)) {
                ++n;
                double m;
                if (parseDouble(CsvReader::get(row, cMin), m)) mins.push_back(m);
            }
            out << n << " rows, transfer_type column " << (cType >= 0 ? "present" : "absent")
                << ", min_transfer_time values: " << mins.size();
            if (!mins.empty()) out << " (median " << medianOf(mins) << " s)";
            out << "\n";
        }
    }

    heading("8. FEED VALIDITY (how fresh is this data?)");
    {
        CsvReader cal(path("calendar.txt"));
        if (cal.ok()) {
            int cS = cal.column("start_date"), cE = cal.column("end_date");
            std::vector<std::string> row;
            long long n = 0;
            while (cal.next(row)) {
                ++n;
                std::string s = CsvReader::get(row, cS), e = CsvReader::get(row, cE);
                if (s.size() == 8 && (st.calendarMinStart.empty() || s < st.calendarMinStart)) st.calendarMinStart = s;
                if (e.size() == 8 && (st.calendarMaxEnd.empty() || e > st.calendarMaxEnd)) st.calendarMaxEnd = e;
            }
            out << "calendar.txt: " << n << " service patterns, valid from " << st.calendarMinStart << " to "
                << st.calendarMaxEnd << " (YYYYMMDD)\n";
        } else {
            out << "calendar.txt: absent\n";
        }
        CsvReader cd(path("calendar_dates.txt"));
        if (cd.ok()) {
            std::vector<std::string> row;
            long long n = 0;
            while (cd.next(row)) ++n;
            out << "calendar_dates.txt: " << n << " exception rows\n";
        }
        CsvReader fi(path("feed_info.txt"));
        if (fi.ok()) {
            std::vector<std::string> row;
            if (fi.next(row)) {
                out << "feed_info.txt:";
                for (size_t c = 0; c < fi.header().size(); ++c)
                    out << "  " << fi.header()[c] << "=" << CsvReader::get(row, static_cast<int>(c));
                out << "\n";
            }
        } else {
            out << "feed_info.txt: absent\n";
        }
        CsvReader ag(path("agency.txt"));
        if (ag.ok()) {
            std::vector<std::string> row;
            int cN = ag.column("agency_name"), cU = ag.column("agency_url");
            while (ag.next(row)) out << "agency: " << CsvReader::get(row, cN) << "  " << CsvReader::get(row, cU) << "\n";
        }
    }

    heading("9. KEY FINDINGS (paste this section back to your assistant)");
    out << "- Stops: " << stops.size() << " usable; routes: " << routes.size() << "; trips: " << st.tripsRows
        << "; stop_times rows: " << st.stopTimesRows << ".\n";
    out << "- Graph size: " << st.undirectedPairs << " undirected edges between consecutive stops.\n";
    out << "- Interchanges: " << st.interchangeNameGroups << " names appear under several stop_ids; " << st.sharedStops
        << " single stops are served by 2+ lines; " << st.parentStationRows << " stops declare a parent_station.\n";
    if (st.sharedStops > 0 && st.interchangeNameGroups == 0)
        out << "  => Interchanges look like ONE shared stop served by several lines.\n";
    else if (st.sharedStops == 0 && st.interchangeNameGroups > 0)
        out << "  => Interchanges look like SEPARATE stops per line with the same name; the importer must link them.\n";
    else if (st.sharedStops > 0 && st.interchangeNameGroups > 0)
        out << "  => MIXED: some interchanges are shared stops, others are separate stops. Needs careful handling.\n";
    else
        out << "  => No interchanges detected by name or by shared stop. Check section 7 and the data.\n";
    out << "- Distances: shape_dist_traveled " << (st.hasShapeDistColumn ? "present" : "ABSENT") << " ("
        << st.shapeDistRows << " rows with values). See section 6 for units.\n";
    out << "- Line direction: trip_headsign " << (st.hasHeadsign ? "present" : "ABSENT") << ", non-empty on "
        << headsignRows << " trips.\n";
    out << "- Run times: " << runs.size() << " usable hops, " << st.nonPositiveRuns << " zero/negative, " << hopsMissingTimes
        << " missing; " << st.variablePairs << " pairs vary by >60 s.\n";
    out << "- Data quality: " << st.orphanStopTimeRows << " rows with unknown stops, " << st.orphanTripRows
        << " rows with unknown trips, " << st.unusedStops << " unused stops, " << st.invalidCoords
        << " stops with bad coordinates.\n";
    out << "- Validity: " << (st.calendarMinStart.empty() ? "unknown" : st.calendarMinStart) << " to "
        << (st.calendarMaxEnd.empty() ? "unknown" : st.calendarMaxEnd) << ".\n";

    if (statsOut) *statsOut = st;
    return out.str();
}

}  // namespace gtfs
}  // namespace metro
