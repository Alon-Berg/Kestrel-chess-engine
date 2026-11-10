/* Page side of the engine: starts the worker and runs UCI command batches.  The engine thinks inside the worker, so the page stays responsive;
   a search cannot be interrupted (a running search ends by its own time limit), the page just ignores a result it no longer wants. */
export class Engine {
  constructor(cfg) {
    this.cfg = cfg;
    this.worker = null;
    this.ready = false;
    this.failed = null;
    this.simd = null;
    this.nextId = 1;
    this.jobs = new Map();
    this.readyWaiters = [];
    this.onProgress = cfg.onProgress || (() => {});
    this.onState = cfg.onState || (() => {});
  }

  start() {
    if (this.worker) return;
    const c = this.cfg;
    try { this.worker = new Worker(new URL('./engine-worker.js', import.meta.url), { type: 'module' }); }
    catch (e) { this._fail('this browser cannot start the engine worker (' + e.message + ')'); return; }
    this.worker.onmessage = (ev) => this._message(ev.data);
    this.worker.onerror = (ev) => this._fail('the engine worker stopped (' + (ev.message || 'unknown error') + ')');
    const plain = {};
    for (const [k, v] of Object.entries(c)) if (typeof v !== 'function') plain[k] = v;
    // the URLs are made absolute here: the worker may live elsewhere than the page
    for (const k of ['wasmUrl', 'wasmNoSimdUrl', 'netUrl']) plain[k] = new URL(plain[k], location.href).href;
    this.worker.postMessage(Object.assign({ type: 'init' }, plain));
  }

  /* a fresh worker (after a crash of the engine, or a failed start); the network comes from the browser's cache storage when it was stored */
  restart() {
    if (this.worker) { this.worker.terminate(); this.worker = null; }
    const err = new Error('the engine was restarted');
    for (const j of this.jobs.values()) j.reject(err);
    for (const w of this.readyWaiters) w.reject(err);
    this.jobs.clear(); this.readyWaiters = [];
    this.ready = false; this.failed = null; this.broken = false;
    this.start();
  }

  _fail(message) {
    this.failed = message;
    for (const j of this.jobs.values()) j.reject(new Error(message));
    this.jobs.clear();
    for (const w of this.readyWaiters) w.reject(new Error(message));
    this.readyWaiters = [];
    this.onState('failed', message);
  }

  _message(m) {
    if (m.type === 'progress') this.onProgress(m);
    else if (m.type === 'ready') {
      this.ready = true; this.simd = m.simd;
      this.onState('ready', m);
      for (const w of this.readyWaiters) w.resolve();
      this.readyWaiters = [];
    } else if (m.type === 'line') {
      // lines belong to the oldest job that has not finished yet (the worker runs the jobs in order)
      const first = this.jobs.values().next().value;
      if (first && first.onLine) first.onLine(m.line);
      if (first) first.lines.push(m.line);
    } else if (m.type === 'done') {
      const j = this.jobs.get(m.id);
      if (j) { this.jobs.delete(m.id); j.resolve(j.lines); }
    } else if (m.type === 'error') {
      if (m.id && this.jobs.has(m.id)) { this.broken = true; const j = this.jobs.get(m.id); this.jobs.delete(m.id); j.reject(new Error(m.message)); }
      else this._fail(m.message);
    }
  }

  whenReady() {
    if (this.failed) return Promise.reject(new Error(this.failed));
    if (this.ready) return Promise.resolve();
    return new Promise((resolve, reject) => this.readyWaiters.push({ resolve, reject }));
  }

  /* Runs the commands in order; resolves with all lines the engine printed.  onLine sees the lines while the search is running. */
  async run(cmds, onLine) {
    await this.whenReady();
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.jobs.set(id, { resolve, reject, onLine, lines: [] });
      this.worker.postMessage({ type: 'run', id, cmds });
    });
  }
}

/* "info depth 12 score cp 23 nodes 63397 nps 189244 hashfull 1 time 335 pv d2d4 d7d5 ..." -> object */
export function parseInfo(line) {
  if (!line.startsWith('info ') || !/\bdepth \d+/.test(line) || !/\bscore (cp|mate) -?\d+/.test(line)) return null;
  const num = (re) => { const m = line.match(re); return m ? +m[1] : null; };
  const sc = line.match(/\bscore (cp|mate) (-?\d+)/);
  const pv = line.match(/\bpv ((?:[a-h][1-8][a-h][1-8][qrbn]? ?)+)/);
  return {
    depth: num(/\bdepth (\d+)/), multipv: num(/\bmultipv (\d+)/) || 1, nodes: num(/\bnodes (\d+)/), nps: num(/\bnps (\d+)/), time: num(/\btime (\d+)/),
    mate: sc[1] === 'mate', score: +sc[2], pv: pv ? pv[1].trim().split(' ') : [],
  };
}
