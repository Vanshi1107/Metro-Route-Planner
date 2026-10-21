#include <filesystem>
#include <fstream>

#include "../src/common/csv.h"
#include "check.h"

namespace fs = std::filesystem;

static int run() {
    // Non-throwing filesystem calls: a test must report a problem, never crash.
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec || dir.empty()) dir = fs::current_path();
    fs::path p = dir / "metro_test_csv.txt";
    {
        std::ofstream f(p, std::ios::binary);
        CHECK(static_cast<bool>(f));
        f << "\xEF\xBB\xBF" "id,name,note\r\n"                 // BOM + CRLF
             "1,Plain,hello\r\n"
             "2,\"Comma, Name\",\"say \"\"hi\"\"\"\r\n"        // quoted comma, doubled quotes
             "\r\n"                                             // blank line
             "3,\"Multi\nLine\",x\r\n"                          // newline inside quotes
             "4,Short\r\n"                                      // short row
             "5,Last,end";                                      // no trailing newline
    }

    {   // The reader lives in its own scope so the file is CLOSED before we delete it.
        // (Windows refuses to delete a file that is still open; Linux allows it.)
        metro::CsvReader r(p.string());
        CHECK(r.ok());
        CHECK_EQ(r.header().size(), static_cast<size_t>(3));
        CHECK_EQ(r.header()[0], std::string("id"));            // BOM must not stick to the first name
        CHECK_EQ(r.column("note"), 2);
        CHECK_EQ(r.column("missing"), -1);

        std::vector<std::string> row;
        CHECK(r.next(row));
        CHECK_EQ(metro::CsvReader::get(row, 1), std::string("Plain"));
        CHECK(r.next(row));
        CHECK_EQ(metro::CsvReader::get(row, 1), std::string("Comma, Name"));
        CHECK_EQ(metro::CsvReader::get(row, 2), std::string("say \"hi\""));
        CHECK(r.next(row));                                     // blank line skipped
        CHECK_EQ(metro::CsvReader::get(row, 0), std::string("3"));
        CHECK_EQ(metro::CsvReader::get(row, 1), std::string("Multi\nLine"));
        CHECK(r.next(row));
        CHECK_EQ(metro::CsvReader::get(row, 2), std::string(""));   // missing column is "", not a crash
        CHECK(r.next(row));
        CHECK_EQ(metro::CsvReader::get(row, 2), std::string("end"));
        CHECK(!r.next(row));
        CHECK_EQ(r.rowNumber(), 5LL);
    }

    metro::CsvReader missing((dir / "does_not_exist_metro.txt").string());
    CHECK(!missing.ok());

    fs::remove(p, ec);   // error_code overload: never throws
    return finish("test_csv");
}

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "test_csv: unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
