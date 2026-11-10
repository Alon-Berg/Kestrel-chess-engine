/* Minimal WASI layer for the Kestrel engine compiled to WebAssembly (wasm32-wasi, reactor model).
   The engine only needs a clock, a random source and stdout (UCI replies); there are no files and no threads.
   Used by the worker of the play page and by the command line tests (Deno / Node). */
export class ProcExit extends Error {
  constructor(code) { super('exit ' + code); this.code = code; }
}

export function makeImports(module, getMemory, hooks = {}) {
  const onOut = hooks.onStdout || (() => {}), onErr = hooks.onStderr || (() => {});
  const dec = new TextDecoder();
  const partial = { 1: '', 2: '' };
  const view = () => new DataView(getMemory().buffer);
  const bytes = () => new Uint8Array(getMemory().buffer);
  const ENOSYS = 52, EBADF = 8, ENOENT = 44, ESPIPE = 70;

  function emit(fd, text) {
    partial[fd] += text;
    let nl;
    while ((nl = partial[fd].indexOf('\n')) >= 0) {
      const line = partial[fd].slice(0, nl).replace(/\r$/, '');
      partial[fd] = partial[fd].slice(nl + 1);
      (fd === 1 ? onOut : onErr)(line);
    }
  }

  const wasi = {
    args_sizes_get: (argc, size) => { const v = view(); v.setUint32(argc, 0, true); v.setUint32(size, 0, true); return 0; },
    args_get: () => 0,
    environ_sizes_get: (n, size) => { const v = view(); v.setUint32(n, 0, true); v.setUint32(size, 0, true); return 0; },
    environ_get: () => 0,
    clock_res_get: (id, ptr) => { view().setBigUint64(ptr, 1000n, true); return 0; },
    clock_time_get: (id, precision, ptr) => {
      const ns = id === 0 ? BigInt(Math.round(Date.now())) * 1000000n : BigInt(Math.round(performance.now() * 1e6));
      view().setBigUint64(ptr, ns, true); return 0;
    },
    fd_write: (fd, iovs, iovsLen, nwritten) => {
      if (fd !== 1 && fd !== 2) return EBADF;
      const v = view(), m = bytes(); let total = 0, text = '';
      for (let i = 0; i < iovsLen; i++) {
        const p = v.getUint32(iovs + i * 8, true), n = v.getUint32(iovs + i * 8 + 4, true);
        if (n) text += dec.decode(m.subarray(p, p + n), { stream: true });
        total += n;
      }
      v.setUint32(nwritten, total, true);
      emit(fd, text);
      return 0;
    },
    fd_read: (fd, iovs, iovsLen, nread) => { view().setUint32(nread, 0, true); return 0; },
    fd_pread: () => ENOSYS, fd_pwrite: () => ENOSYS, fd_tell: () => ESPIPE,
    fd_seek: () => ESPIPE,
    fd_close: () => 0,
    fd_sync: () => 0,
    fd_fdstat_get: (fd, ptr) => {
      if (fd > 2) return EBADF;
      const v = view();
      v.setUint8(ptr, 2); v.setUint8(ptr + 1, 0); v.setUint16(ptr + 2, 0, true);        // character device, no flags
      v.setBigUint64(ptr + 8, 0xFFFFFFFFn, true); v.setBigUint64(ptr + 16, 0n, true);
      return 0;
    },
    fd_fdstat_set_flags: () => 0,
    fd_filestat_get: () => ENOSYS,
    fd_prestat_get: () => EBADF,
    fd_prestat_dir_name: () => EBADF,
    path_open: () => ENOENT,
    path_filestat_get: () => ENOENT,
    path_unlink_file: () => ENOENT, path_remove_directory: () => ENOENT, path_create_directory: () => ENOENT,
    poll_oneoff: () => ENOSYS,
    sched_yield: () => 0,
    random_get: (ptr, len) => {
      const m = bytes();
      for (let off = 0; off < len; off += 65536) crypto.getRandomValues(m.subarray(ptr + off, ptr + Math.min(len, off + 65536)));
      return 0;
    },
    proc_exit: (code) => { throw new ProcExit(code); },
  };
  const imports = { wasi_snapshot_preview1: {} };
  for (const imp of WebAssembly.Module.imports(module)) {
    if (imp.module !== 'wasi_snapshot_preview1') throw new Error('unexpected import ' + imp.module + '.' + imp.name);
    imports.wasi_snapshot_preview1[imp.name] = wasi[imp.name] || (() => ENOSYS);
  }
  imports.flush = () => { for (const fd of [1, 2]) if (partial[fd]) { const s = partial[fd]; partial[fd] = ''; (fd === 1 ? onOut : onErr)(s); } };
  return imports;
}

/* Loads the module, runs its initialisers and returns {exports, memory, flush}. */
export async function instantiate(wasmBytesOrModule, hooks = {}) {
  const module = wasmBytesOrModule instanceof WebAssembly.Module ? wasmBytesOrModule : await WebAssembly.compile(wasmBytesOrModule);
  let memory = null;
  const imports = makeImports(module, () => memory, hooks);
  const flush = imports.flush; delete imports.flush;
  const instance = await WebAssembly.instantiate(module, imports);
  memory = instance.exports.memory;
  if (instance.exports._initialize) instance.exports._initialize();
  return { exports: instance.exports, memory, flush };
}
