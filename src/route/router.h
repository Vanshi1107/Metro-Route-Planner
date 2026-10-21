// The route finder.
//
// MODEL. For every service and every station it calls at there are three nodes:
//   P(station, service)      you are standing on the platform, about to board that service
//   T(station, service, up)  you are on a train moving towards the end of the service's station list,
//   T(station, service, down)   or towards its start, which has just arrived at the station
// Edges:
//   P -> T  board and ride one hop            (run time;            +1 boarding)
//   T -> T  stay on the train for one more hop in the SAME direction (run time + dwell; no boarding)
//   T -> P  get off and walk to another service's platform (walking time from the transfers table)
// There is no free "get off and re-board", so stop times cannot be skipped, and a train never reverses
// mid-line, because direction is part of the node.
// You may also start by walking from the origin to a linked station (a confirmed transfer).
// "Changes" = boardings - 1, so walking at the very start or very end is never a change.
//
// Both modes use a two-part cost compared in order, which is valid because every part is non-negative and additive:
//   Fastest        (seconds, boardings)
//   FewestChanges  (boardings, seconds)
//
// The best costs are found with BELLMAN-FORD: keep a table "cheapest known cost to reach each node", then repeatedly
// relax EVERY edge (improve a node if going through an edge is cheaper) until a whole pass changes nothing. A best path
// has fewer edges than there are nodes, so that many passes always suffice. Work: up to nodes x edges. Bellman-Ford
// also copes with negative costs (not needed here, since every cost is positive).
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "network.h"

namespace metro {
namespace route {

enum class Mode { Fastest, FewestChanges };

struct Step {
    enum class Kind { Ride, Change, Walk };
    Kind kind = Kind::Ride;
    int service = 0;          // Ride: the service. Change: the service you change TO.
    int fromService = 0;      // Change: the service you leave
    int fromStation = 0;
    int toStation = 0;
    int hops = 0;             // Ride: number of stops ridden
    int towardsStation = 0;   // Ride: the terminus in the direction of travel
    int seconds = 0;          // Ride: ride time including stop times. Change/Walk: walking time.
    std::vector<int> via;     // Ride: stations passed between fromStation and toStation
};

struct Route {
    bool found = false;
    Mode mode = Mode::Fastest;
    int fromStation = 0, toStation = 0;
    int totalSeconds = 0;
    int changes = 0;
    int stops = 0;
    std::vector<Step> steps;
};

struct Options {                // what the planner offers a traveller
    Route fastest;
    Route fewest;               // only meaningful when fewestDiffers
    bool fewestDiffers = false; // true if the fewest-changes route has strictly fewer changes
    int extraSeconds = 0;       // how much slower the fewest-changes route is
};

// Parts of the network a search must stay away from (used to find different routes, not just the best one).
struct Restrictions {
    std::vector<int> bannedServices;         // service ids that may not be used at all
    std::vector<int> bannedChangeStations;   // station ids where changing from one service to another is not allowed
    bool empty() const { return bannedServices.empty() && bannedChangeStations.empty(); }
};

struct StationCost {
    int seconds = -1;           // -1 = unreachable (or the origin itself)
    int changes = -1;
};

class Router {
public:
    explicit Router(const Network& net);                       // validates the network

    Route find(int fromStation, int toStation, Mode mode) const;
    Route find(int fromStation, int toStation, Mode mode, const Restrictions& restrictions) const;
    Options plan(int fromStation, int toStation) const;

    // Best cost from one origin to every station (index = station id). Used for verification.
    std::vector<StationCost> costsFrom(int fromStation, Mode mode) const;

    // The best route from one origin to EVERY station, from a single search (index = station id).
    std::vector<Route> routesFrom(int fromStation, Mode mode) const;

private:
    struct Edge {
        int to, seconds, boardings;
    };
    struct Cost {
        int a = 0, b = 0;
    };
    struct SearchResult {
        std::vector<Cost> dist;
        std::vector<int> prev;          // -1 start at the origin, -2 start by walking, else previous node
        std::vector<int> edgeSeconds;   // seconds of the edge that led to each node
        int endNode = -1;   // when a destination was given: the cheapest node there
    };

    SearchResult search(int from, int to, Mode mode, const Restrictions* restrictions = nullptr) const;   // Bellman-Ford; to = -1 for all destinations
    Cost add(Cost c, const Edge& e, Mode mode) const;
    Route buildRoute(const SearchResult& r, int from, int to, Mode mode, int endNode) const;
    void checkStation(int id) const;

    const Network& net_;
    std::vector<int> nodeStation_, nodeService_;
    std::vector<std::vector<Edge>> adj_;
    std::vector<std::vector<int>> platformsAt_;                      // station -> platform nodes
    std::vector<std::vector<std::pair<int, int>>> initialWalks_;     // station -> (platform node, walk seconds)
    std::vector<std::vector<int>> positionIn_;                       // service -> station -> index in service (or -1)
};

}  // namespace route
}  // namespace metro
