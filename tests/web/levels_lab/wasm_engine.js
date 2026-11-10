// Runs the real WebAssembly Kestrel of the play page under Deno (same wasm file, same network, same UCI conversation as docs/play/engine-worker.js).
//   the network: environment variable KESTREL_NET (default: kestrel_net_3_2.bin in the current folder)
import { instantiate } from '../../../docs/play/kestrel-wasi.js';

const PLAY = new URL('../../../docs/play/', import.meta.url);
export const DEFAULT_NET = Deno.env.get('KESTREL_NET') || 'kestrel_net_3_2.bin';

export async function startWasm(opts = {}) {
  const wasm = await Deno.readFile(new URL(opts.simd === false ? 'kestrel-nosimd.wasm' : 'kestrel.wasm', PLAY));
  const net = await Deno.readFile(opts.net || DEFAULT_NET);
  let out = [];
  const inst = await instantiate(wasm, { onStdout: (l) => out.push(l), onStderr: () => {} });
  const e = inst.exports, enc = new TextEncoder();
  const ptr = e.kestrel_net_buffer(net.length);        // (growing the memory detaches the old buffer: read inst.memory.buffer after the call)
  new Uint8Array(inst.memory.buffer, ptr, net.length).set(net);
  if (!e.kestrel_init(opts.hash || 16)) throw new Error('the network was not accepted');
  function uci(cmd) {
    const p = e.kestrel_cmd_buffer(), b = enc.encode(cmd + '\0');
    new Uint8Array(inst.memory.buffer, p, b.length).set(b);
    e.kestrel_uci(); inst.flush();
  }
  return {
    uci,
    /* same shape as Engine.run of the page: commands in, every printed line out */
    async run(cmds, onLine) {
      out = [];
      for (const c of cmds) uci(c);
      const lines = out; out = [];
      if (onLine) for (const l of lines) onLine(l);
      return lines;
    },
  };
}

/* seeded random numbers (mulberry32) so that runs can be repeated */
export function makeRng(seed) {
  let a = (seed >>> 0) || 1;
  return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}
