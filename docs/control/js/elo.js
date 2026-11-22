/* Kestrel Arena - standings, Elo (Bradley-Terry fit with error bars), tie-breaks */
(function () {
  const A = window.A;
  const LN10_400 = Math.LN10 / 400;
  const ELO_PER_LN = 400 / Math.LN10;

  function points(result, whiteIsA) {
    if (result === '1/2-1/2') return [0.5, 0.5];
    if (result === '1-0') return whiteIsA ? [1, 0] : [0, 1];
    if (result === '0-1') return whiteIsA ? [0, 1] : [1, 0];
    return [0, 0];
  }

  /**
   * engines: [{id, name, elo}], games: [{white, black, result}], opts: {extraPoints: {id: pts}}
   * Returns rows sorted by points: {id, name, cfgElo, games, w, d, l, pts, score, sb, elo (relative; anchored to the field), err, rank}
   */
  function standings(engines, games, opts) {
    opts = opts || {};
    const idx = new Map(engines.map((e, i) => [e.id, i]));
    const n = engines.length;
    const rows = engines.map((e) => ({ id: e.id, name: e.name, cfgElo: e.elo || null, games: 0, w: 0, d: 0, l: 0, pts: 0, sb: 0, elo: 0, err: null, rank: 0 }));
    const S = Array.from({ length: n }, () => new Float64Array(n));   // points of i against j
    const N = Array.from({ length: n }, () => new Float64Array(n));   // games of i against j
    const W = Array.from({ length: n }, () => new Float64Array(n)), D = Array.from({ length: n }, () => new Float64Array(n)), L = Array.from({ length: n }, () => new Float64Array(n));
    for (const g of games) {
      const a = idx.get(g.white), b = idx.get(g.black);
      if (a === undefined || b === undefined || !g.result || g.result === '*') continue;
      const [pa, pb] = points(g.result, true);
      S[a][b] += pa; S[b][a] += pb; N[a][b]++; N[b][a]++;
      rows[a].games++; rows[b].games++; rows[a].pts += pa; rows[b].pts += pb;
      if (pa === 1) { rows[a].w++; rows[b].l++; W[a][b]++; L[b][a]++; }
      else if (pb === 1) { rows[b].w++; rows[a].l++; W[b][a]++; L[a][b]++; }
      else { rows[a].d++; rows[b].d++; D[a][b]++; D[b][a]++; }
    }
    for (const id in (opts.extraPoints || {})) { const i = idx.get(id); if (i !== undefined) rows[i].pts += opts.extraPoints[id]; }
    for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) rows[i].sb += S[i][j] * rows[j].pts;
    for (const r of rows) r.score = r.games ? (r.pts - (opts.extraPoints && opts.extraPoints[r.id] || 0)) / r.games : null;

    // Bradley-Terry maximum likelihood (MM iteration) with a weak prior: two virtual draws against an average opponent
    const p = new Float64Array(n).fill(1);
    const Wt = new Float64Array(n);
    for (let i = 0; i < n; i++) { Wt[i] = 1; for (let j = 0; j < n; j++) Wt[i] += S[i][j]; }
    for (let it = 0; it < 200; it++) {
      let delta = 0;
      for (let i = 0; i < n; i++) {
        let den = 2 / (p[i] + 1);
        for (let j = 0; j < n; j++) if (N[i][j]) den += N[i][j] / (p[i] + p[j]);
        const np = Wt[i] / den;
        delta = Math.max(delta, Math.abs(Math.log(np / p[i])));
        p[i] = np;
      }
      let gm = 0; for (let i = 0; i < n; i++) gm += Math.log(p[i]); gm = Math.exp(gm / n);
      for (let i = 0; i < n; i++) p[i] /= gm;
      if (delta < 1e-9) break;
    }
    const anchors = rows.filter((r) => r.cfgElo).map((r) => r.cfgElo);
    const anchor = anchors.length >= 2 && anchors.length === n ? anchors.reduce((a, b) => a + b, 0) / n : 0;
    for (let i = 0; i < n; i++) {
      rows[i].elo = anchor + ELO_PER_LN * Math.log(p[i]);
      let info = 0, v = 0;
      for (let j = 0; j < n; j++) {
        if (!N[i][j]) continue;
        const P = p[i] / (p[i] + p[j]);
        info += N[i][j] * P * (1 - P);
        v += W[i][j] * (1 - P) * (1 - P) + D[i][j] * (0.5 - P) * (0.5 - P) + L[i][j] * P * P;
      }
      rows[i].err = info > 0 && rows[i].games >= 2 ? 1.96 * ELO_PER_LN * Math.sqrt(Math.max(v, 1e-9)) / info : null;
    }
    rows.sort((a, b) => b.pts - a.pts || b.sb - a.sb || b.elo - a.elo);
    rows.forEach((r, i) => { r.rank = i + 1; });
    return { rows, S, N, idx };
  }

  // likelihood of superiority of A over B from a head-to-head (draws ignored)
  function los(w, l) {
    if (w + l === 0) return 0.5;
    const z = (w - l) / Math.sqrt(w + l);
    return 0.5 * (1 + erf(z / Math.SQRT2));
  }
  function erf(x) {
    const s = x < 0 ? -1 : 1; x = Math.abs(x);
    const t = 1 / (1 + 0.3275911 * x);
    const y = 1 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * Math.exp(-x * x);
    return s * y;
  }
  function expected(d) { return 1 / (1 + Math.pow(10, -d / 400)); }
  function eloFromScore(s) { s = Math.min(Math.max(s, 1e-4), 1 - 1e-4); return -400 * Math.log10(1 / s - 1); }

  A.elo = { standings, points, los, erf, expected, eloFromScore };
})();
