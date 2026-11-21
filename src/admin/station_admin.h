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

struct ChangeRow {
    int id = 0;
    std::string gtfsId, kind;                 // kind: "rename" or "closure"
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

    // End a change early: reopens a closed station, or undoes a rename (the station gets its previous name back).
    Result cancel(int changeId, const std::string& by);

    // Newest first, cancelled ones included.
    std::vector<ChangeRow> history(int limit = 500);

    long long now() const;

private:
    std::string checkReason(const std::string& reason) const;
    void bumpDataVersion();

    Database& db_;
    std::function<long long()> clock_;
};

}  // namespace admin
}  // namespace metro
