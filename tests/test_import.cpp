#include <filesystem>
#include <fstream>
#include <sstream>

#include "../src/common/db.h"
#include "../src/import/importer.h"
#include "check.h"

namespace fs = std::filesystem;
using metro::Database;
using namespace metro::import;

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Text snapshot of every network table, in a fixed order, to compare "before" and "after".
static std::string dump(Database& db) {
    std::string out;
    for (const char* q : {
             "SELECT station_id||'|'||gtfs_stop_id||'|'||name||'|'||lat||'|'||lon FROM stations ORDER BY station_id",
             "SELECT line_id||'|'||family||'|'||name||'|'||ifnull(colour,'')||'|'||operator||'|'||gtfs_route_ids FROM lines ORDER BY line_id",
             "SELECT line_id||'|'||seq||'|'||station_id FROM line_stops ORDER BY line_id,seq",
             "SELECT line_id||'|'||from_station||'|'||to_station||'|'||run_seconds||'|'||ifnull(distance_m,'') FROM segments ORDER BY segment_id",
             "SELECT from_station||'|'||from_line||'|'||to_station||'|'||to_line||'|'||walk_seconds FROM transfers "
             "ORDER BY from_station,from_line,to_station,to_line",
             "SELECT operator||'|'||day_type||'|'||ticket||'|'||stop_a||'|'||stop_b||'|'||fare_inr||'|'||valid_from||'|'||source FROM fare_pairs ORDER BY pair_id",
             "SELECT operator||'|'||day_type||'|'||ticket||'|'||min_km||'|'||max_km||'|'||fare_inr||'|'||valid_from||'|'||source FROM fare_slabs ORDER BY slab_id"}) {
        for (const auto& s : db.columnText(q)) out += s + "\n";
        out += "--\n";
    }
    return out;
}

static bool importFeed(Database& db, const std::string& gtfs, const std::string& config) {
    importInto(db, buildModel(gtfs, config));
    return true;
}

static int run(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: test_import <schema.sql> <import_gtfs> <import_config>\n";
        return 2;
    }
    const std::string schema = readFile(argv[1]);
    const std::string gtfs = argv[2], config = argv[3];
    CHECK(!schema.empty());

    // ---- 1. the model built from the designed feed ------------------------------------------
    Model m = buildModel(gtfs, config);
    CHECK_EQ(m.stations.size(), static_cast<size_t>(17));
    CHECK_EQ(m.services.size(), static_cast<size_t>(5));      // Red, Blue x2 (branches), Green (joined), Rapid
    CHECK_EQ(m.segments.size(), static_cast<size_t>(16));
    CHECK_EQ(m.transfers.size(), static_cast<size_t>(12));    // 10 automatic + 2 from the confirmed seed
    CHECK_EQ(m.dwellSeconds, 20);
    CHECK_EQ(m.interchangeWalkSeconds, 200);
    CHECK_EQ(m.fareSlabs.size(), static_cast<size_t>(6));
    CHECK_EQ(m.farePairs.size(), static_cast<size_t>(1));
    CHECK_EQ(m.farePairs[0].stopA, std::string("S1"));     // stored in a fixed order whichever way round the file lists it
    CHECK_EQ(m.farePairs[0].stopB, std::string("S2"));
    CHECK_EQ(m.stationOverridesApplied, 0);
    CHECK_EQ(m.similarTimeSeconds, 180);                      // the planner's defaults when settings.csv does not mention them
    CHECK_EQ(m.maxRoutesPerGroup, 3);
    CHECK_EQ(m.maxSlowerPercent, 25);
    CHECK_EQ(m.distanceOverridesApplied, 0);
    CHECK_EQ(m.seedsImported, 1);
    CHECK_EQ(m.seedsSkipped, 1);                              // the 'no' row is not imported
    CHECK_EQ(m.feedValidUntil, std::string("20991231"));
    CHECK_EQ(m.componentSizes.size(), static_cast<size_t>(2));
    CHECK_EQ(m.componentSizes[0], 12);                        // Red + Blue + Rapid (linked by the seed)
    CHECK_EQ(m.componentSizes[1], 5);                         // Green stands alone

    // Red: 3 routes (one reversed, one short-turn) become ONE service.
    CHECK_EQ(m.services[0].name, std::string("Red Line"));
    CHECK_EQ(m.services[0].stations.size(), static_cast<size_t>(5));
    CHECK_EQ(m.services[0].routeIds.size(), static_cast<size_t>(3));
    // Blue: two branches that each have stops the other lacks stay TWO services.
    CHECK_EQ(m.services[1].name, std::string("Blue Line: Station C to Station L"));
    CHECK_EQ(m.services[2].name, std::string("Blue Line: Station C to Station M"));
    CHECK_EQ(m.services[1].stations.size(), static_cast<size_t>(5));
    CHECK_EQ(m.services[2].stations.size(), static_cast<size_t>(4));
    // Green: two services meeting end to end are joined (flag set in line_map.csv).
    CHECK_EQ(m.services[3].name, std::string("Green Line"));
    CHECK_EQ(m.services[3].stations.size(), static_cast<size_t>(5));
    bool joinNote = false;
    for (const auto& n : m.notes)
        if (n.find("joined two services end to end") != std::string::npos) joinNote = true;
    CHECK(joinNote);

    // Segment values: the B-C hop is seen as 100, 140, 180, 140 and 120 s -> median 140; 1500 m.
    bool foundBC = false, foundOther = false;
    for (const auto& s : m.segments) {
        bool bc = (s.from == 2 && s.to == 3) || (s.from == 3 && s.to == 2);
        if (s.service == 1 && bc) {
            foundBC = true;
            CHECK_EQ(s.runSeconds, 140);
            CHECK_EQ(s.distanceM, 1500);
        }
        if (s.service == 1 && s.from == 3 && s.to == 4) {
            foundOther = true;
            CHECK_EQ(s.runSeconds, 120);
            CHECK_EQ(s.distanceM, 1000);
        }
    }
    CHECK(foundBC);
    CHECK(foundOther);

    // Transfers at station C (id 3): Red, Blue-A and Blue-B all stop there -> 6 ordered pairs at 200 s.
    int atC = 0, seedRows = 0;
    for (const auto& t : m.transfers) {
        if (t.fromStation == 3 && t.toStation == 3) {
            ++atC;
            CHECK_EQ(t.walkSeconds, 200);
        }
        if (t.fromStation != t.toStation) {
            ++seedRows;
            CHECK_EQ(t.walkSeconds, 150);
        }
    }
    CHECK_EQ(atC, 6);
    CHECK_EQ(seedRows, 2);

    // ---- 2. writing it to a database -----------------------------------------------------
    Database db(":memory:", true);
    db.exec(schema);
    CHECK_EQ(importInto(db, m), 1);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM stations;"), 17LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM lines;"), 5LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM line_stops;"), 21LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM segments;"), 16LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM transfers;"), 12LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM fare_slabs;"), 6LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM fare_pairs;"), 1LL);
    CHECK_EQ(db.scalarText("SELECT value FROM meta WHERE key='data_version';"), std::string("1"));
    CHECK_EQ(db.scalarText("SELECT value FROM meta WHERE key='dwell_seconds';"), std::string("20"));
    CHECK(db.columnText("PRAGMA foreign_key_check;").empty());

    // ---- 3. importing the same feed twice gives an identical network; users survive --------
    db.exec("INSERT INTO users(user_id,username,password_hash,created_at) VALUES (1,'tourist','x',1000);");
    db.exec("INSERT INTO saved_journeys(user_id,from_gtfs_stop_id,to_gtfs_stop_id,mode,day_type,data_version,total_seconds,changes,result_text,created_at,updated_at) "
            "VALUES (1,'A','L','fastest','weekday',1,1000,1,'x',1000,1000);");
    const std::string first = dump(db);
    CHECK_EQ(importInto(db, buildModel(gtfs, config)), 2);
    CHECK_EQ(dump(db), first);
    CHECK_EQ(db.scalarText("SELECT value FROM meta WHERE key='data_version';"), std::string("2"));
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM users;"), 1LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM saved_journeys;"), 1LL);

    // ---- 4. a broken feed or config changes nothing ------------------------------------------
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec || tmp.empty()) tmp = fs::current_path();
    tmp /= "metro_test_import_work";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "gtfs", ec);
    fs::create_directories(tmp / "config", ec);

    const std::string before = dump(db);

    // 4a. a route whose line prefix is not in line_map.csv
    fs::copy(gtfs, tmp / "gtfs", fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
    {
        std::string routes = readFile(tmp / "gtfs" / "routes.txt");
        size_t pos = routes.find("RED_A to E");
        CHECK(pos != std::string::npos);
        routes.replace(pos, 3, "PURPLE");
        writeFile(tmp / "gtfs" / "routes.txt", routes);
    }
    bool threw = false;
    try {
        importFeed(db, (tmp / "gtfs").string(), config);
    } catch (const ImportError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("PURPLE") != std::string::npos);
    }
    CHECK(threw);
    CHECK_EQ(dump(db), before);
    CHECK_EQ(db.scalarText("SELECT value FROM meta WHERE key='data_version';"), std::string("2"));

    // 4b. a confirmed transfer that names a stop that does not exist
    fs::copy(config, tmp / "config", fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
    writeFile(tmp / "config" / "transfers_seed.csv",
              "stop_id_a,stop_id_b,walk_seconds,confirmed,note\nA,NOPE,100,yes,bad\n");
    threw = false;
    try {
        importFeed(db, gtfs, (tmp / "config").string());
    } catch (const ImportError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("NOPE") != std::string::npos);
    }
    CHECK(threw);
    CHECK_EQ(dump(db), before);

    // 4c. a missing feed file
    CHECK_THROWS(buildModel("definitely_not_a_folder_metro", config));

    // 4d. a failure in the middle of writing (a segment pointing at a service that does not exist)
    //     must roll the whole import back.
    Model bad = buildModel(gtfs, config);
    bad.segments.push_back({99, 1, 2, 60, 500});
    CHECK_THROWS(importInto(db, bad));
    CHECK_EQ(dump(db), before);
    CHECK_EQ(db.scalarText("SELECT value FROM meta WHERE key='data_version';"), std::string("2"));

    // 4e. a database with the wrong schema version is refused
    db.exec("UPDATE meta SET value='1' WHERE key='schema_version';");
    threw = false;
    try {
        importInto(db, m);
    } catch (const ImportError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("metro_db init") != std::string::npos);
    }
    CHECK(threw);
    CHECK_EQ(dump(db), before);

    // ---- 5. fare slabs and distance corrections ---------------------------------------------------
    auto writeConfig = [&](const std::string& file, const std::string& text) {
        fs::remove_all(tmp / "config", ec);                 // start from the good config each time, so earlier broken files do not linger
        fs::create_directories(tmp / "config", ec);
        fs::copy(config, tmp / "config", fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
        writeFile(tmp / "config" / file, text);
    };
    auto errorFrom = [&](const std::string& what) -> std::string {
        try {
            buildModel(gtfs, (tmp / "config").string());
        } catch (const ImportError& e) {
            return e.what();
        }
        (void)what;
        return "";
    };
    const std::string slabHeader = "operator,day_type,ticket,min_km,max_km,fare_inr,valid_from,source\n";

    writeConfig("fare_slabs.csv", slabHeader + "OP_A,weekday,token,0,2,10,2025-01-01,x\nOP_A,weekday,token,3,5,20,2025-01-01,x\n");
    CHECK(errorFrom("gap").find("gap or an overlap") != std::string::npos);          // slabs must be contiguous
    writeConfig("fare_slabs.csv", slabHeader + "OP_A,weekday,token,1,2,10,2025-01-01,x\n");
    CHECK(errorFrom("start").find("must start at 0") != std::string::npos);
    writeConfig("fare_slabs.csv", slabHeader + "OP_A,weekday,token,0,2,10,2025-01-01,\n");
    CHECK(errorFrom("source").find("source must not be empty") != std::string::npos); // every number needs a source
    writeConfig("fare_slabs.csv", slabHeader + "OP_A,monday,token,0,2,10,2025-01-01,x\n");
    CHECK(errorFrom("day").find("day_type") != std::string::npos);
    writeConfig("fare_slabs.csv", slabHeader + "OP_A,weekday,token,0,2,10,2025-01-01,x\nOP_A,weekday,token,2,5,20,2025-06-01,x\n");
    CHECK(errorFrom("dates").find("valid_from") != std::string::npos);

    // a missing fare file only produces a warning: the network still imports, with no slabs
    fs::remove(tmp / "config" / "fare_slabs.csv", ec);
    Model noFares = buildModel(gtfs, (tmp / "config").string());
    CHECK_EQ(noFares.fareSlabs.size(), static_cast<size_t>(0));
    bool warned = false;
    for (const auto& w : noFares.warnings)
        if (w.find("no fares can be computed") != std::string::npos) warned = true;
    CHECK(warned);

    const std::string overrideHeader = "stop_id_a,stop_id_b,distance_m,source\n";
    writeConfig("distance_overrides.csv", overrideHeader + "B,C,1600,test source\n");
    Model fixed = buildModel(gtfs, (tmp / "config").string());
    CHECK_EQ(fixed.distanceOverridesApplied, 1);
    bool corrected = false;
    for (const auto& sg : fixed.segments)
        if (sg.service == 1 && ((sg.from == 2 && sg.to == 3) || (sg.from == 3 && sg.to == 2))) corrected = (sg.distanceM == 1600);
    CHECK(corrected);
    bool noted = false;
    for (const auto& note : fixed.notes)
        if (note.find("Distance correction") != std::string::npos && note.find("test source") != std::string::npos) noted = true;
    CHECK(noted);

    writeConfig("distance_overrides.csv", overrideHeader + "B,NOPE,1600,test source\n");
    CHECK(errorFrom("unknown").find("NOPE") != std::string::npos);
    writeConfig("distance_overrides.csv", overrideHeader + "A,E,1600,test source\n");          // A and E are not neighbours
    CHECK(errorFrom("hop").find("no hop joins") != std::string::npos);
    writeConfig("distance_overrides.csv", overrideHeader + "B,C,1600,\n");
    CHECK(errorFrom("nosrc").find("source must not be empty") != std::string::npos);

    // a hop that takes real time but has almost no distance is flagged as probably wrong
    writeConfig("distance_overrides.csv", overrideHeader + "A,B,1,deliberately wrong\n");
    Model suspicious = buildModel(gtfs, (tmp / "config").string());
    bool flagged = false;
    for (const auto& w : suspicious.warnings)
        if (w.find("almost no distance") != std::string::npos) flagged = true;
    CHECK(flagged);
    bool flaggedNormally = false;
    for (const auto& w : m.warnings)
        if (w.find("almost no distance") != std::string::npos) flaggedNormally = true;
    CHECK(!flaggedNormally);                                                                     // and not flagged for good data

    // ---- 6. fare pairs and coordinate corrections ---------------------------------------------------
    const std::string pairHeader = "operator,day_type,ticket,stop_id_a,stop_id_b,fare_inr,valid_from,source\n";
    writeConfig("fare_pairs.csv", pairHeader + "OP_C,any,token,S1,S2,25,2025,x\nOP_C,any,token,S2,S1,30,2025,x\n");
    CHECK(errorFrom("dup").find("listed twice") != std::string::npos);                       // the same pair, either way round
    writeConfig("fare_pairs.csv", pairHeader + "OP_C,any,token,S1,NOPE,25,2025,x\n");
    CHECK(errorFrom("stop").find("NOPE") != std::string::npos);
    writeConfig("fare_pairs.csv", pairHeader + "OP_C,any,token,S1,S1,25,2025,x\n");
    CHECK(errorFrom("same").find("two different stops") != std::string::npos);
    writeConfig("fare_pairs.csv", pairHeader + "OP_C,any,token,S1,S2,25,2025,\n");
    CHECK(errorFrom("src").find("source must not be empty") != std::string::npos);
    writeConfig("fare_pairs.csv", pairHeader + "OP_C,monday,token,S1,S2,25,2025,x\n");
    CHECK(errorFrom("day").find("day_type") != std::string::npos);

    const std::string coordHeader = "stop_id,lat,lon,source\n";
    writeConfig("station_overrides.csv", coordHeader + "B,28.123456,77.654321,test source\n");
    Model moved = buildModel(gtfs, (tmp / "config").string());
    CHECK_EQ(moved.stationOverridesApplied, 1);
    CHECK(moved.stations[1].lat == 28.123456);
    CHECK(moved.stations[1].lon == 77.654321);
    bool coordNote = false;
    for (const auto& note : moved.notes)
        if (note.find("Coordinate correction") != std::string::npos && note.find("test source") != std::string::npos) coordNote = true;
    CHECK(coordNote);
    writeConfig("station_overrides.csv", coordHeader + "NOPE,28.1,77.1,x\n");
    CHECK(errorFrom("c1").find("NOPE") != std::string::npos);
    writeConfig("station_overrides.csv", coordHeader + "B,128.1,77.1,x\n");
    CHECK(errorFrom("c2").find("valid coordinates") != std::string::npos);
    writeConfig("station_overrides.csv", coordHeader + "B,28.1,77.1,\n");
    CHECK(errorFrom("c3").find("source must not be empty") != std::string::npos);

    // ---- 7. journey planner settings ------------------------------------------------------------------
    writeConfig("settings.csv", "key,value,status,note\ninterchange_walk_seconds,200,x,x\nsimilar_time_seconds,60,x,x\nmax_routes_per_group,5,x,x\nmax_slower_percent,80,x,x\n");
    Model tuned = buildModel(gtfs, (tmp / "config").string());
    CHECK_EQ(tuned.similarTimeSeconds, 60);
    CHECK_EQ(tuned.maxRoutesPerGroup, 5);
    CHECK_EQ(tuned.maxSlowerPercent, 80);
    {
        Database d2(":memory:", true);
        d2.exec(schema);
        importInto(d2, tuned);
        CHECK_EQ(d2.scalarText("SELECT value FROM meta WHERE key='similar_time_seconds';"), std::string("60"));
        CHECK_EQ(d2.scalarText("SELECT value FROM meta WHERE key='max_routes_per_group';"), std::string("5"));
    }
    for (const char* entry : {"similar_time_seconds,-1", "similar_time_seconds,5000", "max_routes_per_group,0", "max_routes_per_group,50", "max_slower_percent,-5", "max_slower_percent,x"}) {
        writeConfig("settings.csv", std::string("key,value,status,note\ninterchange_walk_seconds,200,x,x\n") + entry + ",x,x\n");
        CHECK(!errorFrom("planner setting").empty());
    }

    fs::remove_all(tmp, ec);
    return finish("test_import");
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "test_import: unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
