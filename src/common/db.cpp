#include "db.h"

#include <sqlite3.h>

namespace metro {

namespace {
[[noreturn]] void fail(sqlite3* db, const std::string& what) {
    std::string msg = what;
    if (db) msg += ": " + std::string(sqlite3_errmsg(db));
    throw std::runtime_error(msg);
}
}  // namespace

Database::Database(const std::string& path, bool createIfMissing) {
    int flags = SQLITE_OPEN_READWRITE | (createIfMissing ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open database '" + path + "': " + msg);
    }
    // SQLite does NOT enforce foreign keys unless asked, on every connection.
    exec("PRAGMA foreign_keys = ON;");
    if (scalarInt("PRAGMA foreign_keys;") != 1)
        throw std::runtime_error("could not enable foreign key enforcement");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQL error: " + msg);
    }
}

long long Database::scalarInt(const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) fail(db_, "prepare failed");
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        long long v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return v;
    }
    sqlite3_finalize(st);
    throw std::runtime_error("query returned no row: " + sql);
}

std::string Database::scalarText(const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) fail(db_, "prepare failed");
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 0);
        std::string s = t ? reinterpret_cast<const char*>(t) : "";
        sqlite3_finalize(st);
        return s;
    }
    sqlite3_finalize(st);
    throw std::runtime_error("query returned no row: " + sql);
}

std::vector<std::string> Database::columnText(const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) fail(db_, "prepare failed");
    std::vector<std::string> out;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(st, 0);
        out.emplace_back(t ? reinterpret_cast<const char*>(t) : "");
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) fail(db_, "step failed");
    return out;
}

std::vector<std::vector<std::string>> Database::rows(const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) fail(db_, "prepare failed");
    const int cols = sqlite3_column_count(st);
    std::vector<std::vector<std::string>> out;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        std::vector<std::string> row;
        row.reserve(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            const unsigned char* t = sqlite3_column_text(st, c);
            row.emplace_back(t ? reinterpret_cast<const char*>(t) : "");
        }
        out.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) fail(db_, "step failed");
    return out;
}

bool Database::tableExists(const std::string& name) {
    // Names come from our own fixed list, never from user input.
    return scalarInt("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='" + name + "';") > 0;
}

std::string sqlQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\0') throw std::runtime_error("text value contains a NUL character");
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN;"); }

Transaction::~Transaction() {
    if (!done_) {
        try {
            db_.exec("ROLLBACK;");
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    db_.exec("COMMIT;");
    done_ = true;
}

}  // namespace metro
