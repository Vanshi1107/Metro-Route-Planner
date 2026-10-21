#include "fare.h"

#include <algorithm>
#include <climits>
#include <stdexcept>
#include <set>

namespace metro {
namespace route {

std::string dayTypeName(DayType d) { return d == DayType::Weekday ? "weekday" : "sunday_holiday"; }

FareCalculator::FareCalculator(const Network& net) : net_(net) {
    for (const auto& s : net.services)
        graph_.emplace(s.op, std::vector<std::vector<std::pair<int, int>>>(net.stations.size() + 1));
    for (const auto& g : net.segments) {
        if (g.distanceM <= 0) continue;   // unknown distance: never used for fares
        auto& adj = graph_[net.service(g.service).op];
        adj[static_cast<size_t>(g.from)].push_back({g.to, g.distanceM});
        adj[static_cast<size_t>(g.to)].push_back({g.from, g.distanceM});
    }
}

std::vector<int> FareCalculator::distancesFrom(int from, const std::string& op) const {
    std::vector<int> dist(net_.stations.size() + 1, -1);
    auto it = graph_.find(op);
    if (it == graph_.end() || from < 1 || from > static_cast<int>(net_.stations.size())) return dist;
    const auto& adj = it->second;
    // Bellman-Ford: relax every edge, again and again, until a whole pass changes nothing.
    // (A shortest path has fewer edges than there are stations, so that many passes always suffice.)
    dist[static_cast<size_t>(from)] = 0;
    const size_t stations = net_.stations.size();
    bool changed = true;
    for (size_t pass = 0; changed; ++pass) {
        if (pass > stations + 1) throw std::logic_error("Bellman-Ford did not settle while measuring distances");
        changed = false;
        for (size_t u = 1; u <= stations; ++u) {
            if (dist[u] < 0) continue;
            for (const auto& e : adj[u]) {
                const int nd = dist[u] + e.second;
                int& cur = dist[static_cast<size_t>(e.first)];
                if (cur < 0 || nd < cur) {
                    cur = nd;
                    changed = true;
                }
            }
        }
    }
    return dist;
}

int FareCalculator::shortestDistanceM(int from, int to, const std::string& op) const {
    if (to < 1 || to > static_cast<int>(net_.stations.size())) return -1;
    return distancesFrom(from, op)[static_cast<size_t>(to)];
}

int FareCalculator::fareForDistance(const std::string& op, DayType day, int distanceM) const {
    if (distanceM <= 0) return -1;
    for (const auto& f : net_.fareSlabs)
        if (f.op == op && f.dayType == dayTypeName(day) && f.ticket == "token" && distanceM > f.minKm * 1000.0 && distanceM <= f.maxKm * 1000.0)
            return f.fareInr;
    return -1;
}

std::vector<std::string> FareCalculator::operatorsWithSlabs() const {
    std::set<std::string> ops;
    for (const auto& f : net_.fareSlabs) ops.insert(f.op);
    return std::vector<std::string>(ops.begin(), ops.end());
}

const std::vector<std::pair<int, int>>& FareCalculator::neighbours(const std::string& op, int station) const {
    static const std::vector<std::pair<int, int>> none;
    auto it = graph_.find(op);
    if (it == graph_.end() || station < 1 || station > static_cast<int>(net_.stations.size())) return none;
    return it->second[static_cast<size_t>(station)];
}

FareQuote FareCalculator::quote(const Route& route, DayType day) const {
    FareQuote q;
    q.ticket = "token";
    if (!route.found) {
        q.reason = "there is no route";
        return q;
    }
    std::set<std::string> ops;
    int first = 0, last = 0;
    for (const auto& s : route.steps)
        if (s.kind == Step::Kind::Ride) {
            ops.insert(net_.service(s.service).op);
            if (first == 0) first = s.fromStation;
            last = s.toStation;
        }
    if (ops.empty()) {
        q.reason = "this trip is a walk, with no train ride";
        return q;
    }
    if (ops.size() > 1) {
        std::string list;
        for (const auto& o : ops) list += (list.empty() ? "" : ", ") + o;
        q.reason = "the journey mixes operators (" + list + ") and no combined fare rule is loaded";
        return q;
    }
    q.op = *ops.begin();

    // An operator with a table of fares per pair of stations: look the pair up.
    bool hasPairs = false;
    for (const auto& f : net_.farePairs)
        if (f.op == q.op) hasPairs = true;
    if (hasPairs) {
        std::string a = net_.station(first).gtfsId, b = net_.station(last).gtfsId;
        if (b < a) std::swap(a, b);
        q.basis = "pair table";
        for (const auto& f : net_.farePairs)
            if (f.op == q.op && f.ticket == "token" && (f.dayType == "any" || f.dayType == dayTypeName(day)) && f.stopA == a && f.stopB == b) {
                q.fareInr = f.fareInr;
                q.source = f.source;
                q.verified = q.source.rfind("UNVERIFIED", 0) != 0;
                q.available = true;
                return q;
            }
        q.reason = "no " + q.op + " fare is listed for " + net_.station(first).name + " and " + net_.station(last).name;
        return q;
    }
    q.basis = "distance";
    bool anySlab = false;
    for (const auto& f : net_.fareSlabs)
        if (f.op == q.op && f.dayType == dayTypeName(day) && f.ticket == "token") anySlab = true;
    if (!anySlab) {
        q.reason = "no fare rule is loaded for operator " + q.op;
        return q;
    }
    q.distanceM = shortestDistanceM(first, last, q.op);
    if (q.distanceM <= 0) {
        q.reason = "the distance between " + net_.station(first).name + " and " + net_.station(last).name + " is unknown or invalid in the data";
        return q;
    }
    q.fareInr = fareForDistance(q.op, day, q.distanceM);
    if (q.fareInr < 0) {
        q.reason = "no fare slab covers " + std::to_string(q.distanceM) + " m";
        return q;
    }
    for (const auto& f : net_.fareSlabs)
        if (f.op == q.op && f.dayType == dayTypeName(day) && f.ticket == "token" && q.distanceM > f.minKm * 1000.0 && q.distanceM <= f.maxKm * 1000.0) {
            q.source = f.source;
            break;
        }
    q.verified = q.source.rfind("UNVERIFIED", 0) != 0;
    q.available = true;
    return q;
}

}  // namespace route
}  // namespace metro
