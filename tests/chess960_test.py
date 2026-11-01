"""Chess960 test against python-chess.   usage: python chess960_test.py engine.exe [games=60] [seed=1]
1. legal moves: random walks from random Chess960 start positions (X-FEN and Shredder-FEN alternately); at every position the engine's legal moves (go perft 1) must equal
   python-chess's (castling written king-takes-rook).  2. perft totals (depth 3 / 4) of Chess960 positions.  3. standard chess is unchanged: random walks from the normal start
   position with UCI_Chess960 off must match python-chess (castling written e1g1 ...).  4. the engine plays a legal move (including castling when it chooses it)."""
import random, re, subprocess, sys, threading, time
import chess
exe = sys.argv[1]; GAMES = int(sys.argv[2]) if len(sys.argv) > 2 else 60; SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rnd = random.Random(SEED)
results = []
def check(name, ok, detail=""):
    results.append(bool(ok)); print(("PASS " if ok else "FAIL ") + name + ("   " + str(detail) if detail != "" else ""), flush=True)

class Eng:
    def __init__(self):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.out = []
        threading.Thread(target=lambda: [self.out.append(l.rstrip()) for l in self.p.stdout], daemon=True).start()
        for c in ["uci", "setoption name OwnBook value false", "setoption name Experience value false", "isready"]: self.send(c)
        self.wait(lambda: "readyok" in self.out, 10)
    def send(self, s): self.p.stdin.write(s + "\n"); self.p.stdin.flush()
    def wait(self, pred, t):
        end = time.time() + t
        while time.time() < end:
            if pred(): return True
            time.sleep(0.001)
        return pred()
    def perft(self, fen, depth, moves=""):
        self.send("position fen %s%s" % (fen, (" moves " + moves) if moves else "")); n = len(self.out); self.send("go perft %d" % depth)
        self.wait(lambda: any(l.startswith("Nodes searched") for l in self.out[n:]), 120)
        lines = self.out[n:]
        per = {l.split(":")[0]: int(l.split(":")[1]) for l in lines if re.match(r"^[a-h][1-8][a-h][1-8][qrbn]?: \d+$", l)}
        tot = next((int(l.split(":")[1]) for l in lines if l.startswith("Nodes searched")), None)
        return per, tot
    def close(self):
        try: self.send("quit")
        except Exception: pass
        self.wait(lambda: self.p.poll() is not None, 5)
        if self.p.poll() is None: self.p.kill()

def py_perft(b, d):
    if d == 0: return 1
    n = 0
    for m in b.legal_moves:
        if d == 1: n += 1
        else:
            b.push(m); n += py_perft(b, d - 1); b.pop()
    return n

# ---------------------------------------------------------------- 1. legal moves along random Chess960 games
e = Eng(); e.send("setoption name UCI_Chess960 value true")
positions = 0; castles = 0; bad = []
for g in range(GAMES):
    b = chess.Board.from_chess960_pos(rnd.randrange(960)); b.chess960 = True
    for ply in range(40):
        fen = b.fen() if (g + ply) % 2 == 0 else b.shredder_fen()
        per, tot = e.perft(fen, 1)
        mine = set(per); ref = set(m.uci() for m in b.legal_moves)
        positions += 1; castles += sum(1 for m in b.legal_moves if b.is_castling(m))
        if mine != ref: bad.append((fen, sorted(mine ^ ref)[:4])); break
        mv = [m for m in b.legal_moves]
        if not mv: break
        castling = [m for m in mv if b.is_castling(m)]
        b.push(rnd.choice(castling) if castling and rnd.random() < 0.5 else rnd.choice(mv))
        if b.is_game_over(): break
check("legal moves equal python-chess in %d Chess960 positions (%d castling moves among them)" % (positions, castles), not bad and castles >= 50, bad[:2])
# ---------------------------------------------------------------- 2. perft
ok = True; n = 0; detail = ""
for i in range(40):
    b = chess.Board.from_chess960_pos(rnd.randrange(960)); b.chess960 = True
    d = 4 if i < 6 else 3
    per, tot = e.perft(b.shredder_fen() if i % 2 else b.fen(), d)
    ref = py_perft(b, d); n += 1
    if tot != ref: ok = False; detail = "%s depth %d engine %s python-chess %d" % (b.fen(), d, tot, ref); break
check("perft totals equal python-chess for %d Chess960 start positions (depth 3, six at depth 4)" % n, ok, detail)
# a middle-game position with castling rights on both sides, after some moves
b = chess.Board.from_chess960_pos(518); b.chess960 = True      # 518 = the normal start position
for u in ("e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "g8f6"): b.push_uci(u)
per, tot = e.perft(b.fen(), 3)
check("normal start position played as Chess960 (king-takes-rook notation, castling e1h1 possible): perft 3", tot == py_perft(b, 3) and "e1h1" in e.perft(b.fen(), 1)[0], tot)
# ---------------------------------------------------------------- 3. the engine plays
plays = 0; legal = True
e.close()
# play real moves: start from a fixed Chess960 position, let the engine choose, apply with python-chess
e = Eng(); e.send("setoption name UCI_Chess960 value true")
for g in range(6):
    base = chess.Board.from_chess960_pos(rnd.randrange(960)); base.chess960 = True; b = base.copy(); moves = []
    for ply in range(30):
        e.send("position fen %s%s" % (base.shredder_fen(), (" moves " + " ".join(moves)) if moves else "")); n0 = len(e.out); e.send("go depth 4")
        e.wait(lambda: any(l.startswith("bestmove") for l in e.out[n0:]), 30)
        bm = next((l.split()[1] for l in e.out[n0:] if l.startswith("bestmove")), None)
        if bm is None or bm == "0000": break
        try: mv = chess.Move.from_uci(bm)
        except Exception: legal = False; break
        if mv not in b.legal_moves: legal = False; print("illegal engine move", bm, b.fen()); break
        if b.is_castling(mv): plays += 1
        b.push(mv); moves.append(bm)
        if b.is_game_over(): break
check("the engine's own moves are legal in 6 short Chess960 games (castling moves played: %d)" % plays, legal)
e.close()
# ---------------------------------------------------------------- 4. standard chess is unchanged
e = Eng()
positions = 0; castles = 0; bad = []; seen = set()
for g in range(40):
    b = chess.Board()
    for ply in range(60):
        per, tot = e.perft(b.fen(), 1)
        mine = set(per); ref = set(m.uci() for m in b.legal_moves)
        positions += 1; castles += sum(1 for m in b.legal_moves if b.is_castling(m)); seen |= set(m.uci() for m in b.legal_moves if b.is_castling(m))
        if mine != ref: bad.append((b.fen(), sorted(mine ^ ref)[:4])); break
        mv = list(b.legal_moves)
        if not mv: break
        castling = [m for m in mv if b.is_castling(m)]
        b.push(rnd.choice(castling) if castling and rnd.random() < 0.6 else rnd.choice(mv))
        if b.is_game_over(): break
check("standard chess (UCI_Chess960 off): legal moves equal python-chess in %d positions; castling e1g1 e1c1 e8g8 e8c8 all seen (%d castling moves)" % (positions, castles), not bad and {"e1g1", "e1c1", "e8g8", "e8c8"} <= seen, bad[:2] or sorted(seen))
e.close()
print("chess960_test: %d checks, %d failed" % (len(results), results.count(False)))
sys.exit(0 if all(results) else 1)
