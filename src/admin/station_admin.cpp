#include "station_admin.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>

#include "../common/text.h"

namespace metro {
namespace admin {

namespace {

long long toLL(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

size_t utf8Length(const std::string& s) {      // characters, not bytes (the text was already checked to be valid UTF-8)
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

bool hasControlCharacter(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

constexpr long long kDay = 24LL * 3600;
constexpr long long kMaxStartAhead = 366 * kDay;      // a closure may be planned up to a year ahead
constexpr long long kMaxEndAhead = 10 * 366 * kDay;   // and may end at most ten years from now

}  // namespace

StationAdmin::StationAdmin(Database& db, std::function<long long()> clock) : db_(db), clock_(std::move(clock)) {}

long long StationAdmin::now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string StationAdmin::checkReason(const std::string& reason) const {
    if (!isValidUtf8(reason) || hasControlCharacter(reason)) return "The reason contains characters that are not allowed.";
    const size_t n = utf8Length(reason);
    if (n < 5) return "Give a reason of at least 5 characters.";
    if (n > 200) return "The reason can be at most 200 characters.";
    return std::string();
}

void StationAdmin::bumpDataVersion() {
    const long long v = db_.scalarInt("SELECT CAST(value AS INTEGER) FROM meta WHERE key='data_version';");
    db_.exec("UPDATE meta SET value = '" + std::to_string(v + 1) + "' WHERE key = 'data_version';");
}

namespace {

// Each station's current name: the feed's name, then every rename that has not been cancelled, oldest first.
std::map<std::string, std::string> currentNames(Database& db) {
    std::map<std::string, std::string> names;
    for (const auto& r : db.rows("SELECT gtfs_stop_id, name FROM stations"))
        names[r[0]] = r[1];
    for (const auto& r : db.rows("SELECT gtfs_stop_id, new_name FROM station_changes WHERE kind = 'rename' AND cancelled_at IS NULL ORDER BY change_id")) {
        auto it = names.find(r[0]);
        if (it != names.end()) it->second = r[1];
    }
    return names;
}

Result bad(Outcome o, const std::string& message) {
    Result r;
    r.outcome = o;
    r.message = message;
    return r;
}

}  // namespace

Result StationAdmin::rename(const std::string& gtfsId, const std::string& newNameRaw, const std::string& reason, const std::string& by) {
    if (by.empty()) return bad(Outcome::Invalid, "Who is making this change?");
    const auto names = currentNames(db_);
    const auto me = names.find(gtfsId);
    if (me == names.end()) return bad(Outcome::NotFound, "There is no such station.");
    const std::string newName = trim(newNameRaw);
    if (!isValidUtf8(newName) || hasControlCharacter(newName)) return bad(Outcome::Invalid, "The name contains characters that are not allowed.");
    const size_t len = utf8Length(newName);
    if (len < 2) return bad(Outcome::Invalid, "The name must be at least 2 characters.");
    if (len > 60) return bad(Outcome::Invalid, "The name can be at most 60 characters.");
    if (newName == me->second) return bad(Outcome::Invalid, "That is already the station's name.");
    for (const auto& n : names)
        if (n.first != gtfsId && lower(n.second) == lower(newName)) return bad(Outcome::Invalid, "Another station is already called '" + n.second + "'.");
    const std::string why = checkReason(reason);
    if (!why.empty()) return bad(Outcome::Invalid, why);

    const long long t = now();
    Transaction tx(db_);
    db_.exec("INSERT INTO station_changes(gtfs_stop_id, kind, new_name, reason, start_at, end_at, created_at, created_by) VALUES (" + sqlQuote(gtfsId) +
             ", 'rename', " + sqlQuote(newName) + ", " + sqlQuote(reason) + ", " + std::to_string(t) + ", NULL, " + std::to_string(t) + ", " + sqlQuote(by) + ");");
    Result r;
    r.changeId = static_cast<int>(db_.scalarInt("SELECT last_insert_rowid();"));
    bumpDataVersion();
    tx.commit();
    r.outcome = Outcome::Ok;
    r.message = "The station is now called '" + newName + "'.";
    return r;
}

Result StationAdmin::close(const std::string& gtfsId, const std::string& reason, long long startAt, long long endAt, const std::string& by) {
    if (by.empty()) return bad(Outcome::Invalid, "Who is making this change?");
    const auto names = currentNames(db_);
    if (names.find(gtfsId) == names.end()) return bad(Outcome::NotFound, "There is no such station.");
    const std::string why = checkReason(reason);
    if (!why.empty()) return bad(Outcome::Invalid, why);
    const long long t = now();
    if (startAt == 0) startAt = t;
    if (startAt < 0) return bad(Outcome::Invalid, "The start time is not valid.");
    if (startAt > t + kMaxStartAhead) return bad(Outcome::Invalid, "A closure can be planned at most a year ahead.");
    if (endAt < 0) return bad(Outcome::Invalid, "The end time is not valid.");
    if (endAt != 0) {
        if (endAt <= startAt) return bad(Outcome::Invalid, "The end must be after the start.");
        if (endAt <= t) return bad(Outcome::Invalid, "The end is already in the past.");
        if (endAt > t + kMaxEndAhead) return bad(Outcome::Invalid, "The end is too far ahead. Choose 'until further notice' instead.");
    }

    Transaction tx(db_);
    db_.exec("INSERT INTO station_changes(gtfs_stop_id, kind, new_name, reason, start_at, end_at, created_at, created_by) VALUES (" + sqlQuote(gtfsId) +
             ", 'closure', NULL, " + sqlQuote(reason) + ", " + std::to_string(startAt) + ", " + (endAt == 0 ? std::string("NULL") : std::to_string(endAt)) + ", " +
             std::to_string(t) + ", " + sqlQuote(by) + ");");
    Result r;
    r.changeId = static_cast<int>(db_.scalarInt("SELECT last_insert_rowid();"));
    bumpDataVersion();
    tx.commit();
    r.outcome = Outcome::Ok;
    r.message = endAt == 0 ? "The station is paused until further notice." : "The station is paused until the time you chose.";
    return r;
}

Result StationAdmin::cancel(int changeId, const std::string& by) {
    if (by.empty()) return bad(Outcome::Invalid, "Who is making this change?");
    const auto rows = db_.rows("SELECT kind, ifnull(end_at, 0), cancelled_at IS NOT NULL FROM station_changes WHERE change_id = " + std::to_string(changeId) + ";");
    if (rows.empty()) return bad(Outcome::NotFound, "There is no such change.");
    if (rows[0][2] == "1") return bad(Outcome::Invalid, "That change was already cancelled.");
    const long long t = now();
    const long long endAt = toLL(rows[0][1]);
    if (rows[0][0] == "closure" && endAt != 0 && endAt <= t) return bad(Outcome::Invalid, "That closure has already ended.");

    Transaction tx(db_);
    db_.exec("UPDATE station_changes SET cancelled_at = " + std::to_string(t) + ", cancelled_by = " + sqlQuote(by) + " WHERE change_id = " + std::to_string(changeId) + ";");
    bumpDataVersion();
    tx.commit();
    Result r;
    r.outcome = Outcome::Ok;
    r.changeId = changeId;
    r.message = rows[0][0] == "closure" ? "The station is open again." : "The rename was undone.";
    return r;
}

std::vector<ChangeRow> StationAdmin::history(int limit) {
    limit = std::max(1, std::min(limit, 2000));
    std::vector<ChangeRow> out;
    for (const auto& r : db_.rows("SELECT change_id, gtfs_stop_id, kind, ifnull(new_name, ''), reason, start_at, ifnull(end_at, 0), created_at, created_by, "
                                  "ifnull(cancelled_at, 0), ifnull(cancelled_by, '') FROM station_changes ORDER BY change_id DESC LIMIT " + std::to_string(limit) + ";")) {
        ChangeRow c;
        c.id = static_cast<int>(toLL(r[0]));
        c.gtfsId = r[1];
        c.kind = r[2];
        c.newName = r[3];
        c.reason = r[4];
        c.startAt = toLL(r[5]);
        c.endAt = toLL(r[6]);
        c.createdAt = toLL(r[7]);
        c.createdBy = r[8];
        c.cancelledAt = toLL(r[9]);
        c.cancelledBy = r[10];
        out.push_back(c);
    }
    return out;
}

}  // namespace admin
}  // namespace metro
