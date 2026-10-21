#include "accounts.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "crypto.h"

namespace metro {
namespace auth {

long long systemNow() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string formatUtc(long long t) {
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    long long z = days + 719468;   // civil-from-days algorithm (H. Hinnant)
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long d = doy - (153 * mp + 2) / 5 + 1;
    const long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld %02lld:%02lld UTC", y, m, d, rem / 3600, (rem % 3600) / 60);
    return buf;
}

namespace {

long long toLL(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

bool isTokenFormat(const std::string& t) {
    if (t.size() != 64) return false;
    for (char c : t)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

}  // namespace

AccountService::AccountService(Database& db, AccountOptions options) : db_(db), opts_(std::move(options)) {
    if (!opts_.clock) opts_.clock = systemNow;
}

const std::string& AccountService::dummyHash() {
    if (dummyHash_.empty()) dummyHash_ = hashPassword("this account does not exist", opts_.hashIterations);
    return dummyHash_;
}

RegisterResult AccountService::registerUser(const std::string& username, const std::string& password) {
    RegisterResult r;
    Check u = checkUsername(username);
    if (!u.ok) {
        r.message = u.message;
        return r;
    }
    Check p = checkPassword(password, username);
    if (!p.ok) {
        r.message = p.message;
        return r;
    }
    if (db_.scalarInt("SELECT COUNT(*) FROM users WHERE username = " + sqlQuote(username) + ";") > 0) {
        r.status = Status::UsernameTaken;
        r.message = "That username is already taken.";
        return r;
    }
    const std::string hash = hashPassword(password, opts_.hashIterations);   // the slow part, done before locking the database
    Transaction tx(db_);
    db_.exec("INSERT INTO users(username, password_hash, created_at) VALUES (" + sqlQuote(username) + "," + sqlQuote(hash) + "," +
             std::to_string(opts_.clock()) + ");");
    r.userId = static_cast<int>(db_.scalarInt("SELECT user_id FROM users WHERE username = " + sqlQuote(username) + ";"));
    tx.commit();
    r.status = Status::Ok;
    r.message = "Account created.";
    return r;
}

void AccountService::recordFailure(const std::string& username, long long now) {
    long long failures = 1, windowStart = now;
    const auto rows = db_.rows("SELECT failures, window_start FROM login_attempts WHERE username = " + sqlQuote(username) + ";");
    if (!rows.empty() && now - toLL(rows[0][1]) <= opts_.failureWindowSeconds) {
        failures = toLL(rows[0][0]) + 1;
        windowStart = toLL(rows[0][1]);
    }
    const long long lockedUntil = failures >= opts_.maxFailures ? now + opts_.lockSeconds : 0;
    db_.exec("INSERT OR REPLACE INTO login_attempts(username, failures, window_start, locked_until) VALUES (" + sqlQuote(username) + "," +
             std::to_string(failures) + "," + std::to_string(windowStart) + "," + std::to_string(lockedUntil) + ");");
}

LoginResult AccountService::login(const std::string& username, const std::string& password) {
    LoginResult r;
    r.message = "Wrong username or password.";
    const long long now = opts_.clock();

    // A name that could never be registered cannot exist. Do the same amount of work anyway, and do not
    // track it (so junk names cannot fill the attempts table).
    if (!checkUsername(username).ok) {
        verifyPassword(password, dummyHash());
        return r;
    }

    const auto attempts = db_.rows("SELECT locked_until FROM login_attempts WHERE username = " + sqlQuote(username) + ";");
    if (!attempts.empty()) {
        const long long lockedUntil = toLL(attempts[0][0]);
        if (lockedUntil > now) {
            r.status = Status::LockedOut;
            r.retryAfterSeconds = lockedUntil - now;
            r.message = "Too many failed attempts. Try again in " + std::to_string((r.retryAfterSeconds + 59) / 60) + " minute(s).";
            return r;
        }
        if (lockedUntil != 0) db_.exec("DELETE FROM login_attempts WHERE username = " + sqlQuote(username) + ";");   // lock has ended: start fresh
    }

    const auto users = db_.rows("SELECT user_id, password_hash FROM users WHERE username = " + sqlQuote(username) + ";");
    bool ok = false;
    if (users.empty()) verifyPassword(password, dummyHash());       // same work whether or not the name exists
    else ok = verifyPassword(password, users[0][1]);

    if (!ok) {
        recordFailure(username, now);
        return r;
    }

    const int userId = static_cast<int>(toLL(users[0][0]));
    Transaction tx(db_);
    db_.exec("DELETE FROM login_attempts WHERE username = " + sqlQuote(username) + ";");
    if (needsRehash(users[0][1], opts_.hashIterations))
        db_.exec("UPDATE users SET password_hash = " + sqlQuote(hashPassword(password, opts_.hashIterations)) + " WHERE user_id = " + std::to_string(userId) + ";");
    const std::string token = toHex(randomBytes(32));
    db_.exec("INSERT INTO sessions(token_hash, user_id, created_at, expires_at) VALUES (" + sqlQuote(sha256Hex(token)) + "," + std::to_string(userId) + "," +
             std::to_string(now) + "," + std::to_string(now + opts_.sessionSeconds) + ");");
    db_.exec("DELETE FROM sessions WHERE expires_at <= " + std::to_string(now) + ";");
    db_.exec("DELETE FROM sessions WHERE user_id = " + std::to_string(userId) + " AND token_hash NOT IN (SELECT token_hash FROM sessions WHERE user_id = " +
             std::to_string(userId) + " ORDER BY created_at DESC, rowid DESC LIMIT " + std::to_string(opts_.maxSessionsPerUser) + ");");
    tx.commit();

    r.status = Status::Ok;
    r.message = "Logged in.";
    r.token = token;
    r.userId = userId;
    return r;
}

std::optional<Account> AccountService::validateSession(const std::string& token) {
    if (!isTokenFormat(token)) return std::nullopt;
    const std::string hash = sha256Hex(token);
    const auto rows = db_.rows("SELECT s.user_id, u.username, s.expires_at FROM sessions s JOIN users u ON u.user_id = s.user_id WHERE s.token_hash = " +
                               sqlQuote(hash) + ";");
    if (rows.empty()) return std::nullopt;
    if (toLL(rows[0][2]) <= opts_.clock()) {
        db_.exec("DELETE FROM sessions WHERE token_hash = " + sqlQuote(hash) + ";");
        return std::nullopt;
    }
    Account a;
    a.id = static_cast<int>(toLL(rows[0][0]));
    a.username = rows[0][1];
    return a;
}

void AccountService::logout(const std::string& token) {
    if (!isTokenFormat(token)) return;
    db_.exec("DELETE FROM sessions WHERE token_hash = " + sqlQuote(sha256Hex(token)) + ";");
}

int AccountService::deleteExpiredSessions() {
    db_.exec("DELETE FROM sessions WHERE expires_at <= " + std::to_string(opts_.clock()) + ";");
    return static_cast<int>(db_.scalarInt("SELECT changes();"));
}

}  // namespace auth
}  // namespace metro
