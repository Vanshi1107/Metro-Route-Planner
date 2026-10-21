Leaflet (the map library) goes in this folder. It is not included in the project; download it once:

  curl.exe -L -o web\vendor\leaflet\leaflet.js  https://unpkg.com/leaflet@1.9.4/dist/leaflet.js
  curl.exe -L -o web\vendor\leaflet\leaflet.css https://unpkg.com/leaflet@1.9.4/dist/leaflet.css

Leaflet is BSD-2-Clause licensed (c) Vladimir Agafonkin, Leaflet contributors.
Without these two files the planner still works; only the map is missing.
Map pictures (tiles) are loaded from OpenStreetMap's servers, so the map needs an internet connection.
