/* Delhi Metro Planner: the page's logic.
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
    plan: null,
    chosen: { from: null, to: null },
    map: null,
    layers: { network: null, route: null },
    mapReady: false
  };

  function $(id) { return document.getElementById(id); }

  // ------------------------------------------------------------------------------------------------ helpers

  /** el('p', {class: 'x'}, 'text', childElement) builds an element safely. */
  function el(tag, attrs) {
    var node = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        if (k === 'text') node.textContent = attrs[k];
        else if (k === 'class') node.className = attrs[k];
        else node.setAttribute(k, attrs[k]);
      });
    }
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      if (c === null || c === undefined) continue;
      node.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
    }
    return node;
  }

  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }

  function api(method, path, params) {
    var opts = { method: method, credentials: 'same-origin', headers: { 'X-Requested-With': 'metro-web' } };
    var url = path;
    var body = params ? new URLSearchParams(params).toString() : '';
    if (method === 'GET') {
      if (body) url += '?' + body;
    } else if (body) {
      opts.headers['Content-Type'] = 'application/x-www-form-urlencoded';
      opts.body = body;
    }
    return fetch(url, opts).then(function (res) {
      return res.json().catch(function () { return null; }).then(function (data) {
        if (!res.ok) {
          var err = new Error((data && data.error) || 'Something went wrong (' + res.status + ').');
          err.status = res.status;
          throw err;
        }
        return data;
      });
    });
  }

  function showMessage(node, text, ok) {
    node.textContent = text;
    node.className = 'message' + (ok ? ' ok' : '');
    node.hidden = !text;
  }

  function minutesText(m) { return m <= 0 ? 'less than a minute' : 'about ' + m + ' min'; }
  function plural(n, one, many) { return n + ' ' + (n === 1 ? one : many); }

  function lineChip(service) {
    // show the line ("Blue Line"), not the long branch name; the branch is still in the hover text and in "towards ..."
    var chip = el('span', { 'class': 'chip', text: service.name.split(':')[0], title: service.name });
    if (service.colour && /^[0-9A-Fa-f]{6}$/.test(service.colour)) paint(chip, '#' + service.colour);
    return chip;
  }

  /** Leaflet treats a plain string given to bindTooltip as HTML, so always hand it a text-only element instead. */
  function tip(text) { return el('span', { text: text }); }

  function stationCoords(id) {
    var s = state.byId[id];
    return s ? [s.lat, s.lon] : null;
  }

  // ------------------------------------------------------------------------------------------------ station pickers

  function Picker(input, list) {
    var self = this;
    var items = [];
    var active = -1;
    var timer = null;
    var seq = 0;
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
      Array.prototype.forEach.call(list.children, function (li, k) {
        li.setAttribute('aria-selected', k === i ? 'true' : 'false');
        if (k === i) input.setAttribute('aria-activedescendant', li.id);
      });
    }

    function choose(item) {
      self.id = item.id;
      input.value = item.name;
      close();
    }
    this.choose = choose;

    function render() {
      clear(list);
      if (!items.length) { close(); return; }
      items.forEach(function (item, k) {
        var li = el('li', { role: 'option', id: list.id + '-' + k, 'aria-selected': 'false', text: item.name });
        li.addEventListener('mousedown', function (e) { e.preventDefault(); choose(item); });
        list.appendChild(li);
      });
      list.hidden = false;
      input.setAttribute('aria-expanded', 'true');
    }

    function search() {
      var q = input.value.trim();
      if (!q) { items = []; close(); return Promise.resolve(); }
      var mine = ++seq;
      return api('GET', '/api/search', { q: q }).then(function (found) {
        if (mine !== seq) return;
        items = found.stations;
        render();
      }).catch(function () { close(); });
    }

    input.addEventListener('input', function () {
      self.id = null;
      clearTimeout(timer);
      timer = setTimeout(search, 120);
    });
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
      return api('GET', '/api/search', { q: q }).then(function (r) {
        if (r.status === 'found') { choose(r.stations[0]); return r.stations[0].id; }
        if (r.status === 'ambiguous') {
          var exact = r.stations.filter(function (f) { return f.name.toLowerCase() === q.toLowerCase(); });
          if (exact.length === 1) { choose(exact[0]); return exact[0].id; }
          items = r.stations;
          render();
          throw new Error('\u201C' + q + '\u201D matches several stations. Pick one from the list.');
        }
        if (r.status === 'suggestions') {
          items = r.stations;
          render();
          throw new Error('No station matches \u201C' + q + '\u201D. Did you mean one of the stations in the list?');
        }
        throw new Error('No station matches \u201C' + q + '\u201D.');
      });
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

  var NEED = {
    fastest: { label: 'Fastest', icon: '\u23F1' },
    cheapest: { label: 'Cheapest', icon: '\u20B9' },
    fewest: { label: 'Fewest changes', icon: '\u21C4' }
  };

  /** Dark text on light colours (like the Yellow Line), white text on dark ones. */
  function textOn(hex) {
    var m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
    if (!m) return '#ffffff';
    var lum = 0.299 * parseInt(m[1], 16) + 0.587 * parseInt(m[2], 16) + 0.114 * parseInt(m[3], 16);
    return lum > 150 ? '#1c2330' : '#ffffff';
  }
  function paint(node, hex) {
    node.style.setProperty('--chip', hex);
    node.style.setProperty('--chip-text', textOn(hex));
    node.style.setProperty('--chip-shadow', textOn(hex) === '#ffffff' ? '0 0 2px rgba(0,0,0,0.55)' : 'none');
  }

  function mins(m) { return m <= 0 ? '<1 min' : m + ' min'; }
  function shortLine(service) { return service.name.split(':')[0]; }
  function lineColour(service, fallback) { return /^[0-9A-Fa-f]{6}$/.test(service.colour) ? '#' + service.colour : fallback; }

  /** Two routes with the same rides over the same stations are the same journey. */
  function routeKey(route) {
    return route.steps.filter(function (s) { return s.kind === 'ride'; }).map(function (s) { return shortLine(s.service) + ':' + s.path.join(','); }).join('|');
  }

  function fareLine(fare) {
    if (!fare.available) return 'Fare not computed: ' + fare.reason + '.';
    var how = fare.basis === 'pair table'
      ? 'from the ' + fare.operator + ' fare table for these two stations'
      : 'based on the shortest ' + fare.operator + ' route between the two stations (' + (fare.distanceM / 1000).toFixed(1) + ' km)';
    return 'Fare \u20B9' + fare.inr + ' (' + fare.ticket + ' ticket), ' + how + (fare.verified ? '' : '. This fare table has not been verified') + '.';
  }

  /** The journey drawn as a bar: one coloured block per line (wider = longer), with a marker where you change. */
  function routeStrip(route) {
    var rides = route.steps.filter(function (s) { return s.kind === 'ride'; });
    var description = rides.map(function (s) { return shortLine(s.service) + ' ' + mins(s.minutes); }).join(', then ');
    var totalRide = rides.reduce(function (sum, r) { return sum + Math.max(r.minutes, 4); }, 0) || 1;
    var strip = el('div', { 'class': 'strip', role: 'img', 'aria-label': description || 'Walk' });
    strip.appendChild(el('span', { 'class': 'dot', 'aria-hidden': 'true' }));
    route.steps.forEach(function (step) {
      if (step.kind === 'ride') {
        var name = shortLine(step.service).replace(' Line', '');
        var share = Math.max(step.minutes, 4) / totalRide;   // how much of the bar this block takes
        var text = share >= 0.3 ? name + ' \u00B7 ' + step.minutes + ' min' : (share >= 0.16 ? name : '');   // narrow blocks carry no text; hover shows it
        var seg = el('span', { 'class': 'seg', title: shortLine(step.service) + ': ' + step.from.name + ' to ' + step.to.name + ' (' + mins(step.minutes) + ')', text: text });
        paint(seg, lineColour(step.service, '#555555'));
        seg.style.flexGrow = String(Math.max(step.minutes, 4));
        strip.appendChild(seg);
      } else {
        var inStation = step.kind === 'change' && step.at.id === step.to.id;
        var where = step.kind === 'change' ? step.at.name : step.from.name;
        strip.appendChild(el('span', { 'class': 'chg' + (inStation ? '' : ' walk'), 'aria-hidden': 'true', title: (inStation ? 'Change at ' + where : 'Walk from ' + where + ' to ' + step.to.name) + ' (' + mins(step.walkMinutes) + ')', text: inStation ? '\u21C4' : '' }));
      }
    });
    strip.appendChild(el('span', { 'class': 'dot', 'aria-hidden': 'true' }));
    return strip;
  }

  /** The step-by-step version, kept out of the way until asked for. */
  function timeline(route) {
    var ol = el('ol', { 'class': 'timeline' });
    route.steps.forEach(function (step) {
      var li;
      if (step.kind === 'ride') {
        li = el('li', { 'class': 't-ride' });
        li.style.setProperty('--chip', lineColour(step.service, '#555555'));
        li.appendChild(el('div', { 'class': 't-line' }, lineChip(step.service), ' towards ' + step.towards.name));
        li.appendChild(el('div', { 'class': 't-path', text: step.from.name + ' \u2192 ' + step.to.name + ' \u00B7 ' + plural(step.hops, 'stop', 'stops') + ' \u00B7 ' + mins(step.minutes) }));
        if (step.via.length) {
          li.appendChild(el('details', null, el('summary', { text: 'Stations passed (' + step.via.length + ')' }),
            el('p', { text: step.via.map(function (v) { return v.name; }).join(', ') })));
        }
      } else if (step.kind === 'change') {
        li = el('li', { 'class': 't-change', text: step.at.id === step.to.id ? '\u21C4 Change at ' + step.at.name + ' \u00B7 walk ' + mins(step.walkMinutes) : 'Walk ' + step.at.name + ' \u2192 ' + step.to.name + ' \u00B7 ' + mins(step.walkMinutes) });
      } else {
        li = el('li', { 'class': 't-walk', text: 'Walk ' + step.from.name + ' \u2192 ' + step.to.name + ' \u00B7 ' + mins(step.walkMinutes) });
      }
      ol.appendChild(li);
    });
    return ol;
  }

  var allCards = [];

  function showOnMap(card, route) {
    drawRoute(route);
    allCards.forEach(function (c) { c.classList.remove('active'); });
    if (state.mapReady) card.classList.add('active');   // the dashed outline marks the route drawn on the map
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
      var label = el('input', { type: 'text', maxlength: '60', placeholder: 'Label (optional)', 'aria-label': 'Label for this saved journey' });
      var confirm = el('button', { type: 'button', 'class': 'small', text: 'Save' });
      var cancel = el('button', { type: 'button', 'class': 'small', text: 'Cancel' });
      labelRow.appendChild(label); labelRow.appendChild(confirm); labelRow.appendChild(cancel);
      var saveBtn = el('button', { type: 'button', 'class': 'small', text: 'Save this journey' });
      saveBtn.addEventListener('click', function () {
        if (!state.user) { status.textContent = 'Log in to save journeys.'; openAuth('login'); return; }
        status.textContent = '';
        labelRow.hidden = false;
        label.focus();
      });
      cancel.addEventListener('click', function () { labelRow.hidden = true; });
      confirm.addEventListener('click', function () {
        confirm.disabled = true;
        api('POST', '/api/journeys', { from: data.from.id, to: data.to.id, mode: o.saveMode, day: data.day, label: label.value.trim() })
          .then(function (r) { labelRow.hidden = true; label.value = ''; status.textContent = r.updated ? 'Updated in your saved journeys.' : 'Saved.'; return refreshSaved(); })
          .catch(function (e) { status.textContent = e.message; })
          .then(function () { confirm.disabled = false; });
      });
      actions.appendChild(saveBtn);
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

  function openAuth(which) {
    $('auth-panel').hidden = false;
    showMessage($('auth-message'), '');
    var field = which === 'register' ? $('reg-user') : $('login-user');
    field.focus();
  }

  function applyUser(user) {
    state.user = user;
    $('account-out').hidden = !!user;
    $('account-in').hidden = !user;
    $('saved-section').hidden = !user;
    if (user) $('whoami').textContent = user;
    if (!user) { clear($('saved-list')); $('detail').hidden = true; }
  }

  function refreshMe() {
    return api('GET', '/api/me').then(function (me) { applyUser(me.loggedIn ? me.username : null); if (me.loggedIn) return refreshSaved(); });
  }

  function onLogin(e) {
    e.preventDefault();
    var msg = $('auth-message');
    api('POST', '/api/login', { username: $('login-user').value, password: $('login-pass').value }).then(function (r) {
      $('login-pass').value = '';
      $('auth-panel').hidden = true;
      applyUser(r.username);
      return refreshSaved();
    }).catch(function (err) { showMessage(msg, err.message); });
  }

  function onRegister(e) {
    e.preventDefault();
    var msg = $('auth-message');
    var user = $('reg-user').value, pass = $('reg-pass').value;
    api('POST', '/api/register', { username: user, password: pass }).then(function () {
      return api('POST', '/api/login', { username: user, password: pass });
    }).then(function (r) {
      $('reg-pass').value = '';
      $('auth-panel').hidden = true;
      applyUser(r.username);
      return refreshSaved();
    }).catch(function (err) { showMessage(msg, err.message); });
  }

  function onLogout() {
    api('POST', '/api/logout').catch(function () {}).then(function () { applyUser(null); });
  }

  // ------------------------------------------------------------------------------------------------ saved journeys

  function savedTitle(j) {
    var from = j.from.name || '(a station that no longer exists)';
    var to = j.to.name || '(a station that no longer exists)';
    return from + ' to ' + to;
  }

  function dateText(epoch) {
    var d = new Date(epoch * 1000);
    return isNaN(d.getTime()) ? '' : d.toLocaleString();
  }

  function renderSaved(list) {
    var ul = $('saved-list');
    clear(ul);
    $('saved-empty').hidden = list.length > 0;
    list.forEach(function (j) {
      var li = el('li');
      li.appendChild(el('div', { 'class': 'title', text: (j.favourite ? '\u2605 ' : '') + (j.label ? j.label + ' \u2013 ' : '') + savedTitle(j) }));
      var meta = (j.mode === 'fastest' ? 'Fastest' : (j.mode === 'cheapest' ? 'Cheapest' : 'Fewest changes')) + ' \u00B7 ' + (j.day === 'weekday' ? 'weekday' : 'Sunday/holiday') + ' \u00B7 ' +
        minutesText(j.minutes) + ' \u00B7 ' + plural(j.changes, 'change', 'changes') + (j.fareInr === null ? '' : ' \u00B7 \u20B9' + j.fareInr) +
        (j.current ? '' : ' \u00B7 saved with older data') + ' \u00B7 ' + dateText(j.savedAt);
      li.appendChild(el('div', { 'class': 'meta', text: meta }));
      var row = el('div', { 'class': 'row' });
      var open = el('button', { type: 'button', 'class': 'small', text: 'Open' });
      var fav = el('button', { type: 'button', 'class': 'small', 'aria-pressed': j.favourite ? 'true' : 'false', text: j.favourite ? 'Favourite' : 'Make favourite' });
      var del = el('button', { type: 'button', 'class': 'small', text: 'Delete' });
      open.addEventListener('click', function () { openSaved(j.id); });
      fav.addEventListener('click', function () {
        api('POST', '/api/journeys/' + j.id + '/favourite', { value: j.favourite ? '0' : '1' }).then(refreshSaved).catch(function (e) { showMessage($('message'), e.message); });
      });
      del.addEventListener('click', function () {
        if (!window.confirm('Delete this saved journey?')) return;
        api('DELETE', '/api/journeys/' + j.id).then(function () { $('detail').hidden = true; return refreshSaved(); }).catch(function (e) { showMessage($('message'), e.message); });
      });
      row.appendChild(open); row.appendChild(fav); row.appendChild(del);
      li.appendChild(row);
      ul.appendChild(li);
    });
  }

  function refreshSaved() {
    return api('GET', '/api/journeys').then(renderSaved).catch(function (e) {
      if (e.status === 401) applyUser(null);
      else showMessage($('message'), e.message);
    });
  }

  function openSaved(id) {
    api('GET', '/api/journeys/' + id).then(function (r) {
      $('detail').hidden = false;
      $('detail-title').textContent = (r.journey.label ? r.journey.label + ' \u2013 ' : '') + savedTitle(r.journey);
      var note = $('detail-note');
      note.textContent = r.note;
      note.hidden = !r.note;
      $('detail-text').textContent = r.text;
      $('detail').scrollIntoView({ block: 'nearest' });
      if (r.status === 'recomputed') refreshSaved();
    }).catch(function (e) { showMessage($('message'), e.message); });
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
    $('login-form').addEventListener('submit', onLogin);
    $('register-form').addEventListener('submit', onRegister);
    $('logout').addEventListener('click', onLogout);
    $('open-login').addEventListener('click', function () { openAuth('login'); });
    $('open-register').addEventListener('click', function () { openAuth('register'); });
    $('close-auth').addEventListener('click', function () { $('auth-panel').hidden = true; });
    $('close-detail').addEventListener('click', function () { $('detail').hidden = true; });

    initMap();
    Promise.all([api('GET', '/api/status'), api('GET', '/api/stations'), api('GET', '/api/lines')]).then(function (r) {
      showStatus(r[0]);
      state.stations = r[1];
      state.stations.forEach(function (s) { state.byId[s.id] = s; });
      state.lines = r[2];
      drawNetwork();
      return refreshMe();
    }).catch(function (err) {
      showMessage($('message'), 'The planner could not load its data: ' + err.message);
    });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
