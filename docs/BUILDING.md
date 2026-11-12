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

## The play page (WebAssembly)

`docs/play/` is a web page on which Kestrel 3.2 can be played in a browser (https://alon-berg.github.io/Kestrel-chess-engine/play/). The engine in it is the 3.2 source compiled to WebAssembly (wasm32-wasi, reactor model) with Zig; nothing else is needed to build it.

```
python scripts/make_wasm.py                                  # writes src/kestrel_3_2_wasm.cpp (byte for byte the file in the repository; runs scripts/make_threads.py 8 first)
ZIG=/path/to/zig python scripts/build_wasm.py                # docs/play/kestrel.wasm (WebAssembly SIMD) and docs/play/kestrel-nosimd.wasm, Zig 0.15, about 15 seconds each
python scripts/pack_net.py kestrel_net_3_2.bin docs/play/kestrel_net_3_2.pk.gz      # the network for the page: 44.2 MB instead of 76.3 MB, about 3 minutes
python scripts/serve_play.py                                 # http://localhost:8000/ (the page does not work from a file: it needs a web server)
```

`make_wasm.py` takes the 3.2 source with 8 search copies and changes only what a browser needs: one search copy, no threads (a thread would run inside its constructor; none is started with one thread), no exceptions (`-fno-exceptions`), a stub for the `mmap` of the Syzygy reader (a browser has no table files), the network handed over as bytes instead of `.incbin`, and `main()` with its stdin loop replaced by exported functions (`kestrel_net_buffer`, `kestrel_init`, `kestrel_uci` for one UCI command per call, `kestrel_bench`); the body of the UCI loop is the text of 3.2. `go infinite` is refused (nothing can interrupt a search inside a browser worker; the page always gives a time or node limit).

The WebAssembly files give the same moves and node counts as the native programs: `deno run --allow-read tests/web/wasm_bench_test.js kestrel_net.bin` checks that the bench searches 859,322 nodes and that the start position searched to depth 12 takes 63,397 nodes, for both files; `python tests/web/gen_rules_cases.py` and `deno run --allow-read tests/web/rules_test.js` check the rules code of the page against python-chess and the perft numbers. Speed in Chrome's engine on one thread: about 190,000 nodes per second with SIMD and 110,000 without (the native AVX2 build: 1.1 million).

`kestrel_net_3_2.pk.gz` is the network with the int16 weights of the first layer split into a plane of low bytes and a plane of high bytes (the high bytes are almost all 0 or -1), then gzip level 9: 58 % of the size instead of 69 % for plain gzip. The page (`engine-worker.js`) unpacks it while it copies the data into the engine's memory and checks the SHA-256 of the result. The 3.2 network is the one of 3.0 and 3.1; a later network needs its own packed file (`netUrl`, `netSize`, `netSha256`, `shuffleCount` and `cacheName` in `docs/play/play.js`).

What the page does not have: more than one thread, tablebases, an opening book (the page varies the first moves itself with `MultiPV`), pondering, analysis of the played game.

### The levels of the page

**Since 2026-11-12 the page offers only full strength** (its one setting is the thinking time). The levels described here were built for it and are still in `docs/play/levels.js` with their tests and measuring programs (`levels.js` also chooses the moves of full strength: the engine's own choice after the thinking time, with a little variety in the first moves); `tests/web/levels_lab/uci_levels.js` is a UCI engine with these levels, so that any chess program can play them.

**Why the page had its own levels.** The first version took them from the engine's `UCI_Elo` option and they were all one strength: Kestrel looks at its node count only every 2,048 nodes, so every `UCI_Elo` from 1320 up to about 2300 searches 1,000 to 2,000 nodes (depth 4 to 7; in four test positions the program of the release searched 1,564, 1,054, 1,697 and 1,663 nodes at 1320, 1500, 1900 and 2300: the same). The level called "about 1300" played far above its name. The engine was not changed (3.2 is as released; `docs/OPTIONS.md` says what `UCI_Elo` does).

**How a level works** (`docs/play/levels.js`, plain data and functions, also used by the test programs). The engine always plays at full strength; a level only limits and disturbs what it is asked:

| | |
|---|---|
| `depth` or `nodes` | how far Kestrel looks: plies (the lower levels), or positions (the two strongest); `UCI_LimitStrength` is off |
| `k` | how many of its best moves it keeps in mind (`MultiPV`, at most 8) |
| `temp` | how carelessly it picks among them: a move that looks x centipawns worse than the best one is played with the weight exp(-x / temp) |
| `slip` | the chance per move of an outright oversight: a random legal move other than the best one |

Four rules hold for every level. The first ten plies are played with care: `temp` is cut to 0.4 times (at least 25 centipawns, which also gives the strong levels a little variety: only moves within 25 centipawns of the best one are played) and there is no oversight before ply 8. A level that is clearly ahead gets steadier: `temp` and `slip` shrink when the best move is worth more than 1.5 pawns, down to 0.35 times at 8 pawns (a person who is a queen up does not start to play at random; without the rule the weak levels lost 21 to 30 % of the games in which they had led by 6 pawns or more). Scores are cut at 30 pawns and a mate counts as 30 pawns, a quicker one a little more. A level other than full strength answers after at least 0.45 to 0.9 seconds, so that the reply does not come before the player has seen their own move.

**How the table was made** (programs and commands: `tests/web/levels_lab/`, which runs the same `levels.js` and the same WebAssembly engine as the page):

1. A grid of 30 settings (depth 1 to 5 times `temp` 400 to 30, `slip` = `temp` / 2500) played its neighbours (89 pairs of 30 games), and Bradley-Terry ratings were fitted: the grid spans 1,600 Elo; one factor of 1.6 in `temp` is worth about 150 Elo, one more ply 100 to 400 depending on the carelessness.
2. The 12 levels were chosen from the grid for steps of about 200 Elo, then changed a little (`temp` of levels 2, 4, 5 and 7) until the steps between neighbours were even; the table below was checked with 150 games per pair.
3. Every level played itself 60 times, and Stockfish (depth 12) judged each position of those games once. The loss of a move is the score of the best move minus the score of the move played (both for the side to move, cut at 10 pawns as lichess does); ACPL is its average.
4. The levels were played against Stockfish at `UCI_Elo` settings (fastchess, 10 s + 0.1 s or 5 s + 0.05 s per game; Stockfish's scale is anchored to the CCRL list).

**What the numbers in the menu mean.** They are an estimate, **not** measured against people. The levels were set so that their mistakes per move are what I expect from players of that rating: at "about 1300" a quarter of the moves lose a tenth of a pawn or more and one in twenty loses a piece or more, at "about 2100" one in fifty-five loses a piece. Real players differ (they make fewer small mistakes and more of the large ones in sharp positions, and they get tired), so a level that feels too strong or too weak can be moved by changing its `temp` and `slip`.

| Level | search | `k` | `temp` | `slip` | ACPL | loses ≥ 1 pawn | loses ≥ 3 pawns | wins after leading by 6 pawns | step up from the level below | Stockfish `UCI_Elo` equivalent |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 (about 500) | depth 1 | 8 | 800 | 0.30 | 132 | 33.0 % | 13.2 % | 50 % | | below 1320 |
| 2 (about 700) | depth 1 | 8 | 430 | 0.17 | 95 | 26.8 % | 8.6 % | 65 % | +188 | below 1320 |
| 3 (about 900) | depth 1 | 8 | 260 | 0.10 | 90 | 27.2 % | 7.3 % | 65 % | +158 | below 1320 |
| 4 (about 1100) | depth 1 | 8 | 130 | 0.052 | 77 | 25.0 % | 5.2 % | 72 % | +220 | about 1100 (below 1320) |
| 5 (about 1300) | depth 2 | 6 | 175 | 0.07 | 69 | 21.6 % | 4.9 % | 80 % | +147 | about 1150 (below 1320) |
| 6 (about 1500) | depth 3 | 5 | 160 | 0.064 | 67 | 21.4 % | 4.7 % | 72 % | +182 | about 1350 |
| 7 (about 1700) | depth 3 | 5 | 112 | 0.045 | 63 | 20.3 % | 3.8 % | 83 % | +176 | about 1400 |
| 8 (about 1900) | depth 4 | 4 | 100 | 0.04 | 56 | 17.7 % | 2.9 % | 85 % | +203 | about 1600 |
| 9 (about 2100) | depth 4 | 4 | 55 | 0.022 | 45 | 14.9 % | 1.8 % | 93 % | +246 | about 2000 |
| 10 (about 2300) | depth 5 | 4 | 40 | 0.016 | 40 | 11.4 % | 1.6 % | 91 % | +191 | about 2400 |
| 11 (about 2500) | 4,096 nodes | 2 | 15 | 0 | 30 | 7.6 % | 1.4 % | 88 % | +325 | about 2800 |
| 12 (about 2800) | 16,384 nodes | 1 | 0 | 0 | 16 | 3.0 % | 0.5 % | 84 % | +325 | about 2800 |

The mistake columns are for a level against itself (60 games each, 60 to 160 moves per game; the loss is judged by Stockfish at depth 12, so even a perfect player has an ACPL of about 10 to 15). "Wins after leading" is the share of games a level won after it had led by 6 pawns or more at some point (the two strongest levels led by that much in only 19 and 50 of their 60 games). "Step up" is the Elo difference of the level to the one below it in games between the two (150 games per pair or more for levels 2 to 9, 60 for the three highest; the 95 % ranges are about ±55 and ±100). The strong levels draw a lot against each other (a deterministic engine repeats positions), which makes the steps at the top of the list less certain.

"Stockfish `UCI_Elo` equivalent" is the rating on Stockfish's scale (Stockfish 19, one thread, `UCI_LimitStrength` on) that a level scored 50 % against: 24 to 50 games against each of two or three settings, so each number is good to about ±100 (and levels 11 and 12 came out equal, although Level 12 beats Level 11 in self-play). Stockfish's lowest setting, 1320, is stronger than "about 1300" and "about 1100" here: it plays about like Level 6, which is why the table has five levels below it. The numbers for Levels 4 and 5 are extrapolated from their score against that lowest setting. Numbers on Stockfish's scale are not human ratings either (the scale is anchored to the CCRL list of engines).
