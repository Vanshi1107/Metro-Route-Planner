// Turning routes into text a traveller can follow.
#pragma once
#include <string>

#include "fare.h"
#include "planner.h"
#include "router.h"

namespace metro {
namespace route {

// "about 12 min", "less than a minute"
std::string minutesText(int seconds);

// One line about the fare: the amount and its basis, or "not computed" and why.
std::string formatFare(const FareQuote& quote, DayType day);

// One route: a headline, numbered steps and (if 'fares' is given) a fare line. 'label' is e.g. "FASTEST".
std::string formatRoute(const Network& net, const Route& route, const std::string& label, bool showStops,
                        const FareCalculator* fares = nullptr, DayType day = DayType::Weekday);

// What the planner offers: the fastest route, plus the fewest-changes route only when it has fewer changes.
std::string formatPlan(const Network& net, const Options& options, bool showStops, const FareCalculator* fares = nullptr,
                       DayType day = DayType::Weekday);

// What the planner found: the routes that best fulfil the traveller's need, then the other needs.
std::string formatPlanResult(const Network& net, const PlanResult& plan, bool showStops, const FareCalculator* fares, DayType day);

// "Fastest", "Cheapest", "Fewest changes"
std::string preferenceLabel(Preference p);

}  // namespace route
}  // namespace metro
