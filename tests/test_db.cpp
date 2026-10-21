#include <fstream>
#include <sstream>

#include "../src/common/db.h"
#include "check.h"

using metro::Database;
using metro::Transaction;

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_db <path-to-schema.sql>\n";
        return 2;
    }
    std::string schema = readFile(argv[1]);
    CHECK(!schema.empty());

    Database db(":memory:", true);
    db.exec(schema);

    for (const char* t : {"meta", "stations", "lines", "line_stops", "segments", "transfers", "fare_slabs", "fare_pairs", "users",
                          "sessions", "login_attempts", "saved_journeys"})
        CHECK(db.tableExists(t));

    CHECK_EQ(db.scalarText("SELECT value FROM meta WHERE key='schema_version';"), std::string("5"));

    // Valid data is accepted.
    db.exec("INSERT INTO stations(station_id,gtfs_stop_id,name,lat,lon) VALUES (1,'A','Alpha',28.6,77.2);");
    db.exec("INSERT INTO stations(station_id,gtfs_stop_id,name,lat,lon) VALUES (2,'B','Beta',28.7,77.3);");
    db.exec("INSERT INTO lines(line_id,family,name,operator,gtfs_route_ids) VALUES (1,'RED','Red','DMRC','1 2');");
    db.exec("INSERT INTO line_stops(line_id,seq,station_id) VALUES (1,0,1);");
    db.exec("INSERT INTO line_stops(line_id,seq,station_id) VALUES (1,1,2);");
    CHECK_THROWS(db.exec("INSERT INTO line_stops(line_id,seq,station_id) VALUES (1,1,1);"));      // duplicate position
    CHECK_THROWS(db.exec("INSERT INTO line_stops(line_id,seq,station_id) VALUES (1,2,99);"));     // unknown station
    CHECK_THROWS(db.exec("INSERT INTO line_stops(line_id,seq,station_id) VALUES (7,0,1);"));      // unknown line
    CHECK_THROWS(db.exec("INSERT INTO lines(family,name,operator,gtfs_route_ids) VALUES ('X','  ','DMRC','1');")); // blank name
    db.exec("INSERT INTO segments(line_id,from_station,to_station,run_seconds,distance_m) VALUES (1,1,2,120,1500);");

    // Bad data is rejected by the database itself.
    CHECK_THROWS(db.exec("INSERT INTO stations(gtfs_stop_id,name,lat,lon) VALUES ('A','Dup',1,1);"));        // duplicate id
    CHECK_THROWS(db.exec("INSERT INTO stations(gtfs_stop_id,name,lat,lon) VALUES ('Z','Bad',100,1);"));      // latitude > 90
    CHECK_THROWS(db.exec("INSERT INTO stations(gtfs_stop_id,name,lat,lon) VALUES ('Y','  ',1,1);"));         // blank name
    CHECK_THROWS(db.exec("INSERT INTO segments(line_id,from_station,to_station,run_seconds) VALUES (1,1,99,60);")); // unknown station
    CHECK_THROWS(db.exec("INSERT INTO segments(line_id,from_station,to_station,run_seconds) VALUES (1,1,1,60);"));  // self loop
    CHECK_THROWS(db.exec("INSERT INTO segments(line_id,from_station,to_station,run_seconds) VALUES (1,2,1,0);"));   // zero time
    CHECK_THROWS(db.exec("INSERT INTO segments(line_id,from_station,to_station,run_seconds) VALUES (1,1,2,90);"));  // duplicate hop
    CHECK_THROWS(db.exec("INSERT INTO fare_slabs(operator,day_type,ticket,min_km,max_km,fare_inr,valid_from,source) "
                         "VALUES ('DMRC','weekday','token',5,5,10,'2025-08-25','x');"));                     // min == max

    // Fare pairs: stored once per pair, in a fixed order.
    const std::string pair = "INSERT INTO fare_pairs(operator,day_type,ticket,stop_a,stop_b,fare_inr,valid_from,source) ";
    db.exec(pair + "VALUES ('AEL','any','token','1','2',21,'2025','test');");
    CHECK_THROWS(db.exec(pair + "VALUES ('AEL','any','token','2','1',21,'2025','test');"));       // wrong order (a must be below b)
    CHECK_THROWS(db.exec(pair + "VALUES ('AEL','any','token','1','2',30,'2025','test');"));       // the same pair twice
    CHECK_THROWS(db.exec(pair + "VALUES ('AEL','monday','token','1','3',21,'2025','test');"));    // unknown day type
    CHECK_THROWS(db.exec(pair + "VALUES ('AEL','any','token','1','3',-5,'2025','test');"));       // negative fare
    CHECK_THROWS(db.exec(pair + "VALUES ('AEL','any','token','3','3',5,'2025','test');"));        // a station paired with itself
    db.exec(pair + "VALUES ('AEL','weekday','token','1','2',25,'2025','test');");                 // another day type is fine

    // Users: usernames are unique ignoring case; sessions and journeys need a real user.
    // Times are whole Unix seconds: text like 'now' is refused.
    db.exec("INSERT INTO users(user_id,username,password_hash,created_at) VALUES (1,'Vanshika','hash',1000);");
    CHECK_THROWS(db.exec("INSERT INTO users(username,password_hash,created_at) VALUES ('vanshika','h',1000);"));    // same name, other case
    CHECK_THROWS(db.exec("INSERT INTO users(username,password_hash,created_at) VALUES ('ab','h',1000);"));          // too short
    CHECK_THROWS(db.exec("INSERT INTO users(username,password_hash,created_at) VALUES ('Another','h','now');"));    // not a timestamp
    CHECK_THROWS(db.exec("INSERT INTO sessions(token_hash,user_id,created_at,expires_at) VALUES ('t',42,1000,2000);"));   // unknown user
    CHECK_THROWS(db.exec("INSERT INTO sessions(token_hash,user_id,created_at,expires_at) VALUES ('t',1,'n','n');"));      // not timestamps
    db.exec("INSERT INTO sessions(token_hash,user_id,created_at,expires_at) VALUES ('t',1,1000,2000);");

    // Failed-login counting is not tied to real users (so unknown names are counted too).
    db.exec("INSERT INTO login_attempts(username,failures,window_start,locked_until) VALUES ('nobody',1,1000,0);");
    CHECK_THROWS(db.exec("INSERT INTO login_attempts(username,failures,window_start,locked_until) VALUES ('NOBODY',2,1000,0);"));   // same name
    CHECK_THROWS(db.exec("INSERT INTO login_attempts(username,failures,window_start) VALUES ('neg',-1,1000);"));

    const std::string journey = "INSERT INTO saved_journeys(user_id,from_gtfs_stop_id,to_gtfs_stop_id,mode,day_type,data_version,total_seconds,changes,result_text,created_at,updated_at) ";
    CHECK_THROWS(db.exec(journey + "VALUES (1,'A','B','quickest','weekday',1,100,0,'x',1000,1000);"));         // invalid mode
    CHECK_THROWS(db.exec(journey + "VALUES (1,'A','B','fastest','monday',1,100,0,'x',1000,1000);"));           // invalid day type
    CHECK_THROWS(db.exec(journey + "VALUES (1,'A','B','fastest','weekday',1,100,0,'x','now',1000);"));         // not a timestamp
    CHECK_THROWS(db.exec(journey + "VALUES (9,'A','B','fastest','weekday',1,100,0,'x',1000,1000);"));          // unknown user
    db.exec(journey + "VALUES (1,'A','B','fastest','weekday',1,100,0,'x',1000,1000);");
    CHECK_THROWS(db.exec(journey + "VALUES (1,'A','B','fastest','weekday',2,100,0,'y',1000,1000);"));          // the same trip twice
    db.exec(journey + "VALUES (1,'A','B','fewest_changes','weekday',1,100,0,'x',1000,1000);");                 // another option is fine
    db.exec(journey + "VALUES (1,'A','B','cheapest','weekday',1,100,0,'x',1000,1000);");                       // 'cheapest' is a valid choice
    CHECK_THROWS(db.exec("UPDATE saved_journeys SET label = '" + std::string(61, 'x') + "' WHERE journey_id = 1;"));   // label too long
    CHECK_THROWS(db.exec("UPDATE users SET role = 'admin' WHERE user_id = 1;"));                               // there is no role column

    // Saved journeys must survive a station disappearing (stored by id text, no foreign key).
    db.exec("DELETE FROM stations WHERE gtfs_stop_id='B';");
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM saved_journeys;"), 3LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM segments;"), 0LL);   // but segments touching it are removed

    // Deleting a user removes their sessions and journeys.
    db.exec("DELETE FROM users WHERE user_id=1;");
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM sessions;"), 0LL);
    CHECK_EQ(db.scalarInt("SELECT COUNT(*) FROM saved_journeys;"), 0LL);

    CHECK_EQ(db.scalarInt("PRAGMA foreign_keys;"), 1LL);
    CHECK(db.columnText("PRAGMA foreign_key_check;").empty());

    // A transaction that is not committed leaves nothing behind.
    {
        Transaction tx(db);
        db.exec("CREATE TABLE scratch(x INTEGER);");
    }
    CHECK(!db.tableExists("scratch"));
    {
        Transaction tx(db);
        db.exec("CREATE TABLE kept(x INTEGER);");
        tx.commit();
    }
    CHECK(db.tableExists("kept"));

    // Opening a missing file without permission to create it fails clearly.
    CHECK_THROWS(Database("/nonexistent_dir_metro/none.db", false));

    return finish("test_db");
}
