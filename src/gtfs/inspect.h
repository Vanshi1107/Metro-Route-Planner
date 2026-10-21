// GTFS feed inspector: reads a folder of GTFS .txt files and produces a plain-text report
// answering the questions our database design depends on (how interchanges appear,
// whether distances and line directions exist, how fresh the feed is, data quality).
#pragma once
#include <string>

namespace metro {
namespace gtfs {

struct InspectStats {
    bool foundStops = false;
    long long stopsRows = 0;
    long long routesRows = 0;
    long long tripsRows = 0;
    long long stopTimesRows = 0;

    bool hasHeadsign = false;
    bool hasShapeDistColumn = false;
    long long shapeDistRows = 0;           // stop_times rows with a usable shape_dist_traveled

    long long invalidCoords = 0;
    long long parentStationRows = 0;       // stops that name a parent_station
    long long directedPairs = 0;           // distinct (from stop -> to stop) adjacent pairs
    long long undirectedPairs = 0;         // same, ignoring direction = edges of the graph
    long long nonPositiveRuns = 0;         // hops whose computed run time is <= 0 seconds
    long long variablePairs = 0;           // pairs whose run time varies by more than 60 s

    long long interchangeNameGroups = 0;   // names shared by 2+ different stop_ids
    long long sharedStops = 0;             // single stop_ids served by 2+ routes
    long long unusedStops = 0;             // stops no trip ever calls at
    long long orphanStopTimeRows = 0;      // stop_times rows pointing to unknown stops
    long long orphanTripRows = 0;          // stop_times rows pointing to unknown trips

    std::string calendarMinStart;          // YYYYMMDD, empty if no calendar.txt
    std::string calendarMaxEnd;
};

// Parses GTFS "H:MM:SS" / "HH:MM:SS" (hours may exceed 23). Returns seconds, or -1 if invalid.
int parseGtfsTime(const std::string& s);

// Lower-case, punctuation removed, whitespace collapsed. "  Central,  Hub " -> "central hub".
std::string normaliseName(const std::string& s);

// Great-circle distance in metres.
double haversineMeters(double lat1, double lon1, double lat2, double lon2);

// Analyses the folder and returns the report. 'limit' caps how many examples each list shows.
std::string analyze(const std::string& dir, size_t limit, InspectStats* statsOut);

}  // namespace gtfs
}  // namespace metro
