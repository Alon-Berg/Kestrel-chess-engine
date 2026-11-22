/* Kestrel Arena - the owner's control page: starts, pauses and stops tournaments in the Arena that runs on THIS PC, and shows them live (js/live.js).
   It talks only to http://127.0.0.1:<port> (the Arena's control server, arena/control.py, which exists while the owner has switched 'Let my website control this Arena' on) with the pairing
   token in an Authorization header.  The token is kept in this browser's localStorage and goes nowhere else; the page has no trackers and loads nothing from other sites. */
(function () {
  'use strict';
  const A = window.A, h = A.h;
  const KEY = 'kestrel-control-v1', PORTS = [8781, 8782, 8783, 8784, 8785, 8786, 8787, 8788, 8789, 8790];
  const $ = (s, r) => (r || document).querySelector(s);
  const DEF = { format: 'roundrobin', mode: 'time', base: 1, inc: 0.01, ms: 100, nodes: 20000, depth: 8, games: 2, rounds: 1, swiss: 5, ko: 2, losses: 2, arm: '3', seeding: 'rating', par: 0, name: '', topn: 7, sel: [] };
  const PRESETS = [['0.5 + 0.005', 0.5, 0.005], ['1 + 0.01', 1, 0.01], ['3 + 0.03', 3, 0.03], ['5 + 0.1', 5, 0.1], ['10 + 0.1', 10, 0.1], ['30 + 0.3', 30, 0.3], ['60 + 0.6', 60, 0.6]];
  const FORMATS = { roundrobin: 'Round robin', gauntlet: 'Gauntlet (the first engine against all)', swiss: 'Swiss', knockout: 'Knockout', elimination: 'Elimination (losers bracket)' };
  const C = { token: '', port: 0, base: '', ok: false, engines: [], limits: null, status: null, list: [], f: Object.assign({}, DEF), fails: 0, tick: 0, timer: 0, retry: 0, estimate: null, estTimer: 0, busy: false, find: '', connecting: false, listSig: '' };
  const U = {};

  // ------------------------------------------------------------------------------------------------------------------ storage (the token stays in this browser)
  (function restore() {
    try { const s = JSON.parse(localStorage.getItem(KEY) || '{}') || {}; C.token = typeof s.token === 'string' ? s.token : ''; C.port = +s.port || 0; Object.assign(C.f, s.f || {}); } catch (e) { /* blocked or damaged: start fresh */ }
  })();
  const persist = () => { try { localStorage.setItem(KEY, JSON.stringify({ token: C.token, port: C.port, f: C.f })); } catch (e) { /* storage blocked: the page works, it just forgets */ } };

  // ------------------------------------------------------------------------------------------------------------------ talking to the Arena
  async function api(method, path, body) {
    let r;
    try {
      r = await A.lfetch(C.base + '/control/api' + path, { method, cache: 'no-store', headers: Object.assign({ Authorization: 'Bearer ' + C.token }, body === undefined ? {} : { 'Content-Type': 'application/json' }), body: body === undefined ? undefined : JSON.stringify(body) });
    } catch (e) { const x = new Error('The Arena does not answer.'); x.net = true; throw x; }
    let d = null;
    try { d = await r.json(); } catch (e) { /* no JSON */ }
    if (!r.ok) { const x = new Error((d && d.error) || 'HTTP ' + r.status); x.status = r.status; x.retry = d && d.retry_after; throw x; }
    return d;
  }
  async function ping(port) {
    try {
      const r = await A.lfetch('http://127.0.0.1:' + port + '/control/api/ping', { cache: 'no-store', signal: typeof AbortSignal !== 'undefined' && AbortSignal.timeout ? AbortSignal.timeout(15000) : undefined });
      const j = await r.json();
      return j && j.app === 'Kestrel Arena' ? port : 0;
    } catch (e) { return 0; }
  }
  async function findArena() {                       // the port the owner used last, then the usual ones (all at once: a closed port answers at once)
    const ports = [...new Set([C.port, ...PORTS].filter(Boolean))];
    return (await Promise.all(ports.map(ping))).find(Boolean) || 0;
  }
  const whyNot = (e) => (e.status === 401 ? 'The Arena refused this token. Copy it again in the Arena (Website control, Copy): it changes when you press Reset pairing.'
    : e.status === 429 ? 'Too many wrong tokens: the Arena is locked for ' + (e.retry || 60) + ' seconds.'
    : e.status === 403 ? 'The Arena refused this page (wrong address or origin). Use the control page you were given, on the PC that runs the Arena.' : e.message);

  async function connect(silent) {
    if (!C.token) { setMsg('Paste the pairing token from the Arena first.', 'warn'); return; }
    if (C.connecting) return;
    C.connecting = true;
    clearTimeout(C.retry);
    try { await connect1(silent); } finally { C.connecting = false; }
  }
  async function connect1(silent) {
    if (!silent) setMsg('Looking for your Arena on this PC...', '');
    const port = await findArena();
    if (!port) {
      if (!silent) setMsg('No Arena answered on this PC (ports 8781-8790). Start the Kestrel Arena, open Live view > Website control and switch on "Let my website control this Arena". If your browser asked about local network access, choose Allow. See the help below.', 'bad');
      return setConnected(false);
    }
    C.port = port; C.base = 'http://127.0.0.1:' + port;
    try {
      const st = await api('GET', '/status');
      C.status = st; C.limits = st.limits;
      C.engines = (await api('GET', '/engines')).engines;
      if (!C.f.par) C.f.par = Math.max(1, Math.min(8, Math.floor(C.limits.logical / 4)));
      persist();
      C.fails = 0;
      setConnected(true);
      setMsg('Connected to the Arena (version ' + st.version + ') at 127.0.0.1:' + port + '. Nothing else of it is reachable from this page.', 'good');
      refreshList();
      if (window.LivePage) window.LivePage.connect({ base: C.base, token: C.token });
    } catch (e) {
      setMsg(whyNot(e), 'bad');
      setConnected(false, e.status !== 401 && e.status !== 403);
    }
  }
  function setConnected(ok, retry) {                 // retry: look for the Arena again every few seconds (it may be started or switched on later); not after a refused token: every try would count as a wrong one
    C.ok = ok;
    clearInterval(C.timer); clearTimeout(C.retry);
    if (ok) C.timer = setInterval(poll, 2000);
    else { if (window.LivePage) window.LivePage.disconnect(); if (C.token && retry !== false) C.retry = setTimeout(() => connect(true), 6000); }
    document.body.classList.toggle('off', !ok);
    U.cstate.textContent = ok ? 'connected' : 'not connected';
    U.cstate.className = 'chip ' + (ok ? 'good' : 'bad');
    renderAll();
  }
  async function poll() {
    try {
      C.status = await api('GET', '/status');
      C.fails = 0;
      if (++C.tick % 3 === 0) refreshList();
      renderRun();
    } catch (e) {
      if (e.net && ++C.fails < 3) return;
      setMsg(e.net ? 'The Arena does not answer any more (closed, or the switch was turned off). Looking for it again...' : whyNot(e), 'bad');
      setConnected(false, e.status !== 401 && e.status !== 403);
    }
  }
  async function refreshList() { try { C.list = (await api('GET', '/tournaments')).tournaments; renderList(); } catch (e) { /* the next poll tells */ } }

  // ------------------------------------------------------------------------------------------------------------------ the form
  const busy = () => !!(C.status && C.status.tournament && ['running', 'paused', 'stopping'].includes(C.status.tournament.status));
  function setF(k, v) { C.f[k] = v; persist(); if (['format', 'mode'].includes(k)) renderForm(); refreshEstimate(); }
  const num = (v, d) => (v === '' || isNaN(+v) ? d : +v);
  function body() {
    const f = C.f, L = C.limits;
    const tc = f.mode === 'time' ? { mode: 'time', base: num(f.base, 1), inc: num(f.inc, 0) } : f.mode === 'movetime' ? { mode: 'movetime', ms: num(f.ms, 100) } : f.mode === 'nodes' ? { mode: 'nodes', n: num(f.nodes, 20000) } : { mode: 'depth', d: num(f.depth, 8) };
    const ids = C.engines.filter((e) => f.sel.includes(e.id)).map((e) => e.id);
    const b = { engines: ids, format: f.format, tc, games: num(f.games, 2), concurrency: Math.min(L.parallel_max, Math.max(1, num(f.par, 1))), seeding: f.seeding };
    if (f.name.trim()) b.name = f.name.trim();
    if (f.format === 'roundrobin' || f.format === 'gauntlet') b.rounds = num(f.rounds, 1);
    if (f.format === 'swiss') b.swiss_rounds = num(f.swiss, 5);
    if (f.format === 'knockout' || f.format === 'elimination') { b.ko_games = num(f.ko, 2); if (f.arm !== '') b.armageddon = num(f.arm, 3); }
    if (f.format === 'elimination') b.losses = num(f.losses, 2);
    return b;
  }
  function refreshEstimate() {
    clearTimeout(C.estTimer);
    C.estimate = null; renderEstimate();
    if (!C.ok) return;
    C.estTimer = setTimeout(async () => {
      const b = body();
      if (b.engines.length < 2) { C.estimate = { err: 'Pick at least two engines.' }; return renderEstimate(); }
      try { C.estimate = { total: (await api('POST', '/tournament/estimate', b)).total }; } catch (e) { C.estimate = { err: e.net ? 'The Arena does not answer.' : e.message }; }
      renderEstimate();
    }, 350);
  }
  function renderEstimate() {
    if (!U.est) return;
    const e = C.estimate, n = C.f.sel.filter((id) => C.engines.some((x) => x.id === id)).length;
    U.est.className = e && e.err ? 'est bad' : 'est';
    U.est.textContent = !C.ok ? '' : busy() ? 'A tournament is running: stop it (or wait) before starting another.' : e ? (e.err ? e.err :'About ' + e.total + ' games' + (C.f.mode === 'time' ? ' (about ' + approxTime(e.total) + ' of play in all)' : '')) : n + ' engines picked...';
    if (U.start) U.start.disabled = !C.ok || busy() || C.busy || !e || !!e.err;
  }
  function approxTime(total) {                       // a rough length: a game takes about 2 x (base + 40 x inc) seconds at most; shorter in practice
    const per = 2 * (num(C.f.base, 1) + 40 * num(C.f.inc, 0)) * 0.8, s = (total * per) / Math.max(1, Math.min(C.limits.parallel_max, num(C.f.par, 1)));
    return s < 90 ? Math.round(s) + ' s' : s < 5400 ? Math.round(s / 60) + ' min' : (s / 3600).toFixed(1) + ' h';
  }
  function pick(ids) { C.f.sel = ids; persist(); renderEngines(); refreshEstimate(); }
  const kestrel = () => C.engines.find((e) => e.builtin && /^kestrel/i.test(e.name)) || C.engines.find((e) => /^kestrel/i.test(e.name));
  const byElo = () => C.engines.filter((e) => !e.builtin).slice().sort((a, b) => (b.elo || 0) - (a.elo || 0));
  const topN = (n) => byElo().slice(0, n).map((e) => e.id);
  function kestrelTop(n) { const k = kestrel(); return [...(k ? [k.id] : []), ...byElo().filter((e) => !k || e.id !== k.id).slice(0, n).map((e) => e.id)]; }

  async function start() {
    const b = body();
    C.busy = true; renderEstimate();
    try {
      const r = await api('POST', '/tournament/create', b);
      setMsg('Started "' + r.tournament.name + '" (' + r.total + ' games). It runs in your Arena and appears below.', 'good');
      C.status = Object.assign({}, C.status, { tournament: r.tournament });
      refreshList();
      const live = $('#live'); if (live && live.scrollIntoView) live.scrollIntoView({ behavior: 'smooth', block: 'start' });
    } catch (e) { setMsg('Not started: ' + whyNot(e), 'bad'); }
    C.busy = false; renderRun(); renderEstimate();
  }
  async function act(tid, what) {
    if (what === 'stop' && !confirm('Stop this tournament? Its finished games are kept and you can continue it later.')) return;
    try { await api('POST', '/tournament/' + tid + '/' + what, {}); C.status = await api('GET', '/status'); refreshList(); setMsg('', ''); } catch (e) { setMsg(whyNot(e), 'bad'); }
    renderRun();
  }

  // ------------------------------------------------------------------------------------------------------------------ drawing
  function setMsg(t, kind) { if (U.msg) { U.msg.textContent = t; U.msg.className = 'msg ' + (kind || ''); } }
  function field(label, el, help) { return h('label', { class: 'fld' }, h('span', { class: 'lb' }, label), el, help ? h('span', { class: 'dim small' }, help) : null); }
  function inp(k, o) { return h('input', Object.assign({ class: 'inp', value: C.f[k], oninput: (e) => setF(k, e.target.value) }, o || {})); }
  function sel(k, opts) { return h('select', { class: 'inp', onchange: (e) => setF(k, e.target.value) }, opts.map(([v, t]) => h('option', { value: v, selected: String(C.f[k]) === String(v) }, t))); }

  function renderConnect() {
    U.token = h('input', { class: 'inp', type: 'password', autocomplete: 'off', spellcheck: 'false', placeholder: 'Pairing token from the Arena', value: C.token, oninput: (e) => { C.token = e.target.value.trim(); }, onkeydown: (e) => { if (e.key === 'Enter') { persist(); connect(); } } });
    U.msg = h('div', { class: 'msg' });
    U.connect.replaceChildren(h('h2', {}, '1. Connect to your Arena'), h('div', { class: 'row' }, U.token,
      h('button', { class: 'btn pri', onclick: () => { persist(); connect(); } }, 'Connect'),
      h('button', { class: 'btn', title: 'Forget the token in this browser', onclick: () => { C.token = ''; U.token.value = ''; persist(); setConnected(false); setMsg('The token was removed from this browser.', ''); } }, 'Forget token')), U.msg,
      h('details', { class: 'help', open: !C.token }, h('summary', {}, 'How to turn this on (once per Arena start)'),
        h('ol', {}, h('li', {}, 'Only the owner\'s own Arena can be controlled from here: it needs the owner key file in its data folder, made once on that PC (tools\\enable_owner_control.py). Other copies of the Arena have no such file and are not reachable.'),
          h('li', {}, 'Start the Kestrel Arena on this PC. In the Tournament tab press "Live view", then "Website control" and tick "Let my website control this Arena". It is always off when the Arena starts.'),
          h('li', {}, 'Press Copy next to the pairing token, paste it above and press Connect. The page keeps it in this browser, so next time you only switch the Arena on and press Connect.'),
          h('li', {}, 'Choose the engines, format and time control below and press Start. The tournament runs in the Arena; here you see it move by move. Pause, Resume and Stop work from here or in the Arena.')),
        h('p', {}, h('b', {}, 'Your browser may ask once '), 'whether this site may connect to devices on your local network (Chrome and Edge: "Local network access"). Choose Allow; the lock icon in the address bar > Site settings changes it later. Firefox allows it without asking. Safari does not allow a secure page to reach http://127.0.0.1: use Chrome, Edge or Firefox.'),
        h('p', {}, 'This page can only reach the Arena on the PC where the browser runs, and only while the switch is on. It cannot add or change engines, run files or read files. Anyone else who opens this page sees a form that does nothing. The token is a long random secret: do not share it, and press "Reset pairing" in the Arena if it leaks. (Every page under alon-berg.github.io shares this browser storage, so keep the switch on only while you use the page.)')));
  }
  function renderEngines() {
    if (!U.engBox) return;
    const q = C.find.toLowerCase(), ids = new Set(C.engines.map((e) => e.id));
    if (C.engines.length) C.f.sel = C.f.sel.filter((id) => ids.has(id));
    const list = C.engines.slice().sort((a, b) => (b.builtin - a.builtin) || (b.elo || 0) - (a.elo || 0));
    U.engBox.replaceChildren(...list.filter((e) => !q || e.name.toLowerCase().includes(q)).map((e) => h('label', { class: 'eng' },
      h('input', { type: 'checkbox', checked: C.f.sel.includes(e.id), onchange: (ev) => { C.f.sel = ev.target.checked ? [...C.f.sel, e.id] : C.f.sel.filter((x) => x !== e.id); persist(); U.picked.textContent = C.f.sel.length + ' picked'; refreshEstimate(); } }),
      h('span', { class: 'nm' }, e.name), h('span', { class: 'dim mono' }, e.elo || ''))));
    U.picked.textContent = C.f.sel.length + ' picked';
  }
  function renderForm() {
    const f = C.f, L = C.limits || { parallel_max: 1, losses_max: 10, games_max: 100, logical: 1 };
    U.engBox = h('div', { class: 'engbox' });
    U.picked = h('span', { class: 'dim' });
    const topIn = h('input', { class: 'inp narrow', type: 'number', min: 1, max: 127, value: f.topn, oninput: (e) => { C.f.topn = Math.max(1, Math.min(127, +e.target.value || 1)); persist(); } });
    const q = (t, fn) => h('button', { class: 'btn sm', onclick: () => pick(fn()) }, t);
    const time = f.mode === 'time';
    U.est = h('div', { class: 'est' });
    U.start = h('button', { class: 'btn pri big', disabled: true, onclick: start }, 'Start tournament');
    U.form.replaceChildren(h('h2', {}, '2. New tournament'),
      h('div', { class: 'row wrap' }, h('input', { class: 'inp grow', type: 'search', placeholder: 'Find an engine...', value: C.find, oninput: (e) => { C.find = e.target.value; renderEngines(); } }),
        h('span', { class: 'dim' }, 'N ='), topIn, q('Kestrel + top N', () => kestrelTop(+topIn.value || 7)), q('Top N', () => topN(+topIn.value || 8)), q('All', () => C.engines.map((e) => e.id)), q('None', () => []), U.picked),
      U.engBox,
      h('div', { class: 'cols' },
        h('div', {}, field('Format', sel('format', Object.entries(FORMATS))),
          f.format === 'roundrobin' || f.format === 'gauntlet' ? field('Rounds', inp('rounds', { type: 'number', min: 1, max: 10 }), 'Every pair plays this many times') : null,
          f.format === 'swiss' ? field('Swiss rounds', inp('swiss', { type: 'number', min: 1, max: 30 })) : null,
          f.format === 'knockout' || f.format === 'elimination' ? field('Games per match', inp('ko', { type: 'number', min: 2, max: 100, step: 2 }), 'Even: both colours') : null,
          f.format === 'elimination' ? field('Losses to be out', inp('losses', { type: 'number', min: 1, max: L.losses_max })) : null,
          f.format === 'knockout' || f.format === 'elimination' ? field('Armageddon', sel('arm', [['', 'Never (better seed goes on)'], ['0', 'Right after the games of the match'], ...[1, 2, 3, 4, 5, 6, 7, 8].map((n) => [String(n), 'After ' + n + ' extra pair' + (n > 1 ? 's' : '')])]), 'One game decides a level match; a draw counts as a win for Black') : null,
          f.format !== 'knockout' && f.format !== 'elimination' ? field('Games per pairing', inp('games', { type: 'number', min: 1, max: L.games_max }), '2 = both colours') : null),
        h('div', {}, field('Time control', sel('mode', [['time', 'Time + increment'], ['movetime', 'Fixed time per move'], ['nodes', 'Fixed nodes per move'], ['depth', 'Fixed depth']])),
          time ? h('div', { class: 'row' }, field('Preset', h('select', { class: 'inp', onchange: (e) => { const p = PRESETS[+e.target.value]; if (p) { C.f.base = p[1]; C.f.inc = p[2]; persist(); renderForm(); refreshEstimate(); } } },
            h('option', { value: '' }, 'choose...'), PRESETS.map((p, i) => h('option', { value: i }, p[0])))), field('Base (s)', inp('base', { type: 'number', min: 0.05, max: 600, step: 'any' })), field('Increment (s)', inp('inc', { type: 'number', min: 0, max: 60, step: 'any' }))) : null,
          f.mode === 'movetime' ? field('Milliseconds per move', inp('ms', { type: 'number', min: 5, max: 60000 })) : null,
          f.mode === 'nodes' ? field('Nodes per move', inp('nodes', { type: 'number', min: 100, max: 5000000 })) : null,
          f.mode === 'depth' ? field('Depth', inp('depth', { type: 'number', min: 1, max: 30 })) : null,
          field('Parallel games', inp('par', { type: 'number', min: 1, max: L.parallel_max }), C.limits ? 'At most ' + L.parallel_max + ' (half of the ' + L.logical + ' logical CPUs). Fewer is more accurate for fast games.' : ''),
          field('Name (optional)', inp('name', { maxlength: 60, placeholder: 'Website tournament...' })))),
      h('div', { class: 'row' }, U.start, U.est));
    renderEngines();
    renderEstimate();
  }
  function buildRun() {                             // built once and updated in place: a button that is rebuilt while it is being pressed would swallow the click
    const R = (U.runEls = { name: h('b'), chip: h('span', { class: 'chip' }), info: h('span', { class: 'dim' }), none: h('div', { class: 'dim' }, 'No tournament is loaded in the Arena.'),
      pause: h('button', { class: 'btn', onclick: () => act(R.id, 'pause') }, 'Pause'), resume: h('button', { class: 'btn', onclick: () => act(R.id, 'resume') }, 'Resume'), stop: h('button', { class: 'btn bad', onclick: () => act(R.id, 'stop') }, 'Stop') });
    R.row = h('div', { class: 'row wrap' }, R.name, R.chip, R.info, h('span', { class: 'sp' }), R.pause, R.resume, R.stop);
    U.run.replaceChildren(h('h2', {}, '3. Running tournament'), R.row, R.none);
  }
  function renderRun() {
    if (!U.run) return;
    if (!U.runEls) buildRun();
    const R = U.runEls, t = C.status && C.status.tournament, on = t && ['running', 'paused', 'stopping'].includes(t.status);
    R.row.classList.toggle('hidden', !t); R.none.classList.toggle('hidden', !!t);
    if (t) {
      R.id = t.id;
      R.name.textContent = t.name || t.id;
      R.chip.textContent = t.status; R.chip.className = 'chip ' + (t.status === 'running' ? 'good' : t.status === 'paused' ? 'warn' : t.status === 'finished' ? 'acc' : t.status === 'error' ? 'bad' : '');
      R.info.textContent = t.done + ' / ' + (t.total || '?') + ' games - ' + t.tc_label + ' - ' + (t.concurrency || 1) + ' parallel';
      R.pause.disabled = !C.ok || t.status !== 'running'; R.resume.disabled = !C.ok || t.status !== 'paused'; R.stop.disabled = !C.ok || !on;
    }
    renderEstimate();
  }
  function renderList() {
    if (!U.list) return;
    const idle = !busy(), sig = JSON.stringify([C.list, idle, C.ok]);
    if (sig === C.listSig) return;                  // (nothing changed: leave the buttons alone)
    C.listSig = sig;
    U.list.replaceChildren(h('h2', {}, 'Tournaments in the Arena'), C.list.length ? h('div', { class: 'tblwrap' }, h('table', { class: 'tbl' }, h('thead', {}, h('tr', {}, h('th', {}, 'Name'), h('th', {}, 'Format'), h('th', {}, 'Status'), h('th', { class: 'r' }, 'Games'), h('th', {}, 'Started'), h('th', {}))),
      h('tbody', {}, C.list.slice(0, 20).map((t) => h('tr', {}, h('td', { class: 'nm' }, t.name || t.id), h('td', {}, t.format === 'elimination' ? (t.losses || 2) + '-loss elimination' : t.format), h('td', {}, t.status), h('td', { class: 'r mono' }, t.games),
        h('td', { class: 'dim' }, (t.created || '').replace('T', ' ').slice(0, 16)), h('td', {}, t.status === 'stopped' ? h('button', { class: 'btn sm', disabled: !idle, title: 'Continue where it stopped', onclick: () => act(t.id, 'start') }, 'Continue') : null)))))) : h('div', { class: 'dim' }, 'None yet.'));
  }
  function renderAll() {
    const off = !C.ok;
    for (const el of [U.form, U.run, U.list]) el.classList.toggle('disabled', off);
    renderForm(); renderRun(); renderList();
    if (U.runEls) for (const b of [U.runEls.pause, U.runEls.resume, U.runEls.stop]) if (off) b.disabled = true;
    U.form.querySelectorAll('input,select,button').forEach((x) => { if (off) x.disabled = true; });
    if (!off) renderEstimate();
    if (!off) refreshEstimate();
  }

  function init() {
    const root = $('#ctrl');
    U.cstate = $('#cstate');
    U.connect = h('section', { class: 'card' }), U.form = h('section', { class: 'card' }), U.run = h('section', { class: 'card' }), U.list = h('section', { class: 'card' });
    root.replaceChildren(h('div', { class: 'wrap' }, U.connect, U.form, U.run, U.list));
    renderConnect();
    document.body.classList.add('off');
    U.cstate.textContent = 'not connected'; U.cstate.className = 'chip bad';
    renderAll();
    if (C.token) connect(false);
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init); else init();
})();
