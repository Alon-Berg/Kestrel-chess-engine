/* Web worker that runs Kestrel (WebAssembly).  It downloads the network (once: the browser keeps a copy in its cache storage), starts the engine and then
   executes UCI commands one after the other; every line the engine prints goes back to the page.
   Messages in : {type:'init', wasmUrl, wasmNoSimdUrl, netUrl, netSize, cacheName, hash}   {type:'run', id, cmds:[...]}
   Messages out: {type:'progress', phase, loaded, total}  {type:'ready'}  {type:'line', line}  {type:'done', id}  {type:'error', message, id?} */
import { instantiate } from './kestrel-wasi.js';

let inst = null;
const encoder = new TextEncoder();
const SIMD_PROBE = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 123, 3, 2, 1, 0, 10, 10, 1, 8, 0, 65, 0, 253, 15, 253, 98, 11]);

function progress(phase, loaded, total) { postMessage({ type: 'progress', phase, loaded, total }); }

async function fetchBytesWithProgress(url, phase, guessTotal) {
  const resp = await fetch(url, { cache: 'no-cache' });
  if (!resp.ok) throw new Error('could not load ' + url + ' (HTTP ' + resp.status + ')');
  const total = +resp.headers.get('content-length') || guessTotal || 0;
  const reader = resp.body.getReader(), chunks = [];
  let loaded = 0, last = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value); loaded += value.length;
    const now = performance.now();
    if (now - last > 100) { last = now; progress(phase, loaded, total); }
  }
  progress(phase, loaded, total);
  return { chunks, loaded };
}

/* the network as a stream of compressed (or raw) bytes: from the cache storage when it is there, else from the server (and then stored) */
async function netBlob(msg) {
  let cache = null;
  try {
    cache = await caches.open(msg.cacheName);
    for (const k of await caches.keys()) if (k.startsWith('kestrel-net-') && k !== msg.cacheName) caches.delete(k);
    const hit = await cache.match(msg.netUrl);
    if (hit) { const b = await hit.blob(); if (b.size > 1000000) { progress('cache', b.size, b.size); return b; } }
  } catch (e) { cache = null; }
  const { chunks, loaded } = await fetchBytesWithProgress(msg.netUrl, 'download', msg.netDownloadSize);
  const blob = new Blob(chunks);
  if (cache && loaded > 1000000) { try { await cache.put(msg.netUrl, new Response(blob)); } catch (e) { /* no storage: next visit downloads again */ } }
  return blob;
}

/* The packed file (tools/pack_net.py) is: [shuffleStart header bytes][shuffleCount low bytes][shuffleCount high bytes][rest], gzip.
   Every incoming piece goes straight to its place in the engine's memory; the result is checked against the SHA-256 of the network. */
async function loadNet(msg, wasmExports, memory) {
  const blob = await netBlob(msg);
  const head = new Uint8Array(await blob.slice(0, 2).arrayBuffer());
  let stream = blob.stream();
  if (head[0] === 0x1f && head[1] === 0x8b) {
    if (typeof DecompressionStream === 'undefined') throw new Error('this browser cannot unpack gzip data (DecompressionStream is missing)');
    stream = stream.pipeThrough(new DecompressionStream('gzip'));
  }
  const total = msg.netSize, S = msg.shuffleStart, N = msg.shuffleCount, loEnd = S + N, hiEnd = S + 2 * N;
  const ptr = wasmExports.kestrel_net_buffer(total);
  if (!ptr) throw new Error('not enough memory for the network');
  const reader = stream.getReader();
  let at = 0, last = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    const len = value.length;
    if (at + len > total) throw new Error('the network file is larger than expected');
    const mem = new Uint8Array(memory.buffer);
    let i = 0;
    while (i < len) {
      const pos = at + i;
      let n;
      if (pos < S) { n = Math.min(len - i, S - pos); mem.set(value.subarray(i, i + n), ptr + pos); }
      else if (pos < loEnd) { n = Math.min(len - i, loEnd - pos); for (let k = 0, d = ptr + S + 2 * (pos - S); k < n; k++, d += 2) mem[d] = value[i + k]; }
      else if (pos < hiEnd) { n = Math.min(len - i, hiEnd - pos); for (let k = 0, d = ptr + S + 2 * (pos - loEnd) + 1; k < n; k++, d += 2) mem[d] = value[i + k]; }
      else { n = len - i; mem.set(value.subarray(i, i + n), ptr + pos); }
      i += n;
    }
    at += len;
    const now = performance.now();
    if (now - last > 100) { last = now; progress('unpack', at, total); }
  }
  if (at !== total) throw new Error('the network file has the wrong size (' + at + ' bytes)');
  progress('unpack', at, total);
  if (msg.netSha256 && crypto.subtle) {
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', new Uint8Array(memory.buffer, ptr, total)));
    const hex = Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
    if (hex !== msg.netSha256) {
      try { await caches.delete(msg.cacheName); } catch (e) { /* ignore */ }
      throw new Error('the network file is damaged (checksum mismatch); reload the page to try again');
    }
  }
}

function uci(cmd) {
  const p = inst.exports.kestrel_cmd_buffer();
  const bytes = encoder.encode(cmd + '\0');
  if (bytes.length > 60000) throw new Error('command too long');
  new Uint8Array(inst.memory.buffer, p, bytes.length).set(bytes);
  inst.exports.kestrel_uci();
  inst.flush();
}

async function init(msg) {
  if (typeof WebAssembly === 'undefined') throw new Error('this browser has no WebAssembly');
  const simd = WebAssembly.validate(SIMD_PROBE);
  const wasmUrl = simd ? msg.wasmUrl : msg.wasmNoSimdUrl;
  let module;
  try { module = await WebAssembly.compileStreaming(fetch(wasmUrl)); }
  catch (e) { module = await WebAssembly.compile(await (await fetch(wasmUrl)).arrayBuffer()); }
  inst = await instantiate(module, { onStdout: (line) => postMessage({ type: 'line', line }), onStderr: () => {} });
  await loadNet(msg, inst.exports, inst.memory);
  if (!inst.exports.kestrel_init(msg.hash)) throw new Error('the engine did not accept the network file');
  postMessage({ type: 'ready', simd });
}

self.onmessage = async (ev) => {
  const msg = ev.data;
  try {
    if (msg.type === 'init') await init(msg);
    else if (msg.type === 'run') {
      for (const c of msg.cmds) uci(c);
      postMessage({ type: 'done', id: msg.id });
    }
  } catch (e) {
    postMessage({ type: 'error', message: String((e && e.message) || e), id: msg.id });
  }
};
