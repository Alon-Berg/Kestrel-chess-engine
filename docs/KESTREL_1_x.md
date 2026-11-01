# Kestrel 1.x (1.0 and 1.1)

This page keeps what the README said about the first two releases, which used two small embedded networks blended with a hand-written evaluation. Kestrel 2.x replaced all of that with one large network; see the [README](../README.md).

Kestrel 1.x is a UCI chess engine written in a single C++17 file (bitboards, alpha-beta search) with two embedded neural networks (king-bucketed inputs) blended with a hand-written evaluation.
**Kestrel 1.1** (development version 40.1) is a bug-fix release on **Kestrel 1.0** (40.0), the first release under the name Kestrel; the engine was called *Claude* up to version 39
(the comment at the top of the source and the book file name still say Claude: the code is identical to Claude 39 apart from the engine name and the experience-file name).

* bucketed transposition table, multi-threaded search (up to 8 threads)
* Chess960 (`UCI_Chess960`), Stockfish's repetition rule, `UCI_ShowWDL`, `UCI_AnalyseMode`, `MultiPV`, `go movetime` with `Move Overhead`
* optional opening book and "experience" file (both off in analysis-style searches)
* **not** available: Syzygy tablebases, NUMA / large pages; `register` and `debug` are ignored

## Build (1.1)

```
cd src
g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel_final.exe kestrel_1_1.cpp
```

`net_weights.h` and `net_weights2.h` are read by the compiler (they are `#include`d), so keep them next to the source. Put `claude_book.txt` next to the exe to use the book
(the file keeps its old name; it is not read when `OwnBook` is false). `-march=x86-64-v3` needs a CPU with AVX2 (Intel Haswell / AMD Zen or newer).
Other CPUs, Linux, macOS and ARM: see [BUILDING.md](BUILDING.md). The 1.1 binaries are on the [1.0 / 1.1 release page](https://github.com/Alon-Berg/Kestrel-chess-engine/releases/tag/v1.0).

## Kestrel 1.1: what changed

Bug fixes only. The networks, the book and the search are unchanged (bench depth 12: 439,247 nodes; 797 of 800 fixed-node test searches give exactly the result of 1.0, the other three are K+N+N against K endings).

* K+N+N against a bare king was evaluated as winning (about +3.4 pawns) although there is no forced mate; now 0 (the search still finds a mate the opponent allows).
* Linux (musl) and macOS: search threads get 8 MB stacks (the default 128 KB / 512 KB is too small for a deep search).
* Linux and macOS: the book and the experience file are found next to the real executable also when it is started by name through PATH or through a symlink.
* The source builds for every CPU target without a patch (generic x86-64, v2, v3, ARM64); see [BUILDING.md](BUILDING.md).

Found by 83 black-box UCI cases and 31 rules / protocol checks (`tests/audit_probe.py`, `tests/audit_sem.py`, logs in `logs/`) and by porting the source to Linux, macOS and ARM.

## Measured strength of Kestrel 1.0 (relative to Claude 39; 1.1 has the same networks and search)

```
MEASURED - Kestrel 1.0 against Claude 39 (fastchess, 1 thread, Experience and OwnBook off; raw lines tests/v40_vs_39_raw.txt, tests/v40e_status.txt)
  equal nodes       20,000 nodes per move, first 3,200 games (the games the release decision was made on): +11.5 +/- 7.9 Elo (W 1072  L 966  D 1162);
                    second, independent 3,200 games (played afterwards): +8.8 +/- 8.0 Elo (W 1058  L 977  D 1165);
                    pooled 6,400 games: +10.2 +/- 6.8 (95 % interval, plain win / draw / loss) - the interval is above 0.
                    60,000 nodes per move, 1,600 games: +5.0 +/- 9.9 Elo (W 471  L 448  D 681).
  timed games       3+0.03, 64 MB, 800 games: -12.2 +/- 16.2 (W 243  L 271  D 286); 5+0.1 (the tournament time control), 400 games: +34.0 +/- 21.4 (W 139  L 100  D 161).  Run AFTER the equal-node tests as a sanity check; NOT part of the release rule.
  speed             pinned depth-14 bench (median of 7, tests/v40_bench_pin.txt): Kestrel 1.0 searches 1234988 nodes at 1500592 nodes/s, Claude 39 1110401 nodes at 1480534 nodes/s (same network size and evaluation code; the new nets give a slightly different tree).
  tests             27-check verification passed (tests/v40_verify_log.txt); the long fuzz campaign and the extra tests of 38.1 / 39 were run again on the Kestrel build.
```

Kestrel 1.0 has not been rated by CCRL or any other list. The numbers above are equal-node and short timed matches played with fastchess on one laptop (1 thread, no book).
Kestrel 1.0 is the third candidate of one line of work (the first two were rejected), so the first +11.5 is the best of three and probably somewhat flattering; see the second 3,200 games.

## How the 1.x networks were made

Self-play of the previous engine, labels from Stockfish (depth 7 / 8) mixed with the game result (lambda 0.8), warm-started networks trained with Adam; see `README_full.txt` (1.1) and `README_full_1_0.txt` (1.0).
Stockfish is used only as a labelling teacher; no Stockfish code or network is included.
