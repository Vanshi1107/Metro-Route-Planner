#include "impact.h"

#include <algorithm>

namespace metro {
namespace route {

bool stationUsable(const Network& net, const Restrictions& r, int station) {
    if (std::find(r.closedStations.begin(), r.closedStations.end(), station) != r.closedStations.end()) return false;
    for (const auto& sv : net.services) {
        if (std::find(r.bannedServices.begin(), r.bannedServices.end(), sv.id) != r.bannedServices.end()) continue;
        if (std::find(sv.stations.begin(), sv.stations.end(), station) != sv.stations.end()) return true;
    }
    return false;
}

ImpactReport assessImpact(const Network& net, const Router& router, const Restrictions& current, const Restrictions& proposed) {
    const int n = static_cast<int>(net.stations.size());
    ImpactReport rep;
    rep.before.assign(static_cast<size_t>(n + 1), std::vector<int>(static_cast<size_t>(n + 1), -1));
    rep.after = rep.before;
    std::vector<char> usableBefore(static_cast<size_t>(n + 1), 0), usableAfter(static_cast<size_t>(n + 1), 0);
    for (int s = 1; s <= n; ++s) {
        usableBefore[static_cast<size_t>(s)] = stationUsable(net, current, s) ? 1 : 0;
        usableAfter[static_cast<size_t>(s)] = stationUsable(net, proposed, s) ? 1 : 0;
        if (usableBefore[static_cast<size_t>(s)] && !usableAfter[static_cast<size_t>(s)]) rep.stationsWithoutTrains.push_back(s);
    }
    for (int o = 1; o <= n; ++o) {
        if (!usableBefore[static_cast<size_t>(o)]) continue;
        const auto b = router.costsFrom(o, Mode::Fastest, current);
        std::vector<StationCost> a;
        if (usableAfter[static_cast<size_t>(o)]) a = router.costsFrom(o, Mode::Fastest, proposed);
        for (int d = 1; d <= n; ++d) {
            if (d == o || !usableBefore[static_cast<size_t>(d)]) continue;
            const int sb = b[static_cast<size_t>(d)].seconds;
            rep.before[static_cast<size_t>(o)][static_cast<size_t>(d)] = sb;
            if (sb < 0) continue;                                         // there was no journey anyway
            if (!usableAfter[static_cast<size_t>(o)] || !usableAfter[static_cast<size_t>(d)]) {
                ++rep.endpointPairs;
                continue;
            }
            const int sa = a[static_cast<size_t>(d)].seconds;
            rep.after[static_cast<size_t>(o)][static_cast<size_t>(d)] = sa;
            ++rep.pairsChecked;
            if (sa < 0) {
                ++rep.lost;
                if (!rep.lostFrom) {
                    rep.lostFrom = o;
                    rep.lostTo = d;
                }
            } else if (sa > sb) {
                ++rep.slower;
                rep.extraSecondsTotal += sa - sb;
                if (sa - sb > rep.maxExtraSeconds) {
                    rep.maxExtraSeconds = sa - sb;
                    rep.maxFrom = o;
                    rep.maxTo = d;
                }
            } else {
                ++rep.unaffected;
            }
        }
    }
    return rep;
}

}  // namespace route
}  // namespace metro
