# MetroGo - Milestone 13

What exists so far:
- `db/schema.sql`          database schema, version 4
- `metro_db`               creates and verifies the database
- `gtfs_inspect`           reads a GTFS folder and prints a report about it
- `metro_import`           loads a GTFS folder into the database (services, segments, transfers)
- `metro_route`            finds routes and fares for the need you choose: fastest, cheapest or fewest changes
- `metro_user`             accounts, login and saved journeys from the command line
- `metro_web`              the web server and the web pages in `web/`
- `config/`                small files that tell the importer what the feed cannot say (see below)
- `docs/evaluation_sheet.csv`  30 trips to compare against DMRC's own planner
- `tests/`                 automated checks (CSV reader, database rules, inspector, importer, router)

## Build (Windows PowerShell, from this folder)

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

## The web page

One-time setup (the two downloads are not part of the project):

    cd C:\dev
    git clone --depth 1 https://github.com/yhirose/cpp-httplib httplib-src
    New-Item -ItemType Directory -Force C:\dev\metro-planner\third_party\httplib | Out-Null
    Copy-Item C:\dev\httplib-src\httplib.h C:\dev\metro-planner\third_party\httplib\httplib.h
    cd C:\dev\metro-planner
    curl.exe -L -o web\vendor\leaflet\leaflet.js  https://unpkg.com/leaflet@1.9.4/dist/leaflet.js
    curl.exe -L -o web\vendor\leaflet\leaflet.css https://unpkg.com/leaflet@1.9.4/dist/leaflet.css

Then re-run the build (the web server is built only when httplib.h is present), and start it:

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    .\build\Release\metro_web.exe

Open http://127.0.0.1:8080/ in a browser. The server listens on this computer only. Press Ctrl+C to stop.
After running `metro_import` again you do not need to restart it: it notices the new data by itself.
Without the Leaflet files the planner still works; only the map is missing. Map pictures come from OpenStreetMap's
servers, so the map needs an internet connection.

How it is built: everything the page asks for is answered by `src/web/api.cpp`, a plain function from request to
response that is tested without any network (`test_web`). `src/tools/metro_web.cpp` only connects it to the
cpp-httplib library. See docs\security.md for the protections and their limits.

## Finding stations (the start and destination boxes)

The page searches in the browser, word by word, so it is instant and nothing is cut off:
- **A line name lists that line's stations**, in order along the line: "aqua l" shows all 21 Aqua Line stations.
- **Every matching station is shown** (24 for "noida", 36 for "sector"), best match first, with the colours of its lines.
- **Words can come in any order** ("142 noida" finds Noida Sector 142), and the data's "Sec" is understood as "sector".
- **Typing mistakes are forgiven:** "dilshaad" offers Dilshad Garden. A misspelling is only suggested, never silently corrected.
- Typing a line name and pressing Find routes explains that it is a line, and lists its stations to pick from.
It is tested against all 262 real station names (`node tests\test_search.js`, needs Node.js; the other tests do not).
The command-line tool still uses its older whole-phrase matching.

## Log in, create an account, save journeys (on the web page)

- **`/login.html`** is a page of its own, reached from "Log in" and "Create account" in the header. It has two tabs, shows
  the username and password rules as you type, can show or hide the password, asks you to type a new password twice, and
  sends you back to the planner when you are done. The address `login.html#register` opens the "Create account" tab.
- **Save** is a button on every option card (top right). Logged in, one tap saves the journey and the button turns into
  "Saved"; you may then give it a name. Tapping "Saved" again lets you rename it. The card for a journey you already
  saved shows "Saved" when you plan the same trip again (same day, same option).
- **Pressing Save while logged out** takes you to the login page and, once you are in, back to the same search with that
  journey saved.
- **My journeys** (a link in the header) is a page of its own, `/journeys.html`. Each saved journey is a large card drawn like the
  planner's (the time, the fare, the coloured bar of lines, the details closed). Favourites are emphasised and come first.
  Right after you save, "See my journeys" opens the page with that journey **highlighted** and scrolled into view (the address
  `journeys.html#12` highlights journey 12). "Plan again" returns to the planner with the same search, and deleting one turns its
  button back to "Save".
- Read docs\security.md for how the pages are protected and what is not covered (for example: there is no password reset).

## Accounts and saved journeys (command line)

    .\build\Release\metro_user.exe register alice
    .\build\Release\metro_user.exe login alice
    .\build\Release\metro_user.exe save --from "Dilshad Garden" --to "Hauz Khas" --label "to uni"
    .\build\Release\metro_user.exe list
    .\build\Release\metro_user.exe open 1
    .\build\Release\metro_user.exe favourite 1        (add --off to undo)
    .\build\Release\metro_user.exe delete 1
    .\build\Release\metro_user.exe logout

- Usernames: 3 to 20 letters or digits. Passwords: at least 8 characters.
- A saved journey keeps the request and a snapshot of the result. If the network data is re-imported later, opening it
  recomputes it and tells you what changed; if a station has gone, the saved version is shown with a note.
- Re-running `metro_import` never touches accounts or saved journeys.
- Read docs\security.md for what is protected and what is not.

## Config files (edit these, not the code)

- `config/line_map.csv`            feed prefix -> display name, colour, operator; `join_end_to_end` for Aqua
- `config/settings.csv`            the interchange walking time (a PLACEHOLDER until calibrated)
- `config/transfers_seed.csv`      hand-checked links between nearby stations; only confirmed = yes rows are used
- `config/fare_slabs.csv`          fare slabs, one row per distance band, each with a source. Rows whose source starts
                                   with UNVERIFIED have not been checked against the operator's own page.
- `config/fare_pairs.csv`          fares for pairs of stations (the Airport Express publishes it this way)
- `config/station_overrides.csv`   corrected coordinates for stations the feed gets wrong, each with a source
- `config/distance_overrides.csv`  corrections for bad distances in the feed, each with a source

Edit a file, then run metro_import again. Nothing needs recompiling.

## Find a route (always from this folder, so the default paths work)

    .\build\Release\metro_route.exe --from "Dilshad Garden" --to "Hauz Khas"
    .\build\Release\metro_route.exe --from "Mayur Vihar Ext" --to "Pandit Shree Ram Sharma" --prefer cheapest
    .\build\Release\metro_route.exe --from "Rajiv Chowk" --to "Hauz Khas" --prefer fewest
    .\build\Release\metro_route.exe --from "Delhi Aerocity" --to "Noida City Centre" --stops
    .\build\Release\metro_route.exe --search "rajiv"
    .\build\Release\metro_route.exe --from "Rajiv Chowk" --to "Hauz Khas" --day sunday
    .\build\Release\metro_route.exe --verify

Station names ignore case and punctuation. If a name is ambiguous or misspelled you get suggestions.
Use `id:<number>` to name a station by its GTFS id.

## How fares work

- The fare is the slab for the SHORTEST route distance between the two stations, whichever route you take.
- It is computed only when every ride is on one operator that has slabs loaded (today: DMRC lines).
  The Airport Express, Aqua, Rapid Metro and mixed trips say "fare not computed" and why.
- A slab covers distances above its minimum up to and including its maximum (so exactly 2 km is in the 0-2 km slab).
  That is an assumption until checked against DMRC's wording.
- Only token fares for now. Use `--day sunday` for Sunday and national-holiday fares.

## Choosing what matters most

`--prefer fastest|cheapest|fewest` (and the "What matters most?" choice on the web page) decides which option comes
first. Routes that equally fulfil it are all shown (quickest first), and the best route for each of the other two needs
always follows, even when it is the same route. Read docs\planner.md for exactly what that means and for the settings in config\settings.csv.

## How the router works (short version)

- Every service has, at every station it calls at, a "platform" node and two "train" nodes (one per direction).
- Riding costs the hop's run time, plus the 20 s stop time at every station you stay on through.
- Changing service costs walking time. A change is counted when you board a second train, so walking at the
  very start or very end of a trip is never a change.
- Both modes use the Bellman-Ford algorithm (see docs\algorithms.md).
  Fastest minimises (seconds, boardings); fewest changes minimises (boardings, seconds).
- The planner shows the fewest-changes route only when it has strictly fewer changes than the fastest.
- Times are estimates from the feed. Waiting for trains and first/last trains are NOT included.

## Checking the router against the real network

`metro_route --verify` compares the router with a separately written Bellman-Ford solver on EVERY pair of
stations in both modes, and re-computes each route's time and changes straight from the database tables.

## Comparing with DMRC (your success measure)

1. `docs\evaluation_sheet.csv` already holds this planner's answers for 30 trips, including fares. Re-create them any time with:
       .\build\Release\metro_route.exe --batch docs\evaluation_pairs.csv --out data\evaluation_results.csv
2. For each trip, ask DMRC's own journey planner and fill the dmrc_min, dmrc_changes and dmrc_fare_inr columns.
3. Compare. Differences are the real accuracy of this project, so keep them honest in your report.

Use a path without spaces or non-English characters (for example C:\dev\metro-planner).
Before the first build, `third_party\sqlite3\` must contain sqlite3.c and sqlite3.h.
The database format changed (version 5): run `metro_db init --force` and then `metro_import --gtfs data\raw` once.
