// What a proposed closure or line suspension would do to journeys, worked out BEFORE the admin confirms it.
//
// For every ordered pair of stations it compares the fastest journey under what is in force now ('current') with the fastest
// journey if the proposal were added ('proposed'). It only computes: nothing is changed.
//   * a pair whose start or end could still be used before, but not after (the station is closed, or no line there runs)
//     is counted as an END-POINT pair, not as "lost": that is the obvious, intended effect
//   * "lost"   : there was a journey, and now there is none, although both stations can still be used
//   * "slower" : there is still a journey, but it takes longer
// The fastest journey is used (time first, then fewer boardings), as in the router.
#pragma once
#include <vector>

#include "network.h"
#include "router.h"

namespace metro {
namespace route {

struct ImpactReport {
    int pairsChecked = 0;            // pairs that had a journey before and whose stations can still be used
    int lost = 0, slower = 0, unaffected = 0;
    int endpointPairs = 0;           // pairs that cannot start or end any more (see above)
    long long extraSecondsTotal = 0; // over the slower pairs
    int maxExtraSeconds = 0, maxFrom = 0, maxTo = 0;
    int lostFrom = 0, lostTo = 0;    // one example of a lost pair (0 = none)
    std::vector<int> stationsWithoutTrains;   // stations usable before but not after
    // seconds of the fastest journey, [from][to], -1 = none; kept so callers can check particular pairs (saved journeys)
    std::vector<std::vector<int>> before, after;
};

// A station can be used if it is not closed and at least one service that calls there is running.
bool stationUsable(const Network& net, const Restrictions& r, int station);

ImpactReport assessImpact(const Network& net, const Router& router, const Restrictions& current, const Restrictions& proposed);

}  // namespace route
}  // namespace metro
