# Known gaps in the data (keep this list honest and current)

| Gap | Evidence | Effect | What would fix it |
|---|---|---|---|
| The feed is old (August 2023, calendar ended 2025-12-31) and is a reduced copy of the official feed | Stop-time and trip counts are about 30% of the official portal's; the calendar has expired | Stations and extensions opened since are missing; timetable details are rough | A current official feed |
| **"Yashobhoomi Dwarka Sector - 25" is missing** | DMRC's own Airport Express fare table lists it; the feed has no such stop | Trips to or from it cannot be planned; the fare-table rows that name it are not loaded | A current feed, then add its rows to `config/fare_pairs.csv` |
| Sarojini Nagar has the same coordinates as Bhikaji Cama Place | Both are at 28.570208, 77.187866 in the feed | The two markers overlap on the map; the hop distance was wrong (corrected to 1.4 km from an unofficial source) | Real coordinates in `config/station_overrides.csv` and an official distance in `config/distance_overrides.csv` |
| 16 hops have feed distances far from the straight-line distance | Listed in the import report | Fares for trips crossing them may land in the wrong slab | DMRC's own station-to-station distances |
| Three Rapid Metro hops take exactly 12 minutes | Import report | Rapid Metro trip times look inflated | A better source for Rapid Metro |
| The change time at stations (240 s) is a placeholder | `config/settings.csv` | Every trip with a change is off by an unknown amount | Calibrate against DMRC's journey planner (see `docs/evaluation_sheet.csv`) |
| Aqua Line is treated as one through-running line | Timetable evidence in the feed (110 of 112 arrivals at Sector 142 are followed by a departure within 5 minutes) | If wrong, trips across Sector 142 gain a false change | Check on DMRC/NMRC's own planner |
| No fares for Aqua, Rapid Metro, or trips that mix operators | No rule loaded | These show "fare not computed" | Official fare rules for each |
| Only token (QR single-journey) fares; no smart-card or off-peak fares | Only token tables were supplied | Cheaper fares with a smart card are not shown | The smart-card rules from DMRC's page |
