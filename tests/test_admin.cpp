// Tests for the station admin feature: roles, the migration from schema 5, renaming and pausing stations, how closures
// change routing, and the protected /api/admin endpoints.
//   test_admin SCHEMA.sql GTFS_FOLDER CONFIG_FOLDER
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../src/admin/station_admin.h"
#include "../src/auth/accounts.h"
#include "../src/common/db.h"
#include "../src/common/migrate.h"
#include "../src/import/importer.h"
#include "../src/route/fare.h"
#include "../src/route/impact.h"
#include "../src/route/network.h"
#include "../src/route/planner.h"
#include "../src/route/router.h"
#include "../src/web/api.h"
#include "../src/web/http.h"
#include "check.h"

namespace fs = std::filesystem;
using namespace metro;

static std::string g_schemaPath, g_gtfs, g_config;
static long long g_now = 1700000000;   // the fake clock (2023-11-14 22:13 UTC)

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static void makeDb(Database& db) {
    db.exec(readFile(g_schemaPath));
    import::importInto(db, import::buildModel(g_gtfs, g_config));
}
static long long dataVersion(Database& db) { return db.scalarInt("SELECT CAST(value AS INTEGER) FROM meta WHERE key='data_version';"); }
static int stationId(const route::Network& n, const std::string& gtfsId) {
    for (const auto& s : n.stations)
        if (s.gtfsId == gtfsId) return s.id;
    return 0;
}
static std::vector<std::string> columns(Database& db, const std::string& table) {
    std::vector<std::string> out;
    for (const auto& r : db.rows("PRAGMA table_info(" + table + ");")) out.push_back(r[1]);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------
static void testMigration() {
    // a database as it was at schema version 5: users without a role, no station_changes table, one real account
    Database old(":memory:", true);
    old.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
             "INSERT INTO meta VALUES ('schema_version', '5'); INSERT INTO meta VALUES ('data_version', '3');"
             "CREATE TABLE users (user_id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE COLLATE NOCASE CHECK (length(username) BETWEEN 3 AND 20),"
             " password_hash TEXT NOT NULL, created_at INTEGER NOT NULL CHECK (typeof(created_at) = 'integer'));"
             "INSERT INTO users VALUES (1, 'alice', 'pbkdf2-sha256$1$aa$bb', 1000);");
    CHECK(!old.tableExists("station_changes"));
    const std::string said = migrateDatabase(old);
    CHECK(contains(said, "upgraded"));
    CHECK_EQ(old.scalarText("SELECT value FROM meta WHERE key='schema_version';"), std::string("7"));     // 5 -> 6 -> 7 in one go
    CHECK(old.tableExists("line_changes"));
    CHECK_EQ(old.scalarText("SELECT value FROM meta WHERE key='data_version';"), std::string("3"));          // nothing else is touched
    CHECK_EQ(old.scalarText("SELECT role FROM users WHERE username='alice';"), std::string("user"));          // existing accounts become plain users
    CHECK_EQ(old.scalarText("SELECT password_hash FROM users WHERE username='alice';"), std::string("pbkdf2-sha256$1$aa$bb"));
    CHECK(old.tableExists("station_changes"));
    CHECK(contains(migrateDatabase(old), "already up to date"));                                               // running it twice is harmless
    CHECK_THROWS(old.exec("UPDATE users SET role = 'root' WHERE user_id = 1;"));                               // the role rule came across too

    // a migrated database has exactly the shape of a fresh one
    Database fresh(":memory:", true);
    fresh.exec(readFile(g_schemaPath));
    CHECK(columns(old, "users") == columns(fresh, "users"));
    CHECK(columns(old, "station_changes") == columns(fresh, "station_changes"));
    CHECK(columns(old, "line_changes") == columns(fresh, "line_changes"));
    CHECK_THROWS(old.exec("INSERT INTO station_changes(gtfs_stop_id, kind, new_name, reason, start_at, created_at, created_by) VALUES ('A','closure','x','long enough',1,1,'a');"));   // a closure has no new name

    // all-or-nothing: if the upgrade cannot finish, nothing is half-changed
    Database broken(":memory:", true);
    broken.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); INSERT INTO meta VALUES ('schema_version', '5');");   // no users table
    CHECK_THROWS(migrateDatabase(broken));
    CHECK_EQ(broken.scalarText("SELECT value FROM meta WHERE key='schema_version';"), std::string("5"));
    CHECK(!broken.tableExists("station_changes"));

    // versions it does not know about, and databases that are not ours
    Database v4(":memory:", true);
    v4.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); INSERT INTO meta VALUES ('schema_version', '4');");
    CHECK_THROWS(migrateDatabase(v4));
    Database stranger(":memory:", true);
    CHECK_THROWS(migrateDatabase(stranger));

    // the network loader tells people to migrate, rather than to throw their data away
    try {
        Database old2(":memory:", true);
        old2.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); INSERT INTO meta VALUES ('schema_version', '5');");
        route::loadNetwork(old2);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(contains(e.what(), "metro_db migrate"));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
static void testRoles() {
    Database db(":memory:", true);
    db.exec(readFile(g_schemaPath));
    auth::AccountOptions o;
    o.hashIterations = 1000;
    o.clock = [] { return g_now; };
    auth::AccountService svc(db, o);
    CHECK(svc.registerUser("alice", "correct horse").status == auth::Status::Ok);
    CHECK(svc.registerUser("bobby", "another good pw").status == auth::Status::Ok);

    auto token = [&](const std::string& u, const std::string& p) { return svc.login(u, p).token; };
    auto a1 = svc.validateSession(token("alice", "correct horse"));
    CHECK(a1.has_value() && a1->role == "user" && !a1->isAdmin());                      // everybody starts as a plain user
    CHECK(svc.adminNames().empty());

    CHECK(svc.setRole("alice", "admin"));
    auto a2 = svc.validateSession(token("alice", "correct horse"));
    CHECK(a2.has_value() && a2->isAdmin());
    CHECK(svc.setRole("ALICE", "admin"));                                                // usernames ignore case, as everywhere
    CHECK_EQ(svc.adminNames().size(), static_cast<size_t>(1));
    CHECK(!svc.validateSession(token("bobby", "another good pw"))->isAdmin());           // somebody else is not affected

    CHECK(!svc.setRole("nobody", "admin"));                                              // no such user
    CHECK(!svc.setRole("x' OR '1'='1", "admin"));                                        // an injection attempt changes nothing
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM users WHERE role = 'admin';"), 1LL);
    CHECK_THROWS(svc.setRole("alice", "superuser"));
    CHECK_THROWS(svc.setRole("alice", ""));
    CHECK_THROWS(svc.setRole("alice", "admin'; DROP TABLE users; --"));
    CHECK(db.tableExists("users"));

    CHECK(svc.setRole("alice", "user"));
    CHECK(!svc.validateSession(token("alice", "correct horse"))->isAdmin());             // the role is looked at on every request, so removal is immediate
    CHECK(svc.adminNames().empty());
}

// ---------------------------------------------------------------------------------------------------------------------------
static void testStationAdminRules() {
    Database db(":memory:", true);
    makeDb(db);
    admin::StationAdmin adm(db, [] { return g_now; });
    const long long v0 = dataVersion(db);

    // a rename
    auto r = adm.rename("A", "  Alpha Central  ", "Station renamed after the new gate", "bobby");
    CHECK(r.outcome == admin::Outcome::Ok && r.changeId > 0);
    CHECK_EQ(dataVersion(db), v0 + 1);                                                    // the running server notices this
    auto net = route::loadNetwork(db);
    CHECK_EQ(net.station(stationId(net, "A")).name, std::string("Alpha Central"));       // trimmed, and applied when the network loads
    CHECK_EQ(net.feedNames[static_cast<size_t>(stationId(net, "A") - 1)], std::string("Station A"));
    CHECK_EQ(db.scalarText("SELECT created_by FROM station_changes WHERE change_id = " + std::to_string(r.changeId) + ";"), std::string("bobby"));

    // refused renames change nothing at all
    const long long v1 = dataVersion(db);
    const long long rows1 = db.scalarInt("SELECT COUNT(*) FROM station_changes;");
    CHECK(adm.rename("ZZ", "Some Name", "Some good reason", "bobby").outcome == admin::Outcome::NotFound);
    CHECK(adm.rename("B", "X", "Some good reason", "bobby").outcome == admin::Outcome::Invalid);                               // 1 character
    CHECK(adm.rename("B", std::string(61, 'x'), "Some good reason", "bobby").outcome == admin::Outcome::Invalid);              // 61 characters
    CHECK(adm.rename("B", std::string(60, 'x'), "Some good reason", "bobby").outcome == admin::Outcome::Ok);                   // 60 is the limit
    CHECK(adm.rename("B", std::string("Bad\nName"), "Some good reason", "bobby").outcome == admin::Outcome::Invalid);         // control character
    CHECK(adm.rename("B", std::string("Bad\xFF" "Name"), "Some good reason", "bobby").outcome == admin::Outcome::Invalid);    // not valid UTF-8
    CHECK(adm.rename("B", std::string(60, 'x'), "Some good reason", "bobby").outcome == admin::Outcome::Invalid);              // already its (new) name
    CHECK(adm.rename("C", "station d", "Some good reason", "bobby").outcome == admin::Outcome::Invalid);                       // another station's name, any case
    CHECK(adm.rename("C", "Alpha Central", "Some good reason", "bobby").outcome == admin::Outcome::Invalid);                   // another station's NEW name
    CHECK(adm.rename("C", "Nice Name", "no", "bobby").outcome == admin::Outcome::Invalid);                                    // reason too short
    CHECK(adm.rename("C", "Nice Name", std::string(201, 'r'), "bobby").outcome == admin::Outcome::Invalid);                   // reason too long
    CHECK(adm.rename("C", "Nice Name", "two\nlines of reason", "bobby").outcome == admin::Outcome::Invalid);
    CHECK(adm.rename("C", "Nice Name", "Some good reason", "").outcome == admin::Outcome::Invalid);                           // who did it?
    CHECK(adm.rename("C", "\xE0\xA4\xA8\xE0\xA4\xAE", "Hindi name, two characters", "bobby").outcome == admin::Outcome::Ok);   // characters, not bytes
    CHECK_EQ(dataVersion(db), v1 + 2);                                                                                       // only the two good ones counted
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM station_changes;"), rows1 + 2);
    CHECK_EQ(adm.rename("D", "Quote'Name", "x'); DROP TABLE users; --", "o'brien").outcome == admin::Outcome::Ok ? 1 : 0, 1);   // quotes are data, never code
    CHECK(db.tableExists("users"));
    CHECK_EQ(db.scalarText("SELECT created_by FROM station_changes ORDER BY change_id DESC LIMIT 1;"), std::string("o'brien"));

    // the newest rename wins; cancelling one gives the previous name back
    auto first = adm.rename("E", "East One", "First new name", "bobby");
    auto second = adm.rename("E", "East Two", "Second new name", "bobby");
    CHECK_EQ(route::loadNetwork(db).station(stationId(net, "E")).name, std::string("East Two"));
    CHECK(adm.cancel(second.changeId, "bobby").outcome == admin::Outcome::Ok);
    CHECK_EQ(route::loadNetwork(db).station(stationId(net, "E")).name, std::string("East One"));
    CHECK(adm.cancel(first.changeId, "bobby").outcome == admin::Outcome::Ok);
    CHECK_EQ(route::loadNetwork(db).station(stationId(net, "E")).name, std::string("Station E"));
    CHECK(adm.cancel(first.changeId, "bobby").outcome == admin::Outcome::Invalid);       // already cancelled
    CHECK(adm.cancel(99999, "bobby").outcome == admin::Outcome::NotFound);
    CHECK_EQ(db.scalarText("SELECT cancelled_by FROM station_changes WHERE change_id = " + std::to_string(first.changeId) + ";"), std::string("bobby"));
    CHECK_EQ(static_cast<int>(adm.history().size()) > 0 ? 1 : 0, 1);                      // cancelled rows are kept: this is the audit trail

    // closures
    const long long h = 3600;
    CHECK(adm.close("B", "Flooding on the platform", 0, 0, "bobby").outcome == admin::Outcome::Ok);                              // until further notice, starting now
    CHECK(adm.close("B", "Flooding on the platform", g_now, g_now + 2 * h, "bobby").outcome == admin::Outcome::Ok);              // temporary
    CHECK(adm.close("B", "Planned works next week", g_now + 7 * 24 * h, g_now + 8 * 24 * h, "bobby").outcome == admin::Outcome::Ok);   // in the future
    CHECK(adm.close("ZZ", "Flooding on the platform", 0, 0, "bobby").outcome == admin::Outcome::NotFound);
    CHECK(adm.close("B", "x", 0, 0, "bobby").outcome == admin::Outcome::Invalid);                                                // reason
    CHECK(adm.close("B", "Flooding on the platform", g_now, g_now, "bobby").outcome == admin::Outcome::Invalid);                 // ends when it starts
    CHECK(adm.close("B", "Flooding on the platform", g_now, g_now - 5, "bobby").outcome == admin::Outcome::Invalid);             // ends before it starts
    CHECK(adm.close("B", "Flooding on the platform", g_now - 3 * h, g_now - h, "bobby").outcome == admin::Outcome::Invalid);     // already over
    CHECK(adm.close("B", "Flooding on the platform", g_now + 400 * 24 * h, 0, "bobby").outcome == admin::Outcome::Invalid);      // more than a year ahead
    CHECK(adm.close("B", "Flooding on the platform", 0, g_now + 4000LL * 24 * h, "bobby").outcome == admin::Outcome::Invalid);   // more than ten years
    CHECK(adm.close("B", "Flooding on the platform", -5, 0, "bobby").outcome == admin::Outcome::Invalid);
    CHECK(adm.close("B", "Flooding on the platform", 0, -5, "bobby").outcome == admin::Outcome::Invalid);
    CHECK(adm.close("B", "Flooding on the platform", g_now - 600, 0, "bobby").outcome == admin::Outcome::Ok);                    // a start a few minutes ago is fine

    // cancelling a closure, and a closure that has already ended
    auto c = adm.close("C", "Signal failure near the station", 0, g_now + h, "bobby");
    CHECK(adm.cancel(c.changeId, "bobby").outcome == admin::Outcome::Ok);
    auto d = adm.close("D", "Signal failure near the station", 0, g_now + h, "bobby");
    g_now += 2 * h;
    CHECK(adm.cancel(d.changeId, "bobby").outcome == admin::Outcome::Invalid);            // it ended an hour ago
    g_now -= 2 * h;
}

// ---------------------------------------------------------------------------------------------------------------------------
static void testClosureTimes() {
    Database db(":memory:", true);
    makeDb(db);
    admin::StationAdmin adm(db, [] { return g_now; });
    const long long h = 3600;
    auto temp = adm.close("B", "Water on the track", g_now + h, g_now + 3 * h, "bobby");          // starts in an hour, ends in three
    auto forever = adm.close("D", "Station demolished for a rebuild", 0, 0, "bobby");
    auto cancelled = adm.close("C", "Reported by mistake", 0, 0, "bobby");
    adm.cancel(cancelled.changeId, "bobby");
    // two overlapping closures at E: the one that lasts longer is reported; a permanent one beats a temporary one
    adm.close("E", "Short problem with the lift", 0, g_now + h, "bobby");
    adm.close("E", "Longer problem with the roof", 0, g_now + 5 * h, "bobby");

    const auto net = route::loadNetwork(db);
    const int B = stationId(net, "B"), D = stationId(net, "D"), C = stationId(net, "C"), E = stationId(net, "E");
    auto closedAt = [&](long long t) {
        std::vector<int> ids;
        for (const auto& c : route::closuresAt(net, t)) ids.push_back(c.station);
        return ids;
    };
    auto has = [&](const std::vector<int>& v, int id) { return std::find(v.begin(), v.end(), id) != v.end(); };

    auto now0 = closedAt(g_now);
    CHECK(!has(now0, B));                       // not started yet
    CHECK(has(now0, D) && has(now0, E));        // a permanent one, and one in progress
    CHECK(!has(now0, C));                       // cancelled ones are not loaded at all
    CHECK(has(closedAt(g_now + h), B));         // starts exactly at its start time
    CHECK(has(closedAt(g_now + 3 * h - 1), B));
    CHECK(!has(closedAt(g_now + 3 * h), B));    // and is over exactly at its end time
    CHECK(has(closedAt(g_now + 100000LL * h), D));   // "until further notice" never ends by itself
    for (const auto& c : route::closuresAt(net, g_now))
        if (c.station == E) CHECK_EQ(c.endAt, g_now + 5 * h);
    adm.close("E", "Now closed for good", 0, 0, "bobby");
    for (const auto& c : route::closuresAt(route::loadNetwork(db), g_now))
        if (c.station == E) CHECK_EQ(c.endAt, 0LL);

    const auto soon = route::upcomingClosures(net, g_now, 2 * h);
    CHECK_EQ(soon.size(), static_cast<size_t>(1));
    CHECK_EQ(soon[0].station, B);
    CHECK(route::upcomingClosures(net, g_now, h / 2).empty());
    (void)temp;
    (void)forever;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Test feed: RED A-B-C-D-E, BLUE C-F-J-K-L and C-F-J-M (Red and Blue meet at C), GREEN G1..G5, RAPID S1-S2, and a walking link E to S1.
static void testRoutingAroundClosures() {
    Database db(":memory:", true);
    makeDb(db);
    const auto net = route::loadNetwork(db);
    route::Router router(net);
    route::FareCalculator fares(net);
    route::Planner planner(net, router, fares);
    const int A = stationId(net, "A"), B = stationId(net, "B"), C = stationId(net, "C"), D = stationId(net, "D"), E = stationId(net, "E");
    const int F = stationId(net, "F"), J = stationId(net, "J"), K = stationId(net, "K"), L = stationId(net, "L"), M = stationId(net, "M");
    const int S1 = stationId(net, "S1"), S2 = stationId(net, "S2");
    using route::DayType;
    using route::Preference;
    auto plan = [&](int from, int to) { return planner.plan(from, to, DayType::Weekday, Preference::Fastest); };

    // before anything is closed
    CHECK(plan(A, L).found && plan(A, D).found && plan(E, S2).found && plan(A, M).found);
    const int baselineStops = router.find(A, D, route::Mode::Fastest).stops;

    // closing a station that a train merely runs through: you can stay on the train, nothing else changes
    planner.setClosedStations({B});
    auto through = plan(A, D);
    CHECK(through.found);
    CHECK_EQ(through.primary.routes[0].route.changes, 0);
    CHECK_EQ(through.primary.routes[0].route.stops, baselineStops);                  // the train still passes it
    bool passesB = false;
    for (const auto& s : through.primary.routes[0].route.steps)
        for (int v : s.via) passesB = passesB || v == B;
    CHECK(passesB);
    CHECK(plan(A, L).found);                                                         // the Blue connection at C is still fine
    CHECK(!plan(B, D).found);                                                        // cannot start at a closed station
    CHECK(!plan(A, B).found);                                                        // or end at one
    CHECK(!plan(B, A).found);

    // closing the station where the only change happens: that journey is no longer possible
    planner.setClosedStations({C});
    CHECK(!plan(A, L).found);
    CHECK(!plan(A, M).found);
    CHECK(plan(A, D).found);                                                         // staying on the Red line through C is fine
    CHECK(plan(C, A).found == false);                                                // but C itself is closed
    planner.setClosedStations({F});
    CHECK(plan(C, K).found);                                                         // Blue passes F without stopping
    CHECK(plan(A, K).found);

    // a closed station at the far end of a walking link cannot be walked to
    planner.setClosedStations({});
    auto viaWalk = plan(E, S2);
    CHECK(viaWalk.found);
    planner.setClosedStations({S1});
    CHECK(!plan(E, S2).found);
    planner.setClosedStations({E});                                                  // getting off at E to walk to S1 is also impossible
    CHECK(!plan(A, S2).found);
    planner.setClosedStations({});
    CHECK(plan(A, S2).found);

    // every alternative the planner offers respects the closure
    planner.setClosedStations({J});
    for (const auto& cand : planner.candidates(A, L, DayType::Weekday))
        for (const auto& s : cand.route.steps)
            if (s.kind == route::Step::Kind::Change) CHECK(s.fromStation != J && s.toStation != J);
    planner.setClosedStations({K});
    CHECK(!plan(A, K).found);
    CHECK(plan(A, L).found);                                                         // K is only passed on the way to L

    // the router agrees with the planner when given the same restriction directly
    route::Restrictions r;
    r.closedStations = {C};
    CHECK(!router.find(A, L, route::Mode::Fastest, r).found);
    CHECK(!router.find(A, L, route::Mode::FewestChanges, r).found);
    CHECK(router.find(A, D, route::Mode::Fastest, r).found);
    CHECK(!r.empty());
    CHECK(route::Restrictions().empty());
}

// ---------------------------------------------------------------------------------------------------------------------------
// The web layer
struct TestSite {
    Database db{":memory:", true};
    std::unique_ptr<web::Api> api;
    std::unique_ptr<auth::AccountService> accounts;
    explicit TestSite(bool adminFromAnywhere = false) {
        makeDb(db);
        web::ApiConfig cfg;
        cfg.webRoot = (fs::temp_directory_path() / "metro_admin_test_web").string();
        cfg.account.hashIterations = 1000;
        cfg.account.clock = [] { return g_now; };
        cfg.adminFromAnywhere = adminFromAnywhere;
        api = std::make_unique<web::Api>(db, cfg);
        auth::AccountOptions o = cfg.account;
        accounts = std::make_unique<auth::AccountService>(db, o);
    }
    web::Response request(const std::string& method, const std::string& target, const std::string& form = "", const std::string& cookie = "",
                          const std::string& remote = "", bool csrfHeader = true) {
        std::map<std::string, std::string> h = {{"Host", "localhost:8080"}};
        if (csrfHeader) h["X-Requested-With"] = "metro-web";
        if (!form.empty()) h["Content-Type"] = "application/x-www-form-urlencoded";
        if (!cookie.empty()) h["Cookie"] = cookie;
        web::Request req = web::makeRequest(method, target, h, form);
        req.remoteAddr = remote;
        return api->handle(req);
    }
    web::Response get(const std::string& target, const std::string& cookie = "", const std::string& remote = "") { return request("GET", target, "", cookie, remote, false); }
    std::string cookieFor(const std::string& user, const std::string& pass) {
        const auto r = request("POST", "/api/login", "username=" + user + "&password=" + pass);
        for (const auto& h : r.headers)
            if (h.first == "Set-Cookie") return h.second.substr(0, h.second.find(';'));
        return std::string();
    }
    int id(const std::string& gtfs) {
        const auto net = route::loadNetwork(db);
        return stationId(net, gtfs);
    }
};

static std::string num(long long v) { return std::to_string(v); }

static void testAdminPermissions() {
    TestSite site;
    CHECK(site.request("POST", "/api/register", "username=alice&password=correct+horse").status == 201);
    CHECK(site.request("POST", "/api/register", "username=bobby&password=another+good+pw").status == 201);
    site.accounts->setRole("bobby", "admin");
    const std::string alice = site.cookieFor("alice", "correct+horse"), bobby = site.cookieFor("bobby", "another+good+pw");
    CHECK(!alice.empty() && !bobby.empty());
    const std::string A = num(site.id("A"));
    const std::string rename = "station=" + A + "&name=Alpha+Central&reason=New+name+after+the+rebuild";

    // who is allowed
    CHECK_EQ(site.get("/api/admin/overview").status, 401);                                             // not logged in
    CHECK_EQ(site.request("POST", "/api/admin/rename", rename).status, 401);
    CHECK_EQ(site.get("/api/admin/overview", alice).status, 403);                                      // a plain user
    CHECK_EQ(site.request("POST", "/api/admin/rename", rename, alice).status, 403);
    CHECK_EQ(site.request("POST", "/api/admin/close", "station=" + A + "&reason=A+good+reason", alice).status, 403);
    CHECK_EQ(site.request("POST", "/api/admin/cancel", "change=1", alice).status, 403);
    CHECK_EQ(site.get("/api/admin/overview", bobby).status, 200);                                      // an admin
    CHECK_EQ(site.get("/api/admin/nothing", bobby).status, 404);
    CHECK_EQ(site.request("DELETE", "/api/admin/overview", "", bobby).status, 405);
    CHECK_EQ(site.get("/api/admin/rename", bobby).status, 405);                                        // changes only by POST
    CHECK_EQ(site.db.scalarInt("SELECT COUNT(*) FROM station_changes;"), 0LL);                         // nothing got through

    // the CSRF rules apply to admins too
    CHECK_EQ(site.request("POST", "/api/admin/rename", rename, bobby, "", false).status, 403);          // no X-Requested-With header
    CHECK_EQ(site.db.scalarInt("SELECT COUNT(*) FROM station_changes;"), 0LL);

    // only from this computer
    CHECK_EQ(site.get("/api/admin/overview", bobby, "203.0.113.9").status, 403);                       // another computer
    CHECK_EQ(site.request("POST", "/api/admin/rename", rename, bobby, "203.0.113.9").status, 403);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "127.0.0.1").status, 200);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "::1").status, 200);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "127.5.4.3").status, 200);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "192.168.1.7").status, 403);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "127.0.0.1.evil.example").status, 403);            // a look-alike is not local
    CHECK_EQ(site.get("/api/admin/overview", bobby, "127.0.0").status, 403);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "127.0.0.256").status, 403);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "127.0.0.1 ").status, 403);
    CHECK_EQ(site.get("/api/admin/overview", bobby, "localhost").status, 403);
    TestSite open(true);                                                                                // unless the site was set up for that
    open.request("POST", "/api/register", "username=bobby&password=another+good+pw");
    open.accounts->setRole("bobby", "admin");
    CHECK_EQ(open.get("/api/admin/overview", open.cookieFor("bobby", "another+good+pw"), "203.0.113.9").status, 200);
    CHECK_EQ(open.get("/api/admin/overview", "", "203.0.113.9").status, 401);                          // and then logging in is still required

    // /api/me says who you are
    CHECK(contains(site.get("/api/me", bobby).body, "\"role\":\"admin\""));
    CHECK(contains(site.get("/api/me", alice).body, "\"role\":\"user\""));
    CHECK(!contains(site.get("/api/me").body, "role"));

    // being made a normal user again takes effect straight away
    site.accounts->setRole("bobby", "user");
    CHECK_EQ(site.get("/api/admin/overview", bobby).status, 403);
    // the sign-up form cannot make an admin
    CHECK(site.request("POST", "/api/register", "username=sneaky&password=a+good+password&role=admin").status == 201);
    CHECK(contains(site.get("/api/me", site.cookieFor("sneaky", "a+good+password")).body, "\"role\":\"user\""));
}

static void testAdminRenameAndClose() {
    TestSite site;
    site.request("POST", "/api/register", "username=bobby&password=another+good+pw");
    site.accounts->setRole("bobby", "admin");
    const std::string bobby = site.cookieFor("bobby", "another+good+pw");
    const std::string A = num(site.id("A")), B = num(site.id("B")), D = num(site.id("D")), C = num(site.id("C"));
    auto post = [&](const std::string& what, const std::string& form) { return site.request("POST", "/api/admin/" + what, form, bobby); };

    // ---- rename: visible straight away to everyone
    const long long v0 = site.api->dataVersion();
    auto r = post("rename", "station=" + A + "&name=Alpha+Central&reason=New+name+after+the+rebuild");
    CHECK_EQ(r.status, 200);
    CHECK(contains(r.body, "\"ok\":true"));
    CHECK_EQ(site.api->dataVersion(), v0 + 1);
    CHECK(contains(site.get("/api/stations").body, "\"name\":\"Alpha Central\""));
    CHECK(!contains(site.get("/api/stations").body, "\"name\":\"Station A\""));
    CHECK(contains(site.get("/api/search?q=alpha").body, "Alpha Central"));
    CHECK(contains(site.get("/api/route?from=" + A + "&to=" + D).body, "Alpha Central"));
    CHECK(contains(site.get("/api/admin/overview", bobby).body, "\"feedName\":\"Station A\""));       // the admin still sees the original name
    CHECK(contains(site.get("/api/admin/overview", bobby).body, "\"status\":\"current\""));

    // ---- refused changes come back as clear 400s and do not change the data version
    const long long v1 = site.api->dataVersion();
    CHECK_EQ(post("rename", "station=" + A + "&name=X&reason=New+name+after+the+rebuild").status, 400);
    CHECK_EQ(post("rename", "station=999&name=Fine+Name&reason=A+good+reason").status, 400);          // not a valid station id
    CHECK_EQ(post("rename", "station=abc&name=Fine+Name&reason=A+good+reason").status, 400);
    CHECK_EQ(post("rename", "name=Fine+Name&reason=A+good+reason").status, 400);                       // no station
    CHECK_EQ(post("rename", "station=" + B + "&name=Alpha+Central&reason=A+good+reason").status, 400); // taken
    CHECK(contains(post("rename", "station=" + B + "&name=Alpha+Central&reason=A+good+reason").body, "already called"));
    CHECK_EQ(post("close", "station=" + B + "&reason=ab").status, 400);
    CHECK_EQ(post("close", "station=" + B + "&reason=Water+on+the+track&start=abc").status, 400);
    CHECK_EQ(post("close", "station=" + B + "&reason=Water+on+the+track&end=12e5").status, 400);
    CHECK_EQ(post("close", "station=" + B + "&reason=Water+on+the+track&end=" + num(g_now - 100)).status, 400);            // in the past
    CHECK_EQ(post("close", "station=" + B + "&reason=Water+on+the+track&start=" + num(g_now + 100) + "&end=" + num(g_now + 50)).status, 400);
    CHECK_EQ(post("cancel", "change=abc").status, 400);
    CHECK_EQ(post("cancel", "change=424242").status, 404);
    CHECK_EQ(site.api->dataVersion(), v1);

    // ---- a temporary closure: B is closed for two hours, starting now
    auto c = post("close", "station=" + B + "&reason=Water+on+the+track&start=" + num(g_now) + "&end=" + num(g_now + 7200));
    CHECK_EQ(c.status, 200);
    const std::string stations = site.get("/api/stations").body;
    CHECK(contains(stations, "\"closed\":{\"reason\":\"Water on the track\""));
    CHECK(contains(stations, "\"until\":" + num(g_now + 7200)));
    const std::string notices = site.get("/api/notices").body;
    CHECK(contains(notices, "Water on the track") && contains(notices, "\"status\":\"active\""));

    // planning from or to the closed station says so, with the reason and the end time
    auto from = site.get("/api/route?from=" + B + "&to=" + D);
    CHECK_EQ(from.status, 200);
    CHECK(contains(from.body, "\"found\":false") && contains(from.body, "is closed") && contains(from.body, "Water on the track") && contains(from.body, "\"closed\":{"));
    CHECK(contains(site.get("/api/route?from=" + A + "&to=" + B).body, "\"found\":false"));
    // a route that runs THROUGH the closed station lists it, and still works
    auto through = site.get("/api/route?from=" + A + "&to=" + D);
    CHECK(contains(through.body, "\"found\":true"));
    CHECK(contains(through.body, "\"closedPassed\":[{"));
    CHECK(contains(through.body, "Water on the track"));
    // a route that needs no closed station has an empty list
    CHECK(contains(site.get("/api/route?from=" + C + "&to=" + D).body, "\"closedPassed\":[]"));

    // the closure ends by itself, with no one doing anything and no data version change
    const long long vNow = site.api->dataVersion();
    g_now += 7200;
    CHECK(!contains(site.get("/api/stations").body, "\"closed\""));
    CHECK(contains(site.get("/api/route?from=" + B + "&to=" + D).body, "\"found\":true"));
    CHECK_EQ(site.api->dataVersion(), vNow);
    CHECK(contains(site.get("/api/admin/overview", bobby).body, "\"status\":\"ended\""));
    g_now -= 7200;

    // ---- a closure that starts later is not in force yet, and starts by itself
    auto later = post("close", "station=" + D + "&reason=Planned+track+works&start=" + num(g_now + 3600) + "&end=" + num(g_now + 5400));
    CHECK_EQ(later.status, 200);
    CHECK(!contains(site.get("/api/stations").body, "Planned track works"));
    CHECK(contains(site.get("/api/notices").body, "\"status\":\"upcoming\""));
    const long long vLater = site.api->dataVersion();
    g_now += 3600;
    CHECK(contains(site.get("/api/stations").body, "Planned track works"));
    CHECK_EQ(site.api->dataVersion(), vLater);
    g_now -= 3600;

    // ---- until further notice: no end time at all
    auto forever = post("close", "station=" + C + "&reason=Station+closed+for+rebuilding");
    CHECK_EQ(forever.status, 200);
    CHECK(contains(site.get("/api/stations").body, "\"until\":null"));
    g_now += 3650LL * 24 * 3600;
    CHECK(contains(site.get("/api/stations").body, "Station closed for rebuilding"));                // still closed ten years later
    g_now -= 3650LL * 24 * 3600;
    CHECK(contains(site.get("/api/route?from=" + site.get("/api/status").body.substr(0, 0) + A + "&to=" + num(site.id("L"))).body, "\"found\":false"));   // C was the only way to change

    // ---- cancelling reopens a station at once
    int changeId = 0;
    {
        const std::string body = forever.body;
        const size_t p = body.find("\"changeId\":");
        changeId = std::atoi(body.c_str() + p + 11);
    }
    CHECK(changeId > 0);
    CHECK_EQ(post("cancel", "change=" + num(changeId)).status, 200);
    CHECK(!contains(site.get("/api/stations").body, "Station closed for rebuilding"));
    CHECK(contains(site.get("/api/route?from=" + A + "&to=" + num(site.id("L"))).body, "\"found\":true"));
    CHECK_EQ(post("cancel", "change=" + num(changeId)).status, 400);                                  // twice is refused

    // ---- the history keeps everything, with who and why
    const std::string history = site.get("/api/admin/overview", bobby).body;
    CHECK(contains(history, "\"createdBy\":\"bobby\""));
    CHECK(contains(history, "\"status\":\"cancelled\"") && contains(history, "\"cancelledBy\":\"bobby\""));
    CHECK(contains(history, "Station closed for rebuilding"));
}

static void testClosuresAndSavedJourneys() {
    TestSite site;
    site.request("POST", "/api/register", "username=alice&password=correct+horse");
    site.request("POST", "/api/register", "username=bobby&password=another+good+pw");
    site.accounts->setRole("bobby", "admin");
    const std::string alice = site.cookieFor("alice", "correct+horse"), bobby = site.cookieFor("bobby", "another+good+pw");
    const std::string A = num(site.id("A")), L = num(site.id("L")), C = num(site.id("C"));

    auto saved = site.request("POST", "/api/journeys", "from=" + A + "&to=" + L + "&mode=fastest&day=weekday&label=to+work", alice);
    CHECK_EQ(saved.status, 201);
    const std::string id = saved.body.substr(saved.body.find("\"id\":") + 5, saved.body.find_first_of(",}", saved.body.find("\"id\":")) - saved.body.find("\"id\":") - 5);
    CHECK(contains(site.get("/api/journeys/" + id, alice).body, "\"status\":\"current\""));

    // the only change station is paused: the saved journey is recomputed, and says there is no route now
    CHECK_EQ(site.request("POST", "/api/admin/close", "station=" + C + "&reason=Signal+failure+near+the+station", bobby).status, 200);
    auto opened = site.get("/api/journeys/" + id, alice);
    CHECK_EQ(opened.status, 200);
    CHECK(contains(opened.body, "\"status\":\"noRoute\"") || contains(opened.body, "\"status\":\"recomputed\""));
    CHECK(contains(opened.body, "\"route\":null"));
    // and trying to save it now is refused, because there is no route to save
    CHECK(site.request("POST", "/api/journeys", "from=" + A + "&to=" + L + "&mode=fastest&day=weekday&label=again", alice).status != 201);

    // reopening the station brings the journey back
    const std::string overview = site.get("/api/admin/overview", bobby).body;
    const size_t p = overview.find("\"id\":");
    const int changeId = std::atoi(overview.c_str() + p + 5);
    CHECK_EQ(site.request("POST", "/api/admin/cancel", "change=" + num(changeId), bobby).status, 200);
    CHECK(contains(site.get("/api/journeys/" + id, alice).body, "\"route\":{"));
}

static void testChangesSurviveReimport() {
    TestSite site;
    site.request("POST", "/api/register", "username=bobby&password=another+good+pw");
    site.accounts->setRole("bobby", "admin");
    const std::string bobby = site.cookieFor("bobby", "another+good+pw");
    const std::string A = num(site.id("A")), B = num(site.id("B"));
    CHECK_EQ(site.request("POST", "/api/admin/rename", "station=" + A + "&name=Alpha+Central&reason=New+name+after+the+rebuild", bobby).status, 200);
    CHECK_EQ(site.request("POST", "/api/admin/close", "station=" + B + "&reason=Water+on+the+track", bobby).status, 200);
    const long long before = site.api->dataVersion();

    // the feed is imported again (a data refresh): the admin's changes are NOT undone
    import::importInto(site.db, import::buildModel(g_gtfs, g_config));
    CHECK(dataVersion(site.db) > before);
    CHECK(contains(site.get("/api/stations").body, "\"name\":\"Alpha Central\""));
    CHECK(contains(site.get("/api/stations").body, "Water on the track"));
    CHECK_EQ(site.db.scalarInt("SELECT COUNT(*) FROM station_changes;"), 2LL);
    CHECK_EQ(site.db.scalarInt("SELECT COUNT(*) FROM users WHERE role = 'admin';"), 1LL);              // and so are the accounts

    // a change for a station that no longer exists stays in the table but does nothing
    site.db.exec("INSERT INTO station_changes(gtfs_stop_id, kind, new_name, reason, start_at, created_at, created_by) VALUES ('GONE','rename','Ghost Station','Station was removed',1,1,'bobby');");
    site.db.exec("UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key = 'data_version';");
    CHECK(!contains(site.get("/api/stations").body, "Ghost Station"));
    CHECK(contains(site.get("/api/admin/overview", bobby).body, "Ghost Station"));                      // still in the history
}

static void testPublicSurface() {
    TestSite site;
    // the passenger-facing endpoints work with nothing closed
    CHECK(contains(site.get("/api/notices").body, "\"closures\":[]"));
    CHECK(!contains(site.get("/api/stations").body, "closed"));
    CHECK(contains(site.get("/api/route?from=1&to=4").body, "\"closedPassed\":[]"));
    // admin pages and endpoints leak nothing to someone who is not logged in
    const auto r = site.get("/api/admin/overview");
    CHECK_EQ(r.status, 401);
    CHECK(!contains(r.body, "createdBy"));
    // the schema version is the new one
    CHECK_EQ(site.db.scalarText("SELECT value FROM meta WHERE key='schema_version';"), std::string("7"));
    // the importer refuses an old schema with a helpful message
    Database old(":memory:", true);
    old.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); INSERT INTO meta VALUES ('schema_version', '5');");
    try {
        import::importInto(old, import::buildModel(g_gtfs, g_config));
        CHECK(false);
    } catch (const std::exception& e) {
        CHECK(contains(e.what(), "metro_db migrate"));
    }
}


// ---------------------------------------------------------------------------------------------------------------------------
// Line changes, moving end times, the impact preview, the CSV export
static void testMigrationFrom6() {
    Database v6(":memory:", true);   // a database as it was at schema version 6, with a closure in it
    v6.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL); INSERT INTO meta VALUES ('schema_version', '6');"
            "CREATE TABLE station_changes (change_id INTEGER PRIMARY KEY AUTOINCREMENT, gtfs_stop_id TEXT NOT NULL,"
            " kind TEXT NOT NULL CHECK (kind IN ('rename', 'closure')), new_name TEXT CHECK (new_name IS NULL OR length(new_name) BETWEEN 2 AND 60),"
            " reason TEXT NOT NULL CHECK (length(reason) BETWEEN 5 AND 200), start_at INTEGER NOT NULL CHECK (typeof(start_at) = 'integer'),"
            " end_at INTEGER CHECK (end_at IS NULL OR typeof(end_at) = 'integer'), created_at INTEGER NOT NULL CHECK (typeof(created_at) = 'integer'),"
            " created_by TEXT NOT NULL, cancelled_at INTEGER CHECK (cancelled_at IS NULL OR typeof(cancelled_at) = 'integer'), cancelled_by TEXT,"
            " CHECK ((kind = 'rename' AND new_name IS NOT NULL AND end_at IS NULL) OR (kind = 'closure' AND new_name IS NULL)),"
            " CHECK (end_at IS NULL OR end_at > start_at));"
            "INSERT INTO station_changes(gtfs_stop_id, kind, reason, start_at, created_at, created_by) VALUES ('B', 'closure', 'Water on the track', 1, 1, 'bobby');");
    CHECK(contains(migrateDatabase(v6), "6 to 7"));
    CHECK_EQ(v6.scalarText("SELECT value FROM meta WHERE key='schema_version';"), std::string("7"));
    CHECK_EQ(v6.scalarText("SELECT reason FROM station_changes;"), std::string("Water on the track"));       // existing changes are kept
    CHECK_EQ(v6.scalarText("SELECT ifnull(replaces_change_id, 'none') FROM station_changes;"), std::string("none"));
    Database fresh(":memory:", true);
    fresh.exec(readFile(g_schemaPath));
    CHECK(columns(v6, "station_changes") == columns(fresh, "station_changes"));
    CHECK(columns(v6, "line_changes") == columns(fresh, "line_changes"));
    // the line_changes rules came across
    CHECK_THROWS(v6.exec("INSERT INTO line_changes(line_family, kind, reason, start_at, created_at, created_by) VALUES ('RED','delay','no minutes given',1,1,'a');"));
    CHECK_THROWS(v6.exec("INSERT INTO line_changes(line_family, kind, reason, start_at, created_at, created_by) VALUES (NULL,'suspension','which line then',1,1,'a');"));
    v6.exec("INSERT INTO line_changes(line_family, kind, reason, start_at, created_at, created_by) VALUES (NULL,'announcement','For everybody',1,1,'a');");
}

static void testLineChangeRules() {
    Database db(":memory:", true);
    makeDb(db);
    admin::StationAdmin adm(db, [] { return g_now; });
    const long long h = 3600;
    const long long v0 = dataVersion(db);
    using admin::Outcome;
    CHECK(adm.lineChange("BLUE", "suspension", 0, "Power failure on the line", 0, g_now + 2 * h, "bobby").outcome == Outcome::Ok);
    CHECK(adm.lineChange("RED", "delay", 15, "Signal problem near C", 0, g_now + h, "bobby").outcome == Outcome::Ok);
    CHECK(adm.lineChange("", "announcement", 0, "Extra trains on Sunday", 0, 0, "bobby").outcome == Outcome::Ok);          // whole network
    CHECK(adm.lineChange("GREEN", "announcement", 0, "New exit opened at G2", 0, 0, "bobby").outcome == Outcome::Ok);
    CHECK_EQ(dataVersion(db), v0 + 4);
    // refused
    CHECK(adm.lineChange("NOPE", "suspension", 0, "Power failure on the line", 0, 0, "bobby").outcome == Outcome::NotFound);
    CHECK(adm.lineChange("", "suspension", 0, "Power failure on the line", 0, 0, "bobby").outcome == Outcome::Invalid);     // which line?
    CHECK(adm.lineChange("", "delay", 5, "Power failure on the line", 0, 0, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "strike", 0, "Power failure on the line", 0, 0, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "delay", 0, "Signal problem near C", 0, 0, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "delay", 241, "Signal problem near C", 0, 0, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "delay", 240, "Signal problem near C", 0, 0, "bobby").outcome == Outcome::Ok);
    CHECK(adm.lineChange("RED", "suspension", 5, "Minutes make no sense here", 0, 0, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "suspension", 0, "no", 0, 0, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "suspension", 0, "Power failure on the line", g_now, g_now - 1, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "suspension", 0, "Power failure on the line", 0, g_now - 1, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED", "suspension", 0, "Power failure on the line", 0, 0, "").outcome == Outcome::Invalid);
    CHECK(adm.lineChange("RED'; DROP TABLE lines; --", "suspension", 0, "Power failure on the line", 0, 0, "bobby").outcome == Outcome::NotFound);
    CHECK(db.tableExists("lines"));

    // the network loads them; a line that no longer exists, and cancelled ones, are left out
    db.exec("INSERT INTO line_changes(line_family, kind, reason, start_at, created_at, created_by) VALUES ('GONE','suspension','Old line removed',1,1,'bobby');");
    auto net = route::loadNetwork(db);
    int suspensions = 0, delays = 0, announcements = 0;
    for (const auto& lc : net.lineChanges) {
        CHECK(lc.family != "GONE");
        suspensions += lc.kind == "suspension";
        delays += lc.kind == "delay";
        announcements += lc.kind == "announcement";
    }
    CHECK_EQ(suspensions, 1);
    CHECK_EQ(delays, 2);
    CHECK_EQ(announcements, 2);
    CHECK_EQ(route::familyName(net, "BLUE"), std::string("Blue Line"));
    CHECK_EQ(route::servicesOfFamily(net, "BLUE").size() > 0 ? 1 : 0, 1);
    CHECK(route::familyName(net, "NOPE").empty());
    CHECK_EQ(route::lineChangesAt(net, g_now + 3 * h).size(), static_cast<size_t>(3));      // the suspension (2h) and the 1h delay are over by then
    CHECK(route::upcomingLineChanges(net, g_now, h).empty());

    // ending one early
    const int susp = net.lineChanges[0].id;
    CHECK(adm.cancel(admin::Scope::Line, susp, "bobby").outcome == Outcome::Ok);
    CHECK(adm.cancel(admin::Scope::Line, susp, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.cancel(admin::Scope::Line, 99999, "bobby").outcome == Outcome::NotFound);
    for (const auto& lc : route::loadNetwork(db).lineChanges) CHECK(lc.id != susp);
}

static void testExtend() {
    Database db(":memory:", true);
    makeDb(db);
    admin::StationAdmin adm(db, [] { return g_now; });
    using admin::Outcome;
    using admin::Scope;
    const long long h = 3600;
    auto closure = adm.close("B", "Water on the track", 0, g_now + 2 * h, "bobby");
    auto later = adm.extend(Scope::Station, closure.changeId, g_now + 5 * h, "carol");
    CHECK(later.outcome == Outcome::Ok && later.changeId != closure.changeId);
    CHECK(contains(later.message, "later"));
    CHECK_EQ(db.scalarText("SELECT replaces_change_id FROM station_changes WHERE change_id = " + num(later.changeId) + ";"), num(closure.changeId));
    CHECK_EQ(db.scalarText("SELECT cancelled_by FROM station_changes WHERE change_id = " + num(closure.changeId) + ";"), std::string("carol"));
    CHECK_EQ(db.scalarText("SELECT created_by FROM station_changes WHERE change_id = " + num(later.changeId) + ";"), std::string("carol"));
    CHECK_EQ(db.scalarInt("SELECT start_at FROM station_changes WHERE change_id = " + num(later.changeId) + ";"),
             db.scalarInt("SELECT start_at FROM station_changes WHERE change_id = " + num(closure.changeId) + ";"));     // the start is kept
    auto net = route::loadNetwork(db);
    auto now0 = route::closuresAt(net, g_now + 3 * h);                         // the old end has passed, the new one has not
    CHECK_EQ(now0.size(), static_cast<size_t>(1));
    CHECK(adm.extend(Scope::Station, closure.changeId, g_now + 6 * h, "bobby").outcome == Outcome::Invalid);     // the replaced row cannot be moved
    auto earlier = adm.extend(Scope::Station, later.changeId, g_now + h, "bobby");
    CHECK(earlier.outcome == Outcome::Ok && contains(earlier.message, "earlier"));
    auto forever = adm.extend(Scope::Station, earlier.changeId, 0, "bobby");
    CHECK(forever.outcome == Outcome::Ok);
    CHECK_EQ(db.scalarText("SELECT ifnull(end_at, 'none') FROM station_changes WHERE change_id = " + num(forever.changeId) + ";"), std::string("none"));
    CHECK(adm.extend(Scope::Station, forever.changeId, 0, "bobby").outcome == Outcome::Invalid);                 // already until further notice
    CHECK(adm.extend(Scope::Station, forever.changeId, g_now - 10, "bobby").outcome == Outcome::Invalid);        // in the past
    CHECK(adm.extend(Scope::Station, forever.changeId, g_now + 4000LL * 24 * h, "bobby").outcome == Outcome::Invalid);
    CHECK(adm.extend(Scope::Station, 99999, g_now + h, "bobby").outcome == Outcome::NotFound);
    auto rename = adm.rename("C", "Central Gate", "New name after the rebuild", "bobby");
    CHECK(adm.extend(Scope::Station, rename.changeId, g_now + h, "bobby").outcome == Outcome::Invalid);          // a rename has no end
    auto shortOne = adm.close("D", "Short signal problem", 0, g_now + h, "bobby");
    g_now += 2 * h;
    CHECK(adm.extend(Scope::Station, shortOne.changeId, g_now + h, "bobby").outcome == Outcome::Invalid);        // it already ended
    g_now -= 2 * h;
    // a line change too
    auto delay = adm.lineChange("RED", "delay", 10, "Signal problem near C", 0, g_now + h, "bobby");
    auto delay2 = adm.extend(Scope::Line, delay.changeId, g_now + 3 * h, "bobby");
    CHECK(delay2.outcome == Outcome::Ok);
    CHECK_EQ(db.scalarInt("SELECT delay_minutes FROM line_changes WHERE change_id = " + num(delay2.changeId) + ";"), 10LL);   // everything else copied
    CHECK_EQ(db.scalarText("SELECT replaces_change_id FROM line_changes WHERE change_id = " + num(delay2.changeId) + ";"), num(delay.changeId));
    // the combined history has both tables, newest first
    const auto all = adm.historyAll();
    CHECK(all.size() >= 8);
    for (size_t i = 1; i < all.size(); ++i) CHECK(all[i - 1].createdAt >= all[i].createdAt);
    bool sawLine = false, sawStation = false;
    for (const auto& r : all) {
        sawLine = sawLine || r.scope == Scope::Line;
        sawStation = sawStation || r.scope == Scope::Station;
    }
    CHECK(sawLine && sawStation);
}

static void testSuspendedRouting() {
    Database db(":memory:", true);
    makeDb(db);
    const auto net = route::loadNetwork(db);
    route::Router router(net);
    route::FareCalculator fares(net);
    route::Planner planner(net, router, fares);
    const int A = stationId(net, "A"), C = stationId(net, "C"), D = stationId(net, "D"), F = stationId(net, "F"), L = stationId(net, "L");
    auto plan = [&](int from, int to) { return planner.plan(from, to, route::DayType::Weekday, route::Preference::Fastest).found; };
    CHECK(plan(A, L) && plan(C, F));
    planner.setSuspendedServices(route::servicesOfFamily(net, "BLUE"));
    CHECK(!plan(A, L));               // only the Blue line reaches L
    CHECK(!plan(F, L));               // stations served only by Blue have no trains
    CHECK(plan(A, D));                // the Red line is fine
    CHECK(plan(C, A));                // C still has the Red line
    for (const auto& cand : planner.candidates(A, D, route::DayType::Weekday))
        for (const auto& s : cand.route.steps)
            if (s.kind == route::Step::Kind::Ride) CHECK(net.service(s.service).family != "BLUE");
    planner.setSuspendedServices({});
    CHECK(plan(A, L));
    // stationUsable agrees
    route::Restrictions r;
    r.bannedServices = route::servicesOfFamily(net, "BLUE");
    CHECK(route::stationUsable(net, r, C) && !route::stationUsable(net, r, F) && route::stationUsable(net, route::Restrictions(), F));
    r.closedStations = {A};
    CHECK(!route::stationUsable(net, r, A));
}

// The impact numbers must match a slow, obvious count done journey by journey with the planner's own router.
static void checkImpactAgainstBruteForce(const route::Network& net, const route::Router& router, const route::Restrictions& cur, const route::Restrictions& prop) {
    const auto rep = route::assessImpact(net, router, cur, prop);
    const int n = static_cast<int>(net.stations.size());
    int lost = 0, slower = 0, same = 0, endpoint = 0, without = 0;
    long long extra = 0;
    for (int s = 1; s <= n; ++s) without += route::stationUsable(net, cur, s) && !route::stationUsable(net, prop, s);
    for (int o = 1; o <= n; ++o)
        for (int d = 1; d <= n; ++d) {
            if (o == d || !route::stationUsable(net, cur, o) || !route::stationUsable(net, cur, d)) continue;
            const auto before = router.find(o, d, route::Mode::Fastest, cur);
            if (!before.found) continue;
            if (!route::stationUsable(net, prop, o) || !route::stationUsable(net, prop, d)) {
                ++endpoint;
                continue;
            }
            const auto after = router.find(o, d, route::Mode::Fastest, prop);
            if (!after.found) ++lost;
            else if (after.totalSeconds > before.totalSeconds) {
                ++slower;
                extra += after.totalSeconds - before.totalSeconds;
            } else ++same;
            CHECK_EQ(rep.before[static_cast<size_t>(o)][static_cast<size_t>(d)], before.totalSeconds);
        }
    CHECK_EQ(rep.lost, lost);
    CHECK_EQ(rep.slower, slower);
    CHECK_EQ(rep.unaffected, same);
    CHECK_EQ(rep.endpointPairs, endpoint);
    CHECK_EQ(rep.extraSecondsTotal, extra);
    CHECK_EQ(static_cast<int>(rep.stationsWithoutTrains.size()), without);
    CHECK_EQ(rep.pairsChecked, lost + slower + same);
}

static void testImpact() {
    Database db(":memory:", true);
    makeDb(db);
    const auto net = route::loadNetwork(db);
    route::Router router(net);
    const int B = stationId(net, "B"), C = stationId(net, "C"), F = stationId(net, "F"), J = stationId(net, "J"), S1 = stationId(net, "S1"), E = stationId(net, "E");
    route::Restrictions none, closeC, closeF, blue, closeS1, closeBthenC, closeE;
    closeC.closedStations = {C};
    closeF.closedStations = {F};
    blue.bannedServices = route::servicesOfFamily(net, "BLUE");
    closeS1.closedStations = {S1};
    closeE.closedStations = {E};
    closeBthenC.closedStations = {B, C};
    route::Restrictions closeB;
    closeB.closedStations = {B};
    checkImpactAgainstBruteForce(net, router, none, closeC);       // the only Red/Blue change station
    checkImpactAgainstBruteForce(net, router, none, closeF);       // trains pass through F
    checkImpactAgainstBruteForce(net, router, none, blue);         // a whole line
    checkImpactAgainstBruteForce(net, router, none, closeS1);      // the end of a walking link
    checkImpactAgainstBruteForce(net, router, none, closeE);
    checkImpactAgainstBruteForce(net, router, closeB, closeBthenC); // something already closed: only the NEW change counts
    route::Restrictions blueAndJ = blue;
    blueAndJ.closedStations = {J};
    checkImpactAgainstBruteForce(net, router, blue, blueAndJ);     // closing a station on a line that is already suspended changes nothing
    const auto c = route::assessImpact(net, router, none, closeC);
    CHECK(c.lost > 0 && c.endpointPairs > 0);                      // the test feed really exercises both
    CHECK_EQ(c.stationsWithoutTrains.size(), static_cast<size_t>(1));
    const auto b = route::assessImpact(net, router, none, blue);
    CHECK_EQ(b.stationsWithoutTrains.size(), static_cast<size_t>(5));   // F, J, K, L, M
    const auto same = route::assessImpact(net, router, blue, blueAndJ);
    CHECK_EQ(same.lost + same.slower + same.endpointPairs, 0);
}

static void testLineApi() {
    TestSite site;
    site.request("POST", "/api/register", "username=alice&password=correct+horse");
    site.request("POST", "/api/register", "username=bobby&password=another+good+pw");
    site.accounts->setRole("bobby", "admin");
    const std::string alice = site.cookieFor("alice", "correct+horse"), bobby = site.cookieFor("bobby", "another+good+pw");
    const std::string A = num(site.id("A")), C = num(site.id("C")), D = num(site.id("D")), F = num(site.id("F")), L = num(site.id("L")), K = num(site.id("K"));
    auto post = [&](const std::string& what, const std::string& form) { return site.request("POST", "/api/admin/" + what, form, bobby); };

    // who may use the new endpoints
    for (const std::string& path : std::vector<std::string>{"/api/admin/preview?station=" + C, "/api/admin/history.csv"}) {
        CHECK_EQ(site.get(path).status, 401);
        CHECK_EQ(site.get(path, alice).status, 403);
        CHECK_EQ(site.get(path, bobby, "203.0.113.9").status, 403);
        CHECK_EQ(site.get(path, bobby).status, 200);
    }
    for (const std::string& what : std::vector<std::string>{"line", "extend"}) {
        CHECK_EQ(site.request("POST", "/api/admin/" + what, "kind=suspension&line=BLUE&reason=Power+failure", "").status, 401);
        CHECK_EQ(site.request("POST", "/api/admin/" + what, "kind=suspension&line=BLUE&reason=Power+failure", alice).status, 403);
        CHECK_EQ(site.request("POST", "/api/admin/" + what, "kind=suspension&line=BLUE&reason=Power+failure", bobby, "", false).status, 403);   // CSRF
    }
    CHECK_EQ(site.db.scalarInt("SELECT COUNT(*) FROM line_changes;"), 0LL);

    // the preview changes nothing and matches the impact module
    auto saved = site.request("POST", "/api/journeys", "from=" + A + "&to=" + L + "&mode=fastest&day=weekday", alice);
    CHECK_EQ(saved.status, 201);
    const long long v0 = site.api->dataVersion();
    auto pv = site.get("/api/admin/preview?station=" + C, bobby);
    CHECK_EQ(pv.status, 200);
    CHECK(contains(pv.body, "\"kind\":\"station\"") && contains(pv.body, "\"alreadyInForce\":false") && contains(pv.body, "\"savedJourneysAffected\":1"));
    {
        auto net = route::loadNetwork(site.db);
        route::Router router(net);
        route::Restrictions none, prop;
        prop.closedStations = {site.id("C")};
        const auto rep = route::assessImpact(net, router, none, prop);
        CHECK(contains(pv.body, "\"lost\":" + num(rep.lost) + ","));
        CHECK(contains(pv.body, "\"endpointPairs\":" + num(rep.endpointPairs)));
    }
    CHECK_EQ(site.api->dataVersion(), v0);
    CHECK_EQ(site.db.scalarInt("SELECT COUNT(*) FROM station_changes;"), 0LL);
    auto pl = site.get("/api/admin/preview?line=BLUE", bobby);
    CHECK(contains(pl.body, "\"kind\":\"line\"") && contains(pl.body, "\"target\":\"Blue Line\"") && contains(pl.body, "\"stationsWithoutTrainsCount\":5"));
    CHECK_EQ(site.get("/api/admin/preview", bobby).status, 400);
    CHECK_EQ(site.get("/api/admin/preview?station=abc", bobby).status, 400);
    CHECK_EQ(site.get("/api/admin/preview?line=NOPE", bobby).status, 404);
    CHECK_EQ(site.request("POST", "/api/admin/preview?station=" + C, "", bobby).status, 405);

    // suspend the Blue line
    CHECK_EQ(post("line", "kind=suspension&line=NOPE&reason=Power+failure+on+the+line").status, 404);
    CHECK_EQ(post("line", "kind=strike&line=BLUE&reason=Power+failure+on+the+line").status, 400);
    CHECK_EQ(post("line", "kind=delay&line=RED&minutes=abc&reason=Signal+problem").status, 400);
    CHECK_EQ(post("line", "kind=delay&line=RED&minutes=0&reason=Signal+problem").status, 400);
    auto susp = post("line", "kind=suspension&line=BLUE&reason=Power+failure+on+the+line&end=" + num(g_now + 7200));
    CHECK_EQ(susp.status, 200);
    CHECK(contains(site.get("/api/admin/preview?line=BLUE", bobby).body, "\"alreadyInForce\":true"));
    const std::string stations = site.get("/api/stations").body;
    CHECK(contains(stations, "\"noTrains\":{\"lines\":[\"Blue Line\"],\"until\":" + num(g_now + 7200) + "}"));
    auto rf = site.get("/api/route?from=" + F + "&to=" + D);
    CHECK(contains(rf.body, "\"found\":false") && contains(rf.body, "No trains are running at Station F: the Blue Line is suspended.") && contains(rf.body, "\"noTrains\":{"));
    auto ral = site.get("/api/route?from=" + A + "&to=" + L);     // L has no trains either
    CHECK(contains(ral.body, "\"found\":false"));
    auto rad = site.get("/api/route?from=" + A + "&to=" + D);
    CHECK(contains(rad.body, "\"found\":true") && contains(rad.body, "\"suspended\":[{") && contains(rad.body, "Power failure on the line"));
    const size_t cAt = stations.find("\"name\":\"Station C\"");
    CHECK(cAt != std::string::npos && stations.substr(cAt, stations.find('}', stations.find("\"lines\"", cAt)) - cAt).find("noTrains") == std::string::npos);   // C still has Red
    CHECK(contains(site.get("/api/notices").body, "\"lines\":[{") && contains(site.get("/api/notices").body, "\"kind\":\"suspension\""));
    CHECK(contains(site.get("/api/journeys", alice).body, "\"current\":false"));      // the saved journey knows the data changed

    // a delay is information only: the route stays the same, and carries the note
    const std::string before = site.get("/api/route?from=" + A + "&to=" + D).body;
    auto delay = post("line", "kind=delay&line=RED&minutes=10&reason=Signal+problem+near+C&end=" + num(g_now + 3600));
    CHECK_EQ(delay.status, 200);
    const std::string after = site.get("/api/route?from=" + A + "&to=" + D).body;
    CHECK(contains(after, "\"delays\":[{") && contains(after, "\"minutes\":10"));
    auto routePart = [](const std::string& b) { const size_t a = b.find("\"primary\""); return b.substr(a, b.find("\"steps\"") - a); };
    CHECK_EQ(routePart(before), routePart(after));       // same time, changes and fare (only the data version moved on)
    CHECK(contains(site.get("/api/route?from=" + num(site.id("G1")) + "&to=" + num(site.id("G3"))).body, "\"delays\":[]"));   // other lines: no note
    // a network-wide announcement
    CHECK_EQ(post("line", "kind=announcement&line=&reason=Extra+trains+on+Sunday").status, 200);
    CHECK(contains(site.get("/api/notices").body, "\"kind\":\"announcement\",\"line\":null"));

    // moving an end, and ending early
    auto getId = [](const std::string& body) { return std::atoi(body.c_str() + body.find("\"changeId\":") + 11); };
    const int suspId = getId(susp.body);
    CHECK_EQ(post("extend", "scope=line&change=" + num(suspId) + "&end=").status, 400);
    CHECK_EQ(post("extend", "scope=line&change=" + num(suspId) + "&end=abc").status, 400);
    CHECK_EQ(post("extend", "scope=other&change=" + num(suspId) + "&end=never").status, 400);
    auto ext = post("extend", "scope=line&change=" + num(suspId) + "&end=" + num(g_now + 10800));
    CHECK_EQ(ext.status, 200);
    const std::string ov = site.get("/api/admin/overview", bobby).body;
    CHECK(contains(ov, "\"status\":\"replaced\"") && contains(ov, "\"replaces\":" + num(suspId)));
    CHECK(contains(site.get("/api/stations").body, "\"until\":" + num(g_now + 10800)));
    auto ext2 = post("extend", "scope=line&change=" + num(getId(ext.body)) + "&end=never");
    CHECK_EQ(ext2.status, 200);
    CHECK(contains(site.get("/api/stations").body, "\"noTrains\":{\"lines\":[\"Blue Line\"],\"until\":null}"));
    CHECK_EQ(post("cancel", "scope=line&change=" + num(getId(ext2.body))).status, 200);
    CHECK(!contains(site.get("/api/stations").body, "noTrains"));
    CHECK(contains(site.get("/api/route?from=" + A + "&to=" + L).body, "\"found\":true"));
    CHECK(contains(site.get("/api/route?from=" + C + "&to=" + K).body, "\"suspended\":[]"));
    CHECK_EQ(post("cancel", "scope=line&change=999").status, 404);
    // a station closure can be moved through the API too
    auto cl = post("close", "station=" + C + "&reason=Escalator+works&end=" + num(g_now + 3600));
    CHECK_EQ(post("extend", "change=" + num(getId(cl.body)) + "&end=" + num(g_now + 7200)).status, 200);    // scope defaults to station

    // the CSV export: a file, safe to open in a spreadsheet
    CHECK_EQ(post("close", "station=" + D + "&reason=%3DHYPERLINK(%22http%3A%2F%2Fevil%22)&end=" + num(g_now + 3600)).status, 200);
    auto csv = site.get("/api/admin/history.csv", bobby);
    CHECK_EQ(csv.status, 200);
    CHECK(contains(csv.contentType, "text/csv"));
    bool attachment = false;
    for (const auto& hd : csv.headers) attachment = attachment || (hd.first == "Content-Disposition" && contains(hd.second, "attachment"));
    CHECK(attachment);
    CHECK_EQ(csv.body.substr(0, 3), std::string("\xEF\xBB\xBF"));
    CHECK(contains(csv.body, "scope,id,kind,station_or_line,new_name,delay_minutes,reason,start,end,made,made_by,cancelled,cancelled_by,replaces,status\r\n"));
    CHECK(contains(csv.body, "\"'=HYPERLINK(\"\"http://evil\"\")\""));          // a formula is defused, quotes doubled
    CHECK(!contains(csv.body, ",\"=HYPERLINK"));
    CHECK(contains(csv.body, "\"line\"") && contains(csv.body, "\"Blue Line\"") && contains(csv.body, "\"All lines\"") && contains(csv.body, "\"replaced\""));
    CHECK(contains(csv.body, "\"until further notice\""));

    // the preview starts from what is ALREADY in force (C and D are closed now), and counts only what the new change adds
    {
        auto pvB = site.get("/api/admin/preview?station=" + num(site.id("B")), bobby);
        auto net = route::loadNetwork(site.db);
        route::Router router(net);
        route::Restrictions cur, prop;
        cur.closedStations = {site.id("C"), site.id("D")};
        prop.closedStations = {site.id("C"), site.id("D"), site.id("B")};
        const auto rep = route::assessImpact(net, router, cur, prop);
        CHECK(contains(pvB.body, "\"endpointPairs\":" + num(rep.endpointPairs) + ","));
        CHECK(contains(pvB.body, "\"pairsChecked\":" + num(rep.pairsChecked) + ","));
        const auto naive = route::assessImpact(net, router, route::Restrictions(), prop);
        CHECK(naive.endpointPairs != rep.endpointPairs);       // so this check can tell the two apart
        CHECK(contains(site.get("/api/admin/preview?station=" + C, bobby).body, "\"alreadyInForce\":true"));
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_admin SCHEMA.sql GTFS_FOLDER CONFIG_FOLDER\n";
        return 2;
    }
    g_schemaPath = argv[1];
    g_gtfs = argv[2];
    g_config = argv[3];
    try {
        testMigration();
        testRoles();
        testStationAdminRules();
        testClosureTimes();
        testRoutingAroundClosures();
        testAdminPermissions();
        testAdminRenameAndClose();
        testClosuresAndSavedJourneys();
        testChangesSurviveReimport();
        testPublicSurface();
        testMigrationFrom6();
        testLineChangeRules();
        testExtend();
        testSuspendedRouting();
        testImpact();
        testLineApi();
    } catch (const std::exception& e) {
        std::cerr << "test_admin: unexpected exception: " << e.what() << "\n";
        return 1;
    }
    return finish("test_admin");
}
