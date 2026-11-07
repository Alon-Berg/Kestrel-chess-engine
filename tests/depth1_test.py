r"""Short searches with many threads must still search: `go movetime M` (default 60 ms) with Threads T (default 128) in a few positions; every search must print at least one `info depth` line before its bestmove,
and in the stalemate trap `6Bk/8/8/K7/8/8/3R4/8 w` (a won position where Rg2 stalemates) the move must not be d2g2.
Why: the helper threads are started first (6 - 14 ms for 127 of them on a quiet computer with enough processors, 35 - 100 ms and more when there are fewer processors than threads or the computer is busy), and the first
time check of a thread runs at node 0, so in Kestrel 3.0 and in the first 3.1 candidate a search whose hard limit had passed by then ended without any result and the best-ordered legal move was played (39 of 60 searches with 128 threads
and `go movetime 60` on a busy 22-processor laptop, 8 of 60 with 64 threads). Since 3.1 a launcher thread starts the helpers so that the main search begins at once, and a time limit cannot end a search before depth 1 has completed.
usage: python depth1_test.py <exe> [threads=128] [trials=40] [movetime=60]
The last line is `depth1_test: PASS` or `depth1_test: FAIL (n of m searches ended without a result, k stalemating moves)`; the exit code is 0 either way, like the other scripts of this folder."""
import queue, subprocess, sys, threading, time

exe = sys.argv[1]; T = int(sys.argv[2]) if len(sys.argv) > 2 else 128; trials = int(sys.argv[3]) if len(sys.argv) > 3 else 40; mt = int(sys.argv[4]) if len(sys.argv) > 4 else 60
FENS = ["6Bk/8/8/K7/8/8/3R4/8 w - - 0 1", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"]
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
q = queue.Queue()


def pump():
    for line in p.stdout: q.put(line.rstrip("\r\n"))
    q.put(None)


threading.Thread(target=pump, daemon=True).start()


def send(s):
    p.stdin.write(s + "\n"); p.stdin.flush()


def until(prefix, timeout=30):
    """the lines up to and including the first one that starts with prefix; None on timeout or when the engine exited"""
    out = []; end = time.time() + timeout
    while time.time() < end:
        try: l = q.get(timeout=max(0.01, end - time.time()))
        except queue.Empty: return None
        if l is None: return None
        out.append(l)
        if l.startswith(prefix): return out
    return None


send("uci"); assert until("uciok"), "no uciok"
send("setoption name Threads value %d" % T); send("setoption name Hash value 64"); send("setoption name Experience value false"); send("setoption name OwnBook value false"); send("isready"); assert until("readyok"), "no readyok"
empty = 0; stalemating = 0; bad = 0
for i in range(trials):
    fen = FENS[i % len(FENS)]
    send("ucinewgame"); send("isready"); assert until("readyok"), "no readyok"
    send("position fen " + fen); send("go movetime %d" % mt)
    lines = until("bestmove", 20)
    if lines is None: bad += 1; print("trial %d: no bestmove within 20 s" % i); break
    if not any(l.startswith("info depth") for l in lines[:-1]): empty += 1
    mv = lines[-1].split()[1] if len(lines[-1].split()) > 1 else "?"
    if mv in ("0000", "?", "(none)"): bad += 1
    if i % len(FENS) == 0 and mv == "d2g2": stalemating += 1
send("quit")
try: p.wait(10)
except subprocess.TimeoutExpired: p.kill()
print("Threads %d, go movetime %d, %d searches: %d ended without a result, %d stalemating moves, %d without a bestmove" % (T, mt, trials, empty, stalemating, bad))
print("depth1_test: PASS" if empty == 0 and stalemating == 0 and bad == 0 else "depth1_test: FAIL (%d of %d searches ended without a result, %d stalemating moves, %d without a bestmove)" % (empty, trials, stalemating, bad))
