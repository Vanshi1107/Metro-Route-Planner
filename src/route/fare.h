// Fares.
//
// RULE (from IRCTC's booking terms, which quote DMRC): the fare depends on the distance of the SHORTEST route
// between the origin and destination stations, not on the path the traveller takes. So the fare is
//     slab( shortest distance between the first boarding station and the last alighting station )
// computed on the network of ONE operator. Operators that publish a fare for each PAIR of stations (the Airport
// Express) use that table instead. A fare is produced only if every ride is on a single operator that has slabs
// or pair fares loaded; otherwise the quote says why not. Nothing is guessed.
//
// Slab boundaries: a slab covers distances ABOVE minKm up to AND INCLUDING maxKm (so exactly 2 km is in the
// 0-2 km slab). That reading is an assumption until checked against DMRC's own wording.
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "network.h"
#include "router.h"

namespace metro {
namespace route {

enum class DayType { Weekday, SundayHoliday };

struct FareQuote {
    bool available = false;
    int fareInr = 0;
    int distanceM = 0;           // the distance the fare is based on (0 when the fare comes from a pair table)
    std::string basis;           // "distance" or "pair table"
    std::string op;              // operator whose rule was used
    std::string ticket;          // always "token" for now
    std::string reason;          // when not available: why
    std::string source;          // where the slab came from
    bool verified = false;       // false while the slab source starts with "UNVERIFIED"
};

class FareCalculator {
public:
    explicit FareCalculator(const Network& net);

    FareQuote quote(const Route& route, DayType day) const;

    // Shortest distance in metres between two stations using only services of 'op' (-1 = not connected).
    int shortestDistanceM(int from, int to, const std::string& op) const;
    // Same from one origin to every station (index = station id). -1 = not connected, 0 = the origin.
    std::vector<int> distancesFrom(int from, const std::string& op) const;
    // The fare for a distance, or -1 if no slab covers it.
    int fareForDistance(const std::string& op, DayType day, int distanceM) const;

    std::vector<std::string> operatorsWithSlabs() const;
    const std::vector<std::pair<int, int>>& neighbours(const std::string& op, int station) const;

private:
    const Network& net_;
    std::map<std::string, std::vector<std::vector<std::pair<int, int>>>> graph_;   // operator -> station -> (neighbour, metres)
};

std::string dayTypeName(DayType d);

}  // namespace route
}  // namespace metro
