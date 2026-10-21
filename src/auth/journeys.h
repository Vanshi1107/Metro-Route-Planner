// Saved journeys.
//
// A saved journey stores the REQUEST (from, to, which option, weekday or Sunday) and a SNAPSHOT of the result
// together with the data version it was computed from. Opening it is instant when the data version has not changed.
// If the network data has been re-imported since, it is recomputed and the person is told what changed; if a station
// has disappeared or no route exists any more, the saved version is shown with a clear note and kept.
// Journeys belong to one user: every operation checks the owner, and someone else's journey looks like "not found".
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/db.h"
#include "../route/fare.h"
#include "../route/network.h"
#include "../route/planner.h"
#include "../route/router.h"

namespace metro {
namespace auth {

struct SavedJourney {
    long long id = 0;
    int userId = 0;
    std::string fromGtfsId, toGtfsId;
    std::string mode;          // "fastest" | "cheapest" | "fewest_changes"
    std::string dayType;       // "weekday" | "sunday_holiday"
    std::string label;
    int dataVersion = 0;
    int totalSeconds = 0;
    int changes = 0;
    int fareInr = -1;          // -1 = no fare was computed
    std::string resultText;
    bool favourite = false;
    long long createdAt = 0, updatedAt = 0;
};

enum class ReopenStatus { Current, Recomputed, StationMissing, NoRoute };

struct Reopened {
    ReopenStatus status = ReopenStatus::Current;
    SavedJourney journey;
    std::string text;          // what to show
    bool changed = false;      // Recomputed: did time, changes or fare differ from the saved version?
    std::string note;          // explanation to show with the text (empty when nothing needs saying)
};

struct SaveResult {
    bool ok = false;
    std::string message;
    long long id = 0;
    bool updatedExisting = false;
};

class JourneyService {
public:
    JourneyService(Database& db, const route::Network& net, const route::Planner& planner, const route::FareCalculator& fares,
                   std::function<long long()> clock = nullptr, int limitPerUser = 200);

    // Saves the best route for the chosen need (the first route the planner shows for it).
    SaveResult save(int userId, int fromStation, int toStation, route::Preference prefer, route::DayType day, const std::string& label);
    std::vector<SavedJourney> list(int userId);                               // favourites first, then most recently saved
    std::optional<SavedJourney> get(int userId, long long id);
    bool setFavourite(int userId, long long id, bool favourite);
    bool remove(int userId, long long id);
    std::optional<Reopened> reopen(int userId, long long id);

private:
    struct Snapshot {
        bool ok = false;
        int totalSeconds = 0, changes = 0, fareInr = -1;
        std::string text;
    };
    Snapshot makeSnapshot(int from, int to, route::Preference prefer, route::DayType day) const;

    Database& db_;
    const route::Network& net_;
    const route::Planner& planner_;
    const route::FareCalculator& fares_;
    std::function<long long()> clock_;
    int limitPerUser_;
    std::unordered_map<std::string, int> stationByGtfsId_;
};

}  // namespace auth
}  // namespace metro
