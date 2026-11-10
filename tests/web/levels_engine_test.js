// The levels of the play page with the real WebAssembly engine: every level must answer with a legal move, stay inside its search limit, give the same move for the same
// random numbers, and the weaker levels must really play worse moves more often than the stronger ones.
//   deno run --allow-read tests/web/levels_engine_test.js <kestrel_net.bin> [folder with kestrel.wasm = docs/play]
import { instantiate } from '../../docs/play/kestrel-wasi.js';
import { Position, START_FEN, moveUci } from '../../docs/play/rules.js';
import { LEVELS, chooseMove } from '../../docs/play/levels.js';
import { parseInfo } from '../../docs/play/engine.js';

const NET = Deno.args[0];
if (!NET) { console.log('usage: deno run --allow-read tests/web/levels_engine_test.js <kestrel_net.bin> [folder]'); Deno.exit(2); }
const DIR = Deno.args[1] || new URL('../../docs/play/', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const net = await Deno.readFile(NET), wasm = await Deno.readFile(DIR + '/kestrel.wasm');
let out = [];
const inst = await instantiate(wasm, { onStdout: (l) => out.push(l) });
const e = inst.exports, enc = new TextEncoder();
const ptr = e.kestrel_net_buffer(net.length);       // (growing the memory detaches the old buffer: read inst.memory.buffer after the call)
new Uint8Array(inst.memory.buffer, ptr, net.length).set(net);
if (!e.kestrel_init(16)) throw new Error('the network was not accepted');
const uci = (c) => { const p = e.kestrel_cmd_buffer(), b = enc.encode(c + '\0'); new Uint8Array(inst.memory.buffer, p, b.length).set(b); e.kestrel_uci(); inst.flush(); };
const run = async (cmds) => { out = []; for (const c of cmds) uci(c); return out; };

let fails = 0;
const ok = (cond, what) => { if (!cond) { fails++; console.log('FAIL', what); } };
const rngFrom = (seed) => { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; };

const FENS = [
  START_FEN,
  'r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP1N1PPP/R2QK2R w KQ - 0 8',
  '2r2rk1/1b2qppp/p2pbn2/1p2p3/4P3/1BN1BN2/PPP2PPP/2RQ1RK1 w - - 0 14',
  '8/5pk1/6p1/3P4/2K5/8/6PP/8 w - - 0 40',
  '6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1',            // mate in one
];

async function move(level, fen, seed, ply, thinkMs = 100) {
  await run(['ucinewgame']);
  const pos = Position.fromFEN(fen), legal = pos.legalMoves().map(moveUci);
  let lines = [];
  const r = await chooseMove({ level, run: async (c) => { const o = await run(c); lines = o; return o; }, posCmd: 'position fen ' + fen, legal, ply, rng: rngFrom(seed), thinkMs });
  return { uci: r.uci, legal, lines };
}

// 1. every level, every position: a legal move, inside the limit; the same random numbers give the same move
for (const level of LEVELS) {
  for (const fen of FENS) {
    const a = await move(level, fen, 11, 30), b = await move(level, fen, 11, 30);
    ok(a.legal.includes(a.uci), level.name + ': legal move in ' + fen + ' (' + a.uci + ')');
    ok(a.uci === b.uci, level.name + ': the same random numbers give the same move in ' + fen + ' (' + a.uci + ' / ' + b.uci + ')');
    if (level.depth) {
      const deepest = Math.max(0, ...a.lines.map(parseInfo).filter(Boolean).map((i) => i.depth));
      ok(deepest <= level.depth, level.name + ': searched to depth ' + deepest + ' > ' + level.depth);
    }
  }
}

// 2. the mate in one is found by all levels but the weakest ones may slip: from level 7 up it is found in at least 19 of 20 tries
for (const level of LEVELS.filter((l) => l.full || l.rating >= 1700)) {
  let found = 0;
  for (let s = 1; s <= 20; s++) { const r = await move(level, FENS[4], s, 40, 50); if (r.uci === 'd1d8') found++; }
  ok(found >= 19, level.name + ': mate in one found ' + found + ' of 20 times');
}

// 3. weaker levels agree less often with the best move (Level 1 against full strength, over 5 positions x 12 seeds)
const bestOf = {};
for (const fen of FENS) bestOf[fen] = (await move(LEVELS[LEVELS.length - 1], fen, 1, 30, 300)).uci;
const agree = async (level) => { let n = 0, t = 0; for (const fen of FENS.slice(0, 4)) for (let s = 1; s <= 12; s++) { t++; if ((await move(level, fen, s, 30)).uci === bestOf[fen]) n++; } return n / t; };
const aWeak = await agree(LEVELS[0]), aMid = await agree(LEVELS[Math.floor(LEVELS.length / 2)]), aStrong = await agree(LEVELS[LEVELS.length - 2]);
console.log('agreement with the best move: first level %s, middle level %s, last level before full strength %s', aWeak.toFixed(2), aMid.toFixed(2), aStrong.toFixed(2));
ok(aWeak < aMid + 0.05 && aMid < aStrong + 0.05 && aWeak < aStrong, 'weaker levels agree with the best move less often');

console.log(fails ? fails + ' FAILED' : 'ALL OK');
Deno.exit(fails ? 1 : 0);
