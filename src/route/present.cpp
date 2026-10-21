#include "present.h"

#include <cctype>
#include <sstream>

namespace metro {
namespace route {

namespace {

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

std::string minutesText(int seconds) {
    int m = (seconds + 30) / 60;
    if (m <= 0) return "less than a minute";
    return "about " + std::to_string(m) + " min";
}

std::string formatFare(const FareQuote& q, DayType day) {
    std::ostringstream o;
    if (!q.available) {
        o << "  Fare: not computed - " << q.reason << ".\n";
        return o.str();
    }
    const std::string dayText = day == DayType::Weekday ? "weekday" : "Sunday/holiday";
    if (q.basis == "pair table") {
        o << "  Fare: Rs " << q.fareInr << " (" << q.ticket << ", " << dayText << "), from the " << q.op << " fare table for these two stations.\n";
        return o.str();
    }
    const int tenths = (q.distanceM + 50) / 100;   // distance rounded to 0.1 km for display
    o << "  Fare: Rs " << q.fareInr << " (" << q.ticket << ", " << dayText << "), based on the shortest " << q.op
      << " route between the two stations: " << tenths / 10 << "." << tenths % 10 << " km.\n";
    return o.str();
}

std::string formatRoute(const Network& net, const Route& r, const std::string& label, bool showStops, const FareCalculator* fares, DayType day) {
    std::ostringstream o;
    if (!r.found) {
        o << label << ": no route found between " << net.station(r.fromStation).name << " and " << net.station(r.toStation).name << ".\n";
        return o.str();
    }
    o << label << ": " << minutesText(r.totalSeconds) << ", " << plural(r.changes, "change", "changes") << ", "
      << plural(r.stops, "stop", "stops") << "\n";
    int n = 0;
    for (const auto& s : r.steps) {
        ++n;
        o << "  " << n << ". ";
        const std::string& from = net.station(s.fromStation).name;
        const std::string& to = net.station(s.toStation).name;
        switch (s.kind) {
            case Step::Kind::Ride:
                o << net.service(s.service).name << ": board at " << from << ", towards " << net.station(s.towardsStation).name << ". Ride "
                  << plural(s.hops, "stop", "stops") << " to " << to << " (" << minutesText(s.seconds) << ").\n";
                if (showStops && !s.via.empty()) {
                    o << "       passing:";
                    for (size_t i = 0; i < s.via.size(); ++i) o << (i ? ", " : " ") << net.station(s.via[i]).name;
                    o << "\n";
                }
                break;
            case Step::Kind::Change:
                if (s.fromStation == s.toStation)
                    o << "Change at " << from << ": walk " << minutesText(s.seconds) << " to the " << net.service(s.service).name << " platform.\n";
                else
                    o << "Walk from " << from << " to " << to << " (" << minutesText(s.seconds) << "), then board the " << net.service(s.service).name << ".\n";
                break;
            case Step::Kind::Walk:
                o << "Walk from " << from << " to " << to << " (" << minutesText(s.seconds) << ").\n";
                break;
        }
    }
    if (fares) o << formatFare(fares->quote(r, day), day);
    return o.str();
}

std::string formatPlan(const Network& net, const Options& p, bool showStops, const FareCalculator* fares, DayType day) {
    std::ostringstream o;
    o << formatRoute(net, p.fastest, "FASTEST", showStops, fares, day);
    if (!p.fastest.found) return o.str();
    if (p.fewestDiffers) {
        const int fewer = p.fastest.changes - p.fewest.changes;
        o << "\n"
          << formatRoute(net, p.fewest,
                         "FEWEST CHANGES (" + plural(fewer, "change", "changes") + " fewer, " + minutesText(p.extraSeconds) + " slower)", showStops,
                         fares, day);
    } else {
        o << "\nThe fastest route also has the fewest changes.\n";
    }
    o << "\nTimes are estimates from the feed. They do not include waiting for trains.\n";
    if (fares) {
        bool unverified = false;
        for (const Route* r : {&p.fastest, &p.fewest}) {
            FareQuote q = fares->quote(*r, day);
            if (q.available && !q.verified) unverified = true;
        }
        if (unverified) o << "Fare slabs come from news reports of DMRC's announcement and have not been checked against DMRC's own page.\n";
    }
    return o.str();
}

std::string preferenceLabel(Preference p) {
    return p == Preference::Fastest ? "Fastest" : (p == Preference::Cheapest ? "Cheapest" : "Fewest changes");
}

namespace {
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string badgeText(const std::vector<Preference>& badges, Preference except, bool useExcept) {
    std::string out;
    for (Preference p : badges) {
        if (useExcept && p == except) continue;
        out += (out.empty() ? "" : ", ") + preferenceLabel(p);
    }
    return out;
}
}  // namespace

std::string formatPlanResult(const Network& net, const PlanResult& plan, bool showStops, const FareCalculator* fares, DayType day) {
    std::ostringstream o;
    if (!plan.found) {
        o << "No route found between these stations.\n";
        return o.str();
    }
    o << "YOUR CHOICE: " << preferenceLabel(plan.prefer) << ". " << plan.primary.summary << "\n\n";
    const size_t n = plan.primary.routes.size();
    for (size_t i = 0; i < n; ++i) {
        const Candidate& c = plan.primary.routes[i];
        std::string label = plan.primary.fellBack ? std::string("FASTEST (no fare available to compare)") : upper(preferenceLabel(plan.prefer));
        if (n > 1) label += " - ROUTE " + std::to_string(i + 1) + " OF " + std::to_string(n);
        const std::string also = badgeText(c.badges, plan.primary.fellBack ? Preference::Fastest : plan.prefer, true);
        if (!also.empty()) label += " (also best for: " + also + ")";
        o << formatRoute(net, c.route, label, showStops, fares, day) << "\n";
    }
    if (!plan.others.empty()) {
        o << "OTHER OPTIONS\n\n";
        for (const auto& g : plan.others) {
            if (g.routes.empty()) continue;
            std::string label = g.fellBack ? std::string("CHEAPEST: no fare available to compare, so the fastest route") : upper(preferenceLabel(g.criterion));
            o << formatRoute(net, g.routes[0].route, label, showStops, fares, day) << "\n";
        }
    }
    for (const auto& note : plan.notes) o << "Note: " << note << "\n";
    o << "Times are estimates from the feed. They do not include waiting for trains.\n";
    return o.str();
}

}  // namespace route
}  // namespace metro
