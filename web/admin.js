/* MetroGo: the station admin page. An admin renames a station, or pauses it for a while or until further notice.
 *
 * Who may do this is decided by the SERVER on every request (admin role, this computer only, CSRF header). Hiding things
 * on this page is only a courtesy. All text from the server goes onto the page with textContent.
 */
(function () {
  'use strict';

  var M = window.MetroGo, S = window.MetroGoSearch;
  var $ = M.$, el = M.el, clear = M.clear, api = M.api, showMessage = M.showMessage;

  var state = { stations: [], byId: {}, lines: [], index: null, overview: null, selected: 0, now: 0 };

  // ---------------------------------------------------------------------------------------------- times

  function pad(n) { return (n < 10 ? '0' : '') + n; }

  /** What a datetime-local box wants: "2026-10-24T18:30" in this computer's own time zone. */
  function toInput(epochSeconds) {
    var d = new Date(epochSeconds * 1000);
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + 'T' + pad(d.getHours()) + ':' + pad(d.getMinutes());
  }

  /** The other way: the text of a datetime-local box as seconds since 1970, or '' when empty or not a real time. */
  function fromInput(text) {
    if (!text) return '';
    var ms = new Date(text).getTime();
    return isNaN(ms) ? '' : String(Math.floor(ms / 1000));
  }

  function nowSeconds() { return state.now || Math.floor(Date.now() / 1000); }

  // ---------------------------------------------------------------------------------------------- the gate

  function gate(title, text, actions) {
    $('gate-title').textContent = title;
    $('gate-text').textContent = text;
    var box = $('gate-actions');
    clear(box);
    (actions || []).forEach(function (a) { box.appendChild(el('a', { 'class': 'button primary', href: a.href, text: a.text })); });
    $('admin-gate').hidden = false;
    $('admin-tools').hidden = true;
  }

  // ---------------------------------------------------------------------------------------------- loading

  function loadAll() {
    return Promise.all([api('GET', '/api/stations'), api('GET', '/api/lines'), api('GET', '/api/admin/overview')]).then(function (r) {
      state.stations = r[0];
      state.byId = {};
      state.stations.forEach(function (s) { state.byId[s.id] = s; });
      state.lines = r[1];
      state.index = S.buildIndex(state.stations, state.lines);
      state.overview = r[2];
      state.now = r[2].now;
      renderTables();
      renderCard();
    });
  }

  // ---------------------------------------------------------------------------------------------- choosing a station

  var items = [], active = -1;

  function renderList(res) {
    var list = $('st-list');
    clear(list);
    items = [];
    active = -1;
    var stations = res.stations.slice(0, 12);
    res.lines.forEach(function (g) { stations = stations.concat(g.stations.slice(0, 12)); });
    stations = stations.slice(0, 12);
    stations.forEach(function (st, k) {
      items.push(st);
      var li = el('li', { role: 'option', id: 'st-opt-' + k, 'aria-selected': 'false' }, el('span', { 'class': 'sname', text: st.name }),
        st.closed ? el('span', { 'class': 'tag-closed', text: 'Closed' }) : null);
      li.addEventListener('mousedown', function (e) { e.preventDefault(); choose(st); });
      list.appendChild(li);
    });
    list.hidden = !items.length;
    $('st-input').setAttribute('aria-expanded', items.length ? 'true' : 'false');
  }

  function setActive(i) {
    active = i;
    Array.prototype.forEach.call($('st-list').querySelectorAll('[role="option"]'), function (li, k) {
      li.setAttribute('aria-selected', k === i ? 'true' : 'false');
      if (k === i) $('st-input').setAttribute('aria-activedescendant', li.id);
    });
  }

  function choose(st) {
    state.selected = st.id;
    $('st-input').value = state.byId[st.id] ? state.byId[st.id].name : st.name;
    $('st-list').hidden = true;
    $('st-input').setAttribute('aria-expanded', 'false');
    document.querySelector('input[name="endkind"][value="until"]').checked = true;    // every station starts on the safe choice: a pause with an end time
    $('close-start').value = toInput(nowSeconds());
    $('close-end').value = toInput(nowSeconds() + 2 * 3600);
    $('close-reason').value = '';
    $('rename-name').value = '';
    $('rename-reason').value = '';
    showMessage($('admin-message'), '');
    renderCard();
  }

  // ---------------------------------------------------------------------------------------------- the station card

  function changesFor(st) {
    return (state.overview ? state.overview.changes : []).filter(function (c) { return c.station.id === st.id; });
  }

  function renderCard() {
    var st = state.byId[state.selected];
    $('st-card').hidden = !st;
    if (!st) return;
    var feed = (state.overview.changes.filter(function (c) { return c.station.id === st.id; })[0] || { station: { feedName: st.name } }).station.feedName || st.name;
    var linesText = st.lines.map(function (id) { var l = state.lines.filter(function (x) { return x.id === id; })[0]; return l ? l.name.split(':')[0] : ''; })
      .filter(function (v, i, a) { return v && a.indexOf(v) === i; }).join(', ');
    $('st-title').textContent = st.name;
    $('st-sub').textContent = (feed !== st.name ? 'Name in the DMRC data: ' + feed + '. ' : '') + 'Lines: ' + linesText + '. Station id: ' + st.gtfsId + '.';
    var mine = changesFor(st);
    var activeNow = mine.filter(function (c) { return c.kind === 'closure' && c.status === 'active'; });
    var upcoming = mine.filter(function (c) { return c.kind === 'closure' && c.status === 'upcoming'; });
    var status = $('st-status');
    if (activeNow.length) {
      var c = activeNow[0];
      status.textContent = 'Paused ' + M.closedUntil({ until: c.endAt }) + '. Reason: ' + c.reason + '.';
    } else if (upcoming.length) {
      status.textContent = 'Open now. A pause is planned from ' + M.when(upcoming[0].startAt) + ' ' + M.closedUntil({ until: upcoming[0].endAt }) + '.';
    } else {
      status.textContent = 'Open. Trains stop here as normal.';
    }
    $('st-reopen').hidden = !activeNow.length;
    $('rename-name').setAttribute('placeholder', st.name);
    syncEndField();
  }

  function syncEndField() {
    var forever = document.querySelector('input[name="endkind"]:checked').value === 'forever';
    $('close-end').disabled = forever;
    $('close-end-label').textContent = forever ? 'Until (not needed: until further notice)' : 'Until (date and time)';
  }

  // ---------------------------------------------------------------------------------------------- tables

  function statusPill(status) { return el('span', { 'class': 'st st-' + status, text: status }); }

  function describe(c) {
    if (c.kind === 'rename') return 'Renamed to \u201C' + c.newName + '\u201D';
    return 'Paused from ' + M.when(c.startAt) + ' ' + M.closedUntil({ until: c.endAt });
  }

  function stationText(c) {
    var name = c.station.name || '(station no longer in the network)';
    return c.station.feedName && c.station.feedName !== c.station.name ? name + ' (was ' + c.station.feedName + ')' : name;
  }

  function actionButton(c) {
    var label = c.kind === 'rename' ? 'Undo rename' : (c.status === 'active' ? 'Reopen now' : 'Cancel pause');
    var b = el('button', { type: 'button', 'class': 'small', text: label });
    b.addEventListener('click', function () { cancelChange(c, label); });
    return b;
  }

  function table(headers, rows) {
    var t = el('table', { 'class': 'admin-table' });
    var head = el('tr');
    headers.forEach(function (h) { head.appendChild(el('th', { scope: 'col', text: h })); });
    t.appendChild(el('thead', null, head));
    var body = el('tbody');
    rows.forEach(function (cells) {
      var tr = el('tr');
      cells.forEach(function (cell) { tr.appendChild(el('td', null, typeof cell === 'string' ? document.createTextNode(cell) : cell)); });
      body.appendChild(tr);
    });
    t.appendChild(body);
    return el('div', { 'class': 'table-scroll' }, t);
  }

  function renderTables() {
    var all = state.overview.changes;
    var inForce = all.filter(function (c) { return c.status === 'active' || c.status === 'upcoming' || c.status === 'current'; });
    var box = $('active-box');
    clear(box);
    if (!inForce.length) box.appendChild(el('p', { 'class': 'hint', text: 'Nothing is paused or renamed at the moment.' }));
    else box.appendChild(table(['Station', 'Change', 'Reason', 'By', 'Status', ''], inForce.map(function (c) {
      return [stationText(c), describe(c), c.reason, c.createdBy, statusPill(c.status), actionButton(c)];
    })));

    var hbox = $('history-box');
    clear(hbox);
    if (!all.length) hbox.appendChild(el('p', { 'class': 'hint', text: 'No changes have been made yet.' }));
    else hbox.appendChild(table(['Made', 'Station', 'Change', 'Reason', 'By', 'Status'], all.slice(0, 100).map(function (c) {
      var status = el('span', null, statusPill(c.status), c.cancelledAt ? ' by ' + c.cancelledBy + ', ' + M.when(c.cancelledAt) : '');
      return [M.when(c.createdAt), stationText(c), describe(c), c.reason, c.createdBy, status];
    })));
  }

  // ---------------------------------------------------------------------------------------------- doing things

  function done(res) {
    showMessage($('admin-message'), res.message, true);
    return loadAll();
  }

  function failed(err) { showMessage($('admin-message'), err.message); }

  function cancelChange(c, label) {
    if (!window.confirm(label + ': ' + stationText(c) + '?')) return;
    api('POST', '/api/admin/cancel', { change: c.id }).then(done).catch(failed);
  }

  function onClose(e) {
    e.preventDefault();
    var st = state.byId[state.selected];
    if (!st) return;
    var reason = $('close-reason').value.trim();
    var forever = document.querySelector('input[name="endkind"]:checked').value === 'forever';
    var start = fromInput($('close-start').value), end = forever ? '' : fromInput($('close-end').value);
    if (reason.length < 5) return failed(new Error('Give a reason of at least 5 characters.'));
    if (!start) return failed(new Error('Choose when the pause starts.'));
    if (!forever && !end) return failed(new Error('Choose when the pause ends, or choose \u201CUntil further notice\u201D.'));
    if (!forever && Number(end) <= Number(start)) return failed(new Error('The end must be after the start.'));
    if (!forever && Number(end) <= nowSeconds()) return failed(new Error('The end is already in the past.'));
    var summary = forever ? 'until further notice' : 'until ' + M.when(Number(end));
    if (!window.confirm('Pause ' + st.name + ' ' + summary + '?\n\nTrains will run through without stopping. Passengers will see this reason:\n' + reason)) return;
    var params = { station: st.id, reason: reason, start: start };
    if (!forever) params.end = end;
    var btn = $('close-submit');
    btn.disabled = true;
    api('POST', '/api/admin/close', params).then(done).catch(failed).then(function () { btn.disabled = false; });
  }

  function onRename(e) {
    e.preventDefault();
    var st = state.byId[state.selected];
    if (!st) return;
    var name = $('rename-name').value.trim(), reason = $('rename-reason').value.trim();
    if (name.length < 2) return failed(new Error('The new name must be at least 2 characters.'));
    if (reason.length < 5) return failed(new Error('Give a reason of at least 5 characters.'));
    if (!window.confirm('Rename ' + st.name + ' to \u201C' + name + '\u201D?')) return;
    var btn = $('rename-submit');
    btn.disabled = true;
    api('POST', '/api/admin/rename', { station: st.id, name: name, reason: reason }).then(done).catch(failed).then(function () { btn.disabled = false; });
  }

  function onReopen() {
    var st = state.byId[state.selected];
    if (!st) return;
    var running = changesFor(st).filter(function (c) { return c.kind === 'closure' && c.status === 'active'; });
    if (!running.length || !window.confirm('Reopen ' + st.name + ' now?')) return;
    running.reduce(function (chain, c) {
      return chain.then(function () { return api('POST', '/api/admin/cancel', { change: c.id }); });
    }, Promise.resolve()).then(function () { return done({ message: st.name + ' is open again.' }); }).catch(failed);
  }

  // ---------------------------------------------------------------------------------------------- start

  function start() {
    $('close-submit').disabled = false;       // the buttons start switched off in the HTML; they work only once this script is running
    $('rename-submit').disabled = false;
    $('tz').textContent = (window.Intl && Intl.DateTimeFormat().resolvedOptions().timeZone) || 'your local time';
    $('close-form').addEventListener('submit', onClose);
    $('rename-form').addEventListener('submit', onRename);
    $('reopen-btn').addEventListener('click', onReopen);
    Array.prototype.forEach.call(document.querySelectorAll('input[name="endkind"]'), function (r) { r.addEventListener('change', syncEndField); });
    $('logout').addEventListener('click', function () {
      api('POST', '/api/logout').catch(function () {}).then(function () { window.location.reload(); });
    });

    var input = $('st-input');
    input.addEventListener('input', function () {
      var q = input.value.trim();
      if (!q || !state.index) { renderList({ stations: [], lines: [] }); return; }
      renderList(S.search(state.index, q, { limit: 12 }));
    });
    input.addEventListener('keydown', function (e) {
      if ($('st-list').hidden) return;
      if (e.key === 'ArrowDown') { e.preventDefault(); setActive(Math.min(active + 1, items.length - 1)); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); setActive(Math.max(active - 1, 0)); }
      else if (e.key === 'Enter') { e.preventDefault(); choose(items[active >= 0 ? active : 0]); }
      else if (e.key === 'Escape') { $('st-list').hidden = true; }
    });
    input.addEventListener('blur', function () { setTimeout(function () { $('st-list').hidden = true; }, 150); });

    api('GET', '/api/me').then(function (me) {
      if (!me.loggedIn) {
        gate('Log in to use the admin tools', 'Only an admin account can rename or pause stations.', [{ href: '/login.html?next=/admin.html', text: 'Log in' }]);
        return null;
      }
      $('who').hidden = false;
      $('whoami').textContent = me.username;
      if (me.role !== 'admin') {
        gate('This account is not an admin', 'Admin accounts are made on the computer that runs MetroGo, with:  metro_user make-admin ' + me.username, [{ href: '/', text: 'Back to the planner' }]);
        return null;
      }
      $('admin-tools').hidden = false;
      return loadAll();
    }).catch(function (e) {
      $('admin-tools').hidden = true;
      showMessage($('admin-message'), e.message);
    });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
