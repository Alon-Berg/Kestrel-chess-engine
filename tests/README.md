# Tests

Black-box tests: every script takes the path of a Kestrel executable and talks to it over UCI. They need Python 3 and `pip install chess` (python-chess is the rules oracle in some of them). The scripts print their result on the last line(s); they exit with 0 even when something failed, so look at the output (the GitHub Actions workflow in `.github/workflows/verify-release.yml` greps for the pass lines).

| Script | What it checks |
|---|---|
| `audit_probe.py <exe>` | 83 protocol and robustness cases: CRLF, garbage input, no legal move, clock handling, mate at the fifty-move boundary, ... |
| `audit_sem.py <exe>` | 31 rule and protocol checks against python-chess (castling, en passant, promotion, repetition, ...) |
| `edge_test.py <exe> [timeout]` | 303 edge cases of the UCI protocol; the process must stay alive, answer `isready` and give a legal `bestmove` for every `go` |
| `chess960_test.py <exe> [games] [seed]` | Chess960: legal moves against python-chess from random start positions, castling notation, short games |
| `clock_limit_test.py <exe>` | a clock next to a depth or node limit must be obeyed (all limits of a `go` apply at once) |
| `test_uci_features.py <exe>` | MultiPV, ponder, Clear Hash, UCI_LimitStrength, threads |
| `interop_test.py <exe>` | the engine driven by python-chess's own UCI client (ponder hits and misses, Chess960, new games) |
| `uci_fuzz.py <exe> <sequences> <seed>` | random command sequences; the engine must survive and answer |
| `race_test.py <exe> <runs>`, `infinite_test.py <exe>` | `go infinite` followed by an immediate `stop`; `go infinite` must wait for `stop` |
| `stress_stop.py <exe> <iterations> <seed> <threads>` | random go / stop / position / ucinewgame / setoption bursts with millisecond gaps |
| `longlist_test.py <exe>` | a move list of 4,000 plies |
| `BENCH` | the depth-12 bench node count of the source in `src/` (`kestrel bench 12`) |

The long test suite that also builds the engine with AddressSanitizer / UBSan, and the scripts that need local data files, are not part of this repository; their logs are in `logs/`.
