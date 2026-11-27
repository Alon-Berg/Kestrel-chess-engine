r"""Kestrel 5.1 and later: robustness against what real GUIs and real files do.  Usage: python robust_test.py <exe> [<kestrel_net.bin> [<Syzygy folder with KQvK.rtbw/.rtbz and KRvK.rtbw/.rtbz>]]
 1. input closed without `quit` (a GUI that dies): the engine exits by itself, also while it searches
 2. `Ponder` on, about 300 short searches (`go movetime 25`) from random positions: every bestmove is legal and the ponder move after it is legal in the position after the bestmove
    (Kestrel 5.0 printed the ponder move of the previous iteration when the clock cut a search off: illegal in 0.3 % of the short searches)
 3. (net given) EvalFile with the path in a folder whose name has spaces, accents, Hebrew or Chinese letters, sent as UTF-8 (fastchess, cutechess) and, when the name fits, in the ANSI code page (Arena), and with the
    path in double quotes (Explorer's "Copy as path"): the net must be loaded (Kestrel 5.0 on Windows: not found with UTF-8 names, quotes taken literally)
 4. (net given) damaged nets are refused and the engine goes on: empty file, truncated, wrong magic, a NaN or infinite scale in the header, a NaN / infinite weight in the head (Kestrel 5.0 accepted a NaN scale:
    every score was NaN and the search never returned)
 5. (Syzygy folder given) tablebases are found in a folder with such names (same name list as 3)
The last line is `robust_test: PASS` or `robust_test: FAIL (n problems)`.  Needs `pip install chess`.  On Linux and macOS the non-ASCII names are plain UTF-8 file names (the ANSI case is skipped)."""
import os
import queue
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

import chess

exe = os.path.abspath(sys.argv[1])
net = sys.argv[2] if len(sys.argv) > 2 else None
syz = sys.argv[3] if len(sys.argv) > 3 else None
NL = chr(10)
problems = []


def report(ok, text):
    print(("PASS " if ok else "FAIL ") + text, flush=True)
    if not ok:
        problems.append(text)


class Eng:
    def __init__(self):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.q = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in iter(self.p.stdout.readline, b""):
            self.q.put(raw.decode("utf-8", errors="replace").rstrip())
        self.q.put(None)

    def send(self, line, enc="utf-8"):
        try:
            self.p.stdin.write((line + NL).encode(enc, errors="replace"))
            self.p.stdin.flush()
        except OSError:
            pass

    def wait(self, token, timeout):
        out = []
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                line = self.q.get(timeout=0.2)
            except queue.Empty:
                continue
            if line is None:
                return out, False
            out.append(line)
            if line.startswith(token):
                return out, True
        return out, False

    def close(self):
        self.send("quit")
        try:
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


# 1. EOF ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
for label, cmds in (("idle", ["uci", "isready"]), ("go infinite", ["uci", "isready", "position startpos", "go infinite"]), ("go movetime 3000", ["uci", "isready", "position startpos", "go movetime 3000"])):
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    for c in cmds:
        p.stdin.write((c + NL).encode())
    p.stdin.flush()
    time.sleep(0.5)
    p.stdin.close()
    try:
        p.wait(timeout=10)
        report(True, "input closed (%s): the engine exits" % label)
    except subprocess.TimeoutExpired:
        p.kill()
        report(False, "input closed (%s): the engine is still running after 10 s" % label)

# 2. ponder moves ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
rnd = random.Random(20261127)
e = Eng()
e.send("uci")
e.send("setoption name Ponder value true")
e.send("setoption name Hash value 32")
e.send("isready")
e.wait("readyok", 30)
bad_bm = bad_pm = n = 0
examples = []
for i in range(300):
    b = chess.Board()
    for _ in range(rnd.randint(8, 70)):
        ms = list(b.legal_moves)
        if not ms or b.is_game_over():
            break
        b.push(rnd.choice(ms))
    if b.is_game_over():
        continue
    e.send("ucinewgame")
    e.send("position fen " + b.fen())
    e.send("go movetime 25")
    out, ok = e.wait("bestmove", 20)
    if not ok:
        report(False, "ponder test: no bestmove within 20 s")
        break
    parts = out[-1].split()
    n += 1
    mv = chess.Move.from_uci(parts[1])
    if mv not in b.legal_moves:
        bad_bm += 1
        continue
    if len(parts) >= 4 and parts[2] == "ponder":
        b.push(mv)
        if chess.Move.from_uci(parts[3]) not in b.legal_moves:
            bad_pm += 1
            if len(examples) < 3:
                examples.append("%s ponder %s" % (parts[1], parts[3]))
e.close()
report(n > 200 and bad_bm == 0 and bad_pm == 0, "ponder: %d searches, %d illegal bestmoves, %d illegal ponder moves %s" % (n, bad_bm, bad_pm, examples))

# 3-5. file names ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
NAMES = [("ascii", "plain_folder", False), ("spaces", "with space & parens (1)", False), ("accents", "café_ünï", False), ("hebrew", "שחמט_chess", False), ("chinese", "棋贛_test", False),
         ("quoted", "quoted_folder", True)]
root = tempfile.mkdtemp(prefix="kestrel_robust_")


def has_error(out, words):
    return any(any(w in l for w in words) for l in out)


def net_loaded(path, enc, quote=False):
    e2 = Eng()
    e2.send("uci", enc)
    e2.send("setoption name EvalFile value " + ('"' + path + '"' if quote else path), enc)
    e2.send("isready", enc)
    out, ok = e2.wait("readyok", 30)
    e2.close()
    return ok and not has_error(out, ("could not load",))


def syzygy_found(path, enc, quote=False):
    e2 = Eng()
    e2.send("uci", enc)
    e2.send("setoption name SyzygyPath value " + ('"' + path + '"' if quote else path), enc)
    e2.send("isready", enc)
    out, ok = e2.wait("readyok", 30)
    e2.close()
    return ok and any("tablebases up to" in l and "found" in l for l in out)


if net:
    for key, nm, quote in NAMES:
        d = os.path.join(root, nm)
        os.makedirs(d)
        shutil.copy(net, os.path.join(d, "kestrel_net.bin"))
        path = os.path.join(d, "kestrel_net.bin")
        encs = ["utf-8"]
        if sys.platform == "win32":
            try:
                path.encode("cp1252")
                encs.append("cp1252")
            except UnicodeEncodeError:
                pass
        for enc in encs:
            report(net_loaded(path, enc, quote), "EvalFile in a folder named '%s' (%s%s)" % (key, enc, ", in quotes" if quote else ""))
        if syz:
            sd = os.path.join(d, "syzygy")
            os.makedirs(sd)
            for f in ("KQvK.rtbw", "KQvK.rtbz", "KRvK.rtbw", "KRvK.rtbz"):
                shutil.copy(os.path.join(syz, f), os.path.join(sd, f))
            for enc in encs:
                report(syzygy_found(sd, enc, quote), "SyzygyPath in a folder named '%s' (%s%s)" % (key, enc, ", in quotes" if quote else ""))
    # damaged nets
    data = open(net, "rb").read()
    h = struct.unpack("<7i", data[8:36])
    head0 = 64 + h[2] * h[1] + 4 * h[1]
    nfl = (len(data) - head0) // 4

    def with_float(offset, val):
        d = bytearray(data)
        d[offset:offset + 4] = struct.pack("<f", val)
        return bytes(d)

    damaged = [("empty file", b""), ("truncated to 1 MB", data[:1 << 20]), ("wrong magic", b"XXXXXXXX" + data[8:]), ("scale = NaN", with_float(40, float("nan"))), ("scale = +inf", with_float(40, float("inf"))),
               ("scale = 0", with_float(40, 0.0)), ("last head weight = NaN", with_float(head0 + 4 * (nfl - 1), float("nan"))), ("first head weight = NaN", with_float(head0, float("nan"))),
               ("middle head weight = -inf", with_float(head0 + 4 * (nfl // 2), float("-inf")))]
    for label, blob in damaged:
        path = os.path.join(root, "damaged.bin")
        open(path, "wb").write(blob)
        e3 = Eng()
        e3.send("uci")
        e3.send("setoption name EvalFile value " + path)
        e3.send("isready")
        out1, ok1 = e3.wait("readyok", 30)
        e3.send("position startpos moves e2e4 e7e5")
        e3.send("go depth 8")
        out2, ok2 = e3.wait("bestmove", 30)
        e3.close()
        refused = has_error(out1, ("could not load",))
        report(ok1 and ok2 and refused, "damaged net (%s): refused, the engine goes on%s" % (label, "" if (ok1 and ok2) else " [NO ANSWER: hang]" if not ok2 else ""))
shutil.rmtree(root, ignore_errors=True)
print("robust_test: PASS" if not problems else "robust_test: FAIL (%d problems)" % len(problems))
