// metro_route: find routes (and fares) in the metro network.
//   metro_route --from "Dilshad Garden" --to "Hauz Khas" [--mode both|fastest|fewest] [--day weekday|sunday] [--stops]
//   metro_route --search "rajiv"
//   metro_route --verify
//   metro_route --batch pairs.csv --out results.csv
//   metro_route --dump-costs costs.csv
//   metro_route --dump-fares fares.csv
// All commands accept --db FILE (default data/metro.db).
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../common/csv.h"
#include "../common/db.h"
#include "../route/fare.h"
#include "../route/network.h"
#include "../route/planner.h"
#include "../route/present.h"
#include "../route/router.h"
#include "../route/verify.h"

using namespace metro;
using namespace metro::route;

namespace {

void usage() {
    std::cout << "Usage:\n"
                 "  metro_route --from NAME --to NAME [--prefer fastest|cheapest|fewest] [--day weekday|sunday] [--stops]\n"
                 "       (--mode fastest|fewest shows just one route, with no alternatives)\n"
                 "  metro_route --search TEXT          find stations by name\n"
                 "  metro_route --verify               check routes and fare distances against independent algorithms\n"
                 "  metro_route --batch FILE --out FILE   run many trips (CSV with columns from,to)\n"
                 "  metro_route --dump-costs FILE      write every pair's route cost (for cross-checking)\n"
                 "  metro_route --dump-fares FILE      write every pair's fare distance and fare (for cross-checking)\n"
                 "Common option: --db FILE (default data/metro.db)\n"
                 "Station names ignore case and punctuation. Use id:<number> to name a station by its GTFS id.\n";
}

std::string describeStation(const Network& n, int id) { return n.station(id).name + " (id:" + n.station(id).gtfsId + ")"; }

// Returns the station id, or 0 after printing why not.
int resolveOrExplain(const Network& n, const std::string& what, const std::string& query) {
    Resolution r = resolveStation(n, query);
    if (r.status == Resolution::Status::Found) return r.station;
    if (r.status == Resolution::Status::Ambiguous) {
        std::cerr << "'" << query << "' (" << what << ") matches several stations. Be more specific, or use id:<number>:\n";
        for (size_t i = 0; i < r.candidates.size() && i < 12; ++i) std::cerr << "    " << describeStation(n, r.candidates[i]) << "\n";
        if (r.candidates.size() > 12) std::cerr << "    ... and " << r.candidates.size() - 12 << " more\n";
    } else {
        std::cerr << "No station matches '" << query << "' (" << what << ").";
        if (!r.candidates.empty()) {
            std::cerr << " Did you mean:\n";
            for (int c : r.candidates) std::cerr << "    " << describeStation(n, c) << "\n";
        } else {
            std::cerr << "\n";
        }
    }
    return 0;
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) out += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

void writeRow(std::ostream& f, const std::vector<std::string>& fields) {
    for (size_t i = 0; i < fields.size(); ++i) f << (i ? "," : "") << csvField(fields[i]);
    f << "\n";
}

std::string serviceList(const Network& n, const Route& r) {
    std::string out;
    for (const auto& s : r.steps)
        if (s.kind == Step::Kind::Ride) out += (out.empty() ? "" : " > ") + n.service(s.service).name;
    return out;
}

std::string fareText(const FareQuote& q) { return q.available ? std::to_string(q.fareInr) : std::string(); }

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string from, to, mode = "both", dbPath = "data/metro.db", search, batch, out, dump, dumpFares, dayName = "weekday", preferName = "fastest";
    bool stops = false, verify = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--from" && hasValue) from = args[++i];
        else if (a == "--to" && hasValue) to = args[++i];
        else if (a == "--mode" && hasValue) mode = args[++i];
        else if (a == "--day" && hasValue) dayName = args[++i];
        else if (a == "--prefer" && hasValue) preferName = args[++i];
        else if (a == "--db" && hasValue) dbPath = args[++i];
        else if (a == "--search" && hasValue) search = args[++i];
        else if (a == "--batch" && hasValue) batch = args[++i];
        else if (a == "--out" && hasValue) out = args[++i];
        else if (a == "--dump-costs" && hasValue) dump = args[++i];
        else if (a == "--dump-fares" && hasValue) dumpFares = args[++i];
        else if (a == "--stops") stops = true;
        else if (a == "--verify") verify = true;
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << a << "\n\n";
            usage();
            return 2;
        }
    }
    const bool wantsRoute = !from.empty() || !to.empty();
    if (!wantsRoute && search.empty() && batch.empty() && dump.empty() && dumpFares.empty() && !verify) {
        usage();
        return 2;
    }
    if (mode != "both" && mode != "fastest" && mode != "fewest") {
        std::cerr << "--mode must be both, fastest or fewest\n";
        return 2;
    }
    if (dayName != "weekday" && dayName != "sunday") {
        std::cerr << "--day must be weekday or sunday (sunday also covers national holidays)\n";
        return 2;
    }
    Preference prefer = Preference::Fastest;
    if (!parsePreference(preferName, prefer)) {
        std::cerr << "--prefer must be fastest, cheapest or fewest\n";
        return 2;
    }
    const DayType day = dayName == "weekday" ? DayType::Weekday : DayType::SundayHoliday;

    try {
        Database db(dbPath, false);
        Network net = loadNetwork(db);

        if (!search.empty()) {
            Resolution r = resolveStation(net, search);
            if (r.status == Resolution::Status::Found) std::cout << describeStation(net, r.station) << "\n";
            else {
                std::cout << (r.status == Resolution::Status::Ambiguous ? "Several stations match:\n" : "No exact match. Closest names:\n");
                for (int c : r.candidates) std::cout << "  " << describeStation(net, c) << "\n";
            }
            return r.status == Resolution::Status::NotFound && r.candidates.empty() ? 1 : 0;
        }

        if (verify) {
            VerifyReport rep = verifyAllPairs(net);
            FareVerifyReport fr = verifyFares(net);
            std::cout << "VERIFY: " << net.stations.size() << " stations, " << net.services.size() << " services, data_version " << net.dataVersion << "\n"
                      << "Checked " << rep.pairs << " (pair, mode) combinations against a separately written Bellman-Ford solver,\n"
                      << "and re-computed every route's time and changes straight from the database tables.\n\n"
                      << "  router vs independent solver      : " << rep.costMismatches << "\n"
                      << "  find() vs single-origin mismatches: " << rep.findMismatches << "\n"
                      << "  routes failing independent check  : " << rep.invalidRoutes << "\n"
                      << "  unreachable pairs                 : " << rep.unreachable << "\n"
                      << "  longest fastest journey           : " << rep.maxSeconds / 60 << " min\n"
                      << "  most changes on any route         : " << rep.maxChanges << "\n"
                      << "  fastest routes by number of changes:";
            for (size_t c = 0; c < rep.changeHistogram.size(); ++c) std::cout << "  " << c << ":" << rep.changeHistogram[c];
            std::cout << "\n\nFare distances: " << fr.pairs << " station pairs checked against an independent Floyd-Warshall solver\n"
                      << "  fare distance mismatches          : " << fr.mismatches << "\n";
            for (const auto& p : rep.problems) std::cout << "  PROBLEM: " << p << "\n";
            for (const auto& p : fr.problems) std::cout << "  PROBLEM: " << p << "\n";
            const bool ok = rep.ok() && fr.ok();
            std::cout << "\n" << (ok ? "RESULT: all checks passed." : "RESULT: PROBLEMS FOUND.") << "\n";
            return ok ? 0 : 1;
        }

        Router router(net);
        FareCalculator fares(net);
        Planner planner(net, router, fares);

        if (!dump.empty()) {
            std::ofstream f(dump, std::ios::binary);
            if (!f) {
                std::cerr << "Could not write " << dump << "\n";
                return 1;
            }
            f << "mode,from_gtfs_id,to_gtfs_id,seconds,changes\n";
            for (Mode m : {Mode::Fastest, Mode::FewestChanges})
                for (int a = 1; a <= static_cast<int>(net.stations.size()); ++a) {
                    auto costs = router.costsFrom(a, m);
                    for (int b = 1; b <= static_cast<int>(net.stations.size()); ++b)
                        if (a != b)
                            f << (m == Mode::Fastest ? "fastest" : "fewest") << "," << csvField(net.station(a).gtfsId) << ","
                              << csvField(net.station(b).gtfsId) << "," << costs[static_cast<size_t>(b)].seconds << ","
                              << costs[static_cast<size_t>(b)].changes << "\n";
                }
            std::cout << "Wrote " << dump << "\n";
            return 0;
        }

        if (!dumpFares.empty()) {
            std::ofstream f(dumpFares, std::ios::binary);
            if (!f) {
                std::cerr << "Could not write " << dumpFares << "\n";
                return 1;
            }
            f << "operator,from_gtfs_id,to_gtfs_id,distance_m,fare_weekday,fare_sunday\n";
            long long rows = 0;
            for (const std::string& op : fares.operatorsWithSlabs())
                for (int a = 1; a <= static_cast<int>(net.stations.size()); ++a) {
                    const std::vector<int> d = fares.distancesFrom(a, op);
                    for (int b = 1; b <= static_cast<int>(net.stations.size()); ++b) {
                        if (a == b || d[static_cast<size_t>(b)] <= 0) continue;
                        writeRow(f, {op, net.station(a).gtfsId, net.station(b).gtfsId, std::to_string(d[static_cast<size_t>(b)]),
                                     std::to_string(fares.fareForDistance(op, DayType::Weekday, d[static_cast<size_t>(b)])),
                                     std::to_string(fares.fareForDistance(op, DayType::SundayHoliday, d[static_cast<size_t>(b)]))});
                        ++rows;
                    }
                }
            std::cout << "Wrote " << dumpFares << " (" << rows << " rows)\n";
            return 0;
        }

        if (!batch.empty()) {
            if (out.empty()) {
                std::cerr << "--batch needs --out FILE\n";
                return 2;
            }
            CsvReader in(batch);
            if (!in.ok() || in.column("from") < 0 || in.column("to") < 0) {
                std::cerr << "Cannot read " << batch << " (it needs a header row with the columns: from,to)\n";
                return 1;
            }
            std::ofstream f(out, std::ios::binary);
            if (!f) {
                std::cerr << "Could not write " << out << "\n";
                return 1;
            }
            writeRow(f, {"from", "to", "fastest_seconds", "fastest_changes", "fewest_seconds", "fewest_changes", "fewest_differs", "fastest_services",
                         "fastest_fare_inr", "fewest_fare_inr", "fare_distance_m", "fare_note", "error"});
            std::vector<std::string> row;
            int done = 0, failed = 0;
            while (in.next(row)) {
                const std::string a = CsvReader::get(row, in.column("from")), b = CsvReader::get(row, in.column("to"));
                Resolution ra = resolveStation(net, a), rb = resolveStation(net, b);
                if (ra.status != Resolution::Status::Found || rb.status != Resolution::Status::Found || ra.station == rb.station) {
                    writeRow(f, {a, b, "", "", "", "", "", "", "", "", "", "", "could not resolve the stations or they are the same"});
                    ++failed;
                    continue;
                }
                Options o = router.plan(ra.station, rb.station);
                const std::string fa = net.station(ra.station).name, fb = net.station(rb.station).name;
                if (!o.fastest.found) {
                    writeRow(f, {fa, fb, "", "", "", "", "", "", "", "", "", "", "no route"});
                    ++failed;
                    continue;
                }
                const Route& fw = o.fewestDiffers ? o.fewest : o.fastest;
                const FareQuote qf = fares.quote(o.fastest, day), qw = fares.quote(fw, day);
                writeRow(f, {fa, fb, std::to_string(o.fastest.totalSeconds), std::to_string(o.fastest.changes), std::to_string(fw.totalSeconds),
                             std::to_string(fw.changes), o.fewestDiffers ? "yes" : "no", serviceList(net, o.fastest), fareText(qf), fareText(qw),
                             qf.available ? std::to_string(qf.distanceM) : std::string(), qf.available ? std::string() : qf.reason, ""});
                ++done;
            }
            std::cout << "Wrote " << out << ": " << done << " trips, " << failed << " could not be run (fares are for " << dayName << ", token).\n";
            return failed == 0 ? 0 : 1;
        }

        if (from.empty() || to.empty()) {
            std::cerr << "Both --from and --to are needed.\n";
            return 2;
        }
        int a = resolveOrExplain(net, "from", from);
        int b = resolveOrExplain(net, "to", to);
        if (a == 0 || b == 0) return 1;
        if (a == b) {
            std::cerr << "The start and the destination are the same station.\n";
            return 1;
        }
        std::cout << net.station(a).name << "  ->  " << net.station(b).name << "\n\n";
        if (mode == "both") {
            std::cout << formatPlanResult(net, planner.plan(a, b, day, prefer), stops, &fares, day);
        } else {
            Mode m = mode == "fastest" ? Mode::Fastest : Mode::FewestChanges;
            Route r = router.find(a, b, m);
            std::cout << formatRoute(net, r, mode == "fastest" ? "FASTEST" : "FEWEST CHANGES", stops, &fares, day);
            std::cout << "\nTimes are estimates from the feed. They do not include waiting for trains.\n";
            FareQuote q = fares.quote(r, day);
            if (q.available && !q.verified)
                std::cout << "Fare slabs come from news reports of DMRC's announcement and have not been checked against DMRC's own page.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
