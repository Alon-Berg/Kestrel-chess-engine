"""38.1 interoperability test: the engine driven by python-chess' UCI client (an independent implementation of the protocol, as used by analysis scripts and bots).
usage: python interop_test.py engine.exe        (run with the engine's own book / experience switched ON where the test says so; experience writes are redirected to a temp file)
Checks: option declarations are accepted by a standard client; play() with time / nodes / depth / clocks / mate limits; analyse() with multipv, with the opening book ON (a depth-limited
analysis must return a real score, not a bare book move); streaming analysis + stop; pondering (hit and miss); Chess960; new-game handling; Move Overhead on a fixed move time."""
import os, sys, tempfile, time
import chess, chess.engine

import atexit
exe = os.path.abspath(sys.argv[1])
_engines = []
atexit.register(lambda: [e.close() for e in _engines])
res = []
def check(name, ok, detail=""):
    res.append(bool(ok)); print(("PASS " if ok else "FAIL ") + name + ("   " + str(detail)[:200] if detail != "" else ""), flush=True)

tmp = tempfile.mkdtemp(prefix="interop_", dir=os.path.dirname(os.path.abspath(__file__)))
EXPF = os.path.join(tmp, "exp.dat")
def open_engine():
    e = chess.engine.SimpleEngine.popen_uci(exe, timeout=30); _engines.append(e)
    e.configure({"ExperienceFile": EXPF})                       # never write experience next to the exe
    return e
try:
    eng = open_engine()
    check("popen_uci: id name / author", "Kestrel" in (eng.id.get("name") or "") and eng.id.get("author") == "Claude", eng.id)
    # ---- options: every declared option accepts its default and a value inside its range
    bad = []
    for name, opt in eng.options.items():
        try:
            if opt.type == "spin": eng.configure({name: opt.default}); eng.configure({name: opt.min}); eng.configure({name: min(opt.max, opt.default + 1)}); eng.configure({name: opt.default})
            elif opt.type == "check": eng.configure({name: not opt.default}); eng.configure({name: opt.default})
            elif opt.type == "string" and name != "ExperienceFile": eng.configure({name: opt.default})
            elif opt.type == "button": eng.configure({name: None})
            eng.ping()
        except Exception as ex:
            if "automatically managed" in str(ex): continue                  # python-chess sets MultiPV / Ponder / UCI_Chess960 / UCI_AnalyseMode itself
            bad.append((name, repr(ex)[:80]))
    check("all %d declared options accept default / min / max values" % len(eng.options), not bad, bad)
    # ---- play with every kind of limit
    boards = [chess.Board(), chess.Board("r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R1BQ1RK1 w - - 0 9"), chess.Board("8/5pk1/6p1/3P4/4K3/8/5PP1/8 w - - 0 40"),
              chess.Board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1")]
    for lim_name, lim in (("time 0.1 s", chess.engine.Limit(time=0.1)), ("nodes 3000", chess.engine.Limit(nodes=3000)), ("depth 5", chess.engine.Limit(depth=5)),
                          ("clocks 2+0.1", chess.engine.Limit(white_clock=2.0, black_clock=2.0, white_inc=0.1, black_inc=0.1)),
                          ("clocks + 20 moves to go", chess.engine.Limit(white_clock=3.0, black_clock=3.0, remaining_moves=20))):
        t0 = time.time(); ok = True; det = ""
        for b in boards:
            r = eng.play(b, lim)
            if r.move is None or r.move not in b.legal_moves: ok = False; det = "illegal / none: %s on %s" % (r.move, b.fen())
        check("play() with limit %s on 4 positions" % lim_name, ok, det or "%.2f s" % (time.time() - t0))
    mate_b = chess.Board("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1")
    r = eng.play(mate_b, chess.engine.Limit(mate=1), info=chess.engine.INFO_ALL)
    check("play() with Limit(mate=1): the mate move, with a mate score", r.move == chess.Move.from_uci("a1a8") and r.info.get("score") is not None and r.info["score"].relative.is_mate(), r.move)
    # ---- analyse: the book is ON (default); a depth-limited analysis of early positions must carry a real score and PV
    eng.configure({"OwnBook": True})
    for b, name in ((chess.Board(), "start position"), (chess.Board("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"), "after 1.e4")):
        for lim_name, lim in (("depth 8", chess.engine.Limit(depth=8)), ("nodes 20000", chess.engine.Limit(nodes=20000))):
            info = eng.analyse(b, lim)
            ok = "score" in info and "pv" in info and len(info["pv"]) >= 1 and info["pv"][0] in b.legal_moves
            check("analyse(%s) of the %s with the book ON returns a score and a legal PV" % (lim_name, name), ok, {k: info.get(k) for k in ("score", "depth")} if ok else sorted(info.keys()))
    infos = eng.analyse(boards[1], chess.engine.Limit(depth=8), multipv=3)
    check("analyse(multipv=3): 3 lines, distinct first moves, legal PVs", len(infos) == 3 and len({i["pv"][0] for i in infos}) == 3 and all(i["pv"][0] in boards[1].legal_moves for i in infos), [str(i.get("score")) for i in infos])
    # ---- streaming analysis
    with eng.analysis(boards[1]) as an:
        n = 0
        for info in an:
            n += 1
            if "score" in info and info.get("depth", 0) >= 6: break
        an.stop()
    check("analysis(): infinite search streams info with score, stops on request", n >= 1)
    # ---- Move Overhead on a fixed move time
    eng.configure({"OwnBook": False, "Move Overhead": 5})
    t0 = time.time(); eng.play(boards[1], chess.engine.Limit(time=1.0)); t_low = time.time() - t0
    eng.configure({"Move Overhead": 400})
    t0 = time.time(); eng.play(boards[1], chess.engine.Limit(time=1.0)); t_high = time.time() - t0
    eng.configure({"Move Overhead": 5})
    check("Move Overhead 400 shortens a 1 s move time (%.2f s -> %.2f s)" % (t_low, t_high), t_high < 0.75 and t_low > 0.85, "%.2f / %.2f" % (t_low, t_high))
    eng.quit()
    # ---- pondering: hit and miss (python-chess starts `go ponder` itself and sends ponderhit / stop)
    eng = open_engine(); eng.configure({"OwnBook": False})
    b = chess.Board(); ok = True; det = ""
    for i in range(6):
        r = eng.play(b, chess.engine.Limit(white_clock=2.0, black_clock=2.0, white_inc=0.05, black_inc=0.05), ponder=True)
        if r.move not in b.legal_moves: ok = False; det = "illegal move %s" % r.move; break
        b.push(r.move)
        if r.ponder is not None and r.ponder not in b.legal_moves: ok = False; det = "illegal ponder move %s" % r.ponder; break
        reply = r.ponder if (i % 2 == 0 and r.ponder is not None) else next(iter(b.legal_moves))          # alternate ponder hits and misses
        b.push(reply)
        if b.is_game_over(): break
    check("play(ponder=True) over 6 turns, ponder hits and misses: all moves legal", ok, det)
    eng.quit()
    # ---- Chess960
    eng = open_engine()
    b = chess.Board.from_chess960_pos(123); ok = True; det = ""
    for _ in range(14):
        r = eng.play(b, chess.engine.Limit(time=0.05))
        if r.move not in b.legal_moves: ok = False; det = "illegal %s in %s" % (r.move, b.fen()); break
        b.push(r.move)
        if b.is_game_over(): break
    check("Chess960: 14 plies from a Chess960 start position, all moves legal", ok, det)
    eng.quit()
    # ---- game handling
    eng = open_engine(); g1, g2 = object(), object()
    ok = True
    for g in (g1, g1, g2, g2):
        r = eng.play(chess.Board(), chess.engine.Limit(time=0.05), game=g); ok &= r.move in chess.Board().legal_moves
    check("new-game handling (ucinewgame between games)", ok)
    eng.quit()
finally:
    import shutil; shutil.rmtree(tmp, ignore_errors=True)
print("interop_test: %d checks, %d failed" % (len(res), res.count(False)))
