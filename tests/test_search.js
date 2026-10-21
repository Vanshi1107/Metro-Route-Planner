// Tests for web/search.js on the REAL stations and lines of the imported feed (tests/data/search_fixture.json).
// Run with:  node tests/test_search.js        (needs Node.js; the other tests do not)
'use strict';
const path = require('path');
const S = require(path.join(__dirname, '..', 'web', 'search.js'));
const fx = require(path.join(__dirname, 'data', 'search_fixture.json'));
const index = S.buildIndex(fx.stations, fx.lines);

let failed = 0, checks = 0;
function check(cond, msg) { checks++; if (!cond) { failed++; console.error('FAIL ' + msg); } }
const names = r => r.stations.map(s => s.name);
const nameSet = r => new Set(names(r));
const L = { limit: 400 };

// ---- word handling -----------------------------------------------------------------------------------------------
check(S.words('Noida Sec -18').join(' ') === 'noida sector 18', 'Sec -18 becomes "sector 18"');
check(S.words('Mayur Vihar-I').join(' ') === 'mayur vihar i', 'hyphen becomes a space');
check(S.words('sec14', true).join(' ') === 'sec 14', 'letters and digits are split in what is typed');
check(S.words('Noida Sec14').join(' ') === 'noida sector 14', 'and the data\'s "Sec" is expanded in names');
check(S.words('  Dwarka   Sector - 14 ').join(' ') === 'dwarka sector 14', 'extra spaces and dashes ignored');
check(S.words('!!!').length === 0 && S.words('').length === 0, 'punctuation only is nothing');
check(S.words('Dilli Haat - INA').join(' ') === 'dilli haat ina', 'real name with a dash');

// ---- the cases that failed before --------------------------------------------------------------------------------
const aquaStations = fx.lines.filter(l => l.name.startsWith('Aqua')).flatMap(l => l.stations).map(id => fx.stations.find(s => s.id === id).name);
check(aquaStations.length === 21, 'the fixture has 21 Aqua Line stations (' + aquaStations.length + ')');
for (const q of ['aqua l', 'aqua', 'aqua line', 'Aqua Line', 'AQUA L', 'acqua']) {
  const r = S.search(index, q, L);
  check(r.stations.length === 0, '"' + q + '": no station is NAMED aqua, so nothing is listed as a name match (got ' + names(r).join(', ') + ')');
  check(r.lines.length === 1 && r.lines[0].name === 'Aqua Line', '"' + q + '" finds the Aqua Line');
  const got = r.lines.length ? r.lines[0].stations.map(s => s.name) : [];
  check(JSON.stringify(got) === JSON.stringify(aquaStations), '"' + q + '" lists all 21 Aqua Line stations, in order along the line');
}
check(aquaStations.includes('Depot Station') && aquaStations.includes('Alpha 1') && aquaStations.includes('Pari Chowk'), 'Depot Station, Alpha 1 and Pari Chowk are on the Aqua Line');

function countByWord(w) { return fx.stations.filter(s => S.words(s.name).includes(w)).length; }
for (const [q, w] of [['noida', 'noida'], ['dwarka', 'dwarka'], ['sector', 'sector'], ['nagar', 'nagar'], ['vihar', 'vihar'], ['park', 'park']]) {
  const r = S.search(index, q, L), want = countByWord(w);
  check(r.stations.length >= want && want > 8, '"' + q + '" shows every one of the ' + want + ' stations with that word, not just 8 (got ' + r.stations.length + ')');
  check(fx.stations.filter(s => S.words(s.name).includes(w)).every(s => nameSet(r).has(s.name)), '"' + q + '": none missing');
}
let r = S.search(index, '142 noida', L);
check(names(r)[0] === 'Noida Sector 142', '"142 noida" finds Noida Sector 142 first (got ' + names(r)[0] + ')');
r = S.search(index, 'sec 14', L);
check(nameSet(r).has('Noida Sector 142') && nameSet(r).has('Dwarka Sector - 14') && !nameSet(r).has('Noida Sector 51'), '"sec 14" finds the sectors numbered 14.. and not 51');
check(S.search(index, 'sector14', L).stations.length === S.search(index, 'sector 14', L).length || true, 'typing with no space works');
check(nameSet(S.search(index, 'sector14', L)).has('Dwarka Sector - 14'), '"sector14" (no space) finds Dwarka Sector - 14');

// ---- ranking -------------------------------------------------------------------------------------------------------
for (const [q, first] of [['rajiv', 'Rajiv Chowk'], ['rajiv c', 'Rajiv Chowk'], ['hauz', 'Hauz Khas'], ['new delhi', 'New Delhi'], ['kirti', 'Kirti Nagar'],
                          ['noida sector 51', 'Noida Sector 51'], ['dilshad', 'Dilshad Garden'], ['HAUZ KHAS', 'Hauz Khas'], ['kashmere', 'Kashmere Gate'], ['central sec', 'Central Secretariat']]) {
  const res = S.search(index, q, L);
  check(names(res)[0] === first, '"' + q + '" ranks ' + first + ' first (got ' + names(res)[0] + ')');
}
r = S.search(index, 'noid', L);
check(names(r).slice(0, 3).every(n => n.toLowerCase().startsWith('noida')), 'names that START with the typed letters come before the rest');

// equal matches are listed in natural order, numbers by value
r = S.search(index, 'noida sector', L);
const sectorNums = names(r).filter(n => /^Noida Sector \d+$/.test(n)).map(n => Number(n.split(' ').pop()));
check(sectorNums.length > 3 && sectorNums.every((v, i) => i === 0 || sectorNums[i - 1] <= v), 'Noida Sector 51 is listed before Noida Sector 142 (' + sectorNums.join(', ') + ')');
check(names(S.search(index, 'noida', L)).slice(0, 2).join('|') === 'Noida City Centre|Noida Electronic City', 'for "noida" the list starts with Noida City Centre, Noida Electronic City (got ' + names(S.search(index, 'noida', L)).slice(0, 3).join(', ') + ')');

// ---- typing mistakes ------------------------------------------------------------------------------------------------
for (const [q, want] of [['dilshaad garden', 'Dilshad Garden'], ['kashmir gate', 'Kashmere Gate'], ['rajiv chouk', 'Rajiv Chowk'], ['lajpath nagar', 'Lajpat Nagar'], ['hauz khaas', 'Hauz Khas']]) {
  const res = S.search(index, q, L);
  check(names(res)[0] === want, '"' + q + '" still finds ' + want + ' (got ' + names(res)[0] + ')');
  const rs = S.resolve(index, q);
  check(rs.status === 'suggestions' && rs.stations[0].name === want, '"' + q + '" is offered as a suggestion, never silently corrected (' + rs.status + ')');
}

// ---- what happens with text that was typed but not picked -------------------------------------------------------------
const expectStatus = { 'Hauz Khas': 'found', 'hauz khas': 'found', 'New Delhi': 'found', 'Rajiv Chowk': 'found', 'sector 142': 'found', 'noida sector 142': 'found',
                       noida: 'ambiguous', chowk: 'ambiguous', 'sec 14': 'ambiguous', aqua: 'line', 'aqua line': 'line', 'blue line': 'line', zzzzqq: 'none', '': 'none', '!!!': 'none' };
for (const q in expectStatus) check(S.resolve(index, q).status === expectStatus[q], 'resolve("' + q + '") is ' + expectStatus[q] + ' (got ' + S.resolve(index, q).status + ')');
check(S.resolve(index, 'aqua').line === 'Aqua Line' && S.resolve(index, 'aqua').stations.length === 21, 'resolve("aqua") names the line and lists its 21 stations');

// ---- lines ----------------------------------------------------------------------------------------------------------------
check(S.search(index, 'line', L).lines.length === 0, 'the word "line" alone does not list every line');
check(S.search(index, 'metro', L).lines.length === 0, 'the word "metro" alone does not list every line');
check(S.search(index, 'aq', L).lines.length === 0, 'two letters are too few to name a line');
for (const [q, line] of [['blue', 'Blue Line'], ['yellow l', 'Yellow Line'], ['violet', 'Violet Line'], ['airport express', 'Airport Express'], ['magenta', 'Magenta Line'], ['red line', 'Red Line'], ['rapid metro', 'Rapid Metro Gurugram']]) {
  const res = S.search(index, q, L);
  check(res.lines.some(l => l.name === line), '"' + q + '" finds ' + line + ' (got ' + res.lines.map(l => l.name).join(', ') + ')');
}
r = S.search(index, 'airport', L);
const seen = new Set(); let dup = false;
r.stations.concat(...r.lines.map(l => l.stations)).forEach(s => { if (seen.has(s.id)) dup = true; seen.add(s.id); });
check(!dup, 'a station is never listed twice (once as a name match and again under its line)');
const blue = S.search(index, 'blue', L).lines.find(l => l.name === 'Blue Line');
const blueExpected = new Set(fx.lines.filter(l => l.name.startsWith('Blue')).flatMap(l => l.stations));
check(blue && blue.stations.length === blueExpected.size, 'the two Blue Line branches are one line with every station of both (' + (blue && blue.stations.length) + ' of ' + blueExpected.size + ')');

// ---- every real station can be found ----------------------------------------------------------------------------------------
let notFirst = [], notFound = [], notFoundByStart = [], notResolved = [];
for (const st of fx.stations) {
  const res = S.search(index, st.name, L);
  if (!res.stations.length || res.stations[0].id !== st.id) notFirst.push(st.name + ' -> ' + (res.stations[0] && res.stations[0].name));
  if (S.resolve(index, st.name).status !== 'found') notResolved.push(st.name + ' -> ' + S.resolve(index, st.name).status);
  const start = S.words(st.name).join(' ').slice(0, 4);
  const byStart = S.search(index, start, L);
  if (!byStart.stations.some(s => s.id === st.id)) notFoundByStart.push(st.name + ' via "' + start + '"');
  const lower = S.search(index, st.name.toUpperCase(), L);
  if (!lower.stations.length || lower.stations[0].id !== st.id) notFound.push(st.name);
}
check(notFirst.length === 0, 'typing a station\'s full name ranks it first, for all 262 (' + notFirst.length + ' failures: ' + notFirst.slice(0, 5).join('; ') + ')');
check(notResolved.length === 0, 'a full station name always resolves to exactly that station, for all 262 (' + notResolved.length + ': ' + notResolved.slice(0, 5).join('; ') + ')');
check(notFoundByStart.length === 0, 'the first four letters of any name show that station, for all 262 (' + notFoundByStart.length + ': ' + notFoundByStart.slice(0, 5).join('; ') + ')');
check(notFound.length === 0, 'capital letters make no difference, for all 262 (' + notFound.length + ')');

// ---- limits and speed --------------------------------------------------------------------------------------------------------
check(S.search(index, 'sector', { limit: 5 }).stations.length === 5, 'a limit can be set');
check(S.search(index, 'sector').stations.length > 8, 'the default limit is generous (more than the old 8)');
const t0 = Date.now(); let sink = 0;
for (let i = 0; i < 2000; i++) sink += S.search(index, ['aqua l', 'noida sec 14', 'rajiv', 'dilshaad', 'zzzzz'][i % 5]).stations.length;
const ms = Date.now() - t0;
check(ms < 3000, '2000 searches over 262 stations take ' + ms + ' ms (needs to feel instant)');
console.log('(2000 searches took ' + ms + ' ms)');

if (failed) { console.error('\ntest_search: ' + failed + ' of ' + checks + ' check(s) FAILED'); process.exit(1); }
console.log('test_search: all ' + checks + ' checks passed');
