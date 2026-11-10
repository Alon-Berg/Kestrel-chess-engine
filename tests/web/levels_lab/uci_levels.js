// The play page's levels as a UCI engine (for fastchess or any GUI): the WebAssembly Kestrel and docs/play/levels.js, nothing else.
//   deno run -A uci_levels.js        options: Level (1..13, 13 = full strength), Custom (a JSON level, overrides Level), Seed
import { Position, START_FEN, parseUci, moveUci } from '../../../docs/play/rules.js';
import { LEVELS, chooseMove } from '../../../docs/play/levels.js';
import { startWasm, makeRng } from './wasm_engine.js';

const enc = new TextEncoder();
const say = (s) => Deno.stdout.writeSync(enc.encode(s + '\n'));
const engine = await startWasm({ hash: 16 });
let levelNo = 5, custom = null, rng = makeRng(1), posCmd = 'position startpos', pos = Position.fromFEN(START_FEN), nMoves = 0;
const level = () => (custom ? Object.assign({ k: 1, temp: 0, slip: 0, name: 'custom' }, JSON.parse(custom)) : LEVELS[levelNo - 1]);

function setPosition(tokens) {
  let i = 1, fen = START_FEN;
  if (tokens[i] === 'startpos') i++;
  else if (tokens[i] === 'fen') { fen = tokens.slice(i + 1, i + 7).join(' '); i += 7; }
  pos = Position.fromFEN(fen); nMoves = 0;
  if (tokens[i] === 'moves') for (const u of tokens.slice(i + 1)) { const m = parseUci(pos, u); if (!m) break; pos = pos.apply(m); nMoves++; }
  posCmd = tokens.join(' ');
}

async function go(tokens) {
  const lv = level(), legal = pos.legalMoves(), lu = legal.map(moveUci);
  if (!lu.length) { say('bestmove 0000'); return; }
  if (lv.full) {
    const lines = await engine.run(['setoption name UCI_LimitStrength value false', 'setoption name MultiPV value 1', posCmd, tokens.join(' ')]);
    say(lines.filter((l) => l.startsWith('bestmove')).pop() || 'bestmove ' + lu[0]); return;
  }
  const r = await chooseMove({ level: lv, run: (c) => engine.run(c), posCmd, legal: lu, ply: nMoves, rng });
  say('bestmove ' + r.uci);
}

const reader = Deno.stdin.readable.pipeThrough(new TextDecoderStream()).getReader();
let buf = '';
for (;;) {
  const { done, value } = await reader.read();
  if (done) break;
  buf += value;
  let nl;
  while ((nl = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, nl).trim(); buf = buf.slice(nl + 1);
    if (!line) continue;
    const t = line.split(/\s+/);
    if (t[0] === 'uci') {
      say('id name KestrelLevels'); say('id author Kestrel play page');
      say('option name Level type spin default 5 min 1 max ' + LEVELS.length);
      say('option name Custom type string default <empty>');
      say('option name Seed type spin default 1 min 0 max 1000000');
      say('option name Hash type spin default 16 min 1 max 16'); say('option name Threads type spin default 1 min 1 max 1');
      say('uciok');
    } else if (t[0] === 'isready') say('readyok');
    else if (t[0] === 'setoption') {
      const name = t[2], value = t.slice(4).join(' ');
      if (name === 'Level') levelNo = +value;
      else if (name === 'Custom') custom = value === '<empty>' || !value ? null : value;
      else if (name === 'Seed') rng = makeRng(+value);
    } else if (t[0] === 'ucinewgame') { await engine.run(['ucinewgame']); }
    else if (t[0] === 'position') setPosition(t);
    else if (t[0] === 'go') await go(t);
    else if (t[0] === 'quit') Deno.exit(0);
  }
}
