/* MetroGo: the page's logic.
 *
 * Safety rules for this file:
 *  - Text from the server (station names, labels, notes...) is only ever put on the page with textContent or
 *    createTextNode, never with innerHTML, so it can never become markup or script.
 *  - Every request that changes something sends the header X-Requested-With: metro-web (the server requires it).
 *  - There are no inline scripts or styles, so the strict Content-Security-Policy holds.
 */
(function () {
  'use strict';

  var TILE_URL = 'https://tile.openstreetmap.org/{z}/{x}/{y}.png';
  var TILE_ATTRIBUTION = '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors';
  var DELHI = [28.6139, 77.209];

  var state = {
    stations: [],
    byId: {},
    lines: [],
    user: null,
    saved: [],
    index: null,
    plan: null,
    chosen: { from: null, to: null },
    map: null,
    layers: { network: null, route: null },
    mapReady: false
  };

  var M = window.MetroGo, S = window.MetroGoSearch;
  var $ = M.$, el = M.el, clear = M.clear, api = M.api, showMessage = M.showMessage, minutesText = M.minutesText, plural = M.plural, lineChip = M.lineChip;

  /** Leaflet treats a plain string given to bindTooltip as HTML, so always hand it a text-only element instead. */
  function tip(text) { return el('span', { text: text }); }

  function stationCoords(id) {
    var s = state.byId[id];
    return s ? [s.lat, s.lon] : null;
  }

  // ------------------------------------------------------------------------------------------------ station pickers

  /** The line colours after a station name (Hauz Khas is on the Yellow and Magenta lines): a glance tells a newcomer where it is. */
  function lineDots(lines) {
    var box = el('span', { 'class': 'dots', 'aria-hidden': 'true' });
    lines.forEach(function (l) {
      var d = el('span', { 'class': 'dot-line', title: l.name });
      if (/^[0-9A-Fa-f]{6}$/.test(l.colour)) d.style.setProperty('--chip', '#' + l.colour);
      box.appendChild(d);
    });
    return box;
  }

  function Picker(input, list) {
    var self = this;
    var items = [];          // the stations that can be chosen, in the order shown (headings are not in here)
    var active = -1;
    this.id = null;
    this.input = input;

    function close() {
      list.hidden = true;
      input.setAttribute('aria-expanded', 'false');
      input.removeAttribute('aria-activedescendant');
      active = -1;
    }

    function setActive(i) {
      active = i;
      Array.prototype.forEach.call(list.querySelectorAll('[role="option"]'), function (li, k) {
        li.setAttribute('aria-selected', k === i ? 'true' : 'false');
        if (k === i) {
          input.setAttribute('aria-activedescendant', li.id);
          if (li.scrollIntoView) li.scrollIntoView({ block: 'nearest' });
        }
      });
    }

    function choose(station) {
      self.id = station.id;
      input.value = station.name;
      close();
    }
    this.choose = choose;

    /** Shows what the search found: name matches, then each matching line's stations, or "did you mean". */
    function render(res) {
      clear(list);
      items = [];
      active = -1;
      function head(text) { list.appendChild(el('li', { role: 'presentation', 'class': 'suggest-head', text: text })); }
      function option(st) {
        var k = items.length;
        items.push(st);
        var names = st.lines.map(function (l) { return l.name; });
        var li = el('li', { role: 'option', id: list.id + '-' + k, 'aria-selected': 'false', 'aria-label': st.name + (names.length ? ' (' + names.join(', ') + ')' : '') },
          el('span', { 'class': 'sname', text: st.name }), lineDots(st.lines));
        li.addEventListener('mousedown', function (e) { e.preventDefault(); choose(st); });
        list.appendChild(li);
      }
      if (res.fuzzyOnly) {
        head('Did you mean');
        res.stations.slice(0, 8).forEach(option);
      } else if (res.stations.length) {
        if (res.stations.length > 8 || res.lines.length) head(res.stations.length + (res.stations.length === 1 ? ' station' : ' stations'));
        res.stations.forEach(option);
      } else if (res.suggestions.length) {
        head('No match. Did you mean');
        res.suggestions.forEach(option);
      }
      res.lines.forEach(function (g) {
        head(g.name + ' stations');
        g.stations.forEach(option);
      });
      if (!items.length) { close(); return; }
      list.hidden = false;
      input.setAttribute('aria-expanded', 'true');
    }

    function search() {
      var q = input.value.trim();
      if (!q || !state.index) { items = []; close(); return; }
      render(S.search(state.index, q, { limit: 80 }));
    }

    input.addEventListener('input', function () { self.id = null; search(); });
    input.addEventListener('focus', function () { if (!self.id && input.value.trim()) search(); });
    input.addEventListener('keydown', function (e) {
      if (list.hidden) return;
      if (e.key === 'ArrowDown') { e.preventDefault(); setActive(Math.min(active + 1, items.length - 1)); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); setActive(Math.max(active - 1, 0)); }
      else if (e.key === 'Enter' && active >= 0) { e.preventDefault(); choose(items[active]); }
      else if (e.key === 'Escape') { close(); }
    });
    input.addEventListener('blur', function () { setTimeout(close, 150); });

    /** If the person typed a name without picking from the list, find the station for it (or say why not). */
    this.resolve = function () {
      if (self.id) return Promise.resolve(self.id);
      var q = input.value.trim();
      if (!q) return Promise.reject(new Error('Enter a station in each box.'));
      if (!state.index) return Promise.reject(new Error('The stations are still loading. Try again in a moment.'));
      var r = S.resolve(state.index, q);
      var quoted = '\u201C' + q + '\u201D';
      if (r.status === 'found') { choose(r.stations[0]); return Promise.resolve(r.stations[0].id); }
      search();   // show what matched, so the person can pick
      if (r.status === 'ambiguous') return Promise.reject(new Error(quoted + ' matches several stations. Pick one from the list.'));
      if (r.status === 'line') return Promise.reject(new Error(quoted + ' is a line, not a station. Pick one of its stations from the list.'));
      if (r.status === 'suggestions') return Promise.reject(new Error('No station matches ' + quoted + '. Did you mean one of the stations in the list?'));
      return Promise.reject(new Error('No station matches ' + quoted + '.'));
    };

    this.set = function (station) { choose({ id: station.id, name: station.name }); };
    this.clear = function () { self.id = null; input.value = ''; close(); };
  }

  // ------------------------------------------------------------------------------------------------ map

  function mapAvailable() { return typeof window.L !== 'undefined' && window.L && typeof window.L.map === 'function'; }

  function initMap() {
    var note = $('map-note');
    if (!mapAvailable()) {
      $('map').hidden = true;
      note.hidden = false;
      note.textContent = 'The map is not installed (see web/vendor/leaflet/README.txt). Everything else works.';
      return;
    }
    var L = window.L;
    state.map = L.map('map', { zoomControl: true }).setView(DELHI, 10);
    L.tileLayer(TILE_URL, { maxZoom: 19, attribution: TILE_ATTRIBUTION }).addTo(state.map);
    state.layers.network = L.layerGroup().addTo(state.map);
    state.layers.route = L.layerGroup().addTo(state.map);
    state.mapReady = true;
  }

  function stationPopup(station) {
    var box = el('div', { 'class': 'popup' }, el('strong', { text: station.name }), el('br'));
    var start = el('button', { type: 'button', 'class': 'small', text: 'Start here' });
    var end = el('button', { type: 'button', 'class': 'small', text: 'End here' });
    start.addEventListener('click', function () { pickers.from.set(station); });
    end.addEventListener('click', function () { pickers.to.set(station); });
    box.appendChild(start);
    box.appendChild(document.createTextNode(' '));
    box.appendChild(end);
    return box;
  }

  function drawNetwork() {
    if (!state.mapReady) return;
    var L = window.L;
    state.layers.network.clearLayers();
    state.lines.forEach(function (line) {
      var pts = line.stations.map(stationCoords).filter(Boolean);
      if (pts.length < 2) return;
      var colour = /^[0-9A-Fa-f]{6}$/.test(line.colour) ? '#' + line.colour : '#666666';
      var poly = L.polyline(pts, { color: colour, weight: 3, opacity: 0.6 });
      poly.bindTooltip(tip(line.name));
      poly.addTo(state.layers.network);
    });
    state.stations.forEach(function (s) {
      var m = L.circleMarker([s.lat, s.lon], { radius: 3, weight: 1, color: '#333333', fillColor: '#ffffff', fillOpacity: 1 });
      m.bindTooltip(tip(s.name));
      m.bindPopup(stationPopup(s));
      m.addTo(state.layers.network);
    });
  }

  function drawRoute(route) {
    if (!state.mapReady) return;
    var L = window.L;
    state.layers.route.clearLayers();
    var all = [];
    route.steps.forEach(function (step) {
      if (step.kind === 'ride') {
        var pts = step.path.map(stationCoords).filter(Boolean);
        var colour = /^[0-9A-Fa-f]{6}$/.test(step.service.colour) ? '#' + step.service.colour : '#222222';
        if (pts.length > 1) L.polyline(pts, { color: colour, weight: 7, opacity: 0.95 }).addTo(state.layers.route);
        all = all.concat(pts);
      } else {
        var fromId = step.kind === 'change' ? step.at.id : step.from.id;
        var a = stationCoords(fromId);
        var b = stationCoords(step.to.id);
        if (a && b && fromId !== step.to.id) {   // a change inside one station has nothing to draw
          L.polyline([a, b], { color: '#444444', weight: 4, dashArray: '2 8' }).addTo(state.layers.route);
          all.push(a, b);
        }
      }
    });
    if (all.length) {
      L.circleMarker(all[0], { radius: 9, color: '#0a7a2f', fillColor: '#2bd45f', fillOpacity: 1, weight: 3 }).bindTooltip(tip('Start')).addTo(state.layers.route);
      L.circleMarker(all[all.length - 1], { radius: 9, color: '#9b1c1c', fillColor: '#ff5a5a', fillOpacity: 1, weight: 3 }).bindTooltip(tip('End')).addTo(state.layers.route);
      state.map.fitBounds(L.latLngBounds(all), { padding: [30, 30] });
    }
  }

  // ------------------------------------------------------------------------------------------------ results

  var NEED = M.NEED, paint = M.paint, mins = M.mins, shortLine = M.shortLine, lineColour = M.lineColour, fareLine = M.fareLine, routeStrip = M.routeStrip, timeline = M.timeline;

  /** Two routes with the same rides over the same stations are the same journey. */
  function routeKey(route) {
    return route.steps.filter(function (s) { return s.kind === 'ride'; }).map(function (s) { return shortLine(s.service) + ':' + s.path.join(','); }).join('|');
  }

  var allCards = [];

  function showOnMap(card, route) {
    drawRoute(route);
    allCards.forEach(function (c) { c.classList.remove('active'); });
    if (state.mapReady) card.classList.add('active');   // the dashed outline marks the route drawn on the map
  }

  // ------------------------------------------------------------------------------------------------ saving

  var MODE_SAVED = { fastest: 'fastest', cheapest: 'cheapest', fewest: 'fewest_changes' };
  var DAY_SAVED = { weekday: 'weekday', sunday: 'sunday_holiday' };
  var PENDING_KEY = 'metrogo.pending';

  /** Is this card's journey already in the person's saved list? */
  function isSaved(card) {
    var s = card._save;
    var from = state.byId[s.data.from.id], to = state.byId[s.data.to.id];
    if (!state.user || !from || !to) return false;
    return state.saved.some(function (j) {
      return j.from.gtfsId === from.gtfsId && j.to.gtfsId === to.gtfsId && j.mode === MODE_SAVED[s.mode] && j.day === DAY_SAVED[s.data.day];
    });
  }

  function paintSaveButton(card) {
    var s = card._save;
    var saved = isSaved(card);
    s.btn.setAttribute('aria-pressed', saved ? 'true' : 'false');
    clear(s.btn);
    s.btn.appendChild(el('span', { 'aria-hidden': 'true', text: saved ? '\u2605' : '\u2606' }));
    s.btn.appendChild(document.createTextNode(saved ? ' Saved' : ' Save'));
    s.btn.title = !state.user ? 'Log in to save this journey' : (saved ? 'Saved. Click to name it.' : 'Save this journey');
  }

  function updateSaveButtons() {
    allCards.forEach(function (c) { if (c._save) paintSaveButton(c); });
  }

  /** Saves (or, if it is already saved, renames) the journey on this card. */
  function saveJourney(card, label) {
    var s = card._save;
    s.btn.disabled = true;
    return api('POST', '/api/journeys', { from: s.data.from.id, to: s.data.to.id, mode: s.mode, day: s.data.day, label: label })
      .then(function (r) {
        clear(s.status);
        s.status.appendChild(document.createTextNode(r.updated ? 'Updated. ' : 'Saved. '));
        s.status.appendChild(el('a', { href: '/journeys.html#' + r.id, text: 'See my journeys' }));
        return refreshSaved().then(function () {
          s.labelRow.hidden = !!label;           // after a quick save, offer to name it; after naming, close the box
          if (!label) s.label.value = '';
        });
      })
      .catch(function (e) { s.status.textContent = e.message; })
      .then(function () { s.btn.disabled = false; });
  }

  /** The person pressed Save while logged out: keep the search so it can be restored after they log in. */
  function rememberPending(data, saveMode) {
    try {
      sessionStorage.setItem(PENDING_KEY, JSON.stringify({ from: data.from.id, to: data.to.id, day: data.day, prefer: data.prefer, save: saveMode }));
    } catch (e) { /* storage may be switched off: the search is simply not restored */ }
  }

  function takePending() {
    try {
      var raw = sessionStorage.getItem(PENDING_KEY);
      sessionStorage.removeItem(PENDING_KEY);
      return raw ? JSON.parse(raw) : null;
    } catch (e) { return null; }
  }

  /** Back from the log in page: show the same search again and, if now logged in, finish the save. */
  function restorePending() {
    var p = takePending();
    if (!p || !state.byId[p.from] || !state.byId[p.to] || !NEED[p.prefer] || !NEED[p.save] || (p.day !== 'weekday' && p.day !== 'sunday')) return Promise.resolve();
    pickers.from.set(state.byId[p.from]);
    pickers.to.set(state.byId[p.to]);
    document.querySelector('input[name="prefer"][value="' + p.prefer + '"]').checked = true;
    document.querySelector('input[name="day"][value="' + p.day + '"]').checked = true;
    return api('GET', '/api/route', { from: p.from, to: p.to, day: p.day, prefer: p.prefer }).then(function (data) {
      renderPlan(data);
      if (!state.user) { showMessage($('message'), 'Log in to save this journey.', true); return null; }
      var card = allCards.filter(function (c) { return c._save && c._save.mode === p.save; })[0];
      return card ? saveJourney(card, '') : null;
    }).catch(function (err) { showMessage($('message'), err.message); });
  }

  /**
   * One option as a card: what it is, the numbers that matter, the journey as a bar, and (collapsed) the details.
   * o = { chosen, index, total, sameAs, saveMode }
   */
  function optionCard(data, group, route, o) {
    var need = NEED[group.criterion];
    var card = el('article', { 'class': 'opt need-' + group.criterion + (o.chosen ? ' chosen' : '') });
    var head = el('div', { 'class': 'opt-head' });
    head.appendChild(el('h3', { 'class': 'opt-title' }, el('span', { 'aria-hidden': 'true', text: need.icon }), ' ' + need.label));
    if (o.chosen && o.index === 0) head.appendChild(el('span', { 'class': 'ribbon', text: 'Your choice' }));
    if (o.total > 1) head.appendChild(el('span', { 'class': 'same', text: 'Route ' + (o.index + 1) + ' of ' + o.total + ', equally good' }));
    if (o.sameAs) head.appendChild(el('span', { 'class': 'same', text: 'Same route as ' + o.sameAs }));
    card.appendChild(head);

    var m = el('div', { 'class': 'metrics' });
    m.appendChild(el('span', { 'class': 'time' }, el('strong', { text: String(route.minutes) }), ' min'));
    m.appendChild(el('span', { 'class': 'pill fare' + (route.fare.available ? '' : ' na'), title: route.fare.available ? '' : route.fare.reason, text: route.fare.available ? '\u20B9' + route.fare.inr : 'Fare n/a' }));
    m.appendChild(el('span', { 'class': 'pill', text: plural(route.changes, 'change', 'changes') }));
    m.appendChild(el('span', { 'class': 'pill', text: plural(route.stops, 'stop', 'stops') }));
    card.appendChild(m);
    card.appendChild(routeStrip(route));
    card.appendChild(el('div', { 'class': 'ends' }, el('span', { text: data.from.name }), el('span', { text: data.to.name })));
    if ((o.chosen && o.index === 0) || group.fellBack) card.appendChild(el('p', { 'class': 'opt-note', text: group.summary }));

    // the details, closed by default
    var more = el('details', { 'class': 'more' }, el('summary', { text: 'Details' }));
    more.appendChild(timeline(route));
    more.appendChild(el('p', { 'class': 'fare-detail', text: fareLine(route.fare) }));
    card.appendChild(more);

    var actions = el('div', { 'class': 'actions' });
    if (state.mapReady) {
      var show = el('button', { type: 'button', 'class': 'small', text: 'Show on map' });
      show.addEventListener('click', function () { showOnMap(card, route); });
      actions.appendChild(show);
    }
    if (o.saveMode) {
      var status = el('span', { 'class': 'hint', role: 'status' });
      var labelRow = el('div', { 'class': 'label-row', hidden: 'hidden' });
      var label = el('input', { type: 'text', maxlength: '60', placeholder: 'Name it (optional)', 'aria-label': 'Name for this saved journey' });
      var confirm = el('button', { type: 'button', 'class': 'small', text: 'Save name' });
      var skip = el('button', { type: 'button', 'class': 'small', text: 'Skip' });
      labelRow.appendChild(label); labelRow.appendChild(confirm); labelRow.appendChild(skip);
      var saveBtn = el('button', { type: 'button', 'class': 'save-btn' });
      head.appendChild(saveBtn);                 // top right of the card, where it is easy to see
      card._save = { btn: saveBtn, mode: o.saveMode, data: data, status: status, labelRow: labelRow, label: label };
      paintSaveButton(card);
      saveBtn.addEventListener('click', function () {
        if (!state.user) {                       // not logged in: go and log in, and come back to finish the save
          rememberPending(data, o.saveMode);
          window.location.assign('/login.html?next=/');
          return;
        }
        if (isSaved(card)) {                     // already saved: the button opens the name box instead
          labelRow.hidden = !labelRow.hidden;
          if (!labelRow.hidden) label.focus();
          return;
        }
        saveJourney(card, '');
      });
      skip.addEventListener('click', function () { labelRow.hidden = true; });
      confirm.addEventListener('click', function () {
        confirm.disabled = true;
        saveJourney(card, label.value.trim()).then(function () { confirm.disabled = false; });
      });
      actions.appendChild(status);
      actions.appendChild(labelRow);
    }
    card.appendChild(actions);
    allCards.push(card);
    card._route = route;
    return card;
  }

  function renderPlan(data) {
    var box = $('results');
    clear(box);
    allCards = [];
    state.plan = data;
    if (!data.found) {
      box.appendChild(el('div', { 'class': 'card' }, el('p', { text: data.message })));
      if (state.mapReady) state.layers.route.clearLayers();
      return;
    }
    box.appendChild(el('h2', { 'class': 'res-title', text: data.from.name + ' \u2192 ' + data.to.name }));

    var seen = {};   // route key -> the option that showed it first, so a repeat can say "same route as ..."
    function sameAs(group, route) {
      var key = routeKey(route);
      if (seen[key]) return seen[key];
      seen[key] = NEED[group.criterion].label;
      return null;
    }

    // your choice first: every route that equally fulfils it
    var primary = data.primary;
    primary.routes.forEach(function (route, i) {
      box.appendChild(optionCard(data, primary, route, { chosen: true, index: i, total: primary.routes.length, sameAs: i === 0 ? sameAs(primary, route) : null, saveMode: i === 0 ? data.prefer : null }));
    });

    // then the best route for each of the other needs, even if it is the same journey
    box.appendChild(el('h2', { 'class': 'alt-head', text: 'Other options' }));
    data.others.forEach(function (group) {
      var top = group.routes[0];
      var holder = el('div', { 'class': 'other' });
      holder.appendChild(optionCard(data, group, top, { chosen: false, index: 0, total: 1, sameAs: sameAs(group, top), saveMode: group.criterion }));
      if (group.routes.length > 1) {
        var extra = group.routes.length - 1;
        var button = el('button', { type: 'button', 'class': 'small more-routes', text: 'Show ' + extra + ' more equally good ' + (extra === 1 ? 'route' : 'routes') });
        button.addEventListener('click', function () {
          group.routes.slice(1).forEach(function (route, i) {
            holder.insertBefore(optionCard(data, group, route, { chosen: false, index: i + 1, total: group.routes.length, sameAs: null, saveMode: null }), button);
          });
          button.remove();
        });
        holder.appendChild(button);
      }
      box.appendChild(holder);
    });

    var about = el('details', { 'class': 'about' }, el('summary', { text: 'About these estimates' }));
    var notes = el('ul');
    data.notes.concat(data.notices).forEach(function (n) { notes.appendChild(el('li', { text: n })); });
    about.appendChild(notes);
    box.appendChild(about);

    showOnMap(allCards[0], primary.routes[0]);
    box.focus();
  }

  // ------------------------------------------------------------------------------------------------ planning

  var pickers = {};

  function selectedDay() {
    var r = document.querySelector('input[name="day"]:checked');
    return r ? r.value : 'weekday';
  }

  function selectedPrefer() {
    var r = document.querySelector('input[name="prefer"]:checked');
    return r ? r.value : 'fastest';
  }

  function onPlan(e) {
    e.preventDefault();
    var message = $('message');
    var button = $('plan-button');
    showMessage(message, '');
    button.disabled = true;
    Promise.all([pickers.from.resolve(), pickers.to.resolve()]).then(function (ids) {
      if (ids[0] === ids[1]) throw new Error('Choose two different stations.');
      return api('GET', '/api/route', { from: ids[0], to: ids[1], day: selectedDay(), prefer: selectedPrefer() });
    }).then(renderPlan).catch(function (err) {
      showMessage(message, err.message);
    }).then(function () { button.disabled = false; });
  }

  function onSwap() {
    var a = { id: pickers.from.id, value: pickers.from.input.value };
    var b = { id: pickers.to.id, value: pickers.to.input.value };
    pickers.from.id = b.id; pickers.from.input.value = b.value;
    pickers.to.id = a.id; pickers.to.input.value = a.value;
  }

  // ------------------------------------------------------------------------------------------------ account

  function applyUser(user) {
    state.user = user;
    $('account-out').hidden = !!user;
    $('account-in').hidden = !user;
    if (user) $('whoami').textContent = user;
    if (!user) state.saved = [];
    updateSaveButtons();
  }

  function refreshMe() {
    return api('GET', '/api/me').then(function (me) { applyUser(me.loggedIn ? me.username : null); if (me.loggedIn) return refreshSaved(); });
  }

  function onLogout() {
    api('POST', '/api/logout').catch(function () {}).then(function () { applyUser(null); });
  }

  // ------------------------------------------------------------------------------------------------ saved journeys
  // The list itself lives on its own page (journeys.html). Here we only need to know WHICH journeys are saved, so the
  // Save buttons can say "Saved".

  function refreshSaved() {
    return api('GET', '/api/journeys').then(function (list) { state.saved = list; updateSaveButtons(); }).catch(function (e) {
      if (e.status === 401) applyUser(null);
      else showMessage($('message'), e.message);
    });
  }

  /** A link such as  /?from=<station id>&to=<station id>&day=weekday&prefer=fastest  (the journeys page uses it for "Plan again"). */
  function applyUrlSearch() {
    var q = new URLSearchParams(window.location.search);
    if (!q.has('from') && !q.has('to')) return Promise.resolve();
    var byGtfs = {};
    state.stations.forEach(function (st) { byGtfs[st.gtfsId] = st; });
    var from = byGtfs[q.get('from')], to = byGtfs[q.get('to')];
    var day = q.get('day') || 'weekday', prefer = q.get('prefer') || 'fastest';
    try { window.history.replaceState(null, '', window.location.pathname); } catch (e) { /* fine */ }
    if (!from || !to || from.id === to.id || (day !== 'weekday' && day !== 'sunday') || !NEED[prefer]) return Promise.resolve();
    pickers.from.set(from);
    pickers.to.set(to);
    document.querySelector('input[name="prefer"][value="' + prefer + '"]').checked = true;
    document.querySelector('input[name="day"][value="' + day + '"]').checked = true;
    return api('GET', '/api/route', { from: from.id, to: to.id, day: day, prefer: prefer }).then(renderPlan).catch(function (err) { showMessage($('message'), err.message); });
  }

  // ------------------------------------------------------------------------------------------------ start

  function formatFeedDate(s) {
    if (!/^\d{8}$/.test(s)) return null;
    return s.slice(0, 4) + '-' + s.slice(4, 6) + '-' + s.slice(6, 8);
  }

  function showStatus(st) {
    var parts = ['Data version ' + st.dataVersion, st.stations + ' stations'];
    var until = formatFeedDate(st.feedValidUntil);
    if (until) {
      parts.push('timetable data valid until ' + until);
      if (new Date(until + 'T23:59:59Z').getTime() < Date.now()) parts.push('this data has expired and may be missing newer stations');
    }
    $('status-line').textContent = parts.join(' \u00B7 ');
  }

  function start() {
    pickers.from = new Picker($('from-input'), $('from-list'));
    pickers.to = new Picker($('to-input'), $('to-list'));
    $('plan-form').addEventListener('submit', onPlan);
    $('swap').addEventListener('click', onSwap);
    $('logout').addEventListener('click', onLogout);

    initMap();
    Promise.all([api('GET', '/api/status'), api('GET', '/api/stations'), api('GET', '/api/lines')]).then(function (r) {
      showStatus(r[0]);
      state.stations = r[1];
      state.stations.forEach(function (s) { state.byId[s.id] = s; });
      state.lines = r[2];
      state.index = S.buildIndex(state.stations, state.lines);
      drawNetwork();
      return refreshMe();
    }).then(restorePending).then(applyUrlSearch).catch(function (err) {
      showMessage($('message'), 'The planner could not load its data: ' + err.message);
    });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
