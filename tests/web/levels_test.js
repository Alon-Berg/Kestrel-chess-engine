// Tests of docs/play/levels.js without an engine: the table of levels, reading the engine's lines, and choosing a move.
//   deno run tests/web/levels_test.js
import { LEVELS, OPENING_PLIES, SAFE_PLIES, SCORE_CAP, EASE_FROM, EASE_TO, EASE_MIN, candidatesFrom, lineScore, pickMove, tempAt, slipAt, steadiness, wantedLines, chooseMove, levelForRating, levelBlurb } from '../../docs/play/levels.js';

let fails = 0;
const ok = (cond, what) => { if (!cond) { fails++; console.log('FAIL', what); } };

function rngFrom(seed) { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

// ---- the table
const real = LEVELS.filter((l) => !l.full);
ok(LEVELS.length >= 8 && LEVELS[LEVELS.length - 1].full && !LEVELS.slice(0, -1).some((l) => l.full), 'the last level is full strength and only the last');
ok(real.every((l, i) => i === 0 || l.rating > real[i - 1].rating), 'ratings rise from level to level');
ok(real.every((l) => (l.depth > 0) !== (l.nodes > 0)), 'a level limits either the depth or the nodes');
ok(real.every((l) => l.k >= 1 && l.k <= 8), 'k is a valid MultiPV');
const sameLimit = (a, b) => (a.depth || 0) === (b.depth || 0) && (a.nodes || 0) === (b.nodes || 0);
ok(real.every((l, i) => i === 0 || !sameLimit(l, real[i - 1]) || l.temp <= real[i - 1].temp), 'with the same search limit the temperature never rises with the level');
ok(real.every((l, i) => i === 0 || !sameLimit(l, real[i - 1]) || l.slip <= real[i - 1].slip), 'with the same search limit the chance of an oversight never rises with the level');
ok(real.every((l, i) => i === 0 || ((l.depth || 99) >= (real[i - 1].depth || 0))), 'the depth never falls with the level');
ok(real.every((l, i) => i === 0 || !l.nodes || !real[i - 1].nodes || l.nodes >= real[i - 1].nodes), 'the node limit never falls with the level');ok(real.every((l) => !l.nodes || l.nodes >= 2048), 'node limits are at least 2048 (the engine checks its node count every 2048 nodes)');
ok(LEVELS.every((l) => l.name === (l.full ? 'Full strength' : 'Level ' + (l.id + 1) + ' (about ' + l.rating + ')')), 'names');
ok(levelForRating(0) === LEVELS.length - 1 && levelForRating(real[3].rating) === 3 && levelForRating(real[3].rating + 10) === 3 && levelForRating(99999) === real.length - 1, 'levelForRating');
ok(LEVELS.every((l) => levelBlurb(l).length > 10), 'every level has a description');

// ---- scores and lines
ok(lineScore({ mate: false, score: 35 }) === 35 && lineScore({ mate: false, score: 5000 }) === SCORE_CAP && lineScore({ mate: false, score: -5000 }) === -SCORE_CAP, 'scores are cut at the cap');
ok(lineScore({ mate: true, score: 1 }) > lineScore({ mate: true, score: 5 }) && lineScore({ mate: true, score: 5 }) > SCORE_CAP - 100 && lineScore({ mate: true, score: -2 }) < -SCORE_CAP + 100, 'mates');
ok(steadiness(0) === 1 && steadiness(EASE_FROM) === 1 && steadiness(-500) === 1 && steadiness(EASE_TO) === EASE_MIN && steadiness(5000) === EASE_MIN && steadiness((EASE_FROM + EASE_TO) / 2) > EASE_MIN && steadiness((EASE_FROM + EASE_TO) / 2) < 1, 'steadiness is 1 in level positions and falls to its floor in won ones');
const L = (depth, mpv, cp, mv) => `info depth ${depth} seldepth ${depth + 2} multipv ${mpv} score cp ${cp} nodes 100 nps 1000 time 1 pv ${mv} a7a6`;
const lines = [L(1, 1, 10, 'e2e4'), L(1, 2, 5, 'd2d4'), L(1, 3, 0, 'g1f3'), L(2, 1, 12, 'e2e4'), L(2, 2, 6, 'd2d4'), L(2, 3, 1, 'g1f3'), L(3, 1, 20, 'e2e4'), L(3, 2, 8, 'd2d4'), 'bestmove e2e4'];
let c = candidatesFrom(lines, 3);
ok(c.length === 3 && c[0].uci === 'e2e4' && c[0].score === 12 && c[0].depth === 2 && c[2].uci === 'g1f3', 'an unfinished deeper iteration (2 of 3 lines) is ignored');
c = candidatesFrom(lines, 2);
ok(c.length === 2 && c[0].depth === 3 && c[0].score === 20, 'the deepest iteration with enough lines is used');
ok(candidatesFrom(['bestmove e2e4'], 3).length === 0, 'no lines, no candidates');

// ---- choosing
const legal = ['e2e4', 'd2d4', 'g1f3', 'a2a3', 'h2h4'];
const cands = [{ uci: 'e2e4', score: 30, depth: 2 }, { uci: 'd2d4', score: 10, depth: 2 }, { uci: 'g1f3', score: -170, depth: 2 }];
const flat = { temp: 0, slip: 0, k: 3 }, sloppy = { temp: 100, slip: 0, k: 3 }, slippy = { temp: 0, slip: 1, k: 3 };
ok(pickMove(cands, legal, flat, 20, rngFrom(1)) === 'e2e4', 'temperature 0 plays the best move');
let n = 0; for (let i = 0; i < 2000; i++) if (pickMove(cands, legal, slippy, 20, rngFrom(i + 1)) !== 'e2e4') n++;
ok(n === 2000, 'a slip never plays the best move (' + n + ' of 2000)');
ok(pickMove(cands, legal, slippy, SAFE_PLIES - 1, rngFrom(1)) === 'e2e4', 'no slip in the first moves');
const won = [{ uci: 'e2e4', score: 900, depth: 2 }, { uci: 'd2d4', score: 800, depth: 2 }]; let nWon = 0; const rW = rngFrom(99);
for (let i = 0; i < 6000; i++) if (pickMove(won, legal, slippy, 40, rW) !== 'e2e4') nWon++;
ok(Math.abs(nWon / 6000 - EASE_MIN) < 0.03, 'a won position cuts the chance of an oversight to ' + EASE_MIN + ' of its value (' + (nWon / 6000).toFixed(3) + ')');
const count = { e2e4: 0, d2d4: 0, g1f3: 0 }; const rng = rngFrom(42);
for (let i = 0; i < 20000; i++) count[pickMove(cands, legal, sloppy, 30, rng)]++;
// weights exp(0), exp(-0.2), exp(-2): shares 0.4897, 0.4009, 0.0663 ... of 1.0/(1+0.8187+0.1353)
const tot = 1 + Math.exp(-0.2) + Math.exp(-2);
ok(Math.abs(count.e2e4 / 20000 - 1 / tot) < 0.015 && Math.abs(count.d2d4 / 20000 - Math.exp(-0.2) / tot) < 0.015 && Math.abs(count.g1f3 / 20000 - Math.exp(-2) / tot) < 0.01, 'moves are chosen with the weights exp(-loss / temp): ' + JSON.stringify(count));
const strong = { temp: 0, slip: 0, k: 3 }; let off = 0; const r2 = rngFrom(7);
for (let i = 0; i < 4000; i++) if (pickMove(cands, legal, strong, 2, r2) === 'g1f3') off++;
ok(off === 0, 'in the opening a strong level only plays moves within the window of the best one');
let var2 = 0; for (let i = 0; i < 4000; i++) if (pickMove(cands, legal, strong, 2, r2) === 'd2d4') var2++;
ok(var2 / 4000 > 0.25 && var2 / 4000 < 0.37, 'in the opening d2d4 (20 below the best move) is played about 31% of the time: ' + var2 / 4000);
ok(tempAt({ temp: 0 }, 0) > 0 && tempAt({ temp: 0 }, OPENING_PLIES) === 0 && tempAt({ temp: 200 }, 0) < 200 && tempAt({ temp: 200 }, OPENING_PLIES) === 200, 'temperature in the opening');
ok(slipAt({ slip: 0.1 }, 0) === 0 && slipAt({ slip: 0.1 }, SAFE_PLIES) === 0.1, 'oversights start after the first moves');
ok(wantedLines({ k: 1 }, 0, 20) === 4 && wantedLines({ k: 1 }, 30, 20) === 1 && wantedLines({ k: 6 }, 30, 3) === 3 && wantedLines({ k: 8 }, 30, 20) === 8, 'lines asked for');

// ---- the whole decision with a pretend engine
const sent = [];
const fake = async (cmds) => { sent.push(...cmds); return [L(2, 1, 40, 'e2e4'), L(2, 2, 30, 'd2d4'), L(2, 3, 20, 'g1f3'), L(2, 4, -400, 'a2a3'), 'bestmove e2e4']; };
const lv = LEVELS.find((l) => l.depth === 2) || LEVELS[2];
let r = await chooseMove({ level: lv, run: fake, posCmd: 'position startpos', legal, ply: 20, rng: rngFrom(3) });
ok(legal.includes(r.uci), 'chooseMove plays a legal move');
ok(sent.includes('setoption name UCI_LimitStrength value false') && sent.includes('position startpos') && sent.some((s) => s === 'go depth ' + lv.depth) && sent.includes('setoption name MultiPV value 1'), 'chooseMove sends the search: ' + JSON.stringify(sent));
sent.length = 0;
const one = await chooseMove({ level: LEVELS[0], run: fake, posCmd: 'p', legal: ['e2e4'], ply: 20 });
ok(one.uci === 'e2e4' && sent.length === 0, 'a single legal move is played without thinking');
const full = LEVELS[LEVELS.length - 1]; sent.length = 0;
r = await chooseMove({ level: full, run: fake, posCmd: 'position startpos', legal, ply: 30, thinkMs: 777, rng: rngFrom(5) });
ok(r.uci === 'e2e4' && sent.includes('go movetime 777'), 'full strength thinks for the chosen time and plays the engine\'s move: ' + JSON.stringify(sent));
sent.length = 0;
await chooseMove({ level: full, run: fake, posCmd: 'position startpos', legal, ply: 2, thinkMs: 777, rng: rngFrom(5) });
ok(sent.includes('go movetime 250') && sent.includes('setoption name MultiPV value 4'), 'full strength varies its first moves');

console.log(fails ? fails + ' FAILED' : 'ALL OK');
Deno.exit(fails ? 1 : 0);
