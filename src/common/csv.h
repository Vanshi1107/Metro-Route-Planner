// Streaming CSV reader for GTFS files.
// Handles: UTF-8 BOM, CRLF or LF line endings, quoted fields containing commas,
// doubled quotes ("") inside quoted fields, newlines inside quoted fields, blank lines.
// Reads the file in chunks, so a 400k-row file does not need to fit in memory.
#pragma once
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace metro {

std::string trim(const std::string& s);

class CsvReader {
public:
    explicit CsvReader(const std::string& path);

    bool ok() const { return ok_; }                       // file opened and has a header row
    const std::vector<std::string>& header() const { return header_; }
    int column(const std::string& name) const;            // index of a header column, or -1
    bool next(std::vector<std::string>& row);             // next data row; false at end of file
    long long rowNumber() const { return rows_; }         // data rows returned so far

    // Safe access: returns "" if col < 0 or the row is shorter than expected.
    static std::string get(const std::vector<std::string>& row, int col);

private:
    int getChar();
    int peekChar();
    bool refill();
    bool readRecord(std::vector<std::string>& out);

    std::ifstream in_;
    std::vector<char> buf_;
    size_t pos_ = 0;
    size_t len_ = 0;
    bool ok_ = false;
    long long rows_ = 0;
    std::vector<std::string> header_;
    std::unordered_map<std::string, int> index_;
};

}  // namespace metro
