<p align="center">
  <img src="web/logo.svg" alt="MetroGo logo" width="84">
</p>

<h1 align="center">MetroGo</h1>

<p align="center">
  <b>A Delhi Metro journey planner for people who are new to the metro.</b><br>
  Pick a start, a destination and what matters most. Get the route, where to change, the time and the fare.
</p>

<p align="center">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-486034">
  <img alt="SQLite" src="https://img.shields.io/badge/database-SQLite-486034">
  <img alt="Platform: Windows (tested)" src="https://img.shields.io/badge/platform-Windows%20(tested)-784a1c">
  <img alt="Status: work in progress" src="https://img.shields.io/badge/status-work%20in%20progress-f8ae45">
</p>

<p align="center">
  <a href="#quick-start-windows">Quick start</a> ·
  <a href="#features">Features</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#testing">Testing</a> ·
  <a href="#data-and-accuracy">Data and accuracy</a> ·
  <a href="#documentation">Documentation</a>
</p>

<p align="center">
  <img src="docs/images/planner.png" alt="MetroGo planner: a fastest trip with its step-by-step details, and a cheapest trip where three routes cost the same lowest fare" width="640">
</p>

> **Status: a working course project, not a finished product.** The routing, fares, accounts and web pages work and are
> tested. Three things are **not** yet true, and you should know them before relying on MetroGo:
> 1. **Its answers have not been compared with DMRC's own planner.** The 30-trip comparison sheet is ready
>    ([`docs/evaluation_sheet.csv`](docs/evaluation_sheet.csv)) but DMRC's columns are empty.
> 2. **The data is old.** The DMRC feed is from August 2023 and its calendar ended on 31 December 2025.
> 3. **It has not been tried by first-time riders,** the people it is meant for.
>
> MetroGo is not affiliated with the Delhi Metro Rail Corporation. Times and fares are estimates. Check with DMRC before you travel.

## Why MetroGo

Most metro apps assume you already know what "towards Rithala" or "change at Kashmere Gate" means. MetroGo is built for
someone who has just arrived in Delhi: it answers **every** need side by side (fastest, cheapest, fewest changes), draws the
journey as a bar of coloured line blocks, and spells out the direction and the change in plain steps.

Route, fare and map features already exist in other apps. Whether MetroGo helps newcomers *more* than they do is the open
question of the next phase, and is described honestly in the [Project Overview](docs/MetroGo_Project_Overview.docx).

## Features

- **Three needs, always answered.** Fastest, cheapest and fewest changes are shown together. Your choice comes first; if one
  route wins several needs it is shown under each. If three routes cost the same lowest fare, all three appear.
- **Search that understands lines.** Type `aqua l` and every Aqua Line station is listed in order; type `noida` and all 24
  Noida stations appear; type `dilshaad` and it offers *Dilshad Garden*. Words can come in any order (`142 noida`), nothing is cut
  off, and each station shows the colours of its lines.
- **A drawn journey.** A bar of coloured blocks with change markers, the time, the fare and the number of changes. Step-by-step
  text (with direction and change stations) is one tap away. A map (Leaflet + OpenStreetMap) shows the network and the route.
- **Honest fares.** DMRC distance slabs (weekday and Sunday/holiday), an Airport Express station-pair table, and
  *"fare not computed"* with the reason for anything else. It never guesses.
- **Accounts and saved journeys.** One-tap **Save**, optional names, favourites, **Plan again**, delete. Press Save while logged
  out and you are taken to log in, then returned to the same search with the journey saved.
- **Readable by design.** A palette with measured contrast, an automatic dark theme, a phone-sized layout, keyboard use.
- **Command-line tools** for routes, accounts, and checking the router against an independent solver.

<table>
  <tr>
    <td width="50%" valign="top"><img src="docs/images/search.png" alt="Station search listing the Aqua Line stations and all Noida stations"><br><sub>Search: a line name lists its stations; every match is shown.</sub></td>
    <td width="50%" valign="top"><img src="docs/images/login.png" alt="Create account page with live password rules"><br><sub>Create account: the rules tick off as you type.</sub></td>
  </tr>
  <tr>
    <td valign="top"><img src="docs/images/journeys.png" alt="My journeys page with a favourite and a highlighted journey"><br><sub>My journeys: favourites first; the journey you just saved is highlighted.</sub></td>
    <td valign="top"><img src="docs/images/journeys-dark.png" alt="My journeys in the dark theme"><br><sub>The dark theme follows your system setting.</sub></td>
  </tr>
</table>

## Quick start (Windows)

MetroGo is developed and tested on Windows 11 (Windows 10 has not been tried). Run every command from the project folder, so the
default paths work. Use a folder path **without spaces or non-English characters** (for example `C:\dev\metro-planner`).

### What you need

- Windows 11 with PowerShell
- **Visual Studio 2022 Build Tools** with the *Desktop development with C++* workload (developed with MSVC 19.44)
- **CMake 3.21 or newer** (developed with 4.4.1)
- **Git**
- Optional: **Node.js** (only for `tests\test_search.js`)

### 1. Get the code

```powershell
git clone <this repository's URL> C:\dev\metro-planner
cd C:\dev\metro-planner
```

### 2. Add what the repository does not include

Four things are downloaded once. They are not in the repository on purpose (size, licences, and the data's own terms).

**SQLite** (required). Download the *amalgamation* zip from <https://www.sqlite.org/download.html> (section *Source Code*, named
`sqlite-amalgamation-XXXXXXX.zip`) and copy `sqlite3.c` and `sqlite3.h` into `third_party\sqlite3\`.

**cpp-httplib** (required for the web server). The project was developed against version 0.62.0; newer versions have not been tried.

```powershell
git clone --depth 1 https://github.com/yhirose/cpp-httplib ..\httplib-src
New-Item -ItemType Directory -Force third_party\httplib | Out-Null
Copy-Item ..\httplib-src\httplib.h third_party\httplib\httplib.h
git -C ..\httplib-src rev-parse HEAD      # write the hash down: it records the version you used
```

**Leaflet** (optional: without it everything works except the map).

```powershell
curl.exe -L -o web\vendor\leaflet\leaflet.js  https://unpkg.com/leaflet@1.9.4/dist/leaflet.js
curl.exe -L -o web\vendor\leaflet\leaflet.css https://unpkg.com/leaflet@1.9.4/dist/leaflet.css
```

**The DMRC GTFS feed** (required). The project used `aux/dmrc_gtfs.zip` from <https://github.com/urschrei/vulture> (August 2023).
On Windows a folder named `aux` is a reserved name, so if cloning that repository fails, download the file from GitHub's web page instead.
Put the zip in the project folder, then:

```powershell
Expand-Archive .\dmrc_gtfs.zip -DestinationPath data\raw
Remove-Item data\raw\__MACOSX -Recurse -Force -ErrorAction SilentlyContinue   # a junk folder inside the zip
```

> **Check the data's terms before you publish or redistribute it.** The licence of this feed has not been checked yet.

### 3. Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`metro_web` is built only when `third_party\httplib\httplib.h` is present.

### 4. Create the database and import the feed

```powershell
.\build\Release\metro_db.exe init
.\build\Release\metro_import.exe --gtfs data\raw
```

For the August 2023 feed you should see **262 stations, 13 services, 327 segments and 174 transfers**, followed by a list of
warnings (placeholder run times, an expired calendar). The warnings are expected and are explained in
[`docs/data_gaps.md`](docs/data_gaps.md).

> `metro_db init --force` re-creates the database and **erases accounts and saved journeys**. Copy `data\metro.db` first if you want to keep them.

### 5. Try it

```powershell
.\build\Release\metro_route.exe --from "Dilshad Garden" --to "Hauz Khas"     # 54 min, 1 change, ₹54
.\build\Release\metro_web.exe                                                 # then open http://127.0.0.1:8080/
```

The server listens on this computer only. Press `Ctrl+C` to stop it. After you run `metro_import` again you do not need to
restart it: it notices the new data by itself.

> **Windows Smart App Control** can block freshly built, unsigned programs. If a program will not start, that is the likely cause.

### Other systems

CMake is set up for Linux and macOS as well (`-DMETRO_USE_SYSTEM_SQLITE=ON` uses the system SQLite, and a portable
password-hashing implementation is used off Windows). The automated tests were also compiled and run on Linux with `g++`
directly, but **the CMake route on Linux and macOS has not been tried**, so treat it as untested.

## Using MetroGo

### On the web

| Page | What it does |
|---|---|
| `/` | The planner: stations, what matters most, the day, results, map, **Save** |
| `/login.html` | Log in or create an account (`#register` opens the second tab) |
| `/journeys.html` | **My journeys**: saved journeys as cards; `journeys.html#12` highlights journey 12 |

### On the command line

<details>
<summary><b>Routes</b></summary>

```powershell
.\build\Release\metro_route.exe --from "Dilshad Garden" --to "Hauz Khas"
.\build\Release\metro_route.exe --from "Mayur Vihar Ext" --to "Pandit Shree Ram Sharma" --prefer cheapest
.\build\Release\metro_route.exe --from "Rajiv Chowk" --to "Hauz Khas" --prefer fewest
.\build\Release\metro_route.exe --from "Delhi Aerocity" --to "Noida City Centre" --stops
.\build\Release\metro_route.exe --from "Rajiv Chowk" --to "Hauz Khas" --day sunday
.\build\Release\metro_route.exe --search "rajiv"
.\build\Release\metro_route.exe --verify
```

Station names ignore case and punctuation. If a name is ambiguous or misspelled you get suggestions. Use `id:<number>` to name a
station by its GTFS id. (The command-line search still uses the older whole-phrase matching; only the web page has the new search.)
</details>

<details>
<summary><b>Accounts and saved journeys</b></summary>

```powershell
.\build\Release\metro_user.exe register alice
.\build\Release\metro_user.exe login alice
.\build\Release\metro_user.exe save --from "Dilshad Garden" --to "Hauz Khas" --label "to uni"
.\build\Release\metro_user.exe list
.\build\Release\metro_user.exe open 1
.\build\Release\metro_user.exe favourite 1          # add --off to undo
.\build\Release\metro_user.exe delete 1
.\build\Release\metro_user.exe logout
```

Usernames are 3 to 20 letters or digits; passwords are at least 8 characters. Re-running `metro_import` never touches accounts or
saved journeys; a saved journey is recomputed when opened and says what changed.
</details>

<details>
<summary><b>Server options</b></summary>

```text
metro_web [--db FILE] [--web FOLDER] [--port N] [--bind ADDRESS]
```

Defaults: `data/metro.db`, `web`, port `8080`, and `127.0.0.1` (this computer only). There is **no HTTPS**: see [Security](#security).
</details>

## How it works

<p align="center">
  <img src="docs/images/architecture.png" alt="Architecture: a browser talks to one C++ program, which keeps its logic in libraries and its data in one SQLite file" width="560">
</p>

- **Import (offline).** `metro_import` turns the GTFS feed plus a few hand-maintained CSV files in `config/` into one SQLite
  database, in a single transaction, and raises a data version. The running server notices and reloads.
- **Routing.** Every service at every station has a platform node and one train node per direction, so a train can never turn
  round between stations. Routes are found with **Bellman-Ford**; fastest minimises *(seconds, boardings)*, fewest changes
  *(boardings, seconds)*. A separately written solver checks it on every pair of stations (`metro_route --verify`).
- **Planner.** Different routes are found by banning one line, change station or operator from the best route and searching
  again. All three needs are always answered.
- **Fares.** DMRC lines use the distance slab for the shortest distance between the two stations; Airport Express uses a
  per-pair table; anything else says why no fare is shown.
- **Web layer.** `Api::handle` (in `src/web/api.cpp`) is a plain function from request to response, tested without a network;
  `src/tools/metro_web.cpp` only connects it to cpp-httplib. The pages are plain HTML, CSS and JavaScript with a strict
  content-security policy (no inline code).

Read more: [`docs/algorithms.md`](docs/algorithms.md), [`docs/planner.md`](docs/planner.md) and the
[Technical Overview](docs/MetroGo_Technical_Overview.docx).

### Configuration

Edit these files in `config/`, then run `metro_import` again. Nothing needs recompiling.

| File | Purpose |
|---|---|
| `settings.csv` | Planner numbers and the walking time for a change (240 s, **a placeholder** until calibrated) |
| `fare_slabs.csv` | DMRC distance slabs, each with a source |
| `fare_pairs.csv` | Airport Express station-pair fares |
| `line_map.csv` | Line names and colours (cosmetic; the feed has none) |
| `transfers_seed.csv` | Hand-checked links between nearby stations (only `confirmed = yes` rows are used) |
| `station_overrides.csv` | Corrected coordinates for stations the feed gets wrong |
| `distance_overrides.csv` | Corrected hop distances |

## Testing

```powershell
ctest --test-dir build -C Release --output-on-failure      # 11 C++ test programs
node tests\test_search.js                                   # 101 checks over all 262 real station names (needs Node.js)
.\build\Release\metro_route.exe --verify                    # router vs an independent solver, every pair, both modes
```

What the checks cover: the CSV reader, database rules, the GTFS inspector and importer, the router (hand-built and random
networks), fares, password hashing (published test vectors), accounts and sessions, saved journeys, the whole web layer
(including thousands of random requests, path traversal and injection attempts), and the planner.

> **Browser tests.** The pages were also driven by a real Chromium (38 test steps plus 6 for the map), but those scripts ran in a
> separate environment and are **not in this repository yet**, so you cannot re-run them. Adding them with a Windows-friendly
> runner is on the to-do list. The [Technical Overview](docs/MetroGo_Technical_Overview.docx) explains exactly what was and was not tested.

## Data and accuracy

| Data | Source | Status |
|---|---|---|
| Stops, routes, trips, stop times | DMRC GTFS feed via the [Vulture repository](https://github.com/urschrei/vulture) | August 2023; calendar ended 31 Dec 2025; a reduced copy. **Licence not yet checked.** |
| DMRC fares | DMRC's fare page (screenshots, 21 Oct 2026) | 12 distance slabs, token tickets, checked against the page |
| Airport Express fares | DMRC's pair table (screenshot, 21 Oct 2026) | 15 pairs; one station named in it is missing from the feed |
| Map pictures | [OpenStreetMap](https://www.openstreetmap.org/copyright) tile servers | For light use only; the map needs an internet connection |

Known gaps include a placeholder change time (240 s), one unverified distance correction, no fares for the Aqua Line, Rapid Metro
or trips that mix operators, and no smart-card or off-peak fares. A fare slab covers distances above its minimum up to and including
its maximum (so exactly 2 km is in the 0 to 2 km slab); that is an assumption until checked against DMRC's wording. The full, honest
list is in [`docs/data_gaps.md`](docs/data_gaps.md). Route times come from median timetable values, so they are plausible estimates and
**not** live or current timetable facts; waiting time and first or last trains are not included.

## Security

MetroGo is built to run **on one computer**. Passwords are stored as salted PBKDF2-SHA256 (600,000 rounds), sessions use random tokens
kept only as hashes, cookies are `HttpOnly` and `SameSite=Strict`, changes need a custom header, pages follow a strict
content-security policy, and every input has a limit. But:

- **There is no HTTPS.** Over plain HTTP a password travels readable. Do not expose MetroGo to a network you do not trust, and do not use
  real passwords on a shared network.
- There is no password reset, email check or two-factor login, and the lockout can be abused by someone who knows a username.
- The map uses OpenStreetMap's public tile servers, which tells them each visitor's address.
- No independent security review has been done.

Details and the full list of limits: [`docs/security.md`](docs/security.md).

## Project layout

```text
metro-planner/
├─ CMakeLists.txt
├─ config/        hand-maintained CSV files: fares, line map, settings, corrections
├─ data/          raw/ (put the GTFS .txt files here) and metro.db (created by you; git-ignored)
├─ db/schema.sql  the database schema (version 5)
├─ docs/          notes, design, security, evaluation sheets, the two overview documents, images/
├─ src/
│  ├─ common/     CSV reader, SQLite wrapper, text helpers
│  ├─ gtfs/       GTFS reader and inspector
│  ├─ import/     the importer
│  ├─ route/      network model, router, fares, planner, verifier
│  ├─ auth/       password hashing, accounts, sessions, saved journeys
│  ├─ web/        JSON writer, request and response types, Api::handle
│  └─ tools/      metro_db, gtfs_inspect, metro_import, metro_route, metro_user, metro_web
├─ tests/         11 C++ test programs, test_search.js, test data
├─ third_party/   sqlite3/ and httplib/ (you add these; see Quick start)
└─ web/           index.html, login.html, journeys.html, scripts, style.css, logo.svg, vendor/leaflet/
```

About 7,200 lines of C++ in 45 files, 3,000 lines of C++ tests, and 1,400 lines of JavaScript.

## Documentation

| Document | What is in it |
|---|---|
| [Project Overview](docs/MetroGo_Project_Overview.docx) | Purpose, progress, scalability, future features, risks, references |
| [Technical Overview](docs/MetroGo_Technical_Overview.docx) | Architecture, technology and why, data model, user flow, security, testing |
| [`docs/algorithms.md`](docs/algorithms.md) | Bellman-Ford in this project, with measured cost |
| [`docs/planner.md`](docs/planner.md) | What "fastest", "cheapest" and "fewest changes" mean here |
| [`docs/data_gaps.md`](docs/data_gaps.md) | Known problems in the data |
| [`docs/security.md`](docs/security.md) | What is protected, and what is not |
| [`docs/design.md`](docs/design.md) | The palette and the derived colours |
| [`docs/problem.md`](docs/problem.md) | The problem statement and success measure (**still to be written**) |

## Roadmap

The next phase starts with learning, not building: watch first-time riders, compare with DMRC's planner, refresh the data and check
its licence. Then the most promising feature is an action-by-action **Guided Journey** for newcomers (ticket, entry, platform
direction, change, exit), built only from facts DMRC publishes. After that: timetable-aware planning, complete fares, a Hindi
interface, and HTTPS deployment. The prioritised list, with the evidence each item needs, is in section 7 of the
[Project Overview](docs/MetroGo_Project_Overview.docx).


## License

**No licence has been chosen yet.** Until one is added, the code is not licensed for reuse. Add a `LICENSE` file before accepting
contributions or sharing the project more widely.

Third-party components keep their own licences: SQLite (public domain), [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT),
[Leaflet](https://leafletjs.com/) (BSD-2-Clause), as stated by those projects; check the licence file in your copy. Map data is
© [OpenStreetMap contributors](https://www.openstreetmap.org/copyright) (ODbL). The GTFS feed is under its own terms, which have not
been checked.

## Acknowledgements

The Delhi Metro Rail Corporation for publishing the network and fare information this project relies on; the
[GTFS](https://gtfs.org/documentation/schedule/reference/) specification; the Vulture repository for hosting a copy of the feed;
OpenStreetMap and its contributors; and the authors of SQLite, cpp-httplib and Leaflet.

## Team Members
- Harshita Agrawal
- Vanshika Maheshwari
- Priyanshi Tamta