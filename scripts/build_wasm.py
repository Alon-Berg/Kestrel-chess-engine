r"""Compiles src/kestrel_3_2_wasm.cpp (scripts/make_wasm.py) to docs/play/kestrel.wasm (with WebAssembly SIMD) and docs/play/kestrel-nosimd.wasm with Zig 0.15 (clang).
usage: python scripts/build_wasm.py [--zig path/to/zig] [--src src/kestrel_3_2_wasm.cpp] [--out-dir docs/play]
The Zig compiler is found with --zig, the environment variable ZIG or on the PATH.  A Zig that cannot build wasm32-wasi will not do: https://ziglang.org/download/ (0.15.x).
Compiling takes about 20 seconds per file.  The network is not part of the files: see scripts/pack_net.py."""
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
opts = {"--zig": os.environ.get("ZIG") or shutil.which("zig") or "", "--src": os.path.join(ROOT, "src", "kestrel_3_2_wasm.cpp"), "--out-dir": os.path.join(ROOT, "docs", "play")}
a = sys.argv[1:]
while a:
    k = a.pop(0)
    assert k in opts and a, "usage: see the head of this file"
    opts[k] = a.pop(0)
if not opts["--zig"]:
    sys.exit("Zig not found: pass --zig, set ZIG or put zig on the PATH")
base = [opts["--zig"], "c++", "-target", "wasm32-wasi", "-std=c++17", "-O3", "-DNDEBUG", "-fno-exceptions", "-mexec-model=reactor",
        "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
        "-Wl,-z,stack-size=8388608", "-Wl,--initial-memory=33554432", "-Wl,--max-memory=2147483648", "-Wl,--gc-sections", "-s"]
for name, extra in (("kestrel.wasm", ["-msimd128"]), ("kestrel-nosimd.wasm", [])):
    out = os.path.join(opts["--out-dir"], name)
    t = time.time()
    r = subprocess.run(base + extra + ["-o", out, opts["--src"]], capture_output=True, text=True)
    print("%s: exit code %d after %.0f s, %s" % (name, r.returncode, time.time() - t, "%d bytes" % os.path.getsize(out) if r.returncode == 0 else "failed"))
    if r.returncode:
        sys.exit((r.stderr or r.stdout)[-3000:])
