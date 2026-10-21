// gtfs_inspect: analyse a folder of GTFS .txt files and print a report.
//   gtfs_inspect <gtfs_folder> [--limit N] [--out report.txt]
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../gtfs/inspect.h"

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string dir, outPath;
    size_t limit = 25;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--limit" && i + 1 < args.size()) {
            long v = std::atol(args[++i].c_str());
            if (v > 0) limit = static_cast<size_t>(v);
        } else if (args[i] == "--out" && i + 1 < args.size()) {
            outPath = args[++i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            dir.clear();
            break;
        } else if (dir.empty()) {
            dir = args[i];
        }
    }
    if (dir.empty()) {
        std::cout << "Usage: gtfs_inspect <gtfs_folder> [--limit N] [--out report.txt]\n"
                     "  <gtfs_folder>  folder containing stops.txt, routes.txt, trips.txt, stop_times.txt ...\n"
                     "  --limit N      how many examples to show in each list (default 25)\n"
                     "  --out FILE     also save the report as a UTF-8 text file\n";
        return 2;
    }
    try {
        std::string report = metro::gtfs::analyze(dir, limit, nullptr);
        std::cout << report;
        if (!outPath.empty()) {
            std::ofstream f(outPath, std::ios::binary);
            if (!f) {
                std::cerr << "\nCould not write " << outPath << "\n";
                return 1;
            }
            f << report;
            std::cout << "\nReport saved to " << outPath << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
