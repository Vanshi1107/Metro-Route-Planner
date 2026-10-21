#include "planner.h"

#include <algorithm>
#include <set>

namespace metro {
namespace route {

const char* preferenceName(Preference p) {
    switch (p) {
        case Preference::Fastest: return "fastest";
        case Preference::Cheapest: return "cheapest";
        case Preference::FewestChanges: return "fewest";
    }
    return "fastest";
}

bool parsePreference(const std::string& text, Preference& out) {
    if (text == "fastest") out = Preference::Fastest;
    else if (text == "cheapest") out = Preference::Cheapest;
    else if (text == "fewest") out = Preference::FewestChanges;
    else return false;
    return true;
}

PlannerSettings settingsOf(const Network& net) {
    PlannerSettings s;
    s.similarTimeSeconds = net.plannerSimilarSeconds;
    s.maxRoutesPerGroup = net.plannerMaxRoutes;
    s.maxSlowerPercent = net.plannerMaxSlowerPercent;
    return s;
}

std::string routeSignature(const Network& net, const Route& r) {
    struct Leg {
        std::string family;
        std::vector<int> stations;
    };
    std::vector<Leg> legs;
    std::string walks;
    for (const auto& s : r.steps) {
        if (s.kind == Step::Kind::Ride) {
            std::vector<int> st;
            st.push_back(s.fromStation);
            st.insert(st.end(), s.via.begin(), s.via.end());
            st.push_back(s.toStation);
            const std::string& family = net.service(s.service).family;
            // continuing on the same line from where the last ride ended is the same stretch of journey
            if (!legs.empty() && legs.back().family == family && legs.back().stations.back() == st.front()) legs.back().stations.insert(legs.back().stations.end(), st.begin() + 1, st.end());
            else legs.push_back({family, st});
        } else if (s.kind == Step::Kind::Walk || s.fromStation != s.toStation) {
            walks += "W" + std::to_string(s.fromStation) + "-" + std::to_string(s.toStation) + ";";
            legs.push_back({"", {}});   // a walk between stations separates the legs
        }
    }
    std::string sig;
    for (const auto& leg : legs) {
        if (leg.family.empty() && leg.stations.empty()) {
            sig += "|";
            continue;
        }
        sig += leg.family + ":";
        for (int st : leg.stations) sig += std::to_string(st) + ",";
        sig += ";";
    }
    return sig + walks;
}

Planner::Planner(const Network& net, const Router& router, const FareCalculator& fares, PlannerSettings settings)
    : net_(net), router_(router), fares_(fares), settings_(settings) {}

Planner::Planner(const Network& net, const Router& router, const FareCalculator& fares)
    : Planner(net, router, fares, settingsOf(net)) {}

namespace {

const size_t kMaxCandidates = 40;
const size_t kMaxExpansions[2] = {100, 8};   // how many routes are varied in round one and round two

bool hasBadge(const Candidate& c, Preference p) { return std::find(c.badges.begin(), c.badges.end(), p) != c.badges.end(); }

int fareOf(const Candidate& c) { return c.fare.available ? c.fare.fareInr : -1; }

void addUnique(std::vector<int>& v, int x) {
    if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

}  // namespace

std::vector<Candidate> Planner::candidates(int from, int to, DayType day) const {
    std::vector<Candidate> out;
    std::set<std::string> seen;
    auto add = [&](const Route& r, const Restrictions& how) {
        if (!r.found) return false;
        const std::string sig = routeSignature(net_, r);
        if (!seen.insert(sig).second) return false;
        Candidate c;
        c.route = r;
        c.fare = fares_.quote(r, day);
        c.signature = sig;
        c.how = how;
        out.push_back(c);
        return true;
    };

    const Restrictions none;
    for (Mode m : {Mode::Fastest, Mode::FewestChanges}) add(router_.find(from, to, m), none);
    if (out.empty()) return out;

    // Round 1 varies the two best routes, round 2 varies what round 1 found.
    size_t levelStart = 0;
    for (int round = 0; round < 2 && out.size() < kMaxCandidates; ++round) {
        const size_t levelEnd = out.size();
        size_t expanded = 0;
        for (size_t i = levelStart; i < levelEnd && out.size() < kMaxCandidates; ++i) {
            if (expanded++ >= kMaxExpansions[round]) break;
            const Candidate parent = out[i];   // a copy: 'out' grows below

            std::vector<int> services, changeStations, operatorsUsed;
            std::vector<std::string> operatorNames;
            for (const auto& s : parent.route.steps) {
                if (s.kind == Step::Kind::Ride) {
                    addUnique(services, s.service);
                    const std::string& op = net_.service(s.service).op;
                    if (std::find(operatorNames.begin(), operatorNames.end(), op) == operatorNames.end()) operatorNames.push_back(op);
                } else if (s.kind == Step::Kind::Change) {
                    addUnique(changeStations, s.fromStation);
                }
            }
            std::vector<Restrictions> variants;
            for (int sv : services) {
                Restrictions r = parent.how;
                addUnique(r.bannedServices, sv);
                variants.push_back(r);
            }
            for (int st : changeStations) {
                Restrictions r = parent.how;
                addUnique(r.bannedChangeStations, st);
                variants.push_back(r);
            }
            for (const auto& op : operatorNames) {   // ban a whole operator (this is how a cheaper, all-DMRC route is found)
                Restrictions r = parent.how;
                for (const auto& s : net_.services)
                    if (s.op == op) addUnique(r.bannedServices, s.id);
                variants.push_back(r);
            }
            for (const auto& r : variants)
                for (Mode m : {Mode::Fastest, Mode::FewestChanges}) {
                    if (out.size() >= kMaxCandidates) break;
                    add(router_.find(from, to, m, r), r);
                }
        }
        levelStart = levelEnd;
    }

    // Badges: which needs each route fulfils.
    int bestKeptTime = out[0].route.totalSeconds, fewest = out[0].route.changes, lowestFare = -1;
    for (const auto& c : out) {
        bestKeptTime = std::min(bestKeptTime, c.route.totalSeconds);
        fewest = std::min(fewest, c.route.changes);
        if (fareOf(c) >= 0 && (lowestFare < 0 || fareOf(c) < lowestFare)) lowestFare = fareOf(c);
    }
    for (auto& c : out) {
        if (c.route.totalSeconds <= bestKeptTime + settings_.similarTimeSeconds) c.badges.push_back(Preference::Fastest);
        if (lowestFare >= 0 && fareOf(c) == lowestFare) c.badges.push_back(Preference::Cheapest);
        if (c.route.changes == fewest) c.badges.push_back(Preference::FewestChanges);
    }
    return out;
}

PlanResult Planner::plan(int from, int to, DayType day, Preference prefer) const {
    PlanResult res;
    res.prefer = prefer;
    if (from == to) return res;
    const std::vector<Candidate> all = candidates(from, to, day);
    res.candidatesConsidered = static_cast<int>(all.size());
    if (all.empty()) return res;
    res.found = true;

    auto byTime = [](const Candidate& a, const Candidate& b) {
        if (a.route.totalSeconds != b.route.totalSeconds) return a.route.totalSeconds < b.route.totalSeconds;
        if (a.route.changes != b.route.changes) return a.route.changes < b.route.changes;
        return a.signature < b.signature;
    };

    // The group of routes that fulfil one need, best first.
    auto groupFor = [&](Preference p) {
        Group g;
        g.criterion = p;
        std::vector<Candidate> members;
        for (const auto& c : all)
            if (hasBadge(c, p)) members.push_back(c);
        std::sort(members.begin(), members.end(), byTime);
        if (p != Preference::Fastest && !members.empty()) {   // drop routes far slower than the group's quickest
            const long long limit = static_cast<long long>(members[0].route.totalSeconds) * (100 + settings_.maxSlowerPercent) / 100;
            members.erase(std::remove_if(members.begin(), members.end(), [&](const Candidate& c) { return c.route.totalSeconds > limit; }), members.end());
        }
        if (static_cast<int>(members.size()) > settings_.maxRoutesPerGroup) members.resize(static_cast<size_t>(settings_.maxRoutesPerGroup));
        g.routes = members;
        return g;
    };
    auto minutes = [](int seconds) { return std::to_string((seconds + 30) / 60); };

    Group fastest = groupFor(Preference::Fastest), cheapest = groupFor(Preference::Cheapest), fewest = groupFor(Preference::FewestChanges);

    // summaries
    {
        const size_t n = fastest.routes.size();
        fastest.summary = n == 1 ? "The fastest route."
                                 : std::to_string(n) + " routes, all within " + minutes(settings_.similarTimeSeconds) + " min of the fastest.";
        const int changes = fewest.routes.empty() ? 0 : fewest.routes[0].route.changes;
        fewest.summary = fewest.routes.size() == 1 ? "Fewest changes: " + std::to_string(changes) + "."
                                                   : std::to_string(fewest.routes.size()) + " routes with the same fewest number of changes (" + std::to_string(changes) + ").";
        if (cheapest.routes.empty()) {
            cheapest.fellBack = true;
            cheapest.routes = fastest.routes;
            cheapest.summary = "No fare could be worked out for any route between these stations, so these are the fastest routes instead.";
        } else {
            const int fare = fareOf(cheapest.routes[0]);
            cheapest.summary = cheapest.routes.size() == 1 ? "Lowest fare: Rs " + std::to_string(fare) + "."
                                                           : std::to_string(cheapest.routes.size()) + " routes cost the same lowest fare (Rs " + std::to_string(fare) + ").";
        }
    }

    res.primary = prefer == Preference::Fastest ? fastest : (prefer == Preference::Cheapest ? cheapest : fewest);

    // The other needs are always answered too, even when the same route wins several of them.
    for (Preference p : {Preference::Fastest, Preference::Cheapest, Preference::FewestChanges}) {
        if (p == prefer) continue;
        res.others.push_back(p == Preference::Fastest ? fastest : (p == Preference::Cheapest ? cheapest : fewest));
    }

    // notes
    bool someUnknown = false, someKnown = false;
    for (const auto& c : all) (c.fare.available ? someKnown : someUnknown) = true;
    if (someUnknown && someKnown) res.notes.push_back("A fare could not be worked out for some routes (for example Aqua, Rapid Metro, or trips that mix operators), so those cannot be ranked as cheapest.");
    return res;
}

}  // namespace route
}  // namespace metro
