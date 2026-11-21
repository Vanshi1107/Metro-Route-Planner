# Station admin: pause or rename a station

For the people who run MetroGo when something happens to a station. It changes MetroGo only, not DMRC.

## Using it

1. Set up once (see the README for the exact commands): `metro_db migrate`, then `metro_user make-admin NAME`.
2. Start the server and open **http://127.0.0.1:8080/login.html**. Choose **Admin log in** (under the login form) and log in as that user. You land on **/admin.html**. (Going straight to `/admin.html` also works, and an **Admin** link appears in the planner's header for admins.) There is deliberately **no sign-up for admins**: that view hides the sign-up and explains how the role is given.
3. **Find a station** by name or by line name, with the mouse or the keyboard.
4. Either **pause** it or **rename** it.

### Pausing a station
- Choose **Until a set time** and give a start and an end, each with a **date and a time**, or **Until further notice** (no end).
- Give a **reason** (5 to 200 characters). Passengers see it.
- What "paused" means: trains run through without stopping. A journey can **pass through** the station (stay on the train), but cannot **start**, **end**, **get off** or **change trains** there. A route that runs through it carries a note.
- A pause that starts later begins by itself at its start time, and one with an end time ends by itself. Nobody has to come back to reopen it.
- **Reopen now** (or the button in the table) ends a pause straight away.

### Renaming a station
- Give the new name (2 to 60 characters; not another station's name) and a reason. It is used everywhere at once.
- **Undo rename** (in the table) gives the previous name back. The newest rename wins if there are several.

### What passengers see
- A **Service notices** box on the planner lists what is closed and what closes within a week.
- A **Closed** tag in the station list.
- Planning from or to a closed station says so, with the reason and the end time.
- A note on any route that runs through a closed station.
- Times are shown in each person's own time zone. The admin page shows which time zone it is using.

## How it works

- Changes live in the `station_changes` table (schema version 6): `kind` (`rename` or `closure`), the station's GTFS id, the new name, the reason, `start_at`, `end_at` (empty = until further notice), who made it and when, and `cancelled_at`/`cancelled_by`. Rows are never deleted.
- They are keyed by GTFS id like saved journeys, so a **data refresh does not undo them**. A change for a station that no longer exists stays in the history and does nothing.
- Every change raises `meta.data_version`. The running server notices on its next request and reloads (renames are applied when the network loads). Saved journeys are re-checked when opened.
- **Closures depend on the clock**, so they are worked out for every request (`closuresAt` in `src/route/network.cpp`) and handed to the planner (`Planner::setClosedStations`). The router (`Router::search`) refuses to get off a train at, or change onto a platform at, a closed station, and `Router::find` refuses to start or end there. Staying on a train that runs through is allowed.
- Upgrading: `metro_db migrate` (`src/common/migrate.cpp`) adds `users.role` and the new table to a version 5 database in one all-or-nothing step, keeping accounts and saved journeys.

## The interface (all under `/api/admin/`, admins only, this computer only)

| Method | Path | What it does |
|---|---|---|
| GET | `/api/admin/overview` | The history (newest first, up to 500) with each change's status |
| POST | `/api/admin/rename` | `station`, `name`, `reason` |
| POST | `/api/admin/close` | `station`, `reason`, `start` and `end` as seconds since 1970 (UTC). Leave `end` out for "until further notice" |
| POST | `/api/admin/cancel` | `change`: reopens a station or undoes a rename |

The passenger-facing additions: `GET /api/notices`, a `closed` object on stations in `GET /api/stations`, a `closedPassed` list on each route, a `closed` object when a journey starts or ends at a closed station, and `role` in `GET /api/me`.

Statuses in the history: closures are `active`, `upcoming`, `ended` or `cancelled`; renames are `current`, `superseded` or `cancelled`.

## Where the code is

| What | Where |
|---|---|
| Rules and database writes | `src/admin/station_admin.cpp` |
| Closures and renames when loading | `src/route/network.cpp` (`loadNetwork`, `closuresAt`, `upcomingClosures`) |
| Routing around closures | `src/route/router.cpp`, `src/route/planner.cpp` |
| Admin endpoints and the checks | `src/web/api.cpp` (`Api::handleAdmin`) |
| Roles | `src/auth/accounts.cpp` (`setRole`), `src/tools/metro_user.cpp` |
| Migration | `src/common/migrate.cpp`, `src/tools/metro_db.cpp` |
| The page | `web/admin.html`, `web/admin.js` |
| Tests | `tests/test_admin.cpp` |

## Testing

`tests/test_admin.cpp` (part of `ctest`) covers the migration, roles, every validation rule, how closures start and end by the clock, routing around closed stations, the permission checks, and that changes survive a data refresh. It was also checked by breaking the code on purpose in eight ways to make sure the tests notice. The admin page was driven in a real browser in a separate test environment (not in this repository), including time zones, the passenger view, readable contrast and a phone-sized screen.

## Known limits

- Admins are local accounts and cannot be checked against DMRC staff. There is no second person to approve a change.
- No HTTPS, so the tools answer only requests from the same computer by default.
- The pause affects MetroGo only. Fares are unchanged by a pause.
- A closure is all-or-nothing for the station. There is no partial closure (one platform, one direction, one exit).
- The page does not refresh by itself: a passenger who already has it open sees a closure after reloading.
