"""Positions that break engines.   usage: python breakers.py <exe> [--quick] [--only perft,mate,forced,limits,deep,fuzz,eval,tb] [--seed N] [--timeout S] [--selfcheck] [--threads N] [--syzygy FOLDER] [--list]

Black-box test over UCI; python-chess is the rules oracle (pip install chess).  The process must stay alive, answer isready afterwards, print nothing on stderr (an AddressSanitizer or UBSan build
reports there), and every `go` must end with a legal bestmove (0000 or (none) when there is no legal move).  Sections:
  perft   the move generator on the classical hard positions: en passant that exposes the king, castling through or out of attacked squares, promotions with check, 218 legal moves, Chess960 castling;
          the node counts come from python-chess (and agree with the published perft tables); every position is also searched (depth 5, 30,000 nodes)
  mate    a mate in one that only an unusual move delivers (underpromotion, castling, en passant, double check) and stalemate traps (the natural move stalemates, another one mates):
          the engine must play a mating move at depth 1, 2, 6, with a node limit and with a time limit; and won positions with a stalemating move where nothing mates (it must not play it)
  forced  one legal move, no legal move (checkmate, stalemate), MultiPV larger than the number of legal moves
  limits  inputs at the limits: a game of 3,880 plies, a move list beyond the 3,900 plies the engine keeps, halfmove clocks and move numbers up to 2^64, Chess960 flags in a normal game
  deep    random positions with many queens, rooks and checks (the search reaches its maximum ply), 300 ms each
  fuzz    random legal positions (random placements with 2 to 32 pieces, with 3 to 6 pieces, and random walks): perft 1 to 3 against python-chess and a legal bestmove
  eval    endings of 3 to 5 men that fool an engine without tablebases (fortresses, narrow wins, drawing defences). The truth was read from the Syzygy files when the table EVAL was made. A "breaker" is a
          position where Kestrel 3.0 plays a move that throws the win away or loses the draw; a "control" is a hard one it plays right (it must keep doing so: a failure is a problem); a "fooled" one is
          played right but scored far from the truth (a draw shown as +3 or more, a win as +1 or less). Breakers and fooled positions are not problems: the section prints how many still beat the engine
          (--list shows them). With --syzygy FOLDER every position must be played right
  tb      (Kestrel 3.0 and later; only with --syzygy FOLDER) positions that break tablebase code: en passant captures, castling rights, fifty-move counters from 0 to 101, 3 to 5 men: the engine's
          tablebase result must agree with python-chess reading the same files, and the search must return a legal move
--quick keeps the depths and counts small (about 30 s); --selfcheck recomputes the perft table with python-chess (depth 1 to 3) before it is used; --threads N sets the option Threads;
--syzygy FOLDER sets SyzygyPath for the whole run (every section then runs with the tablebases on) and adds the section tb. With tablebases a mate in one whose position is in the tables scores as
a tablebase win (about 28000, below every mate score), so the mate section then accepts any move that keeps a forced win (3.0 can play a mate in two instead of the mate in one there).
The last line is "breakers: PASS" or "breakers: FAIL (n problems)"; the exit code is 0 either way (like the other scripts here)."""
import random, re, subprocess, sys, threading, time
import chess

ARGS = sys.argv[1:]
if not ARGS or ARGS[0].startswith("--"): sys.exit(__doc__)
EXE = ARGS[0]
QUICK = "--quick" in ARGS


def opt(name, default):
    return ARGS[ARGS.index(name) + 1] if name in ARGS and ARGS.index(name) + 1 < len(ARGS) else default


SEED = int(opt("--seed", 1)); TMO = float(opt("--timeout", 90)); THREADS = opt("--threads", None); SYZYGY = opt("--syzygy", None)
ONLY = set(opt("--only", "perft,mate,forced,limits,deep,fuzz,eval" + (",tb" if SYZYGY else "")).split(",")); LIST = "--list" in ARGS

# ------------------------------------------------------------------------------------------------------------------------------------ data
# (name, FEN, Chess960?, {depth: nodes}); the counts were computed with python-chess and agree with the published tables (Chess Programming Wiki, Martin Sedlak's perftsuite, the Chess960 perft list)
PERFT = [
    ('startpos', 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', False, {1: 20, 2: 400, 3: 8902, 4: 197281, 5: 4865609}),
    ('kiwipete', 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1', False, {1: 48, 2: 2039, 3: 97862, 4: 4085603}),
    ('cpw3 (rook pins, ep)', '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1', False, {1: 14, 2: 191, 3: 2812, 4: 43238, 5: 674624}),
    ('cpw4 (promotions, castling)', 'r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1', False, {1: 6, 2: 264, 3: 9467, 4: 422333}),
    ('cpw4 mirrored', 'r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1', False, {1: 6, 2: 264, 3: 9467, 4: 422333}),
    ('cpw5', 'rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8', False, {1: 44, 2: 1486, 3: 62379, 4: 2103487}),
    ('cpw6', 'r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10', False, {1: 46, 2: 2079, 3: 89890, 4: 3894594}),
    ('218 legal moves', 'R6R/3Q4/1Q4Q1/4Q3/2Q4Q/Q4Q2/pp1Q4/kBNN1KB1 w - - 0 1', False, {1: 218, 2: 99, 3: 19073, 4: 85043}),
    ('promotions with captures and checks', 'n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1', False, {1: 24, 2: 496, 3: 9483, 4: 182838, 5: 3605103}),
    ('ep: capture would expose the king to a bishop', '8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1', False, {1: 15, 2: 126, 3: 1928, 4: 13931, 5: 206379, 6: 1440467}),
    ('ep: pawn pinned on the rank', '3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1', False, {1: 18, 2: 92, 3: 1670, 4: 10138, 5: 185429, 6: 1134888}),
    ('ep: both pawns on the rank with the king and a rook', '8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1', False, {1: 13, 2: 102, 3: 1266, 4: 10276, 5: 135655, 6: 1015133}),
    ('castling both sides, nothing in between', 'r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1', False, {1: 26, 2: 568, 3: 13744, 4: 314346, 5: 7594526}),
    ('castling with bishops and queen aiming at the squares', 'r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1', False, {1: 26, 2: 1141, 3: 27826, 4: 1274206}),
    ('castling through attacked squares, rook attacked', 'r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1', False, {1: 44, 2: 1494, 3: 50509, 4: 1720476}),
    ('castling gives check', '5k2/8/8/8/8/8/8/4K2R w K - 0 1', False, {1: 15, 2: 66, 3: 1198, 4: 6399, 5: 120330, 6: 661072}),
    ('castling long, rook next to the king', '3k4/8/8/8/8/8/8/R3K3 w Q - 0 1', False, {1: 16, 2: 71, 3: 1286, 4: 7418, 5: 141077, 6: 803711}),
    ('promotion with a rook behind', '2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1', False, {1: 11, 2: 133, 3: 1442, 4: 19174, 5: 266199, 6: 3821001}),
    ('promotion next to the king', '4k3/1P6/8/8/8/8/K7/8 w - - 0 1', False, {1: 9, 2: 40, 3: 472, 4: 2661, 5: 38983, 6: 217342, 7: 3742283}),
    ("promotion, the king on the pawn's file", '8/P1k5/K7/8/8/8/8/8 w - - 0 1', False, {1: 6, 2: 27, 3: 273, 4: 1329, 5: 18135, 6: 92683, 7: 1555980}),
    ('promotion, kings in the corner', 'K1k5/8/P7/8/8/8/8/8 w - - 0 1', False, {1: 2, 2: 6, 3: 13, 4: 63, 5: 382, 6: 2217, 7: 15453}),
    ('promotion, the black king in front', '8/k1P5/8/1K6/8/8/8/8 w - - 0 1', False, {1: 10, 2: 25, 3: 268, 4: 926, 5: 10857, 6: 43261, 7: 567584}),
    ('double check, black to move', '4k3/8/8/8/8/8/4r3/4K1nR w - - 0 1', False, {1: 2, 2: 40, 3: 352, 4: 6683, 5: 85233, 6: 1593323}),
    ('blocked pawns, king and rook endgame', '8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1', False, {1: 29, 2: 165, 3: 5160, 4: 31961, 5: 1004658}),
    ('no pieces but kings and one pawn', '4k3/8/8/8/8/8/4P3/4K3 w - - 0 1', False, {1: 6, 2: 30, 3: 210, 4: 1424, 5: 10819, 6: 72339, 7: 563185}),
    ('960 #1', 'bqnb1rkr/pp3ppp/3ppn2/2p5/5P2/P2P4/NPP1P1PP/BQ1BNRKR w HFhf - 2 9', True, {1: 21, 2: 528, 3: 12189, 4: 326672, 5: 8146062}),
    ('960 #2', '2nnrbkr/p1qppppp/8/1ppb4/6PP/3PP3/PPP2P2/BQNNRBKR w HEhe - 1 9', True, {1: 21, 2: 807, 3: 18002, 4: 667366}),
    ('960 #3', 'b1q1rrkb/pppppppp/3nn3/8/P7/1PPP4/4PPPP/BQNNRKRB w GE - 1 9', True, {1: 20, 2: 479, 3: 10471, 4: 273318, 5: 6417013}),
    ('960 #4', 'qbbnnrkr/2pp2pp/p7/1p2pp2/8/P3PP2/1PPP1KPP/QBBNNR1R w hf - 0 9', True, {1: 22, 2: 593, 3: 13440, 4: 382958}),
    ('960 #5', '1nbbnrkr/p1p1ppp1/3p4/1p3P1p/3Pq2P/8/PPP1P1P1/QNBBNRKR w HFhf - 0 9', True, {1: 28, 2: 1120, 3: 31058, 4: 1171749}),
    ('960 #6', 'qnbnr1kr/ppp1b1pp/4p3/3p1p2/8/2NPP3/PPP1BPPP/1QBNR1KR w HEhe - 1 9', True, {1: 25, 2: 775, 3: 20236, 4: 626315}),
    ('960 #7', 'rbbqn1kr/pp2p1pp/6n1/2pp1p2/2P4P/P7/BP1PPPP1/R1BQNNKR w HAha - 0 9', True, {1: 27, 2: 916, 3: 25798, 4: 890435}),
    ('960 #8', 'rqbbknr1/1ppp2pp/p5n1/4pp2/P7/1PP5/1Q1PPPPP/R1BBKNRN w GAga - 0 9', True, {1: 24, 2: 600, 3: 15347, 4: 408207}),
    ('960 king between the rooks, castling both ways', '1r2k1r1/8/8/8/8/8/8/1R2K1R1 w GBgb - 0 1', True, {1: 25, 2: 525, 3: 12297, 4: 272627, 5: 6457234}),
    ('960 rooks next to the king', 'r1kr4/8/8/8/8/8/8/R1KR4 w DAda - 0 1', True, {1: 22, 2: 403, 3: 9016, 4: 193593, 5: 4425916}),
    ('960 king on the g-file, rook h', '4r1kr/8/8/8/8/8/8/4R1KR w HEhe - 0 1', True, {1: 23, 2: 442, 3: 9729, 4: 204280, 5: 4655008}),
    ('960 castling blocked by the other rook', 'rkr5/8/8/8/8/8/8/RKR5 w CAca - 0 1', True, {1: 20, 2: 328, 3: 6855, 4: 138584, 5: 3021508}),
]

# a mate in one that only an unusual move delivers, and stalemate traps: (class, FEN); the mating moves are found with python-chess when the test runs
MATE = [
    ('underpromotion mate', '6bq/5Ppk/7q/8/8/8/1N3K2/8 w - - 0 1'),
    ('underpromotion mate', '6bb/5Prk/7q/2R5/8/3K4/R7/8 w - - 0 1'),
    ('underpromotion mate', 'nb6/krP5/b7/8/8/5B2/7P/K7 w - - 0 1'),
    ('underpromotion mate', '8/pP6/k1K5/pn6/8/8/8/8 w - - 0 1'),
    ('underpromotion mate', '8/3P1R2/4k3/4n3/3Q4/8/3K4/8 w - - 0 1'),
    ('underpromotion mate', '8/6Pr/6bk/3P2nn/3B4/8/8/4K3 w - - 0 1'),
    ('castling mate', '2N5/8/7N/4Q3/8/5k2/8/4K2R w K - 0 1'),
    ('castling mate', '3B4/8/4Q3/8/5B1P/5k2/8/N3K2R w K - 0 1'),
    ('castling mate', '8/8/3RR3/8/7Q/5k2/b7/4K2R w K - 0 1'),
    ('castling mate', '8/8/8/4Q3/1R6/5k2/8/4K2R w K - 0 1'),
    ('castling mate', '4R3/8/8/8/8/2p2k2/6R1/4K2R w K - 0 1'),
    ('castling mate', '8/8/8/2Q5/5r2/2Nk4/8/R3K3 w Q - 0 1'),
    ('en passant mate', '8/5k1K/7R/4RPp1/5Q2/8/8/8 w - g6 0 1'),
    ('en passant mate', '8/7R/8/6pP/5Kpk/6r1/8/8 w - g6 0 1'),
    ('en passant mate', 'k7/2Q5/8/Pp6/8/8/R3K3/8 w - b6 0 1'),
    ('en passant mate', '5Q2/7k/8/5KpP/8/8/8/8 w - g6 0 1'),
    ('en passant mate', '7k/5K2/3p4/6pP/8/8/8/3R3R w - g6 0 1'),
    ('double check mate', '2Q1N1k1/2K2nqn/8/8/8/8/8/B7 w - - 0 1'),
    ('double check mate', '8/8/4BP2/8/8/8/5Knb/1Q3N1k w - - 0 1'),
    ('double check mate', '1k6/qn6/8/8/2Q2R2/2K4R/7Q/8 w - - 0 1'),
    ('double check mate', '2k1N2Q/1bnq4/4Q3/8/7K/8/3B4/8 w - - 0 1'),
    ('double check mate', '3K4/8/1R6/8/5B2/8/3nnb2/4k1NR w - - 0 1'),
    ('double check mate', '8/8/2B1b1p1/4pkr1/4n1q1/5P2/5R2/K1R5 w - - 0 1'),
    ('stalemate trap', '8/8/8/6Q1/5N2/8/K7/7k w - - 0 1'),
    ('stalemate trap', 'k1K5/8/8/8/8/8/P7/1Q6 w - - 0 1'),
    ('stalemate trap', 'k1N5/2K5/8/8/8/8/8/4Q3 w - - 0 1'),
    ('stalemate trap', '8/8/8/3P4/8/2K5/7Q/k7 w - - 0 1'),
    ('stalemate trap', 'k7/2K5/8/8/8/3N4/3Q4/8 w - - 0 1'),
    ('stalemate trap', '2Q5/k7/3N4/8/8/8/8/7K w - - 0 1'),
]

# a winning position (queen or rook against a bare king ...) where a move stalemates the opponent and nothing mates in one: the engine must not play a stalemating move (depth 2 and more)
STALE_AVOID = [
    '6Bk/8/8/K7/8/8/3R4/8 w - - 0 1',
    '8/8/3N4/8/8/3K2Q1/8/7k w - - 0 1',
    '7k/3N4/Q7/8/8/8/3K4/8 w - - 0 1',
    'k7/6Q1/8/2K5/8/8/8/8 w - - 0 1',
    '8/8/6Q1/K7/8/8/8/7k w - - 0 1',
    'K6k/8/8/R7/8/8/B7/8 w - - 0 1',
    '3R4/8/8/1K2B3/8/8/8/7k w - - 0 1',
    '7k/5Q2/8/8/8/8/8/4K3 w - - 0 1',
    '7k/8/2K5/3Q4/8/8/8/8 w - - 0 1',
    '8/8/KQ6/8/8/8/8/k3B3 w - - 0 1',
]

# the side to move has exactly one legal move (a check evasion)
ONE_LEGAL = [
    ('one legal move', '1B3N2/3q4/3K4/1q6/4k3/8/4P3/8 w - - 0 1'),
    ('one legal move', '8/2K1q3/b6q/8/8/1k1q4/8/8 w - - 0 1'),
    ('one legal move', 'Q7/7N/6n1/8/k1B5/5Q2/6B1/3K4 b - - 0 1'),
    ('one legal move', '4b3/8/8/1r3r2/4NK2/q7/8/4k3 w - - 0 1'),
    ('one legal move', '8/4q3/3k4/2b5/K1q5/8/8/5R2 w - - 0 1'),
    ('one legal move', '8/1K3b2/8/1Q6/8/kQ6/8/8 b - - 0 1'),
    ('one legal move', '2r5/3Q4/2K1r3/6r1/8/k7/8/8 w - - 0 1'),
    ('one legal move', '8/8/8/3K4/8/5Q2/8/1Q5k b - - 0 1'),
]

# positions with 2 to 5 legal moves: (number of legal moves, FEN)
FEW = [
    (2, '8/1R6/8/5b2/8/n3K3/3B4/4b1kR b - - 0 1'),
    (2, '2q3K1/8/8/1k5r/3B4/8/7Q/8 w - - 0 1'),
    (2, '3q4/2K5/8/P7/8/5n2/k1Pq3q/8 w - - 0 1'),
    (3, '3q3k/8/3K3P/6b1/1N6/4N1b1/8/8 w - - 0 1'),
    (3, '8/8/7n/8/8/3k4/1K2B3/3R4 b - - 0 1'),
    (3, '8/3qK3/8/8/8/8/7p/6k1 w - - 0 1'),
    (4, 'q7/8/8/8/2n4Q/2k1KN2/8/8 w - - 0 1'),
    (4, '6q1/8/bk6/8/8/1K2P2r/8/b7 w - - 0 1'),
    (4, '8/8/6k1/3Q4/4K1P1/B7/8/8 b - - 0 1'),
    (5, '5k2/8/8/5Kr1/8/2Q5/8/8 w - - 0 1'),
    (5, '4n3/2N2b2/3k4/8/3B4/2K2q2/6p1/8 w - - 0 1'),
    (5, '5Bk1/4P3/8/8/8/8/2K5/3b1q2 w - - 0 1'),
]

# the side to move has no legal move
NO_LEGAL = [
    ("checkmate (fool's mate)", "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3"),
    ("stalemate, queen and king", "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"),
    ("stalemate, a blocked pawn", "k7/P7/K7/8/8/8/8/8 b - - 0 1"),
    ("smothered mate", "6rk/5Npp/8/8/8/8/8/7K b - - 0 1"),
    ("back-rank mate", "R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1"),
    ("stalemate, white to move", "8/8/8/8/8/5k2/5p2/5K2 w - - 0 1"),
]

# endings of 3 to 5 men that fool an engine without tablebases: (status, truth for the side to move, signature, FEN, the moves that keep the result); the truth comes from the Syzygy 3-4-5 files
# status: breaker (Kestrel 3.0 throws the win away or loses the draw at 50,000 and at 200,000 nodes) | control (a hard position it plays right at 50,000, 200,000 and 1,000,000 nodes)
#         | fooled (played right, but the score is far from the truth at 200,000 nodes)
EVAL = [
    ('control', 'win', 'Fine 70', '8/k7/3p4/p2P1p2/P2P1P2/8/8/K7 w - - 0 1', 'a1b1'),
    ('control', 'win', 'KBBvKR', '8/3B4/8/2r5/8/5k2/8/B4K2 b - - 0 1', 'c5c1'),
    ('control', 'win', 'KBPvKB', '8/b1k2P2/4K3/8/6B1/8/8/8 w - - 0 1', 'e6d5 f7f8q f7f8r'),
    ('control', 'win', 'KBPvKB', '4K2b/4P3/8/8/8/8/4B3/k7 w - - 0 1', 'e8d7 e8f7 e8f8'),
    ('control', 'win', 'KBPvKN', '8/8/6P1/8/8/6k1/B1K1n3/8 w - - 0 1', 'c2b2 c2d2 g6g7'),
    ('control', 'win', 'KBvKPP', '8/1p2k3/6B1/4K3/8/p7/8/8 b - - 0 1', 'a3a2'),
    ('control', 'win', 'KNNvKP', '6NN/1K6/8/8/8/2p5/k7/8 b - - 0 1', 'c3c2'),
    ('control', 'win', 'KNPvKP', '8/8/8/2P5/8/3N3k/3K2p1/8 b - - 0 1', 'g2g1q h3g4'),
    ('control', 'win', 'KPPvKP', '8/8/8/3K2PP/8/4p3/k7/8 b - - 0 1', 'e3e2'),
    ('control', 'win', 'KPvKP', '8/7k/2K5/6p1/8/8/1P6/8 b - - 0 1', 'g5g4'),
    ('control', 'win', 'KPvKP', '7k/8/4P2p/8/8/8/4K3/8 w - - 0 1', 'e6e7'),
    ('control', 'win', 'KQvKBB', 'K7/8/8/8/6k1/1b6/5b2/Q7 w - - 0 1', 'a1e5 a1g7 a1h1'),
    ('control', 'win', 'KQvKBN', '2Q5/8/1b1k4/3n1K2/8/8/8/8 b - - 0 1', 'd5e7'),
    ('control', 'win', 'KQvKNP', 'Q4K2/8/8/5n2/1k6/2p5/8/8 w - - 0 1', 'a8a1 a8a2 a8b7 a8c6 a8e4'),
    ('control', 'win', 'KQvKRB', 'k2r4/8/8/8/8/8/2b4K/4Q3 w - - 0 1', 'e1a1 e1a5 e1c3 e1e6'),
    ('control', 'win', 'KQvKRN', '8/8/6n1/8/r5k1/8/6K1/6Q1 w - - 0 1', 'g1d1'),
    ('control', 'win', 'KQvKRP', '8/K7/8/8/2p3r1/3k4/8/4Q3 w - - 0 1', 'a7a6 a7b6 a7b7 e1b1 e1d1 e1f1'),
    ('control', 'win', 'KRPvKB', '8/5K2/7R/8/8/3bP3/8/4k3 w - - 0 1', 'e3e4 h6e6 h6h2'),
    ('control', 'win', 'KRPvKN', '8/4k3/8/5PR1/8/8/7n/K7 w - - 0 1', 'f5f6'),
    ('control', 'win', 'KRPvKP', '4k3/8/7K/3P4/6R1/8/5p2/8 w - - 0 1', 'g4e4 g4f4'),
    ('control', 'win', 'KRvKN', '5n2/8/8/6R1/8/8/5k2/2K5 w - - 0 1', 'g5f5'),
    ('control', 'win', 'KRvKPP', '8/1R6/8/3k4/7p/2K5/4p3/8 b - - 0 1', 'e2e1q'),
    ('control', 'draw', 'KBBvKN', '8/8/8/2K5/7k/7n/8/2B4B b - - 0 1', 'h3f2'),
    ('control', 'draw', 'KBPvKP', '3B4/8/6P1/8/8/kp2K3/8/8 b - - 0 1', 'b3b2'),
    ('control', 'draw', 'KBPvKP', '8/8/3k4/1P6/8/7B/1K5p/8 w - - 0 1', 'h3g2'),
    ('control', 'draw', 'KBvKP', '8/8/7B/8/4K1k1/8/p7/8 w - - 0 1', 'h6g7'),
    ('control', 'draw', 'KPPvKP', '8/8/8/K7/6Pp/2P5/8/k7 w - - 0 1', 'g4g5'),
    ('control', 'draw', 'KQPvKP', '8/3k4/8/2QP3K/8/8/7p/8 b - - 0 1', 'h2h1q'),
    ('control', 'draw', 'KQPvKP', '5K2/6Q1/8/6Pk/8/8/3p4/8 b - - 0 1', 'd2d1q'),
    ('control', 'draw', 'KQPvKQ', '5Q2/2P5/3K4/8/8/1q6/8/4k3 b - - 0 1', 'b3a3 b3b4 b3b6 b3d1 b3d3 b3g3'),
    ('control', 'draw', 'KQPvKQ', '8/5P2/2q5/8/5k2/8/8/KQ6 b - - 0 1', 'c6a4 c6a6 c6a8 c6c3 c6f6'),
    ('control', 'draw', 'KQQvKQ', '8/8/4q3/8/k7/2Q2K2/8/1Q6 b - - 0 1', 'e6e2 e6f5 e6g4 e6h3'),
    ('control', 'draw', 'KQvKBP', '1K6/4b3/8/1Q6/8/8/2k3p1/8 b - - 0 1', 'e7d6 g2g1q'),
    ('control', 'draw', 'KQvKNP', '8/8/4n3/8/7Q/8/p2k4/5K2 b - - 0 1', 'a2a1q'),
    ('control', 'draw', 'KQvKP', '6Q1/8/8/2k5/8/8/4p3/2K5 b - - 0 1', 'e2e1q'),
    ('control', 'draw', 'KQvKRB', '3Q4/1b6/r7/8/1K6/4k3/8/8 b - - 0 1', 'a6g6 b7e4 b7f3 b7g2'),
    ('control', 'draw', 'KQvKRN', '2r5/6k1/8/7K/Q7/8/7n/8 b - - 0 1', 'c8c5 c8h8'),
    ('control', 'draw', 'KRBvKR', '8/8/r7/5B2/6R1/6K1/8/7k b - - 0 1', 'a6a3 h1g1'),
    ('control', 'draw', 'KRNvKB', '6R1/8/8/1b6/8/3k2N1/K7/8 b - - 0 1', 'b5c4'),
    ('control', 'draw', 'KRNvKB', '1R6/8/8/4K1b1/8/2N3k1/8/8 b - - 0 1', 'g5f4'),
    ('control', 'draw', 'KRvKPP', '8/K6R/8/2p5/8/4k3/3p4/8 w - - 0 1', 'h7e7'),
    ('control', 'draw', 'Reti 1921', '7K/8/k1P5/7p/8/8/8/8 w - - 0 1', 'h8g7'),
    ('breaker', 'win', 'KBBvKN', 'K7/4n3/1B2B2k/8/8/8/8/8 w - - 0 1', 'a8b7'),
    ('breaker', 'win', 'KBBvKN', '8/7B/4n3/8/8/k5B1/8/6K1 w - - 0 1', 'h7g8'),
    ('breaker', 'win', 'KBBvKN', '1n6/7B/5k1B/1K6/8/8/8/8 w - - 0 1', 'h7b1 h7c2'),
    ('breaker', 'win', 'KBBvKN', '1B6/1K1B4/8/8/8/8/2k5/6n1 w - - 0 1', 'b7c6 d7b5'),
    ('breaker', 'win', 'KBBvKN', '1K6/8/5n2/7k/8/B6B/8/8 w - - 0 1', 'a3e7 b8c7 h3e6'),
    ('breaker', 'win', 'KBBvKN', '8/4BB2/k7/8/3n4/8/8/3K4 w - - 0 1', 'd1d2 e7c5 f7e8'),
    ('breaker', 'win', 'KBBvKN', '2k1B3/8/7K/n7/8/8/1B6/8 w - - 0 1', 'b2a3 h6g6 h6g7'),
    ('breaker', 'win', 'KBBvKN', '4B3/3K4/8/8/8/2n1B3/8/3k4 w - - 0 1', 'e3h6 e8g6'),
    ('breaker', 'win', 'KBBvKN', '8/2K5/8/2B5/8/2k5/8/1n3B2 w - - 0 1', 'c5a7 c5b6 c5f2 c7b6 c7c6 c7d6 f1a6 f1b5 f1e2'),
    ('breaker', 'win', 'KBBvKN', '8/B7/7k/8/K1B5/8/7n/8 w - - 0 1', 'a4b4 a4b5'),
    ('breaker', 'win', 'KBBvKN', '8/2K5/8/8/1n3B2/k4B2/8/8 w - - 0 1', 'c7b6 c7d6 f3e4 f4d2 f4e3 f4e5 f4h2'),
    ('breaker', 'win', 'KBBvKN', '8/2K5/5k2/8/8/1n3BB1/8/8 w - - 0 1', 'c7d6'),
    ('breaker', 'win', 'KBBvKN', '3K1B2/8/8/8/8/8/6k1/1B1n4 w - - 0 1', 'b1g6 d8c7 d8d7 d8e7'),
    ('breaker', 'win', 'KBNvKN', 'k7/2K5/8/5n2/5B2/8/1N6/8 w - - 0 1', 'b2d3'),
    ('breaker', 'win', 'KBNvKN', '3n4/6N1/B7/8/3K4/8/8/k7 w - - 0 1', 'a6c4'),
    ('breaker', 'win', 'KBPvKB', '8/8/4KB2/4P3/8/5k2/8/6b1 w - - 0 1', 'e6d5'),
    ('breaker', 'win', 'KBPvKB', 'B7/7k/2P5/8/8/8/4b1K1/8 w - - 0 1', 'c6c7 g2f2'),
    ('breaker', 'win', 'KBPvKB', '8/1K6/7P/8/8/2b5/3k1B2/8 w - - 0 1', 'b7c6'),
    ('breaker', 'win', 'KBPvKN', '8/K7/8/4P3/8/n3B3/6k1/8 w - - 0 1', 'e5e6'),
    ('breaker', 'win', 'KBPvKN', '8/8/7k/8/P2n1K2/B7/8/8 w - - 0 1', 'a4a5'),
    ('breaker', 'win', 'KBPvKN', '8/1k6/8/8/8/3B4/1K4P1/6n1 w - - 0 1', 'b2c3 d3c4'),
    ('breaker', 'win', 'KBPvKN', '1n6/8/1K6/8/8/1k5B/7P/8 w - - 0 1', 'b6c5 b6c7 h3f1 h3g2'),
    ('breaker', 'win', 'KNNvKP', 'N7/5p2/8/8/8/k1N5/8/1K6 w - - 0 1', 'c3d5 c3e2'),
    ('breaker', 'win', 'KNNvKP', '2k5/8/8/2pN4/8/8/7N/5K2 w - - 0 1', 'd5b6'),
    ('breaker', 'win', 'KNNvKP', '7k/3p4/8/N7/6K1/8/8/5N2 w - - 0 1', 'a5b3'),
    ('breaker', 'win', 'KNNvKP', '8/1p4k1/8/1K6/8/8/6N1/2N5 w - - 0 1', 'b5b6'),
    ('breaker', 'win', 'KNNvKP', '1k6/8/1N1p4/8/8/N7/1K6/8 w - - 0 1', 'a3b5 a3c2 b6d5'),
    ('breaker', 'win', 'KNNvKP', '8/8/p6N/7N/K7/8/2k5/8 w - - 0 1', 'a4a5'),
    ('breaker', 'win', 'KNNvKP', '8/N6p/8/8/4K3/2N5/8/4k3 w - - 0 1', 'e4e3'),
    ('breaker', 'win', 'KNNvKP', '8/3N4/7p/3N4/6K1/8/8/k7 w - - 0 1', 'g4h5'),
    ('breaker', 'win', 'KNNvKP', '3k4/5p2/8/8/6K1/8/5N2/5N2 w - - 0 1', 'f1d2 f1e3 f1g3 f1h2 f2d1 f2d3 f2h1 f2h3 g4f3 g4f4 g4f5 g4g3 g4g5'),
    ('breaker', 'win', 'KNPvKB', '2N5/2P5/8/4K3/8/2kb4/8/8 w - - 0 1', 'c8d6'),
    ('breaker', 'win', 'KNPvKB', '8/N7/1KP5/8/8/1b6/5k2/8 w - - 0 1', 'b6c5'),
    ('breaker', 'win', 'KNPvKN', '8/8/8/5PN1/8/8/2K3n1/4k3 w - - 0 1', 'f5f6'),
    ('breaker', 'win', 'KPPvKR', '8/8/8/8/5K1P/5P2/2k5/r7 b - - 0 1', 'c2b3 c2c3 c2d3'),
    ('breaker', 'win', 'KQPvKQ', '8/8/8/1q6/4PQ2/K7/6k1/8 w - - 0 1', 'e4e5 f4g4'),
    ('breaker', 'win', 'KQPvKQ', '2q5/8/8/8/3PQ3/6k1/8/K7 w - - 0 1', 'd4d5'),
    ('breaker', 'win', 'KQPvKQ', '8/6k1/8/8/2P5/7q/2K2Q2/8 w - - 0 1', 'c4c5'),
    ('breaker', 'win', 'KQPvKQ', '8/5Q2/4K3/8/1q6/6P1/k7/8 w - - 0 1', 'e6e5'),
    ('breaker', 'win', 'KQPvKQ', '8/6q1/1k6/5P2/8/3K4/8/2Q5 w - - 0 1', 'c1e3'),
    ('breaker', 'win', 'KQPvKQ', '8/8/8/8/1Q6/8/3P4/1K3k1q w - - 0 1', 'b1c2'),
    ('breaker', 'win', 'KQPvKQ', '8/8/8/6QP/3K4/8/8/k3q3 w - - 0 1', 'g5e5'),
    ('breaker', 'win', 'KQPvKQ', '8/7K/2k5/8/6P1/2q5/Q7/8 w - - 0 1', 'a2a6 g4g5'),
    ('breaker', 'win', 'KQvKBB', '2K5/8/8/6Q1/8/2b5/8/1k1b4 w - - 0 1', 'g5b5'),
    ('breaker', 'win', 'KQvKBB', '8/8/5Q2/4K3/2b5/b7/2k5/8 w - - 0 1', 'f6f2 f6f5 f6g6'),
    ('breaker', 'win', 'KQvKBB', '2Q5/8/8/4b3/3k4/8/K7/5b2 w - - 0 1', 'c8d8'),
    ('breaker', 'win', 'KQvKBB', '1Q6/8/6K1/8/4k1b1/8/3b4/8 w - - 0 1', 'b8a8 b8b1 b8b7 b8e8'),
    ('breaker', 'win', 'KQvKBB', '3K4/8/3k4/1b6/1b6/8/6Q1/8 w - - 0 1', 'g2a2 g2b7 g2f3 g2g6'),
    ('breaker', 'win', 'KQvKBB', 'K7/8/3Q4/8/8/1b6/1b6/1k6 w - - 0 1', 'd6c5 d6c6 d6d2 d6d3 d6g6'),
    ('breaker', 'win', 'KQvKBB', '1b6/8/8/8/8/5bk1/2QK4/8 w - - 0 1', 'c2b1 c2c8 c2d3 c2f5 c2g6 c2h7 d2e1'),
    ('breaker', 'win', 'KQvKBB', '8/Q7/3K4/8/8/5b2/6k1/4b3 w - - 0 1', 'a7a2'),
    ('breaker', 'win', 'KQvKBB', '8/7Q/1K2b3/6k1/5b2/8/8/8 w - - 0 1', 'h7e4'),
    ('breaker', 'win', 'KQvKBB', '1Q6/8/8/8/8/4b3/4b3/3k3K w - - 0 1', 'b8b2'),
    ('breaker', 'win', 'KQvKBB', '8/8/3Q4/8/8/K6k/4b3/6b1 w - - 0 1', 'd6e6 d6g6'),
    ('breaker', 'win', 'KQvKBB', '8/Q3K3/8/8/8/2b5/3kb3/8 w - - 0 1', 'a7f2'),
    ('breaker', 'win', 'KQvKBB', '2b5/2b5/8/3k4/8/8/8/1Q2K3 w - - 0 1', 'b1b5 e1e2 e1f2'),
    ('breaker', 'win', 'KQvKBB', '1Q2K3/8/8/8/6b1/4b3/2k5/8 w - - 0 1', 'b8g3'),
    ('breaker', 'win', 'KQvKBB', '8/8/5Q2/7K/1k6/1b2b3/8/8 w - - 0 1', 'h5g4'),
    ('breaker', 'win', 'KQvKBB', '8/5K2/8/2b5/8/6Q1/2bk4/8 w - - 0 1', 'f7e6'),
    ('breaker', 'win', 'KQvKBB', '8/8/1K6/8/7Q/3k4/6b1/2b5 w - - 0 1', 'b6b5 h4d8 h4h7'),
    ('breaker', 'win', 'KQvKBB', '8/8/1Q4K1/3b4/8/8/6kb/8 w - - 0 1', 'b6b2'),
    ('breaker', 'win', 'KQvKNN', '2n5/4k3/1n4KQ/8/8/8/8/8 w - - 0 1', 'h6c1 h6e3 h6g5 h6g7 h6h4 h6h7'),
    ('breaker', 'win', 'KQvKNN', '3n4/5nk1/8/8/4Q3/8/5K2/8 w - - 0 1', 'e4c4 e4d4 e4e7 e4f5 e4g4 f2e2 f2e3 f2g3'),
    ('breaker', 'win', 'KQvKNN', '6K1/8/8/4k3/Q7/3n4/8/4n3 w - - 0 1', 'a4c4 a4g4'),
    ('breaker', 'win', 'KQvKNN', '7Q/k7/1n6/8/1n5K/8/8/8 w - - 0 1', 'h8c3 h8e5 h8e8'),
    ('breaker', 'win', 'KQvKNN', '6Q1/5K2/1n6/8/2n5/8/3k4/8 w - - 0 1', 'g8g3'),
    ('breaker', 'win', 'KQvKNN', 'Q7/8/8/4n3/8/K7/7n/6k1 w - - 0 1', 'a3b4'),
    ('breaker', 'win', 'KQvKNN', '8/8/8/8/7n/5n2/7k/QK6 w - - 0 1', 'a1g7'),
    ('breaker', 'win', 'KQvKNN', '8/8/K7/1Q6/7n/5n2/8/4k3 w - - 0 1', 'b5b2'),
    ('breaker', 'win', 'KQvKNN', '8/8/5Q2/2k4K/8/4n3/6n1/8 w - - 0 1', 'f6c3'),
    ('breaker', 'win', 'KQvKRB', '5Q2/8/8/8/2k5/K7/8/1b1r4 w - - 0 1', 'a3b2'),
    ('breaker', 'win', 'KQvKRB', '7b/6r1/8/8/3K4/8/3Q4/6k1 w - - 0 1', 'd2e1 d2e3'),
    ('breaker', 'win', 'KQvKRB', '5r2/8/1K6/8/8/3k4/b7/4Q3 w - - 0 1', 'e1g3'),
    ('breaker', 'win', 'KQvKRB', '8/8/7b/3Q4/4K3/8/8/2r3k1 w - - 0 1', 'd5h5'),
    ('breaker', 'win', 'KQvKRB', '7k/8/1K3b2/8/r7/8/6Q1/8 w - - 0 1', 'g2c6'),
    ('breaker', 'win', 'KQvKRN', '1Q6/K7/8/8/8/8/n4r2/6k1 w - - 0 1', 'b8b1 b8b5 b8d6 b8d8 b8e5'),
    ('breaker', 'win', 'KQvKRP', '6Q1/K7/3k4/8/8/8/3p3r/8 w - - 0 1', 'a7b6'),
    ('breaker', 'win', 'KRPvKN', 'K5k1/8/2R5/7P/8/7n/8/8 w - - 0 1', 'c6c4'),
    ('breaker', 'win', 'KRPvKR', '8/8/2R2K2/8/1P6/8/3k4/7r w - - 0 1', 'f6e6'),
    ('breaker', 'win', 'KRPvKR', 'R7/2K5/8/7P/8/5r2/8/7k w - - 0 1', 'a8d8'),
    ('breaker', 'win', 'KRvKB', 'k7/5b2/8/8/3K4/2R5/8/8 w - - 0 1', 'c3c7'),
    ('breaker', 'win', 'KRvKB', '3k4/8/b2KR3/8/8/8/8/8 w - - 0 1', 'e6e7'),
    ('breaker', 'win', 'KRvKN', '4R3/8/8/8/k7/7n/8/1K6 w - - 0 1', 'b1c2'),
    ('breaker', 'win', 'KRvKN', '2R5/8/8/7n/8/8/k7/5K2 w - - 0 1', 'f1e2 f1f2'),
    ('breaker', 'win', 'KRvKN', '5n2/1R6/8/8/7K/8/8/3k4 w - - 0 1', 'b7b6'),
    ('breaker', 'win', 'KRvKN', '8/3R4/6k1/8/8/8/6K1/2n5 w - - 0 1', 'g2f3'),
    ('breaker', 'win', 'KRvKN', '5k2/R7/8/5K2/1n6/8/8/8 w - - 0 1', 'f5e6'),
    ('breaker', 'draw', 'KBPvKB', '8/8/8/5PK1/b7/5B2/8/4k3 b - - 0 1', 'e1d2'),
    ('breaker', 'draw', 'KBPvKN', '7n/8/5P2/3K4/5B2/8/1k6/8 b - - 0 1', 'b2c2 b2c3'),
    ('breaker', 'draw', 'KNNvKP', 'k7/8/p1N5/8/8/8/N7/6K1 b - - 0 1', 'a6a5'),
    ('breaker', 'draw', 'KNNvKP', '8/4p3/8/8/7N/8/4k3/6KN b - - 0 1', 'e2d2 e2d3 e2e3 e7e5 e7e6'),
    ('breaker', 'draw', 'KPPvKR', '8/P7/1K6/8/1k5P/2r5/8/8 b - - 0 1', 'c3a3'),
    ('breaker', 'draw', 'KQPvKQ', 'K7/8/1P6/6q1/4Q3/8/5k2/8 b - - 0 1', 'g5a5 g5b5 g5c5 g5d2 g5h5'),
    ('breaker', 'draw', 'KQPvKQ', 'Q7/4q3/8/8/8/1k6/3K1P2/8 b - - 0 1', 'e7b4 e7d6 e7e6 e7g5'),
    ('breaker', 'draw', 'KQPvKQ', '4k3/2Q4P/8/8/8/8/7K/q7 b - - 0 1', 'a1a2 a1b2'),
    ('breaker', 'draw', 'KQPvKQ', '3q4/8/P1Q5/8/3k4/8/8/4K3 b - - 0 1', 'd8h4'),
    ('breaker', 'draw', 'KQPvKQ', 'k7/3q4/5P2/8/7Q/8/8/4K3 b - - 0 1', 'd7e6'),
    ('breaker', 'draw', 'KQvKBB', '8/8/8/2Q5/4b3/6b1/6k1/3K4 b - - 0 1', 'e4f3 g3f4 g3h4'),
    ('breaker', 'draw', 'KQvKBB', '7K/8/8/5Q2/8/8/1k6/b2b4 b - - 0 1', 'b2b3 b2c1'),
    ('breaker', 'draw', 'KQvKBB', 'Q7/5K2/8/8/8/8/2bb4/2k5 b - - 0 1', 'c1b2 c2b3 c2d3 d2c3'),
    ('breaker', 'draw', 'KQvKBB', '8/8/8/1K2b3/8/8/b1k5/7Q b - - 0 1', 'e5c3'),
    ('breaker', 'draw', 'KQvKBB', '8/1b6/4Q3/8/7b/8/K7/6k1 b - - 0 1', 'g1g2'),
    ('breaker', 'draw', 'KQvKNN', '8/8/2K4Q/3n4/3k4/8/5n2/8 b - - 0 1', 'f2e4'),
    ('breaker', 'draw', 'KQvKNN', '8/8/5n2/1k6/8/8/2n3KQ/8 b - - 0 1', 'c2e3'),
    ('breaker', 'draw', 'KQvKNN', '8/8/2k5/8/K1n5/3n4/7Q/8 b - - 0 1', 'c4b6 c4e5'),
    ('breaker', 'draw', 'KQvKNN', '1k6/7K/8/1n1n4/8/8/8/4Q3 b - - 0 1', 'b8c7 d5c7'),
    ('breaker', 'draw', 'KQvKNN', 'n7/k7/8/8/n7/8/3Q4/7K b - - 0 1', 'a8b6'),
    ('breaker', 'draw', 'KQvKNN', '6Q1/8/8/4n3/8/3k2K1/3n4/8 b - - 0 1', 'e5c4'),
    ('breaker', 'draw', 'KQvKNN', '4n3/3n4/7k/8/8/8/3K4/5Q2 b - - 0 1', 'e8f6'),
    ('breaker', 'draw', 'KQvKNN', '8/8/1nn5/5K2/8/4k3/8/Q7 b - - 0 1', 'c6d4'),
    ('breaker', 'draw', 'KQvKNN', '8/2Q5/2K5/8/8/2nk4/1n6/8 b - - 0 1', 'b2d1 c3e2 d3d2'),
    ('breaker', 'draw', 'KQvKNN', '8/2n5/8/8/2k2KQ1/2n5/8/8 b - - 0 1', 'c4d3'),
    ('breaker', 'draw', 'KQvKNN', '5nk1/8/7n/1K6/8/3Q4/8/8 b - - 0 1', 'g8f7 g8g7'),
    ('breaker', 'draw', 'KQvKNN', '8/2K3n1/8/6n1/8/3k4/7Q/8 b - - 0 1', 'g7f5'),
    ('breaker', 'draw', 'KQvKNN', '8/1n6/kn6/8/5K2/8/7Q/8 b - - 0 1', 'a6b5 b7a5'),
    ('breaker', 'draw', 'KQvKNN', '8/3n3k/Q7/8/5n2/8/8/1K6 b - - 0 1', 'd7f8'),
    ('breaker', 'draw', 'KQvKRB', '1b6/2r5/8/8/8/k7/3Q4/7K b - - 0 1', 'c7b7 c7h7'),
    ('breaker', 'draw', 'KQvKRB', '4r3/8/1b6/8/5Q2/8/8/1K5k b - - 0 1', 'b6e3'),
    ('breaker', 'draw', 'KQvKRB', '8/4k2K/8/7Q/8/8/8/1r2b3 b - - 0 1', 'b1b6'),
    ('breaker', 'draw', 'KQvKRN', '8/2Q5/5r2/5n2/8/k7/6K1/8 b - - 0 1', 'f5e3'),
    ('breaker', 'draw', 'KQvKRN', '4Q3/8/3r4/6n1/8/1k6/8/7K b - - 0 1', 'd6d1 d6d5'),
    ('breaker', 'draw', 'KQvKRN', '8/8/8/2K1k3/8/7r/2Q5/6n1 b - - 0 1', 'h3g3'),
    ('breaker', 'draw', 'KQvKRN', '7r/1Q6/8/8/5K1k/8/8/3n4 b - - 0 1', 'h8f8'),
    ('breaker', 'draw', 'KQvKRN', '8/1K3n2/8/7r/8/1k6/8/2Q5 b - - 0 1', 'f7d8'),
    ('breaker', 'draw', 'KRPvKN', '8/1R6/8/1P6/3k1K2/8/n7/8 b - - 0 1', 'a2b4 a2c3'),
    ('fooled', 'win', 'KBBvKN', '8/8/3K4/4B3/1k6/8/8/3n1B2 w - - 0 1', 'd6c6 e5a1 e5d4 e5f4 e5f6 e5g3 e5g7 e5h2 e5h8 f1a6 f1d3 f1e2 f1h3'),
    ('fooled', 'win', 'KBBvKN', '5B2/8/8/8/7n/1K6/5k2/3B4 w - - 0 1', 'b3c4 f8c5 f8e7 f8h6'),
    ('fooled', 'win', 'KBBvKN', '8/n7/8/8/8/6B1/1k6/5BK1 w - - 0 1', 'f1a6 f1e2 g1f2 g1g2 g1h1 g1h2 g3c7 g3e1 g3e5 g3f2 g3f4 g3h4'),
    ('fooled', 'win', 'KBBvKN', '8/1B6/5k1B/8/8/8/K7/4n3 w - - 0 1', 'a2b3'),
    ('fooled', 'win', 'KBBvKN', '8/8/8/8/8/3n4/B5K1/B2k4 w - - 0 1', 'a1d4 a1f6 a2b1 g2f3 g2g3'),
    ('fooled', 'win', 'KBNvKN', '8/3N1n2/B7/8/5K2/8/8/6k1 w - - 0 1', 'd7c5 d7f6 f4g3'),
    ('fooled', 'win', 'KBNvKN', '2K2n1k/5B2/8/5N2/8/8/8/8 w - - 0 1', 'c8d8'),
    ('fooled', 'win', 'KBNvKN', '8/k7/1n6/1K6/8/8/2N5/3B4 w - - 0 1', 'b5c6'),
    ('fooled', 'win', 'KBPvKB', '8/7k/8/2P4B/8/1b6/8/1K6 w - - 0 1', 'b1b2 c5c6 h5f3'),
    ('fooled', 'win', 'KBPvKN', '4B3/8/8/8/3P4/8/1k6/1n2K3 w - - 0 1', 'd4d5'),
    ('fooled', 'win', 'KBPvKN', '2B5/8/K7/8/4P3/8/8/1n2k3 w - - 0 1', 'e4e5'),
    ('fooled', 'win', 'KBPvKP', '7K/8/8/3kp3/B7/8/3P4/8 w - - 0 1', 'a4b5 a4d1'),
    ('fooled', 'win', 'KBPvKP', '7K/7P/8/8/8/8/2k3p1/4B3 b - - 0 1', 'g2g1q'),
    ('fooled', 'win', 'KNNvKP', '8/K4p1N/8/8/2k5/5N2/8/8 w - - 0 1', 'h7f6'),
    ('fooled', 'win', 'KNNvKP', '8/8/p7/8/8/6N1/4K3/1k3N2 w - - 0 1', 'e2d3 g3e4'),
    ('fooled', 'win', 'KNNvKP', '8/k7/6p1/8/4N3/5K2/8/7N w - - 0 1', 'e4g5'),
    ('fooled', 'win', 'KNNvKP', '8/7p/4N3/4K3/7k/8/2N5/8 w - - 0 1', 'c2e1 c2e3 e5e4 e5f4 e6d4 e6g7'),
    ('fooled', 'win', 'KNNvKP', '2K2k2/8/p7/8/8/8/2N2N2/8 w - - 0 1', 'c8d7 c8d8 f2e4'),
    ('fooled', 'win', 'KNNvKP', '8/N1k4p/8/3K4/3N4/8/8/8 w - - 0 1', 'd4e6 d4f3 d4f5'),
    ('fooled', 'win', 'KNPvKB', '8/6b1/3P4/8/8/K2N4/8/6k1 w - - 0 1', 'd6d7'),
    ('fooled', 'win', 'KNPvKB', '2N2K1b/8/8/1P6/8/8/8/k7 w - - 0 1', 'b5b6'),
    ('fooled', 'win', 'KNPvKB', '8/8/b6K/6P1/8/7k/8/4N3 w - - 0 1', 'g5g6'),
    ('fooled', 'win', 'KPPvKR', '8/P5P1/8/3r4/k7/8/8/7K b - - 0 1', 'd5d8'),
    ('fooled', 'win', 'KPPvKR', '8/8/8/7r/3P4/K3P3/8/k7 b - - 0 1', 'a1b1 h5b5'),
    ('fooled', 'win', 'KQPvKQ', '7K/8/8/8/2P3Q1/q7/k7/8 w - - 0 1', 'g4e2 g4g2'),
    ('fooled', 'win', 'KQPvKQ', '8/8/6P1/3q4/8/2Q5/4K1k1/8 w - - 0 1', 'c3f6 g6g7'),
    ('fooled', 'win', 'KQvKBB', '3Q4/8/8/8/8/bb5K/1k6/8 w - - 0 1', 'd8a5 d8d2 d8d4 d8f6 d8h8'),
    ('fooled', 'win', 'KQvKBB', '4Q3/8/8/2b5/6b1/7k/8/2K5 w - - 0 1', 'c1c2 c1d2 e8b8 e8c6 e8e1 e8e4 e8e5 e8f7 e8h8'),
    ('fooled', 'win', 'KQvKBB', '1b3K2/5Q2/8/8/8/7k/6b1/8 w - - 0 1', 'f7a2 f7b3 f7c4 f7d7 f7e6 f7e8 f7f2 f7f5 f7f6 f7g6 f7g7 f7g8 f7h5 f7h7 f8e7 f8e8 f8g7 f8g8'),
    ('fooled', 'win', 'KQvKBB', '8/8/8/K7/Q7/4bb2/7k/8 w - - 0 1', 'a4b3'),
    ('fooled', 'win', 'KQvKNP', '8/8/6KQ/8/8/8/2n1p3/5k2 w - - 0 1', 'h6f4'),
    ('fooled', 'win', 'KQvKRB', '8/6k1/4Q3/8/2K5/8/8/1b4r1 w - - 0 1', 'e6d7 e6e5 e6e7'),
    ('fooled', 'win', 'KQvKRB', '1b4k1/8/8/7Q/8/8/5r2/1K6 w - - 0 1', 'h5g4 h5g5 h5g6'),
    ('fooled', 'win', 'KQvKRB', '7r/4k2b/8/8/8/8/4K3/5Q2 w - - 0 1', 'f1h3'),
    ('fooled', 'win', 'KQvKRN', '8/5r2/8/Q3K3/5n2/5k2/8/8 w - - 0 1', 'a5a8'),
    ('fooled', 'win', 'KQvKRN', '2k5/3r4/6n1/8/8/2K2Q2/8/8 w - - 0 1', 'f3a8'),
    ('fooled', 'win', 'KQvKRN', '3r2k1/4n3/8/6K1/8/8/1Q6/8 w - - 0 1', 'g5f6'),
    ('fooled', 'win', 'KRBvKR', '8/7k/1B3K2/8/8/3R4/4r3/8 w - - 0 1', 'b6c5 b6e3 d3d7 d3h3 f6f7'),
    ('fooled', 'win', 'KRBvKR', '8/8/8/8/6r1/8/1R1B1K2/7k w - - 0 1', 'f2f3'),
    ('fooled', 'win', 'KRvKB', 'k7/7b/8/8/2K3R1/8/8/8 w - - 0 1', 'c4b5 c4c5'),
    ('fooled', 'win', 'KRvKB', '8/8/7k/R3K3/8/7b/8/8 w - - 0 1', 'e5f6'),
    ('fooled', 'win', 'KRvKB', '5k2/8/8/3b1KR1/8/8/8/8 w - - 0 1', 'f5f6'),
    ('fooled', 'win', 'KRvKN', '8/8/5R2/7k/8/6K1/1n6/8 w - - 0 1', 'f6b6 f6d6 f6f2 f6f4 f6f5'),
    ('fooled', 'win', 'KRvKN', '6k1/8/8/8/8/4n1K1/3R4/8 w - - 0 1', 'g3f4'),
    ('fooled', 'win', 'KRvKPP', '7k/K7/8/8/4p3/5p2/8/3R4 w - - 0 1', 'd1f1'),
    ('fooled', 'win', 'KRvKPP', '8/8/7p/1K4p1/8/6k1/8/5R2 w - - 0 1', 'b5c4'),
    ('fooled', 'draw', 'KBPPvK', '8/1k6/8/2B5/PK6/8/P7/8 b - - 0 1', 'b7a6 b7a8 b7b8 b7c6 b7c7 b7c8'),
]

problems = 0; cases = 0; notes = []


def problem(section, name, msg):
    global problems
    problems += 1; print("PROBLEM [%s] %s: %s" % (section, name, str(msg)[:300]), flush=True)


def note(msg):
    if msg not in notes: notes.append(msg); print("NOTE " + msg, flush=True)


class Eng:
    def __init__(self): self.start()

    def start(self):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1, errors="replace")
        self.out = []; self.err = []; self.c960 = False
        threading.Thread(target=lambda p=self.p, o=self.out: [o.append(l.rstrip()) for l in p.stdout], daemon=True).start()
        threading.Thread(target=lambda p=self.p, e=self.err: [e.append(l.rstrip()) for l in p.stderr], daemon=True).start()
        self.send("uci")
        if THREADS: self.option("Threads", THREADS)
        if SYZYGY: self.option("SyzygyPath", SYZYGY)
        self.send("isready")
        if not self.wait(lambda: "readyok" in self.out, 120): raise RuntimeError("the engine did not answer isready at start")
        if SYZYGY and not any("tablebases up to" in l for l in self.out): raise RuntimeError("the engine found no tablebase files in " + SYZYGY)

    def send(self, s):
        try: self.p.stdin.write(s + "\n"); self.p.stdin.flush(); return True
        except Exception: return False

    def alive(self): return self.p.poll() is None

    def wait(self, pred, t):
        end = time.time() + t
        while time.time() < end:
            if pred(): return True
            if not self.alive(): return pred()
            time.sleep(0.002)
        return pred()

    def go(self, cmd, t=None):
        """returns (finished, lines after the command); a search that does not end in time is stopped"""
        m = len(self.out); self.send(cmd)
        done = lambda: any(l.startswith("bestmove") or l.startswith("Nodes searched") for l in self.out[m:])
        ok = self.wait(done, t or TMO)
        if not ok and self.alive():
            self.send("stop"); ok = self.wait(done, 10)
            return False, self.out[m:]
        return ok, self.out[m:]

    def position(self, fen, moves=None):
        self.send("position " + ("startpos" if fen is None else "fen " + fen) + ((" moves " + " ".join(moves)) if moves else ""))

    def set960(self, flag):
        if flag != self.c960: self.send("setoption name UCI_Chess960 value " + ("true" if flag else "false")); self.c960 = flag

    def option(self, name, value): self.send("setoption name %s value %s" % (name, value))

    def check(self, section, name):
        """the process must be alive, answer isready and have printed nothing on stderr; otherwise it is restarted"""
        global cases
        cases += 1
        bad = [l for l in self.err if l.strip()]
        if not self.alive():
            problem(section, name, "the engine exited with code %s" % self.p.returncode); self.start(); return False
        m = sum(1 for l in self.out if l == "readyok"); self.send("isready")
        if not self.wait(lambda: sum(1 for l in self.out if l == "readyok") > m, 20):
            problem(section, name, "no readyok afterwards"); self.p.kill(); self.start(); return False
        if bad:
            problem(section, name, "stderr: " + " | ".join(bad[:2])); del self.err[:]; return False
        return True

    def close(self):
        self.send("quit"); self.wait(lambda: not self.alive(), 10)
        if self.alive(): self.p.kill()


def bestmove(lines):
    for l in lines:
        if l.startswith("bestmove"):
            t = l.split(); return t[1] if len(t) > 1 else ""
    return None


def nodes_of(lines):
    for l in lines:
        m = re.match(r"Nodes searched:\s*(\d+)", l)
        if m: return int(m.group(1))
    return None


def is_legal(board, bm):
    if bm is None: return False
    if bm in ("0000", "(none)"): return not any(board.legal_moves)
    try: return chess.Move.from_uci(bm) in board.legal_moves
    except Exception: return False


def mates_of(b):
    out = []
    for m in list(b.legal_moves):
        b.push(m); mt = b.is_checkmate(); b.pop()
        if mt: out.append(m.uci())
    return out


def stalemating_of(b):
    out = []
    for m in list(b.legal_moves):
        b.push(m); st = b.is_stalemate(); b.pop()
        if st: out.append(m.uci())
    return out


def final_score(lines):
    """('cp' | 'mate', value) of the last info line of a search"""
    last = None
    for l in lines:
        m = re.search(r" score (cp|mate) (-?\d+)", l)
        if l.startswith("info") and " depth " in l and m: last = (m.group(1), int(m.group(2)))
    return last


def winning_score(lines):
    """a forced mate, or a tablebase win (about 28000 minus the ply)"""
    s = final_score(lines)
    return s is not None and ((s[0] == "mate" and s[1] > 0) or (s[0] == "cp" and s[1] >= 27000))


def py_perft(b, d):
    if d == 1: return b.legal_moves.count()
    n = 0
    for m in list(b.legal_moves):
        b.push(m); n += py_perft(b, d - 1); b.pop()
    return n


def search_ok(E, sec, name, board, cmds):
    """every command must end in a legal bestmove for `board`"""
    for g in cmds:
        ok, lines = E.go(g)
        bm = bestmove(lines)
        if not ok: problem(sec, name, "%s: no bestmove within %.0f s" % (g, TMO))
        elif not is_legal(board, bm): problem(sec, name, "%s: bestmove %s is not legal in %s" % (g, bm, board.fen()))
    E.check(sec, name)


# ------------------------------------------------------------------------------------------------------------------------------------ perft
def sec_perft(E):
    for name, fen, c960, counts in PERFT:
        if "--selfcheck" in ARGS:
            b = chess.Board(fen, chess960=c960)
            for d in (1, 2, 3):
                if py_perft(b, d) != counts[d]: problem("perft", name, "the table is wrong at depth %d (python-chess says %d, the table %d)" % (d, py_perft(b, d), counts[d]))
        E.set960(c960); E.position(fen)
        for d, n in sorted(counts.items()):
            if QUICK and (n > 300000 or d > 4): continue
            if n > 10 ** 7: continue
            ok, lines = E.go("go perft %d" % d)
            got = nodes_of(lines)
            if got != n: problem("perft", name, "depth %d: %s nodes, expected %d" % (d, got, n))
        E.check("perft", name)
        board = chess.Board(fen, chess960=c960)
        search_ok(E, "perft", name + " (search)", board, ["go depth 4" if QUICK else "go depth 5", "go nodes 30000"])
    E.set960(False)


# ------------------------------------------------------------------------------------------------------------------------------------ mate
def sec_mate(E):
    for cls, fen in MATE:
        b = chess.Board(fen); mates = mates_of(b)
        name = "%s %s" % (cls, fen)
        if not mates: problem("mate", name, "TABLE ERROR: python-chess finds no mate in one"); continue
        stale = stalemating_of(b)
        E.send("ucinewgame"); E.position(fen)
        for g in ("go depth 1", "go depth 2", "go depth 6", "go nodes 3000", "go movetime 60"):
            ok, lines = E.go(g); bm = bestmove(lines)
            if not ok: problem("mate", name, "%s: no bestmove" % g)
            elif bm in stale: problem("mate", name, "%s: played %s, which stalemates the opponent" % (g, bm))
            elif bm not in mates and not (SYZYGY and winning_score(lines)): problem("mate", name, "%s: played %s, the mate is %s" % (g, bm, "/".join(mates)))
            # with tablebases a mate in one whose position is in the tables scores as a tablebase win (below every mate score): the score is not 'mate 1' there and a slower mate can be preferred
            if g in ("go depth 2", "go depth 6") and ok and not SYZYGY and not any(re.search(r"score mate 1\b", l) for l in lines if l.startswith("info") and " depth " in l):
                problem("mate", name, "%s: the score is not 'mate 1'" % g)
        E.check("mate", name)
    for fen in STALE_AVOID:
        b = chess.Board(fen); stale = []
        for mv in list(b.legal_moves):
            b.push(mv); st = b.is_stalemate(); b.pop()
            if st: stale.append(mv.uci())
        if not stale or mates_of(b): problem("mate", "stalemate blunder " + fen, "TABLE ERROR: no stalemating move, or a mate in one"); continue
        E.send("ucinewgame"); E.position(fen)
        for g in ("go depth 2", "go depth 6", "go nodes 3000", "go movetime 60"):
            ok, lines = E.go(g); bm = bestmove(lines)
            if not ok: problem("mate", "stalemate blunder " + fen, "%s: no bestmove" % g)
            elif bm in stale: problem("mate", "stalemate blunder " + fen, "%s: played %s, which stalemates the opponent in a won position" % (g, bm))
        E.check("mate", "stalemate blunder " + fen)


# ------------------------------------------------------------------------------------------------------------------------------------ forced
def multipv_lines(lines):
    """{multipv index: first move} of the deepest iteration that printed all its lines"""
    best = {}; depth = 0
    for l in lines:
        m = re.match(r"info depth (\d+) .*?multipv (\d+) .*? pv (\S+)", l)
        if m:
            d = int(m.group(1))
            if d > depth: depth = d; best = {}
            if d == depth: best[int(m.group(2))] = m.group(3)
    return best


def sec_forced(E):
    for name, fen in ONE_LEGAL:
        b = chess.Board(fen); only = [m.uci() for m in b.legal_moves]
        if len(only) != 1: problem("forced", name, "TABLE ERROR: %d legal moves" % len(only)); continue
        E.position(fen)
        for g in ("go depth 1", "go depth 6", "go nodes 1000", "go movetime 50"):
            ok, lines = E.go(g); bm = bestmove(lines)
            if bm != only[0]: problem("forced", name + " " + fen, "%s: played %s, the only move is %s" % (g, bm, only[0]))
        E.option("MultiPV", 8); E.position(fen); ok, lines = E.go("go depth 6"); mp = multipv_lines(lines)
        if mp and list(mp.values()) != only: problem("forced", name + " " + fen, "MultiPV 8 with one legal move printed %s" % mp)
        E.option("MultiPV", 1)
        E.check("forced", name)
    for name, fen in NO_LEGAL:
        b = chess.Board(fen)
        if any(b.legal_moves): problem("forced", name, "TABLE ERROR: there are legal moves"); continue
        E.position(fen)
        for g in ("go depth 1", "go depth 8", "go nodes 5000", "go movetime 50", "go perft 3"):
            ok, lines = E.go(g)
            if g == "go perft 3":
                if nodes_of(lines) != 0: problem("forced", name, "perft 3 gave %s nodes, expected 0" % nodes_of(lines))
            elif bestmove(lines) not in ("0000", "(none)"): problem("forced", name, "%s: bestmove %s, expected 0000 or (none)" % (g, bestmove(lines)))
        E.check("forced", name)
    # MultiPV larger than the number of legal moves
    for nlegal, fen in (FEW[::3] if QUICK else FEW):
        b = chess.Board(fen); legal = {m.uci() for m in b.legal_moves}
        if len(legal) != nlegal: problem("forced", "MultiPV " + fen, "TABLE ERROR: %d legal moves, not %d" % (len(legal), nlegal)); continue
        for k in (8, 5):
            E.option("MultiPV", k); E.position(fen); ok, lines = E.go("go depth 6"); mp = multipv_lines(lines); bm = bestmove(lines)
            n = min(k, len(legal))
            if mp and (len(mp) != n or set(mp.values()) - legal or len(set(mp.values())) != len(mp)): problem("forced", "MultiPV %d, %d legal moves" % (k, len(legal)), "%s | %s" % (fen, mp))
            elif mp and mp.get(1) != bm: problem("forced", "MultiPV %d, %d legal moves" % (k, len(legal)), "bestmove %s is not the first line %s | %s" % (bm, mp.get(1), fen))
        E.option("MultiPV", 1)
        E.check("forced", "MultiPV " + fen)


# ------------------------------------------------------------------------------------------------------------------------------------ limits
def sec_limits(E):
    cyc = "g1f3 g8f6 f3g1 f6g8".split()
    # a long game: the moves are applied up to ply 3,900 (the engine keeps 4,096 plies of history and searches up to 128 plies beyond); the last moves must be the ones played
    for plies in (3880,):
        mv = cyc * (plies // 4) + "e2e4 e7e5 g1f3 b8c6".split()
        b = chess.Board()
        for m in mv: b.push_uci(m)
        E.position(None, mv); search_ok(E, "limits", "game of %d plies" % len(mv), b, ["go depth 8", "go movetime 100"])
    for plies in (3960, 4500, 20000):
        mv = cyc * (plies // 4) + "e2e4 e7e5 g1f3 b8c6".split()
        b = chess.Board()
        for m in mv: b.push_uci(m)
        E.position(None, mv); ok, lines = E.go("go depth 8"); bm = bestmove(lines)
        if not ok or bm is None: problem("limits", "game of %d plies" % len(mv), "no bestmove")
        elif not is_legal(b, bm): note("a move list beyond ply 3,900 is cut there (known limit): after %d plies the engine searched an earlier position and played %s" % (len(mv), bm))
        E.check("limits", "game of %d plies" % len(mv))
    # numbers at the limits in the FEN
    base = "4k3/8/8/8/8/8/4P3/R3K3 w Q - %s %s"
    for hm in ("0", "1", "49", "99", "100", "101", "255", "256", "32767", "32768", "65535", "65536", "2147483647", "2147483648", "4294967295", "4294967296", "18446744073709551616"):
        for fm in ("1", "0", "32768", "65536", "2147483647", "4294967296", "18446744073709551616"):
            if QUICK and hm not in ("0", "100", "65536", "4294967296", "18446744073709551616"): continue
            fen = base % (hm, fm); b = chess.Board(base % ("0", "1"))               # the same position with small counters (python-chess is not asked to parse the huge ones)
            E.position(fen); ok, lines = E.go("go depth 5"); bm = bestmove(lines)
            if not ok: problem("limits", "counters %s %s" % (hm, fm), "no bestmove")
            elif int(hm) < 100 and not is_legal(b, bm): problem("limits", "counters %s %s" % (hm, fm), "bestmove %s is not legal" % bm)
            elif int(hm) >= 100 and bm not in ("0000", "(none)") and not is_legal(b, bm): problem("limits", "counters %s %s" % (hm, fm), "bestmove %s is not legal" % bm)
            E.check("limits", "counters %s %s" % (hm, fm))
    # Chess960 castling flags and file letters in a normal game, castling flags for pieces that are not there, repeated flags
    for fen in ("r3k2r/8/8/8/8/8/8/R3K2R w HAha - 0 1", "r3k2r/8/8/8/8/8/8/R3K2R w KKQQkkqq - 0 1", "4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1", "r3k2r/8/8/8/8/8/8/R3K2R w - - 0 1",
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e3 0 1", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e4 0 1"):
        E.position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")                       # a legal position first: it must survive what follows
        E.position(fen); ok, lines = E.go("go depth 4")
        if not ok or bestmove(lines) is None: problem("limits", "odd flags " + fen, "no bestmove")
        E.check("limits", "odd flags " + fen)
    # everything at once: 8 threads, a tiny hash, MultiPV 8, a position with 218 moves
    E.option("Threads", 8); E.option("Hash", 1); E.option("MultiPV", 8)
    fen = "R6R/3Q4/1Q4Q1/4Q3/2Q4Q/Q4Q2/pp1Q4/kBNN1KB1 w - - 0 1"; E.position(fen)
    search_ok(E, "limits", "218 moves, 8 threads, 1 MB hash, MultiPV 8", chess.Board(fen), ["go depth 6", "go movetime 300"])
    E.option("Threads", 1); E.option("Hash", 64); E.option("MultiPV", 1)


# ------------------------------------------------------------------------------------------------------------------------------------ deep
def soup(rnd):
    while True:
        b = chess.Board(None)
        sq = rnd.sample(list(chess.SQUARES), 12)
        b.set_piece_at(sq[0], chess.Piece(chess.KING, True)); b.set_piece_at(sq[1], chess.Piece(chess.KING, False))
        for s in sq[2:2 + rnd.randint(3, 8)]: b.set_piece_at(s, chess.Piece(rnd.choice([chess.QUEEN, chess.QUEEN, chess.ROOK, chess.BISHOP, chess.KNIGHT]), rnd.random() < 0.5))
        b.turn = rnd.random() < 0.5
        if b.is_valid() and not b.is_game_over() and not b.is_check(): return b.fen()


def sec_deep(E):
    rnd = random.Random(SEED + 23)
    for i in range(4 if QUICK else 24):
        fen = soup(rnd); b = chess.Board(fen); E.position(fen)
        search_ok(E, "deep", "queen soup %d %s" % (i, fen), b, ["go movetime 300"] if QUICK else ["go movetime 300", "go depth 14"])


# ------------------------------------------------------------------------------------------------------------------------------------ fuzz
def random_walk(rnd):
    b = chess.Board()
    for _ in range(rnd.randint(0, 120)):
        ms = list(b.legal_moves)
        if not ms: break
        b.push(rnd.choice(ms))
    return b


def random_placement(rnd):
    while True:
        b = chess.Board(None); sq = rnd.sample(list(chess.SQUARES), rnd.randint(2, 32))
        b.set_piece_at(sq[0], chess.Piece(chess.KING, True)); b.set_piece_at(sq[1], chess.Piece(chess.KING, False))
        for s in sq[2:]:
            kinds = [chess.QUEEN, chess.ROOK, chess.BISHOP, chess.KNIGHT] + ([chess.PAWN] * 3 if chess.square_rank(s) not in (0, 7) else [])
            b.set_piece_at(s, chess.Piece(rnd.choice(kinds), rnd.random() < 0.5))
        b.turn = rnd.random() < 0.5
        if b.is_valid() and not b.is_game_over(): return b


def random_few(rnd):
    """3 to 6 pieces: endings (the range of the tablebases)"""
    while True:
        b = chess.Board(None); sq = rnd.sample(list(chess.SQUARES), rnd.randint(3, 6))
        b.set_piece_at(sq[0], chess.Piece(chess.KING, True)); b.set_piece_at(sq[1], chess.Piece(chess.KING, False))
        for s in sq[2:]:
            kinds = [chess.QUEEN, chess.ROOK, chess.BISHOP, chess.KNIGHT] + ([chess.PAWN] * 3 if chess.square_rank(s) not in (0, 7) else [])
            b.set_piece_at(s, chess.Piece(rnd.choice(kinds), rnd.random() < 0.5))
        b.turn = rnd.random() < 0.5; b.halfmove_clock = rnd.choice([0, 0, 0, 40, 99, 100])
        if b.is_valid() and not b.is_game_over(): return b


def sec_fuzz(E):
    rnd = random.Random(SEED + 37); n = 40 if QUICK else 300; seen = set()
    for i in range(n):
        b = (random_walk, random_placement, random_few)[i % 3](rnd)
        fen = b.fen()
        if fen in seen or not b.is_valid(): continue
        seen.add(fen); E.position(fen)
        for d in (1, 2, 3):
            if d == 3 and (py_perft(b, 2) > 2500 or QUICK and py_perft(b, 2) > 900): continue
            want = py_perft(b, d); ok, lines = E.go("go perft %d" % d); got = nodes_of(lines)
            if got != want: problem("fuzz", fen, "perft %d: %s nodes, python-chess %d" % (d, got, want))
        search_ok(E, "fuzz", fen, b, ["go nodes 4000", "go depth 3"])


# ------------------------------------------------------------------------------------------------------------------------------------ tb
def tb_position(rnd):
    """3 to 5 men with the things that trip up tablebase code: an en passant capture, a castling right, a fifty-move counter near the limit"""
    b = chess.Board(None); kind = rnd.choice(["plain", "ep", "ep", "castle"])

    def free():
        return rnd.choice([s for s in chess.SQUARES if not b.piece_at(s)])
    if kind == "ep":
        f = rnd.randrange(8); wf = f + rnd.choice([-1, 1])
        if not 0 <= wf < 8: return None
        b.turn = rnd.random() < 0.5
        if b.turn:                                           # black has just played f7-f5
            b.set_piece_at(chess.square(f, 4), chess.Piece(chess.PAWN, False)); b.set_piece_at(chess.square(wf, 4), chess.Piece(chess.PAWN, True)); b.ep_square = chess.square(f, 5)
        else:                                                # white has just played f2-f4
            b.set_piece_at(chess.square(f, 3), chess.Piece(chess.PAWN, True)); b.set_piece_at(chess.square(wf, 3), chess.Piece(chess.PAWN, False)); b.ep_square = chess.square(f, 2)
        b.set_piece_at(free(), chess.Piece(chess.KING, True)); b.set_piece_at(free(), chess.Piece(chess.KING, False)); extra = rnd.randint(0, 1)
    elif kind == "castle":
        side = rnd.choice("KQ"); b.set_piece_at(chess.E1, chess.Piece(chess.KING, True)); b.set_piece_at(chess.H1 if side == "K" else chess.A1, chess.Piece(chess.ROOK, True))
        b.castling_rights = chess.BB_H1 if side == "K" else chess.BB_A1; b.set_piece_at(free(), chess.Piece(chess.KING, False)); b.turn = rnd.random() < 0.5; extra = rnd.randint(0, 2)
    else:
        b.set_piece_at(free(), chess.Piece(chess.KING, True)); b.set_piece_at(free(), chess.Piece(chess.KING, False)); b.turn = rnd.random() < 0.5; extra = rnd.randint(1, 3)
    for _ in range(extra):
        s = free(); pt = rnd.choice([chess.PAWN, chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN])
        if pt == chess.PAWN and chess.square_rank(s) in (0, 7): continue
        b.set_piece_at(s, chess.Piece(pt, rnd.random() < 0.5))
    b.halfmove_clock = rnd.choice([0, 0, 0, 3, 49, 50, 99, 100, 101])
    if not (b.is_valid() and not b.is_game_over()) or len(b.piece_map()) > 5: return None
    return b


def sec_tb(E):
    import chess.syzygy
    if not SYZYGY: problem("tb", "setup", "needs --syzygy FOLDER (3-4-5 men files)"); return
    tb = chess.syzygy.open_tablebase(SYZYGY); rnd = random.Random(SEED + 51); done = 0; seen = set(); rooted = 0
    cls = lambda w: 1 if w == 2 else (-1 if w == -2 else 0)
    while done < (60 if QUICK else 400):
        b = tb_position(rnd)
        if b is None or b.fen() in seen: continue
        seen.add(b.fen()); fen = b.fen(); done += 1; E.position(fen)
        m = len(E.out); E.send("tbprobe"); E.send("isready"); E.wait(lambda: any(l == "readyok" for l in E.out[m:]), 30); lines = E.out[m:]
        wdl = None; kept = None
        for l in lines:
            if l.startswith("tbprobe: wdl"): wdl = int(l.split()[-1])
            if l.startswith("tbroot: kept"): kept = l.split()[2:]
        legal = {mv.uci() for mv in b.legal_moves}
        if b.castling_rights:
            if wdl is not None: problem("tb", fen, "a position with castling rights is probed (wdl %s)" % wdl)
        else:
            try: ref = tb.probe_wdl(b)
            except Exception: ref = None
            if ref is not None and wdl is None: problem("tb", fen, "python-chess finds the position in the tablebases (wdl %d), the engine says: %s" % (ref, [l for l in lines if l.startswith("tbprobe")]))
            elif ref is not None and cls(ref) != cls(wdl): problem("tb", fen, "wdl: engine %s, python-chess %s (win/draw/loss class %d against %d)" % (wdl, ref, cls(wdl), cls(ref)))
        if kept is not None:
            rooted += 1
            if not set(kept) <= legal: problem("tb", fen, "the root filter keeps moves that are not legal: %s" % sorted(set(kept) - legal))
        for g in ("go depth 6", "go nodes 20000"):
            ok, lines = E.go(g); bm = bestmove(lines)
            if not ok: problem("tb", fen, "%s: no bestmove" % g)
            elif not is_legal(b, bm): problem("tb", fen, "%s: bestmove %s is not legal" % (g, bm))
            elif kept is not None and bm not in kept and bm not in ("0000", "(none)"): problem("tb", fen, "%s: bestmove %s is not among the kept moves %s" % (g, bm, kept))
        E.check("tb", fen)
    print("tb: %d positions, %d of them at the root in the tablebases" % (done, rooted), flush=True)


# ------------------------------------------------------------------------------------------------------------------------------------ eval
def sec_eval(E):
    """3 to 5 men where an engine without tablebases goes wrong; the table EVAL holds the truth"""
    nodes = 50000 if QUICK else 200000                       # the budgets the statuses were measured at
    noisy = THREADS is not None and int(THREADS) > 1         # several threads: the search is not repeatable
    n = {"control": [0, 0], "breaker": [0, 0], "fooled": [0, 0]}     # [positions, the engine plays right (and for 'fooled': the score is right too)]
    for i, (status, kind, sig, fen, keepers) in enumerate(EVAL):
        if QUICK and i % 3: continue
        keep = set(keepers.split()); b = chess.Board(fen); label = "%s %s %s" % (kind, sig, fen)
        E.send("ucinewgame"); E.position(fen); ok, lines = E.go("go nodes %d" % nodes); bm = bestmove(lines); sc = final_score(lines)
        if not ok or not is_legal(b, bm):
            problem("eval", label, "no legal bestmove (%s)" % bm); E.check("eval", label); continue
        right = bm in keep
        shown = "none" if sc is None else ("mate %d" % sc[1] if sc[0] == "mate" else "%+.2f" % (sc[1] / 100.0))
        if status == "fooled":
            cp = None if sc is None else (sc[1] if sc[0] == "cp" else (30000 - abs(sc[1])) * (1 if sc[1] > 0 else -1))
            right = right and cp is not None and (abs(cp) < 300 if kind == "draw" else cp > 100)       # the score must be right as well
        n[status][0] += 1; n[status][1] += 1 if right else 0
        if SYZYGY:                                                    # in the tables every position is played right (and the score is the tablebase score)
            if bm not in keep: problem("eval", label, "with the tablebases it played %s; the moves that keep the %s are %s" % (bm, "win" if kind == "win" else "draw", " ".join(sorted(keep))))
        elif status == "control" and not right:
            msg = "played %s (score %s) at %d nodes; the moves that keep the %s are %s" % (bm, shown, nodes, "win" if kind == "win" else "draw", " ".join(sorted(keep)))
            ok5, lines5 = E.go("go nodes %d" % (nodes * 5)); bm5 = bestmove(lines5)      # another platform can differ in the last bit of the network: five times the nodes decide
            if noisy: note("eval, control %s %s: %s (several threads: not repeatable, not counted)" % (sig, fen, msg)); n["control"][1] += 1
            elif ok5 and bm5 in keep: note("eval, control %s %s: %s; right at %d nodes (not counted as a problem)" % (sig, fen, msg, nodes * 5)); n["control"][1] += 1
            else: problem("eval", label, msg + "; at %d nodes it played %s" % (nodes * 5, bm5))
        elif LIST and status != "control" and not right:
            print("WEAK %-7s %-7s %s  played %s, score %s; the %s is kept by %s" % (status, sig, fen, bm, shown, kind, " ".join(sorted(keep))), flush=True)
        E.check("eval", label)
    if SYZYGY: print("eval: with the tablebases every position is played right (%d positions)" % sum(v[0] for v in n.values()), flush=True)
    else:
        print("eval: controls played right %d of %d; breakers solved %d of %d; fooled positions now scored right %d of %d (without tablebases; the breakers and the fooled positions are weak spots, not problems)"
              % (n["control"][1], n["control"][0], n["breaker"][1], n["breaker"][0], n["fooled"][1], n["fooled"][0]), flush=True)


# ------------------------------------------------------------------------------------------------------------------------------------ run
t0 = time.time(); E = Eng()
for sec, fn in (("perft", sec_perft), ("mate", sec_mate), ("forced", sec_forced), ("limits", sec_limits), ("deep", sec_deep), ("fuzz", sec_fuzz), ("eval", sec_eval), ("tb", sec_tb)):
    if sec not in ONLY: continue
    p0 = problems; c0 = cases; t1 = time.time()
    try: fn(E)
    except Exception as ex:
        problem(sec, "harness", repr(ex)); E.close(); E = Eng()
    print("%-7s %4d checks, %d problems, %4.0f s" % (sec, cases - c0, problems - p0, time.time() - t1), flush=True)
E.close()
print("breakers: %s  (%d checks, %.0f s%s)" % ("PASS" if problems == 0 else "FAIL (%d problems)" % problems, cases, time.time() - t0, ", quick" if QUICK else ""))
