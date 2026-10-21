// metro_db: create and verify the SQLite database.
//   metro_db init  [--db data/metro.db] [--schema db/schema.sql] [--force]
//   metro_db check [--db data/metro.db]
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../common/db.h"

namespace fs = std::filesystem;
using metro::Database;
using metro::Transaction;

namespace {

const char* kTables[] = {"meta",     "stations",    "lines",    "line_stops", "segments", "transfers",
                         "fare_slabs", "fare_pairs", "users",     "sessions", "login_attempts", "saved_journeys"};

void usage() {
    std::cout << "Usage:\n"
                 "  metro_db init  [--db PATH] [--schema PATH] [--force]   create a new database\n"
                 "  metro_db check [--db PATH]                              verify an existing database\n"
                 "Defaults: --db data/metro.db   --schema db/schema.sql\n"
                 "Run it from the project folder so the default paths work.\n";
}

std::string optionValue(const std::vector<std::string>& args, const std::string& name, const std::string& def) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return def;
}

bool hasFlag(const std::vector<std::string>& args, const std::string& name) {
    for (auto& a : args)
        if (a == name) return true;
    return false;
}

int check(const std::string& dbPath) {
    if (!fs::exists(dbPath)) {
        std::cerr << "Database not found: " << dbPath << "\nRun:  metro_db init\n";
        return 1;
    }
    Database db(dbPath, false);
    bool good = true;
    std::cout << "Database: " << dbPath << "\n\nTables:\n";
    for (const char* t : kTables) {
        if (!db.tableExists(t)) {
            std::cout << "  MISSING  " << t << "\n";
            good = false;
        } else {
            std::cout << "  ok       " << t << "  (" << db.scalarInt(std::string("SELECT COUNT(*) FROM ") + t) << " rows)\n";
        }
    }
    std::string integrity = db.scalarText("PRAGMA integrity_check;");
    auto fkProblems = db.columnText("PRAGMA foreign_key_check;");
    std::cout << "\nIntegrity check:   " << integrity << "\n";
    std::cout << "Foreign key check: " << (fkProblems.empty() ? "ok" : std::to_string(fkProblems.size()) + " problem(s)") << "\n";
    if (integrity != "ok" || !fkProblems.empty()) good = false;
    if (db.tableExists("meta")) {
        std::cout << "\nschema_version = " << db.scalarText("SELECT value FROM meta WHERE key='schema_version'") << "\n";
        std::cout << "data_version   = " << db.scalarText("SELECT value FROM meta WHERE key='data_version'")
                  << "  (0 = no network data imported yet)\n";
    }
    std::cout << "\n" << (good ? "RESULT: database is healthy." : "RESULT: PROBLEMS FOUND.") << "\n";
    return good ? 0 : 1;
}

int init(const std::string& dbPath, const std::string& schemaPath, bool force) {
    if (fs::exists(dbPath)) {
        if (!force) {
            std::cerr << "Database already exists: " << dbPath << "\nUse --force to delete it and start over.\n";
            return 2;
        }
        std::error_code ec;
        fs::remove(dbPath, ec);
        fs::remove(dbPath + "-wal", ec);
        fs::remove(dbPath + "-shm", ec);
        fs::remove(dbPath + "-journal", ec);
    }
    std::ifstream sf(schemaPath, std::ios::binary);
    if (!sf) {
        std::cerr << "Cannot read schema file: " << schemaPath << "\nRun this from the project folder.\n";
        return 2;
    }
    std::stringstream ss;
    ss << sf.rdbuf();

    fs::path parent = fs::path(dbPath).parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    try {
        Database db(dbPath, true);
        Transaction tx(db);   // all-or-nothing: a schema error leaves no half-built database
        db.exec(ss.str());
        db.exec("INSERT INTO meta(key, value) VALUES ('data_version', '0');");
        db.exec("INSERT INTO meta(key, value) VALUES ('created_at', datetime('now'));");
        tx.commit();
    } catch (...) {
        std::error_code ec;          // do not leave an empty or partial file behind
        fs::remove(dbPath, ec);
        fs::remove(dbPath + "-journal", ec);
        throw;
    }
    std::cout << "Created " << dbPath << " from " << schemaPath << "\n\n";
    return check(dbPath);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 2 : 0;
    }
    try {
        std::string db = optionValue(args, "--db", "data/metro.db");
        if (args[0] == "init") return init(db, optionValue(args, "--schema", "db/schema.sql"), hasFlag(args, "--force"));
        if (args[0] == "check") return check(db);
        std::cerr << "Unknown command: " << args[0] << "\n\n";
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
