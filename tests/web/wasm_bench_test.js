// The WebAssembly engine against the native one: the 8-position bench to depth 12 must search exactly 859,322 nodes (as every native build of 3.2), and a fixed UCI search
// must give the same node count as the native program (63,397 nodes to depth 12 from the start position).  Both files are tested: kestrel.wasm (SIMD) and kestrel-nosimd.wasm.
//   deno run --allow-read tests/web/wasm_bench_test.js <kestrel_net.bin> [folder with the .wasm files = docs/play]
import { instantiate } from '../../docs/play/kestrel-wasi.js';

const NET = Deno.args[0];
if (!NET) { console.log('usage: deno run --allow-read tests/web/wasm_bench_test.js <kestrel_net.bin> [folder]'); Deno.exit(2); }
const DIR = Deno.args[1] || new URL('../../docs/play/', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const net = await Deno.readFile(NET);
let fails = 0;

for (const name of ['kestrel.wasm', 'kestrel-nosimd.wasm']) {
  const wasm = await Deno.readFile(DIR + '/' + name);
  let out = [];
  const inst = await instantiate(wasm, { onStdout: (l) => out.push(l) });
  const e = inst.exports;
  const ptr = e.kestrel_net_buffer(net.length);
  new Uint8Array(inst.memory.buffer, ptr, net.length).set(net);
  if (!e.kestrel_init(256)) throw new Error('the network was not accepted');
  const t = performance.now();
  const nodes = Number(e.kestrel_bench(12));
  const ms = performance.now() - t;
  const ok1 = nodes === 859322;
  console.log('%s: bench 12 = %d nodes %s (%d ms, %d knps)', name, nodes, ok1 ? 'ok' : 'WRONG, expected 859322', Math.round(ms), Math.round(nodes / ms));
  const run = (cmd) => {
    const p = e.kestrel_cmd_buffer(), enc = new TextEncoder().encode(cmd + '\0');
    new Uint8Array(inst.memory.buffer, p, enc.length).set(enc);
    out = []; e.kestrel_uci(); inst.flush(); return out;
  };
  run('ucinewgame'); run('position startpos');
  const info = run('go depth 12').filter((l) => l.startsWith('info depth 12 '))[0] || '';
  const n12 = +((info.match(/ nodes (\d+)/) || [])[1]);
  const ok2 = n12 === 63397;
  console.log('%s: startpos depth 12 = %d nodes %s', name, n12, ok2 ? 'ok' : 'WRONG, expected 63397');
  const refused = run('go infinite').join(' ');
  const ok3 = /needs a limit/.test(refused);
  console.log('%s: go infinite is refused: %s', name, ok3 ? 'ok' : 'NO');
  if (!(ok1 && ok2 && ok3)) fails++;
}
console.log(fails ? 'FAILED' : 'ALL OK');
Deno.exit(fails ? 1 : 0);
