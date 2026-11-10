/* SVG chess board: draws a Position, lets the user pick up pieces (click, click or drag and drop) and reports the chosen from / to squares.
   All rules live in rules.js; this file only draws and translates pointer input. */
import { FILES, sqName } from './rules.js';

const NS = 'http://www.w3.org/2000/svg';
const GLYPH = { k: '♚', q: '♛', r: '♜', b: '♝', n: '♞', p: '♟︎' };

function el(name, attrs = {}, text) {
  const e = document.createElementNS(NS, name);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (text !== undefined) e.textContent = text;
  return e;
}

export function pieceGlyph(p) { return GLYPH[p.toLowerCase()]; }

export class BoardView {
  /* opts: canMoveFrom(sq) -> bool, targetsFrom(sq) -> [{to, capture}], onMove(from, to) */
  constructor(container, opts) {
    this.opts = opts;
    this.pos = null; this.orient = 'w';
    this.last = null; this.checkSq = -1;
    this.selected = -1; this.targets = new Map();
    this.drag = null;
    this.svg = el('svg', { viewBox: '0 0 800 800', class: 'board', role: 'img', 'aria-label': 'Chess board' });
    this.gSq = el('g'); this.gHl = el('g'); this.gCo = el('g'); this.gTg = el('g'); this.gPc = el('g'); this.gFloat = el('g');
    for (const g of [this.gSq, this.gHl, this.gCo, this.gTg, this.gPc, this.gFloat]) this.svg.appendChild(g);
    this.sqEls = [];
    for (let s = 0; s < 64; s++) {
      const r = el('rect', { width: 100, height: 100, class: ((s & 7) + (s >> 3)) & 1 ? 'sq-l' : 'sq-d' });
      this.sqEls.push(r); this.gSq.appendChild(r);
    }
    container.appendChild(this.svg);
    this.svg.addEventListener('pointerdown', (e) => this._down(e));
    this.svg.addEventListener('pointermove', (e) => this._move(e));
    this.svg.addEventListener('pointerup', (e) => this._up(e));
    this.svg.addEventListener('pointercancel', () => this._cancelDrag());
    this._layout();
  }

  /* screen position of a square in board units */
  _xy(s) {
    const f = s & 7, r = s >> 3;
    return this.orient === 'w' ? [f * 100, (7 - r) * 100] : [(7 - f) * 100, r * 100];
  }

  _layout() {
    for (let s = 0; s < 64; s++) { const [x, y] = this._xy(s); this.sqEls[s].setAttribute('x', x); this.sqEls[s].setAttribute('y', y); }
    this.gCo.textContent = '';
    for (let i = 0; i < 8; i++) {
      // file letters along the bottom row, rank numbers along the left column
      const fileSq = this.orient === 'w' ? i : 56 + i, rankSq = this.orient === 'w' ? i * 8 : i * 8 + 7;
      const [fx, fy] = this._xy(fileSq), [rx, ry] = this._xy(rankSq);
      const lightF = ((fileSq & 7) + (fileSq >> 3)) & 1, lightR = ((rankSq & 7) + (rankSq >> 3)) & 1;
      this.gCo.appendChild(el('text', { x: fx + 90, y: fy + 92, class: 'coord ' + (lightF ? 'on-l' : 'on-d'), 'text-anchor': 'end' }, FILES[fileSq & 7]));
      this.gCo.appendChild(el('text', { x: rx + 8, y: ry + 24, class: 'coord ' + (lightR ? 'on-l' : 'on-d') }, String((rankSq >> 3) + 1)));
    }
  }

  setOrientation(o) { if (o !== this.orient) { this.orient = o; this._layout(); this.render(); } }

  /* pos: Position; opts.last: {from, to}; opts.check: king square or -1 */
  setPosition(pos, o = {}) {
    this.pos = pos; this.last = o.last || null; this.checkSq = o.check === undefined ? -1 : o.check;
    this.clearSelection(false);
    this.render();
    if (o.animate && this.last) this._slide(this.last);
  }

  _slide(m) {
    const node = this.pieceEls && this.pieceEls[m.to];
    if (!node || !node.animate) return;
    const [x0, y0] = this._xy(m.from), [x1, y1] = this._xy(m.to);
    node.animate([{ transform: 'translate(' + (x0 - x1) + 'px,' + (y0 - y1) + 'px)' }, { transform: 'translate(0,0)' }], { duration: 140, easing: 'ease-out' });
  }

  render() {
    this.gHl.textContent = ''; this.gPc.textContent = ''; this.gTg.textContent = '';
    this.pieceEls = {};
    const hl = (s, cls) => { const [x, y] = this._xy(s); this.gHl.appendChild(el('rect', { x, y, width: 100, height: 100, class: cls })); };
    if (this.last) { hl(this.last.from, 'hl-last'); hl(this.last.to, 'hl-last'); }
    if (this.checkSq >= 0) hl(this.checkSq, 'hl-check');
    if (this.selected >= 0) hl(this.selected, 'hl-sel');
    if (!this.pos) return;
    for (let s = 0; s < 64; s++) {
      const p = this.pos.b[s];
      if (!p) continue;
      const [x, y] = this._xy(s);
      const white = p === p.toUpperCase();
      const t = el('text', { x: x + 50, y: y + 76, class: 'piece ' + (white ? 'w' : 'b') + (this.drag && this.drag.started && this.drag.from === s ? ' lifted' : '') }, GLYPH[p.toLowerCase()]);
      this.gPc.appendChild(t); this.pieceEls[s] = t;
    }
    for (const [s, capture] of this.targets) {
      const [x, y] = this._xy(s);
      this.gTg.appendChild(capture ? el('circle', { cx: x + 50, cy: y + 50, r: 44, class: 'ring' }) : el('circle', { cx: x + 50, cy: y + 50, r: 15, class: 'dot' }));
    }
  }

  select(s) {
    this.selected = s; this.targets = new Map();
    for (const t of this.opts.targetsFrom(s)) this.targets.set(t.to, t.capture);
    this.render();
  }

  clearSelection(redraw = true) { this.selected = -1; this.targets = new Map(); if (redraw) this.render(); }

  _squareAt(e) {
    const r = this.svg.getBoundingClientRect();
    const x = (e.clientX - r.left) / r.width * 8, y = (e.clientY - r.top) / r.height * 8;
    if (x < 0 || y < 0 || x >= 8 || y >= 8) return -1;
    let f = Math.floor(x), rank = 7 - Math.floor(y);
    if (this.orient === 'b') { f = 7 - f; rank = 7 - rank; }
    return rank * 8 + f;
  }

  _down(e) {
    if (e.button !== undefined && e.button > 0) return;
    const s = this._squareAt(e);
    if (s < 0) return;
    if (this.selected >= 0 && this.targets.has(s)) {
      const from = this.selected; this.clearSelection(); this.opts.onMove(from, s); return;
    }
    if (this.opts.canMoveFrom(s)) {
      const wasSelected = this.selected === s;
      this.select(s);
      this.drag = { from: s, started: false, sx: e.clientX, sy: e.clientY, id: e.pointerId, wasSelected };
      try { this.svg.setPointerCapture(e.pointerId); } catch (err) { /* not essential */ }
    } else {
      this.clearSelection();
    }
  }

  _move(e) {
    const d = this.drag;
    if (!d || e.pointerId !== d.id) return;
    if (!d.started) {
      if (Math.hypot(e.clientX - d.sx, e.clientY - d.sy) < 6) return;
      d.started = true;
      const p = this.pos.b[d.from];
      d.float = el('text', { class: 'piece float ' + (p === p.toUpperCase() ? 'w' : 'b') }, GLYPH[p.toLowerCase()]);
      this.gFloat.appendChild(d.float);
      this.render();
    }
    const r = this.svg.getBoundingClientRect();
    d.float.setAttribute('x', (e.clientX - r.left) / r.width * 800);
    d.float.setAttribute('y', (e.clientY - r.top) / r.height * 800 + 26);
  }

  _up(e) {
    const d = this.drag;
    if (!d || e.pointerId !== d.id) return;
    this.drag = null;
    try { this.svg.releasePointerCapture(e.pointerId); } catch (err) { /* ignore */ }
    this.gFloat.textContent = '';
    if (d.started) {
      const s = this._squareAt(e);
      if (s >= 0 && this.targets.has(s)) { const from = d.from; this.clearSelection(); this.opts.onMove(from, s); return; }
      this.render();
    } else if (d.wasSelected) {
      this.clearSelection();                      // a second click on the selected piece puts it down
    }
  }

  _cancelDrag() {
    if (!this.drag) return;
    this.drag = null; this.gFloat.textContent = ''; this.render();
  }
}
