"""Matches of one level of the page against Stockfish at a UCI_Elo setting, through fastchess.
   python ladder.py --bot 5 --sf 1500 [--sf 1700 ...] --rounds 50 [--custom "{...}"] [--tc 10+0.1] [--conc 14]
   environment: FASTCHESS, STOCKFISH, DENO (paths of the programs), KESTREL_NET (the network, read by the engine wrapper)
   Prints wins, losses, draws and the Elo difference of the bot to each Stockfish setting (Stockfish's UCI_Elo is anchored to the CCRL scale)."""
import argparse, math, os, re, subprocess, time

HERE = os.path.dirname(os.path.abspath(__file__))
FASTCHESS = os.environ.get("FASTCHESS", "fastchess")
SF = os.environ.get("STOCKFISH", "stockfish")
DENO = os.environ.get("DENO", "deno")

ap = argparse.ArgumentParser()
ap.add_argument("--bot", default="5")
ap.add_argument("--custom", default="")
ap.add_argument("--sf", type=int, action="append", required=True)
ap.add_argument("--rounds", type=int, default=50)
ap.add_argument("--tc", default="10+0.1")
ap.add_argument("--conc", type=int, default=14)
ap.add_argument("--tag", default="ladder")
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--table", default="")          # a JSON table of level configs: --bot N then means Level N of that table
a = ap.parse_args()
if a.table:
    import json
    a.custom = json.dumps(json.load(open(a.table))[int(a.bot) - 1], separators=(",", ":"))
    a.tag = a.tag + "_t"
os.makedirs(os.path.join(HERE, "out"), exist_ok=True)


def elo_of(p, n):
    p = min(max(p, 0.5 / n), 1 - 0.5 / n)
    return -400 * math.log10(1 / p - 1)


for elo in a.sf:
    pgn = os.path.join(HERE, "out", "%s_bot%s_sf%d.pgn" % (a.tag, a.bot if not a.custom else "custom", elo))
    if os.path.exists(pgn): os.remove(pgn)
    cmd = [FASTCHESS, "-engine", "name=bot", "cmd=" + DENO, "args=run -A " + os.path.join(HERE, "uci_levels.js").replace("\\", "/"),
           "option.Level=" + a.bot, "option.Seed=%d" % a.seed]
    if a.custom:
        cmd.append("option.Custom=" + a.custom)
    cmd += ["-engine", "name=sf%d" % elo, "cmd=" + SF, "option.UCI_LimitStrength=true", "option.UCI_Elo=%d" % elo, "option.Threads=1", "option.Hash=16",
            "-each", "proto=uci", "tc=" + a.tc, "-rounds", str(a.rounds), "-games", "2", "-concurrency", str(a.conc), "-pgnout", "file=" + pgn,
            "-srand", str(a.seed), "-maxmoves", "250", "-recover"]
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = r.stdout + r.stderr
    open(pgn.replace(".pgn", ".log"), "w", encoding="utf-8").write(out)
    m = re.findall(r"Games: (\d+), Wins: (\d+), Losses: (\d+), Draws: (\d+)", out)
    if not m:
        print("no result for Stockfish", elo, out[-600:], flush=True)
        continue
    n, w, l, d = map(int, m[-1])
    p = (w + 0.5 * d) / max(1, n)
    var = (w * (1 - p) ** 2 + d * (0.5 - p) ** 2 + l * p ** 2) / n
    err = 1.96 * math.sqrt(var / n)
    print("bot %s%s vs Stockfish %d: +%d -%d =%d  (%d games)  score %.3f  Elo difference %+.0f (%+.0f..%+.0f)  [%.0f s]" % (
        a.bot, " custom" if a.custom else "", elo, w, l, d, n, p, elo_of(p, n), elo_of(p - err, n), elo_of(p + err, n), time.time() - t0), flush=True)
