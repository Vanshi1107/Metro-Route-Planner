// Thin RAII wrapper around SQLite. All errors are thrown as std::runtime_error.
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;

namespace metro {

class Database {
public:
    // path may be ":memory:". If createIfMissing is false the file must already exist.
    Database(const std::string& path, bool createIfMissing);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const std::string& sql);                           // one or more statements
    long long scalarInt(const std::string& sql);                 // first column of first row
    std::string scalarText(const std::string& sql);
    std::vector<std::string> columnText(const std::string& sql); // first column of every row
    std::vector<std::vector<std::string>> rows(const std::string& sql);   // every column of every row (NULL -> "")
    bool tableExists(const std::string& name);

private:
    sqlite3* db_ = nullptr;
};

// Quotes a text value for use inside an SQL statement ('O''Brien'). Throws if it contains a NUL.
std::string sqlQuote(const std::string& s);

// Begins a transaction; rolls back on destruction unless commit() was called.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

}  // namespace metro
