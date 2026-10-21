#include <fstream>
#include <memory>
#include <sstream>

#include "../src/auth/accounts.h"
#include "../src/auth/crypto.h"
#include "../src/common/db.h"
#include "check.h"

using namespace metro;
using namespace metro::auth;

static long long g_now = 1700000000;   // the test controls the clock

struct Fixture {
    Database db{":memory:", true};
    AccountOptions opts;
    std::unique_ptr<AccountService> service;
    explicit Fixture(const std::string& schema, unsigned long iterations = 1000) {
        db.exec(schema);
        opts.hashIterations = iterations;     // fast for tests; the real default is tested in test_crypto
        opts.clock = [] { return g_now; };
        service = std::make_unique<AccountService>(db, opts);
    }
};

static void testRegistration(const std::string& schema) {
    Fixture f(schema);
    auto& s = *f.service;

    RegisterResult ok = s.registerUser("Alice", "correct horse");
    CHECK(ok.status == Status::Ok);
    CHECK(ok.userId > 0);
    CHECK(s.registerUser("alice", "another password").status == Status::UsernameTaken);   // names are unique ignoring case
    CHECK(s.registerUser("ALICE", "another password").status == Status::UsernameTaken);

    CHECK(s.registerUser("ab", "correct horse").status == Status::InvalidInput);          // username too short
    CHECK(s.registerUser("bad name", "correct horse").status == Status::InvalidInput);    // space in the username
    CHECK(s.registerUser("bobby", "short").status == Status::InvalidInput);               // password too short
    CHECK(s.registerUser("bobby", "bobby").status == Status::InvalidInput);
    CHECK(!s.registerUser("bobby", "short").message.empty());
    CHECK_EQ(f.db.scalarInt("SELECT COUNT(*) FROM users;"), 1LL);                         // rejected attempts leave nothing behind

    // the password is stored only as a hash, with its algorithm and cost
    const std::string stored = f.db.scalarText("SELECT password_hash FROM users WHERE username='alice';");
    CHECK(stored.rfind("pbkdf2-sha256$1000$", 0) == 0);
    CHECK(stored.find("correct horse") == std::string::npos);
    CHECK_EQ(f.db.scalarInt("SELECT created_at FROM users WHERE username='alice';"), g_now);

    // SQL injection through the username is just a rejected name
    CHECK(s.registerUser("x' OR '1'='1", "correct horse").status == Status::InvalidInput);
    CHECK(s.registerUser("a'; DROP TABLE users; --", "correct horse").status == Status::InvalidInput);
    CHECK(f.db.tableExists("users"));
}

static void testLoginAndSessions(const std::string& schema) {
    Fixture f(schema);
    auto& s = *f.service;
    s.registerUser("alice", "correct horse");

    LoginResult l = s.login("alice", "correct horse");
    CHECK(l.status == Status::Ok);
    CHECK_EQ(l.token.size(), static_cast<size_t>(64));
    CHECK(l.userId > 0);
    // the raw token is never stored, only its hash
    CHECK_EQ(f.db.scalarInt("SELECT COUNT(*) FROM sessions WHERE token_hash = '" + sha256Hex(l.token) + "';"), 1LL);
    CHECK_EQ(f.db.scalarInt("SELECT COUNT(*) FROM sessions WHERE token_hash = '" + l.token + "';"), 0LL);

    auto acct = s.validateSession(l.token);
    CHECK(acct.has_value());
    if (acct) {
        CHECK_EQ(acct->username, std::string("alice"));
        CHECK_EQ(acct->id, l.userId);
    }
    CHECK(!s.validateSession("").has_value());
    CHECK(!s.validateSession(std::string(64, 'a')).has_value());          // well-formed but unknown
    CHECK(!s.validateSession("not a token' OR '1'='1").has_value());
    CHECK(!s.validateSession(l.token.substr(0, 63)).has_value());

    LoginResult second = s.login("ALICE", "correct horse");                // usernames ignore case when logging in
    CHECK(second.status == Status::Ok);
    CHECK(second.token != l.token);                                        // every login gets a fresh token

    // sessions expire
    g_now += f.opts.sessionSeconds - 1;
    CHECK(s.validateSession(l.token).has_value());
    g_now += 2;
    CHECK(!s.validateSession(l.token).has_value());
    CHECK_EQ(f.db.scalarInt("SELECT COUNT(*) FROM sessions WHERE token_hash = '" + sha256Hex(l.token) + "';"), 0LL);   // and are removed

    // logout ends a session; it is safe to do twice
    LoginResult third = s.login("alice", "correct horse");
    CHECK(s.validateSession(third.token).has_value());
    s.logout(third.token);
    CHECK(!s.validateSession(third.token).has_value());
    s.logout(third.token);
    s.logout("garbage");

    // old sessions are cleaned up
    LoginResult fourth = s.login("alice", "correct horse");
    g_now += f.opts.sessionSeconds + 10;
    CHECK(s.deleteExpiredSessions() >= 1);
    CHECK(!s.validateSession(fourth.token).has_value());
}

static void testWrongCredentials(const std::string& schema) {
    Fixture f(schema);
    auto& s = *f.service;
    s.registerUser("alice", "correct horse");

    LoginResult wrong = s.login("alice", "wrong password");
    LoginResult unknown = s.login("nobodyhere", "wrong password");
    CHECK(wrong.status == Status::WrongCredentials);
    CHECK(unknown.status == Status::WrongCredentials);
    CHECK_EQ(wrong.message, unknown.message);                              // the answer does not reveal which names exist
    CHECK(wrong.token.empty());
    CHECK_EQ(f.db.scalarInt("SELECT COUNT(*) FROM sessions;"), 0LL);

    CHECK(s.login("", "x").status == Status::WrongCredentials);
    CHECK(s.login("alice", "").status == Status::WrongCredentials);
    CHECK(s.login("x' OR '1'='1", "anything").status == Status::WrongCredentials);   // SQL injection attempts just fail
    CHECK(s.login("alice' --", "anything").status == Status::WrongCredentials);
    CHECK(s.login(std::string("alice\0x", 7), "correct horse").status == Status::WrongCredentials);
    CHECK_EQ(f.db.scalarInt("SELECT COUNT(*) FROM users;"), 1LL);
    CHECK(s.login("alice", "correct horse").status == Status::Ok);                   // and the real account still works
}

static void testLockout(const std::string& schema) {
    Fixture f(schema);
    auto& s = *f.service;
    s.registerUser("alice", "correct horse");

    for (int i = 0; i < 4; ++i) CHECK(s.login("alice", "wrong guess").status == Status::WrongCredentials);
    CHECK(s.login("alice", "correct horse").status == Status::Ok);        // a success before the limit clears the count
    for (int i = 0; i < 4; ++i) CHECK(s.login("alice", "wrong guess").status == Status::WrongCredentials);
    CHECK(s.login("alice", "wrong guess").status == Status::WrongCredentials);   // the 5th failure triggers the lock

    LoginResult locked = s.login("alice", "correct horse");               // even the right password is refused now
    CHECK(locked.status == Status::LockedOut);
    CHECK(locked.token.empty());
    CHECK(locked.retryAfterSeconds > 0 && locked.retryAfterSeconds <= f.opts.lockSeconds);
    CHECK(locked.message.find("minute") != std::string::npos);
    CHECK(s.login("ALICE", "correct horse").status == Status::LockedOut); // the lock ignores case

    g_now += f.opts.lockSeconds - 1;
    CHECK(s.login("alice", "correct horse").status == Status::LockedOut);
    g_now += 2;                                                           // lock over
    CHECK(s.login("alice", "correct horse").status == Status::Ok);

    // after a lock ends the count starts again from zero
    for (int i = 0; i < 4; ++i) CHECK(s.login("alice", "wrong guess").status == Status::WrongCredentials);
    CHECK(s.login("alice", "correct horse").status == Status::Ok);

    // failures spread over more than the window do not add up
    for (int i = 0; i < 4; ++i) {
        CHECK(s.login("alice", "wrong guess").status == Status::WrongCredentials);
        g_now += f.opts.failureWindowSeconds + 1;
    }
    CHECK(s.login("alice", "wrong guess").status == Status::WrongCredentials);
    CHECK(s.login("alice", "correct horse").status == Status::Ok);

    // names that do not exist are counted too, so lockouts do not reveal real names
    for (int i = 0; i < 5; ++i) s.login("ghostuser", "wrong guess");
    CHECK(s.login("ghostuser", "wrong guess").status == Status::LockedOut);
    // and one name being locked does not affect another
    s.registerUser("bobby", "another password");
    CHECK(s.login("bobby", "another password").status == Status::Ok);
}

static void testSessionLimitAndRehash(const std::string& schema) {
    {
        Fixture f(schema);
        f.opts.maxSessionsPerUser = 3;
        f.service = std::make_unique<AccountService>(f.db, f.opts);
        auto& s = *f.service;
        s.registerUser("alice", "correct horse");
        std::vector<std::string> tokens;
        for (int i = 0; i < 5; ++i) {
            g_now += 1;
            tokens.push_back(s.login("alice", "correct horse").token);
        }
        CHECK_EQ(f.db.scalarInt("SELECT COUNT(*) FROM sessions;"), 3LL);   // only the newest 3 remain
        CHECK(!s.validateSession(tokens[0]).has_value());
        CHECK(!s.validateSession(tokens[1]).has_value());
        CHECK(s.validateSession(tokens[4]).has_value());
    }
    {   // a hash made with fewer iterations is upgraded the next time the person logs in
        Fixture f(schema, 1000);
        f.service->registerUser("alice", "correct horse");
        CHECK(f.db.scalarText("SELECT password_hash FROM users;").rfind("pbkdf2-sha256$1000$", 0) == 0);
        AccountOptions stronger = f.opts;
        stronger.hashIterations = 2000;
        AccountService upgraded(f.db, stronger);
        CHECK(upgraded.login("alice", "wrong guess").status == Status::WrongCredentials);
        CHECK(f.db.scalarText("SELECT password_hash FROM users;").rfind("pbkdf2-sha256$1000$", 0) == 0);   // not upgraded on a failure
        CHECK(upgraded.login("alice", "correct horse").status == Status::Ok);
        CHECK(f.db.scalarText("SELECT password_hash FROM users;").rfind("pbkdf2-sha256$2000$", 0) == 0);
        CHECK(upgraded.login("alice", "correct horse").status == Status::Ok);                              // still works afterwards
    }
}

static void testDeletingAUserEndsTheirSessions(const std::string& schema) {
    Fixture f(schema);
    auto& s = *f.service;
    s.registerUser("alice", "correct horse");
    LoginResult l = s.login("alice", "correct horse");
    CHECK(s.validateSession(l.token).has_value());
    f.db.exec("DELETE FROM users WHERE username = 'alice';");
    CHECK(!s.validateSession(l.token).has_value());
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_accounts <schema.sql>\n";
        return 2;
    }
    std::ifstream sf(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << sf.rdbuf();
    const std::string schema = ss.str();
    try {
        testRegistration(schema);
        testLoginAndSessions(schema);
        testWrongCredentials(schema);
        testLockout(schema);
        testSessionLimitAndRehash(schema);
        testDeletingAUserEndsTheirSessions(schema);
    } catch (const std::exception& e) {
        std::cerr << "test_accounts: unexpected exception: " << e.what() << "\n";
        return 1;
    }
    return finish("test_accounts");
}
