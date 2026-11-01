import subprocess, sys, threading, time
exe = sys.argv[1]
moves = " ".join(["g1f3", "g8f6", "f3g1", "f6g8"] * 1000)   # 4000 plies of shuffling: crosses the 3900-ply guard
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
out = []
threading.Thread(target=lambda: [out.append(l.strip()) for l in p.stdout], daemon=True).start()
p.stdin.write("uci\nsetoption name OwnBook value false\nsetoption name Experience value false\nisready\nposition startpos moves " + moves + "\ngo depth 6\n"); p.stdin.flush()
t0 = time.time()
while time.time() - t0 < 15 and not any(l.startswith("bestmove") for l in out) and p.poll() is None: time.sleep(0.05)
print(exe.split("/")[-1], "| alive:", p.poll() is None, "| exit:", p.poll(), "| bestmove:", [l for l in out if l.startswith("bestmove")])
if p.poll() is None: p.kill()
