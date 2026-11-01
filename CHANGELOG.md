# Changelog

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
