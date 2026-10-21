-- MetroGo - database schema, version 5
--
-- Version 5 (journey preferences):
--   * saved_journeys.mode may now also be 'cheapest'
-- Version 4 (web milestone):
--   * new table fare_pairs: operators that publish a fare for each PAIR of stations (the Airport Express)
-- Version 3 (accounts and saved journeys):
--   * users: no role column (there is no admin); created_at is a Unix timestamp
--   * sessions: timestamps are Unix seconds
--   * new table login_attempts: failed-login counting and temporary lockout
--   * saved_journeys: stores the day type, label, snapshot text and timestamps
-- Version 2 (after inspecting the real feed):
--   * "lines" holds SERVICES (a line, or one branch of a line), not raw GTFS routes
--   * line_stops: the ordered stops of each service (gives the direction/terminals)
-- schema_version is set below, so programs can refuse a mismatched file.
-- Foreign keys are enforced: src/common/db.cpp switches them on for every connection.

-- ---------------------------------------------------------------------------
-- Bookkeeping: schema version, which data import is live, where the data came from
-- ---------------------------------------------------------------------------
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT INTO meta(key, value) VALUES ('schema_version', '5');

-- ---------------------------------------------------------------------------
-- NETWORK DATA (rebuilt from the official feed by the importer; never edited by hand)
-- ---------------------------------------------------------------------------

CREATE TABLE stations (
    station_id   INTEGER PRIMARY KEY,
    gtfs_stop_id TEXT    NOT NULL UNIQUE,   -- stable identifier from the feed
    name         TEXT    NOT NULL CHECK (length(trim(name)) > 0),
    lat          REAL    NOT NULL CHECK (lat BETWEEN -90  AND 90),
    lon          REAL    NOT NULL CHECK (lon BETWEEN -180 AND 180)
);

-- A "line" here is a SERVICE: a whole line, or one branch of it. Several GTFS routes
-- (up/down directions, short-turn variants) are merged into one service by the importer.
-- Changing from one service to another counts as a change of train.
CREATE TABLE lines (
    line_id        INTEGER PRIMARY KEY,
    family         TEXT NOT NULL,          -- feed prefix, e.g. 'BLUE'
    name           TEXT NOT NULL CHECK (length(trim(name)) > 0),   -- e.g. 'Blue Line: Dwarka Sector - 21 to Vaishali'
    colour         TEXT,                   -- hex, cosmetic only (the feed has no colours)
    operator       TEXT NOT NULL,          -- fares differ per operator
    gtfs_route_ids TEXT NOT NULL           -- space-separated GTFS route_ids merged into this service
);

-- The ordered stops of a service. The last stop in each direction gives "towards ..." text.
CREATE TABLE line_stops (
    line_id    INTEGER NOT NULL REFERENCES lines(line_id)       ON DELETE CASCADE,
    seq        INTEGER NOT NULL CHECK (seq >= 0),
    station_id INTEGER NOT NULL REFERENCES stations(station_id) ON DELETE CASCADE,
    PRIMARY KEY (line_id, seq)
);
CREATE INDEX idx_line_stops_station ON line_stops(station_id);

-- One hop between two adjacent stations on one service, usable in both directions.
-- from_station comes before to_station in the service's order. distance_m is NULL when the
-- feed has no usable distance (then it must come from another source before fares work).
CREATE TABLE segments (
    segment_id   INTEGER PRIMARY KEY,
    line_id      INTEGER NOT NULL REFERENCES lines(line_id)       ON DELETE CASCADE,
    from_station INTEGER NOT NULL REFERENCES stations(station_id) ON DELETE CASCADE,
    to_station   INTEGER NOT NULL REFERENCES stations(station_id) ON DELETE CASCADE,
    run_seconds  INTEGER NOT NULL CHECK (run_seconds > 0),
    distance_m   INTEGER CHECK (distance_m IS NULL OR distance_m > 0),
    CHECK (from_station <> to_station),
    UNIQUE (line_id, from_station, to_station)
);
CREATE INDEX idx_segments_from ON segments(from_station);
CREATE INDEX idx_segments_to   ON segments(to_station);

-- Walking between two services. from_station = to_station for a change at one station;
-- different stations for a hand-confirmed link between nearby stops (config/transfers_seed.csv).
CREATE TABLE transfers (
    from_station INTEGER NOT NULL REFERENCES stations(station_id) ON DELETE CASCADE,
    from_line    INTEGER NOT NULL REFERENCES lines(line_id)       ON DELETE CASCADE,
    to_station   INTEGER NOT NULL REFERENCES stations(station_id) ON DELETE CASCADE,
    to_line      INTEGER NOT NULL REFERENCES lines(line_id)       ON DELETE CASCADE,
    walk_seconds INTEGER NOT NULL CHECK (walk_seconds >= 0),
    PRIMARY KEY (from_station, from_line, to_station, to_line),
    CHECK (NOT (from_station = to_station AND from_line = to_line))
);

-- Fare slabs are DATA, so a fare revision is a new set of rows, not a code change.
-- Deliberately EMPTY: values must be copied from DMRC's official fare page, with the
-- source and date recorded. [TO VERIFY] whether a slab boundary (e.g. exactly 2 km)
-- belongs to the lower or the higher slab.
CREATE TABLE fare_slabs (
    slab_id    INTEGER PRIMARY KEY,
    operator   TEXT    NOT NULL,
    day_type   TEXT    NOT NULL CHECK (day_type IN ('weekday', 'sunday_holiday')),
    ticket     TEXT    NOT NULL CHECK (ticket   IN ('token', 'smartcard')),
    min_km     REAL    NOT NULL CHECK (min_km >= 0),
    max_km     REAL    NOT NULL,
    fare_inr   INTEGER NOT NULL CHECK (fare_inr >= 0),
    valid_from TEXT    NOT NULL,       -- ISO date the fare took effect
    source     TEXT    NOT NULL,       -- where the number was copied from
    CHECK (max_km > min_km)
);

-- Some operators publish a fare for every PAIR of stations instead of distance slabs. Stations are
-- stored by GTFS stop id with stop_a < stop_b (compared as text), so each pair is stored once.
-- day_type 'any' means the fare is the same every day.
CREATE TABLE fare_pairs (
    pair_id    INTEGER PRIMARY KEY,
    operator   TEXT    NOT NULL,
    day_type   TEXT    NOT NULL CHECK (day_type IN ('any', 'weekday', 'sunday_holiday')),
    ticket     TEXT    NOT NULL CHECK (ticket IN ('token', 'smartcard')),
    stop_a     TEXT    NOT NULL,
    stop_b     TEXT    NOT NULL,
    fare_inr   INTEGER NOT NULL CHECK (fare_inr >= 0),
    valid_from TEXT    NOT NULL,
    source     TEXT    NOT NULL,
    CHECK (stop_a < stop_b),
    UNIQUE (operator, day_type, ticket, stop_a, stop_b)
);

-- ---------------------------------------------------------------------------
-- USER DATA (survives network re-imports)
-- All times are Unix timestamps (whole seconds since 1970, UTC).
-- ---------------------------------------------------------------------------

CREATE TABLE users (
    user_id       INTEGER PRIMARY KEY,
    username      TEXT NOT NULL UNIQUE COLLATE NOCASE
                  CHECK (length(username) BETWEEN 3 AND 20),
    password_hash TEXT NOT NULL,            -- 'pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>'; never a password
    created_at    INTEGER NOT NULL CHECK (typeof(created_at) = 'integer')
);

-- The token itself is never stored, only its SHA-256, so a leaked database cannot be
-- used to hijack sessions.
CREATE TABLE sessions (
    token_hash TEXT PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL CHECK (typeof(created_at) = 'integer'),
    expires_at INTEGER NOT NULL CHECK (typeof(expires_at) = 'integer')
);
CREATE INDEX idx_sessions_expires ON sessions(expires_at);

-- Failed-login counting. Deliberately NOT tied to users(user_id): attempts against names that
-- do not exist are counted too, so lockouts do not reveal which usernames are real.
CREATE TABLE login_attempts (
    username     TEXT PRIMARY KEY COLLATE NOCASE,
    failures     INTEGER NOT NULL CHECK (typeof(failures) = 'integer' AND failures >= 0),
    window_start INTEGER NOT NULL CHECK (typeof(window_start) = 'integer'),   -- first failure of the current window
    locked_until INTEGER NOT NULL DEFAULT 0 CHECK (typeof(locked_until) = 'integer')
);

-- Stations are stored by their GTFS id (text), NOT by a foreign key. That is deliberate:
-- when the network is re-imported and a station disappears, the saved journey must
-- survive so the app can tell the user "this station no longer exists".
-- data_version records which import produced the snapshot; if it differs from the live
-- version, the app recomputes the journey instead of showing a possibly outdated one.
CREATE TABLE saved_journeys (
    journey_id        INTEGER PRIMARY KEY,
    user_id           INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    from_gtfs_stop_id TEXT    NOT NULL,
    to_gtfs_stop_id   TEXT    NOT NULL,
    via_gtfs_stop_id  TEXT,                       -- reserved for the future "via" feature
    mode              TEXT    NOT NULL CHECK (mode IN ('fastest', 'cheapest', 'fewest_changes')),
    day_type          TEXT    NOT NULL CHECK (day_type IN ('weekday', 'sunday_holiday')),
    label             TEXT    NOT NULL DEFAULT '' CHECK (length(label) <= 60),
    data_version      INTEGER NOT NULL,
    total_seconds     INTEGER NOT NULL,
    changes           INTEGER NOT NULL,
    fare_inr          INTEGER,                    -- NULL when no fare could be computed
    result_text       TEXT    NOT NULL,           -- the route exactly as it was shown to the user
    is_favourite      INTEGER NOT NULL DEFAULT 0 CHECK (is_favourite IN (0, 1)),
    created_at        INTEGER NOT NULL CHECK (typeof(created_at) = 'integer'),
    updated_at        INTEGER NOT NULL CHECK (typeof(updated_at) = 'integer'),
    UNIQUE (user_id, from_gtfs_stop_id, to_gtfs_stop_id, mode, day_type)
);
CREATE INDEX idx_journeys_user ON saved_journeys(user_id, updated_at);
