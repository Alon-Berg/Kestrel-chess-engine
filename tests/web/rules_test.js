// Tests web/rules.js against python-chess (cases from gen_rules_cases.py) and against the known perft numbers.
//   python tests/web/gen_rules_cases.py 250 1 && deno run --allow-read tests/web/rules_test.js
import { Position, perft, moveUci, moveSan, gameStatus, parseUci, START_FEN, buildPgn } from '../../docs/play/rules.js';

let fails = 0;
const fail = (...a) => { fails++; if (fails <= 20) console.log('FAIL', ...a); };

// ---- perft
const PERFT = [
  [START_FEN, 4, 197281],
  ['r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1', 3, 97862],
  ['8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1', 4, 43238],
  ['r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1', 3, 9467],
  ['rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8', 3, 62379],
  ['r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10', 3, 89890],
];
for (const [fen, d, want] of PERFT) {
  const got = perft(Position.fromFEN(fen), d);
  console.log('perft %d %s: %d %s', d, fen.slice(0, 30), got, got === want ? 'ok' : 'WRONG (want ' + want + ')');
  if (got !== want) fails++;
}

// ---- python-chess cases
const cases = JSON.parse(await Deno.readTextFile(new URL('./rules_cases.json', import.meta.url)));
let n = 0, sans = 0;
for (const c of cases) {
  n++;
  const pos = Position.fromFEN(c.fen);
  if (pos.fen() !== c.fen) fail('fen round trip', c.fen, pos.fen());
  const legal = pos.legalMoves();
  const mine = legal.map(moveUci).sort();
  if (JSON.stringify(mine) !== JSON.stringify(c.legal)) { fail('legal moves differ', c.fen, mine.join(' '), '|', c.legal.join(' ')); continue; }
  for (const m of legal) {
    const u = moveUci(m);
    const san = moveSan(pos, m, legal);
    sans++;
    if (san !== c.san[u]) fail('san', c.fen, u, san, c.san[u]);
    if (moveSan(pos, parseUci(pos, u)) !== c.san[u]) fail('san with a fresh move list', c.fen, u);
    const after = pos.apply(m).fen();
    if (after !== c.after[u]) fail('position after', c.fen, u, after, c.after[u]);
    const back = parseUci(pos, u, legal);
    if (!back || back !== m) fail('parseUci', c.fen, u);
  }
  if (pos.inCheck() !== c.check) fail('check flag', c.fen);
  const st = gameStatus(pos, []);
  const want = c.mate ? 'checkmate' : c.stale ? 'stalemate' : c.insuf ? 'insufficient material' : c.fifty ? 'fifty-move rule' : '';
  if ((st.over ? st.reason : '') !== want) fail('status', c.fen, st.reason, want);
}
console.log('%d positions, %d SAN strings compared; failures: %d', n, sans, fails);

// ---- repetition: knights going back and forth
{
  let pos = Position.fromFEN(START_FEN); const keys = [pos.key()]; let status;
  for (const u of 'g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1 f6g8'.split(' ')) {
    pos = pos.apply(parseUci(pos, u)); keys.push(pos.key()); status = gameStatus(pos, keys);
  }
  console.log('repetition after 8 plies:', status.reason);
  if (status.reason !== 'threefold repetition') fails++;
}
// ---- pgn
console.log(buildPgn(START_FEN, ['e2e4', 'e7e5', 'g1f3', 'b8c6', 'f1b5'], '*', { White: 'Human', Black: 'Kestrel 3.2' }));
console.log(fails ? 'FAILED' : 'ALL OK');
Deno.exit(fails ? 1 : 0);
