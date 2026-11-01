# Kestrel

A UCI chess engine in one C++17 file, written by [Claude](https://www.anthropic.com/claude), an AI model made by Anthropic. Since version 2.0 it judges every position with one large neural network (39 million parameters) that is stored inside the executable.

**[Download the latest release](https://github.com/Alon-Berg/Kestrel-chess-engine/releases/latest)** (Windows, Linux, macOS) · [Web page](https://alon-berg.github.io/Kestrel-chess-engine/) · [Changelog](CHANGELOG.md) · [UCI options](docs/OPTIONS.md) · licence MIT

## Strength

All numbers are my own, from fastchess matches on one laptop (1 thread, 32 MB hash, no book, no learning, the same openings for both sides). No rating list has tested Kestrel yet. Elo is Kestrel 2.0 against the older version, with fastchess's 95 % interval; 2.1 plays the same moves.

| Time per game (base + increment, seconds) | against Kestrel 1.1 | against Kestrel 1.0 |
|---|---|---|
| 1 + 0.01 | +15 ± 32 | +28 ± 33 |
| 2 + 0.02 | +13 ± 28 | +51 ± 31 |
| 3 + 0.03 | +64 ± 29 | +31 ± 28 |
| 5 + 0.1 | +38 ± 20 | +64 ± 21 |
| 10 + 0.1 | +51 ± 30 | |
| 20 + 0.5 | +41 ± 36 | |

Pooled over all time controls: **+37 ± 11 Elo** against 1.1 (1,900 games) and **+48 ± 13** against 1.0 (1,500 games). At about 20 ms per move the large network is roughly level with 1.1: the network costs speed (about 830,000 nodes per second in the depth-12 bench on a performance core, roughly 30 % fewer than 1.1).
Raw results: `logs/fast_matches_2_0.txt`, `logs/fast_matches_2_1.txt`.

## What is inside

* **Evaluation:** one network. 24,576 inputs per side (12 piece kinds × 64 squares × 32 horizontally mirrored king buckets), 1,536 integer accumulators per side kept up to date move by move, a pairwise-product activation, then 16 → 32 → 1 neurons with 8 output buckets chosen by the number of pieces. The second layer skips the zero inputs (about 90 % are zero); an AVX2 code path does the work on CPUs that have it, with exactly the same results as the plain code.
* **Search:** iterative deepening with aspiration windows and principal-variation search; a bucketed transposition table; null-move pruning, reverse futility, ProbCut, internal iterative reduction, late-move reductions and pruning, futility and SEE pruning, singular extensions; killer, counter-move, history and continuation-history move ordering; a correction history for the static evaluation; quiescence search with SEE; Lazy SMP with up to 8 threads; Stockfish's repetition rule and a damping of the evaluation near the fifty-move limit.
* **Features:** Chess960, `MultiPV` (up to 8), `UCI_ShowWDL`, `UCI_AnalyseMode`, `UCI_LimitStrength`, `go movetime` with `Move Overhead`, pondering, an optional opening book and an optional "experience" file (position learning), `EvalFile` to load another network of the same format.
* **Not available:** Syzygy tablebases, NUMA / large pages.

## Use it

Unzip a release file and add `kestrel.exe` (or `kestrel`) to a UCI program such as Cute Chess, Arena, En Croissant or Lucas Chess. The x86-64-v3 builds need a CPU with AVX2 and BMI2 (most PCs since 2015); the v2 and generic builds work on older CPUs and are about 37 % slower. The defaults are `Hash` 256 MB, 1 thread, opening book and learning off; all options are in [docs/OPTIONS.md](docs/OPTIONS.md). The exe is about 88 MB because the network is inside it, and it is not code-signed (some virus scanners distrust big unsigned programs); the SHA-256 of every release file is in `SHA256.txt` on the release page.

## Build

The 76 MB network is not stored in git. Download `kestrel_net_2_0.bin` from the [release page](https://github.com/Alon-Berg/Kestrel-chess-engine/releases/latest), save it as `kestrel_net.bin` in the folder where you run the compiler (the assembler looks for it in the **current directory**) and compile:

```
g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel path/to/src/kestrel_2_0.cpp
```

Other CPUs (x86-64-v2, plain x86-64), Linux, macOS and ARM: [docs/BUILDING.md](docs/BUILDING.md). `kestrel bench 12` must print `bench: nodes 1051844`.

## How it was tested

* Two black-box audits of the UCI protocol and the rules (83 + 31 checks, `tests/audit_probe.py`, `tests/audit_sem.py`; python-chess is the rules oracle for the second one).
* A 27-check verification with GCC and with clang AddressSanitizer + UBSan builds (also with 4 threads): accumulator self-test over 186,000 positions, perft, perft against python-chess, evaluation mirror symmetry, PV legality, UCI fuzzers, `go infinite` / `stop` races, MultiPV and ponder checks; plus a fuzz campaign of about 8,900 random command sequences. Logs are in `logs/`.
* The Windows x86-64-v3 build ran all of this. The Linux and macOS builds are compiled and file-format-checked with Zig; the repository's GitHub Actions workflow downloads the shipped binaries and runs them on Linux, macOS and Windows machines (see `.github/workflows/`).

## How the network was made

About 238 million positions of the Lichess evaluation database (Stockfish evaluations, CC0) plus 12 million positions from Kestrel's own games labelled by Stockfish (depth 10), trained on a CPU with Adam. Stockfish was only the teacher; no Stockfish code or network is included. The training run overfitted after about 60 hours (the Lichess database had no more positions to offer), so the shipped network is the best snapshot (after about 49 hours of training); a fine-tune on fresh games lost 6.7 ± 11.2 Elo against it and was not used.

## Versions

2.1 (default settings for chess-engine testers), 2.0 (the large network), 1.1 and 1.0 (two small networks and a hand-written evaluation): see [CHANGELOG.md](CHANGELOG.md). What the README said about 1.x is kept in [docs/KESTREL_1_x.md](docs/KESTREL_1_x.md).

## Layout

* `src/kestrel_2_0.cpp` the whole engine (the file name is from 2.0); `src/kestrel_1_1.cpp` and its network headers: Kestrel 1.1; `src/claude_book.txt` the optional opening book (the file keeps its old name)
* `docs/` build notes, option reference, release notes of 1.x, the web page (`index.html`)
* `logs/` result and test logs of the releases
* `tests/` the two black-box audit scripts

The tools that generate the source, train the network and run the long test suites are not part of this repository.

## Licence

MIT, see `LICENSE`.
