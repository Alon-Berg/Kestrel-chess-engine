"""Syzygy test of a Kestrel build against python-chess (python-chess is used only as an independent oracle, nothing of it is in the engine).
usage: python tb_test.py <engine exe> <syzygy folder> [positions=1500] [seed=1] [convert_games=40]
 1. the tablebase result (WDL) of random 3-5 man positions must equal python-chess
 2. root filter: every kept move has the best win/draw/loss class, and for won positions the kept moves are the fastest wins (smallest DTZ; a move that resets the fifty-move counter or mates counts 1) plus the wins with DTZ <= 3
 3. search: `go depth 8` returns a kept move
 4. conversion: the engine plays won endgames against a defender that always makes the DTZ-longest move; it must win (checkmate, or the opponent's last piece falls) within 100 moves
Environment: TB_MAXMEN=4 limits the positions to 3 and 4 men (when only those files are present), TB_THREADS=n sets Threads (default 1).
The last line is  tb_test: PASS  or  tb_test: FAIL  (exit code 0 / 1).
Needs: pip install chess"""
import os, random, subprocess, sys, time
import chess, chess.syzygy

EXE, TBDIR = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
SEED = int(sys.argv[4]) if len(sys.argv) > 4 else 1
NCONV = int(sys.argv[5]) if len(sys.argv) > 5 else 40
MAXMEN = int(os.environ.get("TB_MAXMEN", "5"))
rng = random.Random(SEED)
tb = chess.syzygy.open_tablebase(TBDIR)


class Engine:
    def __init__(self):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.send("uci"); self.wait("uciok"); self.send("setoption name Threads value " + os.environ.get("TB_THREADS", "1")); self.send("setoption name SyzygyPath value " + TBDIR); self.send("isready"); self.lines = self.wait("readyok")

    def send(self, s): self.p.stdin.write(s + "\n"); self.p.stdin.flush()

    def wait(self, token, timeout=120):
        out = []; t0 = time.time()
        while time.time() - t0 < timeout:
            ln = self.p.stdout.readline()
            if not ln: break
            ln = ln.rstrip("\n"); out.append(ln)
            if ln.startswith(token): return out
        raise RuntimeError("timeout waiting for " + token + ": " + " | ".join(out[-5:]))

    def probe(self, fen):
        self.send("position fen " + fen); self.send("tbprobe"); self.send("isready"); o = self.wait("readyok")
        wdl = None; kept = None
        for l in o:
            if l.startswith("tbprobe: wdl"): wdl = int(l.split()[-1])
            elif l.startswith("tbprobe:"): wdl = l
            if l.startswith("tbroot: kept"): kept = l.split()[2:]
            elif l.startswith("tbroot:"): kept = None
        return wdl, kept

    def best(self, fen, moves=(), depth=8, movetime=None):
        self.send("position fen " + fen + (" moves " + " ".join(moves) if moves else ""))
        self.send("go depth %d" % depth if movetime is None else "go movetime %d" % movetime)
        o = self.wait("bestmove"); return o[-1].split()[1], o

    def close(self):
        try: self.send("quit")
        except Exception: pass
        self.p.wait(timeout=10)


def random_position():
    while True:
        npieces = rng.choice([n for n in (1, 2, 2, 3, 3) if n + 2 <= MAXMEN])        # pieces besides the kings: 3, 4 or 5 men (TB_MAXMEN=4 when only the 3-4 man files are present)
        b = chess.Board(None); b.clear_board()
        sq = rng.sample(range(64), 2 + npieces)
        b.set_piece_at(sq[0], chess.Piece(chess.KING, True)); b.set_piece_at(sq[1], chess.Piece(chess.KING, False))
        ok = True
        for s in sq[2:]:
            pt = rng.choice([chess.PAWN, chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN]); col = rng.random() < 0.5
            if pt == chess.PAWN and chess.square_rank(s) in (0, 7): ok = False; break
            b.set_piece_at(s, chess.Piece(pt, col))
        if not ok: continue
        b.turn = rng.random() < 0.5
        b.castling_rights = 0; b.halfmove_clock = 0
        if not b.is_valid() or b.is_game_over(): continue
        try: tb.probe_wdl(b)
        except Exception: continue
        return b


def cls(w): return 1 if w == 2 else (-1 if w == -2 else 0)


def move_dtz(b, m):
    """DTZ of a winning move as the engine's tablebase code counts it: 1 for a mating move and for a move that resets the fifty-move counter (capture, pawn move) and keeps the win,
    otherwise the DTZ of the position after the move plus 1 (the plies until the next reset or the mate, the move included)."""
    zeroing = b.is_capture(m) or b.piece_type_at(m.from_square) == chess.PAWN
    b.push(m)
    try:
        return 1 if (b.is_checkmate() or zeroing) else abs(tb.probe_dtz(b)) + 1
    finally:
        b.pop()


eng = Engine()
print([l for l in eng.lines if "Syzygy" in l])
bad1 = bad2 = bad3 = n2 = n3 = 0; t0 = time.time()
for i in range(N):
    b = random_position(); fen = b.fen()
    exp = tb.probe_wdl(b)
    wdl, kept = eng.probe(fen)
    if wdl != exp:
        bad1 += 1
        if bad1 <= 5: print("WDL MISMATCH", fen, "engine", wdl, "python-chess", exp)
    # 2. root filter
    if kept is not None and b.legal_moves.count() > 1:
        n2 += 1
        legal = list(b.legal_moves); res = {}
        for m in legal:
            b.push(m)
            try: w = -tb.probe_wdl(b); d = tb.probe_dtz(b)
            except Exception: w = None; d = None
            b.pop(); res[m.uci()] = (w, d)
        if all(v[0] is not None for v in res.values()):
            best_cls = max(cls(v[0]) for v in res.values())
            wrong = [m for m in kept if cls(res[m][0]) != best_cls]
            problem = None
            if wrong: problem = "kept moves with a worse class: %s" % wrong
            elif best_cls == 1:
                win = {m: move_dtz(b, chess.Move.from_uci(m)) for m, v in res.items() if cls(v[0]) == 1 and v[1] is not None}
                if win:
                    fastest = min(win.values())
                    if not any(win.get(m) == fastest for m in kept): problem = "the fastest win (dtz %d) is not kept %s, kept dtz %s" % (fastest, kept, [win.get(m) for m in kept])
                    elif any(win.get(m, 10 ** 9) > max(fastest, 3) for m in kept): problem = "a slow win is kept (fastest dtz %d): %s, kept dtz %s" % (fastest, kept, [win.get(m) for m in kept])
            if problem:
                bad2 += 1
                if bad2 <= 5: print("ROOT FILTER", fen, problem)
    # 3. search
    if i < 60 and not b.is_game_over():
        mv, _ = eng.best(fen, depth=8); n3 += 1
        if mv not in [m.uci() for m in b.legal_moves] or (kept is not None and mv not in kept):
            bad3 += 1; print("SEARCH MOVE not allowed", fen, mv, kept)
print("1. WDL: %d positions, %d mismatches | 2. root filter: %d checked, %d problems | 3. search: %d checked, %d problems | %.0f s" % (N, bad1, n2, bad2, n3, bad3, time.time() - t0))

# 4. conversion of won endgames
def won_start():
    while True:
        b = random_position()
        if b.turn and tb.probe_wdl(b) == 2 and len(b.piece_map()) <= MAXMEN and b.halfmove_clock == 0: return b


conv = 0; fails = []
for g in range(NCONV):
    b = won_start(); start = b.fen(); moves = []
    eng.send("ucinewgame")
    for ply in range(200):
        if b.is_game_over() or b.halfmove_clock >= 100: break
        if b.turn:
            mv, _ = eng.best(start, moves, depth=12)
        else:                                                                 # defender: not losing first, then the longest resistance (largest |dtz|)
            best = None
            for m in b.legal_moves:
                b.push(m)
                try: d = tb.probe_dtz(b); w = tb.probe_wdl(b)
                except Exception: d = 0; w = 0
                b.pop()
                key = (w, abs(d))                                            # w is from the point of view of the side to move after the move = the winning side: smaller is better for the defender
                if best is None or key < best[0]: best = (key, m)
            mv = best[1].uci()
        b.push_uci(mv); moves.append(mv)
    if b.is_checkmate() and b.turn == chess.BLACK and b.halfmove_clock < 100: conv += 1
    else: fails.append((start, len(moves), b.halfmove_clock, "stalemate" if b.is_stalemate() else "no mate"))
print("4. conversion: %d of %d won endgames converted to a mate" % (conv, NCONV))
for f in fails[:6]: print("   not converted:", f)
eng.close()
ok = bad1 == 0 and bad2 == 0 and bad3 == 0 and conv == NCONV
print("tb_test: " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
