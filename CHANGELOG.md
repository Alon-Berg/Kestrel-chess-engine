# Changelog

## 3.0 (2026-11-05)

A much stronger network, Syzygy tablebases, and two small fixes. Against 2.3 it scores +130 ± 8 Elo over six time controls (3,800 games, one thread), and +104 ± 11 at 5 s + 0.1 s per game or longer (1,600 games).
**Network:** same architecture as 2.x (39.1 million parameters, 24,576 inputs per side, 1,536 accumulator neurons, 16 -> 32 -> 1 with 8 output buckets), so the speed per node is almost the same (5 % fewer nodes per second, but a smaller search tree), but it was trained again from scratch for 62 hours in two stages on about nine times as many positions. The 2.x network had overfitted its 238 million positions of the Lichess evaluation database; 3.0 also learns from the positions along the best lines stored in that database (2.36 billion positions, mined in two batches) and from 6.6 million positions of its own games. On held-out positions its error is 17 % lower than the 2.3 network's on Lichess positions and 32 % lower on positions from Kestrel's own games. In a match of the two networks inside the same engine (20,000 nodes per move, 2,000 games) the new network scores +138 ± 11 Elo; the engine as a whole scores +130 ± 8 Elo against 2.3 in timed games (3,800 games, six time controls, one thread).
**Syzygy tablebases (new):** the options `SyzygyPath`, `SyzygyProbeDepth`, `Syzygy50MoveRule` and `SyzygyProbeLimit`; WDL probes inside the search, DTZ probes at the root (in a won position only the fastest wins are searched, so the fifty-move budget is not used up); `info` lines carry `tbhits`. The probing code is Fathom (Ronald de Man, basil00, Jon Dart; MIT licence, notice in `THIRD_PARTY_NOTICES.txt`), folded into the single source file. Without tablebase files nothing changes. Tested with the 3-4-5-men files (`tests/tb_test.py`, `docs/SYZYGY.md`); a 600-game match with the tables against the same build without them scored +19.7 +/- 17.5 Elo. 6- and 7-men files use the same code but were not tested.
**Windows:** the engine now asks Windows not to treat it as a background process ("efficiency mode", EcoQoS). A GUI that starts the engine hidden could make Windows slow it down; opting out gave +13 to +17 % nodes per second at 2 to 8 threads in a test.
**Fix:** `info nodes` and `info nps` added the stale node counters of idle helper threads when `MultiPV` was above 1 (that search runs on one thread) or when fewer threads than requested could be started.
**Tests:** the semantic audit `tests/audit_sem.py` accepts a quiet winning move first in its check "promotion to queen when winning" if the principal variation promotes within its next two moves (the 3.0 network plays Ka2 before a8=Q in 8/P6k/8/8/8/8/8/K7; both win, and in 600 such won positions no delayed promotion threw the win away); `tests/race_test.py` now waits for `readyok` before it starts the `go infinite` / `stop` race (a slow start-up of the 76 MB exe was reported as a hang), and `tests/tb_test.py` is new. The search, the time management and every other option are those of 2.3 (depth-12 bench 859,322 nodes with the new network).

## 2.3 (2026-11-02)

Bug-fix release for two defects present since 2.0; strength and speed are unchanged (depth-12 bench still 1,051,844 nodes, same time; a 400-game match against 2.2 scored +7 +/- 21 Elo).
**Evaluation overflow:** the second layer of the network summed 1,536 products in a 32-bit integer; for a few strongly negative neurons of the output bucket for 5 pieces or fewer the exact sum falls below -2^31, wrapped around, and the neuron was read as fully on instead of off (9 of 797,651 evaluations in the depth-12 bench; found by UBSan on Linux). The sums are now accumulated in float32; the AVX2 and the plain code still agree exactly.
It also contains the option fix described under 2.2 (that version was tagged but not released).

## 2.2 (2026-11-02, tag only)

Bug-fix release: the same engine, network and moves as 2.1 (identical search on 18 positions; the depth-12 bench is still 1,051,844 nodes; a 400-game match against 2.1 scored +9 +/- 18 Elo).
Kestrel 2.0 and 2.1 declared options of the 1.x engines with wrong defaults: `NNBlend` was declared with default 70 but the engine runs at 100, so a program that sent the declared defaults (for example after "reset to defaults") mixed the old hand-written evaluation into the search (-6 +/- 17 Elo in a 600-game match, but a different search). The obsolete and development options are no longer declared and release builds ignore `NNBlend`; `tests/options_test.py` guards against this kind of bug.
The source also contains new search ideas and the search constants as switched-off options (`K22Flags`, `T_...`, declared only in development builds); none of the ideas helped in 1,200 to 4,000-game tests, see `logs/fast_matches_2_2.txt`.
Added since 2.1 (not part of the binaries): the README for 2.x, `docs/OPTIONS.md`, `docs/ARCHITECTURE.md`, portable test scripts in `tests/` and a GitHub Actions workflow that runs the published binaries on Linux, macOS and Windows.

## 2.1 (2026-11-01)

Same engine, network and search as 2.0 (checked: identical best move, depth, score, node count and pv on 18 positions; a 400-game match of 2.1 on its defaults against 2.0 with the same settings set by hand scored -3 +/- 19 Elo).
Only the default settings changed, for chess-engine testers: `Hash` 256 MB (was 64), `OwnBook` off (was on), `Experience` off (was on). Depth-12 bench: 1,051,844 nodes (2.0: 934,326, the difference comes from the new defaults).

## 2.0 (2026-11-01)

The first Kestrel with one large neural network (39 million parameters, 76 MB, stored inside the exe) instead of two small networks and a hand-written evaluation.
About +37 +/- 11 Elo ahead of 1.1 and +48 +/- 13 ahead of 1.0 over six time controls (1,900 and 1,500 games; `logs/fast_matches_2_0.txt`).
The network is the one of 2.0 beta 1: the long training run overfitted after about 60 hours and a fine-tune on fresh games did not help (-6.7 +/- 11.2 Elo), so the best snapshot was kept.
New UCI options `EvalFile` and `EvalScale`. Verified with the two black-box audits (83 + 31 checks), a 27-check verification with AddressSanitizer / UBSan and a fuzz campaign.

## 2.0 beta 1 (2026-10-30)

Pre-release of the large-network engine with a network snapshot from the middle of the planned training run.

## 1.1 (2026-10-27)

Bug-fix release: K+N+N against a bare king evaluated as a win; 8 MB search-thread stacks on Linux and macOS; book and experience file found next to the real executable; source builds for every CPU target. Details in [docs/KESTREL_1_x.md](docs/KESTREL_1_x.md).

## 1.0 (2026-10-27)

First release under the name Kestrel (before: Claude 39): single-file C++17 UCI engine with two embedded networks and a hand-written evaluation, MIT licence.
