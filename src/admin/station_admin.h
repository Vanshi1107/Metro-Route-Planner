// Station changes made by an admin: rename a station, or pause (close) it for a while or until further notice.
//
// Where it fits: these rows live in their own table (station_changes), so re-importing the network never undoes them.
// Every change raises meta.data_version, which is how the running server notices and reloads. Rows are never deleted:
// "cancelling" a change only stamps it, so the table doubles as an audit history (who, what, when, and why).
//
// This class only does the DATABASE side and all the validation. Who is allowed to call it (admins only, local only,
// CSRF checks) is decided by the web layer in src/web/api.cpp.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../common/db.h"

namespace metro {
namespace admin {

// Which table a change lives in: a station change (rename, closure) or a line change (suspension, delay, announcement).
enum class Scope { Station, Line };

struct ChangeRow {
    Scope scope = Scope::Station;
    int id = 0;                               // unique within its scope (the two tables number separately)
    std::string gtfsId;                       // station changes
    std::string lineFamily;                   // line changes ("" = all lines, for an announcement)
    std::string kind;                         // "rename", "closure", "suspension", "delay" or "announcement"
    int delayMinutes = 0;                     // delays
    int replacesId = 0;                       // this row replaced an earlier one in the same scope (its end was moved)
    std::string newName, reason, createdBy, cancelledBy;
    long long startAt = 0, endAt = 0;         // seconds since 1970 (UTC); endAt 0 = until further notice
    long long createdAt = 0, cancelledAt = 0; // cancelledAt 0 = not cancelled
};

enum class Outcome { Ok, Invalid, NotFound };

struct Result {
    Outcome outcome = Outcome::Invalid;
    std::string message;                      // plain words, safe to show to the admin
    int changeId = 0;                         // the new change (rename, close) or the one that was cancelled
};

class StationAdmin {
public:
    explicit StationAdmin(Database& db, std::function<long long()> clock = nullptr);   // empty clock = the system clock

    // Give a station a new name from now on. 2 to 60 characters, not another station's name, with a reason of 5 to 200 characters.
    Result rename(const std::string& gtfsId, const std::string& newName, const std::string& reason, const std::string& by);

    // Pause a station: trains run through without stopping. startAt 0 = now. endAt 0 = until further notice (permanent).
    // A temporary closure needs an end after its start and after now.
    Result close(const std::string& gtfsId, const std::string& reason, long long startAt, long long endAt, const std::string& by);

    // A whole line: kind "suspension" (no trains), "delay" (minutes 1 to 240; information only) or "announcement" (information
    // only; family "" = the whole network). startAt 0 = now; endAt 0 = until further notice. Same time rules as close().
    Result lineChange(const std::string& family, const std::string& kind, int delayMinutes, const std::string& reason, long long startAt,
                      long long endAt, const std::string& by);

    // End a change early: reopens a closed station, undoes a rename, ends a line change.
    Result cancel(int changeId, const std::string& by);              // a station change
    Result cancel(Scope scope, int changeId, const std::string& by);

    // Move the end of a closure or line change that has not ended (newEnd 0 = until further notice). The old row is kept,
    // marked cancelled, and a new row that "replaces" it is written, so the history shows both.
    Result extend(Scope scope, int changeId, long long newEnd, const std::string& by);

    // Newest first, cancelled ones included.
    std::vector<ChangeRow> history(int limit = 500);                 // station changes only
    std::vector<ChangeRow> historyAll(int limit = 2000);             // both tables, newest first

    long long now() const;

private:
    std::string checkReason(const std::string& reason) const;
    void bumpDataVersion();

    Database& db_;
    std::function<long long()> clock_;
};

}  // namespace admin
}  // namespace metro
