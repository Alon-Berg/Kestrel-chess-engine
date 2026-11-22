/* Kestrel Arena - Live page: small helpers. The page does not load the app's util.js; board.js and elo.js only need window.A with h / svg / clear.
   Also the little chess code the page needs: positions are rebuilt from the UCI move lists (placement only, no rules - the server has checked every move). */
(function () {
  const A = (window.A = window.A || {});
  const SVGNS = 'http://www.w3.org/2000/svg';

  // ---- DOM ---------------------------------------------------------------------------------------------------------------
  function build(el, props, kids) {
    for (const k in props || {}) {
      const v = props[k];
      if (v === undefined || v === null || v === false) continue;
      if (k === 'class') el.setAttribute('class', v);
      else if (k === 'style') { if (typeof v === 'string') el.setAttribute('style', v); else Object.assign(el.style, v); }
      else if (k === 'text') el.textContent = v;
      else if (k === 'dataset') Object.assign(el.dataset, v);
      else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
      else if (!(el instanceof SVGElement) && (k === 'value' || k === 'checked' || k === 'disabled' || k === 'selected')) el[k] = v;
      else el.setAttribute(k, v === true ? '' : v);
    }
    (function add(list) {
      for (const c of list) {
        if (c === null || c === undefined || c === false) continue;
        if (Array.isArray(c)) add(c);
        else el.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
      }
    })(kids);
    return el;
  }
  A.h = (tag, props, ...kids) => build(document.createElement(tag), props, kids);
  A.svg = (tag, props, ...kids) => build(document.createElementNS(SVGNS, tag), props, kids);
  A.clear = (el) => { while (el.firstChild) el.removeChild(el.firstChild); return el; };
  A.token = ((typeof document !== 'undefined' && document.querySelector('meta[name=arena-token]')) || {}).content || '';

  // ---- formatting --------------------------------------------------------------------------------------------------------
  const F = (A.fmt = {});
  F.dur = (s) => {
    s = Math.max(0, Math.round(s));
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
    if (h) return h + 'h ' + String(m).padStart(2, '0') + 'm';
    if (m) return m + 'm ' + String(x).padStart(2, '0') + 's';
    return x + 's';
  };
  F.clock = (ms) => {
    if (ms === null || ms === undefined) return '-';
    ms = Math.max(0, ms);
    const s = ms / 1000;
    if (s < 10) return '0:0' + s.toFixed(1);
    const hh = Math.floor(s / 3600), mm = Math.floor((s % 3600) / 60), ss = Math.floor(s % 60);
    return hh ? hh + ':' + String(mm).padStart(2, '0') + ':' + String(ss).padStart(2, '0') : mm + ':' + String(ss).padStart(2, '0');
  };
  F.eval = (cp) => {
    if (cp === null || cp === undefined) return '';
    if (Math.abs(cp) >= 29000) return (cp > 0 ? '+' : '-') + 'M' + (30000 - Math.abs(cp));
    return (cp > 0 ? '+' : '') + (cp / 100).toFixed(2);
  };
  F.num = (n) => (n === null || n === undefined ? '-' : n >= 1e9 ? (n / 1e9).toFixed(2) + 'G' : n >= 1e6 ? (n / 1e6).toFixed(2) + 'M' : n >= 1e3 ? (n / 1e3).toFixed(0) + 'k' : String(n));
  F.pct = (x, d) => (x === null || x === undefined || isNaN(x) ? '-' : (100 * x).toFixed(d === undefined ? 1 : d) + '%');
  F.pts = (x) => String(Math.round(x * 2) / 2);
  F.ms = (ms) => (ms === null || ms === undefined ? '-' : ms < 1000 ? Math.round(ms) + ' ms' : (ms / 1000).toFixed(ms < 10000 ? 2 : 1) + ' s');

  // ---- positions from move lists -----------------------------------------------------------------------------------------
  const START = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
  const C = (A.chess = {});
  C.START = START;
  C.parseFen = (fen) => {
    const p = (fen || START).split(' ');
    const cells = new Array(64).fill('');                          // index = rank * 8 + file, rank 0 = rank 1
    p[0].split('/').forEach((row, i) => {
      let f = 0;
      for (const ch of row) { if (ch >= '1' && ch <= '8') f += +ch; else { cells[(7 - i) * 8 + f] = ch; f++; } }
    });
    return { cells, turn: p[1] || 'w', full: parseInt(p[5], 10) || 1 };
  };
  C.placement = (cells) => {
    const rows = [];
    for (let r = 7; r >= 0; r--) {
      let e = 0, row = '';
      for (let f = 0; f < 8; f++) { const c = cells[r * 8 + f]; if (!c) e++; else { if (e) { row += e; e = 0; } row += c; } }
      rows.push(e ? row + e : row);
    }
    return rows.join('/');
  };
  C.applyUci = (cells, u) => {                                     // changes `cells` in place
    const f1 = u.charCodeAt(0) - 97, r1 = +u[1] - 1, f2 = u.charCodeAt(2) - 97, r2 = +u[3] - 1;
    const a = r1 * 8 + f1, b = r2 * 8 + f2, p = cells[a];
    if (!p) return;
    const lp = p.toLowerCase();
    if (lp === 'p' && f1 !== f2 && !cells[b]) cells[r1 * 8 + f2] = '';                                   // en passant
    if (lp === 'k' && Math.abs(f2 - f1) === 2) { const rf = f2 > f1 ? 7 : 0, rt = f2 > f1 ? 5 : 3; cells[r1 * 8 + rt] = cells[r1 * 8 + rf]; cells[r1 * 8 + rf] = ''; }   // castling
    cells[b] = u.length > 4 ? (p === p.toUpperCase() ? u[4].toUpperCase() : u[4].toLowerCase()) : p;      // promotion
    cells[a] = '';
  };
  /** g: {start_fen, uci:[...]}; keeps g._pl = the piece placement after 0..uci.length moves (extended as moves arrive). */
  C.ensure = (g) => {
    if (!g._pl || g._start !== g.start_fen || g._pl.length > g.uci.length + 1) {
      const p = C.parseFen(g.start_fen);
      g._cells = p.cells; g._turn0 = p.turn; g._full0 = p.full; g._start = g.start_fen; g._pl = [C.placement(p.cells)];
    }
    while (g._pl.length <= g.uci.length) { C.applyUci(g._cells, g.uci[g._pl.length - 1]); g._pl.push(C.placement(g._cells)); }
    return g;
  };
  C.fenOf = (pl) => pl + ' w - - 0 1';                             // (the board only reads the placement)
  C.find = (pl, ch) => {                                           // the square of a piece in a placement string (the king in check), or null
    const rows = pl.split('/');
    for (let i = 0; i < 8; i++) {
      let f = 0;
      for (const c of rows[i]) { if (c >= '1' && c <= '8') f += +c; else { if (c === ch) return 'abcdefgh'[f] + (8 - i); f++; } }
    }
    return null;
  };
  /** The colour (0 white, 1 black) that played ply i (0-based), given the colour to move at the start. */
  C.colorOf = (g, i) => ((g._turn0 === 'b') ? (i % 2 === 0 ? 1 : 0) : (i % 2 === 0 ? 0 : 1));

  if (typeof module === 'object' && module.exports) module.exports = A;       // (tests run it under node / deno)
})();
