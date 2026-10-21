#include <random>

#include "../src/common/db.h"
#include "../src/import/importer.h"
#include "../src/route/network.h"
#include "../src/route/present.h"
#include "../src/route/router.h"
#include "../src/route/verify.h"
#include "check.h"

#include <fstream>
#include <sstream>

using namespace metro;
using namespace metro::route;

static Network makeNetwork(int stations, int dwell) {
    Network n;
    for (int i = 1; i <= stations; ++i) {
        Station s;
        s.id = i;
        s.gtfsId = std::to_string(i);
        s.name = "S" + std::to_string(i);
        n.stations.push_back(s);
    }
    n.dwellSeconds = dwell;
    return n;
}

static void addService(Network& n, const std::string& name, std::vector<int> stations, int run) {
    Service s;
    s.id = static_cast<int>(n.services.size()) + 1;
    s.family = "X";
    s.name = name;
    s.op = "OP";
    s.stations = stations;
    n.services.push_back(s);
    for (size_t i = 0; i + 1 < stations.size(); ++i) n.segments.push_back({s.id, stations[i], stations[i + 1], run, 1000});
}

static void addTransfer(Network& n, int st1, int sv1, int st2, int sv2, int walk) {
    n.transfers.push_back({st1, sv1, st2, sv2, walk});
}

static void compareSingleAndAllDestinations(const Network& n, const std::string& label);   // defined further down

static void checkValid(const Network& n, const Route& r) {
    std::string problem = validateRoute(n, r);
    if (!problem.empty()) std::cerr << "route invalid: " << problem << "\n";
    CHECK(problem.empty());
}

// ---- small hand-computed networks -------------------------------------------------------------
static void testFastVsFewest() {
    Network n = makeNetwork(4, 20);
    addService(n, "Direct", {1, 4}, 1000);   // service 1
    n.segments.back().runSeconds = 1000;
    addService(n, "Fast1", {1, 2}, 100);     // service 2
    addService(n, "Fast2", {2, 4}, 100);     // service 3
    addTransfer(n, 2, 2, 2, 3, 60);
    addTransfer(n, 2, 3, 2, 2, 60);
    Router r(n);

    Options o = r.plan(1, 4);
    CHECK(o.fastest.found);
    CHECK_EQ(o.fastest.totalSeconds, 260);          // 100 + 60 walk + 100
    CHECK_EQ(o.fastest.changes, 1);
    CHECK_EQ(o.fastest.steps.size(), static_cast<size_t>(3));
    CHECK(o.fastest.steps[0].kind == Step::Kind::Ride);
    CHECK(o.fastest.steps[1].kind == Step::Kind::Change);
    CHECK_EQ(o.fastest.steps[1].seconds, 60);
    CHECK(o.fastest.steps[2].kind == Step::Kind::Ride);
    CHECK(o.fewestDiffers);
    CHECK_EQ(o.fewest.totalSeconds, 1000);
    CHECK_EQ(o.fewest.changes, 0);
    CHECK_EQ(o.extraSeconds, 740);
    checkValid(n, o.fastest);
    checkValid(n, o.fewest);

    std::string text = formatPlan(n, o, false);
    CHECK(text.find("FASTEST") != std::string::npos);
    CHECK(text.find("FEWEST CHANGES") != std::string::npos);
    CHECK(text.find("Change at S2") != std::string::npos);
    CHECK(text.find("slower") != std::string::npos);

    Options same = r.plan(1, 2);                     // one service does it: fastest is also fewest
    CHECK(!same.fewestDiffers);
    CHECK_EQ(same.fastest.totalSeconds, 100);
}

static void testStopTimesAreNotSkipped() {
    Network n = makeNetwork(3, 20);
    addService(n, "Z", {1, 2, 3}, 100);
    Router r(n);
    Route a = r.find(1, 3, Mode::Fastest);
    CHECK_EQ(a.totalSeconds, 220);                   // 100 + 20 stop time at station 2 + 100. NOT 200.
    CHECK_EQ(a.changes, 0);
    CHECK_EQ(a.stops, 2);
    CHECK_EQ(a.steps.size(), static_cast<size_t>(1));
    CHECK_EQ(a.steps[0].via.size(), static_cast<size_t>(1));
    CHECK_EQ(a.steps[0].via[0], 2);
    CHECK_EQ(a.steps[0].towardsStation, 3);
    Route b = r.find(3, 1, Mode::Fastest);
    CHECK_EQ(b.totalSeconds, 220);
    CHECK_EQ(b.steps[0].towardsStation, 1);          // direction of travel decides the terminus
    checkValid(n, a);
    checkValid(n, b);
}

static void testWalksAtStartAndEndAreNotChanges() {
    Network n = makeNetwork(6, 20);
    addService(n, "A", {1, 2}, 100);                 // service 1
    addService(n, "B", {3, 4}, 100);                 // service 2
    addService(n, "C", {5, 6}, 100);                 // service 3: unconnected to the rest
    addTransfer(n, 2, 1, 3, 2, 300);                 // a confirmed link between stations 2 and 3
    addTransfer(n, 3, 2, 2, 1, 300);
    Router r(n);

    Route change = r.find(1, 4, Mode::Fastest);      // ride A, walk, ride B
    CHECK_EQ(change.totalSeconds, 500);
    CHECK_EQ(change.changes, 1);
    Route finalWalk = r.find(1, 3, Mode::Fastest);   // ride A, then walk to the destination
    CHECK_EQ(finalWalk.totalSeconds, 400);
    CHECK_EQ(finalWalk.changes, 0);                  // walking to the end is not a change
    CHECK(finalWalk.steps.back().kind == Step::Kind::Walk);
    Route onlyWalk = r.find(2, 3, Mode::Fastest);
    CHECK_EQ(onlyWalk.totalSeconds, 300);
    CHECK_EQ(onlyWalk.changes, 0);
    CHECK_EQ(onlyWalk.steps.size(), static_cast<size_t>(1));
    CHECK(onlyWalk.steps[0].kind == Step::Kind::Walk);
    Route startWalk = r.find(2, 4, Mode::Fastest);   // walk first, then ride B
    CHECK_EQ(startWalk.totalSeconds, 400);
    CHECK_EQ(startWalk.changes, 0);
    CHECK(startWalk.steps[0].kind == Step::Kind::Walk);
    for (const Route* x : {&change, &finalWalk, &onlyWalk, &startWalk}) checkValid(n, *x);

    CHECK(!r.find(1, 5, Mode::Fastest).found);       // no way to get there
    CHECK(!r.find(1, 1, Mode::Fastest).found);       // same station
    CHECK_THROWS(r.find(1, 99, Mode::Fastest));      // unknown station
    CHECK(!r.plan(1, 5).fastest.found);
}

static void construct(const Network& n) {
    Router r(n);
    (void)r;
}

// A train cannot reverse mid-line. With incomplete transfer permissions, a model without direction would
// "ride one stop out and back" just to unlock a transfer. This network is built to tempt it.
static void testTrainsDoNotReverse() {
    Network n = makeNetwork(4, 20);
    addService(n, "A", {1, 2}, 100);      // service 1
    addService(n, "B", {2, 3}, 100);      // service 2
    addService(n, "C", {2, 4}, 100);      // service 3
    addTransfer(n, 2, 1, 2, 2, 60);       // A -> B is allowed
    addTransfer(n, 2, 2, 2, 3, 60);       // B -> C is allowed
    // but A -> C is not. The only way "1 to 4" would work is riding B out to 3 and back to 2.
    Router r(n);
    CHECK(!r.find(1, 4, Mode::Fastest).found);
    CHECK(!r.find(1, 4, Mode::FewestChanges).found);
    CHECK(r.find(1, 3, Mode::Fastest).found);   // the legitimate part still works
}

static void testBadNetworksAreRejected() {
    {
        Network n = makeNetwork(3, 20);
        addService(n, "Z", {1, 2, 3}, 100);
        n.segments.push_back({1, 1, 3, 50, 0});      // 1 and 3 are not neighbours
        CHECK_THROWS(construct(n));
    }
    {
        Network n = makeNetwork(3, 20);
        addService(n, "Z", {1, 2, 1}, 100);          // visits station 1 twice
        CHECK_THROWS(construct(n));
    }
    {
        Network n = makeNetwork(3, 20);
        addService(n, "Z", {1, 2}, 100);
        addTransfer(n, 3, 1, 3, 1, 10);              // station 3 is not on service 1
        CHECK_THROWS(construct(n));
    }
    {
        Network n = makeNetwork(3, 20);
        addService(n, "Z", {1, 2}, 100);
        n.segments[0].runSeconds = 0;                // a hop must take time
        CHECK_THROWS(construct(n));
    }
}

// ---- random networks against the independent oracle ------------------------------------------------
static void testRandomNetworks() {
    for (unsigned seed = 1; seed <= 300; ++seed) {
        std::mt19937 rng(seed);
        auto pick = [&](unsigned k) { return static_cast<int>(rng() % k); };   // portable, unlike std::uniform_int_distribution
        const int stations = 6 + pick(8);
        Network n = makeNetwork(stations, pick(41));
        const int services = 2 + pick(5);
        for (int s = 0; s < services; ++s) {
            std::vector<int> all;
            for (int i = 1; i <= stations; ++i) all.push_back(i);
            for (size_t i = all.size(); i > 1; --i) std::swap(all[i - 1], all[static_cast<size_t>(pick(static_cast<unsigned>(i)))]);
            const int len = 2 + pick(static_cast<unsigned>(std::min(6, stations - 1)));
            addService(n, "L" + std::to_string(s), std::vector<int>(all.begin(), all.begin() + len), 30);
        }
        for (auto& g : n.segments) g.runSeconds = 30 + pick(371);
        // same-station changes between any two services that share a station
        for (const auto& a : n.services)
            for (const auto& b : n.services) {
                if (a.id == b.id) continue;
                for (int st : a.stations)
                    for (int st2 : b.stations)
                        if (st == st2 && pick(4) != 0) addTransfer(n, st, a.id, st, b.id, pick(301));
            }
        // a few links between different stations
        for (int k = 0; k < 3; ++k) {
            const auto& a = n.services[static_cast<size_t>(pick(static_cast<unsigned>(services)))];
            const auto& b = n.services[static_cast<size_t>(pick(static_cast<unsigned>(services)))];
            int sa = a.stations[static_cast<size_t>(pick(static_cast<unsigned>(a.stations.size())))];
            int sb = b.stations[static_cast<size_t>(pick(static_cast<unsigned>(b.stations.size())))];
            if (sa == sb) continue;
            int walk = 60 + pick(600);
            addTransfer(n, sa, a.id, sb, b.id, walk);
            addTransfer(n, sb, b.id, sa, a.id, walk);
        }
        if (seed % 10 == 0) compareSingleAndAllDestinations(n, "random " + std::to_string(seed));
        VerifyReport rep = verifyAllPairs(n);
        if (!rep.ok()) {
            std::cerr << "random network with seed " << seed << " failed:\n";
            for (const auto& p : rep.problems) std::cerr << "  " << p << "\n";
        }
        CHECK(rep.ok());

        Router router(n);
        for (int a = 1; a <= stations; ++a)
            for (int b = 1; b <= stations; ++b) {
                if (a == b) continue;
                Options o = router.plan(a, b);
                if (!o.fastest.found) continue;
                CHECK(o.fewest.changes <= o.fastest.changes);                 // fewest-changes never has more changes
                CHECK(o.fastest.totalSeconds <= o.fewest.totalSeconds);       // fastest is never slower
            }
    }
}

// ---- one search for all destinations must give exactly what separate searches give ---------------------------------------
static void compareSingleAndAllDestinations(const Network& n, const std::string& label) {
    Router r(n);
    const int stations = static_cast<int>(n.stations.size());
    for (Mode mode : {Mode::Fastest, Mode::FewestChanges})
        for (int from = 1; from <= stations; ++from) {
            const std::vector<Route> all = r.routesFrom(from, mode);
            CHECK_EQ(all.size(), static_cast<size_t>(stations) + 1);
            for (int to = 1; to <= stations; ++to) {
                const Route single = r.find(from, to, mode);
                const Route& one = all[static_cast<size_t>(to)];
                if (single.found != one.found || single.totalSeconds != one.totalSeconds || single.changes != one.changes) {
                    std::cerr << label << ": routesFrom and find disagree " << from << " -> " << to << "\n";
                    CHECK(false);
                }
                if (one.found) CHECK(validateRoute(n, one).empty());
            }
        }
}

static void testRoutesFromAgreeWithFind() {
    Network n = makeNetwork(4, 20);
    addService(n, "Direct", {1, 4}, 1000);
    n.segments.back().runSeconds = 1000;
    addService(n, "Fast1", {1, 2}, 100);
    addService(n, "Fast2", {2, 4}, 100);
    addTransfer(n, 2, 2, 2, 3, 60);
    addTransfer(n, 2, 3, 2, 2, 60);
    compareSingleAndAllDestinations(n, "hand-built");
    Router r(n);
    const auto all = r.routesFrom(1, Mode::Fastest);
    CHECK(all[4].found);
    CHECK_EQ(all[4].totalSeconds, 260);
    CHECK(!all[1].found);                                    // the origin itself
    CHECK_THROWS(r.routesFrom(99, Mode::Fastest));
}

// ---- station name matching -------------------------------------------------------------------------------
static void testNameMatching() {
    Network n = makeNetwork(6, 0);
    const char* names[] = {"Rajiv Chowk", "Noida Sec-52", "Noida Sector 51", "Kashmere Gate", "Sector 1", "Sector 2"};
    for (int i = 0; i < 6; ++i) n.stations[static_cast<size_t>(i)].name = names[i];

    auto id = [&](const std::string& q) { Resolution r = resolveStation(n, q); return r.status == Resolution::Status::Found ? r.station : 0; };
    CHECK_EQ(id("rajiv chowk"), 1);
    CHECK_EQ(id("  RAJIV   Chowk "), 1);
    CHECK_EQ(id("rajiv"), 1);                              // a unique partial match
    CHECK_EQ(id("noida sec 52"), 2);                       // punctuation is ignored
    CHECK_EQ(id("Noida Sector 52"), 2);                    // "sec" and "sector" are the same word
    CHECK_EQ(id("noida sec-51"), 3);
    CHECK_EQ(id("id:4"), 4);                               // by GTFS id
    CHECK_EQ(id("id:99"), 0);

    Resolution many = resolveStation(n, "sector");
    CHECK(many.status == Resolution::Status::Ambiguous);
    CHECK_EQ(many.candidates.size(), static_cast<size_t>(4));   // both Noida stations and both "Sector N" stations

    Resolution typo = resolveStation(n, "Noida Sectr 52");     // a real typo: suggest the closest ones
    CHECK(typo.status == Resolution::Status::NotFound);
    bool suggested = false;
    for (int c : typo.candidates) suggested |= (c == 2);
    CHECK(suggested);
    CHECK(resolveStation(n, "").status == Resolution::Status::NotFound);
}

// ---- the real import pipeline end to end ----------------------------------------------------------------
static void testFromImportedDatabase(const std::string& schemaPath, const std::string& gtfs, const std::string& config) {
    std::ifstream f(schemaPath, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Database db(":memory:", true);
    db.exec(ss.str());
    import::importInto(db, import::buildModel(gtfs, config));
    Network n = loadNetwork(db);
    CHECK_EQ(n.stations.size(), static_cast<size_t>(17));
    CHECK_EQ(n.services.size(), static_cast<size_t>(5));
    CHECK_EQ(n.dwellSeconds, 20);
    Router r(n);

    // Station ids follow stops.txt order: A=1 B=2 C=3 D=4 E=5 F=6 J=7 K=8 L=9 M=10 G1..G5=11..15 S1=16 S2=17
    Route redOnly = r.find(1, 5, Mode::Fastest);           // A to E: 500 s riding + 3 stop times
    CHECK_EQ(redOnly.totalSeconds, 560);
    CHECK_EQ(redOnly.changes, 0);
    CHECK_EQ(redOnly.stops, 4);
    Route branches = r.find(9, 10, Mode::Fastest);         // L to M: change between Blue branches at the junction J
    CHECK_EQ(branches.totalSeconds, 580);                  // 120+20+120 ride, 200 change, 120 ride
    CHECK_EQ(branches.changes, 1);
    CHECK(branches.steps[1].kind == Step::Kind::Change);
    CHECK_EQ(branches.steps[1].fromStation, 7);            // the change happens at J
    Route redToBlue = r.find(1, 9, Mode::Fastest);         // A to L: Red to C, change, Blue to L
    CHECK_EQ(redToBlue.totalSeconds, 1020);
    CHECK_EQ(redToBlue.changes, 1);
    Route crossOperator = r.find(1, 17, Mode::Fastest);    // A to S2: Red to E, confirmed link E-S1 (150 s), Rapid S1 to S2
    CHECK_EQ(crossOperator.totalSeconds, 830);
    CHECK_EQ(crossOperator.changes, 1);
    Route linkWalk = r.find(5, 16, Mode::Fastest);         // E to S1 is just the confirmed walk
    CHECK_EQ(linkWalk.totalSeconds, 150);
    CHECK_EQ(linkWalk.changes, 0);
    Route reverse = r.find(10, 5, Mode::Fastest);          // M to E
    CHECK_EQ(reverse.totalSeconds, 860);
    CHECK_EQ(reverse.changes, 1);
    CHECK(!r.find(1, 11, Mode::Fastest).found);            // Green line is not connected to the rest
    for (const Route* x : {&redOnly, &branches, &redToBlue, &crossOperator, &linkWalk, &reverse}) checkValid(n, *x);

    VerifyReport rep = verifyAllPairs(n);
    CHECK(rep.ok());
    CHECK(rep.unreachable > 0);
    compareSingleAndAllDestinations(n, "imported test feed");                            // the Green line pairs
}

static int run(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_router <schema.sql> <import_gtfs> <import_config>\n";
        return 2;
    }
    testFastVsFewest();
    testStopTimesAreNotSkipped();
    testTrainsDoNotReverse();
    testWalksAtStartAndEndAreNotChanges();
    testBadNetworksAreRejected();
    testRoutesFromAgreeWithFind();
    testNameMatching();
    testFromImportedDatabase(argv[1], argv[2], argv[3]);
    testRandomNetworks();
    return finish("test_router");
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "test_router: unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
