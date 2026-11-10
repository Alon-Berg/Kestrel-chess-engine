/* Play Kestrel in the browser: game state, buttons, and the conversation with the engine. */
import { Position, START_FEN, parseUci, moveUci, moveSan, gameStatus, buildPgn, sqName } from './rules.js';
import { BoardView, pieceGlyph } from './board.js';
import { Engine, parseInfo } from './engine.js';

const VERSION = '3.2';
const CONFIG = {
  wasmUrl: 'kestrel.wasm',
  wasmNoSimdUrl: 'kestrel-nosimd.wasm',
  netUrl: 'kestrel_net_3_2.pk.gz',   // the network of 3.2 as made by scripts/pack_net.py
  netSize: 76322400,                 // bytes of the network file once unpacked
  netSha256: '0d26104e6f228e498c7fbe4c3f1d5e8fab5df1cefad64f702b868aa74f8218cd',
  shuffleStart: 64, shuffleCount: 37750272,      // layout of the packed file (see pack_net.py)
  netDownloadSize: 44200000,         // about as many bytes are downloaded (only used for the progress bar when the server hides the size)
  cacheName: 'kestrel-net-3.2',
  hash: 32,                          // MB of hash table
};

/* UCI_Elo makes Kestrel search a limited number of positions per move (4000 * 2^((Elo - 2400) / 100)); the numbers are rough guides */
const LEVELS = [
  { name: 'Level 1 (about 1300)', elo: 1320 },
  { name: 'Level 2 (about 1500)', elo: 1500 },
  { name: 'Level 3 (about 1700)', elo: 1700 },
  { name: 'Level 4 (about 1900)', elo: 1900 },
  { name: 'Level 5 (about 2100)', elo: 2100 },
  { name: 'Level 6 (about 2300)', elo: 2300 },
  { name: 'Level 7 (about 2500)', elo: 2500 },
  { name: 'Level 8 (about 2800)', elo: 2800 },
  { name: 'Full strength', elo: 0 },
];
const TIMES = [[500, '0.5 seconds'], [1000, '1 second'], [2000, '2 seconds'], [5000, '5 seconds'], [10000, '10 seconds']];
const STORE = 'kestrel-play-v1';

const $ = (id) => document.getElementById(id);
const S = {
  startFen: START_FEN, moves: [], human: 'w', level: 3, thinkMs: 2000, flipped: false, over: null, token: 0, thinking: false, showThink: true, resigning: false,
};
let positions = [Position.fromFEN(START_FEN)];
let keys = [positions[0].key()];
let sanList = [];
const cur = () => positions[positions.length - 1];

/* ---------------------------------------------------------------- engine */
const engine = new Engine(Object.assign({
  onProgress: (m) => showLoading(m),
  onState: (state, info) => {
    if (state === 'ready') {
      $('loadbox').hidden = true;
      $('simd').textContent = info.simd ? '' : ' (this browser has no SIMD support, so Kestrel runs slower)';
      renderStatus();
    }
    if (state === 'failed') {
      $('loadbox').hidden = false; $('loadbox').classList.add('bad'); $('meter').hidden = true;
      $('loadtext').textContent = 'Kestrel could not start: ' + info;
      renderStatus();
    }
  },
}, CONFIG));

function mb(n) { return (n / 1e6).toFixed(1); }
function showLoading(m) {
  if (engine.ready || engine.failed) return;
  $('loadbox').hidden = false; $('meter').hidden = false; $('loadbox').classList.remove('bad');
  let text;
  if (m.phase === 'download') text = 'Loading Kestrel (first visit only): ' + mb(m.loaded) + ' of about ' + mb(m.total || CONFIG.netDownloadSize) + ' MB';
  else if (m.phase === 'cache') text = 'Loading Kestrel from this browser’s storage…';
  else text = 'Starting Kestrel…';
  const frac = m.phase === 'download' ? Math.min(1, m.loaded / (m.total || CONFIG.netDownloadSize)) * 0.8 : m.phase === 'cache' ? 0.8 : 0.8 + 0.2 * (m.loaded / m.total);
  $('bar').style.width = Math.round(frac * 100) + '%';
  $('loadtext').textContent = text;
}

/* ---------------------------------------------------------------- board */
const board = new BoardView($('board'), {
  canMoveFrom: (s) => humanTurn() && cur().b[s] && (cur().b[s] === cur().b[s].toUpperCase()) === (S.human === 'w'),
  targetsFrom: (s) => {
    const seen = new Map();
    for (const m of cur().legalMoves()) if (m.from === s) seen.set(m.to, !!m.cap);
    return [...seen].map(([to, capture]) => ({ to, capture }));
  },
  onMove: (from, to) => userMove(from, to),
});

function humanTurn() { return !S.over && !S.thinking && cur().turn === S.human; }

function userMove(from, to) {
  const options = cur().legalMoves().filter((m) => m.from === from && m.to === to);
  if (!options.length) return;
  if (options.length === 1) return playMove(options[0], false);
  // promotion: ask which piece
  const box = $('promo'); box.textContent = ''; box.hidden = false;
  const white = S.human === 'w';
  for (const pr of ['q', 'r', 'b', 'n']) {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'promo-btn ' + (white ? 'w' : 'b'); b.setAttribute('aria-label', { q: 'queen', r: 'rook', b: 'bishop', n: 'knight' }[pr]);
    b.textContent = pieceGlyph(pr);
    b.onclick = () => { box.hidden = true; playMove(options.find((m) => m.promo === pr), false); };
    box.appendChild(b);
  }
  const cancel = document.createElement('button');
  cancel.type = 'button'; cancel.className = 'promo-cancel'; cancel.textContent = 'Cancel';
  cancel.onclick = () => { box.hidden = true; };
  box.appendChild(cancel);
}

/* ---------------------------------------------------------------- game */
function playMove(m, byEngine) {
  const pos = cur();
  const san = moveSan(pos, m);
  S.moves.push(moveUci(m)); sanList.push(san);
  const next = pos.apply(m);
  positions.push(next); keys.push(next.key());
  const st = gameStatus(next, keys);
  if (st.over) S.over = st;
  renderBoard({ from: m.from, to: m.to }, byEngine);
  renderMoves(); renderStatus(); save();
  if (S.over) return;
  if (next.turn !== S.human) engineTurn();
}

function newGame(side, fen = START_FEN) {
  S.token++; S.thinking = false; S.over = null; S.resigning = false;
  if (engine.worker && (engine.failed || engine.broken)) { $('loadbox').classList.remove('bad'); engine.restart(); }
  S.human = side === 'r' ? (Math.random() < 0.5 ? 'w' : 'b') : side;
  S.flipped = false; S.needNewGame = true;
  S.startFen = fen; S.moves = []; sanList = [];
  positions = [Position.fromFEN(fen)]; keys = [positions[0].key()];
  const st0 = gameStatus(positions[0], keys); if (st0.over) S.over = st0;          // a start position that is over already
  $('promo').hidden = true; $('think').textContent = '';
  renderBoard(null, false); renderMoves(); renderStatus(); save();
  if (cur().turn !== S.human) engineTurn();
}

function undo() {
  if (!S.moves.length) return;
  S.token++; S.thinking = false; S.over = null; S.resigning = false;
  $('promo').hidden = true; $('think').textContent = '';
  const pop = () => { S.moves.pop(); sanList.pop(); positions.pop(); keys.pop(); };
  pop();
  while (S.moves.length && cur().turn !== S.human) pop();
  const last = S.moves.length ? S.moves[S.moves.length - 1] : null;
  renderBoard(last ? { from: sqIdx(last.slice(0, 2)), to: sqIdx(last.slice(2, 4)) } : null, false);
  renderMoves(); renderStatus(); save();
  if (cur().turn !== S.human) engineTurn();
}
const sqIdx = (n) => (n.charCodeAt(0) - 97) + (n.charCodeAt(1) - 49) * 8;

function resign() {
  if (S.over) return;
  S.token++; S.thinking = false; S.resigning = false;
  S.over = { over: true, result: S.human === 'w' ? '0-1' : '1-0', reason: 'resignation' };
  renderBoard(lastMoveObj(), false); renderStatus(); save();
}

function lastMoveObj() {
  const u = S.moves[S.moves.length - 1];
  return u ? { from: sqIdx(u.slice(0, 2)), to: sqIdx(u.slice(2, 4)) } : null;
}

async function engineTurn() {
  if (S.over) return;
  const tk = ++S.token;
  S.thinking = true; renderStatus(); board.clearSelection();
  const lvl = LEVELS[S.level];
  const posCmd = 'position fen ' + S.startFen + (S.moves.length ? ' moves ' + S.moves.join(' ') : '');
  const pos = cur();
  const variety = positions.length <= 9 && (lvl.elo === 0 || lvl.elo >= 2300);          // a little variety in the opening
  let limit = ['setoption name UCI_LimitStrength value ' + (lvl.elo ? 'true' : 'false')].concat(lvl.elo ? ['setoption name UCI_Elo value ' + lvl.elo] : []);
  if (S.needNewGame) { limit = ['ucinewgame'].concat(limit); S.needNewGame = false; }
  let chosen = null;
  try {
    if (variety) {
      const lines = await engine.run(limit.concat(['setoption name MultiPV value 4', posCmd, 'go movetime 250', 'setoption name MultiPV value 1']), (l) => { if (tk === S.token) showThink(l, pos); });
      if (tk !== S.token) return;
      chosen = pickVaried(lines, pos);
    }
    if (!chosen) {
      const lines = await engine.run(limit.concat(['setoption name MultiPV value 1', posCmd, 'go movetime ' + S.thinkMs]), (l) => { if (tk === S.token) showThink(l, pos); });
      if (tk !== S.token) return;
      const bm = lines.filter((l) => l.startsWith('bestmove')).pop();
      const u = bm ? bm.split(' ')[1] : null;
      chosen = u ? parseUci(pos, u) : null;
    }
  } catch (e) {
    if (tk !== S.token) return;
    S.thinking = false; $('status').textContent = 'Kestrel stopped: ' + e.message + ' Start a new game to restart it.'; $('status').classList.add('bad'); return;
  }
  if (tk !== S.token) return;
  if (!chosen) { const all = pos.legalMoves(); chosen = all[Math.floor(Math.random() * all.length)]; S.fallbacks = (S.fallbacks || 0) + 1; }       // should not happen
  S.thinking = false;
  playMove(chosen, true);
}

/* the best of the last complete MultiPV round, or one of the moves within 25 centipawns of it (opening variety) */
function pickVaried(lines, pos) {
  const infos = lines.map(parseInfo).filter(Boolean);
  if (!infos.length) return null;
  const depth = Math.max(...infos.map((i) => i.depth));
  const top = infos.filter((i) => i.depth === depth && i.pv.length).sort((a, b) => a.multipv - b.multipv);
  if (!top.length) return null;
  const best = top[0];
  const pool = best.mate ? [best] : top.filter((i) => !i.mate && i.score >= best.score - 25);
  const weights = pool.map((_, k) => (k === 0 ? 2 : 1));
  let r = Math.random() * weights.reduce((a, b) => a + b, 0), k = 0;
  while (k < pool.length - 1 && r >= weights[k]) { r -= weights[k]; k++; }
  return parseUci(pos, pool[k].pv[0]);
}

/* ---------------------------------------------------------------- display */
function fmtScore(info, pos) {
  const sign = pos.turn === 'w' ? 1 : -1;
  if (info.mate) { const m = info.score * sign; return (m < 0 ? '-' : '+') + 'M' + Math.abs(info.score); }
  const v = info.score * sign / 100;
  return (v > 0 ? '+' : '') + v.toFixed(2);
}

function showThink(line, pos) {
  if (!S.showThink) return;
  const info = parseInfo(line);
  if (!info || info.multipv > 1) return;
  const san = []; let p = pos;
  for (const u of info.pv.slice(0, 8)) { const m = parseUci(p, u); if (!m) break; san.push((p.turn === 'w' ? p.full + '. ' : (san.length ? '' : p.full + '... ')) + moveSan(p, m)); p = p.apply(m); }
  const nodes = info.nodes >= 1e6 ? (info.nodes / 1e6).toFixed(1) + 'M' : Math.round(info.nodes / 1000) + 'k';
  $('think').textContent = 'depth ' + info.depth + '  ·  ' + fmtScore(info, pos) + '  ·  ' + nodes + ' nodes' + (info.nps ? '  ·  ' + Math.round(info.nps / 1000) + ' knps' : '') + '  ·  ' + san.join(' ');
}

function renderBoard(last, animate) {
  const pos = cur();
  board.setOrientation((S.human === 'w') !== S.flipped ? 'w' : 'b');
  board.setPosition(pos, { last, check: pos.inCheck() ? pos.kingSq(pos.turn) : -1, animate });
  const bottomWhite = (S.human === 'w') !== S.flipped;
  $('bottomname').querySelector('.who').textContent = bottomWhite === (S.human === 'w') ? 'You' : 'Kestrel ' + VERSION;
  $('topname').querySelector('.who').textContent = bottomWhite === (S.human === 'w') ? 'Kestrel ' + VERSION : 'You';
}

function renderMoves() {
  const ol = $('moves'); ol.textContent = '';
  const first = Position.fromFEN(S.startFen);
  let ply = first.turn === 'b' ? 1 : 0, no = first.full;
  let li = null;
  if (first.turn === 'b' && sanList.length) { li = document.createElement('li'); li.append(no + '. ...'); ol.appendChild(li); }
  sanList.forEach((san, i) => {
    const white = (i + (first.turn === 'b' ? 1 : 0)) % 2 === 0;
    if (white) { li = document.createElement('li'); const n = document.createElement('span'); n.className = 'no'; n.textContent = (no + Math.floor((i + (first.turn === 'b' ? 1 : 0)) / 2)) + '.'; li.appendChild(n); ol.appendChild(li); }
    else if (!li) { li = document.createElement('li'); ol.appendChild(li); }
    const s = document.createElement('span'); s.className = 'mv' + (i === sanList.length - 1 ? ' last' : ''); s.textContent = san; li.appendChild(s);
  });
  ol.scrollTop = ol.scrollHeight;
  $('undo').disabled = !S.moves.length;
  $('resign').disabled = !!S.over;
  $('pgn').disabled = !S.moves.length; $('pgnfile').disabled = !S.moves.length;
}

function resultText(o) {
  const youWon = (o.result === '1-0') === (S.human === 'w') && o.result !== '1/2-1/2';
  if (o.result === '1/2-1/2') return 'Draw by ' + o.reason + '.';
  if (o.reason === 'checkmate') return 'Checkmate. ' + (youWon ? 'You win!' : 'Kestrel wins.');
  if (o.reason === 'resignation') return 'You resigned. Kestrel wins.';
  return youWon ? 'You win.' : 'Kestrel wins.';
}

function renderStatus() {
  const st = $('status'); st.classList.remove('bad', 'done');
  const humanBottom = (S.human === 'w') !== S.flipped;
  const mine = cur().turn === S.human && !S.over;
  const theirs = cur().turn !== S.human && !S.over;
  $('bottomtag').textContent = (humanBottom ? mine : theirs) ? 'to move' : '';
  $('toptag').textContent = (humanBottom ? theirs : mine) ? 'to move' : '';
  if (S.over) { st.textContent = resultText(S.over); st.classList.add('done'); return; }
  if (engine.failed) { st.textContent = 'Kestrel is not available.'; st.classList.add('bad'); return; }
  if (S.thinking) st.textContent = engine.ready ? 'Kestrel is thinking…' : 'Kestrel will move as soon as it has loaded.';
  else if (mine) st.textContent = cur().inCheck() ? 'Your move (you are in check).' : 'Your move.';
  else st.textContent = '';
}

/* ---------------------------------------------------------------- storage and settings */
function save() {
  try { localStorage.setItem(STORE, JSON.stringify({ startFen: S.startFen, moves: S.moves, human: S.human, level: S.level, thinkMs: S.thinkMs, flipped: S.flipped, over: S.over, showThink: S.showThink })); } catch (e) { /* private mode */ }
}

function load() {
  let d = null;
  try { d = JSON.parse(localStorage.getItem(STORE) || 'null'); } catch (e) { d = null; }
  if (!d) return false;
  try {
    if (Number.isInteger(d.level) && d.level >= 0 && d.level < LEVELS.length) S.level = d.level;
    if (TIMES.some((t) => t[0] === d.thinkMs)) S.thinkMs = d.thinkMs;
    S.showThink = d.showThink !== false;
    if (d.human === 'w' || d.human === 'b') S.human = d.human;
    S.flipped = !!d.flipped;
    S.startFen = START_FEN;
    if (typeof d.startFen === 'string') { try { Position.fromFEN(d.startFen); S.startFen = d.startFen; } catch (e) { S.startFen = START_FEN; } }
    let pos = Position.fromFEN(S.startFen); const ps = [pos], ks = [pos.key()], ss = [];
    for (const u of d.moves || []) { const m = parseUci(pos, u); if (!m) throw new Error('bad move'); ss.push(moveSan(pos, m)); pos = pos.apply(m); ps.push(pos); ks.push(pos.key()); }
    S.moves = (d.moves || []).slice(); positions = ps; keys = ks; sanList = ss;
    S.over = d.over && d.over.over ? d.over : null;
    if (!S.over) { const st = gameStatus(pos, ks); if (st.over) S.over = st; }
    return true;
  } catch (e) { S.startFen = START_FEN; S.moves = []; positions = [Position.fromFEN(START_FEN)]; keys = [positions[0].key()]; sanList = []; S.over = null; return false; }
}

function pgnText() {
  const res = S.over ? S.over.result : '*';
  const h = S.human === 'w' ? { White: 'Human', Black: 'Kestrel ' + VERSION } : { White: 'Kestrel ' + VERSION, Black: 'Human' };
  h.Date = new Date().toISOString().slice(0, 10).replace(/-/g, '.');
  return buildPgn(S.startFen, S.moves, res, h);
}

async function copyText(text) {
  try { await navigator.clipboard.writeText(text); return true; } catch (e) { /* fall through */ }
  const ta = document.createElement('textarea'); ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0'; document.body.appendChild(ta); ta.select();
  let ok = false; try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
  ta.remove(); return ok;
}

/* ---------------------------------------------------------------- start */
function init() {
  const sel = $('level');
  LEVELS.forEach((l, i) => { const o = document.createElement('option'); o.value = i; o.textContent = l.name; sel.appendChild(o); });
  const ts = $('time');
  TIMES.forEach(([ms, label]) => { const o = document.createElement('option'); o.value = ms; o.textContent = label; ts.appendChild(o); });
  // ?fen=<position>&side=w|b starts from that position (handy for puzzles and endgames; "New game" goes back to the normal start)
  const q = new URLSearchParams(location.search);
  let customFen = null;
  if (q.get('fen')) { try { customFen = Position.fromFEN(q.get('fen')).fen(); } catch (e) { customFen = null; } }
  const restored = customFen ? false : load();
  sel.value = S.level; ts.value = S.thinkMs; $('showthink').checked = S.showThink;
  sel.onchange = () => { S.level = +sel.value; save(); };
  ts.onchange = () => { S.thinkMs = +ts.value; save(); };
  $('showthink').onchange = (e) => { S.showThink = e.target.checked; if (!S.showThink) $('think').textContent = ''; save(); };
  document.querySelectorAll('[data-side]').forEach((b) => { b.onclick = () => newGame(b.dataset.side); });
  $('undo').onclick = undo;
  $('flip').onclick = () => { S.flipped = !S.flipped; renderBoard(lastMoveObj(), false); renderStatus(); save(); };
  $('resign').onclick = () => {
    if (!S.resigning) { S.resigning = true; $('resign').textContent = 'Really resign?'; setTimeout(() => { S.resigning = false; $('resign').textContent = 'Resign'; }, 3000); return; }
    $('resign').textContent = 'Resign'; resign();
  };
  $('pgn').onclick = async () => { const ok = await copyText(pgnText()); $('pgn').textContent = ok ? 'Copied' : 'Copy failed'; setTimeout(() => { $('pgn').textContent = 'Copy PGN'; }, 1500); };
  $('pgnfile').onclick = () => {
    const url = URL.createObjectURL(new Blob([pgnText()], { type: 'application/x-chess-pgn' }));
    const a = document.createElement('a'); a.href = url; a.download = 'kestrel-game.pgn'; document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  renderBoard(lastMoveObj(), false); renderMoves(); renderStatus();
  if (typeof WebAssembly === 'undefined' || typeof Worker === 'undefined') {
    $('status').textContent = 'This browser cannot run Kestrel (it needs WebAssembly and web workers).'; $('status').classList.add('bad'); return;
  }
  engine.start();
  if (customFen) { newGame(q.get('side') === 'b' ? 'b' : 'w', customFen); }
  else if (restored && !S.over && cur().turn !== S.human) engineTurn();
  window.kestrelPlay = { S, cur, newGame, engine, board, playMove, positions: () => positions };      // handy in the browser console
}
init();
