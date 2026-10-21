// metro_import: load a GTFS folder into the database.
//   metro_import --gtfs data/raw [--config config] [--db data/metro.db] [--dry-run] [--out report.txt]
//
// --dry-run reads and checks everything and prints the report, but does not touch the database.
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../common/db.h"
#include "../import/importer.h"

namespace {

void usage() {
    std::cout << "Usage: metro_import --gtfs FOLDER [--config FOLDER] [--db FILE] [--dry-run] [--out FILE]\n"
                 "  --gtfs FOLDER   folder containing stops.txt, routes.txt, trips.txt, stop_times.txt\n"
                 "  --config FOLDER folder with line_map.csv, settings.csv, transfers_seed.csv (default: config)\n"
                 "  --db FILE       database made by 'metro_db init' (default: data/metro.db)\n"
                 "  --dry-run       check the feed and print the report; do not change the database\n"
                 "  --out FILE      also save the report as a UTF-8 text file\n"
                 "Run it from the project folder so the default paths work.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string gtfs, config = "config", db = "data/metro.db", out;
    bool dryRun = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        bool hasValue = i + 1 < args.size();
        if (a == "--gtfs" && hasValue) gtfs = args[++i];
        else if (a == "--config" && hasValue) config = args[++i];
        else if (a == "--db" && hasValue) db = args[++i];
        else if (a == "--out" && hasValue) out = args[++i];
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << a << "\n\n";
            usage();
            return 2;
        }
    }
    if (gtfs.empty()) {
        usage();
        return 2;
    }

    try {
        metro::import::Model model = metro::import::buildModel(gtfs, config);
        std::string report = metro::import::describe(model);
        std::cout << report;

        if (!out.empty()) {
            std::ofstream f(out, std::ios::binary);
            if (!f) {
                std::cerr << "\nCould not write " << out << "\n";
                return 1;
            }
            f << report;
            std::cout << "\nReport saved to " << out << "\n";
        }
        if (dryRun) {
            std::cout << "\nDRY RUN: the database was not changed.\n";
            return 0;
        }
        metro::Database database(db, false);
        int version = metro::import::importInto(database, model);
        std::cout << "\nIMPORTED into " << db << ". data_version is now " << version << ".\n";
    } catch (const metro::import::ImportError& e) {
        std::cerr << "\nIMPORT FAILED (the database was not changed):\n  " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "\nERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
