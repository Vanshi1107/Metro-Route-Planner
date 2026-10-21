/* MetroGo: code shared by the planner (app.js) and the My journeys page (journeys.js).
 *
 * Text from the server is only ever put on the page with textContent or createTextNode, never innerHTML.
 * Every request sends X-Requested-With: metro-web, which the server requires for anything that changes something.
 */
(function () {
  'use strict';

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


  window.MetroGo = {
    $: $, el: el, clear: clear, api: api, showMessage: showMessage, minutesText: minutesText, plural: plural,
    NEED: NEED, textOn: textOn, paint: paint, mins: mins, shortLine: shortLine, lineColour: lineColour,
    lineChip: lineChip, fareLine: fareLine, routeStrip: routeStrip, timeline: timeline
  };
})();
