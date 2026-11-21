#include "migrate.h"

#include <stdexcept>

namespace metro {

namespace {

// The same definitions as the end of db/schema.sql (and the users.role column is added last there too).
const char* kUsersRole =
    "ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'user' CHECK (role IN ('user', 'admin'));";

const char* kStationChanges =
    "CREATE TABLE station_changes ("
    "    change_id    INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    gtfs_stop_id TEXT    NOT NULL,"
    "    kind         TEXT    NOT NULL CHECK (kind IN ('rename', 'closure')),"
    "    new_name     TEXT    CHECK (new_name IS NULL OR length(new_name) BETWEEN 2 AND 60),"
    "    reason       TEXT    NOT NULL CHECK (length(reason) BETWEEN 5 AND 200),"
    "    start_at     INTEGER NOT NULL CHECK (typeof(start_at) = 'integer'),"
    "    end_at       INTEGER CHECK (end_at IS NULL OR typeof(end_at) = 'integer'),"
    "    created_at   INTEGER NOT NULL CHECK (typeof(created_at) = 'integer'),"
    "    created_by   TEXT    NOT NULL,"
    "    cancelled_at INTEGER CHECK (cancelled_at IS NULL OR typeof(cancelled_at) = 'integer'),"
    "    cancelled_by TEXT,"
    "    CHECK ((kind = 'rename' AND new_name IS NOT NULL AND end_at IS NULL) OR (kind = 'closure' AND new_name IS NULL)),"
    "    CHECK (end_at IS NULL OR end_at > start_at)"
    ");"
    "CREATE INDEX idx_station_changes_stop ON station_changes(gtfs_stop_id, change_id);";

}  // namespace

std::string migrateDatabase(Database& db) {
    std::string version;
    try {
        version = db.scalarText("SELECT value FROM meta WHERE key='schema_version';");
    } catch (const std::exception&) {
        throw std::runtime_error("this is not a metro database (no schema_version). Create one with: metro_db init");
    }
    if (version == kCurrentSchemaVersion) return "already up to date (schema version " + version + ")";
    if (version != "5")
        throw std::runtime_error("cannot upgrade from schema version " + version + " (only 5 can be upgraded). Recreate the database with: metro_db init --force, then metro_import");

    Transaction tx(db);          // all-or-nothing
    db.exec(kUsersRole);
    db.exec(kStationChanges);
    db.exec("UPDATE meta SET value = '6' WHERE key = 'schema_version';");
    tx.commit();
    return "upgraded from schema version 5 to 6: users now have a role, and station changes can be stored";
}

}  // namespace metro
