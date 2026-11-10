"""Average centipawn loss and mistake rates per level, from the games of selfplay.js (every position is judged once by Stockfish).

   python analyze.py out/s1_*.jsonl            environment: STOCKFISH (path of the engine), SF_DEPTH (default 12), SF_JOBS (default 14), LEVEL_TABLE (see table_tools.py)
   Scores are cut at +-1000 centipawns as lichess does; the loss of a move is  S(before) + S(after)  with S = the score for the side to move.
   Results are cached in eval_cache_d<depth>.json (position -> score) so that repeated runs only judge new positions."""
import json, math, os, sys, collections, time
import multiprocessing as mp
import chess, chess.engine
from table_tools import short

SF = os.environ.get("STOCKFISH", "stockfish")
DEPTH = int(os.environ.get("SF_DEPTH", "12"))
JOBS = int(os.environ.get("SF_JOBS", "14"))
CAP = 1000
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "eval_cache_d%d.json" % DEPTH)
eng = None


def init():
    global eng
    for attempt in range(5):
        try:
            eng = chess.engine.SimpleEngine.popen_uci(SF, timeout=120)
            eng.configure({"Threads": 1, "Hash": 64})
            return
        except Exception:
            eng = None
            time.sleep(5)


def judge(key):
    global eng
    try:
        b = chess.Board(key + " 0 1")
        if b.is_checkmate():
            return key, -CAP
        if b.is_stalemate() or b.is_insufficient_material():
            return key, 0
        if eng is None:
            init()
        info = eng.analyse(b, chess.engine.Limit(depth=DEPTH), game=None)
        s = info["score"].relative.score(mate_score=100000)
        return key, max(-CAP, min(CAP, s))
    except Exception:
        try:
            eng.close()
        except Exception:
            pass
        eng = None
        return key, None


def win_pct(cp):
    return 50 + 50 * (2 / (1 + math.exp(-0.00368208 * cp)) - 1)


def game_keys(g):
    b = chess.Board()
    keys = [b.epd()]
    for u in g["moves"]:
        b.push(chess.Move.from_uci(u))
        keys.append(b.epd())
    return keys


def main(paths):
    games = []
    for p in paths:
        for line in open(p, encoding="utf-8"):
            if line.strip():
                games.append(json.loads(line))
    cache = json.load(open(CACHE)) if os.path.exists(CACHE) else {}
    allkeys = [game_keys(g) for g in games]
    allneed = sorted({k for ks in allkeys for k in ks} - set(cache))
    print("%d games, %d positions to judge (%d cached), depth %d" % (len(games), len(allneed), len(cache), DEPTH), flush=True)
    for attempt in range(3):
        need = sorted({k for ks in allkeys for k in ks} - set(cache))
        if not need:
            break
        with mp.Pool(JOBS, initializer=init) as pool:
            for i, (k, s) in enumerate(pool.imap_unordered(judge, need, chunksize=16)):
                if s is not None:
                    cache[k] = s
                if (i + 1) % 10000 == 0:
                    print("  pass %d: judged %d/%d" % (attempt + 1, i + 1, len(need)), flush=True)
                    json.dump(cache, open(CACHE, "w"))
        json.dump(cache, open(CACHE, "w"))
    missing = {k for ks in allkeys for k in ks} - set(cache)
    if missing:
        print("WARNING: %d positions could not be judged; their games are skipped" % len(missing), flush=True)
        keep = [(g, ks) for g, ks in zip(games, allkeys) if not any(k in missing for k in ks)]
        games = [g for g, _ in keep]; allkeys = [ks for _, ks in keep]
    stats = collections.defaultdict(lambda: collections.Counter())
    for g, ks in zip(games, allkeys):
        S = [cache[k] for k in ks]
        W = [(s if i % 2 == 0 else -s) for i, s in enumerate(S)]          # white's point of view
        lead = None
        for w in W:                                                         # the side that was first ahead by 600 centipawns or more
            if w >= 600: lead = "w"; break
            if w <= -600: lead = "b"; break
        for i in range(len(g["moves"])):
            name = g["wl"] if i % 2 == 0 else g["bl"]
            loss = max(0, S[i] + S[i + 1])
            drop = max(0.0, win_pct(S[i]) - win_pct(-S[i + 1]))             # win-percentage drop for the mover: before = S[i], after = -S[i+1]
            st = stats[name]
            st["n"] += 1; st["loss"] += loss
            st["ge100"] += loss >= 100; st["ge200"] += loss >= 200; st["ge300"] += loss >= 300
            st["w10"] += drop >= 10; st["w20"] += drop >= 20; st["w30"] += drop >= 30
        for side, name in (("w", g["wl"]), ("b", g["bl"])):
            st = stats[name]; st["games"] += 1; st["plies"] += len(g["moves"]); st["ms"] += g["ms"][side]
            res = g["result"]
            if res == "1/2-1/2": st["draws"] += 1
            elif (res == "1-0") == (side == "w"): st["wins"] += 1
            else: st["losses"] += 1
            if lead == side:                                                # this level led by 600+: did it win?
                st["led"] += 1
                if (res == "1-0" and side == "w") or (res == "0-1" and side == "b"): st["converted"] += 1
    print("%-34s %6s %7s %6s %6s %6s %6s %6s %6s %6s %7s %9s" % ("level", "moves", "ACPL", ">=100", ">=200", ">=300", "w>=10", "w>=20", "w>=30", "plies", "ms/move", "converts"))
    for name, st in sorted(stats.items(), key=lambda kv: (len(short(kv[0])), short(kv[0]))):
        n = st["n"]
        print("%-34s %6d %7.1f %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%% %5.1f%% %6.0f %7.1f %8.0f%%" % (
            short(name)[:34], n, st["loss"] / n, 100 * st["ge100"] / n, 100 * st["ge200"] / n, 100 * st["ge300"] / n,
            100 * st["w10"] / n, 100 * st["w20"] / n, 100 * st["w30"] / n, st["plies"] / st["games"], st["ms"] / n,
            100 * st["converted"] / max(1, st["led"])))


if __name__ == "__main__":
    mp.freeze_support()
    main(sys.argv[1:])
