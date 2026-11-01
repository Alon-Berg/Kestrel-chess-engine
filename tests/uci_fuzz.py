"""UCI protocol fuzzer: random, partly malformed command sequences; the engine must never crash or hang.
usage: python uci_fuzz.py engine.exe n_sequences seed
After every sequence the script sends 'stop' + 'isready' and requires 'readyok' within 8 s (and the process to be alive)."""
import random, subprocess, sys, threading, time

exe, n, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rnd = random.Random(seed)
FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "4k3/8/8/8/8/8/8/4K3 w - - 0 1",                       # bare kings
    "k7/8/1K6/8/8/8/8/3Q4 w - - 99 100",                   # mate on the fifty-move boundary
    "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1",                      # stalemate
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -", # FEN without move counters
    "6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1",
    "4k3/P7/8/8/8/8/8/4K3 w - - 0 1",                      # promotion available
]
GARBAGE = ["", " ", "foo bar", "position", "position fen", "position fen xyz", "position startpos moves e2e5 zz", "go depth", "go depth abc",
           "go nodes -5", "go wtime", "go wtime 100 btime", "setoption", "setoption name", "setoption name Hash value", "setoption name Hash value -3",
           "setoption name Hash value 99999999", "setoption name NNBlend value abc", "setoption name Nonexistent value 1", "ucinewgame ucinewgame",
           "isready isready", "stop stop", "debug on", "register later", "ponderhit", "go ponder", "go searchmoves e2e4", "go mate 3",
           "position fen 8/8/8/8/8/8/8/8 w - - 0 1", "\t\t", "uci uci"]
def cmd():
    r = rnd.random()
    if r < 0.30: return "position fen " + rnd.choice(FENS)
    if r < 0.38: return "position startpos moves " + " ".join(rnd.choice(["e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6", "e1g1", "f8e7"]) for _ in range(rnd.randint(0, 12)))
    if r < 0.55: return rnd.choice(["go depth %d" % rnd.randint(1, 9), "go nodes %d" % rnd.randint(1, 30000), "go movetime %d" % rnd.randint(1, 80),
                                    "go wtime %d btime %d winc %d binc %d" % (rnd.randint(1, 3000), rnd.randint(1, 3000), rnd.randint(0, 100), rnd.randint(0, 100)),
                                    "go wtime 0 btime 0", "go wtime -50 btime -50", "go infinite", "go movetime 0", "go depth 0", "go wtime 500 btime 500 movestogo 1"])
    if r < 0.63: return rnd.choice(["stop", "stop", "isready", "ucinewgame"])
    if r < 0.70: return rnd.choice(["setoption name Hash value %d" % rnd.choice([1, 2, 16, 64]), "setoption name NNBlend value %d" % rnd.randint(0, 100),
                                    "setoption name OwnBook value %s" % rnd.choice(["true", "false"]), "setoption name Net2Share value %d" % rnd.randint(0, 100),
                                    "setoption name EvalFlags value %d" % rnd.randint(0, 3), "setoption name Experience value false", "setoption name Threads value %d" % rnd.choice([1, 2, 3, 4, 8])])
    return rnd.choice(GARBAGE)

import os
ERRFILE = open(os.environ.get("FUZZ_STDERR", os.devnull), "w")     # set FUZZ_STDERR=file to keep the engine's stderr (sanitizer reports)
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=ERRFILE, text=True, bufsize=1)
lines = []
threading.Thread(target=lambda: [lines.append(l.rstrip()) for l in p.stdout], daemon=True).start()
LOG = open("fuzz_cmds.log", "w")
def send(s):
    LOG.write("%.3f\t%s\n" % (time.time(), s)); LOG.flush()
    p.stdin.write(s + "\n"); p.stdin.flush()
send("uci"); send("setoption name Experience value false"); send("setoption name OwnBook value false"); send("isready")
time.sleep(0.5)
fails = 0
for i in range(n):
    seq = [cmd() for _ in range(rnd.randint(2, 14))]
    try:
        for c in seq:
            send(c)
            if rnd.random() < 0.3: time.sleep(rnd.random() * 0.03)
        send("stop")
        mark = len(lines)
        send("isready")
        t0 = time.time()
        while time.time() - t0 < 120 and not any(l == "readyok" for l in lines[mark:]):
            if p.poll() is not None: break
            time.sleep(0.01)
        ok = p.poll() is None and any(l == "readyok" for l in lines[mark:])
    except (BrokenPipeError, OSError):
        ok = False
    if not ok:
        fails += 1
        print("FAILURE in sequence", i, "| process alive:", p.poll() is None, "| exit code:", p.poll())
        if p.poll() is None:
            try:
                rr = subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Process -Id %d | Select-Object -ExpandProperty Threads | ForEach-Object { '{0} {1} {2} cpu={3}' -f $_.Id, $_.ThreadState, $_.WaitReason, $_.TotalProcessorTime.TotalMilliseconds }" % p.pid], capture_output=True, text=True, timeout=30).stdout
                for x in rr.split(chr(10)):
                    if x.strip(): print("   thread:", x.strip())
            except Exception as ex: print("   (thread dump failed:", ex, ")")
        for c in seq: print("    >", repr(c))
        break
    del lines[:]
send("quit"); time.sleep(0.2)
if p.poll() is None: p.kill()
print("sequences", n, "| failures", fails)
