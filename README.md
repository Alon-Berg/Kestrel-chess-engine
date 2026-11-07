# Kestrel

[![verify-release](https://github.com/Alon-Berg/Kestrel-chess-engine/actions/workflows/verify-release.yml/badge.svg)](https://github.com/Alon-Berg/Kestrel-chess-engine/actions/workflows/verify-release.yml)

A UCI chess engine in one C++17 file, written by [Claude](https://www.anthropic.com/claude), an AI model made by Anthropic, with one exception: the Syzygy tablebase reader inside it is [Fathom](https://github.com/jdart1/Fathom) (MIT licence, see [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)). Since version 2.0 it judges every position with one large neural network (39.1 million parameters) that is stored inside the executable.

**[Download the latest release](https://github.com/Alon-Berg/Kestrel-chess-engine/releases/latest)** (Windows, Linux, macOS) · [Web page](https://alon-berg.github.io/Kestrel-chess-engine/) · [Changelog](CHANGELOG.md) · [UCI options](docs/OPTIONS.md) · [Syzygy tablebases](docs/SYZYGY.md) · licence MIT

## Strength

All numbers are my own, from fastchess matches on one laptop (1 thread, 64 MB hash, no book, no learning, no adjudication, the same openings for both sides, every engine process at full speed with Windows' efficiency throttling switched off). No rating list has tested Kestrel yet. Elo is Kestrel 3.0 against the older version, with fastchess's 95 % interval. Kestrel 3.1 has the network and the search of 3.0 and plays exactly like it with one thread, so the numbers below hold for 3.1 with one thread.

| Time per game (base + increment, seconds) | Kestrel 3.0 against Kestrel 2.3 |
|---|---|
| 1 + 0.01 | +192 ± 23 |
| 2 + 0.02 | +150 ± 23 |
| 3 + 0.03 | +146 ± 17 |
| 5 + 0.1 | +105 ± 14 |
| 10 + 0.1 | +102 ± 22 |
| 20 + 0.5 | +102 ± 28 |

Pooled over all time controls: **+130 ± 8 Elo** against 2.3 (3,800 games, inverse-variance weights). The gain is largest at the shortest time controls and levels off with more time: from 5 s + 0.1 s per game on it is **+104 ± 11 Elo** (1,600 games). The gain comes from the network alone: the search and the time management are those of 2.3, and in a match of the two networks inside the same engine at 20,000 nodes per move the 3.0 network is +138 ± 11 Elo ahead (2,000 games). The network has the same size as before, so the cost of one node is almost the same (1.07 million nodes per second in the depth-12 bench on a performance core against 1.13 million for 2.3, 5 % fewer), but the stronger network makes the search tree smaller: the bench needs 859,322 nodes instead of 1,051,844 and takes about 808 ms instead of 935 ms (14 % less time).

In my own round robin against 100 other engines (5 s + 0.1 s per game, one thread, this laptop, every engine at full speed) 3.0 scored 76.0 of 200 games; fitted onto the CCRL Blitz scale through the engines whose ratings did not change that is about 3576 (95 % bootstrap interval 3552 to 3597), which would put it 71 of 101 in that table. This is an estimate under my own conditions, not a rating-list result.
Raw results: `logs/fast_matches_3_0.txt`.

**Many threads (3.1):** 8 threads score +123 ± 24 Elo against 1 thread of the same build at 10 s + 0.1 s per game (200 games with a development build; +108 ± 38 in 100 games with the release exe; on a laptop with 22 logical processors). 64 and 128 threads on the same 22 processors gain nothing (+11 ± 71 and +11 ± 57, 32 games each), so use at most as many threads as the computer has logical processors. Lazy SMP does not scale linearly (the nodes per second grow 2.9 times with 8 threads and 3.7 times with 20 on that laptop), and I could not test a computer with many more processors; details in [CHANGELOG.md](CHANGELOG.md).

For comparison, Kestrel 2.0 against the older versions (the same kind of match): **+37 ± 11 Elo** against 1.1 (1,900 games) and **+48 ± 13** against 1.0 (1,500 games), see `logs/fast_matches_2_0.txt`.

## What is inside

* **Evaluation:** one network. 24,576 inputs per side (12 piece kinds × 64 squares × 32 horizontally mirrored king buckets), 1,536 integer accumulators per side kept up to date move by move, a pairwise-product activation, then 16 → 32 → 1 neurons with 8 output buckets chosen by the number of pieces. The second layer skips the zero inputs (about 90 % are zero); an AVX2 code path does the work on CPUs that have it, with exactly the same results as the plain code.
* **Search:** iterative deepening with aspiration windows and principal-variation search; a bucketed transposition table; null-move pruning, reverse futility, ProbCut, internal iterative reduction, late-move reductions and pruning, futility and SEE pruning, singular extensions; killer, counter-move, history and continuation-history move ordering; a correction history for the static evaluation; quiescence search with SEE; Lazy SMP with up to 128 threads (3.1; 3.0: 8) and a hash of up to 1,048,576 MB (3.0: 4096); Stockfish's repetition rule and a damping of the evaluation near the fifty-move limit.
* **Syzygy tablebases** (new in 3.0): probing inside the search and at the root with the WDL and DTZ files; the 3-4-5-men files were tested, 6 and 7 men use the same code. See [docs/SYZYGY.md](docs/SYZYGY.md).
* **Features:** Chess960, `MultiPV` (up to 8), `UCI_ShowWDL`, `UCI_AnalyseMode`, `UCI_LimitStrength`, `go movetime` with `Move Overhead`, pondering, an optional opening book and an optional "experience" file (position learning), `EvalFile` to load another network of the same format.
* **Not available:** NUMA / large pages.

## Use it

Unzip a release file and add `kestrel.exe` (or `kestrel`) to a UCI program such as Cute Chess, Arena, En Croissant or Lucas Chess. The x86-64-v3 builds need a CPU with AVX2 and BMI2 (most PCs since 2015); the v2 and generic builds work on older CPUs and are about 37 % slower. The defaults are `Hash` 256 MB, 1 thread, opening book and learning off; all options are in [docs/OPTIONS.md](docs/OPTIONS.md). The exe is about 88 MB because the network is inside it, and it is not code-signed (some virus scanners distrust big unsigned programs); the SHA-256 of every release file is in `SHA256.txt` on the release page.

## Build

The 76 MB network is not stored in git. Download `kestrel_net_3_1.bin` from the [release page](https://github.com/Alon-Berg/Kestrel-chess-engine/releases/latest), save it as `kestrel_net.bin` in the folder where you run the compiler (the assembler looks for it in the **current directory**) and compile:

```
g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel path/to/src/kestrel_3_1.cpp
```

The source has 128 copies of the search, so the compile takes about two minutes and 1.1 GB of memory (3.0: 16 s). Other CPUs (x86-64-v2, plain x86-64), Linux, macOS and ARM: [docs/BUILDING.md](docs/BUILDING.md). `kestrel bench 12` must print `bench: nodes 859322`.

## How it was tested

* Two black-box audits of the UCI protocol and the rules (83 + 31 checks, `tests/audit_probe.py`, `tests/audit_sem.py`; python-chess is the rules oracle for the second one).
* A 30-check verification with GCC and with clang AddressSanitizer + UBSan builds (also with 4 threads): accumulator self-test over 186,000 positions, perft, perft against python-chess, evaluation mirror symmetry, PV legality, UCI fuzzers, `go infinite` / `stop` races, MultiPV and ponder checks, and for 3.0 the Syzygy test (`tests/tb_test.py`: win/draw/loss of random 3-5-men positions against python-chess reading the same files, the root move filter, search, and conversion of won endgames). Logs are in `logs/`.
* The Windows x86-64-v3 build ran all of this. The repository's GitHub Actions workflow ([`verify-release.yml`](.github/workflows/verify-release.yml)) runs the published files on GitHub's machines (Linux x86-64 v3, v2 and generic, Linux ARM64, macOS Apple silicon, Windows): its runs on the published files are on the Actions page.
* The macOS Intel build is run by the same workflow on GitHub's macOS Intel machine (`macos-15-intel`); I have no Mac myself.
* For the many threads of 3.1: thread start / stop stress tests with 16, 64 and 128 threads, `tests/breakers.py` with 1, 8 and 64 threads, an AddressSanitizer + UBSan build run with up to 128 threads, matches of 8 threads against 1 (200 and 100 games), and the scaling measurements of the changelog (`logs/threads_3_1.txt`). All of it ran on one Windows laptop with 22 logical processors.

## How the network was made

The network of 3.0 has the shape of the 2.x network (39.1 million parameters, 24,576 inputs per side, 1,536 neurons, 16 -> 32 -> 1, 8 output buckets) and was trained again from scratch with a longer run and about nine times more positions. All training ran on the CPU of one laptop (Core Ultra 7 155H, no GPU) with my own trainer (not in the repository): Adam, batches of 16,384 positions, a cosine learning-rate schedule. The loss is the squared difference between the network's win probability and the target, which is the win probability of the Stockfish evaluation, `1 / (1 + 10^(-cp / 400))` (mixed 80:20 with the game result for positions that have one). Stockfish only supplied labels; none of its code is in Kestrel.

Three sources of positions, all quiet (side to move not in check, best move not a capture):

* **238 million positions of the Lichess evaluation database** (CC0): the deepest evaluation of each position, depth 20 or more, a centipawn score (no mate), at most 1,500 centipawns. These are all the positions the database offers in that form, and they are everything the 2.x network learned from (plus 12 million positions of Kestrel's own games). Training on them for longer only overfitted: after about 50 hours the error on held-out positions stopped falling while the error on the training positions kept falling.
* **2.36 billion positions along the best lines** stored with every entry of that database (the lines that Stockfish found): the positions after 1 to 9 moves of the best line and after 1 to 6 moves of up to four other lines, each labelled with the evaluation of its line, with every duplicate and every position of the held-out set removed. They are new positions with high-quality labels, and they cut the held-out error of a small test network by about a quarter. They were mined in two batches that do not overlap: 1.08 billion for the main run and 1.28 billion for the second stage.
* **6.6 million positions from self-play games of Kestrel and its predecessor engines**, labelled by Stockfish at depth 10: positions of the kind the engine really meets in a game.

Training had two stages. The main run took 650,000 steps (54 hours) with a learning rate that fell from 1e-3 to 1e-5; every batch took 56 % of its positions from the first batch of line positions (each seen about 5.5 times), 3 % from Kestrel's own games and the rest from the database positions (each seen about 18 times). The second stage continued from the final network of the main run for 90,000 steps (8 hours) with a learning rate that fell from 6e-5 to 6e-7; 70 % of every batch came from the second set of line positions, which the main run had never seen and which were used at most once. The shipped network is the best snapshot of the second stage by held-out error. It beat the best snapshot of the main run by +7.7 ± 8.1 Elo in 4,000 equal-node games, and the main run's network beat the 2.3 network by +129.8 ± 10.9 Elo in 2,000 equal-node games (20,000 nodes per move).

The held-out error (mean squared error of the win probability, 400,000 positions of the database that were never trained on, and 150,000 of Kestrel's own games) fell from 0.0128 / 0.0164 for the 2.x network to 0.0107 / 0.0111 (17 % and 32 % lower). Two earlier ideas did not help and are not in 3.0: fine-tuning on fresh games of the engine itself labelled by Stockfish (-6.7 ± 11.2 Elo against the network it started from), and training longer on the database positions alone.

## Versions

3.1 (up to 128 threads and a hash of up to 1,048,576 MB; plays like 3.0 with one thread), 3.0 (a new network trained on about nine times more positions, Syzygy tablebases), 2.3 (bug fixes: an evaluation overflow in endgames; obsolete options with wrong defaults removed, which was 2.2), 2.1 (default settings for chess-engine testers), 2.0 (the large network), 1.1 and 1.0 (two small networks and a hand-written evaluation): see [CHANGELOG.md](CHANGELOG.md). What the README said about 1.x is kept in [docs/KESTREL_1_x.md](docs/KESTREL_1_x.md).

## Layout

* `src/kestrel_3_1.cpp` the whole engine (4.1 MB: the engine of 3.0 with 128 copies of the search, made by `scripts/make_threads.py` from `src/kestrel_3_0.cpp`, which is the same engine with 8 copies and the file to read); `src/kestrel_2_0.cpp` is 2.0 - 2.3; `src/kestrel_1_1.cpp` and its network headers: Kestrel 1.1; `src/claude_book.txt` the optional opening book (the file keeps its old name)
* `scripts/` the generator of the 3.1 source (`make_threads.py`) and the Windows version-info file (`kestrel.rc`)
* `docs/` build notes, option reference, Syzygy notes, an architecture guide, release notes of 1.x, the web page (`index.html`)
* `logs/` result and test logs of the releases
* `tests/` black-box test scripts for any Kestrel executable (see `tests/README.md`)

The tools that train the network and run the long test suites are not part of this repository.

## Licence

MIT, see `LICENSE`. The Syzygy probing code (Fathom) is MIT-licensed too; its notice is in `THIRD_PARTY_NOTICES.txt` and at the start of its block in `src/kestrel_3_1.cpp`.
