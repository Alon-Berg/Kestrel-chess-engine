"""Thread start/stop stress test: random go/stop/position/ucinewgame/setoption bursts with millisecond gaps.
usage: python stress_stop.py engine.exe [iterations] [seed] [threads]
Checks: (1) the engine always answers isready (hang detection - on a hang the state of every OS thread is printed),
        (2) every 'go' produces exactly one 'bestmove' (counted after a final stop)."""
import random, subprocess, sys, threading, time
exe = sys.argv[1]
iters = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
th = sys.argv[4] if len(sys.argv) > 4 else "4"
rnd = random.Random(seed)
FENS = ["rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", "4k3/8/8/8/8/8/8/4K3 w - - 0 1"]
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
lines = []
threading.Thread(target=lambda: [lines.append(l.rstrip()) for l in p.stdout], daemon=True).start()
def send(s): p.stdin.write(s + "\n"); p.stdin.flush()
send("uci"); send("setoption name Experience value false"); send("setoption name OwnBook value false"); send("setoption name Threads value " + th); send("isready")
time.sleep(0.5)
gos = 0
def threads_report():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
        "Get-Process -Id %d | Select-Object -ExpandProperty Threads | ForEach-Object { '{0} {1} {2}' -f $_.Id, $_.ThreadState, $_.WaitReason }" % p.pid],
        capture_output=True, text=True).stdout.split("\n")
    return [x.strip() for x in r if x.strip()]
def sync(tag):
    mark = len(lines); send("stop"); send("isready"); t0 = time.time()
    while time.time() - t0 < 15 and not any(l == "readyok" for l in lines[mark:]): time.sleep(0.005)
    if not any(l == "readyok" for l in lines[mark:]):
        print("HANG after", tag, "| process alive:", p.poll() is None, "| iteration", i)
        for x in threads_report(): print("   thread:", x)
        return False
    return True
ok = True
for i in range(iters):
    c = rnd.random()
    try:
        if c < 0.30: send(rnd.choice(["go depth %d" % rnd.randint(1, 6), "go movetime %d" % rnd.randint(1, 25), "go wtime 60 btime 60", "go nodes %d" % rnd.randint(1, 3000)])); gos += 1
        elif c < 0.38: send("go infinite"); gos += 1
        elif c < 0.55: send("stop")
        elif c < 0.70: send("position startpos moves " + " ".join(rnd.choice(["e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6"]) for _ in range(rnd.randint(0, 6))))
        elif c < 0.78: send("position fen " + rnd.choice(FENS))
        elif c < 0.84: send("ucinewgame")
        elif c < 0.88: send("setoption name Threads value %d" % rnd.choice([1, 2, 3, 4, 6, 8]))
        else: send("isready")
    except OSError:
        print("engine died at iteration", i); ok = False; break
    time.sleep(rnd.random() * 0.003)
    if i % 40 == 39 and not sync("iteration %d" % i): ok = False; break
if ok:
    mark = len(lines); send("stop"); send("isready")
    time.sleep(1.5)
    bm = len([l for l in lines if l.startswith("bestmove")])
    print("iterations %d | go commands %d | bestmoves %d | %s" % (iters, gos, bm, "OK" if bm == gos else "MISMATCH (a go produced no or several bestmoves)"))
    ok = bm == gos
try: send("quit")
except OSError: pass
time.sleep(0.3)
if p.poll() is None: p.kill()
sys.exit(0 if ok else 1)
