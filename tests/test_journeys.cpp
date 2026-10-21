#include <fstream>
#include <sstream>

#include "../src/auth/accounts.h"
#include "../src/auth/journeys.h"
#include "../src/common/db.h"
#include "../src/import/importer.h"
#include "../src/route/present.h"
#include "check.h"

using namespace metro;
using namespace metro::auth;
using namespace metro::route;

static long long g_now = 1700000000;

// A network with its own router and fare calculator, so a "different version of the data" can be built by editing a copy.
struct World {
    Network net;
    Router router;
    FareCalculator fares;
    Planner planner;
    explicit World(Network n) : net(std::move(n)), router(net), fares(net), planner(net, router, fares) {}
    World(const World&) = delete;
    World& operator=(const World&) = delete;
};

static void testJourneys(const std::string& schemaPath, const std::string& gtfs, const std::string& config) {
    std::ifstream sf(schemaPath, std::ios::binary);
    std::stringstream ss;
    ss << sf.rdbuf();
    Database db(":memory:", true);
    db.exec(ss.str());
    import::importInto(db, import::buildModel(gtfs, config));
    const Network base = loadNetwork(db);
    CHECK_EQ(base.dataVersion, 1);

    AccountOptions ao;
    ao.hashIterations = 1000;
    ao.clock = [] { return g_now; };
    AccountService accounts(db, ao);
    const int alice = accounts.registerUser("alice", "correct horse").userId;
    const int bobby = accounts.registerUser("bobby", "another password").userId;
    CHECK(alice > 0 && bobby > 0);

    auto clock = [] { return g_now; };
    World w1{Network(base)};
    JourneyService js(db, w1.net, w1.planner, w1.fares, clock, 200);

    // Station ids: A=1 B=2 C=3 D=4 E=5 F=6 J=7 K=8 L=9 M=10 G1..G5=11..15 S1=16 S2=17
    // ---- saving ---------------------------------------------------------------------------------
    SaveResult s1 = js.save(alice, 1, 5, Preference::Fastest, DayType::Weekday, "to the office");
    CHECK(s1.ok);
    CHECK(!s1.updatedExisting);
    CHECK(s1.id > 0);
    auto saved = js.get(alice, s1.id);
    CHECK(saved.has_value());
    if (saved) {
        CHECK_EQ(saved->fromGtfsId, std::string("A"));
        CHECK_EQ(saved->toGtfsId, std::string("E"));
        CHECK_EQ(saved->mode, std::string("fastest"));
        CHECK_EQ(saved->dayType, std::string("weekday"));
        CHECK_EQ(saved->label, std::string("to the office"));
        CHECK_EQ(saved->totalSeconds, 560);
        CHECK_EQ(saved->changes, 0);
        CHECK_EQ(saved->fareInr, 20);                     // 4500 m on a weekday in the test slabs
        CHECK_EQ(saved->dataVersion, 1);
        CHECK(!saved->favourite);
        CHECK(saved->resultText.find("Station A  ->  Station E") != std::string::npos);
        CHECK(saved->resultText.find("FASTEST") != std::string::npos);
        CHECK(saved->resultText.find("Red Line") != std::string::npos);
        CHECK_EQ(saved->createdAt, g_now);
    }

    g_now += 100;
    SaveResult again = js.save(alice, 1, 5, Preference::Fastest, DayType::Weekday, "renamed");   // the same trip again refreshes it
    CHECK(again.ok && again.updatedExisting);
    CHECK_EQ(again.id, s1.id);
    CHECK_EQ(js.list(alice).size(), static_cast<size_t>(1));
    CHECK_EQ(js.get(alice, s1.id)->label, std::string("renamed"));
    CHECK_EQ(js.get(alice, s1.id)->createdAt, g_now - 100);
    CHECK_EQ(js.get(alice, s1.id)->updatedAt, g_now);

    SaveResult fewest = js.save(alice, 1, 5, Preference::FewestChanges, DayType::Weekday, "");   // another option is a separate entry
    SaveResult sunday = js.save(alice, 1, 5, Preference::Fastest, DayType::SundayHoliday, "");
    CHECK(fewest.ok && sunday.ok);
    CHECK(fewest.id != s1.id && sunday.id != s1.id);
    CHECK_EQ(js.get(alice, sunday.id)->fareInr, 10);                     // Sunday slab
    CHECK_EQ(js.list(alice).size(), static_cast<size_t>(3));

    SaveResult withChange = js.save(alice, 1, 9, Preference::Fastest, DayType::Weekday, "");    // A to L: Red then Blue
    CHECK(withChange.ok);
    CHECK_EQ(js.get(alice, withChange.id)->changes, 1);
    SaveResult mixed = js.save(alice, 1, 17, Preference::Fastest, DayType::Weekday, "");        // Red then Rapid: no fare can be computed
    CHECK(mixed.ok);
    CHECK_EQ(js.get(alice, mixed.id)->fareInr, -1);
    CHECK(js.get(alice, mixed.id)->resultText.find("not computed") != std::string::npos);

    // 'cheapest' can be saved too: on ordinary lines every route costs the same, so it is the quickest of the equally cheap ones
    SaveResult cheapest = js.save(alice, 1, 5, Preference::Cheapest, DayType::Weekday, "");
    CHECK(cheapest.ok);
    CHECK_EQ(js.get(alice, cheapest.id)->mode, std::string("cheapest"));
    CHECK_EQ(js.get(alice, cheapest.id)->fareInr, 20);
    CHECK(js.get(alice, cheapest.id)->resultText.find("CHEAPEST") != std::string::npos);
    CHECK(js.remove(alice, cheapest.id));

    // ---- things that must be refused ----------------------------------------------------------------
    CHECK(!js.save(alice, 1, 11, Preference::Fastest, DayType::Weekday, "").ok);                 // no route (Green line is separate)
    CHECK(!js.save(alice, 1, 1, Preference::Fastest, DayType::Weekday, "").ok);                  // same station
    CHECK(!js.save(alice, 0, 5, Preference::Fastest, DayType::Weekday, "").ok);
    CHECK(!js.save(alice, 1, 99, Preference::Fastest, DayType::Weekday, "").ok);
    CHECK(!js.save(alice, 2, 5, Preference::Fastest, DayType::Weekday, std::string(61, 'x')).ok);    // label too long
    CHECK(!js.save(alice, 2, 5, Preference::Fastest, DayType::Weekday, "bad\nlabel").ok);            // control character
    CHECK(js.save(alice, 2, 5, Preference::Fastest, DayType::Weekday, "it's \"quoted\"; DROP TABLE x;--").ok);   // odd text is stored safely
    CHECK(db.tableExists("saved_journeys"));

    // ---- other people cannot see or touch my journeys -----------------------------------------------
    CHECK(js.list(bobby).empty());
    CHECK(!js.get(bobby, s1.id).has_value());
    CHECK(!js.reopen(bobby, s1.id).has_value());
    CHECK(!js.setFavourite(bobby, s1.id, true));
    CHECK(!js.remove(bobby, s1.id));
    CHECK(js.get(alice, s1.id).has_value());                                               // untouched
    CHECK(!js.get(alice, 99999).has_value());

    // ---- favourites and deleting ---------------------------------------------------------------------
    CHECK(js.setFavourite(alice, sunday.id, true));
    CHECK_EQ(js.list(alice).front().id, sunday.id);                                        // favourites come first
    CHECK(js.list(alice).front().favourite);
    CHECK(js.setFavourite(alice, sunday.id, false));
    CHECK(!js.list(alice).front().favourite);
    CHECK(js.remove(alice, fewest.id));
    CHECK(!js.get(alice, fewest.id).has_value());
    CHECK(!js.remove(alice, fewest.id));                                                   // already gone

    // the limit per user: a NEW trip is refused at the limit, but refreshing an existing one still works
    {
        const size_t have = js.list(alice).size();
        JourneyService full(db, w1.net, w1.planner, w1.fares, clock, static_cast<int>(have));
        SaveResult refused = full.save(alice, 3, 5, Preference::Fastest, DayType::Weekday, "");
        CHECK(!refused.ok);
        CHECK(refused.message.find("limit") != std::string::npos);
        CHECK(full.save(alice, 1, 5, Preference::Fastest, DayType::Weekday, "").ok);
        CHECK_EQ(js.list(alice).size(), have);
    }

    // ---- reopening ------------------------------------------------------------------------------------
    auto current = js.reopen(alice, s1.id);                                                // same data version: shown as saved
    CHECK(current.has_value());
    CHECK(current->status == ReopenStatus::Current);
    CHECK(!current->changed);
    CHECK(current->note.empty());
    CHECK_EQ(current->text, js.get(alice, s1.id)->resultText);

    // (a) the data is re-imported and a run time changes: the journey is recomputed and the difference explained
    Network changed = base;
    changed.dataVersion = 2;
    for (auto& g : changed.segments)
        if (g.service == 1 && ((g.from == 2 && g.to == 3) || (g.from == 3 && g.to == 2))) g.runSeconds += 60;
    World w2{std::move(changed)};
    JourneyService js2(db, w2.net, w2.planner, w2.fares, clock, 200);
    auto recomputed = js2.reopen(alice, s1.id);
    CHECK(recomputed.has_value());
    CHECK(recomputed->status == ReopenStatus::Recomputed);
    CHECK(recomputed->changed);
    CHECK_EQ(recomputed->journey.totalSeconds, 620);
    CHECK(recomputed->note.find("version 1 to 2") != std::string::npos);
    CHECK(recomputed->note.find("It was: about 9 min") != std::string::npos);
    CHECK(recomputed->note.find("It is now: about 10 min") != std::string::npos);
    CHECK_EQ(js2.get(alice, s1.id)->dataVersion, 2);                                       // the saved copy was refreshed
    CHECK_EQ(js2.get(alice, s1.id)->totalSeconds, 620);
    auto after = js2.reopen(alice, s1.id);
    CHECK(after->status == ReopenStatus::Current);                                         // and is current again

    // (b) the data is re-imported but nothing changed for this journey
    Network same = base;
    same.dataVersion = 3;
    World w3{std::move(same)};
    JourneyService js3(db, w3.net, w3.planner, w3.fares, clock, 200);
    auto unchanged = js3.reopen(alice, sunday.id);
    CHECK(unchanged->status == ReopenStatus::Recomputed);
    CHECK(!unchanged->changed);
    CHECK(unchanged->note.find("unchanged") != std::string::npos);

    // (c) a station has disappeared: the saved version is kept and shown with a clear note
    Network gone = base;
    gone.dataVersion = 4;
    gone.stations[0].gtfsId = "A_RENAMED";
    World w4{std::move(gone)};
    JourneyService js4(db, w4.net, w4.planner, w4.fares, clock, 200);
    auto missing = js4.reopen(alice, withChange.id);
    CHECK(missing->status == ReopenStatus::StationMissing);
    CHECK(missing->note.find("no longer exists") != std::string::npos);
    CHECK_EQ(missing->text, js.get(alice, withChange.id)->resultText);                     // still the saved text
    CHECK(js.get(alice, withChange.id).has_value());                                       // and the journey is still in the list

    // (d) the stations still exist but the way between them is gone
    Network cut = base;
    cut.dataVersion = 5;
    cut.transfers.clear();                                                                 // no change at C any more: A to L is impossible
    World w5{std::move(cut)};
    JourneyService js5(db, w5.net, w5.planner, w5.fares, clock, 200);
    auto noRoute = js5.reopen(alice, withChange.id);
    CHECK(noRoute->status == ReopenStatus::NoRoute);
    CHECK(noRoute->note.find("no longer a route") != std::string::npos);
    CHECK_EQ(noRoute->text, js.get(alice, withChange.id)->resultText);

    // deleting the user removes their journeys
    db.exec("DELETE FROM users WHERE user_id = " + std::to_string(alice) + ";");
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM saved_journeys WHERE user_id = " + std::to_string(alice) + ";"), 0LL);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_journeys <schema.sql> <import_gtfs> <import_config>\n";
        return 2;
    }
    try {
        testJourneys(argv[1], argv[2], argv[3]);
    } catch (const std::exception& e) {
        std::cerr << "test_journeys: unexpected exception: " << e.what() << "\n";
        return 1;
    }
    return finish("test_journeys");
}
