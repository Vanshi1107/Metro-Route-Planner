// The journey planner: given what matters to the traveller (fastest, cheapest or fewest changes) it returns the routes
// that best fulfil that need, and then the best route(s) for each of the other two needs. Every need is always answered,
// even when one route wins several of them.
//
// HOW DIFFERENT ROUTES ARE FOUND. The router gives the single best route for time and for changes. To find genuinely
// different ones, the planner takes each route it has, bans one part of it (one line, one change station, or one whole
// operator), searches again, and keeps every new distinct route. It does this for two rounds. Nothing is thrown away
// here: the true fewest-changes or cheapest route must be found even if it is slow. Routes are cut down later, inside
// each group (see below).
//
// WHAT "FULFILS THE NEED" MEANS (the numbers are settings in config/settings.csv):
//   fastest        all routes within similar_time_seconds of the quickest
//   fewest changes all routes with the same lowest number of changes
//   cheapest       all routes with the same lowest fare, among routes whose fare can be worked out
// Within each group routes are ordered quickest first, groups drop routes more than max_slower_percent slower than the
// group's best, and at most max_routes_per_group routes are kept.
//
// "CHEAPEST" ON THIS NETWORK. On ordinary DMRC lines the fare depends only on the two stations, so every sensible route
// costs the same. Fares really differ only when a route uses the Airport Express (its own fare table). A route whose
// fare cannot be worked out (Aqua, Rapid Metro, mixed operators) cannot be ranked for "cheapest". If no route has a
// computable fare, "cheapest" falls back to the fastest routes and says so.
#pragma once
#include <string>
#include <vector>

#include "fare.h"
#include "network.h"
#include "router.h"

namespace metro {
namespace route {

enum class Preference { Fastest, Cheapest, FewestChanges };
const char* preferenceName(Preference p);                 // "fastest", "cheapest", "fewest"
bool parsePreference(const std::string& text, Preference& out);

struct PlannerSettings {
    int similarTimeSeconds = 180;
    int maxRoutesPerGroup = 3;
    int maxSlowerPercent = 25;
};
PlannerSettings settingsOf(const Network& net);

struct Candidate {
    Route route;
    FareQuote fare;
    std::string signature;                   // identifies the route: its rides and where it changes
    std::vector<Preference> badges;          // the needs this route fulfils, in any group
    Restrictions how;                        // what was banned to find it (empty for the two best routes)
};

struct Group {
    Preference criterion = Preference::Fastest;
    std::string summary;
    bool fellBack = false;                   // "cheapest" was asked for but no fare could be worked out
    std::vector<Candidate> routes;
};

struct PlanResult {
    bool found = false;
    Preference prefer = Preference::Fastest;
    Group primary;                           // the routes for the chosen need
    std::vector<Group> others;               // the other two needs, in the order fastest, cheapest, fewest changes
    std::vector<std::string> notes;
    int candidatesConsidered = 0;
};

class Planner {
public:
    Planner(const Network& net, const Router& router, const FareCalculator& fares, PlannerSettings settings);
    Planner(const Network& net, const Router& router, const FareCalculator& fares);   // settings from the network

    PlanResult plan(int fromStation, int toStation, DayType day, Preference prefer) const;

    // The distinct, worthwhile routes found, with badges (exposed for tests).
    std::vector<Candidate> candidates(int fromStation, int toStation, DayType day) const;

private:
    const Network& net_;
    const Router& router_;
    const FareCalculator& fares_;
    PlannerSettings settings_;
};

// Identifies a journey by the lines and stations a passenger would see. Two routes with the same signature are the same
// journey: for example riding "Blue via one branch service" or "Blue via the other" between the same stations, or
// changing between two services of the same line partway along it. Only the first one found is kept.
std::string routeSignature(const Network& net, const Route& r);

}  // namespace route
}  // namespace metro
