"""Option defaults test.   usage: python options_test.py engine.exe
A chess program may send the DECLARED default of every option (for example after "reset to defaults"). That must not change the engine: the search with all declared defaults sent has to be
identical (best move, depth, score, nodes, pv) to the search with no option sent at all.  Kestrel 2.0 and 2.1 fail this test: they declared NNBlend with default 70 but ran at 100.
Prints the differences and a last line  'options_test: N options, M positions, K differences'."""
import os, re, subprocess, sys, time
exe = sys.argv[1]; cwd = os.path.dirname(os.path.abspath(exe))
POS = ["startpos moves e2e4 e7e5 g1f3", "fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "fen r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R1BQ1RK1 w - - 0 8",
       "fen 8/5pk1/6p1/8/8/6P1/5PK1/8 w - - 0 1", "fen rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq - 0 6"]

class Eng:
    def __init__(self):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd, text=True, bufsize=1)
    def send(self, s): self.p.stdin.write(s + "\n"); self.p.stdin.flush()
    def until(self, key, timeout=120):
        out = []; t0 = time.time()
        while time.time() - t0 < timeout:
            l = self.p.stdout.readline()
            if not l: break
            out.append(l.rstrip())
            if l.startswith(key): break
        return out
    def close(self):
        try: self.send("quit"); self.p.wait(10)
        except Exception: self.p.kill()

def declared():
    e = Eng(); e.send("uci"); lines = e.until("uciok"); e.close(); opts = []
    for l in lines:
        m = re.match(r"option name (.+?) type (\w+)(?: default (.*?))?(?: min .*)?$", l)
        if m and m.group(2) in ("spin", "check", "string") and m.group(3) is not None: opts.append((m.group(1), m.group(2), m.group(3).strip()))
    return opts

def search_all(opts):
    e = Eng(); e.send("uci"); e.until("uciok")
    for name, typ, dflt in opts: e.send("setoption name %s value %s" % (name, dflt))
    e.send("isready"); e.until("readyok"); res = []
    for f in POS:
        e.send("ucinewgame"); e.send("position " + f); e.send("go nodes 60000"); out = e.until("bestmove")
        info = [l for l in out if l.startswith("info depth") and " score " in l][-1:]
        m = re.search(r"depth (\d+).*? score (\S+ \S+).*? nodes (\d+)", info[0]) if info else None
        pv = re.search(r" pv (.*)$", info[0]).group(1) if info and " pv " in info[0] else ""
        res.append((m.groups() if m else None, out[-1], pv))
    e.close(); return res

opts = declared(); a = search_all([]); b = search_all(opts); diffs = 0
for f, x, y in zip(POS, a, b):
    if x != y: diffs += 1; print("DIFF", f[:60], "\n  nothing sent:", x, "\n  defaults sent:", y)
print("options_test: %d options, %d positions, %d differences" % (len(opts), len(POS), diffs))
