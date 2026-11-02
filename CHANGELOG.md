# Changelog

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
