#include <fstream>
#include <random>
#include <set>
#include <sstream>

#include "../src/common/db.h"
#include "../src/import/importer.h"
#include "../src/route/fare.h"
#include "../src/route/network.h"
#include "../src/route/planner.h"
#include "../src/route/present.h"
#include "../src/route/router.h"
#include "../src/route/verify.h"
#include "check.h"

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

static void addService(Network& n, const std::string& name, const std::string& op, std::vector<int> stations, int run, int distance, const std::string& family = "") {
    Service s;
    s.id = static_cast<int>(n.services.size()) + 1;
    s.family = family.empty() ? name : family;
    s.name = name;
    s.op = op;
    s.stations = stations;
    n.services.push_back(s);
    for (size_t i = 0; i + 1 < stations.size(); ++i) n.segments.push_back({s.id, stations[i], stations[i + 1], run, distance});
}

static void addTransfer(Network& n, int st, int sv1, int sv2, int walk) {
    n.transfers.push_back({st, sv1, st, sv2, walk});
    n.transfers.push_back({st, sv2, st, sv1, walk});
}

static void addSlab(Network& n, const std::string& op, double lo, double hi, int fare) {
    FareSlab f;
    f.op = op;
    f.dayType = "weekday";
    f.ticket = "token";
    f.minKm = lo;
    f.maxKm = hi;
    f.fareInr = fare;
    f.validFrom = "2025";
    f.source = "test";
    n.fareSlabs.push_back(f);
}

static void addPair(Network& n, const std::string& op, const std::string& a, const std::string& b, int fare) {
    FarePair f;
    f.op = op;
    f.dayType = "any";
    f.ticket = "token";
    f.stopA = a < b ? a : b;
    f.stopB = a < b ? b : a;
    f.fareInr = fare;
    f.validFrom = "2025";
    f.source = "test";
    n.farePairs.push_back(f);
}

static bool hasBadge(const Candidate& c, Preference p) {
    for (Preference b : c.badges)
        if (b == p) return true;
    return false;
}

static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A fast but dearer Airport-Express-like route against a slower, cheaper two-leg route on ordinary lines.
static Network expressVersusLocal() {
    Network n = makeNetwork(5, 20);
    addService(n, "Express", "AEL", {1, 5}, 600, 9000);          // service 1: one fast hop
    addService(n, "LocalA", "DMRC", {1, 2}, 500, 4000);          // service 2
    addService(n, "LocalB", "DMRC", {2, 5}, 500, 4000);          // service 3: 8 km in total on ordinary lines
    addTransfer(n, 2, 2, 3, 60);
    addSlab(n, "DMRC", 0, 5, 20);
    addSlab(n, "DMRC", 5, 1000, 30);
    addPair(n, "AEL", "1", "5", 64);
    return n;
}

static void testNeedsDiffer() {
    Network n = expressVersusLocal();
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);

    // the candidates: the express (600 s, 0 changes, Rs 64) and the local route (1060 s, 1 change, Rs 30)
    const auto cands = p.candidates(1, 5, DayType::Weekday);
    CHECK_EQ(cands.size(), static_cast<size_t>(2));
    std::set<std::string> sigs;
    for (const auto& c : cands) {
        sigs.insert(c.signature);
        CHECK(validateRoute(n, c.route).empty());
    }
    CHECK_EQ(sigs.size(), cands.size());

    PlanResult cheap = p.plan(1, 5, DayType::Weekday, Preference::Cheapest);
    CHECK(cheap.found);
    CHECK_EQ(cheap.primary.routes.size(), static_cast<size_t>(1));
    CHECK_EQ(cheap.primary.routes[0].fare.fareInr, 30);               // the local route is the cheapest
    CHECK_EQ(cheap.primary.routes[0].route.changes, 1);
    CHECK(contains(cheap.primary.summary, "Lowest fare: Rs 30"));
    CHECK(!cheap.primary.fellBack);
    CHECK_EQ(cheap.others.size(), static_cast<size_t>(2));            // every other need is answered...
    CHECK(cheap.others[0].criterion == Preference::Fastest);
    CHECK(cheap.others[1].criterion == Preference::FewestChanges);
    CHECK_EQ(cheap.others[0].routes[0].route.totalSeconds, 600);       // ...even though the express wins both of them
    CHECK_EQ(cheap.others[1].routes[0].route.totalSeconds, 600);
    CHECK_EQ(cheap.others[0].routes[0].signature, cheap.others[1].routes[0].signature);

    PlanResult fast = p.plan(1, 5, DayType::Weekday, Preference::Fastest);
    CHECK_EQ(fast.primary.routes.size(), static_cast<size_t>(1));
    CHECK_EQ(fast.primary.routes[0].route.totalSeconds, 600);
    CHECK(hasBadge(fast.primary.routes[0], Preference::Fastest));
    CHECK(hasBadge(fast.primary.routes[0], Preference::FewestChanges));
    CHECK(!hasBadge(fast.primary.routes[0], Preference::Cheapest));
    CHECK_EQ(fast.others.size(), static_cast<size_t>(2));
    CHECK(fast.others[0].criterion == Preference::Cheapest);
    CHECK_EQ(fast.others[0].routes[0].fare.fareInr, 30);
    CHECK(fast.others[1].criterion == Preference::FewestChanges);

    PlanResult few = p.plan(1, 5, DayType::Weekday, Preference::FewestChanges);
    CHECK_EQ(few.primary.routes[0].route.changes, 0);
    CHECK_EQ(few.others.size(), static_cast<size_t>(2));

    // the text version names the choice and then every other option
    const std::string text = formatPlanResult(n, cheap, false, &f, DayType::Weekday);
    CHECK(contains(text, "YOUR CHOICE: Cheapest"));
    CHECK(contains(text, "OTHER OPTIONS"));
    CHECK(contains(text, "FASTEST:"));
    CHECK(contains(text, "FEWEST CHANGES:"));
    CHECK(contains(text, "Rs 64"));
}

// Several routes can fulfil the same need.
static Network twoEqualFareRoutes(int secondRouteRun) {
    Network n = makeNetwork(4, 0);
    addService(n, "Direct", "DMRC", {1, 4}, 600, 6000);          // service 1
    addService(n, "LegOne", "DMRC", {1, 2}, secondRouteRun, 3000);
    addService(n, "LegTwo", "DMRC", {2, 4}, secondRouteRun, 3000);
    addTransfer(n, 2, 2, 3, 0);
    addSlab(n, "DMRC", 0, 5, 20);
    addSlab(n, "DMRC", 5, 1000, 30);                              // both routes: shortest DMRC distance 6 km, so the same fare
    return n;
}

static void testSeveralRoutesFulfilTheNeed() {
    {
        Network n = twoEqualFareRoutes(330);                      // second route: 660 s, 1 change, same fare
        Router r(n);
        FareCalculator f(n);
        Planner p(n, r, f);
        PlanResult cheap = p.plan(1, 4, DayType::Weekday, Preference::Cheapest);
        CHECK_EQ(cheap.primary.routes.size(), static_cast<size_t>(2));       // both cost the same
        CHECK_EQ(cheap.primary.routes[0].route.totalSeconds, 600);           // quickest first
        CHECK_EQ(cheap.primary.routes[1].route.totalSeconds, 660);
        CHECK(contains(cheap.primary.summary, "2 routes cost the same lowest fare (Rs 30)"));
        CHECK_EQ(cheap.others.size(), static_cast<size_t>(2));               // the other needs are still answered
        for (const auto& g : cheap.others) CHECK(!g.routes.empty());

        PlanResult fast = p.plan(1, 4, DayType::Weekday, Preference::Fastest);
        CHECK_EQ(fast.primary.routes.size(), static_cast<size_t>(2));        // 60 s apart: within the 3 minute window
        CHECK(contains(fast.primary.summary, "2 routes, all within 3 min of the fastest"));

        PlanResult few = p.plan(1, 4, DayType::Weekday, Preference::FewestChanges);
        CHECK_EQ(few.primary.routes.size(), static_cast<size_t>(1));         // only the direct one has the fewest changes
        CHECK(contains(few.primary.summary, "Fewest changes: 0"));

        PlannerSettings tight;
        tight.similarTimeSeconds = 0;                                        // a strict window: only an exact tie counts
        Planner strict(n, r, f, tight);
        CHECK_EQ(strict.plan(1, 4, DayType::Weekday, Preference::Fastest).primary.routes.size(), static_cast<size_t>(1));
        PlannerSettings one;
        one.maxRoutesPerGroup = 1;                                           // the cap on routes per need
        Planner capped(n, r, f, one);
        CHECK_EQ(capped.plan(1, 4, DayType::Weekday, Preference::Cheapest).primary.routes.size(), static_cast<size_t>(1));
    }
    {
        Network n = twoEqualFareRoutes(600);                      // second route: 1200 s, twice as slow
        Router r(n);
        FareCalculator f(n);
        Planner p(n, r, f);
        PlanResult cheap = p.plan(1, 4, DayType::Weekday, Preference::Cheapest);
        CHECK_EQ(cheap.primary.routes.size(), static_cast<size_t>(1));       // 100% slower: over the 25% limit, so not shown
        CHECK_EQ(cheap.primary.routes[0].route.totalSeconds, 600);
        PlannerSettings loose;
        loose.maxSlowerPercent = 150;
        Planner generous(n, r, f, loose);
        CHECK_EQ(generous.plan(1, 4, DayType::Weekday, Preference::Cheapest).primary.routes.size(), static_cast<size_t>(2));
    }
}

static void testCheapestWhenNoFareExists() {
    Network n = makeNetwork(3, 0);
    addService(n, "OnlyLine", "NOFARES", {1, 2, 3}, 100, 1000);   // an operator with no fare rule at all
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);
    PlanResult cheap = p.plan(1, 3, DayType::Weekday, Preference::Cheapest);
    CHECK(cheap.found);
    CHECK(cheap.primary.fellBack);
    CHECK(contains(cheap.primary.summary, "No fare could be worked out"));
    CHECK_EQ(cheap.primary.routes.size(), static_cast<size_t>(1));
    CHECK_EQ(cheap.primary.routes[0].route.totalSeconds, 200);          // the fastest route stands in
    CHECK(!p.plan(1, 3, DayType::Weekday, Preference::Fastest).primary.fellBack);

    CHECK(!p.plan(1, 1, DayType::Weekday, Preference::Fastest).found);   // same station
}

static void testUnknownFaresCannotWinCheapest() {
    // one route has a fare, the other has none: the one with a fare is the answer, with a note
    Network n = makeNetwork(4, 0);
    addService(n, "Known", "DMRC", {1, 4}, 900, 6000);                  // service 1: slow, fare Rs 30
    addService(n, "NoFareA", "MYSTERY", {1, 2}, 200, 1000);
    addService(n, "NoFareB", "MYSTERY", {2, 4}, 200, 1000);            // quicker, but no fare can be worked out
    addTransfer(n, 2, 2, 3, 0);
    addSlab(n, "DMRC", 0, 5, 20);
    addSlab(n, "DMRC", 5, 1000, 30);
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);
    PlanResult cheap = p.plan(1, 4, DayType::Weekday, Preference::Cheapest);
    CHECK(!cheap.primary.fellBack);
    CHECK_EQ(cheap.primary.routes[0].fare.fareInr, 30);
    bool noted = false;
    for (const auto& note : cheap.notes) noted |= contains(note, "could not be worked out");
    CHECK(noted);
    CHECK_EQ(p.plan(1, 4, DayType::Weekday, Preference::Fastest).primary.routes[0].route.totalSeconds, 400);
}

// Two services of the same line (branches that share tracks) must not produce duplicate-looking routes.
static void testSameLineIsNotOfferedTwice() {
    Network n = makeNetwork(6, 20);
    addService(n, "BranchA", "DMRC", {1, 2, 3, 4}, 100, 1500, "BLUE");     // service 1: stations 1-2-3-4
    addService(n, "BranchB", "DMRC", {1, 2, 3, 5}, 100, 1500, "BLUE");     // service 2: shares 1-2-3, then goes to 5
    addTransfer(n, 1, 1, 2, 240);
    addTransfer(n, 2, 1, 2, 240);
    addTransfer(n, 3, 1, 2, 240);
    addSlab(n, "DMRC", 0, 5, 20);
    addSlab(n, "DMRC", 5, 1000, 30);
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);
    // 1 to 3 can be ridden on either branch service: the same journey, so only one route is offered
    const auto c = p.candidates(1, 3, DayType::Weekday);
    CHECK_EQ(c.size(), static_cast<size_t>(1));
    CHECK_EQ(p.plan(1, 3, DayType::Weekday, Preference::Cheapest).primary.routes.size(), static_cast<size_t>(1));
    CHECK_EQ(p.plan(1, 3, DayType::Weekday, Preference::Fastest).primary.routes.size(), static_cast<size_t>(1));
    // 4 to 5 needs a change between the branches at the junction: one journey, with its one real change
    const auto d = p.candidates(4, 5, DayType::Weekday);
    CHECK(!d.empty());
    CHECK_EQ(d[0].route.changes, 1);
    for (size_t i = 1; i < d.size(); ++i) CHECK(d[i].signature != d[0].signature);
    // a pointless extra change between the same line's services is the same journey as the direct ride
    Route direct = r.find(1, 3, Mode::Fastest);
    Restrictions ban;
    ban.bannedServices.push_back(direct.steps[0].service);
    Route other = r.find(1, 3, Mode::Fastest, ban);
    CHECK_EQ(routeSignature(n, direct), routeSignature(n, other));
}

static void testRouterRestrictions() {
    Network n = expressVersusLocal();
    Router r(n);
    Route plain = r.find(1, 5, Mode::Fastest);
    CHECK_EQ(plain.totalSeconds, 600);
    Restrictions ban;
    ban.bannedServices.push_back(1);                                    // the Express
    Route local = r.find(1, 5, Mode::Fastest, ban);
    CHECK(local.found);
    CHECK_EQ(local.changes, 1);
    CHECK(validateRoute(n, local).empty());
    for (const auto& s : local.steps)
        if (s.kind == Step::Kind::Ride) CHECK(s.service != 1);
    Restrictions noChange;
    noChange.bannedServices.push_back(1);
    noChange.bannedChangeStations.push_back(2);                         // and no changing at the only place it is possible
    CHECK(!r.find(1, 5, Mode::Fastest, noChange).found);
    Restrictions everything;
    everything.bannedServices = {1, 2, 3};
    CHECK(!r.find(1, 5, Mode::Fastest, everything).found);
    Restrictions bad;
    bad.bannedServices.push_back(99);
    CHECK_THROWS(r.find(1, 5, Mode::Fastest, bad));
    CHECK(r.find(1, 5, Mode::Fastest, Restrictions()).found);           // no restrictions behaves like the plain search
    CHECK_EQ(r.find(1, 5, Mode::Fastest, Restrictions()).totalSeconds, 600);
}

static void testSlowRoutesAreCutFromGroupsButTheTrueBestIsKept() {
    Network n = makeNetwork(3, 0);
    addService(n, "Quick", "DMRC", {1, 3}, 600, 3000);                  // 10 minutes, no change
    addService(n, "Crawl", "DMRC", {1, 2, 3}, 3000, 1500);              // 100 minutes, no change either
    addSlab(n, "DMRC", 0, 5, 20);
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);
    CHECK_EQ(p.candidates(1, 3, DayType::Weekday).size(), static_cast<size_t>(2));          // both exist...
    CHECK_EQ(p.plan(1, 3, DayType::Weekday, Preference::FewestChanges).primary.routes.size(), static_cast<size_t>(1));   // ...but the 100 minute one is not offered
    CHECK_EQ(p.plan(1, 3, DayType::Weekday, Preference::Cheapest).primary.routes.size(), static_cast<size_t>(1));
    CHECK_EQ(p.plan(1, 3, DayType::Weekday, Preference::Fastest).primary.routes[0].route.totalSeconds, 600);
}

// Someone who asks for the fewest changes gets the true minimum, however slow it is.
static void testTrueFewestChangesIsAlwaysFound() {
    Network n = makeNetwork(4, 0);
    addService(n, "SlowDirect", "DMRC", {1, 4}, 5000, 8000);            // no change, but 83 minutes
    addService(n, "FastA", "DMRC", {1, 2}, 100, 3000);
    addService(n, "FastB", "DMRC", {2, 4}, 100, 3000);                  // 1 change, 3.5 minutes
    addTransfer(n, 2, 2, 3, 10);
    addSlab(n, "DMRC", 0, 5, 20);
    addSlab(n, "DMRC", 5, 1000, 30);
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);
    PlanResult few = p.plan(1, 4, DayType::Weekday, Preference::FewestChanges);
    CHECK_EQ(few.primary.routes[0].route.changes, 0);                   // not the quick one with a change
    CHECK_EQ(few.primary.routes[0].route.totalSeconds, 5000);
    CHECK_EQ(few.others.size(), static_cast<size_t>(2));                // the quick route answers the "fastest" need
    CHECK(few.others[0].criterion == Preference::Fastest);
    CHECK_EQ(few.others[0].routes[0].route.totalSeconds, 210);
}

// ---- consistency on a whole network -----------------------------------------------------------------------------------
static void checkPlanProperties(const Network& n, const std::string& label, bool allPairs, unsigned stride) {
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);
    const int stations = static_cast<int>(n.stations.size());
    unsigned counter = 0;
    for (int from = 1; from <= stations; ++from)
        for (int to = 1; to <= stations; ++to) {
            if (from == to) continue;
            if (!allPairs && (counter++ % stride) != 0) continue;
            const Route fastestRoute = r.find(from, to, Mode::Fastest), fewestRoute = r.find(from, to, Mode::FewestChanges);
            for (Preference prefer : {Preference::Fastest, Preference::Cheapest, Preference::FewestChanges}) {
                const PlanResult plan = p.plan(from, to, DayType::Weekday, prefer);
                if (plan.found != fastestRoute.found) {
                    std::cerr << label << ": found disagrees " << from << " -> " << to << "\n";
                    CHECK(false);
                }
                if (!plan.found) continue;
                CHECK(!plan.primary.routes.empty());
                CHECK(static_cast<int>(plan.primary.routes.size()) <= settingsOf(n).maxRoutesPerGroup);
                std::set<std::string> seen;
                for (const auto& c : plan.primary.routes) {
                    CHECK(seen.insert(c.signature).second);              // no route twice within a group
                    CHECK(validateRoute(n, c.route).empty());
                }
                CHECK_EQ(plan.others.size(), static_cast<size_t>(2));    // the other two needs are always answered
                std::set<int> criteria = {static_cast<int>(prefer)};
                for (const auto& g : plan.others) {
                    CHECK(criteria.insert(static_cast<int>(g.criterion)).second);   // three different needs in all
                    CHECK(!g.routes.empty());
                    std::set<std::string> inGroup;
                    for (const auto& c : g.routes) {
                        CHECK(inGroup.insert(c.signature).second);
                        CHECK(validateRoute(n, c.route).empty());
                    }
                    if (g.criterion == Preference::Fastest) CHECK_EQ(g.routes[0].route.totalSeconds, fastestRoute.totalSeconds);
                    if (g.criterion == Preference::FewestChanges) {
                        CHECK_EQ(g.routes[0].route.changes, fewestRoute.changes);
                        CHECK_EQ(g.routes[0].route.totalSeconds, fewestRoute.totalSeconds);
                    }
                }
                const Candidate& top = plan.primary.routes[0];
                if (prefer == Preference::Fastest) CHECK_EQ(top.route.totalSeconds, fastestRoute.totalSeconds);
                if (prefer == Preference::FewestChanges) {
                    CHECK_EQ(top.route.changes, fewestRoute.changes);
                    CHECK_EQ(top.route.totalSeconds, fewestRoute.totalSeconds);
                }
                if (prefer == Preference::Cheapest && !plan.primary.fellBack) {
                    CHECK(top.fare.available);
                    for (const auto& c : plan.primary.routes) CHECK_EQ(c.fare.fareInr, top.fare.fareInr);   // all equally cheap
                }
                // ordered quickest first
                for (size_t i = 1; i < plan.primary.routes.size(); ++i) CHECK(plan.primary.routes[i - 1].route.totalSeconds <= plan.primary.routes[i].route.totalSeconds);
            }
        }
}

static void testWholeNetworks(const std::string& schemaPath, const std::string& gtfs, const std::string& config) {
    std::ifstream sf(schemaPath, std::ios::binary);
    std::stringstream ss;
    ss << sf.rdbuf();
    Database db(":memory:", true);
    db.exec(ss.str());
    import::importInto(db, import::buildModel(gtfs, config));
    Network n = loadNetwork(db);
    CHECK_EQ(n.plannerSimilarSeconds, 180);          // the settings came through the config file and the database
    CHECK_EQ(n.plannerMaxRoutes, 3);
    CHECK_EQ(n.plannerMaxSlowerPercent, 25);
    checkPlanProperties(n, "imported test feed", true, 1);

    // the test feed: Red then Blue between A and L has just the one sensible route
    Router r(n);
    FareCalculator f(n);
    Planner p(n, r, f);
    PlanResult al = p.plan(1, 9, DayType::Weekday, Preference::Cheapest);
    CHECK(al.found);
    CHECK_EQ(al.primary.routes[0].fare.fareInr, 30);
}

static void testRandomNetworks() {
    for (unsigned seed = 1; seed <= 60; ++seed) {
        std::mt19937 rng(seed);
        auto pick = [&](unsigned k) { return static_cast<int>(rng() % k); };
        const int stations = 6 + pick(7);
        Network n = makeNetwork(stations, pick(31));
        const int services = 3 + pick(4);
        for (int s = 0; s < services; ++s) {
            std::vector<int> all;
            for (int i = 1; i <= stations; ++i) all.push_back(i);
            for (size_t i = all.size(); i > 1; --i) std::swap(all[i - 1], all[static_cast<size_t>(pick(static_cast<unsigned>(i)))]);
            const int len = 2 + pick(static_cast<unsigned>(std::min(5, stations - 1)));
            addService(n, "L" + std::to_string(s), pick(3) == 0 ? "AEL" : "DMRC", std::vector<int>(all.begin(), all.begin() + len), 60, 1000);
        }
        for (auto& g : n.segments) {
            g.runSeconds = 60 + pick(400);
            g.distanceM = 500 + pick(3000);
        }
        for (const auto& a : n.services)
            for (const auto& b : n.services) {
                if (a.id >= b.id) continue;
                for (int st : a.stations)
                    for (int st2 : b.stations)
                        if (st == st2 && pick(4) != 0) addTransfer(n, st, a.id, b.id, pick(300));
            }
        addSlab(n, "DMRC", 0, 5, 20);
        addSlab(n, "DMRC", 5, 1000, 40);
        for (int i = 1; i <= stations; ++i)
            for (int j = i + 1; j <= stations; ++j)
                if (pick(2) == 0) addPair(n, "AEL", std::to_string(i), std::to_string(j), 30 + pick(60));
        checkPlanProperties(n, "random " + std::to_string(seed), false, 3);
    }
}

static int run(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_planner <schema.sql> <import_gtfs> <import_config>\n";
        return 2;
    }
    testNeedsDiffer();
    testSeveralRoutesFulfilTheNeed();
    testCheapestWhenNoFareExists();
    testUnknownFaresCannotWinCheapest();
    testSameLineIsNotOfferedTwice();
    testRouterRestrictions();
    testSlowRoutesAreCutFromGroupsButTheTrueBestIsKept();
    testTrueFewestChangesIsAlwaysFound();
    testWholeNetworks(argv[1], argv[2], argv[3]);
    testRandomNetworks();
    return finish("test_planner");
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "test_planner: unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
