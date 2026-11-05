# Syzygy tablebases in Kestrel 3.0

Kestrel 3.0 can read Syzygy endgame tablebases. Without tablebase files nothing changes: the option `SyzygyPath` is empty by default and the engine searches as it did before.

## Getting the files

The files come from <https://tablebase.lichess.ovh/tables/standard/>:

| Set | Folders | Size |
|---|---|---|
| 3, 4 and 5 men | `3-4-5-wdl` (`*.rtbw`) and `3-4-5-dtz` (`*.rtbz`) | about 940 MB together |
| 6 men | see the site | about 160 GB (WDL 73 GB, DTZ 87 GB) |
| 7 men | see the site | about 18 TB |

Put the `.rtbw` (WDL) and `.rtbz` (DTZ) files into one folder, or spread them over several folders, and give the folders to the engine with `SyzygyPath` (separated by `;` on Windows and by `:` on Linux and macOS). Only the 3-4-5-men set has been tested for this release; 6 and 7 men use the same code but have not been run here.

## Options

| Option | Type | Default | Range | What it does |
|---|---|---|---|---|
| `SyzygyPath` | string | `<empty>` | | Folders with the tablebase files. The engine prints `info string Syzygy: tablebases up to N men found` when it has read them. |
| `SyzygyProbeDepth` | spin | 1 | 1 - 100 | Probe inside the search only at nodes with at least this remaining depth. Raise it on a slow disk. |
| `Syzygy50MoveRule` | check | true | | Respect the fifty-move rule. With `false` a win that needs more than 50 moves counts as a win, not as a draw. |
| `SyzygyProbeLimit` | spin | 7 | 0 - 7 | Largest number of men that is probed (the tables found limit it further). |

## What the engine does with the tables

* **Inside the search** it looks a position up (win, draw or loss) right after a capture or a pawn move, when the position has at most as many men as the largest table, no castling rights and a fifty-move counter of zero. The result ends the search of that line at once and is stored in the transposition table. A tablebase win is scored as a very large number that falls with the distance (about 28000 minus the ply, always above every evaluation and below every mate score), so the engine prefers the quickest tablebase win.
* **At the root** (a position that is already in the tablebases) it asks the DTZ files which moves keep the best result under the fifty-move rule, and it searches only those. In a won position that means only the fastest wins: the moves with the smallest DTZ (a capture or a pawn move that keeps the win counts as 1, a mating move too), plus the wins with a DTZ of at most 3 so that the search can still prefer a short mate it sees. Without this the search could not tell the winning moves apart (they all score as a tablebase win) and a long ending could use up the whole fifty-move budget. If the DTZ files are missing it uses the WDL files instead, which keeps the result but does not guarantee progress. When only one move is left it plays it at once. The engine prints `info string Syzygy: root position in the tablebases, N of M moves excluded` when this happens.
* **`tbhits`** appears in the `info` lines when tablebases are in use.
* The root filter is not used with `MultiPV` above 1 or with `go searchmoves`.
* Test command (not a UCI command): `tbprobe` prints the result and the moves that would be searched for the current position.

## Tests that were run (3-4-5 men)

All with the files from the table above and python-chess only as the independent reader of the same files:

* 1,500 random positions with 3 to 5 men: **0** differences in the win/draw/loss result.
* Root filter: 1,483 positions checked, **0** problems (every kept move has the best win/draw/loss class, and in won positions the kept moves are the fastest wins, see above).
* Search: in 60 positions `go depth 8` with 4 threads returned one of the kept moves, **0** problems.
* Conversion: 30 of 30 won endgames were converted to a checkmate before the fifty-move rule could end the game, against a defender that always plays the move that takes longest to lose.
* A 600-game match at 3+0.03 with tablebases (3-4-5 men) against the same build without: 199 wins, 165 losses, 236 draws (+19.7 ± 17.5 Elo). All 600 games ended normally. That is no sign of a problem; the gain is inside the noise of a match this size.

Two limits: positions with castling rights are never probed (they cannot be in the tables), and DTM tables are not used.

## Credits

The probing code is **Fathom** (<https://github.com/jdart1/Fathom>), (c) 2013-2018 Ronald de Man, (c) 2015 basil00, (c) 2016-2025 Jon Dart, MIT licence. It is folded into `src/kestrel_3_0.cpp` (namespace `fathom`) with its licence text at the start of the block. The Syzygy table format is by Ronald de Man.
