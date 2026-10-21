#include <fstream>
#include <random>
#include <sstream>

#include "../src/common/db.h"
#include "../src/import/importer.h"
#include "../src/route/fare.h"
#include "../src/route/network.h"
#include "../src/route/present.h"
#include "../src/route/router.h"
#include "../src/route/verify.h"
#include "check.h"

using namespace metro;
using namespace metro::route;

static Network makeNetwork(int stations) {
    Network n;
    for (int i = 1; i <= stations; ++i) {
        Station s;
        s.id = i;
        s.gtfsId = std::to_string(i);
        s.name = "S" + std::to_string(i);
        n.stations.push_back(s);
    }
    n.dwellSeconds = 20;
    return n;
}

static void addService(Network& n, const std::string& name, const std::string& op, std::vector<int> stations, int run, int distance) {
    Service s;
    s.id = static_cast<int>(n.services.size()) + 1;
    s.family = "X";
    s.name = name;
    s.op = op;
    s.stations = stations;
    n.services.push_back(s);
    for (size_t i = 0; i + 1 < stations.size(); ++i) n.segments.push_back({s.id, stations[i], stations[i + 1], run, distance});
}

static void addSlab(Network& n, const std::string& op, const std::string& day, double lo, double hi, int fare, const std::string& source = "test") {
    FareSlab f;
    f.op = op;
    f.dayType = day;
    f.ticket = "token";
    f.minKm = lo;
    f.maxKm = hi;
    f.fareInr = fare;
    f.validFrom = "2025-01-01";
    f.source = source;
    n.fareSlabs.push_back(f);
}

static void addStandardSlabs(Network& n, const std::string& op) {
    addSlab(n, op, "weekday", 0, 2, 10);
    addSlab(n, op, "weekday", 2, 5, 20);
    addSlab(n, op, "weekday", 5, 1000, 30);
    addSlab(n, op, "sunday_holiday", 0, 2, 5, "UNVERIFIED test");
    addSlab(n, op, "sunday_holiday", 2, 5, 10, "UNVERIFIED test");
    addSlab(n, op, "sunday_holiday", 5, 1000, 15, "UNVERIFIED test");
}

static void testSlabBoundaries() {
    Network n = makeNetwork(2);
    addService(n, "A", "OP", {1, 2}, 100, 1000);
    addStandardSlabs(n, "OP");
    FareCalculator f(n);
    // a slab covers distances ABOVE its minimum up to AND INCLUDING its maximum
    CHECK_EQ(f.fareForDistance("OP", DayType::Weekday, 1), 10);
    CHECK_EQ(f.fareForDistance("OP", DayType::Weekday, 2000), 10);    // exactly 2 km: the lower slab
    CHECK_EQ(f.fareForDistance("OP", DayType::Weekday, 2001), 20);
    CHECK_EQ(f.fareForDistance("OP", DayType::Weekday, 5000), 20);
    CHECK_EQ(f.fareForDistance("OP", DayType::Weekday, 5001), 30);
    CHECK_EQ(f.fareForDistance("OP", DayType::SundayHoliday, 2001), 10);
    CHECK_EQ(f.fareForDistance("OP", DayType::Weekday, 0), -1);       // no distance, no fare
    CHECK_EQ(f.fareForDistance("OP", DayType::Weekday, 2000000), -1); // beyond every slab
    CHECK_EQ(f.fareForDistance("OTHER", DayType::Weekday, 1000), -1); // unknown operator
}

// The fare uses the SHORTEST distance, not the distance of the route the traveller takes.
static void testFareIgnoresTheRouteTaken() {
    Network n = makeNetwork(3);
    addService(n, "Slow", "OP", {1, 2, 3}, 300, 1000);      // service 1: 1-2-3, 2000 m in total, slow
    addService(n, "Express", "OP", {1, 3}, 60, 6000);       // service 2: direct but a long 6000 m, fast
    addStandardSlabs(n, "OP");
    Router r(n);
    FareCalculator f(n);
    Route fastest = r.find(1, 3, Mode::Fastest);
    CHECK_EQ(fastest.steps.size(), static_cast<size_t>(1));
    CHECK_EQ(fastest.steps[0].service, 2);                  // the fast route is the long-distance one
    FareQuote q = f.quote(fastest, DayType::Weekday);
    CHECK(q.available);
    CHECK_EQ(q.distanceM, 2000);                            // but the fare distance is the shortest, 2000 m
    CHECK_EQ(q.fareInr, 10);                                // so it is the 0-2 km fare, not the 5+ km fare
    CHECK(q.verified);                                      // the source "test" is not marked UNVERIFIED
    Route fewest = r.find(1, 3, Mode::FewestChanges);
    CHECK_EQ(f.quote(fewest, DayType::Weekday).fareInr, 10);   // same trip, same fare, whichever route
}

static void testWhenAFareIsNotComputed() {
    Network n = makeNetwork(7);
    addService(n, "A", "OP_A", {1, 2}, 100, 1000);          // service 1
    addService(n, "B", "OP_B", {3, 4}, 100, 1000);          // service 2: operator without slabs
    addService(n, "C", "OP_A", {5, 6}, 100, 0);             // service 3: unknown distances
    addService(n, "D", "OP_A", {2, 7}, 100, 1000);          // service 4
    n.transfers.push_back({2, 1, 3, 2, 300});               // a walking link between OP_A (station 2) and OP_B (station 3)
    n.transfers.push_back({3, 2, 2, 1, 300});
    n.transfers.push_back({2, 1, 2, 4, 60});                // change between two OP_A services at station 2
    n.transfers.push_back({2, 4, 2, 1, 60});
    addStandardSlabs(n, "OP_A");
    Router r(n);
    FareCalculator f(n);

    FareQuote ok = f.quote(r.find(1, 7, Mode::Fastest), DayType::Weekday);        // A then D, both OP_A
    CHECK(ok.available);
    CHECK_EQ(ok.distanceM, 2000);
    CHECK_EQ(ok.fareInr, 10);

    FareQuote noRule = f.quote(r.find(3, 4, Mode::Fastest), DayType::Weekday);    // operator with no slabs
    CHECK(!noRule.available);
    CHECK(noRule.reason.find("OP_B") != std::string::npos);

    FareQuote mixed = f.quote(r.find(1, 4, Mode::Fastest), DayType::Weekday);     // OP_A then OP_B
    CHECK(!mixed.available);
    CHECK(mixed.reason.find("mixes operators") != std::string::npos);

    FareQuote walk = f.quote(r.find(2, 3, Mode::Fastest), DayType::Weekday);      // only a walk
    CHECK(!walk.available);
    CHECK(walk.reason.find("walk") != std::string::npos);

    FareQuote unknown = f.quote(r.find(5, 6, Mode::Fastest), DayType::Weekday);   // distances are unknown
    CHECK(!unknown.available);
    CHECK(unknown.reason.find("unknown") != std::string::npos);

    FareQuote none = f.quote(Route(), DayType::Weekday);                          // no route
    CHECK(!none.available);

    std::string line = formatFare(noRule, DayType::Weekday);
    CHECK(line.find("not computed") != std::string::npos);
    CHECK(formatFare(ok, DayType::Weekday).find("Rs 10") != std::string::npos);
    CHECK(formatFare(ok, DayType::Weekday).find("2.0 km") != std::string::npos);
}

static void addPair(Network& n, const std::string& op, const std::string& day, const std::string& a, const std::string& b, int fare) {
    FarePair f;
    f.op = op;
    f.dayType = day;
    f.ticket = "token";
    f.stopA = a < b ? a : b;
    f.stopB = a < b ? b : a;
    f.fareInr = fare;
    f.validFrom = "2025";
    f.source = "test";
    n.farePairs.push_back(f);
}

// Operators that publish a fare for each pair of stations (the Airport Express).
static void testPairFares() {
    Network n = makeNetwork(4);
    addService(n, "Express", "AEL", {1, 2, 3}, 100, 5000);     // service 1: stations 1-2-3
    addService(n, "Other", "OP", {3, 4}, 100, 1000);           // service 2
    addStandardSlabs(n, "OP");
    addPair(n, "AEL", "any", "1", "2", 21);
    addPair(n, "AEL", "any", "2", "3", 32);
    addPair(n, "AEL", "any", "3", "1", 64);                    // listed the other way round: still found
    addPair(n, "AEL", "sunday_holiday", "1", "2", 15);         // a Sunday-only fare in addition to the 'any' one
    n.transfers.push_back({3, 1, 3, 2, 60});
    n.transfers.push_back({3, 2, 3, 1, 60});
    Router r(n);
    FareCalculator f(n);

    FareQuote q = f.quote(r.find(1, 2, Mode::Fastest), DayType::Weekday);
    CHECK(q.available);
    CHECK_EQ(q.fareInr, 21);
    CHECK_EQ(q.basis, std::string("pair table"));
    CHECK_EQ(q.distanceM, 0);
    CHECK_EQ(f.quote(r.find(2, 1, Mode::Fastest), DayType::Weekday).fareInr, 21);          // either direction
    CHECK_EQ(f.quote(r.find(1, 3, Mode::Fastest), DayType::Weekday).fareInr, 64);          // the pair listed as 3,1
    CHECK_EQ(f.quote(r.find(1, 2, Mode::Fastest), DayType::SundayHoliday).fareInr, 21);    // 'any' also covers Sundays
    CHECK(!f.quote(r.find(1, 4, Mode::Fastest), DayType::Weekday).available);              // AEL then OP: mixed operators
    FareQuote same = f.quote(r.find(3, 4, Mode::Fastest), DayType::Weekday);               // OP has slabs only
    CHECK(same.available);
    CHECK_EQ(same.basis, std::string("distance"));

    Network gap = n;
    gap.farePairs.pop_back();                                                              // drop the Sunday row
    gap.farePairs.erase(gap.farePairs.begin() + 2);                                        // drop the 3-1 pair
    Router r2(gap);
    FareCalculator f2(gap);
    FareQuote missing = f2.quote(r2.find(1, 3, Mode::Fastest), DayType::Weekday);
    CHECK(!missing.available);
    CHECK(missing.reason.find("no AEL fare is listed") != std::string::npos);
    std::string line = formatFare(q, DayType::Weekday);
    CHECK(line.find("Rs 21") != std::string::npos);
    CHECK(line.find("fare table") != std::string::npos);
}

static void testUnverifiedFlag() {
    Network n = makeNetwork(2);
    addService(n, "A", "OP", {1, 2}, 100, 1000);
    addSlab(n, "OP", "weekday", 0, 100, 10, "UNVERIFIED - from the news");
    Router r(n);
    FareCalculator f(n);
    FareQuote q = f.quote(r.find(1, 2, Mode::Fastest), DayType::Weekday);
    CHECK(q.available);
    CHECK(!q.verified);
    CHECK(q.source.find("news") != std::string::npos);
    std::string text = formatPlan(n, r.plan(1, 2), false, &f, DayType::Weekday);
    CHECK(text.find("not been checked") != std::string::npos);     // the plan warns about unverified slabs
}

// Random networks: fare distances from Bellman-Ford must equal an independent Floyd-Warshall everywhere.
static void testRandomFareDistances() {
    for (unsigned seed = 1; seed <= 200; ++seed) {
        std::mt19937 rng(seed);
        auto pick = [&](unsigned k) { return static_cast<int>(rng() % k); };
        const int stations = 6 + pick(8);
        Network n = makeNetwork(stations);
        for (int s = 0; s < 2 + pick(4); ++s) {
            std::vector<int> all;
            for (int i = 1; i <= stations; ++i) all.push_back(i);
            for (size_t i = all.size(); i > 1; --i) std::swap(all[i - 1], all[static_cast<size_t>(pick(static_cast<unsigned>(i)))]);
            const int len = 2 + pick(static_cast<unsigned>(std::min(6, stations - 1)));
            addService(n, "L" + std::to_string(s), pick(2) ? "OP_A" : "OP_B", std::vector<int>(all.begin(), all.begin() + len), 60, 1000);
        }
        for (auto& g : n.segments) g.distanceM = pick(5) == 0 ? 0 : 300 + pick(4000);   // some distances unknown
        addStandardSlabs(n, "OP_A");
        addStandardSlabs(n, "OP_B");
        FareVerifyReport rep = verifyFares(n);
        if (!rep.ok()) {
            std::cerr << "random fare network seed " << seed << " failed\n";
            for (const auto& p : rep.problems) std::cerr << "  " << p << "\n";
        }
        CHECK(rep.ok());
        CHECK(rep.pairs > 0);
    }
}

// The whole pipeline: config files -> importer -> database -> fares.
static void testFromImportedDatabase(const std::string& schemaPath, const std::string& gtfs, const std::string& config) {
    std::ifstream sf(schemaPath, std::ios::binary);
    std::stringstream ss;
    ss << sf.rdbuf();
    Database db(":memory:", true);
    db.exec(ss.str());
    import::importInto(db, import::buildModel(gtfs, config));
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM fare_slabs;"), 6LL);
    Network n = loadNetwork(db);
    CHECK_EQ(n.fareSlabs.size(), static_cast<size_t>(6));
    Router r(n);
    FareCalculator f(n);

    // Station ids: A=1 B=2 C=3 D=4 E=5 F=6 J=7 K=8 L=9 M=10 G1..G5=11..15 S1=16 S2=17. Hops are 1000 m except B-C 1500 m.
    auto fare = [&](int a, int b, DayType d) { return f.quote(r.find(a, b, Mode::Fastest), d); };
    FareQuote ae = fare(1, 5, DayType::Weekday);                  // A to E on the Red Line: 1000+1500+1000+1000 = 4500 m
    CHECK(ae.available);
    CHECK_EQ(ae.distanceM, 4500);
    CHECK_EQ(ae.fareInr, 20);
    CHECK_EQ(fare(1, 5, DayType::SundayHoliday).fareInr, 10);
    FareQuote ce = fare(3, 5, DayType::Weekday);                  // C to E: exactly 2000 m is the lower slab
    CHECK_EQ(ce.distanceM, 2000);
    CHECK_EQ(ce.fareInr, 10);
    FareQuote al = fare(1, 9, DayType::Weekday);                  // A to L: Red then Blue, both OP_A: 6500 m
    CHECK_EQ(al.distanceM, 6500);
    CHECK_EQ(al.fareInr, 30);
    CHECK(!fare(1, 17, DayType::Weekday).available);              // Red (OP_A) then Rapid (OP_C): mixed
    CHECK(!fare(11, 15, DayType::Weekday).available);             // Green is OP_B: no slabs loaded
    FareQuote rapid = fare(16, 17, DayType::Weekday);             // Rapid (OP_C) has one pair fare, listed as S2,S1 in the file
    CHECK(rapid.available);
    CHECK_EQ(rapid.fareInr, 25);
    CHECK_EQ(rapid.basis, std::string("pair table"));
    CHECK_EQ(fare(17, 16, DayType::SundayHoliday).fareInr, 25);
    CHECK(ae.verified);                                           // source "test data" is not marked UNVERIFIED
    CHECK(!fare(1, 5, DayType::SundayHoliday).verified);          // the Sunday rows are marked UNVERIFIED

    FareVerifyReport rep = verifyFares(n);
    CHECK(rep.ok());
    CHECK(rep.pairs > 0);
}

static int run(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_fare <schema.sql> <import_gtfs> <import_config>\n";
        return 2;
    }
    testSlabBoundaries();
    testFareIgnoresTheRouteTaken();
    testWhenAFareIsNotComputed();
    testPairFares();
    testUnverifiedFlag();
    testFromImportedDatabase(argv[1], argv[2], argv[3]);
    testRandomFareDistances();
    return finish("test_fare");
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "test_fare: unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
