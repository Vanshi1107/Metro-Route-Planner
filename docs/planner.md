# The journey planner: fastest, cheapest or fewest changes

The traveller chooses what matters most. The page always shows an option card for EACH of the three needs. The chosen
need comes first, larger and marked "Your choice", with every route that equally fulfils it. The other two follow
under "Other options", each with its best route. If one route wins several needs it is still shown under each, tagged
"Same route as ..." so nothing looks missing.

## What "fulfils the need" means

| Need | A route fulfils it if... |
|---|---|
| Fastest | it is within `similar_time_seconds` (default 180 s) of the quickest route |
| Fewest changes | it has the same lowest number of changes as the best route |
| Cheapest | its fare equals the lowest fare among routes whose fare can be worked out |

Inside each group routes are ordered quickest first, routes more than `max_slower_percent` (default 25%) slower than the
quickest of the group are dropped (except for "fastest", which has its own time window), and at most
`max_routes_per_group` (default 3) are shown. All three numbers are in `config/settings.csv` (they are design choices,
not measured facts) and are stored in the database by `metro_import`.

Within one need a journey is never shown twice, even if it would reach you through two internal services of the same
line (see routeSignature). Across needs the same journey may appear more than once, on purpose.

## How different routes are found

`Router` finds the single best route for time and for changes. To find genuinely different ones, `Planner` takes each
route it has, bans one part of it (one line, one place where you change, or all lines of one operator), searches again
with that ban (`Router::find(..., Restrictions)`), and keeps every new distinct route. It repeats this once on what it
found. At most 40 routes are collected. Nothing is thrown away at this stage, because the true fewest-changes or cheapest
route must be found even if it is slow.

Two routes are the same journey if they follow the same lines and stations (see `routeSignature`).

## What "cheapest" really means here

- On ordinary DMRC lines the fare depends only on the two stations, not on the route, so every sensible route costs the
  same. Fares differ in practice only when a route uses the Airport Express (its own fare table).
- A route whose fare cannot be worked out (Aqua, Rapid Metro, trips that mix operators) cannot be ranked as cheapest.
- If no route has a computable fare, "cheapest" falls back to the fastest routes and the page says so.

## How a result is drawn

Each option is a card: a big time, the fare, the number of changes and stops, and the journey as a bar of coloured
blocks, one per line, wider for longer rides, with a marker where you change (a dotted marker for a walk between
stations). The step-by-step text and the fare explanation are inside a closed "Details" section. Hovering a block names
the line and the stations.

## Saving

Saving keeps the best route for the need that was chosen (the first one in its group), so a saved journey can be
recomputed exactly later. Each option card can be saved (under that need); extra equally good routes within the chosen
need cannot.

## Known limits

- The search for different routes is a heuristic: it finds a good, varied set, not every possible route.
- The three settings are my defaults. Different values give different (still correct) answers.
- Times are estimates that exclude waiting for trains, and the time to change trains is a placeholder.
