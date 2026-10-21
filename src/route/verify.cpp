#include "verify.h"

#include "fare.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <tuple>

namespace metro {
namespace route {

namespace {

struct Pair {
    long long a = LLONG_MAX / 4, b = LLONG_MAX / 4;
};
bool less(const Pair& x, const Pair& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; }

}  // namespace

std::vector<std::pair<int, int>> bellmanFordFrom(const Network& net, int from, Mode mode) {
    // state = (service, station, kind): 0 = on the platform, 1 = on a train moving towards the end of the
    // service's list, 2 = on a train moving towards its start
    std::map<std::tuple<int, int, int>, Pair> dist;
    for (const auto& s : net.services)
        for (int st : s.stations)
            for (int kind = 0; kind < 3; ++kind) dist[std::make_tuple(s.id, st, kind)] = Pair();
    auto positionIn = [&](int service, int station) {
        const auto& v = net.service(service).stations;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == station) return static_cast<int>(i);
        return -1;
    };
    auto cost = [&](long long seconds, long long boardings) {
        Pair p;
        if (mode == Mode::Fastest) { p.a = seconds; p.b = boardings; }
        else { p.a = boardings; p.b = seconds; }
        return p;
    };
    auto relax = [&](const std::tuple<int, int, int>& u, const std::tuple<int, int, int>& v, long long seconds, long long boardings) {
        const Pair& du = dist[u];
        if (du.a >= LLONG_MAX / 8) return false;
        Pair step = cost(seconds, boardings);
        Pair cand{du.a + step.a, du.b + step.b};
        Pair& dv = dist[v];
        if (less(cand, dv)) {
            dv = cand;
            return true;
        }
        return false;
    };

    for (const auto& s : net.services)
        for (int st : s.stations)
            if (st == from) dist[std::make_tuple(s.id, st, 0)] = Pair{0, 0};
    for (const auto& t : net.transfers)
        if (t.fromStation == from && t.toStation != from) {
            Pair c = cost(t.walkSeconds, 0);
            Pair& d = dist[std::make_tuple(t.toService, t.toStation, 0)];
            if (less(c, d)) d = c;
        }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& g : net.segments) {
            const int pairs[2][2] = {{g.from, g.to}, {g.to, g.from}};
            for (const auto& xy : pairs) {
                const int kind = positionIn(g.service, xy[1]) > positionIn(g.service, xy[0]) ? 1 : 2;   // direction of this hop
                changed |= relax(std::make_tuple(g.service, xy[0], 0), std::make_tuple(g.service, xy[1], kind), g.runSeconds, 1);
                changed |= relax(std::make_tuple(g.service, xy[0], kind), std::make_tuple(g.service, xy[1], kind),
                                 g.runSeconds + net.dwellSeconds, 0);
            }
        }
        for (const auto& t : net.transfers)
            for (int kind = 1; kind <= 2; ++kind)
                changed |= relax(std::make_tuple(t.fromService, t.fromStation, kind), std::make_tuple(t.toService, t.toStation, 0), t.walkSeconds, 0);
    }

    std::vector<Pair> best(net.stations.size() + 1);
    for (const auto& kv : dist) {
        const int st = std::get<1>(kv.first);
        if (st == from) continue;
        if (less(kv.second, best[static_cast<size_t>(st)])) best[static_cast<size_t>(st)] = kv.second;
    }
    std::vector<std::pair<int, int>> out(net.stations.size() + 1, {-1, -1});
    for (size_t st = 1; st < best.size(); ++st) {
        if (best[st].a >= LLONG_MAX / 8) continue;
        const long long seconds = mode == Mode::Fastest ? best[st].a : best[st].b;
        const long long boardings = mode == Mode::Fastest ? best[st].b : best[st].a;
        out[st] = {static_cast<int>(seconds), static_cast<int>(std::max(0LL, boardings - 1))};
    }
    return out;
}

std::string validateRoute(const Network& net, const Route& r) {
    if (!r.found) return "";
    auto positionOf = [&](int service, int station) {
        const auto& v = net.service(service).stations;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == station) return static_cast<int>(i);
        return -1;
    };
    auto runBetween = [&](int service, int a, int b) {
        for (const auto& g : net.segments)
            if (g.service == service && ((g.from == a && g.to == b) || (g.from == b && g.to == a))) return g.runSeconds;
        return -1;
    };
    auto hasTransfer = [&](int fromSt, int fromSv, int toSt, int toSv, int walk) {   // service 0 = any
        for (const auto& t : net.transfers)
            if (t.fromStation == fromSt && t.toStation == toSt && (fromSv == 0 || t.fromService == fromSv) &&
                (toSv == 0 || t.toService == toSv) && t.walkSeconds == walk)
                return true;
        return false;
    };

    int current = r.fromStation, total = 0, rides = 0, stops = 0;
    for (size_t i = 0; i < r.steps.size(); ++i) {
        const Step& s = r.steps[i];
        if (s.fromStation != current) return "step " + std::to_string(i + 1) + " does not start where the previous one ended";
        if (s.kind == Step::Kind::Ride) {
            std::vector<int> chain;
            chain.push_back(s.fromStation);
            for (int v : s.via) chain.push_back(v);
            chain.push_back(s.toStation);
            if (s.hops != static_cast<int>(chain.size()) - 1 || s.hops < 1) return "ride has a wrong hop count";
            int expected = 0, direction = 0;
            for (size_t k = 0; k + 1 < chain.size(); ++k) {
                int pa = positionOf(s.service, chain[k]), pb = positionOf(s.service, chain[k + 1]);
                if (pa < 0 || pb < 0 || std::abs(pa - pb) != 1) return "ride uses stations that are not neighbours on its service";
                int dir = pb > pa ? 1 : -1;
                if (direction != 0 && dir != direction) return "ride reverses direction";
                direction = dir;
                int run = runBetween(s.service, chain[k], chain[k + 1]);
                if (run < 0) return "ride uses a hop that has no segment";
                expected += run;
                if (k > 0) expected += net.dwellSeconds;
            }
            if (expected != s.seconds) return "ride time " + std::to_string(s.seconds) + " differs from recomputed " + std::to_string(expected);
            const auto& stationsOfService = net.service(s.service).stations;
            int terminus = direction > 0 ? stationsOfService.back() : stationsOfService.front();
            if (terminus != s.towardsStation) return "ride 'towards' station is wrong";
            ++rides;
            stops += s.hops;
        } else if (s.kind == Step::Kind::Change) {
            if (i == 0 || r.steps[i - 1].kind != Step::Kind::Ride || r.steps[i - 1].service != s.fromService)
                return "a change must follow a ride on the service being left";
            if (i + 1 >= r.steps.size() || r.steps[i + 1].kind != Step::Kind::Ride || r.steps[i + 1].service != s.service ||
                r.steps[i + 1].fromStation != s.toStation)
                return "a change must be followed by a ride on the service being joined";
            if (!hasTransfer(s.fromStation, s.fromService, s.toStation, s.service, s.seconds))
                return "no matching transfer row for a change";
        } else {   // Walk: only at the very start or the very end
            if (i == 0) {
                if (!hasTransfer(s.fromStation, 0, s.toStation, 0, s.seconds) || s.toStation == s.fromStation)
                    return "no matching transfer row for the starting walk";
            } else if (i + 1 == r.steps.size()) {
                if (r.steps[i - 1].kind != Step::Kind::Ride) return "a final walk must follow a ride";
                if (!hasTransfer(s.fromStation, r.steps[i - 1].service, s.toStation, 0, s.seconds))
                    return "no matching transfer row for the final walk";
            } else {
                return "a walk in the middle of a route";
            }
        }
        total += s.seconds;
        current = s.toStation;
    }
    if (current != r.toStation) return "route does not end at the destination";
    if (total != r.totalSeconds) return "total time " + std::to_string(r.totalSeconds) + " differs from the sum of steps " + std::to_string(total);
    if (r.changes != std::max(0, rides - 1)) return "change count does not match the number of rides";
    if (r.stops != stops) return "stop count does not match the rides";
    return "";
}

VerifyReport verifyAllPairs(const Network& net) {
    Router router(net);
    VerifyReport rep;
    const int n = static_cast<int>(net.stations.size());
    auto note = [&](const std::string& text) {
        if (rep.problems.size() < 8) rep.problems.push_back(text);
    };
    for (Mode mode : {Mode::Fastest, Mode::FewestChanges}) {
        for (int from = 1; from <= n; ++from) {
            const auto fast = router.costsFrom(from, mode);
            const auto oracle = bellmanFordFrom(net, from, mode);
            const std::vector<Route> routes = router.routesFrom(from, mode);
            for (int to = 1; to <= n; ++to) {
                if (to == from) continue;
                ++rep.pairs;
                const std::string label = net.station(from).name + " -> " + net.station(to).name;
                const auto& a = fast[static_cast<size_t>(to)];
                const auto& b = oracle[static_cast<size_t>(to)];
                if (a.seconds != b.first || a.changes != b.second) {
                    ++rep.costMismatches;
                    note("router and the independent solver disagree for " + label);
                }
                const Route& r = routes[static_cast<size_t>(to)];
                if (r.found != (a.seconds >= 0)) {
                    ++rep.findMismatches;
                    note("routesFrom() and costsFrom() disagree on reachability for " + label);
                } else if (r.found) {
                    if (r.totalSeconds != a.seconds || r.changes != a.changes) {
                        ++rep.findMismatches;
                        note("routesFrom() and costsFrom() disagree for " + label);
                    }
                    std::string problem = validateRoute(net, r);
                    if (!problem.empty()) {
                        ++rep.invalidRoutes;
                        note("invalid route " + label + ": " + problem);
                    }
                    rep.maxSeconds = std::max(rep.maxSeconds, r.totalSeconds);
                    rep.maxChanges = std::max(rep.maxChanges, r.changes);
                    if (mode == Mode::Fastest) {
                        if (rep.changeHistogram.size() <= static_cast<size_t>(r.changes)) rep.changeHistogram.resize(static_cast<size_t>(r.changes) + 1, 0);
                        rep.changeHistogram[static_cast<size_t>(r.changes)]++;
                    }
                } else {
                    ++rep.unreachable;
                }
                // find() runs its own search; check it against the single-origin results for a spread of pairs
                if ((static_cast<long long>(from) * n + to) % 7 == 0) {
                    const Route single = router.find(from, to, mode);
                    if (single.found != r.found || (r.found && (single.totalSeconds != r.totalSeconds || single.changes != r.changes))) {
                        ++rep.findMismatches;
                        note("find() and routesFrom() disagree for " + label);
                    }
                }
            }
        }
    }
    return rep;
}

FareVerifyReport verifyFares(const Network& net) {
    FareVerifyReport rep;
    FareCalculator calc(net);
    const int n = static_cast<int>(net.stations.size());
    for (const std::string& op : calc.operatorsWithSlabs()) {
        const long long INF = LLONG_MAX / 4;
        std::vector<std::vector<long long>> d(static_cast<size_t>(n) + 1, std::vector<long long>(static_cast<size_t>(n) + 1, INF));
        for (int i = 1; i <= n; ++i) d[static_cast<size_t>(i)][static_cast<size_t>(i)] = 0;
        for (const auto& g : net.segments) {
            if (g.distanceM <= 0 || net.service(g.service).op != op) continue;
            auto& a = d[static_cast<size_t>(g.from)][static_cast<size_t>(g.to)];
            a = std::min<long long>(a, g.distanceM);
            auto& b = d[static_cast<size_t>(g.to)][static_cast<size_t>(g.from)];
            b = std::min<long long>(b, g.distanceM);
        }
        for (int k = 1; k <= n; ++k)
            for (int i = 1; i <= n; ++i)
                for (int j = 1; j <= n; ++j)
                    if (d[static_cast<size_t>(i)][static_cast<size_t>(k)] + d[static_cast<size_t>(k)][static_cast<size_t>(j)] < d[static_cast<size_t>(i)][static_cast<size_t>(j)])
                        d[static_cast<size_t>(i)][static_cast<size_t>(j)] = d[static_cast<size_t>(i)][static_cast<size_t>(k)] + d[static_cast<size_t>(k)][static_cast<size_t>(j)];
        for (int from = 1; from <= n; ++from) {
            const std::vector<int> fast = calc.distancesFrom(from, op);
            for (int to = 1; to <= n; ++to) {
                if (to == from) continue;
                ++rep.pairs;
                const long long expected = d[static_cast<size_t>(from)][static_cast<size_t>(to)] >= INF ? -1 : d[static_cast<size_t>(from)][static_cast<size_t>(to)];
                if (fast[static_cast<size_t>(to)] != expected) {
                    ++rep.mismatches;
                    if (rep.problems.size() < 8)
                        rep.problems.push_back(op + ": distance " + net.station(from).name + " -> " + net.station(to).name + " differs between Bellman-Ford and Floyd-Warshall");
                }
            }
        }
    }
    return rep;
}

}  // namespace route
}  // namespace metro
