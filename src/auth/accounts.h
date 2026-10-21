// Accounts, login and sessions.
//
// Safety rules built in:
//  * passwords are only ever stored as PBKDF2 hashes (see password.h)
//  * a login failure says only "Wrong username or password." whether or not the username exists, and takes
//    the same work either way, so the answer does not reveal which usernames are real
//  * after 5 failed logins in 15 minutes the name is locked for 15 minutes (counted for unknown names too)
//  * a session token is 256 random bits; only its SHA-256 is stored, so a leaked database cannot be used
//    to take over a session
//  * every query that includes text from the user goes through sqlQuote()
// Time comes from an injectable clock so tests can move it.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "../common/db.h"
#include "password.h"

namespace metro {
namespace auth {

long long systemNow();                        // seconds since 1970 (UTC)
std::string formatUtc(long long epochSeconds); // "2026-10-21 14:05 UTC"

struct AccountOptions {
    unsigned long hashIterations = kDefaultIterations;
    long long sessionSeconds = 7LL * 24 * 3600;     // a session lasts 7 days
    int maxSessionsPerUser = 10;                    // the oldest sessions beyond this are dropped
    int maxFailures = 5;
    long long failureWindowSeconds = 15 * 60;
    long long lockSeconds = 15 * 60;
    std::function<long long()> clock;               // empty = the system clock
};

struct Account {
    int id = 0;
    std::string username;
};

enum class Status { Ok, InvalidInput, UsernameTaken, WrongCredentials, LockedOut };

struct RegisterResult {
    Status status = Status::InvalidInput;
    std::string message;
    int userId = 0;
};

struct LoginResult {
    Status status = Status::WrongCredentials;
    std::string message;
    std::string token;                // 64 hex characters; shown once, never stored
    int userId = 0;
    long long retryAfterSeconds = 0;  // when locked out
};

class AccountService {
public:
    explicit AccountService(Database& db, AccountOptions options = AccountOptions());

    RegisterResult registerUser(const std::string& username, const std::string& password);
    LoginResult login(const std::string& username, const std::string& password);

    // The account a session token belongs to, or nothing if the token is unknown or expired.
    std::optional<Account> validateSession(const std::string& token);
    void logout(const std::string& token);
    int deleteExpiredSessions();

private:
    void recordFailure(const std::string& username, long long now);
    const std::string& dummyHash();

    Database& db_;
    AccountOptions opts_;
    std::string dummyHash_;
};

}  // namespace auth
}  // namespace metro
