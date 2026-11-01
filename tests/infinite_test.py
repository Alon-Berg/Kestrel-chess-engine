import subprocess, sys, time, threading
exe = sys.argv[1]
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
lines = []
def reader():
    for line in p.stdout: lines.append((time.time(), line.strip()))
threading.Thread(target=reader, daemon=True).start()
p.stdin.write("uci\nsetoption name OwnBook value false\nsetoption name Experience value false\nisready\nposition fen k7/8/1K6/8/8/8/8/3Q4 w - - 0 1\ngo infinite\n"); p.stdin.flush()
time.sleep(2.0)
early = [l for t, l in lines if l.startswith("bestmove")]
p.stdin.write("stop\n"); p.stdin.flush(); time.sleep(0.5)
late = [l for t, l in lines if l.startswith("bestmove")]
print("bestmove printed BEFORE stop:", bool(early), "| after stop:", late)
p.stdin.write("quit\n"); p.stdin.flush(); time.sleep(0.1); p.kill()
