// Independent checks of the router.
//
// bellmanFordFrom() solves the same problem as the router but with a different algorithm AND a
// separately written model, working straight from the network tables. validateRoute() recomputes a
// route's time and changes from the tables without using the router at all. verifyAllPairs() uses both
// on every pair of stations.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "router.h"

namespace metro {
namespace route {

// Best (seconds, changes) from 'from' to every station, index = station id; (-1,-1) = unreachable.
std::vector<std::pair<int, int>> bellmanFordFrom(const Network& net, int from, Mode mode);

// Returns "" if the route is consistent with the network, otherwise a description of the problem.
std::string validateRoute(const Network& net, const Route& route);

struct VerifyReport {
    long long pairs = 0;             // ordered station pairs x 2 modes
    long long unreachable = 0;
    long long costMismatches = 0;    // router vs Bellman-Ford
    long long findMismatches = 0;    // find() vs the single-origin costs
    long long invalidRoutes = 0;     // failed validateRoute()
    int maxSeconds = 0;
    int maxChanges = 0;
    std::vector<long long> changeHistogram;   // index = changes (fastest mode)
    std::vector<std::string> problems;        // the first few problems, for display
    bool ok() const { return costMismatches == 0 && findMismatches == 0 && invalidRoutes == 0; }
};

VerifyReport verifyAllPairs(const Network& net);

struct FareVerifyReport {
    long long pairs = 0;         // station pairs checked, over every operator that has fare slabs
    long long mismatches = 0;    // Bellman-Ford distance (used for fares) vs an independent Floyd-Warshall distance
    std::vector<std::string> problems;
    bool ok() const { return mismatches == 0; }
};

// Checks the shortest distances used for fares (Bellman-Ford) with a completely different algorithm (Floyd-Warshall).
FareVerifyReport verifyFares(const Network& net);

}  // namespace route
}  // namespace metro
