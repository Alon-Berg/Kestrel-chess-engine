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
* Linux and macOS builds were compiled with Zig and checked for file format. They were first run on 2026-11-01 by the GitHub Actions workflow `.github/workflows/verify-release.yml`, which downloads the published files and runs them on GitHub's machines: Linux x86-64 (v3, v2 and generic), Linux ARM64 and macOS (Apple silicon) all gave the recorded bench and answered a UCI session (the Linux x86-64-v3 build also passed both audits and the test scripts in `tests/`). The macOS Intel build has not been run.

## Kestrel 2.0

`src/kestrel_2_0.cpp` is the whole engine. The network (76 MB) is embedded with `.incbin "kestrel_net.bin"`, and the assembler looks that file up in the **current directory** (not next to the source).
The network is not stored in the git history (76 MB per version): download `kestrel_net_2_0.bin` from the release page and save it as `kestrel_net.bin` in the folder where you run the compiler.

| Target | Compile with |
|---|---|
| Windows / Linux x86-64-v3 (AVX2 + BMI2) | `g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel path/to/src/kestrel_2_0.cpp` |
| x86-64-v2, plain x86-64 | same with `-march=x86-64-v2` or `-march=x86-64` (plain evaluation loops, about 37 % slower) |
| ARM64, macOS, Linux cross builds | as for 1.1 above with `kestrel_2_0.cpp` (compiled with Zig, never run) |

The AVX2 evaluation code is compiled only when the compiler targets AVX2 (the last layers also need FMA); every other target uses plain loops with exactly the same results
(bench depth 12 = 1,051,844 nodes on all three Windows builds of 2.1; 2.0 gave 934,326: the difference comes only from the changed default settings, Hash 256 MB and no opening book). The tools used to find and check the speed-ups are not part of the repository.

2.1 is 2.0 with other default settings (Hash 256, `OwnBook` and `Experience` off) and was checked again (files with `_2_1_` in the name in `logs/`). 2.2 plays the same moves as 2.1 (only the option list changed) and was checked again too (`_2_2_`). 2.3 fixes an int32 overflow in the evaluation and was checked again (`_2_3_`); its sanitizer verification ran on Linux in the GitHub Actions workflow as well. Tested for 2.0 (Windows x86-64-v3, logs in `logs/`): the 27-check verification with AddressSanitizer / UBSan, also with 4 threads (`v2_0_verify_log.txt`), a fuzz campaign (`v2_0_fuzz_campaign_log.txt`), the two 1.1 audits (`audit_probe_2_0_output.txt`, 83 of 83; `audit_sem_2_0_output.txt`, 31 of 31) and the matches against 1.1 and 1.0 (`fast_matches_2_0.txt`). The Linux and macOS builds were compiled with Zig; what the GitHub Actions workflow ran of them is described above.

## Kestrel 3.0

`src/kestrel_3_0.cpp` is the whole engine, including the Syzygy tablebase reader (Fathom, MIT licence; its notice is at the start of its block and in `THIRD_PARTY_NOTICES.txt`; it makes the file about 4,700 lines longer and the compile a few seconds slower). The network (76 MB) is embedded with `.incbin "kestrel_net.bin"`, and the assembler looks that file up in the **current directory** (not next to the source).
The network is not stored in the git history: download `kestrel_net_3_0.bin` from the release page and save it as `kestrel_net.bin` in the folder where you run the compiler.

| Target | Compile with |
|---|---|
| Windows / Linux x86-64-v3 (AVX2 + BMI2) | `g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel path/to/src/kestrel_3_0.cpp` |
| x86-64-v2, plain x86-64 | same with `-march=x86-64-v2` or `-march=x86-64` (plain evaluation loops, about 37 % slower) |
| ARM64, macOS, Linux cross builds | as for 1.1 above with `kestrel_3_0.cpp` (compiled with Zig; the macOS Apple silicon and Linux ARM64 builds ran in the GitHub Actions workflow) |
| another network width | add `-DKESTREL_H1=<width>` (a multiple of 64; the default is 1536, the width of the released network; the network file must have the same width) |

Nothing else changes for 3.0: the AVX2 evaluation code is compiled only when the compiler targets AVX2 and FMA, every other target uses plain loops with exactly the same results (depth-12 bench = 859,322 nodes on all three Windows builds). Syzygy tablebases need no compile option: Fathom is part of the file and is switched on at run time by the option `SyzygyPath` (see `docs/SYZYGY.md`). The Windows release exes are stripped and carry the version-info block as described above.

Tested for 3.0 (Windows x86-64-v3, logs in `logs/`): the 30-check verification with AddressSanitizer / UBSan, also with 4 threads, and the Syzygy test (`v3_0_verify_log.txt`, 30 of 30), the two audits (`audit_probe_3_0_output.txt`, 83 of 83; `audit_sem_3_0_output.txt`, 31 of 31), the options test (20 options, 0 differences) and, with the 3-4-5-men files, `tests/tb_test.py` (1,500 positions, 0 mismatches against python-chess; conversion 30 of 30). The Linux and macOS builds were compiled with Zig (clang) and checked for file format; the GitHub Actions workflow runs the published files and the source on GitHub's machines.

## Kestrel 3.2

`src/kestrel_3_2.cpp` is the whole engine with 128 copies of the search (4.6 MB, about 83,700 lines). It is generated from `src/kestrel_3_2_base.cpp` (the same engine with 8 copies, 11,300 lines: the file to read) by `scripts/make_threads.py`: `python scripts/make_threads.py 128 src/kestrel_3_2.cpp --id-name "Kestrel 3.2"` writes the file byte for byte (the script checks that the copies are identical and prints the few places where its output differs from its input). The network is the one of 3.0 and 3.1: download `kestrel_net_3_2.bin` (the same file as `kestrel_net_3_0.bin`) from the release page and save it as `kestrel_net.bin` in the folder where you run the compiler.

| Target | Compile with |
|---|---|
| Windows / Linux x86-64-v3 (AVX2 + BMI2) | `g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel path/to/src/kestrel_3_2.cpp` |
| x86-64-v2, plain x86-64 | same with `-march=x86-64-v2` or `-march=x86-64` (plain evaluation loops, about 37 % slower) |
| ARM64, macOS, Linux cross builds | as for 1.1 above with `kestrel_3_2.cpp` (compiled with Zig; the GitHub Actions workflow runs them) |

Compiling the file takes about 2.5 minutes and 1.1 GB with g++ -O3 on a fast PC (the Zig/clang build of the same source was 7 % slower at run time than the g++ one). The Windows x86-64-v3 exe is about 82 MB. The depth-12 bench is 859,322 nodes on all three Windows builds, as in 3.0 and 3.1. A development build (`-DKESTREL_TUNE_OPTIONS`) of `src/kestrel_3_2_base.cpp` has the search constants and the switched-off ideas as options (`K22Flags`, `K31Flags`, `K32Flags`, `T_...`).

Tested for 3.2 (Windows x86-64-v3, logs in `logs/`): `threads_3_2.txt`, `repo_tests_3_2.txt` (the test scripts of `tests/`), `v3_2_verify_log.txt` (the sanitizer verification), `audit_probe_3_2_output.txt` (83 of 83) and `audit_sem_3_2_output.txt` (31 of 31); the thread measurements of 3.1 (`threads_3_1.txt`) apply, the thread code is unchanged.

## Kestrel 3.1

`src/kestrel_3_1.cpp` is the whole engine with 128 copies of the search (4.1 MB, about 78,900 lines). It is generated from `src/kestrel_3_0.cpp` by `scripts/make_threads.py`: `python scripts/make_threads.py 128 src/kestrel_3_1.cpp --id-name "Kestrel 3.1"` writes the file byte for byte (the script checks that the copies are identical and prints the few places where its output differs from its input; the number of copies must be at least 8, and only 64 and 128 were built and tested). The network is the one of 3.0: download `kestrel_net_3_1.bin` (the same file as `kestrel_net_3_0.bin`) from the release page and save it as `kestrel_net.bin` in the folder where you run the compiler.

| Target | Compile with |
|---|---|
| Windows / Linux x86-64-v3 (AVX2 + BMI2) | `g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel path/to/src/kestrel_3_1.cpp` |
| x86-64-v2, plain x86-64 | same with `-march=x86-64-v2` or `-march=x86-64` (plain evaluation loops, about 37 % slower) |
| ARM64, macOS, Linux cross builds | as for 1.1 above with `kestrel_3_1.cpp` (compiled with Zig; not run before the release) |

Compiling this file costs more than 3.0: g++ -O3 needs about 136 s and 1.1 GB of memory on a fast PC (3.0: 16 s, 0.5 GB; a 64-copy file of 2.2 MB: 69 s and 0.8 GB), Zig (clang) 55 - 71 s and 2.3 GB; no compiler limit was reached. The Windows x86-64-v3 exe is about 82 MB (3.0: 88.4 MB). Every search copy reserves 6.3 MB of address space, so a process commits about 0.84 GB when it starts (3.0: 79 MB) although a one-thread game touches only the first copy. The depth-12 bench is 859,322 nodes on all three Windows builds, as in 3.0.

Tested for 3.1 (Windows x86-64-v3, logs in `logs/`): `threads_3_1.txt` (thread tests, scaling measurements and matches), `repo_tests_3_1.txt` (the test scripts of `tests/`, among them `breakers.py` with 1, 8 and 64 threads), `v3_1_verify_log.txt` (the sanitizer verification), `audit_probe_3_1_output.txt` (83 of 83) and `audit_sem_3_1_output.txt` (31 of 31). The Linux and macOS builds were compiled with Zig (clang) and checked for file format; the GitHub Actions workflow runs the published files and the source on GitHub's machines.
