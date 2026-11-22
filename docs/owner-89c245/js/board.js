/* Kestrel Arena - SVG chess board. It only draws positions (FEN); all chess rules live on the server. */
(function () {
  const A = window.A;
  const GLYPH = { k: '♚', q: '♛', r: '♜', b: '♝', n: '♞', p: '♟︎' };
  const FILES = 'abcdefgh';

  class Board {
    constructor(o) {
      o = o || {};
      this.size = o.size || 400;
      this.flipped = !!o.flipped;
      this.interactive = !!o.interactive;
      this.coords = o.coords !== false;
      this.onSquare = o.onSquare || null;
      this.fen = null;
      const svg = (this.svg = A.svg('svg', { class: 'board' + (this.interactive ? '' : ' static'), viewBox: '0 0 800 800' }));
      this.gSq = A.svg('g'); this.gHl = A.svg('g'); this.gCo = A.svg('g'); this.gTg = A.svg('g'); this.gPc = A.svg('g'); this.gHit = A.svg('g');
      [this.gSq, this.gHl, this.gCo, this.gTg, this.gPc, this.gHit].forEach((g) => svg.appendChild(g));
      this.sq = {}; this.pc = {};
      for (let f = 0; f < 8; f++) {
        for (let r = 0; r < 8; r++) {
          const name = FILES[f] + (r + 1), light = (f + r) % 2 === 1;
          this.sq[name] = A.svg('rect', { width: 100, height: 100, class: light ? 'sq-l' : 'sq-d' });
          this.gSq.appendChild(this.sq[name]);
          this.pc[name] = A.svg('text', { 'font-size': 78, 'stroke-width': 3 });
          this.gPc.appendChild(this.pc[name]);
          if (this.interactive) {
            const hit = A.svg('rect', { width: 100, height: 100, class: 'hit', dataset: { sq: name } });
            hit.addEventListener('pointerdown', (e) => { if (this.onSquare) this.onSquare(name, e); });
            this.gHit.appendChild(hit);
            this.hit = this.hit || {};
            this.hit[name] = hit;
          }
        }
      }
      this.wrap = A.h('div', { class: 'board-wrap' }, svg);
      this.el = this.wrap;
      this.setSize(this.size);
      this._layout();
      this.setPosition('rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1');
    }

    xy(name) {
      const f = FILES.indexOf(name[0]), r = parseInt(name[1], 10) - 1;
      return this.flipped ? [(7 - f) * 100, r * 100] : [f * 100, (7 - r) * 100];
    }

    _layout() {
      for (const name in this.sq) {
        const [x, y] = this.xy(name);
        for (const m of [this.sq, this.hit || {}]) if (m[name]) { m[name].setAttribute('x', x); m[name].setAttribute('y', y); }
        const p = this.pc[name];
        p.setAttribute('x', x + 50); p.setAttribute('y', y + 50); p.setAttribute('dy', '.34em');
      }
      A.clear(this.gCo);
      if (this.coords) {
        for (let i = 0; i < 8; i++) {
          const fname = FILES[i] + (this.flipped ? '8' : '1'), rname = (this.flipped ? 'h' : 'a') + (i + 1);
          const [fx, fy] = this.xy(fname), [rx, ry] = this.xy(rname);
          const lightF = this.sq[fname].getAttribute('class') === 'sq-l', lightR = this.sq[rname].getAttribute('class') === 'sq-l';
          this.gCo.appendChild(A.svg('text', { x: fx + 88, y: fy + 92, 'font-size': 20, 'font-weight': 700, 'text-anchor': 'end', class: 'coord ' + (lightF ? 'on-l' : 'on-d') }, FILES[i]));
          this.gCo.appendChild(A.svg('text', { x: rx + 6, y: ry + 22, 'font-size': 20, 'font-weight': 700, class: 'coord ' + (lightR ? 'on-l' : 'on-d') }, String(i + 1)));
        }
      }
    }

    setSize(px) {
      this.size = px;
      this.svg.setAttribute('width', px); this.svg.setAttribute('height', px);
    }

    flip(on) {
      this.flipped = on === undefined ? !this.flipped : !!on;
      this._layout();
      if (this.fen) this.setPosition(this.fen, this.last_opts);
    }

    /** o: {last:[from,to], check:'e1', selected:'e2', targets:[{sq,capture}]} */
    setPosition(fen, o) {
      o = o || {};
      this.fen = fen; this.last_opts = o;
      const rows = fen.split(' ')[0].split('/');
      const occ = {};
      for (let i = 0; i < 8; i++) {
        let f = 0;
        for (const ch of rows[i]) {
          if (ch >= '1' && ch <= '8') f += +ch;
          else { occ[FILES[f] + (8 - i)] = ch; f++; }
        }
      }
      for (const name in this.pc) {
        const ch = occ[name], t = this.pc[name];
        if (!ch) { if (t.textContent) { t.textContent = ''; t.setAttribute('class', ''); } continue; }
        const white = ch === ch.toUpperCase();
        t.textContent = GLYPH[ch.toLowerCase()];
        t.setAttribute('class', 'piece ' + (white ? 'w' : 'b'));
      }
      this.occ = occ;
      A.clear(this.gHl); A.clear(this.gTg);
      const hl = (sq, cls) => { if (!sq || !this.sq[sq]) return; const [x, y] = this.xy(sq); this.gHl.appendChild(A.svg('rect', { x, y, width: 100, height: 100, class: cls })); };
      if (o.last) { hl(o.last[0], 'hl-last'); hl(o.last[1], 'hl-last'); }
      hl(o.check, 'hl-check');
      hl(o.selected, 'hl-sel');
      for (const t of o.targets || []) {
        const [x, y] = this.xy(t.sq);
        this.gTg.appendChild(t.capture ? A.svg('circle', { cx: x + 50, cy: y + 50, r: 44, class: 'ring' }) : A.svg('circle', { cx: x + 50, cy: y + 50, r: 16, class: 'dot' }));
      }
    }

    pieceAt(sq) { return (this.occ || {})[sq] || null; }
  }
  Board.glyph = (type) => GLYPH[type.toLowerCase()];
  A.Board = Board;
})();
