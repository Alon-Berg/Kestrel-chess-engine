"""Functional tests of the UCI features: MultiPV, Ponder/ponderhit, Clear Hash, hashfull, UCI_LimitStrength/UCI_Elo, Threads.
usage: python test_uci_features.py engine.exe"""
import re, subprocess, sys, threading, time
import chess

exe = sys.argv[1]
results = []
def check(name, ok, detail=""):
    results.append(ok); print(("PASS " if ok else "FAIL ") + name + ("   " + detail if detail else ""), flush=True)

class Eng:
    def __init__(self):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.lines = []
        threading.Thread(target=lambda: [self.lines.append((time.time(), l.rstrip())) for l in self.p.stdout], daemon=True).start()
        self.send("uci"); self.send("setoption name Experience value false"); self.send("setoption name OwnBook value false"); self.send("isready"); self.wait("readyok", 10)
    def send(self, s): self.p.stdin.write(s + "\n"); self.p.stdin.flush()
    def mark(self): return len(self.lines)
    def wait(self, prefix, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for ts, l in self.lines[since:]:
                if l.startswith(prefix): return l
            time.sleep(0.01)
        return None
    def new(self, since): return [l for _, l in self.lines[since:]]
    def close(self):
        try: self.send("quit")
        except OSError: pass
        time.sleep(0.2)
        if self.p.poll() is None: self.p.kill()

def pv_legal(fen, pv_moves):
    b = chess.Board(fen)
    for m in pv_moves:
        mv = chess.Move.from_uci(m)
        if mv not in b.legal_moves: return False
        b.push(mv)
    return True

START = chess.STARTING_FEN
# ---------------- MultiPV
e = Eng(); e.send("setoption name MultiPV value 3"); e.send("isready"); e.wait("readyok", 5)
m = e.mark(); e.send("position startpos"); e.send("go depth 10"); bm = e.wait("bestmove", 30, m)
info = [l for l in e.new(m) if l.startswith("info depth 10 ") and "multipv" in l]
firsts, scores, legal = [], [], True
for l in info:
    g = re.search(r"multipv (\d) score (cp|mate) (-?\d+).* pv (.*)$", l)
    if not g: continue
    mvs = g.group(4).split(); firsts.append(mvs[0]); scores.append(int(g.group(3)) if g.group(2) == "cp" else 10000 if int(g.group(3)) > 0 else -10000)
    legal = legal and pv_legal(START, mvs)
check("MultiPV 3: three lines at depth 10", len(info) == 3, "%d lines" % len(info))
check("MultiPV 3: distinct first moves, PVs legal", len(set(firsts)) == 3 and legal, " ".join(firsts))
check("MultiPV 3: scores not increasing", scores == sorted(scores, reverse=True), str(scores))
check("MultiPV 3: bestmove = line 1", bm is not None and firsts and bm.split()[1] == firsts[0], str(bm))
e.send("setoption name MultiPV value 1"); e.close()

# ---------------- Ponder
e = Eng(); e.send("setoption name Ponder value true"); e.send("isready"); e.wait("readyok", 5)
m = e.mark(); e.send("position startpos moves e2e4"); e.send("go ponder wtime 4000 btime 4000 winc 0 binc 0")
time.sleep(1.5)
early = [l for l in e.new(m) if l.startswith("bestmove")]
check("ponder: no bestmove while pondering", not early)
t_hit = time.time(); e.send("ponderhit"); bm = e.wait("bestmove", 10, m)
lat = (time.time() - t_hit)
check("ponder: bestmove after ponderhit (with ponder move)", bm is not None and " ponder " in bm, "%s in %.2f s" % (bm, lat))
if bm and " ponder " in bm:
    mv = bm.split()[1]; pm = bm.split()[3]
    b = chess.Board(); b.push_uci("e2e4"); ok = chess.Move.from_uci(mv) in b.legal_moves
    if ok: b.push_uci(mv); ok = chess.Move.from_uci(pm) in b.legal_moves
    check("ponder: bestmove and ponder move are legal", ok, bm)
m = e.mark(); e.send("position startpos moves e2e4 e7e5"); e.send("go ponder wtime 4000 btime 4000"); time.sleep(0.5)
e.send("stop"); bm2 = e.wait("bestmove", 5, m)
check("ponder: stop ends a pondering search with a bestmove", bm2 is not None, str(bm2))
e.close()

# ---------------- Clear Hash, hashfull
e = Eng(); m = e.mark(); e.send("position startpos"); e.send("go depth 12"); e.wait("bestmove", 30, m)
hf = [int(g.group(1)) for l in e.new(m) for g in [re.search(r"hashfull (\d+)", l)] if g]
check("hashfull is reported", len(hf) > 0 and max(hf) > 0, "values %s" % hf[-3:])
m = e.mark(); e.send("setoption name Clear Hash"); e.send("isready"); r = e.wait("readyok", 5, m)
e.send("position startpos"); m = e.mark(); e.send("go depth 8"); bm = e.wait("bestmove", 20, m)
hf2 = [int(g.group(1)) for l in e.new(m) for g in [re.search(r"hashfull (\d+)", l)] if g]
check("Clear Hash works and the engine keeps searching", r is not None and bm is not None, "hashfull after clear: %s" % hf2[:2])
e.close()

# ---------------- UCI_LimitStrength
e = Eng(); e.send("setoption name UCI_LimitStrength value true"); e.send("setoption name UCI_Elo value 1500"); e.send("isready"); e.wait("readyok", 5)
m = e.mark(); e.send("position startpos"); e.send("go movetime 2000"); bm = e.wait("bestmove", 10, m)
nodes = [int(g.group(1)) for l in e.new(m) for g in [re.search(r" nodes (\d+)", l)] if g]
check("UCI_Elo 1500 limits the search", bm is not None and nodes and max(nodes) < 5000, "max nodes %s" % (max(nodes) if nodes else None))
e.send("setoption name UCI_Elo value 3200"); e.send("setoption name UCI_LimitStrength value false"); e.close()

# ---------------- Threads
for th in (1, 4):
    e = Eng(); e.send("setoption name Threads value %d" % th); e.send("setoption name Hash value 128"); e.send("isready"); e.wait("readyok", 5)
    m = e.mark(); e.send("position startpos"); e.send("go movetime 1000"); bm = e.wait("bestmove", 10, m)
    nps = [int(g.group(1)) for l in e.new(m) for g in [re.search(r" nps (\d+)", l)] if g]
    check("Threads %d: search completes with a legal move" % th, bm is not None and chess.Move.from_uci(bm.split()[1]) in chess.Board().legal_moves, "nps %s" % (nps[-1] if nps else None))
    e.close()

print("\n%d checks, %d failed" % (len(results), results.count(False)))
sys.exit(1 if False in results else 0)
