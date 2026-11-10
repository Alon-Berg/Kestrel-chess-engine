"""Helpers for level tables (a JSON list of level configs, Level 1 first; tables/final.json is the table of docs/play/levels.js).
   python table_tools.py pairs tables/final.json out/pairs.json      neighbour pairs [[L2, L1], [L3, L2], ...] as spec strings for run_selfplay.py --pairs-file
   python table_tools.py selfplay tables/final.json out/self.json    every level against itself
   short(name) turns a spec string into 'L5' when LEVEL_TABLE (environment variable) names a table file that contains it."""
import json, os, sys


def spec(cfg):
    return json.dumps(cfg, separators=(",", ":"))


def short(name):
    path = os.environ.get("LEVEL_TABLE")
    if path and os.path.exists(path):
        table = [spec(c) for c in json.load(open(path))]
        try:
            canon = spec(json.loads(name))
        except Exception:
            return name
        if canon in table:
            return "L%d" % (table.index(canon) + 1)
    return name


if __name__ == "__main__":
    kind, src, dst = sys.argv[1:4]
    table = [spec(c) for c in json.load(open(src))]
    if kind == "pairs":
        pairs = [[table[i + 1], table[i]] for i in range(len(table) - 1)]
    elif kind == "selfplay":
        pairs = [[t, t] for t in table]
    else:
        raise SystemExit("pairs | selfplay")
    json.dump(pairs, open(dst, "w"))
    print(len(pairs), "pairs ->", dst)
