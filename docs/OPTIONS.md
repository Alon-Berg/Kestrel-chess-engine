# UCI options of Kestrel 3.2

Add the executable to any UCI program (Cute Chess, Arena, En Croissant, Lucas Chess, fastchess, ...). The options below are what `uci` lists; every other option name of earlier versions is accepted and ignored.

| Option | Type | Default | Range | What it does |
|---|---|---|---|---|
| `Hash` | spin | 256 | 1 - 1048576 | Size of the transposition table in MB, rounded down to a power of two (24,000 gives a table of 16,384 MB). A request above 3/4 of the installed memory is cut to that size (an `info string` says so). Up to 3.0 the largest value was 4096. (2.0: 64.) |
| `Threads` | spin | 1 | 1 - 128 | Search threads (Lazy SMP). Up to 3.0 the largest value was 8. Use at most as many threads as the computer has logical processors (more do not help) and, from 64 threads on, raise `Move Overhead` to 50 - 100 ms. With `MultiPV` above 1 the search uses one thread. |
| `OwnBook` | check | false | | Use the opening book `claude_book.txt` (it must lie next to the executable) in the first 24 plies. Not used in analysis mode, for `go infinite` or limited `go` commands, for Chess960 or with `MultiPV` above 1. (2.0: on.) |
| `Move Overhead` | spin | 5 | 0 - 5000 | Milliseconds kept in reserve per move for the GUI and the connection. Since 3.2 another millisecond per 8 threads above 8 is held back (15 ms with 128 threads). |
| `MultiPV` | spin | 1 | 1 - 8 | Number of lines reported. |
| `Ponder` | check | false | | Think on the opponent's time (the GUI sends `go ponder`). |
| `Clear Hash` | button | | | Empties the transposition table and the history tables. |
| `UCI_LimitStrength` | check | false | | Play weaker: the nodes searched per move are limited according to `UCI_Elo` (approximate, not calibrated against rated players). Use one thread: the limit counts the nodes of the main thread only, so with more threads the helpers add search and the engine is stronger than set. |
| `UCI_Elo` | spin | 3200 | 1320 - 3200 | Target rating for `UCI_LimitStrength`. |
| `UseNNUE` | check | true | | The network. Switch it off only for tests: the engine then falls back to the old hand-written evaluation, which is much weaker. |
| `Experience` | check | false | | Position learning: remembers moves that won or lost in earlier games and replays or avoids them. The file is written when games end. (2.0: on.) |
| `ExperienceFile` | string | `kestrel_experience_v2.dat` | | Where the learning file lives (a bare name means next to the executable). |
| `EvalFile` | string | `<internal>` | | Load another network file of the same format (see `kestrel_net_3_2.bin` in the release, the same file as `kestrel_net_3_0.bin` and `kestrel_net_3_1.bin`); `<internal>` is the network inside the exe. |
| `EvalScale` | spin | 100 | 50 - 200 | Percent: multiplies the network's output. 100 was best in a test of 60, 80, 125 and 160 (600 games each); changing it is not recommended. |
| `SyzygyPath` | string | `<empty>` | | New in 3.0. Folders with Syzygy tablebase files (`;` between folders on Windows, `:` on Linux and macOS). Empty = no tablebases. See [SYZYGY.md](SYZYGY.md). |
| `SyzygyProbeDepth` | spin | 1 | 1 - 100 | New in 3.0. Probe inside the search only at nodes with at least this remaining depth. |
| `Syzygy50MoveRule` | check | true | | New in 3.0. Respect the fifty-move rule (a win that needs more than 50 moves counts as a draw). |
| `SyzygyProbeLimit` | spin | 7 | 0 - 7 | New in 3.0. Largest number of men that is probed. |
| `UCI_ShowWDL` | check | false | | Adds win / draw / loss in per mille to the `info` lines. |
| `UCI_AnalyseMode` | check | false | | Analysis behaviour: no book and no learning. |
| `UCI_Chess960` | check | false | | Fischer random chess (king-takes-rook castling notation). |

## Notes

* Syzygy tablebases are supported since 3.0 (3-4-5 men tested), see [SYZYGY.md](SYZYGY.md). With tablebases in use the `info` lines also carry `tbhits`.
* `go` understands `wtime btime winc binc movestogo depth nodes movetime mate infinite ponder searchmoves`. Since 3.1 the limit of `go nodes N` is compared with the node count of the main thread only (in 3.0 every thread compared its own count, so with several threads the first helper that reached N ended the search, often before the main thread had finished depth 1); the `nodes` of the `info` lines count all threads and can be larger than N.
* Many threads (3.1): the threads share only the transposition table and the stop flag; a table of many GB is cleared by several threads (16 GB in about a second). More threads than logical processors did not help in a test (64 and 128 threads on a 22-processor laptop scored +11 Elo against one thread, 8 threads +108 to +123 Elo); see [CHANGELOG.md](../CHANGELOG.md).
* Versions 2.0 and 2.1 declared a few options that no longer do anything (`NNBlend`, `Net2Share`, `UseNet2`) and development options with the defaults of 1.x. `NNBlend` was declared with default 70 although the engine runs at 100, so a program that sent the declared defaults mixed the old hand-written evaluation into the search. Since 2.2 (and so in 2.3) these options are not declared any more and the release builds ignore them (`tests/options_test.py` checks that sending every declared default changes nothing).
* Time management (3.2): with a clock the engine decides after every iteration whether to start the next one by a smooth rule (the best move stayed the same, the score fell, one move got most of the nodes); see [CHANGELOG.md](../CHANGELOG.md). It does not apply to `go movetime`, `depth`, `nodes` or `infinite`.
* Defaults changed in 2.1 for chess-engine testers: `Hash` 256 MB, `OwnBook` and `Experience` off.
