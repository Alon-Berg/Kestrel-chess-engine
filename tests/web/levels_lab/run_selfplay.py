"""Runs selfplay.js for several levels or pairs of levels in parallel (chunks of games, many deno processes) and writes out/<tag>_<job>.jsonl.
   python run_selfplay.py --tag s1 --levels 1,2,3,r1300 --games 100 --chunk 10 --procs 16      every level against itself (equal opponents, as in rating statistics)
   python run_selfplay.py --tag n1 --pair "2|1" --pair "3|2" --games 60                        two different levels against each other (also JSON specs)
   python run_selfplay.py --tag n2 --pairs-file out/pairs.json                                 a JSON list of [a, b] level specs (see table_tools.py)
   needs Deno (environment variable DENO, else deno from the PATH) and the network (environment variable KESTREL_NET)."""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
DENO = os.environ.get("DENO", "deno")
ap = argparse.ArgumentParser()
ap.add_argument("--tag", required=True)
ap.add_argument("--levels", default="")
ap.add_argument("--pair", action="append", default=[])
ap.add_argument("--pairs-file", default="")
ap.add_argument("--games", type=int, default=100)
ap.add_argument("--chunk", type=int, default=10)
ap.add_argument("--procs", type=int, default=16)
ap.add_argument("--think", type=int, default=100)
ap.add_argument("--seed", type=int, default=1)
a = ap.parse_args()

specs, depth, cur = [], 0, ""            # split the comma list but keep JSON objects together
for ch in a.levels:
    if ch == "{": depth += 1
    if ch == "}": depth -= 1
    if ch == "," and depth == 0:
        specs.append(cur); cur = ""
    else:
        cur += ch
if cur.strip(): specs.append(cur)
jobs = [(s, s) for s in specs if s.strip()]
for p in a.pair:
    x, y = p.split("|"); jobs.append((x, y))
if a.pairs_file:
    jobs += [tuple(p) for p in json.load(open(a.pairs_file))]

out_dir = os.path.join(HERE, "out"); os.makedirs(out_dir, exist_ok=True)
queue = [(ji, c, x, y) for ji, (x, y) in enumerate(jobs) for c in range(max(1, a.games // a.chunk))]
running, files, t0 = [], {}, time.time()
while queue or running:
    while queue and len(running) < a.procs:
        ji, c, x, y = queue.pop(0)
        f = os.path.join(out_dir, "%s_j%d_c%d.jsonl" % (a.tag, ji, c))
        files.setdefault(ji, []).append(f)
        cmd = [DENO, "run", "-A", os.path.join(HERE, "selfplay.js"), "--a=" + x, "--b=" + y, "--games=%d" % a.chunk,
               "--seed=%d" % (a.seed * 1000 + ji * 100 + c), "--think=%d" % a.think, "--out=" + f]
        running.append(subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True))
    time.sleep(0.5)
    for p in running[:]:
        if p.poll() is not None:
            running.remove(p)
            if p.returncode != 0:
                print("FAILED:", p.stderr.read()[-500:], flush=True)
print("done in %.0f s" % (time.time() - t0))
for ji, (x, y) in enumerate(jobs):          # merge the chunks of every job
    dst = os.path.join(out_dir, "%s_%d.jsonl" % (a.tag, ji))
    with open(dst, "w", encoding="utf-8") as w:
        for f in files.get(ji, []):
            if os.path.exists(f):
                w.write(open(f, encoding="utf-8").read()); os.remove(f)
    print(dst, "<-", x, "vs", y)
