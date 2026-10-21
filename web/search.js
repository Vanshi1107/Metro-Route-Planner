/* MetroGo: finding a station while someone types.
 *
 * It works word by word, not as one unbroken phrase, and it never throws results away because a list got long:
 *   - "142 noida" finds "Noida Sector 142"; "sec 14" finds the sector stations numbered 14...; "rajiv c" finds Rajiv Chowk
 *   - results are ranked: exact name, then names that start with what you typed, then words that start with it, then the rest
 *   - a LINE name finds that line's stations, in the order along the line: "aqua l" lists the Aqua Line's stations
 *   - small typing mistakes are tolerated: "dilshaad" finds Dilshad Garden, "kashmir gate" finds Kashmere Gate
 *   - "did you mean" suggestions compare word by word, not the whole text against the whole name
 *
 * It runs in the browser on the station list the page already has, so answers are instant. It has no dependencies and
 * is also loadable by Node, which is how tests/test_search.js checks it against all 262 real station names.
 */
(function (root) {
  'use strict';

  var ALIAS = { sec: 'sector', sect: 'sector' };          // the data itself writes "Noida Sec -18"
  var GENERIC = { line: 1, lines: 1, metro: 1, rail: 1, station: 1, stations: 1 };   // words that name no particular line

  /**
   * "Noida Sec -18" -> ["noida", "sector", "18"]. Case, accents and punctuation do not matter.
   * The data's own abbreviation is expanded in NAMES only. What a person types is left as typed, so "sec" can still
   * start both "sector" and "secretariat".
   */
  function words(text, typed) {
    var t = String(text).toLowerCase();
    if (t.normalize) t = t.normalize('NFD').replace(/[\u0300-\u036f]/g, '');
    t = t.replace(/([a-z])([0-9])/g, '$1 $2').replace(/([0-9])([a-z])/g, '$1 $2');      // "sec14" -> "sec 14"
    var list = t.replace(/[^a-z0-9]+/g, ' ').trim().split(' ').filter(Boolean);
    return typed ? list : list.map(function (w) { return ALIAS[w] || w; });
  }

  function editDistance(a, b) {
    if (a === b) return 0;
    var prev = [], cur = [], i, j;
    for (j = 0; j <= b.length; j++) prev[j] = j;
    for (i = 1; i <= a.length; i++) {
      cur[0] = i;
      for (j = 1; j <= b.length; j++) cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a.charAt(i - 1) === b.charAt(j - 1) ? 0 : 1));
      var swap = prev; prev = cur; cur = swap;
    }
    return prev[b.length];
  }

  function allowedMistakes(len) { return len >= 6 ? 2 : (len >= 4 ? 1 : 0); }

  /** How well one typed word matches one word of a name: 0 = not at all. 'fuzzy' means it needed a spelling correction. */
  function wordScore(token, w, loose) {
    if (w === token) return { s: 100, fuzzy: false };
    if (w.indexOf(token) === 0) return { s: 80 + 10 * token.length / w.length, fuzzy: false };
    if (token.length >= 3 && w.indexOf(token) > 0) return { s: 40, fuzzy: false };
    var allowed = allowedMistakes(token.length);
    if (loose && token.length >= 3) allowed = Math.max(allowed, token.length >= 5 ? 2 : 1);
    if (allowed > 0) {
      var d = Math.min(editDistance(token, w), editDistance(token, w.slice(0, token.length)));   // also forgives a typo in a half-typed word
      if (d <= allowed) return { s: 30 - 6 * d, fuzzy: true };
    }
    return { s: 0, fuzzy: false };
  }

  /** Score of a whole station against all typed words (0 = every word must match something). */
  function scoreStation(st, tokens, loose) {
    var total = 0, fuzzy = false, i, j;
    for (i = 0; i < tokens.length; i++) {
      var best = { s: 0, fuzzy: false };
      for (j = 0; j < st.words.length; j++) {
        var r = wordScore(tokens[i], st.words[j], loose);
        if (r.s > best.s) best = r;
      }
      if (best.s === 0) return { s: 0, fuzzy: false };
      total += best.s;
      fuzzy = fuzzy || best.fuzzy;
    }
    var key = tokens.join(' ');
    var score = total / tokens.length;
    if (st.key === key) score += 200;                                    // the exact name
    else if (st.key.indexOf(key) === 0) score += 60;                     // the name starts with what was typed
    else if (st.words[0] && st.words[0].indexOf(tokens[0]) === 0) score += 20;
    score -= st.words.length * 0.2;                                      // shorter names first, all else equal
    return { s: score, fuzzy: fuzzy };
  }

  function lineDisplayName(name) { return String(name).split(':')[0].trim(); }

  /**
   * stations: [{id, name, lines: [service ids]}]   services: [{id, name, colour, stations: [ids]}]
   * Several services of one line (the two Blue Line branches) become one line named "Blue Line".
   */
  function buildIndex(stations, services) {
    var serviceById = {}, groups = [], groupByName = {};
    services.forEach(function (sv) {
      serviceById[sv.id] = sv;
      var name = lineDisplayName(sv.name);
      var g = groupByName[name];
      if (!g) { g = groupByName[name] = { name: name, colour: sv.colour, words: words(name), stations: [], seen: {} }; groups.push(g); }
      sv.stations.forEach(function (id) { if (!g.seen[id]) { g.seen[id] = true; g.stations.push(id); } });
    });
    var byId = {}, list = stations.map(function (s) {
      var seen = {}, lines = [];
      (s.lines || []).forEach(function (svId) {
        var sv = serviceById[svId]; if (!sv) return;
        var n = lineDisplayName(sv.name);
        if (!seen[n]) { seen[n] = true; lines.push({ name: n, colour: sv.colour }); }
      });
      var entry = { id: s.id, name: s.name, words: words(s.name), key: words(s.name).join(' '), lines: lines };
      byId[s.id] = entry;
      return entry;
    });
    return { stations: list, byId: byId, lines: groups };
  }

  /** Lines whose NAME the typed words describe: every word must start a word of the line's name, and at least one must be distinctive. */
  function matchLines(index, tokens) {
    var distinctive = tokens.filter(function (t) { return !GENERIC[t] && t.length >= 3; });
    if (!distinctive.length) return [];
    var out = [];
    index.lines.forEach(function (g) {
      var ok = tokens.every(function (t) {
        return g.words.some(function (w) { return w.indexOf(t) === 0 || (t.length >= 4 && editDistance(t, w.slice(0, t.length)) <= 1); });
      });
      if (ok) out.push(g);
    });
    return out;
  }

  /**
   * Everything to show while typing:
   *   stations     stations whose own name matches, best first
   *   lines        [{name, colour, stations}]  lines the typed words name, with their stations in order along the line
   *                (stations already listed above are left out of this list so nothing appears twice)
   *   suggestions  only when nothing matched: the closest names, word by word
   *   exact        true when the typed words are the full name of a station
   */
  function search(index, query, opts) {
    var limit = (opts && opts.limit) || 60;
    var tokens = words(query, true);
    var res = { tokens: tokens, stations: [], lines: [], suggestions: [], exact: false };
    if (!tokens.length) return res;

    var hits = [];
    index.stations.forEach(function (st) {
      var r = scoreStation(st, tokens, false);
      if (r.s > 0) hits.push({ st: st, s: r.s, fuzzy: r.fuzzy });
    });
    var firm = hits.filter(function (h) { return !h.fuzzy; });
    var use = firm.length ? firm : hits;                                  // spelling-corrected matches only if nothing matched as typed
    // best score first; equal scores in natural order (so "Noida Sector 51" comes before "Noida Sector 142")
    use.sort(function (a, b) { return b.s - a.s || a.st.key.localeCompare(b.st.key, undefined, { numeric: true }); });
    res.stations = use.slice(0, limit).map(function (h) { return h.st; });
    res.fuzzyOnly = !firm.length && hits.length > 0;
    res.exact = use.length > 0 && !use[0].fuzzy && use[0].st.key === tokens.join(' ');

    var shown = {};
    res.stations.forEach(function (st) { shown[st.id] = true; });
    matchLines(index, tokens).forEach(function (g) {
      var members = g.stations.filter(function (id) { return !shown[id] && index.byId[id]; }).map(function (id) { return index.byId[id]; });
      if (members.length) res.lines.push({ name: g.name, colour: g.colour, stations: members.slice(0, limit) });
    });

    if (!res.stations.length && !res.lines.length) {                      // nothing matched: the closest names, word by word
      var near = [];
      index.stations.forEach(function (st) {
        var r = scoreStation(st, tokens, true);
        if (r.s > 0) near.push({ st: st, s: r.s });
      });
      near.sort(function (a, b) { return b.s - a.s || (a.st.name < b.st.name ? -1 : 1); });
      res.suggestions = near.slice(0, 5).map(function (h) { return h.st; });
    }
    return res;
  }

  /**
   * What to do with text that was typed but not picked from the list.
   *   found        exactly one station (the typed words are its full name, or only one station matches)
   *   ambiguous    several stations match
   *   line         the words name a line, not a station
   *   suggestions  nothing matched as typed; here are close names
   *   none         nothing at all
   */
  function resolve(index, query) {
    var r = search(index, query, { limit: 60 });
    if (!r.tokens.length) return { status: 'none', stations: [] };
    if (r.stations.length && !r.fuzzyOnly) {
      if (r.exact) {                                                       // a full name beats longer names that merely contain it
        var key = r.tokens.join(' ');
        var exact = r.stations.filter(function (s) { return s.key === key; });
        if (exact.length === 1) return { status: 'found', stations: exact };
      }
      return r.stations.length === 1 ? { status: 'found', stations: r.stations } : { status: 'ambiguous', stations: r.stations };
    }
    if (r.fuzzyOnly) return { status: 'suggestions', stations: r.stations.slice(0, 5) };      // never silently "correct" a spelling
    if (r.lines.length) return { status: 'line', line: r.lines[0].name, stations: r.lines[0].stations };
    return r.suggestions.length ? { status: 'suggestions', stations: r.suggestions } : { status: 'none', stations: [] };
  }

  var api = { words: words, editDistance: editDistance, buildIndex: buildIndex, search: search, resolve: resolve, lineDisplayName: lineDisplayName };
  root.MetroGoSearch = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
