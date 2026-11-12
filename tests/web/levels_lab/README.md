# Measuring the levels of the play page

The twelve weaker levels in `docs/play/levels.js` (the play page offers only full strength since 2026-11-12) were measured with the programs in this folder. They use the same
`levels.js` and the same WebAssembly engine as the page, so a number measured here is a number of the levels. The table of results is in [`docs/BUILDING.md`](../../../docs/BUILDING.md#the-levels-of-the-page).

You need Deno (https://deno.com), Python 3 with `chess` and `numpy`, the network of the release (`kestrel_net_3_2.bin`), and for two of the steps Stockfish and fastchess.

| Environment variable | Meaning |
|---|---|
| `KESTREL_NET` | the network file (default: `kestrel_net_3_2.bin` in the current folder) |
| `STOCKFISH` | Stockfish (judging the games, and the ladder) |
| `FASTCHESS` | fastchess (the ladder) |
| `DENO` | Deno (default: `deno` from the PATH) |
| `SF_DEPTH`, `SF_JOBS` | depth (default 12) and number of Stockfish processes (default 14) of `analyze.py` |
| `LEVEL_TABLE` | a table file (see below); names of levels are then printed as `L1`, `L2`, ... |

## The steps

Run everything in this folder (`cd tests/web/levels_lab`). The games are written to `out/`.

```
# 1. games: every level of the page against itself (equal opponents, as in rating statistics), and against its neighbours
set KESTREL_NET=C:\path\to\kestrel_net_3_2.bin
python run_selfplay.py --tag self --levels 1,2,3,4,5,6,7,8,9,10,11,12 --games 60
python table_tools.py pairs tables/final.json out/pairs.json
python run_selfplay.py --tag nb --pairs-file out/pairs.json --games 150

# 2. mistakes: Stockfish judges every position once; average centipawn loss, share of moves losing 100 / 200 / 300 centipawns or more, and how often a level wins
#    a game after it led by 600 centipawns or more
set STOCKFISH=C:\path\to\stockfish.exe
set LEVEL_TABLE=tables/final.json
python analyze.py out/self_*.jsonl

# 3. strength: score and Elo difference of every level against the one below it; ratings of all of them together
python pair_scores.py out/nb_*.jsonl
python fit_elo.py "out/nb_*.jsonl"

# 4. Stockfish's ladder: a level against Stockfish at a UCI_Elo setting (fastchess, 10 s + 0.1 s per game); --table makes --bot a line of a table file
set FASTCHESS=C:\path\to\fastchess.exe
python ladder.py --table tables/final.json --bot 12 --sf 2500 --sf 2800 --rounds 15
```

`run_selfplay.py --levels` takes level numbers, `r1300` (the level named about 1300), `full`, or a JSON level such as `{"depth":2,"k":6,"temp":160,"slip":0.064}`;
that is how other settings were tried before they went into `levels.js`. `tables/final.json` is the table of `levels.js` (without ratings and names).

`uci_levels.js` is the page's levels as a UCI engine (`deno run -A uci_levels.js`, options `Level`, `Custom` and `Seed`), which lets any chess program play against them.

## What the numbers mean

* **ACPL** is the average centipawn loss of a move: the score (Stockfish, depth 12) of the best move minus the score of the move played, both seen from the side to move,
  cut at +-1000 centipawns as lichess does. People of low rating lose about 60 to 100 centipawns per move by this measure and strong players 20 to 35 (my estimate; the
  figure depends on the depth and on the time control of the games and the analysis).
* The levels are **not** measured against people. Their ratings are an estimate: the lower levels were set so that their mistakes per move (a quarter of the moves lose
  a tenth of a pawn or more, one in twenty loses a piece or more at "about 1300") are what I would expect of players of that rating, and the steps between the levels are
  about 200 Elo in games between the levels themselves. If a level feels too strong or too weak, change its `temp` and `slip` in `docs/play/levels.js`.
