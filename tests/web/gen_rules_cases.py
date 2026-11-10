"""Writes tests/web/rules_cases.json: positions from random games (python-chess is the reference) for tests/web/rules_test.js.
usage: python tests/web/gen_rules_cases.py [games=300] [seed=1]   (needs pip install chess)"""
import json
import os
import random
import sys

import chess

games = int(sys.argv[1]) if len(sys.argv) > 1 else 300
rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
cases = []
extra_fens = [
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "8/8/8/8/8/8/8/K1k5 w - - 0 1",
    "4k3/8/8/8/8/8/8/4KB1B w - - 0 1",
    "4k3/8/8/8/8/8/8/2B1K1B1 w - - 0 1",
    "4k3/8/8/8/8/5n2/8/4KN2 w - - 0 1",
]


def record(board, keys):
    legal = list(board.legal_moves)
    case = {
        "fen": board.fen(en_passant="fen"),
        "legal": sorted(m.uci() for m in legal),
        "san": {m.uci(): board.san(m) for m in legal},
        "after": {m.uci(): None for m in legal},
        "mate": board.is_checkmate(), "stale": board.is_stalemate(), "insuf": board.is_insufficient_material(),
        "fifty": board.halfmove_clock >= 100 and bool(legal), "check": board.is_check(), "rep3": board.is_repetition(3),
        "keys": list(keys),
    }
    for m in legal:
        board.push(m)
        case["after"][m.uci()] = board.fen(en_passant="fen")
        board.pop()
    return case


for fen in extra_fens:
    b = chess.Board(fen)
    cases.append(record(b, [""]))
for g in range(games):
    b = chess.Board()
    keys = []
    for ply in range(rng.randint(10, 260)):
        if b.is_game_over(claim_draw=False):
            break
        keys.append(b._transposition_key())
        cases.append(record(b, ["k%d" % hash(k) for k in keys]))
        # prefer captures / promotions / castling now and then so that rare moves show up
        legal = list(b.legal_moves)
        special = [m for m in legal if b.is_capture(m) or m.promotion or b.is_castling(m) or b.is_en_passant(m)]
        m = rng.choice(special) if special and rng.random() < 0.35 else rng.choice(legal)
        b.push(m)
json.dump(cases, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "rules_cases.json"), "w"))
print("cases:", len(cases))
