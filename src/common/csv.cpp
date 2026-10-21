#include "csv.h"

namespace metro {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

CsvReader::CsvReader(const std::string& path) : in_(path, std::ios::binary), buf_(1 << 16) {
    if (!in_) return;
    // Skip a UTF-8 byte-order mark if the file starts with one (common in Windows exports).
    if (refill() && len_ >= 3 &&
        static_cast<unsigned char>(buf_[0]) == 0xEF &&
        static_cast<unsigned char>(buf_[1]) == 0xBB &&
        static_cast<unsigned char>(buf_[2]) == 0xBF) {
        pos_ = 3;
    }
    std::vector<std::string> h;
    if (!readRecord(h) || h.empty()) return;
    for (size_t i = 0; i < h.size(); ++i) {
        h[i] = trim(h[i]);
        index_.emplace(h[i], static_cast<int>(i));
    }
    header_ = h;
    ok_ = true;
}

bool CsvReader::refill() {
    if (!in_) return false;
    in_.read(buf_.data(), static_cast<std::streamsize>(buf_.size()));
    len_ = static_cast<size_t>(in_.gcount());
    pos_ = 0;
    return len_ > 0;
}

int CsvReader::getChar() {
    if (pos_ >= len_ && !refill()) return -1;
    return static_cast<unsigned char>(buf_[pos_++]);
}

int CsvReader::peekChar() {
    if (pos_ >= len_ && !refill()) return -1;
    return static_cast<unsigned char>(buf_[pos_]);
}

bool CsvReader::readRecord(std::vector<std::string>& out) {
    out.clear();
    std::string field;
    bool inQuotes = false;
    bool anyChar = false;
    int c;
    while ((c = getChar()) != -1) {
        anyChar = true;
        if (inQuotes) {
            if (c == '"') {
                if (peekChar() == '"') {          // doubled quote = literal quote
                    field.push_back('"');
                    getChar();
                } else {
                    inQuotes = false;
                }
            } else {
                field.push_back(static_cast<char>(c));
            }
        } else if (c == '"' && field.empty()) {
            inQuotes = true;
        } else if (c == ',') {
            out.push_back(field);
            field.clear();
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && peekChar() == '\n') getChar();
            if (out.empty() && field.empty()) continue;   // blank line: skip
            out.push_back(field);
            return true;
        } else {
            field.push_back(static_cast<char>(c));
        }
    }
    if (!anyChar || (out.empty() && field.empty())) return false;
    out.push_back(field);
    return true;
}

int CsvReader::column(const std::string& name) const {
    auto it = index_.find(name);
    return it == index_.end() ? -1 : it->second;
}

bool CsvReader::next(std::vector<std::string>& row) {
    if (!ok_) return false;
    if (!readRecord(row)) return false;
    ++rows_;
    return true;
}

std::string CsvReader::get(const std::vector<std::string>& row, int col) {
    if (col < 0 || static_cast<size_t>(col) >= row.size()) return std::string();
    return trim(row[static_cast<size_t>(col)]);
}

}  // namespace metro
