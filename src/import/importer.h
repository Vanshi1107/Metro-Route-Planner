// Network importer: turns a GTFS folder plus a few small config files into rows in the
// SQLite database.
//
//   buildModel()  reads and checks everything IN MEMORY. It never touches the database, so a
//                 bad feed or bad config fails here and leaves the database exactly as it was.
//   importInto()  writes a finished model inside ONE transaction (all or nothing).
//
// Key idea: a GTFS "route" is one direction of one pattern. A passenger thinks in "services"
// (a line, or one branch of a line). The importer merges routes into services by containment:
// if one route's stop sequence appears inside another's (in either direction) they are the
// same service. See buildModel() in importer.cpp for the exact rules.
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

namespace metro {

class Database;

namespace import {

class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Station {
    int id = 0;
    std::string gtfsId, name;
    double lat = 0, lon = 0;
};

struct Service {
    int id = 0;
    std::string family, name, colour, op;
    std::vector<int> stations;           // station ids in travel order
    std::vector<std::string> routeIds;   // GTFS routes merged into this service
};

struct Segment {
    int service = 0, from = 0, to = 0;
    int runSeconds = 0;
    int distanceM = 0;                   // 0 = unknown
};

struct Transfer {
    int fromStation = 0, fromService = 0, toStation = 0, toService = 0;
    int walkSeconds = 0;
};

struct FareSlabRow {
    std::string op, dayType, ticket;     // operator, 'weekday' | 'sunday_holiday', 'token' | 'smartcard'
    double minKm = 0, maxKm = 0;         // the slab covers distances above minKm up to and including maxKm
    int fareInr = 0;
    std::string validFrom, source;       // source starts with "UNVERIFIED" until checked against the operator
};

struct FarePairRow {
    std::string op, dayType, ticket;     // dayType: 'any' | 'weekday' | 'sunday_holiday'
    std::string stopA, stopB;            // GTFS stop ids, stopA < stopB as text
    int fareInr = 0;
    std::string validFrom, source;
};

struct Model {
    std::vector<Station> stations;
    std::vector<Service> services;
    std::vector<Segment> segments;
    std::vector<Transfer> transfers;
    std::vector<FareSlabRow> fareSlabs;
    std::vector<FarePairRow> farePairs;
    int stationOverridesApplied = 0;
    int distanceOverridesApplied = 0;
    int dwellSeconds = 0;                // median time a train waits at a stop, from the feed
    int similarTimeSeconds = 180;        // journey planner settings from config/settings.csv
    int maxRoutesPerGroup = 3;
    int maxSlowerPercent = 25;
    int interchangeWalkSeconds = 0;      // from config/settings.csv (a placeholder until calibrated)
    std::string feedValidUntil;          // YYYYMMDD from calendar.txt, empty if unknown
    int seedsImported = 0, seedsSkipped = 0;
    std::vector<int> componentSizes;     // sizes of connected pieces of the network, largest first
    std::vector<std::string> notes;      // decisions the importer made automatically
    std::vector<std::string> warnings;   // things worth a human look
};

// Throws ImportError for any problem with the feed or the config. Touches no database.
Model buildModel(const std::string& gtfsDir, const std::string& configDir);

// Human-readable report of a model.
std::string describe(const Model& m);

// Replaces the network tables with the model, inside one transaction, and bumps data_version.
// Users, sessions and saved journeys are not touched. Returns the new data_version.
// Throws ImportError if the database schema version does not match.
int importInto(Database& db, const Model& m);

}  // namespace import
}  // namespace metro
