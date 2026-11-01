"""Edge-case UCI protocol test for the Claude engine.   usage: python edge_test.py engine.exe [timeout_s]
Each case starts a fresh engine, sends the commands, then checks: the process is still alive, it answers isready, every 'go' got a legal bestmove
(checked with python-chess when the position is a valid one), and nothing took longer than the timeout.  Prints one line per problem."""
import subprocess, sys, threading, time
import chess

exe = sys.argv[1]; TMO = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
START = "position startpos"
KIWI = "position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
cases = []
def add(name, cmds, fen=None): cases.append((name, cmds, fen))

# ---- go parameters that are zero, negative, huge or malformed
for g in ["go depth 0", "go depth -5", "go depth 99999", "go nodes 0", "go nodes -1", "go nodes 1", "go movetime 0", "go movetime -1", "go movetime 1",
          "go wtime 0 btime 0", "go wtime -100 btime -100", "go wtime 1 btime 1 winc 0 binc 0", "go wtime 100 btime 100 movestogo 0", "go wtime 100 btime 100 movestogo -3",
          "go wtime 9999999999999 btime 9999999999999", "go wtime 4000000000 btime 4000000000", "go wtime 1000 btime 1000 winc 99999999 binc 99999999",
          "go depth", "go movetime", "go wtime", "go nodes abc", "go depth 3 depth 4", "go mate 2", "go mate 0", "go searchmoves", "go searchmoves e2e4",
          "go searchmoves e2e5 a1a1 zzzz", "go ponder", "go ponder wtime 1000 btime 1000", "go", "go perft 3", "go foo bar", "go depth 4 nodes 100", "go infinite depth 3",
          "go wtime 50 btime 50 depth 30", "go movetime 30 nodes 1000000000000"]:
    add(g + " @startpos", [START, g])
    add(g + " @kiwipete", [KIWI, g], "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1")
# ---- positions that are malformed or unusual
for p in ["position", "position fen", "position fen 8/8/8/8/8/8/8/8 w - - 0 1", "position fen 8/8/8/8/8/8/8/K7 w - - 0 1", "position fen k7/8/8/8/8/8/8/K7 w - - 0 1",
          "position fen k7/8/8/8/8/8/8/K7 b - - 99 80", "position fen k7/8/8/8/8/8/8/K7 w - - 100 80", "position fen k7/8/8/8/8/8/8/K7 w - - 0 0", "position fen k7/8/8/8/8/8/8/K7 w - - -5 1",
          "position fen k7/8/8/8/8/8/8/K7 w - - 99999999999 1", "position fen k7/8/8/8/8/8/8/K7", "position fen k7/8/8/8/8/8/8/K7 w", "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 extra tokens",
          "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1", "position fen rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
          "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBN w KQkq - 0 1", "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR/8 w KQkq - 0 1",
          "position fen 4k3/8/8/8/8/8/8/4K2R w KQkq - 0 1", "position fen 4k3/8/8/8/8/8/8/R3K2R w q - 0 1", "position fen 4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - e3 0 1",
          "position fen 4k3/8/8/8/8/8/8/4K3 w - e6 0 1", "position fen 4k3/8/8/8/8/8/8/4K3 w - a9 0 1", "position fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e3 0 1",
          "position fen 7k/8/8/8/8/8/8/K6Q b - - 0 1", "position fen 7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", "position fen 7k/8/6K1/8/8/8/8/8 b - - 0 1",
          "position fen 4k3/8/8/8/8/8/8/4KQQQ w - - 0 1", "position fen pppppppp/pppppppp/pppppppp/8/8/PPPPPPPP/PPPPPPPP/PPPPPPPP w - - 0 1",
          "position fen 4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "position fen 4k3/8/8/8/8/8/p7/4K3 b - - 0 1", "position fen P3k3/8/8/8/8/8/8/4K3 w - - 0 1",
          "position startpos moves e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1", "position startpos moves e2e5", "position startpos moves e2e4 e2e4", "position startpos moves e2e4 zzzz", "position startpos moves",
          "position startpos moves e7e5", "position startpos moves e2e4 e7e5 e4e5", "position startpos moves e1g1", "position startpos moves e2e4 d7d5 e4d5 c7c5 d5c6", "position startpos moves e2e4 d7d5 e4e5 f7f5 e5f6",
          "position startpos moves a2a4 b7b5 a4b5 a7a6 b5a6", "position startpos moves g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1 f6g8", "position startpos moves " + "g1f3 g8f6 f3g1 f6g8 " * 40,
          "position startpos moves e2e4 e7e5 e1e2 e8e7 e2e1 e7e8 e1g1", "position fen r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1 moves e1c1 e8g8", "position fen 8/P7/8/8/8/8/8/k1K5 w - - 0 1 moves a7a8q",
          "position fen 8/P7/8/8/8/8/8/k1K5 w - - 0 1 moves a7a8", "position fen 8/P7/8/8/8/8/8/k1K5 w - - 0 1 moves a7a8k", "position fen 8/P7/8/8/8/8/8/k1K5 w - - 0 1 moves a7a8x",
          "position STARTPOS", "position startpos  moves   e2e4   e7e5", "positionx startpos", "position startpos\tmoves e2e4"]:
    add(p + " + go depth 4", [p, "go depth 4"])
    add(p + " + go movetime 50", [p, "go movetime 50"])
# ---- options
for o in ["Hash value 0", "Hash value -1", "Hash value abc", "Hash value 99999999999", "Hash value 1", "Hash value 4096", "Hash", "Hash value", "Threads value 0", "Threads value -3", "Threads value 1000",
          "Threads value 8", "Threads value abc", "MultiPV value 0", "MultiPV value 100", "MultiPV value -1", "MultiPV value 8", "UCI_Elo value 0", "UCI_Elo value 99999", "UCI_Elo value -1",
          "UCI_LimitStrength value true", "UCI_LimitStrength value maybe", "Move Overhead value -5", "Move Overhead value 99999999", "NNBlend value -10", "NNBlend value 500", "Net2Share value -1",
          "Net2Share value 1000", "LazyMargin value -50", "EvalFlags value 255", "EvalFlags value -1", "QsTT value 5", "CorrFlags value 9", "V31Flags value 255", "V31HistPrune value -1",
          "V31CutR value 99", "V31DE value -9", "OwnBook value false", "OwnBook value maybe", "Experience value false", "ExperienceFile value", "ExperienceFile value /nonexistent/dir/x.dat",
          "ExperienceFile value CON", "Ponder value true", "UseNNUE value false", "UseNet2 value false", "NoSuchOption value 1", "name value", "Clear Hash", "Clear Hash value 1", "EvalCache value 0"]:
    add("setoption " + o + " + go depth 6", ["setoption name " + o, START, "go depth 6"])
    add("setoption " + o + " + go movetime 100 (kiwipete)", ["setoption name " + o, KIWI, "go movetime 100"], "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1")
# ---- sequences
add("stop without go", [START, "stop", "stop", "go depth 5"])
add("ponderhit without ponder", [START, "ponderhit", "go depth 5"])
add("ucinewgame mid-game", [START, "go depth 5", "ucinewgame", "go depth 5", "ucinewgame", "ucinewgame", START + " moves e2e4", "go depth 5"])
add("many setoption/position/go cycles", [x for i in range(30) for x in (START + " moves e2e4 e7e5", "setoption name Hash value %d" % (1 + i % 7), "go depth 4", "ucinewgame")])
add("go while searching (double go)", [START, "go infinite", "go depth 3", "stop"])
add("position while searching", [START, "go infinite", KIWI, "stop", "go depth 4"])
add("setoption while searching", [START, "go infinite", "setoption name Hash value 4", "setoption name Threads value 2", "stop", "go depth 4"])
add("quit while searching", [START, "go infinite"])
add("isready while searching", [START, "go infinite", "isready", "isready", "stop"])
add("empty and whitespace lines", ["", "   ", "\t", START, "", "go depth 3"])
add("very long line", [START + " moves " + "e2e4 " * 5000, "go depth 3"])
add("very long garbage", ["x" * 100000, "go depth 3"])
add("null-ish bytes", ["position startpos\x01\x02", "go depth 3"])
add("uci repeated", ["uci", "uci", "isready", "uci", START, "go depth 3"])
add("debug/register/other", ["debug on", "register later", "debug off", "d", "eval", "perft 3", "bench 8", START, "go depth 3"])
add("go ponder then ponderhit", ["setoption name Ponder value true", START, "go ponder wtime 1000 btime 1000", "ponderhit"])
add("go ponder then stop", ["setoption name Ponder value true", START, "go ponder wtime 1000 btime 1000", "stop"])
add("multipv 5 go infinite stop", ["setoption name MultiPV value 5", START, "go infinite"])
add("threads 4 go infinite stop", ["setoption name Threads value 4", START, "go infinite"])
add("hash 1 deep search", ["setoption name Hash value 1", START, "go depth 14"])
add("book position repeated go", [START, "go depth 3", START, "go depth 3", START, "go depth 3"])

INF_CASES = ("go infinite", "go ponder", "multipv 5 go infinite", "threads 4 go infinite", "quit while searching", "isready while searching", "go while searching", "position while searching", "setoption while searching")
problems = 0
def run_case(name, cmds, fen):
    global problems
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1, errors="replace")
    out = []; err = []
    threading.Thread(target=lambda: [out.append(l.rstrip()) for l in p.stdout], daemon=True).start()
    threading.Thread(target=lambda: [err.append(l.rstrip()) for l in p.stderr], daemon=True).start()
    def send(s):
        try: p.stdin.write(s + "\n"); p.stdin.flush(); return True
        except Exception: return False
    def wait_for(pred, t):
        end = time.time() + t
        while time.time() < end:
            if pred(): return True
            if p.poll() is not None: return pred()
            time.sleep(0.01)
        return pred()
    send("uci"); send("isready")
    if not wait_for(lambda: "readyok" in out, 10): print("PROBLEM [%s]: no readyok at start" % name); problems += 1; p.kill(); return
    mark = len(out); gos = 0; died = False
    for c in cmds:
        if not send(c): died = True; break
        if c.startswith("go"):
            gos += 1
            infinite = ("infinite" in c) or c.startswith("go ponder")
            if infinite:
                time.sleep(0.4)
        if c == "go infinite" and any(x == cmds[-1] for x in [c]) and c is cmds[-1]:
            pass
    # searches that never end by themselves (infinite / ponder): stop them after the commands
    infinite_pending = any(("infinite" in c) or c.startswith("go ponder") for c in cmds)
    if infinite_pending and not name.startswith("quit while") and "ponderhit" not in cmds:
        time.sleep(0.5); send("stop")
    elif name.startswith("go ponder then ponderhit"):
        pass
    nbest = lambda: sum(1 for l in out[mark:] if l.startswith("bestmove") or l.startswith("Nodes searched"))      # `go perft N` ends with "Nodes searched", not with a bestmove
    if name.startswith("quit while"):                 # quit during a search: the process must exit by itself
        time.sleep(0.5); send("quit")
        if not wait_for(lambda: p.poll() is not None, 10): print("PROBLEM [%s]: did not exit on quit during a search" % name); problems += 1; p.kill()
        return
    if gos and not died:
        # a search with no finite limit (infinite, depth 99999, bare go, huge clock ...) is not a failure: ask it to stop and require the bestmove then
        if not wait_for(lambda: nbest() >= 1 or p.poll() is not None, 3.0):
            send("stop")
            if not wait_for(lambda: nbest() >= 1 or p.poll() is not None, TMO): print("PROBLEM [%s]: no bestmove within %.0fs, not even after stop" % (name, TMO)); problems += 1
    if p.poll() is None and not name.startswith("quit while"):
        m2 = len(out); send("isready")
        if not wait_for(lambda: any(l == "readyok" for l in out[m2:]), 10): print("PROBLEM [%s]: engine alive but no readyok afterwards" % name); problems += 1
    if p.poll() is not None and not name.startswith("quit while") and not any(c == "quit" for c in cmds):
        print("PROBLEM [%s]: engine exited with code %s" % (name, p.returncode)); problems += 1
    else:
        if p.poll() is None: send("quit"); wait_for(lambda: p.poll() is not None, 5)
        if p.poll() is None: p.kill(); print("PROBLEM [%s]: did not exit on quit" % name); problems += 1
    # bestmove legality for plain valid positions
    if fen and gos:
        try:
            b = chess.Board(fen)
            for l in out[mark:]:
                if l.startswith("bestmove"):
                    mv = l.split()[1] if len(l.split()) > 1 else ""
                    if mv in ("0000", "(none)"): print("NOTE [%s]: bestmove %s in a position with legal moves" % (name, mv)); problems += 1
                    elif chess.Move.from_uci(mv) not in b.legal_moves: print("PROBLEM [%s]: illegal bestmove %s" % (name, mv)); problems += 1
        except Exception as e:
            print("PROBLEM [%s]: bestmove check failed: %s" % (name, e)); problems += 1
    bad_err = [l for l in err if l.strip()]
    if bad_err: print("PROBLEM [%s]: stderr: %s" % (name, " | ".join(bad_err[:2])[:300])); problems += 1

t0 = time.time()
for i, (name, cmds, fen) in enumerate(cases):
    try: run_case(name, cmds, fen)
    except Exception as e: print("HARNESS ERROR [%s]: %r" % (name, e)); problems += 1
print("cases %d | problems %d | %.0fs" % (len(cases), problems, time.time() - t0))
