r"""Semantic audit of Kestrel's UCI behaviour (1.1 bug search, 2026-10-27).  usage: python audit_sem.py <exe>
Uses python-chess as the rules oracle.  Prints PASS / FAIL per check."""
import os, re, subprocess, sys, threading, time
import chess
EXE = sys.argv[1]; CWD = os.path.dirname(EXE)
OFF = ["setoption name Experience value false", "setoption name OwnBook value false"]

class Eng:
    def __init__(self, extra=()):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=CWD, text=True, bufsize=1)
        self.lines = []; self.lock = threading.Lock()
        threading.Thread(target=self._rd, daemon=True).start()
        self.send("uci"); self.wait("uciok")
        for c in list(OFF) + list(extra): self.send(c)
        self.send("isready"); self.wait("readyok")
    def _rd(self):
        for l in self.p.stdout:
            with self.lock: self.lines.append(l.rstrip("\r\n"))
    def send(self, c):
        try: self.p.stdin.write(c + "\n"); self.p.stdin.flush()
        except OSError: pass
    def wait(self, prefix, timeout=30, start=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                for i in range(start, len(self.lines)):
                    if self.lines[i].startswith(prefix): return i
            if self.p.poll() is not None: return -1
            time.sleep(0.01)
        return -1
    def go(self, fen, cmd, timeout=30):
        with self.lock: start = len(self.lines)
        self.send("position fen " + fen); self.send(cmd)
        i = self.wait("bestmove", timeout, start)
        with self.lock: seg = self.lines[start:(i + 1 if i >= 0 else len(self.lines))]
        return seg, (seg[-1] if i >= 0 else None)
    def close(self):
        self.send("quit")
        try: self.p.wait(5)
        except Exception: self.p.kill()

res = []
def check(name, ok, ev=""):
    res.append((name, ok)); print(("PASS  " if ok else "FAIL  ") + name + (("  | " + str(ev)[:260]) if (ev and not ok) or ev else ""), flush=True)
def score_of(seg):
    sc = None
    for l in seg:
        m = re.search(r"score (cp|mate) (-?\d+)", l)
        if m and " multipv " not in l or (m and "multipv 1 " in l): sc = (m.group(1), int(m.group(2)))
    return sc

e = Eng()
# S1: no legal move
for fen, name in (("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", "stalemate"), ("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3", "checkmate")):
    seg, bm = e.go(fen, "go depth 5")
    check("no legal move (%s): bestmove line" % name, bm is not None and (bm.split()[1] in ("0000", "(none)")), bm)
# S2: 50-move rule and mate precedence
seg, bm = e.go("k7/8/1K6/8/8/8/8/7R w - - 99 80", "go depth 6"); check("clock 99, mate in 1 available: plays Rh8#", bm and bm.split()[1] == "h1h8" and score_of(seg) == ("mate", 1), (bm, score_of(seg)))
seg, bm = e.go("k7/8/1K6/8/8/8/8/7R w - - 100 80", "go depth 6"); sc = score_of(seg); check("clock 100, mate in 1: like Stockfish the mate counts", sc == ("mate", 1), (bm, sc))
seg, bm = e.go("k7/8/1K6/8/8/8/8/7R w - - 98 80", "go depth 6"); check("clock 98, mate in 1: still mate", score_of(seg) == ("mate", 1), score_of(seg))
seg, bm = e.go("8/8/8/4k3/8/4K3/8/7R w - - 97 90", "go depth 10"); sc = score_of(seg); check("clock 97, long win (KRK): not a mate score", sc is not None and (sc[0] == "cp" or sc[1] > 3) , sc)
# S3: insufficient material
for fen, name in (("8/8/8/4k3/8/4K3/8/8 w - - 0 1", "KvK"), ("8/8/8/4k3/8/4K3/8/5N2 w - - 0 1", "KNvK"), ("8/8/8/4k3/8/4K3/8/5B2 w - - 0 1", "KBvK"), ("8/8/3b4/4k3/8/4K3/8/5B2 w - - 0 1", "KB v KB (same colour)"), ("8/8/8/4k3/8/4K3/8/5NN1 w - - 0 1", "KNNvK")):
    seg, bm = e.go(fen, "go depth 10"); sc = score_of(seg); check("insufficient material %s: draw score" % name, sc is not None and sc[0] == "cp" and abs(sc[1]) <= 15, sc)
# S4: searchmoves honoured
for sm in ("e2e4 d2d4", "a2a3", "g1f3 b1c3 h2h3"):
    seg, bm = e.go(chess.STARTING_FEN, "go depth 6 searchmoves " + sm); check("searchmoves " + sm, bm and bm.split()[1] in sm.split(), bm)
# S5: go mate
seg, bm = e.go("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", "go mate 1"); check("go mate 1: Rd8#", bm and bm.split()[1] == "d1d8", bm)
seg, bm = e.go("r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4", "go mate 1"); check("go mate 1: Qxf7#", bm and bm.split()[1] == "h5f7", bm)
t0 = time.time(); seg, bm = e.go("2bqkbn1/2pppp2/np2N3/r3P1p1/p2N2B1/5Q2/PPPPKPP1/RNB2r2 w KQkq - 0 1", "go mate 3", timeout=30); check("go mate 3 (known mate in 3 w/ Qxf7 line): stops with a mate score", score_of(seg) is not None and score_of(seg)[0] == "mate" and time.time() - t0 < 25, (score_of(seg), "%.1fs" % (time.time() - t0)))
e.close()

# S6: MultiPV
e = Eng(["setoption name MultiPV value 3"])
for fen in (chess.STARTING_FEN, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", "4k3/8/8/8/8/8/8/4K2R w K - 0 1", "7k/8/8/8/8/8/6P1/6K1 w - - 0 1"):
    seg, bm = e.go(fen, "go depth 9"); b = chess.Board(fen); nleg = b.legal_moves.count()
    last = {}
    for l in seg:
        m = re.search(r"info depth (\d+) .*?multipv (\d+) score (cp|mate) (-?\d+).* pv (.*)$", l)
        if m: last[(int(m.group(1)), int(m.group(2)))] = (m.group(3), int(m.group(4)), m.group(5).split())
    dmax = max([d for d, _ in last] or [0]); lines = sorted((k[1], v) for k, v in last.items() if k[0] == dmax)
    ok = len(lines) == min(3, nleg) and [i for i, _ in lines] == list(range(1, len(lines) + 1))
    legal = True; firsts = set()
    for _, (t, s, pv) in lines:
        bb = chess.Board(fen)
        try:
            for mv in pv: bb.push_uci(mv)
        except Exception: legal = False
        firsts.add(pv[0])
    vals = [(1000000 - abs(s)) * (1 if s > 0 else -1) if t == "mate" else s for _, (t, s, pv) in lines]
    check("MultiPV 3 on %s: %d lines, legal PVs, distinct first moves, sorted" % (fen[:24], len(lines)), ok and legal and len(firsts) == len(lines) and vals == sorted(vals, reverse=True), (len(lines), nleg, vals))
e.close()

# S7: info lines sanity (WDL, depth monotonic, hashfull, seldepth)
e = Eng(["setoption name UCI_ShowWDL value true"])
seg, bm = e.go("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "go depth 12"); bad = []; prev = 0
for l in seg:
    if not l.startswith("info depth"): continue
    d = int(re.search(r"depth (\d+)", l).group(1)); sd = re.search(r"seldepth (\d+)", l); w = re.search(r"wdl (\d+) (\d+) (\d+)", l); h = re.search(r"hashfull (\d+)", l); n = re.search(r"nodes (\d+)", l); sc = re.search(r"score cp (-?\d+)", l)
    if d < prev: bad.append("depth went down: " + l[:80])
    prev = d
    if sd and int(sd.group(1)) < d: bad.append("seldepth<depth: " + l[:80])
    if w and sum(map(int, w.groups())) != 1000: bad.append("wdl sum: " + l[:100])
    if h and not 0 <= int(h.group(1)) <= 1000: bad.append("hashfull: " + l[:80])
    if sc and abs(int(sc.group(1))) > 31000: bad.append("cp too large: " + l[:80])
check("info lines: depth monotonic, seldepth>=depth, WDL sums to 1000, hashfull in range", not bad, bad[:2])
e.close()

# S8: ponder flow
e = Eng(["setoption name Ponder value true"])
e.send("position startpos moves e2e4 e7e5"); with_lock = len(e.lines); e.send("go ponder wtime 20000 btime 20000 winc 100 binc 100"); time.sleep(1.5)
early = any(l.startswith("bestmove") for l in e.lines[with_lock:]); check("ponder: no bestmove before ponderhit / stop", not early, e.lines[-1:])
e.send("ponderhit"); i = e.wait("bestmove", 15, with_lock); check("ponder: bestmove after ponderhit", i >= 0, e.lines[-1:])
with_lock = len(e.lines); e.send("position startpos moves e2e4 e7e5 g1f3"); e.send("go ponder wtime 20000 btime 20000"); time.sleep(0.5); e.send("stop"); i = e.wait("bestmove", 10, with_lock); check("ponder: stop gives a bestmove", i >= 0, e.lines[-1:])
e.close()

# S9: limited strength
e = Eng(["setoption name UCI_LimitStrength value true", "setoption name UCI_Elo value 1320"])
t0 = time.time(); seg, bm = e.go(chess.STARTING_FEN, "go wtime 5000 btime 5000"); b = chess.Board(); mv = bm.split()[1] if bm else ""
check("UCI_LimitStrength 1320: legal bestmove within the clock", bm is not None and chess.Move.from_uci(mv) in b.legal_moves and time.time() - t0 < 4, (bm, "%.1fs" % (time.time() - t0)))
e.close()

# S10: promotions and en passant at the root
e = Eng()
seg, bm = e.go("8/P6k/8/8/8/8/8/K7 w - - 0 1", "go depth 6"); check("promotion to queen when winning", bm and bm.split()[1] in ("a7a8q",), bm)
seg, bm = e.go("8/8/8/8/k7/8/p7/2K5 b - - 0 1", "go depth 8"); check("promotion with check/underpromotion options handled", bm and chess.Move.from_uci(bm.split()[1]) in chess.Board("8/8/8/8/k7/8/p7/2K5 b - - 0 1").legal_moves, bm)
seg, bm = e.go("8/8/8/8/k2pP3/8/8/4K3 b - e3 0 1", "go depth 6"); b = chess.Board("8/8/8/8/k2pP3/8/8/4K3 b - e3 0 1"); check("en passant position: legal bestmove", bm and chess.Move.from_uci(bm.split()[1]) in b.legal_moves, bm)
seg, bm = e.go("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "go depth 6"); b = chess.Board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"); check("castling position: legal bestmove", bm and chess.Move.from_uci(bm.split()[1]) in b.legal_moves, bm)
e.close()

print("\n%d checks, %d failed" % (len(res), sum(1 for _, ok in res if not ok)))
for n, ok in res:
    if not ok: print("  FAILED:", n)
