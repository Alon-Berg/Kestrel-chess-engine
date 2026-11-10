/* Chess rules for the Kestrel play page: legal moves, check / mate / draw detection, move notation.
   Standard chess only (no Chess960).  Squares are numbered a1 = 0 ... h1 = 7, a2 = 8 ... h8 = 63.  Pieces are the FEN letters (white upper case). */

export const FILES = 'abcdefgh';
export const START_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

const KNIGHT = [[1, 2], [2, 1], [2, -1], [1, -2], [-1, -2], [-2, -1], [-2, 1], [-1, 2]];
const KING = [[1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1], [0, -1], [1, -1]];
const DIAG = [[1, 1], [1, -1], [-1, 1], [-1, -1]];
const ORTHO = [[1, 0], [-1, 0], [0, 1], [0, -1]];

export const sqName = (s) => FILES[s & 7] + ((s >> 3) + 1);
export const sqFromName = (n) => (n.charCodeAt(0) - 97) + (n.charCodeAt(1) - 49) * 8;
const isWhitePiece = (p) => p >= 'A' && p <= 'Z';
const colorOf = (p) => (isWhitePiece(p) ? 'w' : 'b');

/* move kinds */
export const NORMAL = 0, DOUBLE = 1, EP = 2, CASTLE_K = 3, CASTLE_Q = 4;

export class Position {
  constructor() {
    this.b = new Array(64).fill('');
    this.turn = 'w';
    this.castle = '';          // subset of KQkq in that order
    this.ep = -1;              // the square a pawn that just made a double step skipped, or -1
    this.half = 0;
    this.full = 1;
  }

  static fromFEN(fen) {
    const parts = String(fen).trim().split(/\s+/);
    if (parts.length < 2) throw new Error('bad FEN');
    const p = new Position();
    const rows = parts[0].split('/');
    if (rows.length !== 8) throw new Error('bad FEN: ranks');
    for (let i = 0; i < 8; i++) {
      const r = 7 - i; let f = 0;
      for (const ch of rows[i]) {
        if (ch >= '1' && ch <= '8') f += +ch;
        else if ('pnbrqkPNBRQK'.includes(ch)) { if (f > 7) throw new Error('bad FEN: rank too long'); p.b[r * 8 + f] = ch; f++; }
        else throw new Error('bad FEN: ' + ch);
      }
      if (f !== 8) throw new Error('bad FEN: rank length');
    }
    p.turn = parts[1] === 'b' ? 'b' : 'w';
    const c = parts[2] && parts[2] !== '-' ? parts[2] : '';
    for (const ch of 'KQkq') if (c.includes(ch)) p.castle += ch;
    p.ep = parts[3] && parts[3] !== '-' && /^[a-h][36]$/.test(parts[3]) ? sqFromName(parts[3]) : -1;
    p.half = parts[4] ? Math.max(0, parseInt(parts[4], 10) || 0) : 0;
    p.full = parts[5] ? Math.max(1, parseInt(parts[5], 10) || 1) : 1;
    // castling rights need the king and the rook at home
    let cc = '';
    for (const ch of p.castle) {
      const kingSq = ch === 'K' || ch === 'Q' ? 4 : 60, rookSq = ch === 'K' ? 7 : ch === 'Q' ? 0 : ch === 'k' ? 63 : 56;
      if (p.b[kingSq] === (ch === 'K' || ch === 'Q' ? 'K' : 'k') && p.b[rookSq] === (ch === 'K' || ch === 'Q' ? 'R' : 'r')) cc += ch;
    }
    p.castle = cc;
    // sanity: one king each, no pawns on the back ranks, the side that is not to move must not be in check
    let wk = 0, bk = 0;
    for (let s = 0; s < 64; s++) {
      const q = p.b[s];
      if (q === 'K') wk++; else if (q === 'k') bk++;
      if ((q === 'P' || q === 'p') && (s < 8 || s >= 56)) throw new Error('bad FEN: pawn on the back rank');
    }
    if (wk !== 1 || bk !== 1) throw new Error('bad FEN: each side needs exactly one king');
    if (p.attackedBy(p.kingSq(p.turn === 'w' ? 'b' : 'w'), p.turn)) throw new Error('bad FEN: the side that moved is in check');
    return p;
  }

  clone() {
    const q = new Position();
    q.b = this.b.slice(); q.turn = this.turn; q.castle = this.castle; q.ep = this.ep; q.half = this.half; q.full = this.full;
    return q;
  }

  fen() {
    let s = '';
    for (let r = 7; r >= 0; r--) {
      let e = 0;
      for (let f = 0; f < 8; f++) {
        const q = this.b[r * 8 + f];
        if (!q) e++; else { if (e) { s += e; e = 0; } s += q; }
      }
      if (e) s += e;
      if (r) s += '/';
    }
    return s + ' ' + this.turn + ' ' + (this.castle || '-') + ' ' + (this.ep >= 0 ? sqName(this.ep) : '-') + ' ' + this.half + ' ' + this.full;
  }

  kingSq(color) {
    const k = color === 'w' ? 'K' : 'k';
    for (let s = 0; s < 64; s++) if (this.b[s] === k) return s;
    return -1;
  }

  /* is square s attacked by a piece of color `by`? */
  attackedBy(s, by) {
    const b = this.b, f = s & 7, r = s >> 3, white = by === 'w';
    // pawns
    const pr = white ? r - 1 : r + 1;
    if (pr >= 0 && pr < 8) {
      const pawn = white ? 'P' : 'p';
      if (f > 0 && b[pr * 8 + f - 1] === pawn) return true;
      if (f < 7 && b[pr * 8 + f + 1] === pawn) return true;
    }
    const knight = white ? 'N' : 'n', king = white ? 'K' : 'k';
    for (const [df, dr] of KNIGHT) { const nf = f + df, nr = r + dr; if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8 && b[nr * 8 + nf] === knight) return true; }
    for (const [df, dr] of KING) { const nf = f + df, nr = r + dr; if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8 && b[nr * 8 + nf] === king) return true; }
    const bishop = white ? 'B' : 'b', rook = white ? 'R' : 'r', queen = white ? 'Q' : 'q';
    for (const [df, dr] of DIAG) {
      let nf = f + df, nr = r + dr;
      while (nf >= 0 && nf < 8 && nr >= 0 && nr < 8) {
        const q = b[nr * 8 + nf];
        if (q) { if (q === bishop || q === queen) return true; break; }
        nf += df; nr += dr;
      }
    }
    for (const [df, dr] of ORTHO) {
      let nf = f + df, nr = r + dr;
      while (nf >= 0 && nf < 8 && nr >= 0 && nr < 8) {
        const q = b[nr * 8 + nf];
        if (q) { if (q === rook || q === queen) return true; break; }
        nf += df; nr += dr;
      }
    }
    return false;
  }

  inCheck(color = this.turn) {
    return this.attackedBy(this.kingSq(color), color === 'w' ? 'b' : 'w');
  }

  /* all pseudo-legal moves of the side to move (the own king may be left in check) */
  pseudoMoves() {
    const out = [], b = this.b, white = this.turn === 'w';
    const push = (from, to, piece, kind = NORMAL, promo = '') => out.push({ from, to, piece, cap: kind === EP ? (white ? 'p' : 'P') : b[to], promo, kind });
    for (let s = 0; s < 64; s++) {
      const piece = b[s];
      if (!piece || isWhitePiece(piece) !== white) continue;
      const f = s & 7, r = s >> 3, t = piece.toUpperCase();
      if (t === 'P') {
        const dr = white ? 1 : -1, startRank = white ? 1 : 6, promoRank = white ? 7 : 0, nr = r + dr;
        if (nr < 0 || nr > 7) continue;
        const addPawn = (to, kind) => {
          if ((to >> 3) === promoRank) for (const pr of 'qrbn') push(s, to, piece, kind, pr);
          else push(s, to, piece, kind);
        };
        if (!b[nr * 8 + f]) {
          addPawn(nr * 8 + f, NORMAL);
          if (r === startRank && !b[(r + 2 * dr) * 8 + f]) push(s, (r + 2 * dr) * 8 + f, piece, DOUBLE);
        }
        for (const df of [-1, 1]) {
          const nf = f + df; if (nf < 0 || nf > 7) continue;
          const to = nr * 8 + nf, q = b[to];
          if (q && isWhitePiece(q) !== white) addPawn(to, NORMAL);
          else if (!q && to === this.ep) push(s, to, piece, EP);
        }
      } else if (t === 'N' || t === 'K') {
        for (const [df, dr] of (t === 'N' ? KNIGHT : KING)) {
          const nf = f + df, nr = r + dr;
          if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
          const to = nr * 8 + nf, q = b[to];
          if (!q || isWhitePiece(q) !== white) push(s, to, piece);
        }
        if (t === 'K') {
          const home = white ? 4 : 60, enemy = white ? 'b' : 'w';
          if (s === home && !this.attackedBy(s, enemy)) {
            if (this.castle.includes(white ? 'K' : 'k') && !b[home + 1] && !b[home + 2] && b[home + 3] === (white ? 'R' : 'r') &&
                !this.attackedBy(home + 1, enemy) && !this.attackedBy(home + 2, enemy)) push(s, home + 2, piece, CASTLE_K);
            if (this.castle.includes(white ? 'Q' : 'q') && !b[home - 1] && !b[home - 2] && !b[home - 3] && b[home - 4] === (white ? 'R' : 'r') &&
                !this.attackedBy(home - 1, enemy) && !this.attackedBy(home - 2, enemy)) push(s, home - 2, piece, CASTLE_Q);
          }
        }
      } else {
        const dirs = t === 'B' ? DIAG : t === 'R' ? ORTHO : DIAG.concat(ORTHO);
        for (const [df, dr] of dirs) {
          let nf = f + df, nr = r + dr;
          while (nf >= 0 && nf < 8 && nr >= 0 && nr < 8) {
            const to = nr * 8 + nf, q = b[to];
            if (!q) push(s, to, piece);
            else { if (isWhitePiece(q) !== white) push(s, to, piece); break; }
            nf += df; nr += dr;
          }
        }
      }
    }
    return out;
  }

  /* the position after move m (no legality check) */
  apply(m) {
    const q = this.clone(), b = q.b, white = this.turn === 'w';
    b[m.from] = ''; b[m.to] = m.promo ? (white ? m.promo.toUpperCase() : m.promo) : m.piece;
    if (m.kind === EP) b[m.to + (white ? -8 : 8)] = '';
    else if (m.kind === CASTLE_K) { b[m.to - 1] = b[m.to + 1]; b[m.to + 1] = ''; }
    else if (m.kind === CASTLE_Q) { b[m.to + 1] = b[m.to - 2]; b[m.to - 2] = ''; }
    let c = q.castle;
    const drop = (ch) => { c = c.replace(ch, ''); };
    if (m.piece === 'K') { drop('K'); drop('Q'); } else if (m.piece === 'k') { drop('k'); drop('q'); }
    for (const sq of [m.from, m.to]) {
      if (sq === 0) drop('Q'); else if (sq === 7) drop('K'); else if (sq === 56) drop('q'); else if (sq === 63) drop('k');
    }
    q.castle = c;
    q.ep = m.kind === DOUBLE ? (m.from + m.to) >> 1 : -1;
    q.half = (m.piece.toUpperCase() === 'P' || m.cap) ? 0 : this.half + 1;
    if (!white) q.full = this.full + 1;
    q.turn = white ? 'b' : 'w';
    return q;
  }

  legalMoves() {
    const out = [], me = this.turn, enemy = me === 'w' ? 'b' : 'w';
    for (const m of this.pseudoMoves()) {
      const q = this.apply(m);
      if (!q.attackedBy(q.kingSq(me), enemy)) out.push(m);
    }
    return out;
  }

  /* repetition key: placement, side to move, castling rights and an en passant square only if a legal capture exists */
  key() {
    let epPart = '-';
    if (this.ep >= 0 && this.legalMoves().some((m) => m.kind === EP)) epPart = sqName(this.ep);
    return this.fen().split(' ').slice(0, 3).join(' ') + ' ' + epPart;
  }
}

/* ---- notation -------------------------------------------------------------------------------------------------------------------- */
export function moveUci(m) { return sqName(m.from) + sqName(m.to) + m.promo; }

export function parseUci(pos, str, legal = pos.legalMoves()) {
  if (!/^[a-h][1-8][a-h][1-8][qrbn]?$/.test(str)) return null;
  const from = sqFromName(str.slice(0, 2)), to = sqFromName(str.slice(2, 4)), promo = str[4] || '';
  return legal.find((m) => m.from === from && m.to === to && m.promo === promo) || null;
}

export function moveSan(pos, m, legal = pos.legalMoves()) {
  let s;
  const t = m.piece.toUpperCase();
  if (m.kind === CASTLE_K) s = 'O-O';
  else if (m.kind === CASTLE_Q) s = 'O-O-O';
  else if (t === 'P') {
    s = (m.cap ? FILES[m.from & 7] + 'x' : '') + sqName(m.to) + (m.promo ? '=' + m.promo.toUpperCase() : '');
  } else {
    s = t;
    const rivals = legal.filter((o) => o.from !== m.from && o.piece === m.piece && o.to === m.to);
    if (rivals.length) {
      const sameFile = rivals.some((o) => (o.from & 7) === (m.from & 7)), sameRank = rivals.some((o) => (o.from >> 3) === (m.from >> 3));
      if (!sameFile) s += FILES[m.from & 7]; else if (!sameRank) s += (m.from >> 3) + 1; else s += sqName(m.from);
    }
    s += (m.cap ? 'x' : '') + sqName(m.to);
  }
  const after = pos.apply(m);
  if (after.inCheck()) s += after.legalMoves().length ? '+' : '#';
  return s;
}

/* ---- game status -------------------------------------------------------------------------------------------------------------------- */
export function insufficientMaterial(pos) {
  const minors = [];
  for (let s = 0; s < 64; s++) {
    const p = pos.b[s];
    if (!p || p === 'K' || p === 'k') continue;
    const t = p.toUpperCase();
    if (t === 'P' || t === 'R' || t === 'Q') return false;
    minors.push({ t, sq: s });
  }
  if (minors.length <= 1) return true;                                    // K v K, K+B v K, K+N v K
  if (minors.every((x) => x.t === 'B')) {                                 // bishops that all stand on the same colour of square
    const col = (s) => ((s & 7) + (s >> 3)) & 1;
    return minors.every((x) => col(x.sq) === col(minors[0].sq));
  }
  return false;
}

/* keys: the repetition keys of every position of the game so far, the current one last */
export function gameStatus(pos, keys = []) {
  const moves = pos.legalMoves();
  if (!moves.length) {
    if (pos.inCheck()) return { over: true, result: pos.turn === 'w' ? '0-1' : '1-0', reason: 'checkmate' };
    return { over: true, result: '1/2-1/2', reason: 'stalemate' };
  }
  if (insufficientMaterial(pos)) return { over: true, result: '1/2-1/2', reason: 'insufficient material' };
  if (pos.half >= 100) return { over: true, result: '1/2-1/2', reason: 'fifty-move rule' };
  if (keys.length) {
    const cur = keys[keys.length - 1];
    let n = 0;
    for (const k of keys) if (k === cur) n++;
    if (n >= 3) return { over: true, result: '1/2-1/2', reason: 'threefold repetition' };
  }
  return { over: false };
}

export function perft(pos, depth) {
  if (depth === 0) return 1;
  const moves = pos.legalMoves();
  if (depth === 1) return moves.length;
  let n = 0;
  for (const m of moves) n += perft(pos.apply(m), depth - 1);
  return n;
}

/* ---- PGN -------------------------------------------------------------------------------------------------------------------------------- */
export function buildPgn(startFen, ucis, result, headers = {}) {
  const h = Object.assign({ Event: 'Casual game', Site: 'Kestrel play page', Result: result || '*' }, headers);
  if (startFen !== START_FEN) { h.SetUp = '1'; h.FEN = startFen; }
  let pos = Position.fromFEN(startFen), text = '';
  const words = [];
  for (const u of ucis) {
    const m = parseUci(pos, u);
    if (!m) break;
    const san = moveSan(pos, m);
    if (pos.turn === 'w') words.push(pos.full + '. ' + san);
    else words.push((words.length === 0 ? pos.full + '... ' : '') + san);
    pos = pos.apply(m);
  }
  for (const [k, v] of Object.entries(h)) text += '[' + k + ' "' + String(v).replace(/(["\\])/g, '\\$1') + '"]\n';
  const body = words.join(' ') + (words.length ? ' ' : '') + (result || '*');
  // wrap at 80 columns
  const lines = []; let line = '';
  for (const w of body.split(' ')) { if ((line + ' ' + w).trim().length > 80) { lines.push(line); line = w; } else line = (line + ' ' + w).trim(); }
  if (line) lines.push(line);
  return text + '\n' + lines.join('\n') + '\n';
}
