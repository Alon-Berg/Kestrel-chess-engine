/* The strength levels of the play page, and how Kestrel picks its move at each of them.

   Why not just UCI_Elo?  Kestrel's UCI_Elo option limits the nodes searched per move, but the engine looks at its node count only every 2048 nodes, so every setting
   below about 2300 searches the same ~2000 nodes (4 to 7 plies, with a very good evaluation) and plays at one and the same, rather high, strength.
   And a weak player does not just look less deeply: a weak player also overlooks things now and then.  So a level has four parts:

     depth | nodes   how much Kestrel looks at (plies, or positions)
     k               how many of its best moves it keeps in mind (MultiPV, at most 8)
     temp            how carelessly it picks among them: a move that looks x centipawns worse than the best one gets the weight exp(-x / temp)
     slip            the chance per move of a plain oversight: a random legal move other than the best one

   The first moves of a game are always played with care (and, at the strong levels, with a little variety).  A player who is clearly ahead gets steadier: temp and
   slip shrink when the best move is worth more than EASE_FROM centipawns (a person who is a queen up does not suddenly play at random).  Everything here is plain
   data and functions, so the same file runs in the page and in the test programs that measured the levels (tests/web/levels_lab, tests/web/levels_*.js). */
import { parseInfo } from './engine.js';

export const OPENING_PLIES = 10;       // half moves played carefully; the same few ideas as a book would give
export const SAFE_PLIES = 8;           // no oversights before this ply
export const OPENING_TEMP = 25;        // centipawns: variety of the first moves (at the strong levels, and the floor for the others)
export const OPENING_WINDOW = 25;      // in the opening only moves within this many centipawns of the best one may be played at the strong levels
export const SCORE_CAP = 3000;         // scores are cut at +-30 pawns: a mate counts as +-30 pawns (faster mates a little more)
export const EASE_FROM = 150;          // when the best move is worth more than this many centipawns the player gets steadier: temp and slip shrink ...
export const EASE_TO = 800;            // ... down to EASE_MIN times their value at this score (a person who is a queen up does not suddenly play at random)
export const EASE_MIN = 0.35;

/* rating: the number in the name; the whole table is calibrated by tests/web/levels_*.js and documented in docs/BUILDING.md */
export const LEVELS = [
  { rating: 500, depth: 1, k: 8, temp: 800, slip: 0.30 },
  { rating: 700, depth: 1, k: 8, temp: 430, slip: 0.17 },
  { rating: 900, depth: 1, k: 8, temp: 260, slip: 0.10 },
  { rating: 1100, depth: 1, k: 8, temp: 130, slip: 0.052 },
  { rating: 1300, depth: 2, k: 6, temp: 175, slip: 0.07 },
  { rating: 1500, depth: 3, k: 5, temp: 160, slip: 0.064 },
  { rating: 1700, depth: 3, k: 5, temp: 112, slip: 0.045 },
  { rating: 1900, depth: 4, k: 4, temp: 100, slip: 0.04 },
  { rating: 2100, depth: 4, k: 4, temp: 55, slip: 0.022 },
  { rating: 2300, depth: 5, k: 4, temp: 40, slip: 0.016 },
  { rating: 2500, nodes: 4096, k: 2, temp: 15, slip: 0 },
  { rating: 2800, nodes: 16384, k: 1, temp: 0, slip: 0 },
  { rating: 0, full: true, k: 1, temp: 0, slip: 0 },
].map((l, i) => Object.assign({ id: i, name: l.full ? 'Full strength' : 'Level ' + (i + 1) + ' (about ' + l.rating + ')' }, l));

export const DEFAULT_RATING = 1300;

/* index of the level whose rating is nearest to r (0 = full strength) */
export function levelForRating(r) {
  if (r === 0) return LEVELS.length - 1;
  let best = 0;
  LEVELS.forEach((l, i) => { if (!l.full && Math.abs(l.rating - r) < Math.abs(LEVELS[best].rating - r)) best = i; });
  return best;
}

/* one sentence under the level menu; the mistake rates are those measured for the levels (a serious mistake = a move that loses about a piece or more, 300
   centipawns, as judged by Stockfish; levels 1 to 12 make one with 13.2, 8.6, 7.3, 5.2, 4.9, 4.7, 3.8, 2.9, 1.8, 1.6, 1.4 and 0.5 % of their moves against an
   opponent of the same level, see docs/BUILDING.md) */
export function levelBlurb(l) {
  if (l.full) return 'Kestrel at its best, for as long as you let it think.';
  if (l.rating <= 800) return 'A beginner: sees one move ahead and makes a serious mistake every eight to twelve moves.';
  if (l.rating <= 1200) return 'Sees one move ahead and makes a serious mistake every fifteen to twenty moves.';
  if (l.rating <= 1600) return 'Sees two or three moves ahead and makes a serious mistake about every twenty moves.';
  if (l.rating <= 2000) return 'Sees three or four moves ahead and makes a serious mistake about every thirty moves.';
  if (l.rating <= 2400) return 'Sees four or five moves ahead and makes a serious mistake only every sixty moves or so.';
  return 'Looks at thousands of positions for every move and seldom makes a serious mistake.';
}

export const clampScore = (s) => Math.max(-SCORE_CAP, Math.min(SCORE_CAP, s));

/* "score" of an info line for the side to move: centipawns, cut at +-SCORE_CAP; a mate is worth the cap, minus a little for each move it takes */
export function lineScore(info) {
  if (info.mate) return info.score > 0 ? SCORE_CAP - Math.min(info.score, 50) * 0.5 : -SCORE_CAP + Math.min(-info.score, 50) * 0.5;
  return clampScore(info.score);
}

/* the candidate moves of the deepest iteration that finished for all wanted lines: [{uci, score, depth}] best first */
export function candidatesFrom(lines, want) {
  const byDepth = new Map();
  for (const l of lines) {
    const i = parseInfo(l);
    if (!i || !i.pv.length) continue;
    if (!byDepth.has(i.depth)) byDepth.set(i.depth, new Map());
    byDepth.get(i.depth).set(i.multipv, i);
  }
  if (!byDepth.size) return [];
  const depths = [...byDepth.keys()].sort((a, b) => b - a);
  let pick = depths.find((d) => byDepth.get(d).size >= want);
  if (pick === undefined) pick = depths[0];
  return [...byDepth.get(pick).values()].sort((a, b) => a.multipv - b.multipv).map((i) => ({ uci: i.pv[0], score: lineScore(i), depth: i.depth }));
}

export const tempAt = (level, ply) => (ply < OPENING_PLIES ? Math.max(OPENING_TEMP, level.temp * 0.4) : level.temp);
export const slipAt = (level, ply) => (ply < SAFE_PLIES ? 0 : level.slip);

/* 1 in a level position, EASE_MIN in a won one: how much of its carelessness a player shows when the best move is worth `best` centipawns */
export function steadiness(best) {
  if (!(best > EASE_FROM)) return 1;
  return Math.max(EASE_MIN, 1 - (1 - EASE_MIN) * (best - EASE_FROM) / (EASE_TO - EASE_FROM));
}

/* Chooses one of the candidates (best first) the way the level says.  rng() gives numbers in [0, 1). */
export function pickMove(cands, legal, level, ply, rng) {
  if (!cands.length) return legal[Math.floor(rng() * legal.length)];
  const g = steadiness(cands[0].score);
  const slip = slipAt(level, ply) * g;
  if (slip > 0 && rng() < slip) {
    const others = legal.filter((u) => u !== cands[0].uci);
    if (others.length) return others[Math.floor(rng() * others.length)];
  }
  const t = tempAt(level, ply) * g;
  if (!(t > 0) || cands.length === 1) return cands[0].uci;
  const top = cands[0].score;
  // in the opening of the strong levels only moves close to the best one are allowed
  const pool = level.temp * 0.4 < OPENING_TEMP && ply < OPENING_PLIES ? cands.filter((c) => c.score >= top - OPENING_WINDOW) : cands;
  const w = pool.map((c) => Math.exp((c.score - top) / t));
  let r = rng() * w.reduce((a, b) => a + b, 0), k = 0;
  while (k < pool.length - 1 && r >= w[k]) { r -= w[k]; k++; }
  return pool[k].uci;
}

/* How many lines the engine is asked for at this point of the game. */
export function wantedLines(level, ply, nLegal) {
  const opening = ply < OPENING_PLIES;
  return Math.max(1, Math.min(nLegal, opening ? Math.max(level.k, 4) : level.k));
}

/* The whole decision.  o: {level, run(cmds, onLine) -> Promise<lines>, posCmd, legal: [uci], ply, onLine, thinkMs, rng}.  Returns {uci, cands}. */
export async function chooseMove(o) {
  const { level, run, posCmd, legal, ply, onLine, thinkMs = 2000, rng = Math.random } = o;
  if (!legal.length) return null;
  if (legal.length === 1) return { uci: legal[0], cands: [] };
  const opening = ply < OPENING_PLIES;
  const k = wantedLines(level, ply, legal.length);
  const go = level.full ? 'go movetime ' + (opening ? 250 : thinkMs) : level.depth ? 'go depth ' + level.depth : 'go nodes ' + level.nodes;
  const lines = await run(['setoption name UCI_LimitStrength value false', 'setoption name MultiPV value ' + k, posCmd, go, 'setoption name MultiPV value 1'], onLine);
  const bm = lines.filter((l) => l.startsWith('bestmove')).pop();
  const best = bm ? bm.split(' ')[1] : null;
  const cands = k > 1 ? candidatesFrom(lines, k) : [];
  if (!cands.length) {
    // one line wanted (or none came back): the engine's own choice, unless the level slips
    const slip = slipAt(level, ply);
    if (slip > 0 && rng() < slip) { const others = legal.filter((u) => u !== best); if (others.length) return { uci: others[Math.floor(rng() * others.length)], cands }; }
    return { uci: legal.includes(best) ? best : legal[Math.floor(rng() * legal.length)], cands };
  }
  const uci = pickMove(cands, legal, level, ply, rng);
  return { uci: legal.includes(uci) ? uci : legal[0], cands };
}
