/* MetroGo: the My journeys page.
 *
 * Each saved journey is a large card drawn the same way as on the planner (a bar of coloured line blocks, the numbers
 * that matter, the details closed). Favourites are emphasised, and the journey in the address (journeys.html#12) is
 * highlighted and scrolled into view. That is where "See my journeys" on the planner sends you right after you save.
 *
 * Text from the server is only put on the page with textContent. Everything that changes something goes through the
 * shared api() helper, which sends the header the server requires.
 */
(function () {
  'use strict';

  var M = window.MetroGo;
  var $ = M.$, el = M.el, clear = M.clear, api = M.api, NEED = M.NEED, mins = M.mins, plural = M.plural, routeStrip = M.routeStrip,
      timeline = M.timeline, fareLine = M.fareLine, showMessage = M.showMessage;

  var NEED_OF = { fastest: 'fastest', cheapest: 'cheapest', fewest_changes: 'fewest' };   // saved mode -> the planner's name for it
  var DAY_TEXT = { weekday: 'Mon\u2013Sat', sunday_holiday: 'Sunday / holiday' };
  var DAY_URL = { weekday: 'weekday', sunday_holiday: 'sunday' };
  var cards = {};   // journey id -> its list item

  function dateText(epoch) {
    var d = new Date(epoch * 1000);
    return isNaN(d.getTime()) ? '' : d.toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' });
  }

  function stationName(side) { return side.name || '(a station that no longer exists)'; }

  /** A link back to the planner that repeats this search. */
  function plannerLink(j) {
    return '/?from=' + encodeURIComponent(j.from.gtfsId) + '&to=' + encodeURIComponent(j.to.gtfsId) + '&day=' + DAY_URL[j.day] + '&prefer=' + NEED_OF[j.mode];
  }

  function chips(j) {
    var need = NEED[NEED_OF[j.mode]];
    var box = el('div', { 'class': 'j-chips' });
    box.appendChild(el('span', { 'class': 'pill need-pill' }, el('span', { 'aria-hidden': 'true', text: need.icon }), ' ' + need.label));
    box.appendChild(el('span', { 'class': 'pill', text: DAY_TEXT[j.day] }));
    box.appendChild(el('span', { 'class': 'pill quiet', text: 'Saved ' + dateText(j.savedAt) }));
    if (!j.current) box.appendChild(el('span', { 'class': 'pill quiet', title: 'Saved before the network data was last updated', text: 'older data' }));
    return box;
  }

  function favouriteButton(j) {
    var b = el('button', { type: 'button', 'class': 'save-btn', 'aria-pressed': j.favourite ? 'true' : 'false', title: j.favourite ? 'Favourite. Click to remove.' : 'Make this a favourite' });
    b.appendChild(el('span', { 'aria-hidden': 'true', text: j.favourite ? '\u2605' : '\u2606' }));
    b.appendChild(document.createTextNode(j.favourite ? ' Favourite' : ' Favourite?'));
    b.addEventListener('click', function () {
      b.disabled = true;
      api('POST', '/api/journeys/' + j.id + '/favourite', { value: j.favourite ? '0' : '1' }).then(load).catch(function (e) { showMessage($('journeys-message'), e.message); b.disabled = false; });
    });
    return b;
  }

  /** The card, with the route still loading. fill() completes it. */
  function skeleton(j) {
    var need = NEED_OF[j.mode];
    var li = el('li', { 'class': 'journey need-' + need + (j.favourite ? ' favourite' : ''), id: 'j-' + j.id, tabindex: '-1' });
    var head = el('div', { 'class': 'j-head' });
    var titleBox = el('div', { 'class': 'j-titlebox' });
    var route = stationName(j.from) + ' \u2192 ' + stationName(j.to);
    if (j.label) {
      titleBox.appendChild(el('h2', { 'class': 'j-title', text: j.label }));
      titleBox.appendChild(el('p', { 'class': 'j-sub', text: route }));
    } else {
      titleBox.appendChild(el('h2', { 'class': 'j-title', text: route }));
    }
    head.appendChild(titleBox);
    head.appendChild(favouriteButton(j));
    li.appendChild(head);
    li.appendChild(chips(j));
    li.appendChild(el('div', { 'class': 'j-body', 'aria-busy': 'true' }, el('p', { 'class': 'hint', text: 'Loading the route\u2026' })));

    var actions = el('div', { 'class': 'j-actions' });
    if (j.from.name && j.to.name) actions.appendChild(el('a', { 'class': 'button small-link', href: plannerLink(j), text: 'Plan again' }));
    var del = el('button', { type: 'button', 'class': 'small', text: 'Delete' });
    del.addEventListener('click', function () {
      if (!window.confirm('Delete this saved journey?')) return;
      del.disabled = true;
      api('DELETE', '/api/journeys/' + j.id).then(load).catch(function (e) { showMessage($('journeys-message'), e.message); del.disabled = false; });
    });
    actions.appendChild(del);
    li.appendChild(actions);
    return li;
  }

  /** Puts the route, the numbers and the details into a card. */
  function fill(li, j, detail) {
    var body = li.querySelector('.j-body');
    clear(body);
    body.removeAttribute('aria-busy');
    if (detail.note) body.appendChild(el('p', { 'class': 'notice', role: 'status', text: detail.note }));
    var route = detail.route;
    if (route) {
      var m = el('div', { 'class': 'metrics' });
      m.appendChild(el('span', { 'class': 'time' }, el('strong', { text: String(route.minutes) }), ' min'));
      m.appendChild(el('span', { 'class': 'pill fare' + (route.fare.available ? '' : ' na'), title: route.fare.available ? '' : route.fare.reason, text: route.fare.available ? '\u20B9' + route.fare.inr : 'Fare n/a' }));
      m.appendChild(el('span', { 'class': 'pill', text: plural(route.changes, 'change', 'changes') }));
      m.appendChild(el('span', { 'class': 'pill', text: plural(route.stops, 'stop', 'stops') }));
      body.appendChild(m);
      body.appendChild(routeStrip(route));
      body.appendChild(el('div', { 'class': 'ends' }, el('span', { text: stationName(j.from) }), el('span', { text: stationName(j.to) })));
      var more = el('details', { 'class': 'more' }, el('summary', { text: 'Details' }));
      more.appendChild(timeline(route));
      more.appendChild(el('p', { 'class': 'fare-detail', text: fareLine(route.fare) }));
      body.appendChild(more);
    } else {
      // a station is gone or no route exists any more: show what was saved
      var d = detail.journey;
      var m2 = el('div', { 'class': 'metrics' });
      m2.appendChild(el('span', { 'class': 'time' }, el('strong', { text: String(d.minutes) }), ' min'));
      m2.appendChild(el('span', { 'class': 'pill', text: plural(d.changes, 'change', 'changes') }));
      body.appendChild(m2);
      body.appendChild(el('pre', { 'class': 'plain', text: detail.text }));
    }
  }

  function failCard(li, err) {
    var body = li.querySelector('.j-body');
    clear(body);
    body.removeAttribute('aria-busy');
    body.appendChild(el('p', { 'class': 'message', text: 'This journey could not be loaded: ' + err.message }));
  }

  // ------------------------------------------------------------------------------------------------ highlight

  /** journeys.html#12 highlights journey 12 and brings it into view. */
  function applyHash() {
    Object.keys(cards).forEach(function (id) {
      cards[id].classList.remove('highlight');
      var old = cards[id].querySelector('.ribbon');
      if (old) old.remove();
    });
    var id = window.location.hash.replace('#', '');
    var li = cards[id];
    if (!li) return;
    li.classList.add('highlight');
    var recent = (Date.now() / 1000 - Number(li.getAttribute('data-saved'))) < 180;
    li.querySelector('.j-head').insertBefore(el('span', { 'class': 'ribbon', text: recent ? 'Saved just now' : 'Selected' }), li.querySelector('.j-head .save-btn'));
    if (li.scrollIntoView) li.scrollIntoView({ block: 'center' });
    li.focus({ preventScroll: true });
  }

  // ------------------------------------------------------------------------------------------------ loading

  var generation = 0;   // each load() is numbered; a newer one cancels the older one's remaining requests

  function load() {
    var list = $('journeys-list');
    var mine = ++generation;
    return api('GET', '/api/journeys').then(function (journeys) {
      if (mine !== generation) return null;
      clear(list);
      cards = {};
      $('journeys-empty').hidden = journeys.length > 0;
      var favourites = journeys.filter(function (j) { return j.favourite; }).length;
      $('journeys-count').textContent = journeys.length ? plural(journeys.length, 'journey', 'journeys') + ' saved' + (favourites ? ' \u00B7 ' + favourites + ' favourite' + (favourites === 1 ? '' : 's') : '') : '';
      journeys.forEach(function (j) {
        var li = skeleton(j);
        li.setAttribute('data-saved', String(j.savedAt));
        cards[j.id] = li;
        list.appendChild(li);
      });
      applyHash();
      // fill the cards one after another, so the first ones appear quickly and the server is never flooded
      return journeys.reduce(function (chain, j) {
        return chain.then(function () {
          if (mine !== generation) return null;          // something newer has taken over (for example a delete): stop asking
          return api('GET', '/api/journeys/' + j.id).then(function (detail) {
            if (mine === generation) fill(cards[j.id], j, detail);
          }).catch(function (e) {
            if (mine !== generation) return;
            if (e.status === 404) cards[j.id].remove();   // deleted meanwhile (say in another tab): it simply disappears
            else failCard(cards[j.id], e);
          });
        });
      }, Promise.resolve());
    }).then(applyHash).catch(function (e) {
      if (e.status === 401) showSignedOut();
      else showMessage($('journeys-message'), e.message);
    });
  }

  function showSignedOut() {
    $('who').hidden = true;
    $('signed-out').hidden = false;
    $('journeys-empty').hidden = true;
    clear($('journeys-list'));
    $('journeys-count').textContent = '';
  }

  function start() {
    $('logout').addEventListener('click', function () {
      api('POST', '/api/logout').catch(function () {}).then(function () { window.location.reload(); });
    });
    window.addEventListener('hashchange', applyHash);
    api('GET', '/api/me').then(function (me) {
      if (!me.loggedIn) { showSignedOut(); return null; }
      $('who').hidden = false;
      $('whoami').textContent = me.username;
      return load();
    }).catch(function (e) { showMessage($('journeys-message'), 'Your journeys could not be loaded: ' + e.message); });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
