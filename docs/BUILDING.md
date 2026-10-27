# Building Kestrel for other CPUs and operating systems

The engine is one C++17 file plus two generated network headers (`src/`). The plain build in the README (`-march=x86-64-v3`, Windows, MinGW-w64) is the one that went through the full test suite.
All other release builds use the same source with up to three mechanical substitutions, done by `scripts/patch_source.py` on a **copy** (`src/` is never edited):

| Flag | What it does | Needed for |
|---|---|---|
| `--portable` | `_pext_u64` (BMI2) is replaced by a plain-C function and `pextIsFast()` returns false, so no BMI2 instruction is ever executed. The v3 build uses PEXT only on Intel and AMD Zen 3+ and otherwise the shift-based attack tables, so the move generator and the search tree are the same: the depth-12 bench is 439,247 nodes in every build. | every build except x86-64-v3 |
| `--bigstack` | `std::thread` becomes a small pthread wrapper with an 8 MB stack. The default thread stack is 128 KB with musl and 512 KB on macOS; one search frame is about 2.7 KB and a search can go 128 plies deep (a test build with a 128 KB stack died with a stack overflow in 6 of 8 test positions, at depth 21 to 31). | Linux (musl) and macOS |
| `--shim` | stub `<immintrin.h>` (only `_mm_prefetch`) and `<cpuid.h>` for ARM targets; pass `-I OUTDIR/shim`. | ARM (macOS Apple Silicon, Linux arm64) |

The opening book `claude_book.txt` (copied next to the patched source) must be placed next to the executable; on Linux and macOS the executable's directory is taken from `argv[0]`, so start the engine with its full path (GUIs do).

## Windows (MinGW-w64 g++, Windows 10 or newer, 64-bit)

```
python scripts/patch_source.py build_v2 --portable
g++ -std=c++17 -O3 -march=x86-64-v2 -static -pthread -DNDEBUG -o kestrel.exe build_v2/kestrel_1_0.cpp
```

`-march=x86-64-v2` needs SSE4.2 and POPCNT, plain `-march=x86-64` runs on any 64-bit CPU. The x86-64-v3 build is the README one (no patch).

## Linux and macOS with Zig as the cross compiler (what the release builds use)

```
python scripts/patch_source.py build_linux --portable --bigstack
zig c++ -std=c++17 -O3 -target x86_64-linux-musl -mcpu=x86_64_v2 -pthread -DNDEBUG -o kestrel build_linux/kestrel_1_0.cpp

python scripts/patch_source.py build_arm --portable --bigstack --shim
zig c++ -std=c++17 -O3 -target aarch64-linux-musl -mcpu=generic -pthread -DNDEBUG -I build_arm/shim -o kestrel build_arm/kestrel_1_0.cpp

zig c++ -std=c++17 -O3 -target x86_64-macos.10.15 -mcpu=x86_64_v2 -pthread -DNDEBUG -o kestrel build_linux/kestrel_1_0.cpp
zig c++ -std=c++17 -O3 -target aarch64-macos.11.0 -mcpu=apple_m1 -pthread -DNDEBUG -I build_arm/shim -o kestrel build_arm/kestrel_1_0.cpp
```

Release builds used Zig 0.15.2. For x86-64-v3 on Linux leave out `--portable` and use `-mcpu=x86_64_v3`; for any x86 CPU use `-mcpu=x86_64`.

## Natively on Linux or macOS

```
python scripts/patch_source.py build --portable --bigstack
g++ -std=c++17 -O3 -march=native -pthread -DNDEBUG -o kestrel build/kestrel_1_0.cpp        # or clang++; on ARM also pass --shim to the script and -I build/shim to the compiler
```

(With `-march=native` on an x86 CPU that has BMI2 you can leave out `--portable`.)

## What was and was not tested

* Windows x86-64-v3: the full test suite of the release (27-check verification, fuzzing, 303 edge cases) and 6,400 + 1,600 + 1,200 games against Claude 39.
* Windows x86-64-v2 and x86-64: bench equal to v3 (439,247 nodes, depth 12), a UCI session (`go infinite` + `stop`, `movetime`, `depth`, MultiPV, 4 threads) and a disassembly check that no AVX, BMI or (baseline) POPCNT/SSE4 instruction is present.
* The pthread wrapper was compiled and run on Windows (winpthreads): 8 endgame/middlegame positions on 1 and 4 threads with a deliberately tiny 128 KB main stack, `stop`, `movetime`, MultiPV.
* Linux and macOS builds were compiled with Zig and checked for file format (ELF static / Mach-O, ad-hoc signature on arm64) but **never run**: no Linux or macOS machine was available.
