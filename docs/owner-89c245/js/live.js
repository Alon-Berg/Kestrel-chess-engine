/* Kestrel Arena - Live page (TCEC / CCC style). One page, three sources of data:
     the Arena's stream (/live/api/stream, the default),  a snapshot file (?src=<url>[&refresh=<seconds>]),  or a snapshot inside the page (window.__BROADCAST__, an exported broadcast file).
   The data is a state message ({t:'state'}) and, for the stream, small updates ({t:'tick'}); see arena/live.py.  The page only draws: all chess rules are on the server. */
(function () {
  'use strict';
  const A = window.A, h = A.h, svg = A.svg, fmt = A.fmt, C = A.chess;
  const $ = (s, r) => (r || document).querySelector(s);
  const S = {
    src: null, conn: false, frozen: false, meta: null, metaAt: 0, engines: [], names: {}, games: [], gmap: new Map(), active: new Map(), recs: new Map(), recsRaw: null, pending: new Set(), missing: new Set(),
    structure: null, sel: null, follow: true, ply: null, flipped: false, holdUntil: 0, tab: 'standings', filter: '', rows: 300, taken: null,
  };
  const U = { minis: new Map() };                 // DOM references
  const D = { head: true, main: true, minis: true, side: true };
  const FORMAT = { roundrobin: 'Round robin', gauntlet: 'Gauntlet', swiss: 'Swiss', knockout: 'Knockout' };
  const fname = (m) => (m.format === 'elimination' ? ((m.params || {}).losses || 2) + '-loss elimination' : FORMAT[m.format] || m.format);
  const nm = (id) => (id ? S.names[id] || id : 'bye');
  const rnd = (r) => (r === null || r === undefined ? '' : r >= 1000 ? Math.floor(r / 1000) + 1 + '.' + (r % 1000) : String(r));
  const lossTxt = (n) => n + (n === 1 ? ' loss' : ' losses');
  const isBracket = () => !!S.meta && (S.meta.format === 'knockout' || S.meta.format === 'elimination');

  // ====================================================================================================== data sources
  function streamSource() {
    let es = null;
    const src = {
      kind: 'live',
      start(on, conn) {
        es = new EventSource('/live/api/stream');
        es.onopen = () => conn(true);
        es.onmessage = (e) => { try { on(JSON.parse(e.data)); } catch (err) { console.error(err); } };
        es.onerror = () => conn(false);              // EventSource connects again by itself; the server then starts with the whole state
      },
      resync() { if (es) es.close(); this.start(...this.args); },
      game: (gid) => fetch('/live/api/game/' + gid).then((r) => (r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)))),
    };
    return src;
  }
  function snapshotSource(get, refresh) {            // get(): Promise<snapshot object>
    let timer = null;
    return {
      kind: 'snapshot',
      start(on, conn) {
        const load = () => get().then((s) => { if (s.snapshot === undefined) s.snapshot = true; on(Object.assign({ t: 'state' }, s)); conn(true); }).catch((e) => { console.error('live page: the snapshot could not be read', e); conn(false); });
        load();
        if (refresh > 0) timer = setInterval(load, refresh * 1000);
      },
      resync() {},
      game: (gid) => (S.recsRaw && S.recsRaw[gid] ? Promise.resolve(S.recsRaw[gid]) : fetch('/live/api/game/' + gid).then((r) => (r.ok ? r.json() : null)).catch(() => null)),     // (a followed folder keeps only the latest games in the snapshot)
    };
  }
  function localSource(cfg) {                        // the Arena on this PC, read through its local control server with the pairing token; cfg = {base, token}
    let ctl = null, stopped = false, fast = false;
    const lf = (path, extra) => A.lfetch(cfg.base + path, Object.assign({ headers: { Authorization: 'Bearer ' + cfg.token }, cache: 'no-store' }, extra));
    return {
      kind: 'live',
      start(on, conn) {
        (async () => {
          while (!stopped) {
            ctl = new AbortController();
            try {                                    // the stream is read with fetch (an EventSource cannot send the Authorization header)
              const r = await lf('/control/api/live/stream', { signal: ctl.signal });
              if (r.status === 401 || r.status === 403) { stopped = true; conn(false); return; }          // refused: trying again would only count as more wrong tokens
              if (!r.ok) throw new Error('HTTP ' + r.status);
              conn(true);
              const rd = r.body.getReader(), dec = new TextDecoder();
              let buf = '';
              for (;;) {
                const { value, done } = await rd.read();
                if (done) break;
                buf += dec.decode(value, { stream: true });
                let i;
                while ((i = buf.indexOf('\n\n')) >= 0) {
                  const ev = buf.slice(0, i); buf = buf.slice(i + 2);
                  for (const ln of ev.split('\n')) if (ln.startsWith('data: ')) { try { on(JSON.parse(ln.slice(6))); } catch (e) { console.error(e); } }
                }
              }
            } catch (e) { /* closed, refused or aborted: connect again below (the server then sends the whole state first) */ }
            if (stopped) return;
            conn(false);
            if (fast) fast = false; else await new Promise((res) => setTimeout(res, 2000));
          }
        })();
      },
      resync() { fast = true; if (ctl) ctl.abort(); },
      game: (gid) => lf('/control/api/live/game/' + gid).then((r) => (r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)))),
      stop() { stopped = true; if (ctl) ctl.abort(); },
    };
  }
  window.LivePage = {                                // used by the control page (it connects when the owner has pasted the token)
    connect(cfg) { window.LivePage.disconnect(); S.src = localSource(cfg); S.src.args = [onMsg, (ok) => { S.conn = ok; dirty('head'); }]; S.src.start(...S.src.args); dirty('head'); },
    disconnect() { if (S.src && S.src.stop) S.src.stop(); S.src = null; S.conn = false; onMsg({ t: 'state', tournament: null }); },
  };
  function makeSource() {
    if (window.__BROADCAST__) return snapshotSource(() => Promise.resolve(window.__BROADCAST__), 0);
    if (window.LIVE_LOCAL) return null;              // (the control page connects later: LivePage.connect)
    const q = new URLSearchParams(location.search), D = window.LIVE_DEFAULT || {};           // (a website's copy of the page names its data file in LIVE_DEFAULT, see tools/make_live_site.py)
    const src = q.get('src') || D.src, refresh = +(q.get('refresh') || D.refresh) || 0;
    if (src) return snapshotSource(() => fetch(src + (refresh ? (src.includes('?') ? '&' : '?') + '_=' + Date.now() : ''), { cache: 'no-store' }).then((r) => r.json()), refresh);
    return streamSource();
  }

  // ====================================================================================================== state
  function schedule() { if (!U.raf) U.raf = requestAnimationFrame(render); }
  function dirty(...k) { k.forEach((x) => { D[x] = true; }); schedule(); }

  function onMsg(m) {
    try {
      if (m.t === 'state') applyState(m);
      else if (m.t === 'tick' && S.meta) applyTick(m);
    } catch (e) { console.error('live page: a message could not be applied', e); }
  }
  function applyState(m) {
    const was = S.meta && S.meta.id;
    S.meta = m.tournament || null; S.metaAt = performance.now(); S.frozen = !!m.snapshot; S.taken = m.taken || null;
    S.recsRaw = m.records || null;
    if (!S.meta) { S.engines = []; S.games = []; S.gmap = new Map(); S.active = new Map(); S.structure = null; S.sel = null; dirty('head', 'main', 'minis', 'side'); return; }
    if (S.meta.id !== was) { S.tab = isBracket() ? 'bracket' : 'standings'; S.sel = null; S.ply = null; S.recs = new Map(); S.pending = new Set(); S.missing = new Set(); S.follow = true; S.filter = ''; if (U.filter) U.filter.value = ''; if (U.followBox) U.followBox.checked = true; }
    S.engines = m.engines || [];
    S.names = {}; S.engines.forEach((e) => { S.names[e.id] = e.name; });
    S.games = m.games || []; S.gmap = new Map(S.games.map((g) => [g.gid, g]));
    S.structure = m.structure || null;
    S.active = new Map();
    (m.active || []).forEach(putGame);
    dirty('head', 'main', 'minis', 'side');
  }
  function applyTick(m) {
    if (m.meta) { Object.assign(S.meta, m.meta); S.metaAt = performance.now(); dirty('head'); }
    (m.g || []).forEach(putGame);
    (m.end || []).forEach(endGame);
    (m.gone || []).forEach((gid) => { const g = S.active.get(gid); if (g) { g.gone = true; g.goneAt = performance.now(); } });
    if (m.structure) { S.structure = m.structure; dirty('side'); }
    dirty('main', 'minis');
  }
  function putGame(v) {
    let g = S.active.get(v.gid);
    if (!g) { g = { gid: v.gid, live: true, uci: [], san: [], cp: [], depth: [], ms: [], start_fen: null, book: 0 }; S.active.set(v.gid, g); }
    if (v.white !== undefined) Object.assign(g, { white: v.white, black: v.black, round: v.round, key: v.key, start_fen: v.start_fen, book: v.book, arm: v.arm });
    if (v.from > g.uci.length) { if (S.src && S.src.resync) S.src.resync(); return; }          // a gap: start again from the whole state
    const k = v.from;
    g.uci.length = g.san.length = g.cp.length = g.depth.length = g.ms.length = k;
    for (let i = 0; i < v.uci.length; i++) { g.uci.push(v.uci[i]); g.san.push(v.san[i]); g.cp.push(v.cp[i]); g.depth.push(v.depth[i]); g.ms.push(v.ms[i]); }
    Object.assign(g, { info: v.info || [], clocks: v.clocks, turn: v.turn, since: v.since, rt: performance.now(), check: v.check, wait: v.wait, think: v.think, gone: false, frozen: S.frozen });
  }
  function mkRec(gid, r) {
    const s = S.gmap.get(gid) || {};
    return { gid, live: false, white: r.white, black: r.black, round: r.round, result: r.result || s.result, reason: r.reason || s.reason, start_fen: r.start_fen, uci: r.uci || [], san: r.san || [], cp: r.cp || [],
             depth: r.depth || [], ms: r.ms || [], book: r.book || 0, arm: r.arm, info: null };
  }
  function endGame(e) {
    const s = e.sum;
    if (!S.gmap.has(s.gid)) { S.games.push(s); S.gmap.set(s.gid, s); dirty('side'); }
    S.active.delete(s.gid);
    if (e.rec) S.recs.set(s.gid, mkRec(s.gid, e.rec));
    if (S.sel && S.sel.gid === s.gid) { S.holdUntil = performance.now() + 4000; if (!e.rec) fetchRec(s.gid); }
  }
  function fetchRec(gid) {
    if (S.recs.has(gid) || S.pending.has(gid)) return;
    S.pending.add(gid);
    const lost = () => { if (S.src.kind === 'snapshot') S.missing.add(gid); dirty('main'); };          // (a published snapshot only holds the latest games' moves)
    S.src.game(gid).then((r) => { if (r) { S.recs.set(gid, mkRec(gid, r)); dirty('main'); } else lost(); }, lost).then(() => S.pending.delete(gid));
  }
  const viewOf = (gid) => S.active.get(gid) || S.recs.get(gid) || null;

  function select(gid, auto) {
    S.sel = { gid }; S.ply = null;
    if (!auto) S.follow = false;
    if (!S.active.has(gid)) fetchRec(gid);
    if (U.followBox) U.followBox.checked = S.follow;
    dirty('main', 'minis');
  }
  function housekeeping() {
    const now = performance.now();
    for (const [gid, g] of S.active) if (g.gone && now - g.goneAt > 3000) S.active.delete(gid);       // aborted (a game that finished is replaced by its record when the result arrives)
    if (!S.meta) return;
    const cur = S.sel ? S.sel.gid : null;
    if (cur !== null && !viewOf(cur) && !S.pending.has(cur) && !S.gmap.has(cur)) S.sel = null;
    if (S.follow) {
      if (cur !== null && S.active.has(cur) && !S.active.get(cur).gone) return;
      if (cur !== null && S.holdUntil > now) return;
      const ids = [...S.active.keys()].filter((id) => !S.active.get(id).gone).sort((a, b) => a - b);
      if (ids.length) select(ids[0], true);
      else if (S.games.length && (cur === null || S.active.size === 0)) { const last = S.games[S.games.length - 1].gid; if (cur !== last && (cur === null || S.holdUntil <= now)) select(last, true); }
    } else if (cur === null && S.games.length) select(S.games[S.games.length - 1].gid, false);
  }

  // ====================================================================================================== clocks
  function thinkMs(g) { return g.live && !g.wait && !g.frozen ? (g.since || 0) + (performance.now() - g.rt) : 0; }
  function clockOf(g, c) {                           // ms to show for colour c (0 white, 1 black): the clock of a timed game, else the thinking time
    const timed = !S.meta || !S.meta.tc || (S.meta.tc.mode || 'time') === 'time';
    const moving = g.live && !g.gone && g.turn === (c ? 'b' : 'w');
    if (timed) {
      if (!g.clocks) return null;
      return Math.max(0, g.clocks[c] - (moving ? thinkMs(g) : 0));
    }
    if (moving) return thinkMs(g);
    for (let i = g.ms.length - 1; i >= Math.max(0, g.ms.length - 2); i--) if (C.colorOf(C.ensure(g), i) === c && g.ms[i] !== null) return g.ms[i];
    return null;
  }
  function tickClocks() {
    const g = S.sel ? viewOf(S.sel.gid) : null;
    if (g && U.clk && U.clk[0] && U.clk[1]) { const atEnd = S.ply === null; for (let c = 0; c < 2; c++) U.clk[c].textContent = g.live && atEnd ? fmt.clock(clockOf(g, c)) : (g.live ? fmt.clock(null) : ''); }
    for (const [gid, m] of U.minis) { const mg = S.active.get(gid); if (mg) for (let c = 0; c < 2; c++) m.clk[c].textContent = fmt.clock(clockOf(mg, c)); }
    if (S.meta && U.elapsed) {
      const run = S.meta.status === 'running' && !S.frozen && S.src && S.src.kind === 'live';
      U.elapsed.textContent = S.meta.elapsed || run ? fmt.dur(S.meta.elapsed + (run ? (performance.now() - S.metaAt) / 1000 : 0)) : '';
    }
    if (g && U.pl && U.pl[0] && U.pl[1]) for (let c = 0; c < 2; c++) U.pl[c].classList.toggle('turn', !!g.live && !g.gone && !g.frozen && S.ply === null && g.turn === (c ? 'b' : 'w'));
  }

  // ====================================================================================================== page skeleton
  function buildUI() {
    const root = $('#live');
    U.board = new A.Board({ size: 640, coords: true });
    U.pl = [null, null];
    const player = () => h('div', { class: 'pl' }, h('span', { class: 'sw' }), h('span', { class: 'pn' }), h('span', { class: 'pe' }), h('span', { class: 'ptag' }), h('span', { class: 'sp' }), h('span', { class: 'pc mono' }));
    U.pTop = player(), U.pBot = player();
    U.evbar = h('div', { class: 'evbar' }, h('div', { class: 'w' }), h('span', { class: 't top' }), h('span', { class: 't bot' }));
    U.engBox = [0, 1].map(() => h('div', { class: 'eng' }, h('div', { class: 'eh' }, h('b', { class: 'en' }), h('span', { class: 'tag' }), h('span', { class: 'sp' }), h('span', { class: 'ev mono' })),
      h('div', { class: 'kv' }, ['depth', 'nodes', 'nps', 'time'].map((k) => h('div', {}, h('small', {}, k), h('b', { class: 'mono ' + k }, '-')))), h('div', { class: 'pv mono' })));
    U.msg = h('div', { class: 'msg' });
    U.nav = h('div', { class: 'nav' },
      ...[['⏮', () => gotoPly(0), 'First move (Home)'], ['◀', () => stepPly(-1), 'Back (left arrow)'], ['▶', () => stepPly(1), 'Forward (right arrow)'], ['⏭', () => gotoPly(1e9), 'Last move / live (End)']]
        .map(([t, f, ti]) => h('button', { class: 'btn', onclick: f, title: ti }, t)),
      h('button', { class: 'btn', onclick: () => { S.flipped = !S.flipped; dirty('main'); }, title: 'Flip the board (F)' }, 'Flip'),
      h('label', { class: 'chk', title: 'Show the longest running game; switch to the next one when it ends' }, U.followBox = h('input', { type: 'checkbox', checked: true, onchange: (e) => { S.follow = e.target.checked; if (S.follow) schedule(); } }), ' Auto-follow'));
    U.moves = h('div', { class: 'moves' });
    U.graph = h('div', { class: 'graph' });
    U.minisBox = h('div', { class: 'minis' });
    U.minisHint = h('div', { class: 'hint' }, 'No other game is running.');
    U.tabBtns = h('div', { class: 'tabs' });
    U.filter = h('input', { class: 'inp', type: 'search', placeholder: 'Filter games by engine...', oninput: (e) => { S.filter = e.target.value.trim().toLowerCase(); S.rows = 300; U.sideAt = -1e9; dirty('side'); } });
    U.sideBody = h('div', { class: 'sidebody' });
    U.tname = h('b', { class: 'tname' }), U.tsub = h('span', { class: 'dim tsub' }), U.status = h('span', { class: 'chip' }), U.conn = h('span', { class: 'chip' });
    U.prog = h('div', { class: 'prog' }, h('i')), U.progT = h('span', { class: 'mono dim' }), U.elapsed = h('span', { class: 'mono dim' });
    U.share = h('div', { class: 'share hidden' });
    U.ctl = h('div', { class: 'ctl' });
    root.append(
      h('header', { class: 'top' }, h((window.LIVE_DEFAULT || {}).home ? 'a' : 'div', { class: 'brand', href: (window.LIVE_DEFAULT || {}).home || null }, h('img', { src: 'assets/logo.svg', width: 22, height: 22, alt: '' }), h('b', {}, 'Kestrel Arena'), h('span', { class: 'dim' }, 'Live')),
        h('div', { class: 'ttl' }, U.tname, U.tsub), U.status, h('div', { class: 'pg' }, U.prog, U.progT, U.elapsed), h('span', { class: 'sp' }), U.conn, U.ctl),
      U.share,
      h('div', { class: 'grid' },
        h('section', { class: 'bcol' }, U.pTop, h('div', { class: 'bw' }, U.evbar, U.board.el), U.pBot, U.msg, U.nav),
        h('section', { class: 'icol' }, h('div', { class: 'panels' }, U.engBox), h('div', { class: 'below' }, U.moves, U.graph)),
        h('section', { class: 'side' }, h('div', { class: 'cap' }, 'Other games running'), U.minisBox, U.minisHint, U.tabBtns, U.filter, U.sideBody)));
    U.clk = [null, null];
    document.addEventListener('keydown', (e) => {
      if (e.target && /INPUT|TEXTAREA|SELECT/.test(e.target.tagName)) return;
      if (e.key === 'ArrowLeft') stepPly(-1); else if (e.key === 'ArrowRight') stepPly(1); else if (e.key === 'Home') gotoPly(0); else if (e.key === 'End') gotoPly(1e9);
      else if (e.key === 'f' || e.key === 'F') { S.flipped = !S.flipped; dirty('main'); } else return;
      e.preventDefault();
    });
    if (A.token && !window.__BROADCAST__) {               // only the page served to this PC by the Arena has the controls; the shared page and an exported broadcast are read-only
      U.ctl.append(h('button', { class: 'btn', onclick: toggleShare }, 'Share on the network'), h('a', { class: 'btn', href: '/api/live/export?t=' + encodeURIComponent(A.token), download: '', title: 'One html file with every game, the standings and the graphs' }, 'Export broadcast'));
    }
  }
  const gotoPly = (p) => { const g = S.sel && viewOf(S.sel.gid); if (!g) return; const n = g.uci.length; p = Math.max(0, Math.min(p, n)); S.ply = p === n ? null : p; dirty('main'); };
  const stepPly = (d) => { const g = S.sel && viewOf(S.sel.gid); if (g) gotoPly((S.ply === null ? g.uci.length : S.ply) + d); };

  // ====================================================================================================== head
  function renderHead() {
    const m = S.meta;
    U.tname.textContent = m ? m.name || m.id : 'No tournament';
    const snap = !!S.src && S.src.kind === 'snapshot';
    U.tsub.textContent = m ? [fname(m), S.engines.length + ' engines', m.tc_label, (m.concurrency || 1) + ' parallel'].join(' · ') : snap ? "Tournaments are run on the owner's PC and published here." : window.LIVE_LOCAL ? 'Connect to your Arena above, start a tournament and it appears here.' : 'Start a tournament in the Arena (or open a saved one) and it appears here.';
    const st = m ? m.status : '';
    U.status.textContent = S.frozen ? (st === 'running' ? 'In progress' : st === 'finished' ? 'Finished' : 'Snapshot') : { running: 'Running', paused: 'Paused', stopping: 'Stopping', stopped: 'Stopped', finished: 'Finished', error: 'Error', ready: 'Ready' }[st] || st;
    U.status.className = 'chip ' + (S.frozen ? (st === 'running' ? 'good' : st === 'finished' ? 'acc' : '') : st === 'running' ? 'good' : st === 'finished' ? 'acc' : st === 'error' ? 'bad' : st === 'paused' ? 'warn' : '');
    const live = S.src && S.src.kind === 'live';
    U.conn.textContent = S.frozen ? (S.taken ? (st === 'running' ? 'as of ' : 'taken ') + S.taken : 'saved broadcast') : live ? (S.conn ? '● live' : '○ reconnecting…') : S.taken ? 'updated ' + S.taken : '';
    U.conn.className = 'chip ' + (live ? (S.conn ? 'good' : 'bad') : 'dim');
    if (m) {
      const done = S.games.length, total = Math.max(m.total || 0, done), approx = isBracket() && m.status !== 'finished';
      const pct = total ? Math.min(approx ? 99 : 100, (100 * done) / (approx && done >= total ? done + Math.max(1, S.active.size) : total)) : 0;
      U.prog.firstChild.style.width = pct + '%';
      U.progT.textContent = done + ' / ' + (approx ? '~' : '') + total + ' games';
    } else { U.prog.firstChild.style.width = '0'; U.progT.textContent = ''; }
    if (!m && !(S.src && S.src.kind === 'live')) { U.status.textContent = ''; U.conn.textContent = ''; }          // (nothing published yet: no chips)
    document.title = (m ? m.name + ' - ' : '') + (window.LIVE_LOCAL ? 'Kestrel Control' : 'Kestrel Arena Live');
  }

  // ====================================================================================================== the big board
  const cpAt = (g, ply) => { for (let i = ply - 1; i >= 0; i--) if (g.cp[i] !== null && g.cp[i] !== undefined) return g.cp[i]; return null; };
  const barPct = (cp) => 50 + 50 * (2 / (1 + Math.exp(-0.00368208 * Math.max(-3000, Math.min(3000, cp)))) - 1);
  function sideInfo(g, ply, c) {                     // the last thing engine c did at or before `ply`
    for (let i = ply - 1; i >= Math.max(0, ply - 2); i--) {
      if (i < g.book) return null;
      if (C.colorOf(g, i) === c) {
        const r = (g.info || []).find((x) => x[0] === i);
        return { cp: g.cp[i], depth: g.depth[i], ms: g.ms[i], nodes: r ? r[3] : null, nps: r ? r[4] : null, pv: r ? r[6] : '', san: g.san[i] };
      }
    }
    return null;
  }
  function renderMain() {
    const g = S.sel ? viewOf(S.sel.gid) : null;
    if (!g) {
      const snap = !!S.src && S.src.kind === 'snapshot';
      U.board.setPosition(C.START); U.msg.textContent = S.meta ? (S.sel ? (S.missing.has(S.sel.gid) ? 'The moves of this game are not published (only the latest games are).' : 'Loading the game...') : 'Waiting for the first game...') : snap ? 'No tournament has been published yet.' : 'No tournament is loaded.';
      for (const p of [U.pTop, U.pBot]) { p.children[1].textContent = ''; p.children[2].textContent = ''; p.children[3].textContent = ''; p.children[5].textContent = ''; }
      U.moves.textContent = ''; U.mvKey = null; U.graph.textContent = ''; U.evbar.firstChild.style.height = '50%';
      U.engBox.forEach((b) => { $('.en', b).textContent = ''; $('.ev', b).textContent = ''; $('.pv', b).textContent = ''; $('.tag', b).textContent = ''; });
      return;
    }
    C.ensure(g);
    const n = g.uci.length, ply = S.ply === null ? n : Math.max(0, Math.min(S.ply, n)), atEnd = ply === n;
    if (U.board.flipped !== S.flipped) U.board.flip(S.flipped);
    const last = ply > 0 ? [g.uci[ply - 1].slice(0, 2), g.uci[ply - 1].slice(2, 4)] : null;
    let check = null;
    if (ply > 0 && /[+#]$/.test(g.san[ply - 1] || '')) { const wtm = (g._turn0 === 'w') === (ply % 2 === 0); check = C.find(g._pl[ply], wtm ? 'K' : 'k'); }
    U.board.setPosition(C.fenOf(g._pl[ply]), { last, check });
    // players (the bottom one is white unless the board is flipped)
    const sideRow = (c, el) => {
      const id = c ? g.black : g.white, e = S.engines.find((x) => x.id === id);
      el.children[0].className = 'sw ' + (c ? 'b' : 'w'); el.children[1].textContent = nm(id); el.children[1].title = nm(id);
      el.children[2].textContent = e && e.elo ? e.elo : '';
      el.children[3].textContent = g.live && g.wait ? 'starting...' : '';
      return el;
    };
    const top = S.flipped ? 0 : 1, bot = 1 - top;
    U.pl = []; U.pl[top] = sideRow(top, U.pTop); U.pl[bot] = sideRow(bot, U.pBot);
    U.clk = []; U.clk[top] = U.pTop.children[5]; U.clk[bot] = U.pBot.children[5];
    // result / state line
    const sum = S.gmap.get(g.gid);
    const res = g.result || (sum && sum.result);
    U.msg.className = 'msg' + (res ? ' over' : '');
    const where = ' · round ' + rnd(g.round) + (atEnd ? '' : ' · ply ' + ply + ' of ' + n + ' (End = back to the last move)');
    U.msg.textContent = g.live ? (g.arm ? 'ARMAGEDDON, a draw counts as a win for Black · ' : '') + (g.gone ? 'Game finished...' : g.frozen ? 'Not finished when this broadcast was saved' : atEnd ? 'Live' : 'Replay') + where
      : res ? res + ' · ' + (g.reason || (sum && sum.reason) || '') + (g.arm ? ' · Armageddon' : '') + where : 'Loading...';
    // eval bar and the two engine panels
    const thinking = g.live && g.think && atEnd && !g.gone ? g.think : null;
    const cp = thinking && thinking.cp !== null ? thinking.cp : cpAt(g, ply);
    U.evbar.firstChild.style.height = (cp === null ? 50 : barPct(cp)) + '%';
    $('.top', U.evbar).textContent = ''; $('.bot', U.evbar).textContent = '';
    if (cp !== null) $('.' + (cp >= 0 === !S.flipped ? 'bot' : 'top'), U.evbar).textContent = fmt.eval(cp);        // the text sits in the colour that is ahead
    U.evbar.classList.toggle('flip', S.flipped);
    for (let c = 0; c < 2; c++) {
      const b = U.engBox[c], id = c ? g.black : g.white, th = thinking && thinking.side === (c ? 'b' : 'w') && g.turn === thinking.side ? thinking : null;
      const info = th ? { cp: th.cp, depth: th.depth, ms: th.time, nodes: th.nodes, nps: th.nps, pv: th.pv } : sideInfo(g, ply, c);
      $('.en', b).textContent = nm(id); b.classList.toggle('on', !!th);
      $('.tag', b).textContent = th ? 'thinking' : info && info.san ? 'played ' + info.san : '';
      $('.ev', b).textContent = info && info.cp !== null && info.cp !== undefined ? fmt.eval(info.cp) : '';
      const kv = (k, v) => { $('.' + k, b).textContent = v; };
      kv('depth', info && info.depth ? info.depth + (th && th.sd ? '/' + th.sd : '') : '-'); kv('nodes', info ? fmt.num(info.nodes) : '-'); kv('nps', info ? fmt.num(info.nps) : '-'); kv('time', info ? fmt.ms(info.ms) : '-');
      $('.pv', b).textContent = info && info.pv ? info.pv : '';
    }
    renderMoves(g, ply); renderGraph(g, ply);
    tickClocks();
  }
  function renderMoves(g, ply) {
    const key = g.gid + ':' + g.uci.length + ':' + ply;
    if (U.mvKey === key) return;
    U.mvKey = key;
    const bk = g._turn0 === 'b', n = g.uci.length, rows = Math.ceil((n + (bk ? 1 : 0)) / 2), frag = document.createDocumentFragment();
    const cell = (idx) => {
      if (idx < 0 || idx >= n) return h('span', { class: 'm none' }, idx < 0 ? '...' : '');
      const cp = g.cp[idx], tip = (cp !== null && cp !== undefined ? fmt.eval(cp) : 'book') + (g.depth[idx] ? ' / ' + g.depth[idx] : '') + (g.ms[idx] ? ' · ' + fmt.ms(g.ms[idx]) : '');
      return h('span', { class: 'm' + (idx === ply - 1 ? ' cur' : '') + (idx < g.book ? ' bk' : ''), title: tip, onclick: () => gotoPly(idx + 1) }, g.san[idx]);
    };
    for (let r = 0; r < rows; r++) { const w = bk ? 2 * r - 1 : 2 * r; frag.append(h('span', { class: 'no' }, g._full0 + r + '.'), cell(w), cell(w + 1)); }
    U.moves.textContent = ''; U.moves.append(frag);
    const cur = $('.cur', U.moves);
    if (cur) U.moves.scrollTop = Math.max(0, cur.offsetTop - U.moves.clientHeight / 2);
  }
  function renderGraph(g, ply) {
    const n = g.uci.length, W = 420, H = 120, X = Math.max(n, 60), y = (cp) => H / 2 - (Math.max(-500, Math.min(500, cp)) / 500) * (H / 2 - 6), x = (i) => 4 + (i / X) * (W - 8);
    const pts = [];
    for (let i = 0; i < n; i++) if (g.cp[i] !== null && g.cp[i] !== undefined) pts.push([x(i + 1), y(g.cp[i])]);
    const d = pts.map((p, i) => (i ? 'L' : 'M') + p[0].toFixed(1) + ' ' + p[1].toFixed(1)).join('');
    const area = pts.length ? d + 'L' + pts[pts.length - 1][0].toFixed(1) + ' ' + H / 2 + 'L' + pts[0][0].toFixed(1) + ' ' + H / 2 + 'Z' : '';
    const grid = [-300, -100, 0, 100, 300].map((v) => svg('line', { x1: 0, x2: W, y1: y(v), y2: y(v), class: v === 0 ? 'g0' : 'gl' }));
    const el = svg('svg', { viewBox: '0 0 ' + W + ' ' + H, preserveAspectRatio: 'none', class: 'gsvg', onclick: (e) => { const r = e.currentTarget.getBoundingClientRect(); gotoPly(Math.round((((e.clientX - r.left) / r.width) * W - 4) / (W - 8) * X)); } },
      svg('rect', { x: 0, y: 0, width: W, height: H, class: 'gbg' }), grid, area ? svg('path', { d: area, class: 'garea' }) : null, svg('path', { d, class: 'gline' }),
      ply > 0 ? svg('line', { x1: x(ply), x2: x(ply), y1: 0, y2: H, class: 'gcur' }) : null);
    A.clear(U.graph).append(h('div', { class: 'cap' }, 'Evaluation (white\'s point of view, ±5)'), el);
  }

  // ====================================================================================================== other running games
  function makeMini(g) {
    const board = new A.Board({ size: 128, coords: false });
    const el = h('div', { class: 'mini', onclick: () => select(g.gid, false) },
      h('div', { class: 'mh' }, h('span', { class: 'mn' }), h('span', { class: 'me mono' })), board.el, h('div', { class: 'mf' }, h('span', { class: 'mc mono' }), h('span', { class: 'mc mono' })));
    return { el, board, name: el.querySelector('.mn'), ev: el.querySelector('.me'), clk: [...el.querySelectorAll('.mc')] };
  }
  function renderMinis() {
    const ids = [...S.active.keys()].filter((id) => !S.sel || id !== S.sel.gid).sort((a, b) => a - b);
    for (const [id, m] of U.minis) if (!ids.includes(id)) { m.el.remove(); U.minis.delete(id); }
    ids.forEach((id, k) => {
      const g = S.active.get(id);
      let m = U.minis.get(id);
      if (!m) { m = makeMini(g); U.minis.set(id, m); }
      if (U.minisBox.children[k] !== m.el) U.minisBox.insertBefore(m.el, U.minisBox.children[k] || null);
      C.ensure(g);
      const n = g.uci.length, last = n ? [g.uci[n - 1].slice(0, 2), g.uci[n - 1].slice(2, 4)] : null;
      m.board.setPosition(C.fenOf(g._pl[n]), { last });
      m.name.textContent = nm(g.white) + ' - ' + nm(g.black); m.name.title = m.name.textContent;
      const cp = cpAt(g, n);
      m.ev.textContent = cp === null ? '' : fmt.eval(cp);
      m.clk[0].textContent = fmt.clock(clockOf(g, 0)); m.clk[1].textContent = fmt.clock(clockOf(g, 1));
    });
    U.minisHint.classList.toggle('hidden', ids.length > 0);
  }

  // ====================================================================================================== standings, crosstable, bracket, games
  function standingsOf() {
    const extra = {}, st = S.structure;
    if (S.meta.format === 'swiss' && st && st.byes) for (const b in st.byes) extra[st.byes[b]] = (extra[st.byes[b]] || 0) + st.bye_points;
    return A.elo.standings(S.engines, S.games, { extraPoints: extra });
  }
  function elimInfo() {
    const st = S.structure;
    if (!S.meta || S.meta.format !== 'elimination' || !st || (st.kind !== 'elimination' && st.kind !== 'bracket')) return null;
    const out = {}; st.out.forEach((o, i) => { out[o.id] = { round: o.round, order: i }; });
    return { st, out, losses: st.losses };
  }
  const elimRank = (el, r) => (r.id === el.st.champion ? 0 : el.out[r.id] ? 2 : 1);
  function orderElimination(res, el) {
    res.rows.sort((a, b) => elimRank(el, a) - elimRank(el, b) || (elimRank(el, a) === 1 ? el.st.loss[a.id] - el.st.loss[b.id] : elimRank(el, a) === 2 ? el.out[b.id].round - el.out[a.id].round : 0) || b.pts - a.pts || b.sb - a.sb);
    res.rows.forEach((r, i) => { r.rank = i + 1; });
  }
  function standingsTable() {
    const res = standingsOf(), el = elimInfo();
    if (el) orderElimination(res, el);
    const playing = new Set(); S.active.forEach((g) => { playing.add(g.white); playing.add(g.black); });
    const featured = S.sel && viewOf(S.sel.gid), anchored = S.engines.every((e) => e.elo);
    const rows = res.rows.map((r) => h('tr', { class: (playing.has(r.id) ? 'play ' : '') + (featured && (featured.white === r.id || featured.black === r.id) ? 'feat' : '') },
      h('td', { class: 'r dim' }, r.rank), h('td', { class: 'nm', title: r.name }, r.name),
      el ? h('td', { class: 'r mono' }, el.st.loss[r.id] + ' / ' + el.losses) : null,
      el ? h('td', {}, r.id === el.st.champion ? 'champion' : el.out[r.id] ? 'out in round ' + el.out[r.id].round : 'alive') : null,
      h('td', { class: 'r mono' }, r.games ? Math.round(r.elo) : ''), h('td', { class: 'r mono dim' }, r.err ? '±' + Math.round(r.err) : ''),
      h('td', { class: 'r mono b' }, fmt.pts(r.pts)), h('td', { class: 'r mono dim' }, r.games),
      h('td', { class: 'r mono' }, h('span', { class: 'good' }, r.w), ' / ', r.d, ' / ', h('span', { class: 'bad' }, r.l)),
      h('td', { class: 'r mono' }, r.score === null ? '' : fmt.pct(r.score, 1)), h('td', { class: 'r mono dim' }, fmt.pts(r.sb))));
    return h('table', { class: 'tbl' }, h('thead', {}, h('tr', {}, h('th', { class: 'r' }, '#'), h('th', {}, 'Engine'), el ? h('th', { class: 'r' }, 'Losses') : null, el ? h('th', {}, 'Status') : null,
      h('th', { class: 'r', title: anchored ? "Rating fitted to the results, anchored to the engines' Elo values" : 'Rating difference fitted to the results (average 0)' }, anchored ? 'Elo' : 'Elo diff'),
      h('th', { class: 'r', title: '95% confidence' }, '±'), h('th', { class: 'r' }, 'Pts'), h('th', { class: 'r' }, 'Games'), h('th', { class: 'r' }, 'W / D / L'), h('th', { class: 'r' }, 'Score'), h('th', { class: 'r', title: 'Sonneborn-Berger tie-break' }, 'SB'))),
      h('tbody', {}, rows));
  }
  function crossTable() {
    const res = standingsOf(), n = res.rows.length;
    if (n > 40) return h('div', { class: 'hint' }, 'Too many engines for a matrix (' + n + '): use the standings.');
    const order = res.rows.map((r) => res.idx.get(r.id)), names = S.engines.map((e) => e.name);
    return h('table', { class: 'tbl xt' }, h('thead', {}, h('tr', {}, h('th', {}), order.map((j, k) => h('th', { title: names[j], class: 'c' }, k + 1)), h('th', { class: 'c' }, 'Pts'))),
      h('tbody', {}, order.map((i, k) => h('tr', {}, h('th', { class: 'l' }, (k + 1) + ' ' + names[i]), order.map((j) => {
        if (i === j) return h('td', { class: 'self' });
        const g = res.N[i][j];
        if (!g) return h('td', { class: 'dim' }, '·');
        const s = res.S[i][j];
        return h('td', { class: s > g / 2 ? 's1' : s < g / 2 ? 's0' : 'sh', title: names[i] + ' vs ' + names[j] + ': ' + s + ' / ' + g }, fmt.pts(s));
      }), h('td', { class: 'mono b' }, fmt.pts(res.rows[k].pts))))));
  }
  const BK = { w: 176, fw: 232, h: 42, hb: 22, colGap: 30, rowH: 54, pad: 8, top: 14 };
  function bracketTree(st) {                         // the elimination bracket as a tree (a port of the Tournament tab's drawing)
    const live = new Set(); S.active.forEach((g) => { live.add(g.white + '|' + g.black); live.add(g.black + '|' + g.white); });
    const place = (s) => (s ? (s[0] === 'W' ? 'Winner of #' + s[1] : s[0] === 'L' ? 'Loser of #' + s[1] : 'Seed ' + (s[1] + 1)) : 'bye');
    const lossTag = (id, lo) => {
      if (lo === undefined || lo === null) return null;
      return h('span', { class: 'ls' + (!st.ladder && lo === st.losses - 1 ? ' last' : ''), title: nm(id) + ': ' + lossTxt(lo) + ' before this match' }, lossTxt(lo));
    };
    const row = (m, i) => {
      const id = i === 0 ? m.a : m.b, sc = i === 0 ? m.sa : m.sb;
      if (id) return h('div', { class: 'r' + (m.winner && m.winner === id ? ' win' : '') }, h('span', { class: 'nm', title: nm(id) }, nm(id)), lossTag(id, i === 0 ? m.la : m.lb), m.kind === 'real' && (m.done || sc || m.tb) ? h('span', { class: 'mono' }, fmt.pts(sc)) : null);
      return h('div', { class: 'r ph' }, h('span', { class: 'nm' }, place(m.s[i])));
    };
    const box = (m, x, y, hh, ww) => h('div', { class: 'mb' + (m.kind === 'bye' ? ' bye' : '') + (m.kind === 'real' && !m.done && m.a && m.b && live.has(m.a + '|' + m.b) ? ' live' : '') + (m.g && m.g.length ? ' clk' : ''), onclick: m.g && m.g.length ? () => select(m.g[0], false) : null,
      style: 'left:' + x + 'px;top:' + y + 'px;height:' + hh + 'px' + (ww ? ';width:' + ww + 'px' : ''), title: (m.no ? 'Match #' + m.no : '') + (m.g && m.g.length ? ' - click to replay its first game' : '') + (m.tb ? ' - ' + m.tb + ' extra pair(s) of games' : '') + (m.arm ? ' - decided by an Armageddon game' : '') },
      m.no ? h('span', { class: 'no' }, '#' + m.no + (m.arm ? ' - Armageddon' : '')) : null, m.kind === 'bye' ? [row(m, m.s[0] ? 0 : 1), h('span', { class: 'bye-tag' }, 'bye')] : [row(m, 0), row(m, 1)]);
    const els = [], segs = [], pos = new Map();
    let y = BK.pad, cols = 0;
    st.brackets.forEach((bk) => {
      els.push(h('div', { class: 'bt', style: 'left:' + BK.pad + 'px;top:' + y + 'px' }, bk.name, bk.k ? h('span', { class: 'dim' }, ' (' + lossTxt(bk.k) + ')') : null));
      const base = y + 36;
      bk.rounds.forEach((rd, ri) => els.push(h('div', { class: 'ch', style: 'left:' + (BK.pad + ri * (BK.w + BK.colGap)) + 'px;top:' + (base - 14) + 'px;width:' + BK.w + 'px' }, 'Round ' + rd.round)));
      bk.rounds.forEach((rd) => rd.matches.forEach((m) => {
        const x = BK.pad + m.x * (BK.w + BK.colGap), cy = base + BK.top + m.y * BK.rowH + BK.h / 2, hh = m.kind === 'bye' ? BK.hb : BK.h;
        pos.set(m.id, { x, cy, w: BK.w }); els.push(box(m, x, cy - hh / 2, hh));
      }));
      y = base + BK.top + bk.rows * BK.rowH + 22; cols = Math.max(cols, bk.rounds.length);
    });
    st.brackets.forEach((bk) => bk.rounds.forEach((rd) => rd.matches.forEach((m) => (m.f || []).forEach((fid) => {
      const a = pos.get(fid), b = pos.get(m.id);
      if (!a || !b) return;
      const x1 = a.x + BK.w, xm = x1 + BK.colGap / 2;
      segs.push('M' + x1 + ' ' + a.cy + 'H' + xm + 'V' + b.cy + 'H' + b.x);
    }))));
    const fx = BK.pad + cols * (BK.w + BK.colGap) + 6, wb = st.brackets[0].rounds[st.brackets[0].rounds.length - 1].matches[0];
    let fy = wb ? pos.get(wb.id).cy - BK.h / 2 : BK.pad + 50, wMax = fx + BK.fw + BK.pad;
    const byNo = new Map(); st.brackets.forEach((bk) => bk.rounds.forEach((rd) => rd.matches.forEach((m) => { if (m.no) byNo.set(m.no, pos.get(m.id)); })));
    const bn = (k) => (k === 0 ? 'winners bracket' : 'bracket ' + k);
    const fm = (f) => ({ kind: 'real', a: f.a, b: f.b, sa: f.sa, sb: f.sb, tb: f.tb, winner: f.winner, done: f.done, s: f.s, no: f.no, g: f.g, la: f.la, lb: f.lb, arm: f.arm });
    const champ = (px, py) => h('div', { class: 'mb champ', style: 'left:' + px + 'px;top:' + py + 'px;height:' + BK.h + 'px;width:' + BK.fw + 'px' }, h('div', { class: 'r win' }, h('span', { class: 'nm', title: nm(st.champion) }, nm(st.champion))), h('div', { class: 'r' }, h('span', { class: 'nm dim' }, 'Champion, ' + lossTxt(st.loss[st.champion]))));
    if (st.ladder) {
      if (st.final.length) els.push(h('div', { class: 'bt', style: 'left:' + fx + 'px;top:' + BK.pad + 'px' }, 'Final stage'));
      st.final.forEach((f, i) => {
        const x = fx + i * (BK.fw + BK.colGap), srcs = (f.s || []).map((sl) => sl && byNo.get(sl[1])).filter(Boolean), fk = f.fk || [];
        const cy = srcs.length ? srcs.reduce((t, p) => t + p.cy, 0) / srcs.length : fy + BK.h / 2;
        srcs.forEach((p) => segs.push('M' + (p.x + p.w) + ' ' + p.cy + 'H' + (x - BK.colGap / 2) + 'V' + cy + 'H' + x));
        els.push(h('div', { class: 'ch', style: 'left:' + x + 'px;top:' + (cy - BK.h / 2 - 30) + 'px;width:' + BK.fw + 'px' }, st.losses === 2 ? 'Grand final' : 'Final ' + (i + 1) + ': ' + (fk[0] === null ? 'winner' : bn(fk[0])) + ' vs ' + bn(fk[1])));
        els.push(box(fm(f), x, cy - BK.h / 2, BK.h, BK.fw)); byNo.set(f.no, { x, cy, w: BK.fw });
      });
      const lastP = st.final.length ? byNo.get(st.final[st.final.length - 1].no) : (wb ? pos.get(wb.id) : null);
      if (lastP) {
        const cx = lastP.x + lastP.w + BK.colGap;
        segs.push('M' + (lastP.x + lastP.w) + ' ' + lastP.cy + 'H' + cx);
        if (st.champion) els.push(champ(cx, lastP.cy - BK.h / 2));
        wMax = Math.max(wMax, cx + BK.fw + BK.pad);
      }
    } else {
      if (!st.ladder) els.push(h('div', { class: 'bt', style: 'left:' + fx + 'px;top:' + (fy - 44) + 'px' }, 'Final'));
      st.final.forEach((f, i) => {
        if (i === 0) (f.s || []).forEach((sl) => { const a = sl && byNo.get(sl[1]); if (a) segs.push('M' + (a.x + BK.w) + ' ' + a.cy + 'H' + (fx - BK.colGap / 2) + 'V' + (fy + BK.h / 2) + 'H' + fx); });
        els.push(h('div', { class: 'ch', style: 'left:' + fx + 'px;top:' + (fy - 30) + 'px;width:' + BK.fw + 'px' }, st.losses === 2 ? (i === 0 ? 'Grand final' : 'Deciding match') : 'Final ' + (i + 1)));
        els.push(box(fm(f), fx, fy, BK.h, BK.fw)); fy += BK.rowH + 22;
      });
      if (st.champion) els.push(champ(fx, fy + 6));
    }
    const W = wMax, H = Math.max(y, fy + BK.h + 30), lines = [];
    for (let k = 0; k < st.losses; k++) {
      const ids = S.engines.map((e) => e.id).filter((id) => st.loss[id] === k && !st.out.some((o) => o.id === id));
      if (ids.length) lines.push(h('div', {}, h('b', {}, lossTxt(k) + ' (' + ids.length + ')'), ids.length <= 8 ? ': ' + ids.map(nm).join(', ') : null));
    }
    if (st.out.length) lines.push(h('div', { class: 'dim' }, 'Out (' + st.out.length + ')'));
    return h('div', {}, h('div', { class: 'help' }, lines), h('div', { class: 'brk', style: 'width:' + W + 'px;height:' + H + 'px' }, svg('svg', { width: W, height: H }, svg('path', { d: segs.join(''), fill: 'none', stroke: '#6b7480', 'stroke-width': 1 })), els));
  }
  function bracketRounds(st) {                       // knockout (and the first version of the elimination): the rounds as columns
    const side = (m, who) => {
      const id = who === 'a' ? m.a : m.b, sc = who === 'a' ? m.sa : m.sb, lv = who === 'a' ? m.la : m.lb;
      return h('div', { class: m.winner && m.winner === id ? 'win' : '' }, h('span', { class: 'nm', title: nm(id) }, nm(id), id && !m.bye && m.la !== undefined && m.la !== m.lb ? h('span', { class: 'dim' }, ' [' + lv + ']') : null), h('span', { class: 'mono' }, m.bye ? '' : fmt.pts(sc)));
    };
    const cols = st.rounds.map((r) => h('div', { class: 'rnd' }, h('div', { class: 'dim c' }, 'Round ' + r.round),
      r.matches.map((m) => h('div', { class: 'match' + (m.g && m.g.length ? ' clk' : ''), onclick: m.g && m.g.length ? () => select(m.g[0], false) : null }, side(m, 'a'), side(m, 'b'), m.arm ? h('div', { class: 'dim small' }, 'Armageddon: ' + nm(m.arm) + ' had White') : null))));
    if (st.champion) cols.push(h('div', { class: 'rnd' }, h('div', { class: 'dim c' }, 'Champion'), h('div', { class: 'match' }, h('div', { class: 'win' }, nm(st.champion)))));
    return h('div', { class: 'bracket' }, cols);
  }
  function bracketView() {
    const st = S.structure;
    if (st && st.kind === 'bracket') return bracketTree(st);
    if (!st || !st.rounds) return h('div', { class: 'hint' }, 'The bracket appears when the first games finish.');
    return bracketRounds(st);
  }
  function gamesList() {
    const f = S.filter, list = [];
    for (let i = S.games.length - 1; i >= 0 && list.length < S.rows; i--) {
      const g = S.games[i];
      if (f && !(nm(g.white) + ' ' + nm(g.black)).toLowerCase().includes(f)) continue;
      list.push(g);
    }
    const cur = S.sel && S.sel.gid;
    const body = list.map((g) => h('tr', { class: 'clk' + (g.gid === cur ? ' feat' : ''), onclick: () => select(g.gid, false) }, h('td', { class: 'r dim' }, g.gid), h('td', { class: 'r dim' }, rnd(g.round)), h('td', { class: 'nm' }, nm(g.white)),
      h('td', { class: 'c mono b' }, g.result + (g.arm ? ' A' : '')), h('td', { class: 'nm' }, nm(g.black)), h('td', { class: 'r dim' }, g.plies), h('td', { class: 'dim' }, g.reason)));
    const more = S.games.length > list.length && !f ? h('button', { class: 'btn', onclick: () => { S.rows += 500; U.sideAt = -1e9; dirty('side'); } }, 'Show more') : null;
    return h('div', {}, h('table', { class: 'tbl' }, h('thead', {}, h('tr', {}, h('th', { class: 'r' }, '#'), h('th', { class: 'r' }, 'Rd'), h('th', {}, 'White'), h('th', { class: 'c' }, 'Result'), h('th', {}, 'Black'), h('th', { class: 'r' }, 'Plies'), h('th', {}, 'End'))), h('tbody', {}, body)),
      list.length ? null : h('div', { class: 'hint' }, S.games.length ? 'No game matches.' : 'Finished games appear here; click one to replay it.'), more);
  }
  function renderSide() {
    U.sideAt = performance.now();
    const tabs = [['standings', 'Standings'], ['crosstable', 'Crosstable']];
    if (isBracket()) tabs.unshift(['bracket', 'Bracket']);
    tabs.push(['games', 'Games (' + S.games.length + ')']);
    if (!tabs.some((t) => t[0] === S.tab)) S.tab = tabs[0][0];
    A.clear(U.tabBtns).append(...tabs.map(([id, l]) => h('button', { class: 'tb' + (S.tab === id ? ' on' : ''), onclick: () => { S.tab = id; U.sideAt = -1e9; dirty('side'); } }, l)));
    U.filter.classList.toggle('hidden', S.tab !== 'games');
    const sc = U.sideBody.scrollTop, sl = U.sideBody.scrollLeft;
    A.clear(U.sideBody);
    if (S.meta) U.sideBody.append(S.tab === 'bracket' ? bracketView() : S.tab === 'crosstable' ? crossTable() : S.tab === 'games' ? gamesList() : standingsTable());
    U.sideBody.scrollTop = sc; U.sideBody.scrollLeft = sl;
  }

  // ====================================================================================================== sharing (only the page that the Arena serves to this PC has these controls)
  async function api(method, path, body) {
    const r = await fetch(path, { method, headers: { 'X-Arena-Token': A.token, 'Content-Type': 'application/json' }, body: body === undefined ? undefined : JSON.stringify(body) });
    let d = null;
    try { d = await r.json(); } catch (e) { /* not json */ }
    if (!r.ok) throw new Error((d && d.error) || 'HTTP ' + r.status);
    return d;
  }
  function drawShare(s, err) {
    A.clear(U.share);
    const on = !!(s && s.on);
    U.share.append(...[h('label', { class: 'chk' }, h('input', { type: 'checkbox', checked: on, onchange: async (e) => { try { drawShare(await api('POST', '/api/live/share', { on: e.target.checked })); } catch (x) { drawShare(s, x.message); } } }), ' Share this live page on the local network (read-only)'),
      h('span', { class: 'dim' }, on ? ' - anyone on your network can watch; nothing else of the Arena is reachable. Switch it off when you are done.' : ' - off: nothing is reachable from other PCs. Windows may ask once whether to allow the connection.'),
      on ? h('div', { class: 'urls' }, (s.urls || []).map((u) => h('div', {}, h('a', { href: u, target: '_blank' }, u), ' ', h('button', { class: 'btn sm', onclick: () => { if (navigator.clipboard) navigator.clipboard.writeText(u); } }, 'Copy'))), h('span', { class: 'dim' }, s.viewers + ' viewer(s) connected')) : null,
      err ? h('div', { class: 'bad' }, err) : null].filter(Boolean));            // (the native append() would write 'null')
  }
  async function toggleShare() {
    U.share.classList.toggle('hidden');
    if (U.share.classList.contains('hidden')) { clearInterval(U.shareTimer); return; }
    const load = async () => { try { drawShare(await api('GET', '/api/live/share')); } catch (e) { drawShare(null, e.message); } };
    await load();
    clearInterval(U.shareTimer); U.shareTimer = setInterval(() => { if (!U.share.classList.contains('hidden') && !U.share.contains(document.activeElement)) load(); }, 5000);
  }

  // ====================================================================================================== main loop
  function render() {
    U.raf = 0;
    housekeeping();
    if (D.head) renderHead();
    if (D.main) renderMain();
    if (D.minis) renderMinis();
    if (D.side) {
      const wait = 700 - (performance.now() - (U.sideAt || 0));
      if (wait > 0) { if (!U.sideTimer) U.sideTimer = setTimeout(() => { U.sideTimer = 0; schedule(); }, wait); }
      else { renderSide(); D.side = false; }
    }
    D.head = D.main = D.minis = false;
  }
  function init() {
    buildUI();
    U.sideAt = -1e9;                                  // (the first drawing of the tables is not held back)
    S.src = makeSource();
    if (S.src) {
      S.src.args = [onMsg, (ok) => { S.conn = ok; dirty('head'); }];
      S.src.start(...S.src.args);
    }
    setInterval(tickClocks, 100);
    setInterval(() => { if (S.follow || S.active.size) schedule(); }, 500);
    dirty('head', 'main', 'minis', 'side');
  }
  if (typeof document !== 'undefined') { if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init(); }
})();
