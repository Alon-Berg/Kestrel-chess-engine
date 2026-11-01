"""v36.1 regression test: a clock next to a depth / node limit must be obeyed (UCI: all limits of a `go` apply at once).
usage: python clock_limit_test.py engine.exe
v36 and older ignored wtime/btime whenever `depth` or `nodes` was also given, so `go wtime 50 btime 50 depth 30` searched to depth 30 (minutes) and a
game with both a time control and a depth limit could lose on time.  The cases below also check that the limits WITHOUT a clock are unchanged."""
import re, subprocess, sys, threading, time

exe = sys.argv[1]
KIWI = "position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
results = []

def run(go, stop_after=None, limit=15.0):
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    out = []
    threading.Thread(target=lambda: [out.append(l.rstrip()) for l in p.stdout], daemon=True).start()
    def send(s): p.stdin.write(s + "\n"); p.stdin.flush()
    for c in ["uci", "setoption name OwnBook value false", "setoption name Experience value false", "setoption name Threads value 1", "isready"]: send(c)
    while "readyok" not in out: time.sleep(0.01)
    send(KIWI); t0 = time.time(); send(go)
    while time.time() - t0 < limit and not any(l.startswith("bestmove") for l in out): time.sleep(0.005)
    dt = time.time() - t0
    done = any(l.startswith("bestmove") for l in out)
    if not done: send("stop"); time.sleep(0.5)
    depths = [int(m.group(1)) for l in out for m in [re.match(r"info depth (\d+)", l)] if m]
    nodes = [int(m.group(1)) for l in out for m in [re.search(r" nodes (\d+)", l)] if m]
    try: send("quit")
    except Exception: pass
    time.sleep(0.2)
    if p.poll() is None: p.kill()
    return done, dt, (max(depths) if depths else 0), (max(nodes) if nodes else 0)

def check(name, ok, detail):
    results.append(ok); print(("PASS " if ok else "FAIL ") + name + "   " + detail, flush=True)

d, t, dep, n = run("go wtime 50 btime 50 depth 30");               check("clock + depth limit: answers quickly", d and t < 2.0, "%.2fs, depth %d" % (t, dep))
d, t, dep, n = run("go wtime 100 btime 100 nodes 100000000000");   check("clock + node limit: answers quickly", d and t < 2.0, "%.2fs, depth %d" % (t, dep))
d, t, dep, n = run("go wtime 2000 btime 2000 winc 0 binc 0 depth 40"); check("2 s clock + depth 40: stops on the clock (< 1 s)", d and t < 1.0, "%.2fs, depth %d" % (t, dep))
d, t, dep, n = run("go wtime 600000 btime 600000 depth 7");        check("big clock + depth 7: depth limit ends the search", d and dep == 7, "%.2fs, depth %d" % (t, dep))
d, t, dep, n = run("go depth 7");                                    check("depth limit without a clock: unchanged", d and dep == 7, "%.2fs, depth %d" % (t, dep))
d, t, dep, n = run("go wtime 0 btime 0 depth 6");                    check("zero clock + depth 6: depth limit (no clock budget)", d and dep == 6, "%.2fs, depth %d" % (t, dep))
d, t, dep, n = run("go nodes 30000");                                check("node limit without a clock: unchanged", d and 10000 < n < 60000, "%.2fs, nodes %d (v36 gives the same count)" % (t, n))
d, t, dep, n = run("go movetime 200 depth 40");                      check("movetime + depth 40: movetime wins", d and t < 0.8, "%.2fs, depth %d" % (t, dep))
d, t, dep, n = run("go wtime 3000 btime 3000");                      check("clock alone: unchanged (about 3000/28 ms)", d and t < 1.0, "%.2fs, depth %d" % (t, dep))
print("clock_limit_test: %d checks, %d failed" % (len(results), results.count(False)))
sys.exit(0 if all(results) else 1)
