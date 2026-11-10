"""Score and Elo difference of the first level against the second one, from selfplay.js files of two different levels.
   python pair_scores.py out/n1_0.jsonl out/n1_1.jsonl ...    (the first level 'a' is the one that was white in the first game of the file)"""
import json, math, sys
from table_tools import short


def elo_of(p, n):
    p = min(max(p, 0.5 / n), 1 - 0.5 / n)
    return -400 * math.log10(1 / p - 1)


for path in sys.argv[1:]:
    w = d = l = 0
    names = None
    plies = 0
    for line in open(path, encoding="utf-8"):
        if not line.strip():
            continue
        g = json.loads(line)
        a_white = g["aWhite"]
        names = (g["wl"], g["bl"]) if a_white else (g["bl"], g["wl"])
        r = g["result"]
        plies += len(g["moves"])
        if r == "1/2-1/2": d += 1
        elif (r == "1-0") == a_white: w += 1
        else: l += 1
    n = w + d + l
    p = (w + 0.5 * d) / n if n else 0.5
    var = (w * (1 - p) ** 2 + d * (0.5 - p) ** 2 + l * p ** 2) / n if n else 0
    err = 1.96 * math.sqrt(var / n) if n else 0               # rough 95 % error from the variance of the game scores
    print("%-24s vs %-24s  +%d =%d -%d  score %.3f  Elo %+.0f  (%+.0f..%+.0f)  %d plies/game" % (
        short(names[0])[:24], short(names[1])[:24], w, d, l, p, elo_of(p, n), elo_of(p - err, n), elo_of(p + err, n), plies / n))
