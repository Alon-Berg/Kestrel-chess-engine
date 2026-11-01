import subprocess, sys, time, threading
exe = sys.argv[1]; n = int(sys.argv[2])
hung = 0
for i in range(n):
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    p.stdin.write("uci\nsetoption name OwnBook value false\nsetoption name Experience value false\nisready\n"); p.stdin.flush()
    # go infinite immediately followed by stop (as fast as possible)
    p.stdin.write("position startpos\ngo infinite\nstop\n"); p.stdin.flush()
    got = {"bestmove": False}
    def reader():
        for line in p.stdout:
            if line.startswith("bestmove"): got["bestmove"] = True; break
    t = threading.Thread(target=reader, daemon=True); t.start(); t.join(5.0)
    if not got["bestmove"]: hung += 1
    try: p.stdin.write("quit\n"); p.stdin.flush()
    except Exception: pass
    time.sleep(0.05); p.kill()
print("runs", n, "hung (no bestmove within 5 s):", hung)
