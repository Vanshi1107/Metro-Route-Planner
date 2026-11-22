/* MetroGo: the station admin page.
 *   - a station: pause it (for a while or until further notice) or rename it
 *   - a whole line: suspend it, or publish a delay or an announcement (those two are information only)
 *   - before a pause or a suspension: what it would do to journeys (the impact preview)
 *   - change the end of anything that has one; end it early; undo a rename
 *   - the history: search, filter, download as CSV
 *
 * Who may do this is decided by the SERVER on every request (admin role, this computer only, CSRF header). Hiding things
 * on this page is only a courtesy. All text from the server goes onto the page with textContent.
 */
(function () {
  'use strict';

  var M = window.MetroGo, S = window.MetroGoSearch;
  var $ = M.$, el = M.el, clear = M.clear, api = M.api, showMessage = M.showMessage;

  var state = { stations: [], byId: {}, lines: [], families: [], index: null, overview: null, selected: 0, now: 0, shown: 100 };

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

  /** Checks a start/end pair from two boxes. Returns an error message, or '' if fine. */
  function checkTimes(start, end, forever) {
    if (!start) return 'Choose when it starts.';
    if (!forever && !end) return 'Choose when it ends, or choose \u201CUntil further notice\u201D.';
    if (!forever && Number(end) <= Number(start)) return 'The end must be after the start.';
    if (!forever && Number(end) <= nowSeconds()) return 'The end is already in the past.';
    return '';
  }

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
      state.families = r[2].lines;
      state.now = r[2].now;
      fillLineSelect();
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
      var tag = st.closed ? 'Closed' : (st.noTrains ? 'No trains' : '');
      var li = el('li', { role: 'option', id: 'st-opt-' + k, 'aria-selected': 'false' }, el('span', { 'class': 'sname', text: st.name }),
        tag ? el('span', { 'class': 'tag-closed', text: tag }) : null);
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
    $('close-preview').hidden = true;
    showMessage($('admin-message'), '');
    renderCard();
  }

  // ---------------------------------------------------------------------------------------------- the station card

  function changesFor(st) {
    return (state.overview ? state.overview.changes : []).filter(function (c) { return c.scope === 'station' && c.station.id === st.id; });
  }

  function renderCard() {
    var st = state.byId[state.selected];
    $('st-card').hidden = !st;
    if (!st) return;
    var mine = changesFor(st);
    var feed = (mine[0] || { station: { feedName: st.name } }).station.feedName || st.name;
    var linesText = st.lines.map(function (id) { var l = state.lines.filter(function (x) { return x.id === id; })[0]; return l ? l.name.split(':')[0] : ''; })
      .filter(function (v, i, a) { return v && a.indexOf(v) === i; }).join(', ');
    $('st-title').textContent = st.name;
    $('st-sub').textContent = (feed !== st.name ? 'Name in the DMRC data: ' + feed + '. ' : '') + 'Lines: ' + linesText + '. Station id: ' + st.gtfsId + '.';
    var activeNow = mine.filter(function (c) { return c.kind === 'closure' && c.status === 'active'; });
    var upcoming = mine.filter(function (c) { return c.kind === 'closure' && c.status === 'upcoming'; });
    var status = $('st-status');
    if (activeNow.length) {
      var c = activeNow[0];
      status.textContent = 'Paused ' + M.closedUntil({ until: c.endAt }) + '. Reason: ' + c.reason + '.';
    } else if (st.noTrains) {
      status.textContent = 'Open, but no trains stop here: ' + st.noTrains.lines.join(' and ') + ' suspended ' + M.closedUntil(st.noTrains) + '.';
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

  // ---------------------------------------------------------------------------------------------- the impact preview

  function pairText(p) { return p ? p.from.name + ' \u2192 ' + p.to.name : ''; }

  /** Shows a preview from the server in a box; returns one short sentence for the confirmation question. */
  function renderPreview(box, p) {
    clear(box);
    box.hidden = false;
    if (p.alreadyInForce) {
      box.appendChild(el('p', { text: 'This is already in force, so nothing more would change.' }));
      return 'It is already in force.';
    }
    box.appendChild(el('h4', { text: 'If this started now: ' + p.target }));
    var ul = el('ul');
    function li(text) { ul.appendChild(el('li', { text: text })); }
    if (p.stationsWithoutTrainsCount) {
      var names = p.stationsWithoutTrains.slice(0, 8).join(', ') + (p.stationsWithoutTrainsCount > 8 ? ', and ' + (p.stationsWithoutTrainsCount - 8) + ' more' : '');
      li((p.stationsWithoutTrainsCount === 1 ? '1 station has' : p.stationsWithoutTrainsCount + ' stations have') + ' no trains: ' + names + '.');
    }
    li(p.endpointPairs + ' journeys that start or end there can no longer be made.');
    li(p.lost + ' other journeys would have no route at all' + (p.lostExample ? ' (for example ' + pairText(p.lostExample) + ').' : '.'));
    li(p.slower + ' journeys would be slower' + (p.slower ? ', by about ' + p.averageExtraMinutes + ' min on average and up to ' + p.maxExtraMinutes + ' min (' + pairText(p.maxExample) + ').' : '.'));
    li(p.unaffected + ' journeys are not affected.');
    li(p.savedJourneysAffected + ' saved ' + (p.savedJourneysAffected === 1 ? 'journey is' : 'journeys are') + ' affected.');
    box.appendChild(ul);
    box.appendChild(el('p', { 'class': 'hint', text: 'Fastest journeys between every pair of stations, compared with what is in force now. Worked out in ' + p.milliseconds + ' ms. Nothing has been changed yet.' }));
    return p.lost + ' journeys would have no route, ' + p.slower + ' would be slower, and ' + p.savedJourneysAffected + ' saved journeys are affected.';
  }

  function previewStation(st) {
    return api('GET', '/api/admin/preview', { station: st.id }).then(function (p) { return renderPreview($('close-preview'), p); });
  }
  function previewLine(family) {
    return api('GET', '/api/admin/preview', { line: family }).then(function (p) { return renderPreview($('line-preview'), p); });
  }

  // ---------------------------------------------------------------------------------------------- a whole line

  function lineKind() { return document.querySelector('input[name="linekind"]:checked').value; }

  function fillLineSelect() {
    var sel = $('line-select'), keep = sel.value;
    clear(sel);
    sel.appendChild(el('option', { value: '', text: 'All lines (announcements only)' }));
    state.families.forEach(function (l) { sel.appendChild(el('option', { value: l.family, text: l.name })); });
    sel.value = keep || (state.families[0] ? state.families[0].family : '');
    syncLineForm();
  }

  function syncLineForm() {
    var kind = lineKind(), sel = $('line-select');
    sel.options[0].disabled = kind !== 'announcement';
    if (kind !== 'announcement' && sel.value === '' && state.families[0]) sel.value = state.families[0].family;
    $('minutes-row').hidden = kind !== 'delay';
    $('line-preview-btn').hidden = kind !== 'suspension';
    $('line-reason-label').textContent = kind === 'announcement' ? 'Message (passengers will see this)' : 'Reason (passengers will see this)';
    $('line-submit').textContent = kind === 'suspension' ? 'Suspend line' : 'Publish';
    var forever = document.querySelector('input[name="lineend"]:checked').value === 'forever';
    $('line-end').disabled = forever;
    $('line-end-label').textContent = forever ? 'Until (not needed: until further notice)' : 'Until (date and time)';
    if (kind !== 'suspension') $('line-preview').hidden = true;
  }

  function resetLineForm() {
    $('line-start').value = toInput(nowSeconds());
    $('line-end').value = toInput(nowSeconds() + 2 * 3600);
    $('line-reason').value = '';
    $('line-minutes').value = '';
    document.querySelector('input[name="lineend"][value="until"]').checked = true;
    syncLineForm();
  }

  function onLine(e) {
    e.preventDefault();
    var kind = lineKind(), family = $('line-select').value;
    var reason = $('line-reason').value.trim();
    var forever = document.querySelector('input[name="lineend"]:checked').value === 'forever';
    var start = fromInput($('line-start').value), end = forever ? '' : fromInput($('line-end').value);
    if (kind !== 'announcement' && !family) return failed(new Error('Choose a line.'));
    if (kind === 'delay') {
      var m = Number($('line-minutes').value);
      if (!(m >= 1 && m <= 240 && Math.floor(m) === m)) return failed(new Error('A delay must be a whole number of minutes from 1 to 240.'));
    }
    if (reason.length < 5) return failed(new Error(kind === 'announcement' ? 'Write a message of at least 5 characters.' : 'Give a reason of at least 5 characters.'));
    var timeProblem = checkTimes(start, end, forever);
    if (timeProblem) return failed(new Error(timeProblem));
    var lineName = family ? $('line-select').selectedOptions[0].textContent : 'all lines';
    var summary = forever ? 'until further notice' : 'until ' + M.when(Number(end));
    var params = { kind: kind, line: family, reason: reason, start: start };
    if (!forever) params.end = end;
    if (kind === 'delay') params.minutes = $('line-minutes').value;
    var btn = $('line-submit');
    btn.disabled = true;
    var ask = kind === 'suspension'
      ? previewLine(family).then(function (impact) { return 'Suspend the ' + lineName + ' ' + summary + '?\n\n' + impact + '\n\nPassengers will see this reason:\n' + reason; })
      : Promise.resolve((kind === 'delay' ? 'Publish a delay of ' + params.minutes + ' min on the ' : 'Publish this announcement for ') + lineName + ' ' + summary + '?\n\n' + reason);
    ask.then(function (question) {
      if (!window.confirm(question)) return null;
      return api('POST', '/api/admin/line', params).then(function (res) { resetLineForm(); $('line-preview').hidden = true; return done(res); });
    }).catch(failed).then(function () { btn.disabled = false; });
  }

  // ---------------------------------------------------------------------------------------------- tables

  function statusPill(status) { return el('span', { 'class': 'st st-' + status, text: status }); }

  function targetText(c) {
    if (c.scope === 'line') return c.line ? c.line.name : 'All lines';
    var name = c.station.name || '(station no longer in the network)';
    return c.station.feedName && c.station.feedName !== c.station.name ? name + ' (was ' + c.station.feedName + ')' : name;
  }

  function span(c) { return 'from ' + M.when(c.startAt) + ' ' + M.closedUntil({ until: c.endAt }); }

  function describe(c) {
    if (c.kind === 'rename') return 'Renamed to \u201C' + c.newName + '\u201D';
    if (c.kind === 'closure') return 'Paused ' + span(c);
    if (c.kind === 'suspension') return 'No trains ' + span(c);
    if (c.kind === 'delay') return 'Delayed by about ' + c.minutes + ' min, ' + span(c);
    return 'Announcement, ' + span(c);
  }

  function cancelLabel(c) {
    if (c.kind === 'rename') return 'Undo rename';
    if (c.kind === 'closure') return c.status === 'active' ? 'Reopen now' : 'Cancel pause';
    return c.status === 'active' ? 'End now' : 'Cancel';
  }

  function rowActions(c) {
    var box = el('div', { 'class': 'row-actions' });
    var cancel = el('button', { type: 'button', 'class': 'small', text: cancelLabel(c) });
    cancel.addEventListener('click', function () { cancelChange(c, cancel.textContent); });
    if (c.kind !== 'rename') {
      var change = el('button', { type: 'button', 'class': 'small', text: 'Change end' });
      change.addEventListener('click', function () { openEndEditor(box, c); });
      box.appendChild(change);
    }
    box.appendChild(cancel);
    return box;
  }

  /** A small editor in the table row: a new end time, or until further notice. */
  function openEndEditor(box, c) {
    if (box.querySelector('.end-editor')) return;
    var input = el('input', { type: 'datetime-local', 'aria-label': 'New end for ' + targetText(c) });
    input.value = toInput(c.endAt || nowSeconds() + 2 * 3600);
    var forever = el('input', { type: 'checkbox', id: 'forever-' + c.scope + c.id });
    var foreverLabel = el('label', { 'for': 'forever-' + c.scope + c.id }, forever, ' Until further notice');
    var save = el('button', { type: 'button', 'class': 'small', text: 'Save end' });
    var cancel = el('button', { type: 'button', 'class': 'small', text: 'Cancel' });
    var editor = el('div', { 'class': 'end-editor' }, input, foreverLabel, save, cancel);
    forever.addEventListener('change', function () { input.disabled = forever.checked; });
    cancel.addEventListener('click', function () { box.removeChild(editor); });
    save.addEventListener('click', function () {
      var end = forever.checked ? 'never' : fromInput(input.value);
      if (!end) return failed(new Error('Choose the new end, or tick \u201CUntil further notice\u201D.'));
      if (end !== 'never' && Number(end) <= nowSeconds()) return failed(new Error('The new end is already in the past. To end it now, use ' + cancelLabel(c) + '.'));
      var text = end === 'never' ? 'until further notice' : 'until ' + M.when(Number(end));
      if (!window.confirm('Change the end of this (' + targetText(c) + ') to ' + text + '?')) return;
      api('POST', '/api/admin/extend', { scope: c.scope, change: c.id, end: end }).then(done).catch(failed);
    });
    box.appendChild(editor);
    input.focus();
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
    if (!inForce.length) box.appendChild(el('p', { 'class': 'hint', text: 'Nothing is paused, suspended, announced or renamed at the moment.' }));
    else box.appendChild(table(['Station or line', 'Change', 'Reason', 'By', 'Status', ''], inForce.map(function (c) {
      return [targetText(c), describe(c), c.reason, c.createdBy, statusPill(c.status), rowActions(c)];
    })));
    renderHistory();
  }

  function matchesFilters(c) {
    var text = $('h-text').value.trim().toLowerCase(), kind = $('h-kind').value, status = $('h-status').value;
    if (kind && c.kind !== kind) return false;
    if (status === 'now' && !(c.status === 'active' || c.status === 'upcoming' || c.status === 'current')) return false;
    if (status === 'ended' && !(c.status === 'ended' || c.status === 'superseded')) return false;
    if (status === 'cancelled' && !(c.status === 'cancelled' || c.status === 'replaced')) return false;
    if (text) {
      var hay = [targetText(c), c.reason, c.createdBy, c.cancelledBy || '', c.newName || '', c.kind].join(' ').toLowerCase();
      if (hay.indexOf(text) < 0) return false;
    }
    return true;
  }

  function renderHistory() {
    var all = state.overview.changes, rows = all.filter(matchesFilters);
    var hbox = $('history-box');
    clear(hbox);
    $('h-count').textContent = rows.length === all.length ? all.length + ' changes.' : rows.length + ' of ' + all.length + ' changes match.';
    if (!rows.length) hbox.appendChild(el('p', { 'class': 'hint', text: all.length ? 'Nothing matches.' : 'No changes have been made yet.' }));
    else hbox.appendChild(table(['Made', 'Station or line', 'Change', 'Reason', 'By', 'Status'], rows.slice(0, state.shown).map(function (c) {
      var extra = c.cancelledAt ? ' by ' + c.cancelledBy + ', ' + M.when(c.cancelledAt) : '';
      var status = el('span', null, statusPill(c.status), extra);
      return [M.when(c.createdAt), targetText(c), describe(c), c.reason, c.createdBy, status];
    })));
    $('h-more').hidden = rows.length <= state.shown;
  }

  // ---------------------------------------------------------------------------------------------- doing things

  function done(res) {
    showMessage($('admin-message'), res.message, true);
    return loadAll();
  }

  function failed(err) { showMessage($('admin-message'), err.message); }

  function cancelChange(c, label) {
    if (!window.confirm(label + ': ' + targetText(c) + '?')) return;
    api('POST', '/api/admin/cancel', { scope: c.scope, change: c.id }).then(done).catch(failed);
  }

  function onClose(e) {
    e.preventDefault();
    var st = state.byId[state.selected];
    if (!st) return;
    var reason = $('close-reason').value.trim();
    var forever = document.querySelector('input[name="endkind"]:checked').value === 'forever';
    var start = fromInput($('close-start').value), end = forever ? '' : fromInput($('close-end').value);
    if (reason.length < 5) return failed(new Error('Give a reason of at least 5 characters.'));
    var timeProblem = checkTimes(start, end, forever);
    if (timeProblem) return failed(new Error(timeProblem));
    var summary = forever ? 'until further notice' : 'until ' + M.when(Number(end));
    var params = { station: st.id, reason: reason, start: start };
    if (!forever) params.end = end;
    var btn = $('close-submit');
    btn.disabled = true;
    previewStation(st).then(function (impact) {        // the admin sees what it would do before saying yes
      if (!window.confirm('Pause ' + st.name + ' ' + summary + '?\n\n' + impact + '\n\nTrains will run through without stopping. Passengers will see this reason:\n' + reason)) return null;
      return api('POST', '/api/admin/close', params).then(function (res) { $('close-preview').hidden = true; return done(res); });
    }).catch(failed).then(function () { btn.disabled = false; });
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
      return chain.then(function () { return api('POST', '/api/admin/cancel', { scope: 'station', change: c.id }); });
    }, Promise.resolve()).then(function () { return done({ message: st.name + ' is open again.' }); }).catch(failed);
  }

  // ---------------------------------------------------------------------------------------------- start

  function start() {
    ['close-submit', 'rename-submit', 'line-submit'].forEach(function (id) { $(id).disabled = false; });   // they start switched off in the HTML
    $('tz').textContent = (window.Intl && Intl.DateTimeFormat().resolvedOptions().timeZone) || 'your local time';
    $('close-form').addEventListener('submit', onClose);
    $('rename-form').addEventListener('submit', onRename);
    $('line-form').addEventListener('submit', onLine);
    $('reopen-btn').addEventListener('click', onReopen);
    $('close-preview-btn').addEventListener('click', function () {
      var st = state.byId[state.selected];
      if (st) previewStation(st).catch(failed);
    });
    $('line-preview-btn').addEventListener('click', function () { if ($('line-select').value) previewLine($('line-select').value).catch(failed); });
    Array.prototype.forEach.call(document.querySelectorAll('input[name="endkind"]'), function (r) { r.addEventListener('change', syncEndField); });
    Array.prototype.forEach.call(document.querySelectorAll('input[name="linekind"], input[name="lineend"]'), function (r) { r.addEventListener('change', syncLineForm); });
    $('line-select').addEventListener('change', function () { $('line-preview').hidden = true; });
    ['h-text', 'h-kind', 'h-status'].forEach(function (id) {
      $(id).addEventListener(id === 'h-text' ? 'input' : 'change', function () { state.shown = 100; if (state.overview) renderHistory(); });
    });
    $('h-more').addEventListener('click', function () { state.shown += 100; renderHistory(); });
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
      return loadAll().then(resetLineForm);
    }).catch(function (e) {
      $('admin-tools').hidden = true;
      showMessage($('admin-message'), e.message);
    });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
