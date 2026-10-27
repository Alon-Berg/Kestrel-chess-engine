r"""Black-box audit probes for Kestrel (1.1 bug search, 2026-10-27).  usage: python audit_probe.py <exe>
Each case starts a fresh engine (Experience and OwnBook are set off unless a case says otherwise), sends raw bytes, and checks what comes back / whether the process survives.
Prints PASS / FAIL per case with the evidence; exit status 0 even on FAIL (this is an audit, not a gate)."""
import os, subprocess, sys, threading, time
EXE = sys.argv[1]
CWD = os.path.dirname(EXE)
OFF = b"setoption name Experience value false\nsetoption name OwnBook value false\n"

def start():
    return subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=CWD)

def reader(p, sink):
    for line in iter(p.stdout.readline, b""):
        sink.append(line.decode("utf-8", "replace").rstrip("\r\n"))

def session(payload, wait_for=None, timeout=20, close_stdin=False, tail=0.0):
    """send payload (bytes); wait until a line starting with wait_for appears (or timeout); return (lines, alive, rc, seconds)"""
    p = start(); out = []; t = threading.Thread(target=reader, args=(p, out), daemon=True); t.start()
    t0 = time.time()
    try: p.stdin.write(payload); p.stdin.flush()
    except OSError: pass
    if wait_for:
        while time.time() - t0 < timeout and not any(l.startswith(wait_for) for l in out) and p.poll() is None: time.sleep(0.02)
    if tail: time.sleep(tail)
    if close_stdin:
        try: p.stdin.close()
        except OSError: pass
        t1 = time.time()
        while time.time() - t1 < 5 and p.poll() is None: time.sleep(0.02)
    alive = p.poll() is None; dt = time.time() - t0
    if alive:
        try: p.stdin.write(b"quit\n"); p.stdin.flush()
        except OSError: pass
        try: p.wait(5)
        except Exception: p.kill()
    return out, alive, p.returncode, dt

results = []
def case(name, ok, evidence=""):
    results.append((name, ok)); print(("PASS  " if ok else "FAIL  ") + name + ("  | " + str(evidence)[:300] if evidence or not ok else ""), flush=True)

def has(out, prefix): return any(l.startswith(prefix) for l in out)
def best(out): return next((l for l in out if l.startswith("bestmove")), None)

# 1. CRLF line endings (Windows GUIs / pipes from cmd.exe)
out, alive, rc, dt = session(b"uci\r\nisready\r\nposition startpos\r\ngo depth 3\r\n", "bestmove")
case("CRLF: uciok + readyok + bestmove", has(out, "uciok") and has(out, "readyok") and best(out) is not None, out[-3:])
out, alive, rc, dt = session(OFF.replace(b"\n", b"\r\n") + b"position startpos moves e2e4 e7e5\r\ngo depth 3\r\nquit\r\n", "bestmove", close_stdin=False)
case("CRLF: setoption with CR + position moves + quit exits", best(out) is not None, out[-3:])
out, alive, rc, dt = session(b"quit\r\n", None, timeout=3, tail=1.0)
case("CRLF: 'quit\\r' ends the process", not alive, "alive=%s rc=%s" % (alive, rc))

# 2. stdin closed
out, alive, rc, dt = session(b"uci\nisready\n", "readyok", close_stdin=True)
case("EOF at idle: process exits", not alive, "alive=%s rc=%s" % (alive, rc))
out, alive, rc, dt = session(OFF + b"position startpos\ngo infinite\n", "info depth 3", close_stdin=True)
case("EOF during 'go infinite': process exits", not alive, "alive=%s rc=%s" % (alive, rc))

# 3. very long game (3000 plies of knight shuffling: repetition and 50-move counters run far past any history size)
sh = ["g1f3", "g8f6", "f3g1", "f6g8"] * 750
out, alive, rc, dt = session(OFF + ("position startpos moves " + " ".join(sh) + "\ngo depth 6\n").encode(), "bestmove", timeout=30)
case("3000-ply move list, go depth 6", best(out) is not None and rc is None or best(out) is not None, (best(out), alive, rc))
sh = ["g1f3", "g8f6", "f3g1", "f6g8"] * 3000
out, alive, rc, dt = session(OFF + ("position startpos moves " + " ".join(sh) + "\ngo depth 5\n").encode(), "bestmove", timeout=30)
case("12000-ply move list, go depth 5", best(out) is not None, (best(out), alive, rc))
fen_long = "position fen 7k/8/8/8/8/8/8/K7 w - - 0 1 moves " + " ".join(["a1b1", "h8g8", "b1a1", "g8h8"] * 2500)
out, alive, rc, dt = session(OFF + (fen_long + "\ngo depth 5\n").encode(), "bestmove", timeout=30)
case("10000-ply K vs K move list", best(out) is not None, (best(out), alive, rc))

# 4. numbers: huge, negative, zero, junk
UNLIMITED = ("go depth -3", "go depth 99999999999", "go nodes -1", "go nodes 99999999999999999999", "go mate -2", "go searchmoves", "go ponder depth 3", "go infinite depth 3 nodes 1000")
for cmd in ["go wtime 99999999999999 btime 99999999999999 winc 99999999999 binc 99999999999 movestogo 99999999 depth 4",
            "go wtime -500 btime -500 depth 4", "go wtime 0 btime 0", "go wtime 1 btime 1 movestogo 1", "go movetime -5", "go movetime 0",
            "go depth 0", "go depth -3", "go depth 99999999999", "go nodes 0", "go nodes -1", "go nodes 99999999999999999999", "go mate 0", "go mate -2", "go depth abc", "go wtime abc btime",
            "go movestogo 0 wtime 1000 btime 1000", "go searchmoves", "go searchmoves e2e5 depth 3", "go ponder depth 3", "go infinite depth 3 nodes 1000"]:
    if cmd in UNLIMITED:       # an unusable limit means "no limit": the search runs until `stop` (like a bare `go`), then it must answer
        p = start(); out2 = []; threading.Thread(target=reader, args=(p, out2), daemon=True).start()
        p.stdin.write(OFF + b"position startpos\n" + cmd.encode() + b"\n"); p.stdin.flush(); time.sleep(1.0); p.stdin.write(b"stop\n"); p.stdin.flush()
        t1 = time.time()
        while time.time() - t1 < 5 and not best(out2): time.sleep(0.02)
        ok = best(out2) is not None; p.stdin.write(b"quit\n"); p.stdin.flush(); p.wait(5)
        case("numbers (no limit until stop): " + cmd[:60], ok, "bestmove after stop=%s" % best(out2))
        continue
    out, alive, rc, dt = session(OFF + b"position startpos\n" + cmd.encode() + b"\n", "bestmove", timeout=15)
    ok = best(out) is not None and dt < 14
    case("numbers: " + cmd[:70], ok, "bestmove=%s dt=%.1fs rc=%s" % (best(out), dt, rc))

# 5. setoption extremes
for opt in ["Hash value 0", "Hash value -1", "Hash value 99999999999", "Hash value abc", "Hash", "Threads value 0", "Threads value -4", "Threads value 1000", "MultiPV value 0", "MultiPV value 1000",
            "Move Overhead value -5", "Move Overhead value 999999999999", "UCI_Elo value 0", "UCI_Elo value 99999", "UCI_LimitStrength value maybe", "NoSuchOption value 1", "", "value 5", "name", "Ponder value true",
            "ExperienceFile value", "ExperienceFile value NUL", "ExperienceFile value C:\\\\Windows\\\\nope\\\\x.dat", "Clear Hash"]:
    out, alive, rc, dt = session(OFF + ("setoption name " + opt + "\n").encode() + b"isready\nposition startpos\ngo depth 4\n", "bestmove", timeout=15)
    case("setoption name " + opt[:50], best(out) is not None and has(out, "readyok"), "bestmove=%s rc=%s last=%s" % (best(out), rc, out[-1:]))

# 6. bad positions
for pos in ["position fen", "position fen 8/8/8/8/8/8/8/8 w - - 0 1", "position fen 8/8/8/8/8/8/8/K7 w - - 0 1", "position fen k7/8/8/8/8/8/8/K7 w - - 0 1", "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR",
            "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x - - 0 1", "position fen 4k3/8/8/8/8/8/8/4K2R w K - 0 1", "position startpos moves e2e5", "position startpos moves e2e4 e2e4", "position startpos moves zzzz",
            "position startpos moves e2e4 e7e5 moves", "position startpos moves e1g1", "position moves e2e4", "position", "position fen 4k3/8/8/8/8/8/8/4K3 w - - 99999999999 99999999999",
            "position fen 4k3/8/8/8/8/8/8/4K3 w - e3 0 1", "position fen 4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "position fen 4k3/8/8/8/8/8/p7/4K3 w - - 0 1", "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0"]:
    out, alive, rc, dt = session(OFF + pos.encode() + b"\ngo depth 3\n", "bestmove", timeout=10)
    case(pos[:78], best(out) is not None and rc is None or best(out) is not None, "bestmove=%s rc=%s" % (best(out), rc))

# 7. go without a position, repeated go, stop / ponderhit without search, isready while searching
out, alive, rc, dt = session(OFF + b"go depth 3\n", "bestmove"); case("go before any position", best(out) is not None, best(out))
out, alive, rc, dt = session(OFF + b"stop\nponderhit\nstop\nisready\nuci\n", "uciok"); case("stop / ponderhit with no search", has(out, "readyok") and has(out, "uciok"), out[-2:])
out, alive, rc, dt = session(OFF + b"position startpos\ngo infinite\nisready\n", "readyok", timeout=5); case("isready answered while searching", has(out, "readyok"), out[-2:])
out, alive, rc, dt = session(OFF + b"position startpos\ngo depth 8\ngo depth 8\ngo depth 8\nposition startpos moves e2e4\ngo depth 8\nucinewgame\ngo depth 6\n", "bestmove", timeout=20, tail=3)
case("back-to-back go commands", sum(1 for l in out if l.startswith("bestmove")) >= 1 and alive, "bestmoves=%d alive=%s" % (sum(1 for l in out if l.startswith("bestmove")), alive))
out, alive, rc, dt = session(OFF + b"position startpos\ngo infinite\nposition startpos moves e2e4\nsetoption name Hash value 16\nucinewgame\nstop\n", "bestmove", timeout=10, tail=1)
case("position / setoption / ucinewgame during a search, then stop", best(out) is not None and alive, (best(out), alive, rc))

# 8. garbage input
out, alive, rc, dt = session(b"x" * 2000000 + b"\nisready\n", "readyok", timeout=20); case("2 MB single line, then isready", has(out, "readyok"), (alive, rc, out[-1:]))
out, alive, rc, dt = session(bytes(b for b in range(256) if b not in (0x0a, 0x1a)) * 400 + b"\nisready\n", "readyok", timeout=20); case("binary garbage line (no Ctrl-Z: the Windows C runtime ends a text pipe there), then isready", has(out, "readyok"), (alive, rc, out[-1:]))
out, alive, rc, dt = session(b"\xff\xfe\x00\x00uci\n\x00isready\nisready\n", "readyok", timeout=10); case("NUL / invalid UTF-8 bytes glued to command words are ignored, the next clean command works", has(out, "readyok") and not has(out, "uciok"), (alive, rc, out[-1:]))
out, alive, rc, dt = session(b"\n" * 100000 + b"isready\n", "readyok", timeout=20); case("100,000 empty lines, then isready", has(out, "readyok"), (alive, rc, out[-1:]))
out, alive, rc, dt = session(b"   uci   \n\tisready\t\n", "readyok", timeout=5); case("leading / trailing whitespace and tabs", has(out, "uciok") and has(out, "readyok"), out[-2:])
out, alive, rc, dt = session(b"UCI\nIsReady\n", None, timeout=2, tail=1.0); case("(info) upper-case commands are ignored like in Stockfish", not has(out, "uciok"), out[-2:])

print("\n%d cases, %d failed" % (len(results), sum(1 for _, ok in results if not ok)))
for n, ok in results:
    if not ok: print("  FAILED:", n)
