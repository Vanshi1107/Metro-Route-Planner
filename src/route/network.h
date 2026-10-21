// The metro network as the router sees it: plain data, loaded from the database.
// Keeping it as plain structs means the router can be tested without any database.
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

namespace metro {

class Database;

namespace route {

struct Station {
    int id = 0;                    // 1..N, contiguous
    std::string gtfsId, name;
    double lat = 0, lon = 0;
};

struct Service {                   // a line, or one branch of a line
    int id = 0;                    // 1..K, contiguous
    std::string family, name, colour, op;
    std::vector<int> stations;     // station ids in travel order, no repeats
};

struct Segment {                   // one hop between neighbours on a service, usable both ways
    int service = 0, from = 0, to = 0;
    int runSeconds = 0;
    int distanceM = 0;             // 0 = unknown
};

struct Transfer {                  // walking from a train of one service to the platform of another
    int fromStation = 0, fromService = 0, toStation = 0, toService = 0;
    int walkSeconds = 0;
};

struct FareSlab {                  // fare for distances above minKm up to and including maxKm
    std::string op, dayType, ticket;   // operator; 'weekday' | 'sunday_holiday'; 'token' | 'smartcard'
    double minKm = 0, maxKm = 0;
    int fareInr = 0;
    std::string validFrom, source;     // source starts with "UNVERIFIED" until checked against the operator
};

struct FarePair {                  // a published fare for one PAIR of stations (stopA < stopB as text)
    std::string op, dayType, ticket;   // dayType: 'any' | 'weekday' | 'sunday_holiday'
    std::string stopA, stopB;          // GTFS stop ids
    int fareInr = 0;
    std::string validFrom, source;
};

struct Network {
    std::vector<Station> stations;
    std::vector<Service> services;
    std::vector<Segment> segments;
    std::vector<Transfer> transfers;
    std::vector<FareSlab> fareSlabs;
    std::vector<FarePair> farePairs;
    int dwellSeconds = 0;          // time a train waits at each stop
    int dataVersion = 0;
    // journey planner settings (from config/settings.csv, stored in the database)
    int plannerSimilarSeconds = 180;
    int plannerMaxRoutes = 3;
    int plannerMaxSlowerPercent = 25;

    const Station& station(int id) const { return stations[static_cast<size_t>(id - 1)]; }
    const Service& service(int id) const { return services[static_cast<size_t>(id - 1)]; }
};

// Checks the network is internally consistent. Throws std::invalid_argument describing the first problem.
void validateNetwork(const Network& n);

// Reads the network from a database made by metro_db + metro_import. Throws std::runtime_error with a
// helpful message if the schema is wrong or no network has been imported.
Network loadNetwork(Database& db);

// Turning what a person typed into a station.
struct Resolution {
    enum class Status { Found, Ambiguous, NotFound };
    Status status = Status::NotFound;
    int station = 0;                    // Found
    std::vector<int> candidates;        // Ambiguous: all matches. NotFound: closest suggestions (may be empty)
};

// Matching rules, in order: "id:<gtfs stop id>"; exact name (ignoring case and punctuation);
// a name containing the text; otherwise the closest names as suggestions.
Resolution resolveStation(const Network& n, const std::string& query);

}  // namespace route
}  // namespace metro
