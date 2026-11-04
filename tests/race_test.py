import subprocess, sys, time, threading
exe = sys.argv[1]; n = int(sys.argv[2])
hung = 0; slowest_start = 0.0
def reader(p, ev):
    for line in p.stdout:
        if line.startswith("readyok"): ev["readyok"].set()
        elif line.startswith("bestmove"): ev["bestmove"].set(); break
for i in range(n):
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    ev = {"readyok": threading.Event(), "bestmove": threading.Event()}
    threading.Thread(target=reader, args=(p, ev), daemon=True).start()
    t0 = time.time()
    p.stdin.write("uci\nsetoption name OwnBook value false\nsetoption name Experience value false\nisready\n"); p.stdin.flush()
    # starting an 80 MB program on a busy machine (or behind a virus scanner) can take several seconds and is not what is tested here:
    # wait for readyok (up to 60 s) and time only the race below
    if ev["readyok"].wait(60.0):
        slowest_start = max(slowest_start, time.time() - t0)
        # go infinite immediately followed by stop (as fast as possible)
        p.stdin.write("position startpos\ngo infinite\nstop\n"); p.stdin.flush()
        if not ev["bestmove"].wait(5.0): hung += 1
    else: hung += 1
    try: p.stdin.write("quit\n"); p.stdin.flush()
    except Exception: pass
    time.sleep(0.05); p.kill()
print("runs", n, "hung (no bestmove within 5 s):", hung, "| slowest start %.1f s" % slowest_start)
