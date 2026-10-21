#include "router.h"

#include <algorithm>
#include <climits>
#include <stdexcept>

namespace metro {
namespace route {

namespace {
bool lessCost(int a1, int b1, int a2, int b2) { return a1 != a2 ? a1 < a2 : b1 < b2; }
}  // namespace

Router::Router(const Network& net) : net_(net) {
    validateNetwork(net);

    const size_t stations = net.stations.size();
    positionIn_.assign(net.services.size(), std::vector<int>(stations + 1, -1));
    std::vector<int> base(net.services.size(), 0);
    int total = 0;
    for (size_t s = 0; s < net.services.size(); ++s) {
        base[s] = total;
        for (size_t i = 0; i < net.services[s].stations.size(); ++i)
            positionIn_[s][static_cast<size_t>(net.services[s].stations[i])] = static_cast<int>(i);
        total += static_cast<int>(net.services[s].stations.size());
    }
    nodeStation_.assign(static_cast<size_t>(3 * total), 0);
    nodeService_.assign(static_cast<size_t>(3 * total), 0);
    adj_.assign(static_cast<size_t>(3 * total), {});
    platformsAt_.assign(stations + 1, {});
    initialWalks_.assign(stations + 1, {});

    // global position index of (service, station); node ids: platform = 3*g, train moving up = 3*g+1,
    // train moving down = 3*g+2 ("up" = towards the end of the service's station list)
    auto globalPos = [&](int service, int station) {
        return base[static_cast<size_t>(service - 1)] + positionIn_[static_cast<size_t>(service - 1)][static_cast<size_t>(station)];
    };
    for (const auto& s : net.services)
        for (int st : s.stations) {
            int g = globalPos(s.id, st);
            for (int k = 0; k < 3; ++k) {
                nodeStation_[static_cast<size_t>(3 * g + k)] = st;
                nodeService_[static_cast<size_t>(3 * g + k)] = s.id;
            }
            platformsAt_[static_cast<size_t>(st)].push_back(3 * g);
        }

    for (const auto& g : net.segments) {
        // lo = the end of the hop nearer the start of the service, hi = the end nearer its finish
        const bool fromIsLower = positionIn_[static_cast<size_t>(g.service - 1)][static_cast<size_t>(g.from)] <
                                 positionIn_[static_cast<size_t>(g.service - 1)][static_cast<size_t>(g.to)];
        const int lo = fromIsLower ? g.from : g.to, hi = fromIsLower ? g.to : g.from;
        const int gl = globalPos(g.service, lo), gh = globalPos(g.service, hi);
        adj_[static_cast<size_t>(3 * gl)].push_back({3 * gh + 1, g.runSeconds, 1});                          // board at lo, ride up
        adj_[static_cast<size_t>(3 * gh)].push_back({3 * gl + 2, g.runSeconds, 1});                          // board at hi, ride down
        adj_[static_cast<size_t>(3 * gl + 1)].push_back({3 * gh + 1, g.runSeconds + net.dwellSeconds, 0});   // stay on, going up
        adj_[static_cast<size_t>(3 * gh + 2)].push_back({3 * gl + 2, g.runSeconds + net.dwellSeconds, 0});   // stay on, going down
    }
    for (const auto& t : net.transfers) {
        const int gf = globalPos(t.fromService, t.fromStation);
        const int to = 3 * globalPos(t.toService, t.toStation);             // on a platform
        adj_[static_cast<size_t>(3 * gf + 1)].push_back({to, t.walkSeconds, 0});   // get off a train moving up...
        adj_[static_cast<size_t>(3 * gf + 2)].push_back({to, t.walkSeconds, 0});   // ...or moving down
        if (t.fromStation != t.toStation) {
            auto& w = initialWalks_[static_cast<size_t>(t.fromStation)];
            auto it = std::find_if(w.begin(), w.end(), [&](const std::pair<int, int>& p) { return p.first == to; });
            if (it == w.end()) w.push_back({to, t.walkSeconds});
            else it->second = std::min(it->second, t.walkSeconds);
        }
    }
}

void Router::checkStation(int id) const {
    if (id < 1 || id > static_cast<int>(net_.stations.size())) throw std::invalid_argument("unknown station id " + std::to_string(id));
}

Router::Cost Router::add(Cost c, const Edge& e, Mode mode) const {
    return mode == Mode::Fastest ? Cost{c.a + e.seconds, c.b + e.boardings} : Cost{c.a + e.boardings, c.b + e.seconds};
}

Router::SearchResult Router::search(int from, int to, Mode mode, const Restrictions* restrictions) const {
    const size_t n = adj_.size();
    std::vector<char> serviceBanned, stationBanned;
    if (restrictions && !restrictions->empty()) {
        serviceBanned.assign(net_.services.size() + 1, 0);
        stationBanned.assign(net_.stations.size() + 1, 0);
        for (int id : restrictions->bannedServices)
            if (id >= 1 && id <= static_cast<int>(net_.services.size())) serviceBanned[static_cast<size_t>(id)] = 1;
        for (int id : restrictions->bannedChangeStations)
            if (id >= 1 && id <= static_cast<int>(net_.stations.size())) stationBanned[static_cast<size_t>(id)] = 1;
    }
    const bool restricted = !serviceBanned.empty();
    SearchResult r;
    r.dist.assign(n, Cost{INT_MAX, INT_MAX});
    r.prev.assign(n, -1);
    r.edgeSeconds.assign(n, 0);

    // starting points: every platform at the origin, and a walk to any linked station
    for (int p : platformsAt_[static_cast<size_t>(from)]) {
        if (restricted && serviceBanned[static_cast<size_t>(nodeService_[static_cast<size_t>(p)])]) continue;
        r.dist[static_cast<size_t>(p)] = Cost{0, 0};
    }
    for (const auto& w : initialWalks_[static_cast<size_t>(from)]) {
        if (restricted && serviceBanned[static_cast<size_t>(nodeService_[static_cast<size_t>(w.first)])]) continue;
        Cost c = add(Cost{0, 0}, Edge{w.first, w.second, 0}, mode);
        Cost& cur = r.dist[static_cast<size_t>(w.first)];
        if (lessCost(c.a, c.b, cur.a, cur.b)) {
            cur = c;
            r.prev[static_cast<size_t>(w.first)] = -2;
            r.edgeSeconds[static_cast<size_t>(w.first)] = w.second;
        }
    }

    // Relax every edge, over and over, until a whole pass changes nothing. With no negative costs a shortest path has
    // fewer edges than there are nodes, so this must stop within n passes; anything else would mean a negative cycle.
    size_t passes = 0;
    bool changed = true;
    while (changed) {
        if (++passes > n + 1) throw std::logic_error("Bellman-Ford did not settle: the network has a negative cost cycle");
        changed = false;
        for (size_t u = 0; u < n; ++u) {
            if (r.dist[u].a == INT_MAX) continue;
            for (const Edge& e : adj_[u]) {
                if (restricted) {
                    if (serviceBanned[static_cast<size_t>(nodeService_[static_cast<size_t>(e.to)])]) continue;   // may not use that service
                    const bool isChange = (u % 3 != 0) && (e.to % 3 == 0);                                       // train -> platform
                    if (isChange && stationBanned[static_cast<size_t>(nodeStation_[u])]) continue;               // may not change here
                }
                const Cost c = add(r.dist[u], e, mode);
                Cost& cur = r.dist[static_cast<size_t>(e.to)];
                if (lessCost(c.a, c.b, cur.a, cur.b)) {
                    cur = c;
                    r.prev[static_cast<size_t>(e.to)] = static_cast<int>(u);
                    r.edgeSeconds[static_cast<size_t>(e.to)] = e.seconds;
                    changed = true;
                }
            }
        }
    }

    // The search always runs to the end, then the cheapest node at the destination is picked.
    if (to >= 0) {
        int best = -1;
        for (size_t node = 0; node < n; ++node) {
            if (nodeStation_[node] != to || r.dist[node].a == INT_MAX) continue;
            if (best < 0 || lessCost(r.dist[node].a, r.dist[node].b, r.dist[static_cast<size_t>(best)].a, r.dist[static_cast<size_t>(best)].b)) best = static_cast<int>(node);
        }
        r.endNode = best;
    }
    return r;
}

Route Router::buildRoute(const SearchResult& r, int from, int to, Mode mode, int endNode) const {
    Route route;
    route.mode = mode;
    route.fromStation = from;
    route.toStation = to;
    if (endNode < 0) return route;
    route.found = true;

    std::vector<int> nodes;
    for (int v = endNode; v >= 0; v = r.prev[static_cast<size_t>(v)]) nodes.push_back(v);
    std::reverse(nodes.begin(), nodes.end());

    const Cost end = r.dist[static_cast<size_t>(endNode)];
    route.totalSeconds = mode == Mode::Fastest ? end.a : end.b;

    auto isTrain = [](int node) { return node % 3 != 0; };
    auto station = [&](int node) { return nodeStation_[static_cast<size_t>(node)]; };
    auto service = [&](int node) { return nodeService_[static_cast<size_t>(node)]; };

    if (r.prev[static_cast<size_t>(nodes.front())] == -2) {
        Step w;
        w.kind = Step::Kind::Walk;
        w.fromStation = from;
        w.toStation = station(nodes.front());
        w.seconds = r.edgeSeconds[static_cast<size_t>(nodes.front())];
        route.steps.push_back(w);
    }

    Step ride;
    bool riding = false, movingUp = false;
    auto flush = [&]() {
        if (!riding) return;
        const auto& stops = net_.service(ride.service).stations;
        ride.towardsStation = movingUp ? stops.back() : stops.front();
        route.stops += ride.hops;
        route.steps.push_back(ride);
        ride = Step();
        riding = false;
    };

    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        const int u = nodes[i], v = nodes[i + 1];
        const int secs = r.edgeSeconds[static_cast<size_t>(v)];
        if (isTrain(v)) {                               // a ride hop (boarding or staying on)
            if (!isTrain(u)) {                          // boarding: start a new ride
                flush();
                ride.kind = Step::Kind::Ride;
                ride.service = service(u);
                ride.fromStation = station(u);
                riding = true;
                movingUp = (v % 3 == 1);
            } else {
                ride.via.push_back(station(u));         // we passed through station(u)
            }
            ride.toStation = station(v);
            ride.hops++;
            ride.seconds += secs;
        } else {                                        // train -> platform: a walk
            flush();
            Step w;
            w.fromStation = station(u);
            w.toStation = station(v);
            w.seconds = secs;
            w.fromService = service(u);
            w.service = service(v);
            const bool last = (i + 2 == nodes.size());
            w.kind = last ? Step::Kind::Walk : Step::Kind::Change;
            route.steps.push_back(w);
        }
    }
    flush();

    int rides = 0;
    for (const auto& s : route.steps)
        if (s.kind == Step::Kind::Ride) ++rides;
    route.changes = std::max(0, rides - 1);
    return route;
}

Route Router::find(int from, int to, Mode mode) const { return find(from, to, mode, Restrictions()); }

Route Router::find(int from, int to, Mode mode, const Restrictions& restrictions) const {
    for (int id : restrictions.bannedServices)
        if (id < 1 || id > static_cast<int>(net_.services.size())) throw std::invalid_argument("unknown service id " + std::to_string(id));
    checkStation(from);
    checkStation(to);
    if (from == to) {
        Route r;
        r.mode = mode;
        r.fromStation = from;
        r.toStation = to;
        return r;   // found = false
    }
    const SearchResult r = search(from, to, mode, &restrictions);
    return buildRoute(r, from, to, mode, r.endNode);
}

Options Router::plan(int from, int to) const {
    Options o;
    o.fastest = find(from, to, Mode::Fastest);
    if (!o.fastest.found) return o;
    o.fewest = find(from, to, Mode::FewestChanges);
    o.fewestDiffers = o.fewest.found && o.fewest.changes < o.fastest.changes;
    o.extraSeconds = o.fewest.totalSeconds - o.fastest.totalSeconds;
    return o;
}

std::vector<StationCost> Router::costsFrom(int from, Mode mode) const {
    checkStation(from);
    SearchResult r = search(from, -1, mode);
    std::vector<StationCost> out(net_.stations.size() + 1);
    // Nodes are settled in increasing cost order, but 'dist' holds each node's final cost, so the best
    // arrival at a station is simply the smallest final cost among its nodes.
    std::vector<Cost> best(net_.stations.size() + 1, Cost{INT_MAX, INT_MAX});
    for (size_t node = 0; node < r.dist.size(); ++node) {
        const Cost& c = r.dist[node];
        if (c.a == INT_MAX) continue;
        const int st = nodeStation_[node];
        if (st == from) continue;
        Cost& b = best[static_cast<size_t>(st)];
        if (lessCost(c.a, c.b, b.a, b.b)) b = c;
    }
    for (size_t st = 1; st < best.size(); ++st) {
        if (best[st].a == INT_MAX) continue;
        const int seconds = mode == Mode::Fastest ? best[st].a : best[st].b;
        const int boardings = mode == Mode::Fastest ? best[st].b : best[st].a;
        out[st].seconds = seconds;
        out[st].changes = std::max(0, boardings - 1);
    }
    return out;
}

std::vector<Route> Router::routesFrom(int from, Mode mode) const {
    checkStation(from);
    const SearchResult r = search(from, -1, mode);
    const size_t stations = net_.stations.size();
    std::vector<int> best(stations + 1, -1);   // cheapest node at each station (the same rule find() uses)
    for (size_t node = 0; node < r.dist.size(); ++node) {
        if (r.dist[node].a == INT_MAX) continue;
        const size_t st = static_cast<size_t>(nodeStation_[node]);
        if (static_cast<int>(st) == from) continue;
        if (best[st] < 0 || lessCost(r.dist[node].a, r.dist[node].b, r.dist[static_cast<size_t>(best[st])].a, r.dist[static_cast<size_t>(best[st])].b))
            best[st] = static_cast<int>(node);
    }
    std::vector<Route> out(stations + 1);
    for (size_t to = 1; to <= stations; ++to) {
        Route& route = out[to];
        route.mode = mode;
        route.fromStation = from;
        route.toStation = static_cast<int>(to);
        if (static_cast<int>(to) == from || best[to] < 0) continue;   // not found
        route = buildRoute(r, from, static_cast<int>(to), mode, best[to]);
    }
    return out;
}

}  // namespace route
}  // namespace metro
