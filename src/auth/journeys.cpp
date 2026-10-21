#include "journeys.h"

#include <cstdlib>

#include "../common/text.h"
#include "../route/present.h"
#include "accounts.h"

using namespace metro::route;

namespace metro {
namespace auth {

namespace {

long long toLL(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

std::string modeName(Preference p) {
    return p == Preference::Fastest ? "fastest" : (p == Preference::Cheapest ? "cheapest" : "fewest_changes");
}

Preference preferenceOf(const std::string& mode) {
    return mode == "fastest" ? Preference::Fastest : (mode == "cheapest" ? Preference::Cheapest : Preference::FewestChanges);
}

const char* labelOf(Preference p) {
    return p == Preference::Fastest ? "FASTEST" : (p == Preference::Cheapest ? "CHEAPEST" : "FEWEST CHANGES");
}

const char* kColumns =
    "journey_id,user_id,from_gtfs_stop_id,to_gtfs_stop_id,mode,day_type,label,data_version,total_seconds,changes,fare_inr,result_text,is_favourite,created_at,updated_at";

SavedJourney fromRow(const std::vector<std::string>& r) {
    SavedJourney j;
    j.id = toLL(r[0]);
    j.userId = static_cast<int>(toLL(r[1]));
    j.fromGtfsId = r[2];
    j.toGtfsId = r[3];
    j.mode = r[4];
    j.dayType = r[5];
    j.label = r[6];
    j.dataVersion = static_cast<int>(toLL(r[7]));
    j.totalSeconds = static_cast<int>(toLL(r[8]));
    j.changes = static_cast<int>(toLL(r[9]));
    j.fareInr = r[10].empty() ? -1 : static_cast<int>(toLL(r[10]));   // NULL comes back as ""
    j.resultText = r[11];
    j.favourite = r[12] == "1";
    j.createdAt = toLL(r[13]);
    j.updatedAt = toLL(r[14]);
    return j;
}

std::string fareSql(int fare) { return fare < 0 ? std::string("NULL") : std::to_string(fare); }

std::string describeResult(int seconds, int changes, int fare) {
    std::string s = minutesText(seconds) + ", " + std::to_string(changes) + (changes == 1 ? " change" : " changes");
    s += fare >= 0 ? ", Rs " + std::to_string(fare) : ", no fare";
    return s;
}

}  // namespace

JourneyService::JourneyService(Database& db, const Network& net, const Planner& planner, const FareCalculator& fares,
                               std::function<long long()> clock, int limitPerUser)
    : db_(db), net_(net), planner_(planner), fares_(fares), clock_(clock ? clock : std::function<long long()>(systemNow)), limitPerUser_(limitPerUser) {
    for (const auto& s : net.stations) stationByGtfsId_[s.gtfsId] = s.id;
}

JourneyService::Snapshot JourneyService::makeSnapshot(int from, int to, Preference prefer, DayType day) const {
    Snapshot s;
    if (from == to) return s;
    const PlanResult plan = planner_.plan(from, to, day, prefer);
    if (!plan.found || plan.primary.routes.empty()) return s;
    const Candidate& best = plan.primary.routes[0];
    s.ok = true;
    s.totalSeconds = best.route.totalSeconds;
    s.changes = best.route.changes;
    s.fareInr = best.fare.available ? best.fare.fareInr : -1;
    s.text = net_.station(from).name + "  ->  " + net_.station(to).name + "\n\n" + formatRoute(net_, best.route, labelOf(prefer), false, &fares_, day);
    return s;
}

SaveResult JourneyService::save(int userId, int from, int to, Preference prefer, DayType day, const std::string& label) {
    SaveResult res;
    if (!isValidUtf8(label)) {
        res.message = "The label must be valid text.";
        return res;
    }
    if (label.size() > 60) {
        res.message = "The label can be at most 60 characters.";
        return res;
    }
    for (unsigned char c : label)
        if (c < 0x20 || c == 0x7f) {
            res.message = "The label must not contain control characters.";
            return res;
        }
    if (from < 1 || to < 1 || from > static_cast<int>(net_.stations.size()) || to > static_cast<int>(net_.stations.size()) || from == to) {
        res.message = "Choose two different, valid stations.";
        return res;
    }
    const Snapshot snap = makeSnapshot(from, to, prefer, day);
    if (!snap.ok) {
        res.message = "There is no route between those stations, so there is nothing to save.";
        return res;
    }
    const std::string fromId = net_.station(from).gtfsId, toId = net_.station(to).gtfsId;
    const std::string where = "user_id = " + std::to_string(userId) + " AND from_gtfs_stop_id = " + sqlQuote(fromId) + " AND to_gtfs_stop_id = " +
                              sqlQuote(toId) + " AND mode = " + sqlQuote(modeName(prefer)) + " AND day_type = " + sqlQuote(dayTypeName(day));
    const long long now = clock_();
    const auto existing = db_.rows("SELECT journey_id FROM saved_journeys WHERE " + where + ";");
    if (!existing.empty()) {   // saving the same trip again refreshes it
        res.id = toLL(existing[0][0]);
        db_.exec("UPDATE saved_journeys SET label = " + sqlQuote(label.empty() ? std::string() : label) + ", data_version = " + std::to_string(net_.dataVersion) +
                 ", total_seconds = " + std::to_string(snap.totalSeconds) + ", changes = " + std::to_string(snap.changes) + ", fare_inr = " + fareSql(snap.fareInr) +
                 ", result_text = " + sqlQuote(snap.text) + ", updated_at = " + std::to_string(now) + " WHERE journey_id = " + std::to_string(res.id) + ";");
        res.ok = true;
        res.updatedExisting = true;
        res.message = "Journey updated.";
        return res;
    }
    if (db_.scalarInt("SELECT COUNT(*) FROM saved_journeys WHERE user_id = " + std::to_string(userId) + ";") >= limitPerUser_) {
        res.message = "You have reached the limit of " + std::to_string(limitPerUser_) + " saved journeys. Delete one first.";
        return res;
    }
    db_.exec(std::string("INSERT INTO saved_journeys(user_id,from_gtfs_stop_id,to_gtfs_stop_id,mode,day_type,label,data_version,total_seconds,changes,fare_inr,result_text,is_favourite,created_at,updated_at) VALUES (") +
             std::to_string(userId) + "," + sqlQuote(fromId) + "," + sqlQuote(toId) + "," + sqlQuote(modeName(prefer)) + "," + sqlQuote(dayTypeName(day)) + "," +
             sqlQuote(label) + "," + std::to_string(net_.dataVersion) + "," + std::to_string(snap.totalSeconds) + "," + std::to_string(snap.changes) + "," +
             fareSql(snap.fareInr) + "," + sqlQuote(snap.text) + ",0," + std::to_string(now) + "," + std::to_string(now) + ");");
    res.id = db_.scalarInt("SELECT last_insert_rowid();");
    res.ok = true;
    res.message = "Journey saved.";
    return res;
}

std::vector<SavedJourney> JourneyService::list(int userId) {
    std::vector<SavedJourney> out;
    for (const auto& r : db_.rows(std::string("SELECT ") + kColumns + " FROM saved_journeys WHERE user_id = " + std::to_string(userId) +
                                  " ORDER BY is_favourite DESC, updated_at DESC, journey_id DESC;"))
        out.push_back(fromRow(r));
    return out;
}

std::optional<SavedJourney> JourneyService::get(int userId, long long id) {
    const auto rows = db_.rows(std::string("SELECT ") + kColumns + " FROM saved_journeys WHERE journey_id = " + std::to_string(id) +
                               " AND user_id = " + std::to_string(userId) + ";");
    if (rows.empty()) return std::nullopt;
    return fromRow(rows[0]);
}

bool JourneyService::setFavourite(int userId, long long id, bool favourite) {
    if (!get(userId, id)) return false;
    db_.exec(std::string("UPDATE saved_journeys SET is_favourite = ") + (favourite ? "1" : "0") + " WHERE journey_id = " + std::to_string(id) +
             " AND user_id = " + std::to_string(userId) + ";");
    return true;
}

bool JourneyService::remove(int userId, long long id) {
    if (!get(userId, id)) return false;
    db_.exec("DELETE FROM saved_journeys WHERE journey_id = " + std::to_string(id) + " AND user_id = " + std::to_string(userId) + ";");
    return true;
}

std::optional<Reopened> JourneyService::reopen(int userId, long long id) {
    auto saved = get(userId, id);
    if (!saved) return std::nullopt;
    Reopened out;
    out.journey = *saved;
    out.text = saved->resultText;

    if (saved->dataVersion == net_.dataVersion) {   // nothing has changed since it was saved
        out.status = ReopenStatus::Current;
        return out;
    }

    const std::string versions = "version " + std::to_string(saved->dataVersion) + " to " + std::to_string(net_.dataVersion);
    auto fromIt = stationByGtfsId_.find(saved->fromGtfsId), toIt = stationByGtfsId_.find(saved->toGtfsId);
    if (fromIt == stationByGtfsId_.end() || toIt == stationByGtfsId_.end()) {
        out.status = ReopenStatus::StationMissing;
        out.note = "The network data was updated (" + versions + ") and a station in this journey no longer exists. Showing the version you saved; it may be out of date.";
        return out;
    }
    const Preference prefer = preferenceOf(saved->mode);
    const DayType day = saved->dayType == "weekday" ? DayType::Weekday : DayType::SundayHoliday;
    const Snapshot snap = makeSnapshot(fromIt->second, toIt->second, prefer, day);
    if (!snap.ok) {
        out.status = ReopenStatus::NoRoute;
        out.note = "The network data was updated (" + versions + ") and there is no longer a route between these stations. Showing the version you saved; it may be out of date.";
        return out;
    }

    out.status = ReopenStatus::Recomputed;
    out.changed = snap.totalSeconds != saved->totalSeconds || snap.changes != saved->changes || snap.fareInr != saved->fareInr;
    out.text = snap.text;
    out.note = out.changed ? "The network data was updated (" + versions + ") and this journey changed. It was: " +
                                 describeResult(saved->totalSeconds, saved->changes, saved->fareInr) + ". It is now: " +
                                 describeResult(snap.totalSeconds, snap.changes, snap.fareInr) + "."
                           : "The network data was updated (" + versions + "). This journey is unchanged.";
    db_.exec("UPDATE saved_journeys SET data_version = " + std::to_string(net_.dataVersion) + ", total_seconds = " + std::to_string(snap.totalSeconds) +
             ", changes = " + std::to_string(snap.changes) + ", fare_inr = " + fareSql(snap.fareInr) + ", result_text = " + sqlQuote(snap.text) +
             ", updated_at = " + std::to_string(clock_()) + " WHERE journey_id = " + std::to_string(id) + " AND user_id = " + std::to_string(userId) + ";");
    out.journey.dataVersion = net_.dataVersion;
    out.journey.totalSeconds = snap.totalSeconds;
    out.journey.changes = snap.changes;
    out.journey.fareInr = snap.fareInr;
    out.journey.resultText = snap.text;
    return out;
}

}  // namespace auth
}  // namespace metro
