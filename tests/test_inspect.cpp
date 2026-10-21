#include <cmath>

#include "../src/gtfs/inspect.h"
#include "check.h"

using namespace metro::gtfs;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_inspect <path-to-mini_gtfs>\n";
        return 2;
    }

    // Helpers
    CHECK_EQ(parseGtfsTime("08:05:09"), 8 * 3600 + 5 * 60 + 9);
    CHECK_EQ(parseGtfsTime("8:05:09"), 8 * 3600 + 5 * 60 + 9);
    CHECK_EQ(parseGtfsTime("25:10:00"), 25 * 3600 + 10 * 60);   // GTFS allows hours past midnight
    CHECK_EQ(parseGtfsTime(""), -1);
    CHECK_EQ(parseGtfsTime("08:75:00"), -1);
    CHECK_EQ(parseGtfsTime("garbage"), -1);
    CHECK_EQ(normaliseName("  Central,  Hub "), std::string("central hub"));
    CHECK_EQ(normaliseName("Rajiv-Chowk (Blue)"), std::string("rajiv chowk blue"));
    CHECK(std::fabs(haversineMeters(28.0, 77.0, 28.01, 77.0) - 1112.0) < 5.0);   // 0.01 deg of latitude

    // Whole analysis on the sample feed (BOM, CRLF, quoted comma, orphan rows).
    InspectStats s;
    std::string report = analyze(argv[1], 25, &s);

    CHECK(s.foundStops);
    CHECK_EQ(s.stopsRows, 7LL);
    CHECK_EQ(s.routesRows, 2LL);
    CHECK_EQ(s.tripsRows, 4LL);
    CHECK_EQ(s.stopTimesRows, 14LL);
    CHECK(s.hasHeadsign);
    CHECK(s.hasShapeDistColumn);
    CHECK_EQ(s.shapeDistRows, 3LL);
    CHECK_EQ(s.invalidCoords, 0LL);
    CHECK_EQ(s.orphanStopTimeRows, 1LL);      // stop ZZ does not exist
    CHECK_EQ(s.orphanTripRows, 1LL);          // trip tX does not exist
    CHECK_EQ(s.unusedStops, 1LL);             // X9
    CHECK_EQ(s.directedPairs, 6LL);           // A1>A2, A2>C1, C1>A2, A2>A1, C2>B2, B2>B3
    CHECK_EQ(s.undirectedPairs, 4LL);
    CHECK_EQ(s.nonPositiveRuns, 0LL);
    CHECK_EQ(s.variablePairs, 1LL);           // A1>A2 takes 120 s on one trip and 200 s on another
    CHECK_EQ(s.interchangeNameGroups, 1LL);   // "Central, Hub" and "Central Hub"
    CHECK_EQ(s.sharedStops, 0LL);
    CHECK_EQ(s.calendarMinStart, std::string("20230101"));
    CHECK_EQ(s.calendarMaxEnd, std::string("20231231"));

    CHECK(report.find("Central, Hub") != std::string::npos);        // quoted comma survived
    CHECK(report.find("SEPARATE stops per line") != std::string::npos);
    CHECK(report.find("look like METRES") != std::string::npos);

    // Error handling
    InspectStats bad;
    std::string r2 = analyze("definitely_not_a_folder_metro", 25, &bad);
    CHECK(r2.find("folder not found") != std::string::npos);
    CHECK(!bad.foundStops);

    return finish("test_inspect");
}
