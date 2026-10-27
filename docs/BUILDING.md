# Building Kestrel for other CPUs and operating systems

The engine is one C++17 file plus two generated network headers (`src/`). Since Kestrel 1.1 the source builds as it is for every target; no patch step is needed
(Kestrel 1.0 needed three mechanical substitutions for non-x86-64-v3 builds, see the history of this file).

| Target | Compile with | Notes |
|---|---|---|
| Windows x86-64-v3 (AVX2 + BMI2) | `g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel.exe src/kestrel_1_1.cpp` | the build of the full test suite |
| Windows x86-64-v2 (SSE4.2 + POPCNT) | same with `-march=x86-64-v2` | PEXT is not compiled in; about 30 % slower |
| Windows any 64-bit CPU | same with `-march=x86-64` | about 35 % slower |
| Linux / macOS, natively | `g++ -std=c++17 -O3 -march=native -pthread -DNDEBUG -o kestrel src/kestrel_1_1.cpp` (or `clang++`) | x86-64 and ARM64 |
| Cross compiling with Zig 0.15.2 | `zig c++ -std=c++17 -O3 -target x86_64-linux-musl -mcpu=x86_64_v3 -pthread -DNDEBUG -o kestrel src/kestrel_1_1.cpp` | targets used for the releases: `x86_64-linux-musl` (`-mcpu=x86_64_v3`, `x86_64_v2`, `x86_64`), `aarch64-linux-musl` (`-mcpu=generic`), `x86_64-macos.10.15` (`-mcpu=x86_64_v2`), `aarch64-macos.11.0` (`-mcpu=apple_m1`) |

Put `claude_book.txt` next to the executable to use the opening book (the file keeps its old name; it is not read when `OwnBook` is false). The book and the experience file are looked up in the folder of the real
executable (`GetModuleFileNameW` on Windows, `/proc/self/exe` on Linux, `_NSGetExecutablePath` on macOS).

The Windows release exes are additionally stripped (`-s`) and carry a version-info block (`scripts/kestrel.rc`):

```
windres scripts/kestrel.rc -O coff -o kestrel_rc.o
g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -s -o kestrel.exe src/kestrel_1_1.cpp kestrel_rc.o
```

This changes no code or data: every section of the stripped x86-64-v3 exe is byte-identical to the fully tested one, only the symbol table, the version info and the timestamp differ. It was done because
Microsoft's machine-learning heuristic flagged the first, unstripped Kestrel 1.0 exe on VirusTotal (1 of 71 engines, `Trojan:Win32/Wacatac.C!ml`, a false positive); the stripped exe with version info scored 0 of 71.
The executables are not code-signed.

## What the code does for portability (Kestrel 1.1)

* PEXT (BMI2) attack tables exist only when the compiler targets BMI2 (`__BMI2__`); they are used on Intel and on AMD Zen 3 and newer, and everywhere else the shift-based tables. Node counts are identical either way.
* `<cpuid.h>` only on x86; the transposition-table prefetch is `__builtin_prefetch`.
* Off Windows, search threads start with an 8 MB stack (`KThread`, a small pthread wrapper): musl gives threads 128 KB and macOS 512 KB, one search frame is about 2.7 KB and a search can go 128 plies deep.

## What was and was not tested

* Windows x86-64-v3: the full test suite of the release (27-check verification with AddressSanitizer / UBSan, long fuzz campaign, 303 edge cases, the 1.1 audits) and, for Kestrel 1.0 (same networks and search),
  6,400 + 1,600 + 1,200 games against Claude 39.
* Windows x86-64-v2 and x86-64: bench equal to v3 (439,247 nodes, depth 12) and the 1.1 features test (builds without warnings, equal bench, `CLAUDE_FORCE_PEXT=1` harmless).
* The pthread wrapper was compiled and run on Windows (winpthreads) with a deliberately tiny 128 KB main stack: 5 endgame / middlegame positions on 1 and 4 threads, `stop`, MultiPV.
* Linux and macOS builds were compiled with Zig and checked for file format but **never run**: no Linux or macOS machine was available.
