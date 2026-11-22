// Upgrading an existing database to the current schema, WITHOUT losing accounts or saved journeys.
//   version 5 -> 6 : users get a role ('user' or 'admin'); a station_changes table is added (admin renames and closures)
//   version 6 -> 7 : a line_changes table (line suspensions, delays, announcements); station_changes.replaces_change_id
// The result is exactly what db/schema.sql would have created (tests/test_admin.cpp checks that).
#pragma once
#include <string>

#include "db.h"

namespace metro {

constexpr const char* kCurrentSchemaVersion = "7";

// Returns a one-line description of what it did ("already up to date" if nothing was needed).
// Throws std::runtime_error if the database is not one of ours or its version is unknown.
// All-or-nothing: a failure leaves the database exactly as it was.
std::string migrateDatabase(Database& db);

}  // namespace metro
