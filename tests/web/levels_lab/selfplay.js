// Games between two levels of the play page (the same levels.js and the same wasm engine as in the browser), one JSON line per game.
//   deno run -A selfplay.js --a=3 --b=3 --games=20 --seed=1 --out=games.jsonl
// A level is "5" (Level 5), "r1300" (the level labelled about 1300), "full", or a JSON object like {"depth":2,"k":6,"temp":90,"slip":0.03}.
import { Position, START_FEN, parseUci, moveUci, gameStatus } from '../../../docs/play/rules.js';
import { LEVELS, chooseMove } from '../../../docs/play/levels.js';
import { startWasm, makeRng } from './wasm_engine.js';

export function parseLevel(s) {
  if (s.trim().startsWith('{')) return Object.assign({ k: 1, temp: 0, slip: 0, name: s }, JSON.parse(s));
  if (s === 'full') return LEVELS[LEVELS.length - 1];
  if (s[0] === 'r') { const l = LEVELS.find((x) => x.rating === +s.slice(1)); if (!l) throw new Error('no level with rating ' + s); return l; }
  const l = LEVELS[+s - 1]; if (!l) throw new Error('no level ' + s); return l;
}

export async function playGame(engine, lw, lb, rng, o = {}) {
  let pos = Position.fromFEN(START_FEN); const keys = [pos.key()], moves = [], ms = { w: 0, b: 0 };
  await engine.run(['ucinewgame']);
  for (;;) {
    const st = gameStatus(pos, keys);
    if (st.over) return { moves, result: st.result, reason: st.reason, ms };
    if (moves.length >= (o.maxPlies || 300)) return { moves, result: '1/2-1/2', reason: 'move limit', ms };
    const level = pos.turn === 'w' ? lw : lb, legal = pos.legalMoves(), lu = legal.map(moveUci);
    const posCmd = 'position startpos' + (moves.length ? ' moves ' + moves.join(' ') : '');
    const t = performance.now();
    const r = await chooseMove({ level, run: (c) => engine.run(c), posCmd, legal: lu, ply: moves.length, rng, thinkMs: o.thinkMs || 100 });
    ms[pos.turn] += performance.now() - t;
    const m = parseUci(pos, r.uci, legal);
    if (!m) throw new Error('illegal move ' + r.uci);
    moves.push(r.uci); pos = pos.apply(m); keys.push(pos.key());
  }
}

if (import.meta.main) {
  const arg = {}; for (const a of Deno.args) { const m = a.match(/^--([^=]+)=(.*)$/s); if (m) arg[m[1]] = m[2]; }
  const la = parseLevel(arg.a || '1'), lb = parseLevel(arg.b || arg.a || '1');
  const games = +(arg.games || 10), rng = makeRng(+(arg.seed || 1));
  const engine = await startWasm({ hash: 16 });
  const out = [];
  const t0 = performance.now();
  for (let g = 0; g < games; g++) {
    const aWhite = g % 2 === 0;
    const r = await playGame(engine, aWhite ? la : lb, aWhite ? lb : la, rng, { thinkMs: +(arg.think || 100) });
    out.push(JSON.stringify({ wl: (aWhite ? la : lb).name, bl: (aWhite ? lb : la).name, aWhite, ...r }));
    if (arg.progress) console.log('game %d/%d %s %s (%d plies) %ds', g + 1, games, r.result, r.reason, r.moves.length, Math.round((performance.now() - t0) / 1000));
  }
  if (arg.out) await Deno.writeTextFile(arg.out, out.join('\n') + '\n');
  else console.log(out.join('\n'));
}
