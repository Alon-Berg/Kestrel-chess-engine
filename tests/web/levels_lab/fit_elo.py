"""Bradley-Terry ratings (Elo scale) of all configurations that played in the given selfplay.js files (a small draw prior keeps the ratings finite).
   python fit_elo.py "out/g1_*.jsonl"      (the names are the level specs; LEVEL_TABLE, see table_tools.py, shortens them)"""
import json, math, sys, glob
import numpy as np
from table_tools import short


def load(paths):
    games = []
    for p in paths:
        for line in open(p, encoding="utf-8"):
            if line.strip():
                games.append(json.loads(line))
    return games


def fit(games, prior=0.5):
    names = sorted({g["wl"] for g in games} | {g["bl"] for g in games})
    idx = {n: i for i, n in enumerate(names)}
    n = len(names)
    S = np.zeros((n, n)); N = np.zeros((n, n))
    for g in games:
        i, j = idx[g["wl"]], idx[g["bl"]]
        pw = {"1-0": 1.0, "0-1": 0.0}.get(g["result"], 0.5)
        S[i, j] += pw; S[j, i] += 1 - pw; N[i, j] += 1; N[j, i] += 1
    played = N > 0
    S = S + prior * 0.5 * played; N = N + prior * played                      # the draw prior
    r = np.zeros(n); k = math.log(10) / 400
    for _ in range(3000):
        P = 1 / (1 + 10 ** (-(r[:, None] - r[None, :]) / 400))
        grad = (S - N * P * played).sum(axis=1)
        hess = (N * P * (1 - P) * played).sum(axis=1) * k
        step = np.where(hess > 0, grad / np.maximum(hess, 1e-9), 0)
        r += 0.7 * np.clip(step, -100, 100)
        r -= r.mean()
    return names, r, N


if __name__ == "__main__":
    paths = []
    for a in sys.argv[1:]:
        paths += glob.glob(a)
    games = load(paths)
    names, r, N = fit(games)
    print(len(games), "games,", len(names), "configurations")
    for i in np.argsort(r):
        print("%7.0f  %s   (%d games)" % (r[i], short(names[i]), int(N[i].sum())))
