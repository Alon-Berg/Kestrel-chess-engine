r"""Kestrel 4.0 (many search threads): turns src/kestrel_4_0_base.cpp (the 4.0 source with 8 search copies) into a source with N search copies S0..S(N-1) and a larger Hash maximum.
usage: python scripts/make_threads.py <N> [out.cpp] [--hash-max MB] [--src file] [--id-name "Kestrel 4.0"] [--pawn-bss 0|1] [--mem 0|1] [--prio 0|1] [--nodes 0|1] [--depth1 0|1] [--async 0|1]
src/kestrel_4_0.cpp is the output of `python scripts/make_threads.py 128 src/kestrel_4_0.cpp --id-name "Kestrel 4.0"` (byte for byte); with `--src src/kestrel_3_0.cpp --id-name "Kestrel 3.1"` the script
makes the source of 3.1 (src/kestrel_3_1.cpp) except for the huge-page advice of 13.

How 3.0 got its limit of 8 threads: the search (search, qsearch, move ordering, history / continuation / correction / pawn tables, the per-thread position with its accumulator ring) is compiled once
per thread, as the namespaces S0..S7, so that every thread works on its own globals (no thread_local, no pointer to a thread record: the generated code is the one of a single-threaded engine).
The 8 copies of the input are textually identical, only the namespace name differs.  This script clones S0 into S8..S(N-1) and changes exactly:
  1. const int MAXTH = 8;                                  -> N   (+ const int HASH_MAX_MB)
  2. helperFns[MAXTH], nodeCtr[MAXTH], clearAllTables()    -> list all N copies
  3. "option name Threads ... max 8"                       -> max N      "option name Hash ... max 4096" -> max <hash-max>
  4. the Hash option handler: min(4096, atoi(..)) -> a clamp of a 64-bit parse to [1, HASH_MAX_MB] (atoi of a number above 2^31 wraps on some platforms)
  5. resizeTT: a message (info string) when the table could not be allocated and a smaller one is tried
  6. id name Kestrel 3.0 -> the --id-name, and one comment line at the top
  7. (--pawn-bss 1, the default) the evaluation's pawn-structure table of every copy starts as all-zero instead of w = b = 1: a zero entry is the correct entry for a position without pawns
     (computePawnEntry gives 0 / 0 / no passed pawns for it), so every evaluation is the same, but the table (1.25 MB per copy) lives in .bss and no longer in the exe file
     (3.0's 8 copies hold 10 MB of that in .data; 128 copies would add 150 MB to the file).  --pawn-bss 0 keeps the 3.0 initialisation (a plain clone).
  8. (--mem 1, the default) three changes for big machines, all in the code around the hash table and the table clearing (--mem 0 leaves them out):
     a. Hash requests above 3/4 of the installed memory are cut to that size (info string): the 99999999999 of the audit tests became a 64 GB allocation that Windows granted (RAM + page file) and then paged the PC to death;
     b. the hash table is zeroed by many threads (allocation and the in-place clear of ucinewgame / Clear Hash): 16 GB took 4 - 7 s with one thread; blocks below 512 MB are cleared as before;
     c. ucinewgame / Clear Hash clear the history tables of the copies that have been started since the last clear only (clearing all 128 touched ~0.4 GB of memory that a Threads 1 game never needs).
  9. (--prio 1, the default; Windows only) the search thread runs at a higher priority while it starts the helper threads and drops back to normal after the last one: with more threads than processors the
     helpers that already run keep the creator in the ready queue, and starting 127 of them took 3 s on average and up to 20 s (measured with 22 logical processors: first info line 2.6 s median at Threads 128);
     helper threads and the priority of the search itself are unchanged.
 10. (--nodes 1, the default) the node limit of `go nodes N` and UCI_LimitStrength is compared with the node count of the main thread only (helpers set `isHelper` in helperMain and never stop on it; 3 lines
     in every copy + one declaration).  In 3.0 every thread compared its own count, so the first helper to reach N stopped the search; with many threads the main thread had often not completed depth 1
     by then and the best-ordered move was played (breakers.py `go nodes 3000`: a stalemating queen move in a won ending; 20 of 20 tries with 128 threads, 7 of 20 with 8 threads, also in 3.0).
 11. (--depth1 1, the default) a time limit (go movetime, the clock) cannot end a search before depth 1 has completed: `depth1Done` (shared, false from the start of think() until its first iteration is done) is
     part of the condition in checkTime() of every copy.  Without it the first time check of a thread, which runs at node 0, could end the search before the main thread had searched anything when the helpers
     took longer than the hard limit to start (128 threads on a busy 22-processor laptop, go movetime 60: 39 of 60 searches played the best-ordered move without a search; 64 threads: 8 of 60); `stop` and node limits are not affected.
 12. (--async 1, the default) the helper threads are started by a launcher thread and the main search begins at once (3.0: think() started all helpers first; with 64 - 128 threads that takes 6 - 100 ms and more, which with a short
     move time was most of it: 128 threads, go movetime 60 on a 22-processor laptop: 19 of 60 searches ended at depth 1 and played a stalemating move, the stalemate being visible from depth 2 on).  The launcher stops as soon as the
     search ends; think() joins it before the helpers.  `searchThreads` becomes atomic (the launcher writes it while the main thread reads it for the node count of the info lines).
 14. (--mem 1, the default; macOS) the limit of 3/4 of the installed memory (item 8a) also works on macOS (sysctl hw.memsize).
 13. (--mem 1, the default; Linux only) the hash table is advised to use transparent huge pages (madvise MADV_HUGEPAGE on its 2 MB aligned part, tables of 256 MB and more): the random probes of many threads
     then miss the TLB far less often.  An advice only: ignored where huge pages are off; Windows and macOS are unchanged.
Nothing else in the search or the evaluation is touched by this script.
The script then checks its own result: all N copies identical after renaming, and the lines outside the copies differ from the input only in the places listed above (it prints that diff)."""
import difflib, os, re, sys

D = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
N = int(args.pop(0))
opts = {"--hash-max": "1048576", "--src": os.path.join(os.path.dirname(D), "src", "kestrel_4_0_base.cpp"), "--id-name": "Kestrel 4.0", "--pawn-bss": "1", "--mem": "1", "--prio": "1", "--nodes": "1", "--depth1": "1", "--async": "1"}
out = os.path.join(os.path.dirname(D), "src", "kestrel_4_0.cpp")       # (default name; the plain-clone variants of 2026-11-06 were written with the old default)
i = 0
while i < len(args):
    if args[i] in opts: opts[args[i]] = args[i + 1]; i += 2
    else: out = args[i]; i += 1
HASH_MAX = int(opts["--hash-max"]); ID_NAME = opts["--id-name"]
assert N >= 8, "the source has 8 copies; N must be at least 8"
text = open(opts["--src"], encoding="utf-8", newline="").read()
assert "\r" not in text, "the source must have LF line ends"
lines = text.split("\n")


def blocks_of(ls):
    """{k: (start, end)}: line index range of the namespace S<k> block (the namespace line up to the line before the next one / before 'using namespace S0;')"""
    st = [(int(m.group(1)), j) for j, l in enumerate(ls) for m in [re.match(r"namespace S(\d+) \{$", l)] if m]
    end = next(j for j, l in enumerate(ls) if l == "using namespace S0;")
    assert [k for k, _ in st] == list(range(len(st))), "namespaces are not S0..Sn in order"
    return {k: (a, st[n + 1][1] if n + 1 < len(st) else end) for n, (k, a) in enumerate(st)}


def check_copies(ls, expect):
    bl = blocks_of(ls); assert len(bl) == expect, "found %d copies, expected %d" % (len(bl), expect)
    ref = "\n".join(ls[bl[0][0]:bl[0][1]])
    assert len(re.findall(r"\bS\d+\b", ref)) == 1                      # the namespace name only
    for k in range(1, expect):
        t = "\n".join(ls[bl[k][0]:bl[k][1]]); assert ls[bl[k][1] - 1] == "}", "S%d does not end with }" % k
        assert re.sub(r"\bS%d\b" % k, "S0", t) == ref, "copy S%d differs from S0 beyond its name" % k
    return bl


bl = check_copies(lines, 8)
a0, b0 = bl[0]; end8 = bl[7][1]
s0 = lines[a0:b0]
clones = []
for k in range(8, N): clones += [re.sub(r"\bS0\b", "S%d" % k, l) for l in s0]
body = lines[:end8] + clones + lines[end8:]
t = "\n".join(body)


def rep(old, new, n=1):
    global t
    c = t.count(old); assert c == n, "expected %d match(es), found %d for: %s" % (n, c, old[:100])
    t = t.replace(old, new)


rep("const int MAXTH = 8;\n", "const int MAXTH = %d;                    // search copies S0..S%d (one per thread)\nconst int HASH_MAX_MB = %d;          // largest Hash option value\n" % (N, N - 1, HASH_MAX))
rep("static void (*helperFns[MAXTH])(int) = { nullptr, S1::helperMain, S2::helperMain, S3::helperMain, S4::helperMain, S5::helperMain, S6::helperMain, S7::helperMain };",
    "static void (*helperFns[MAXTH])(int) = { nullptr, " + ", ".join("S%d::helperMain" % k for k in range(1, N)) + " };")
rep("static long long* nodeCtr[MAXTH] = { &S0::nodes_, &S1::nodes_, &S2::nodes_, &S3::nodes_, &S4::nodes_, &S5::nodes_, &S6::nodes_, &S7::nodes_ };",
    "static long long* nodeCtr[MAXTH] = { " + ", ".join("&S%d::nodes_" % k for k in range(N)) + " };")
if opts["--mem"] == "1":
    rep("static void clearAllTables() { S0::clearTables(); S1::clearTables(); S2::clearTables(); S3::clearTables(); S4::clearTables(); S5::clearTables(); S6::clearTables(); S7::clearTables(); }",
        "static void (*clearFns[MAXTH])() = { " + ", ".join("S%d::clearTables" % k for k in range(N)) + " };\n"
        "static int copiesUsed = 1;               // search copies S0..S(copiesUsed-1) may hold data since the last clear; the others were never started (their tables are still untouched zero pages)\n"
        "static void clearAllTables() { for (int k = 0; k < copiesUsed; k++) clearFns[k](); copiesUsed = 1; }")
    if opts["--async"] != "1":                 # (with --async 1 the same bookkeeping is part of the new start-up code below)
        rep("        catch (const std::exception&) {}              // fewer threads than asked for\n    }\n",
            "        catch (const std::exception&) {}              // fewer threads than asked for\n    }\n    copiesUsed = std::max(copiesUsed, searchThreads);\n")
else:
    rep("static void clearAllTables() { S0::clearTables(); S1::clearTables(); S2::clearTables(); S3::clearTables(); S4::clearTables(); S5::clearTables(); S6::clearTables(); S7::clearTables(); }",
        "static void clearAllTables() { " + " ".join("S%d::clearTables();" % k for k in range(N)) + " }")
rep("option name Hash type spin default 256 min 1 max 4096", "option name Hash type spin default 256 min 1 max %d" % HASH_MAX)
rep("option name Threads type spin default 1 min 1 max 8\\n", "option name Threads type spin default 1 min 1 max %d\\n" % N)
rep("hashMB = max(1, min(4096, atoi(value.c_str())));", "hashMB = (int)max(1LL, min((long long)HASH_MAX_MB, strtoll(value.c_str(), nullptr, 10)));")
rep("catch (const std::exception&) { tt.reset(); if (mb <= 1) throw; mb /= 2; }",
    "catch (const std::exception&) { tt.reset(); if (mb <= 1) throw; mb /= 2; printf(\"info string Hash: not enough memory, trying %d MB\\n\", mb); fflush(stdout); }")
rep("id name Kestrel 3.0\\n", "id name %s\\n" % ID_NAME)
if opts["--pawn-bss"] == "1":
    rep("struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };", "struct PawnEntry { U64 w = 0, b = 0; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };", N)
if opts["--nodes"] == "1":
    # go nodes N / UCI_LimitStrength: each thread compared its OWN node count with the limit, so the first helper to reach it stopped the whole search - with many threads (they are started one after the other and the
    # first ones run for milliseconds before the main thread begins) before the main thread had finished depth 1: bestmove = the best-ordered move, a stalemating one in 20 of 20 tries of a won KNQ-v-K ending (Threads 128)
    rep("long long nodes_ = 0;\n", "long long nodes_ = 0;\nstatic bool isHelper = false;                 // 3.1: set by helperMain; only the main thread's node count ends a go nodes / UCI_LimitStrength search\n", N)
    rep("    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;\n", "    if (nodeLimit > 0 && nodes_ >= nodeLimit && !isHelper) stopFlag = true;\n", N)
    rep("void helperMain(int id) {\n    pos = gRoot; nodes_ = 0;\n", "void helperMain(int id) {\n    isHelper = true;\n    pos = gRoot; nodes_ = 0;\n", N)
if opts["--depth1"] == "1":
    rep("long long softMs = 0, hardMs = 0, nodeLimit = 0;\n", "long long softMs = 0, hardMs = 0, nodeLimit = 0;\nstatic std::atomic<bool> depth1Done{true};     // 3.1: false from the start of think() until its first iteration (depth 1) has completed; a time limit cannot end a search before that\n")
    rep("    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;\n", "    if (hardMs > 0 && elapsedMs() >= hardMs && depth1Done.load(std::memory_order_relaxed)) stopFlag = true;\n", N)
    rep("void think() {\n    winFullSpeed();\n", "void think() {\n    winFullSpeed();\n    struct Depth1Guard { Depth1Guard() { depth1Done = false; } ~Depth1Guard() { depth1Done = true; } } depth1Guard;       // 3.1: see depth1Done\n")
    rep("        if (stopFlag) break;\n        int prevScoreIt = bestScore, prevBestIt = bestMove;\n", "        if (stopFlag) break;\n        depth1Done = true;\n        int prevScoreIt = bestScore, prevBestIt = bestMove;\n")
if opts["--async"] == "1":
    # 3.0 started the helpers in think() before the main search began: 127 threads take 6 - 14 ms on a computer with enough processors and 35 - 100 ms and more with fewer processors than threads or a busy computer,
    # so with a short move time the main thread got a few plies (128 threads, go movetime 60 on a 22-processor laptop: 19 of 60 searches ended at depth 1 and played a stalemating move in a won ending, 32 of 60 before depth 6).
    # Now a launcher thread starts the helpers (at a higher priority on Windows, so that it does not queue behind the helpers that already run) and the main search begins at once; the launcher stops when the search ends.
    assert opts["--mem"] == "1", "--async needs --mem (copiesUsed)"
    rep("static int searchThreads = 1;           // threads of the running (or last) search: the node counters of the others are stale\n",
        "static std::atomic<int> searchThreads{1};           // threads of the running (or last) search: the node counters of the others are stale (3.1: written by the launcher thread)\n")
    rep("    KThread helperTh[MAXTH]; const int nth = (multiPV > 1) ? 1 : max(1, min(MAXTH, numThreads));\n"
        "    struct HelperJoin { KThread* h; int n; ~HelperJoin() { stopFlag = true; for (int k = 1; k < n; k++) if (h[k].joinable()) h[k].join(); } } helperJoin{helperTh, nth};\n"
        "    searchThreads = 1;\n"
        "    if (nth > 1) {\n"
        "        gRoot = pos;\n"
        "        try { for (int k = 1; k < nth; k++) { helperTh[k] = KThread(helperFns[k], k); searchThreads = k + 1; } }\n"
        "        catch (const std::exception&) {}              // fewer threads than asked for\n"
        "    }\n",
        "    KThread helperTh[MAXTH], launcher; const int nth = (multiPV > 1) ? 1 : max(1, min(MAXTH, numThreads));\n"
        "    struct HelperJoin { KThread* h; int n; KThread* l; ~HelperJoin() { stopFlag = true; if (l->joinable()) l->join(); for (int k = 1; k < n; k++) if (h[k].joinable()) h[k].join(); } } helperJoin{helperTh, nth, &launcher};\n"
        "    searchThreads = 1;\n"
        "    copiesUsed = std::max(copiesUsed, nth);          // 3.1: up to nth search copies are going to hold data\n"
        "    if (nth > 1) {\n"
        "        gRoot = pos;\n"
        "        // 3.1: the helpers are started by a thread of their own, so that the main search begins at once (starting 127 threads takes 6 - 100 ms and more when the computer is busy: with a short move time that was most of it)\n"
        "        try {\n"
        "            launcher = KThread([&helperTh, nth]() {\n"
        "#ifdef _WIN32\n"
        "                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);        // do not queue behind the helpers that are already running\n"
        "#endif\n"
        "                try { for (int k = 1; k < nth && !stopFlag; k++) { helperTh[k] = KThread(helperFns[k], k); searchThreads = k + 1; } }\n"
        "                catch (const std::exception&) {}              // fewer threads than asked for\n"
        "            });\n"
        "        }\n"
        "        catch (const std::exception&) {}              // no thread for the launcher: the main thread searches alone\n"
        "    }\n")
    rep("    stopFlag = true;                                  // the helpers stop with the main search\n    for (int k = 1; k < nth; k++) if (helperTh[k].joinable()) helperTh[k].join();\n",
        "    stopFlag = true;                                  // the helpers stop with the main search\n    if (launcher.joinable()) launcher.join();\n    for (int k = 1; k < nth; k++) if (helperTh[k].joinable()) helperTh[k].join();\n")
if opts["--prio"] == "1" and opts["--async"] != "1":
    rep("        try { for (int k = 1; k < nth; k++) { helperTh[k] = KThread(helperFns[k], k); searchThreads = k + 1; } }\n        catch (const std::exception&) {}              // fewer threads than asked for\n",
        "#ifdef _WIN32\n"
        "        const int creatorPrio = GetThreadPriority(GetCurrentThread()); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);        // 3.1: do not queue behind the helpers that are already running\n"
        "#endif\n"
        "        try { for (int k = 1; k < nth; k++) { helperTh[k] = KThread(helperFns[k], k); searchThreads = k + 1; } }\n"
        "        catch (const std::exception&) {}              // fewer threads than asked for\n"
        "#ifdef _WIN32\n"
        "        if (creatorPrio != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), creatorPrio);\n"
        "#endif\n")
if opts["--mem"] == "1":
    PHYS_ZERO = r'''// ---------------------------------------------------------------- 3.1: big hash tables
#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
static long long physMemMB() {           // installed physical memory in MB (0 = unknown)
#if defined(_WIN32)
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (long long)(ms.ullTotalPhys >> 20) : 0;
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE);
    return (pages > 0 && psz > 0) ? (long long)((unsigned long long)pages * (unsigned long long)psz >> 20) : 0;
#elif defined(__APPLE__)
    int64_t memsize = 0; size_t len = sizeof memsize;
    return (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0 && memsize > 0) ? (long long)(memsize >> 20) : 0;
#else
    return 0;
#endif
}
#if defined(__linux__) && defined(MADV_HUGEPAGE)
static void adviseHuge(void* p, size_t n) {       // 3.2: ask for transparent huge pages (2 MB) for a big table; the kernel may ignore it
    const uintptr_t mask = ((uintptr_t)2 << 20) - 1, a = ((uintptr_t)p + mask) & ~mask, e = ((uintptr_t)p + n) & ~mask;
    if (n >= ((size_t)256 << 20) && e > a) madvise((void*)a, (size_t)(e - a), MADV_HUGEPAGE);
}
#else
static void adviseHuge(void*, size_t) {}
#endif
// Zeroes a big block with several threads: a hash table of many GB takes seconds to clear with one thread (at every ucinewgame), a few tenths of a second with all cores; the pages are also first touched by
// many threads, which spreads them over the memory nodes of a multi-socket machine.  Blocks below 512 MB are cleared by the calling thread alone (as in 3.0).
static void zeroMem(void* p, size_t n) {
    const size_t unit = (size_t)256 << 20; unsigned hw = std::thread::hardware_concurrency(); if (hw == 0) hw = 1;
    const size_t nt = std::min<size_t>(std::min<size_t>(hw, 64), n / unit);
    if (nt < 2) { memset(p, 0, n); return; }
    const size_t chunk = ((n / nt) + 4095) & ~(size_t)4095;
    std::vector<KThread> th; char* base = (char*)p;
    for (size_t i = 1; i < nt; i++) {
        const size_t a = i * chunk, b = std::min(n, a + chunk); if (a >= n) break;
        try { th.emplace_back([base, a, b]() { memset(base + a, 0, b - a); }); }
        catch (const std::exception&) { memset(base + a, 0, b - a); }              // no thread available: do it here
    }
    memset(base, 0, std::min(n, chunk));
    for (auto& t : th) if (t.joinable()) t.join();
}
'''
    rep("void resizeTT(int mb) {\n", PHYS_ZERO + "void resizeTT(int mb) {\n")
    rep("        if (ttDirty) { memset((void*)ttBase, 0, (size_t)(ttMask + 1) * sizeof(TTE)); ttDirty = false; }\n",
        "        if (ttDirty) { zeroMem((void*)ttBase, (size_t)(ttMask + 1) * sizeof(TTE)); ttDirty = false; }\n")
    rep("    const int requested = mb;\n",
        "    const int requested = mb;\n"
        "    { const long long phys = physMemMB();       // never ask for more than 3/4 of the installed memory: the table would be paged out and the whole machine with it\n"
        "      if (phys > 0 && (long long)mb > phys * 3 / 4) { mb = (int)(phys * 3 / 4); printf(\"info string Hash: limited to %d MB (3/4 of the installed memory, %lld MB)\\n\", mb, phys); fflush(stdout); } }\n")
    rep("try { tt.reset(new TTE[p + 4]()); ttBase = (TTE*)(((uintptr_t)tt.get() + 63) & ~(uintptr_t)63); ttMask = p - 1; ttRequestedMB = requested; ttDirty = false; return; }",
        "try { tt.reset(new TTE[p + 4]); ttBase = (TTE*)(((uintptr_t)tt.get() + 63) & ~(uintptr_t)63); ttMask = p - 1; adviseHuge((void*)ttBase, p * sizeof(TTE)); zeroMem((void*)ttBase, p * sizeof(TTE)); ttRequestedMB = requested; ttDirty = false; return; }")
t = t.replace("// Claude 1.0 - a from-scratch UCI chess engine (bitboards, alpha-beta, hand-made eval).\n",
              "// Claude 1.0 - a from-scratch UCI chess engine (bitboards, alpha-beta, hand-made eval).\n"
              "// %s: Kestrel 4.0 with %d search copies (Threads up to %d) and Hash up to %d MB%s; generated by scripts/make_threads.py from src/kestrel_4_0_base.cpp\n" % (
                  ID_NAME, N, N, HASH_MAX, ", pawn table zero-initialised (in .bss)" if opts["--pawn-bss"] == "1" else ""), 1)
new_lines = t.split("\n")
nb = check_copies(new_lines, N)
# the lines outside the copies must differ from the input only where listed
outside_old = lines[:a0] + lines[end8:]
outside_new = new_lines[:nb[0][0]] + new_lines[nb[N - 1][1]:]
sm = difflib.SequenceMatcher(None, outside_old, outside_new, autojunk=False)
changed = []
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    if tag != "equal": changed.append((tag, i1, i2, j1, j2))
print("changed regions outside the copies: %d" % len(changed))
for tag, i1, i2, j1, j2 in changed:
    print("  %-8s old lines %d-%d -> new lines %d-%d: %s" % (tag, i1 + 1, i2, j1 + 1, j2, (" | ".join(outside_new[j1:j2]))[:150].replace("\n", " ")))
assert len(changed) <= 20, "unexpected number of changes"
if opts["--pawn-bss"] == "1" or opts["--nodes"] == "1" or opts["--depth1"] == "1":              # the copies are still identical to each other (check_copies above); S0 differs from the input's S0 in these lines only
    ref_old = lines[a0:b0]; ref_new = new_lines[nb[0][0]:nb[0][1]]
    d = [l for l in difflib.unified_diff(ref_old, ref_new, lineterm="", n=0) if l[:1] in "+-" and l[:3] not in ("+++", "---")]
    assert len(d) == (2 if opts["--pawn-bss"] == "1" else 0) + (4 if opts["--nodes"] == "1" else 0) + (2 if opts["--depth1"] == "1" else 0), d
    print("S0 differs from 3.0's S0 in %d line(s) (pawn table initialisation, node limit of the main thread only, no time stop before depth 1):" % len(d))
    for l in d: print("    " + l[:150])
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
open(out, "w", encoding="utf-8", newline="").write(t)
print("wrote %s: %d copies, %d lines, %d bytes (3.0 source: %d lines, %d bytes); Threads max %d, Hash max %d MB" % (out, N, len(new_lines), len(t.encode()), len(lines), len(text.encode()), N, HASH_MAX))
