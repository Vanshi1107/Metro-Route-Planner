# Bellman-Ford in this project

Every shortest-path search in this project uses the Bellman-Ford algorithm: the route search (fastest and fewest
changes) in `src/route/router.cpp`, and the shortest-distance search behind the fares in `src/route/fare.cpp`.

## The idea

Keep a table "cheapest known cost to reach each node". Then repeat one move, called **relaxation**:
if going through edge (a to b) is cheaper than the best known way to b, update b.
Bellman-Ford relaxes EVERY edge, then does it all again, until a whole pass changes nothing.

Why that is enough: a best path never visits the same node twice, so it has fewer edges than there are nodes. After
pass 1 every best path of one edge is right, after pass 2 every best path of two edges is right, and so on. So after
at most (number of nodes) passes everything is settled. The code relies on this: if it ever needed more passes, that
would mean a negative-cost loop and it stops with an error instead of running forever.

Work: up to (nodes x edges). Bellman-Ford does not need costs to be non-negative; here they all are (run times,
walking times, boardings), so that strength is not used.

## How it is used for routes

Each node is "on the platform of a service at a station" or "on a train of that service moving up or down its line",
so that boarding, staying on, changing and walking are all just edges (see `src/route/router.h`).
The cost has two parts compared in order: fastest compares (seconds, boardings); fewest changes compares
(boardings, seconds). Both parts are non-negative and add up, so Bellman-Ford handles them directly.
One search from an origin gives the best route to every station (`Router::routesFrom`), which is what the checks use.

## How correctness is checked

- `metro_route --verify` compares the router with a SEPARATELY WRITTEN Bellman-Ford solver (in `verify.cpp`; it works
  straight from the database tables with its own model of the network) for every pair of stations in both modes, and
  re-computes every route's time and changes from the tables without using the router.
- The distances behind fares are checked against a different algorithm, Floyd-Warshall, for every pair.
- The tests repeat these checks on hand-built networks and hundreds of random ones.
- During development, before Dijkstra was removed, this router's answers were compared with a separate Dijkstra
  implementation for all 136,764 (pair, mode) combinations on the real network: identical. (That cross-check is not
  part of the shipped tests, which no longer contain Dijkstra.)

## What it cost

Measured on the machine used to build this project, with both algorithms side by side on the real network
(524 searches, one from every station in both modes):

| | time | edge checks |
|---|---|---|
| Dijkstra | 27 ms | 591,258 |
| Bellman-Ford | 84 ms | 17,416,892 |

Bellman-Ford made about 29 times as many edge checks but took only about 3 times as long, since its plain loop is
cache-friendly. A single route search takes a fraction of a millisecond either way. Dijkstra has been removed
from the project at the author's request (the course recommended Bellman-Ford); it could be added back as an option.
