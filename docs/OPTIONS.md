# UCI options of Kestrel 3.0

Add the executable to any UCI program (Cute Chess, Arena, En Croissant, Lucas Chess, fastchess, ...). The options below are what `uci` lists; every other option name of earlier versions is accepted and ignored.

| Option | Type | Default | Range | What it does |
|---|---|---|---|---|
| `Hash` | spin | 256 | 1 - 4096 | Size of the transposition table in MB. (2.0: 64.) |
| `Threads` | spin | 1 | 1 - 8 | Search threads (Lazy SMP). With `MultiPV` above 1 the search uses one thread. |
| `OwnBook` | check | false | | Use the opening book `claude_book.txt` (it must lie next to the executable) in the first 24 plies. Not used in analysis mode, for `go infinite` or limited `go` commands, for Chess960 or with `MultiPV` above 1. (2.0: on.) |
| `Move Overhead` | spin | 5 | 0 - 5000 | Milliseconds kept in reserve per move for the GUI and the connection. |
| `MultiPV` | spin | 1 | 1 - 8 | Number of lines reported. |
| `Ponder` | check | false | | Think on the opponent's time (the GUI sends `go ponder`). |
| `Clear Hash` | button | | | Empties the transposition table and the history tables. |
| `UCI_LimitStrength` | check | false | | Play weaker: the nodes searched per move are limited according to `UCI_Elo` (approximate, not calibrated against rated players). |
| `UCI_Elo` | spin | 3200 | 1320 - 3200 | Target rating for `UCI_LimitStrength`. |
| `UseNNUE` | check | true | | The network. Switch it off only for tests: the engine then falls back to the old hand-written evaluation, which is much weaker. |
| `Experience` | check | false | | Position learning: remembers moves that won or lost in earlier games and replays or avoids them. The file is written when games end. (2.0: on.) |
| `ExperienceFile` | string | `kestrel_experience_v2.dat` | | Where the learning file lives (a bare name means next to the executable). |
| `EvalFile` | string | `<internal>` | | Load another network file of the same format (see `kestrel_net_3_0.bin` in the release); `<internal>` is the network inside the exe. |
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
* `go` understands `wtime btime winc binc movestogo depth nodes movetime mate infinite ponder searchmoves`.
* Versions 2.0 and 2.1 declared a few options that no longer do anything (`NNBlend`, `Net2Share`, `UseNet2`) and development options with the defaults of 1.x. `NNBlend` was declared with default 70 although the engine runs at 100, so a program that sent the declared defaults mixed the old hand-written evaluation into the search. Since 2.2 (and so in 2.3) these options are not declared any more and the release builds ignore them (`tests/options_test.py` checks that sending every declared default changes nothing).
* Defaults changed in 2.1 for chess-engine testers: `Hash` 256 MB, `OwnBook` and `Experience` off.
