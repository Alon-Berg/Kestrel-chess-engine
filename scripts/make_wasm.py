r"""Kestrel 3.2 for the browser: makes src/kestrel_3_2_wasm.cpp, the 3.2 engine as a single-thread WebAssembly program (wasm32-wasi, reactor model).
usage:  python scripts/make_wasm.py [out.cpp]          (default: src/kestrel_3_2_wasm.cpp, byte for byte the file in the repository)

The input is the 3.2 source with 8 search copies (python scripts/make_threads.py 8 ... --id-name "Kestrel 3.2", run by this script in a temporary folder).  What changes (the search,
the evaluation, the network and the time management are the text of 3.2):
  1. only the search copy S0 stays (WebAssembly runs one thread here): S1..S7 and their entries in the helper tables are removed, MAXTH = 1, option Threads max 1
  2. no threads, no exceptions, no files: KThread runs its function inside the constructor (never used with one thread), try / catch become if (true) / else if (false) (an allocation
     that fails ends the program instead of trying a smaller hash table), the Syzygy reader keeps compiling but its mmap is a stub (no table files exist in a browser), go infinite is refused
  3. the network is not embedded (.incbin): the page hands it over as bytes (kestrel_net_buffer, then kestrel_init)
  4. main() and its stdin loop are replaced by exported functions: kestrel_cmd_buffer, kestrel_net_buffer, kestrel_init, kestrel_uci (one UCI command per call, the replies go to stdout)
     and kestrel_bench; the body of the UCI loop is the text of 3.2
Compile with scripts/build_wasm.py (Zig).  The result gives the same moves and the same node counts as the native programs: bench 12 = 859,322 nodes."""
import os
import re
import subprocess
import sys
import tempfile

D = os.path.dirname(os.path.abspath(__file__))
out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(D), "src", "kestrel_3_2_wasm.cpp")
with tempfile.TemporaryDirectory() as tmp:
    n8 = os.path.join(tmp, "kestrel_3_2_n8.cpp")
    r = subprocess.run([sys.executable, os.path.join(D, "make_threads.py"), "8", n8, "--id-name", "Kestrel 3.2"], capture_output=True, text=True)
    assert r.returncode == 0, "make_threads.py failed: " + r.stderr[-500:]
    text = open(n8, encoding="utf-8", newline="").read()
assert "\r" not in text
log = []


def sub_once(old, new, what):
    global text
    assert text.count(old) == 1, "%s: %d matches" % (what, text.count(old))
    text = text.replace(old, new)
    log.append(what)


# ---- includes -------------------------------------------------------------------------------------------------------------------------------------------------
sub_once("#include <thread>\n", "", "no <thread>")
sub_once("#include <mutex>\n", "", "no <mutex>")
sub_once("#include <sys/mman.h>\n",
         "// WebAssembly: no mmap (the Syzygy reader keeps compiling, it finds no table files)\n#define PROT_READ 1\n#define MAP_SHARED 1\n#define MAP_FAILED ((void*)-1)\n"
         "static void* mmap(void*, size_t, int, int, int, long long) { return MAP_FAILED; }\nstatic int munmap(void*, size_t) { return 0; }\n", "mmap stub")
sub_once("#define TB_NO_HELPER_API\n", "#define TB_NO_THREADS\n#define TB_NO_HELPER_API\n", "Fathom without locks")
sub_once("#if defined(_WIN32) || defined(_MSC_VER) || defined(__EMSCRIPTEN__)\n", "#if defined(_WIN32) || defined(_MSC_VER) || defined(__EMSCRIPTEN__) || defined(__wasm__)\n", "Fathom byte order")

# ---- KThread: no threads -----------------------------------------------------------------------------------------------------------------------------------
a = text.index("#ifdef _WIN32\nusing KThread = std::thread;\n#else\n")
endmark = "    ~KThread() { if (on) std::terminate(); }\n};\n#endif\n"
b = text.index(endmark, a) + len(endmark)
text = text[:a] + """#include <stdexcept>
#include <exception>
struct KThread {      // WebAssembly: there are no threads; the function runs to completion inside the constructor (the engine starts none with Threads = 1)
    KThread() {}
    template <class F, class... A> explicit KThread(F f, A... a) { f(a...); }
    KThread(KThread&&) noexcept {}
    KThread& operator=(KThread&&) noexcept { return *this; }
    bool joinable() const { return false; }
    void join() {}
};
""" + text[b:]
log.append("KThread stub")

# ---- the network is handed over at run time ---------------------------------------------------------------------------------------------------------------------
m = re.search(r'__asm__\(KNET_SECTION[^\n]*\n(extern "C" const unsigned char kestrel_net_data\[\];\n)(extern "C" const unsigned char kestrel_net_end\[\];\n)', text)
assert m, "incbin block"
text = text[:m.start()] + "static const unsigned char* kestrel_net_data = nullptr;      // WebAssembly: set by kestrel_net_buffer\nstatic const unsigned char* kestrel_net_end = nullptr;\n" + text[m.end():]
log.append("network from memory")

# ---- exceptions off: try / catch become plain blocks (no #include may follow the macros) ---------------------------------------------------------------------
first_try = text.index("try {", text.index("namespace fathom {"))
assert all(l.strip().startswith("#include <sys/") for l in re.findall(r"^[ \t]*#[ \t]*include.*$", text[first_try:], re.M)), "an #include after the first try"
sub_once("namespace fathom {\n", "// WebAssembly: built with -fno-exceptions; try blocks always succeed, catch blocks never run\n#define try if (true)\n#define catch(...) else if (false)\nnamespace fathom {\n", "try / catch macros")
sub_once("if (mb <= 1) throw;", "if (mb <= 1) abort();", "no throw")

# ---- big-hash helper used threads --------------------------------------------------------------------------------------------------------------------------------
a = text.index("static void zeroMem(void* p, size_t n) {")
b = text.index("void resizeTT(int mb) {")
text = text[:a] + "static void zeroMem(void* p, size_t n) { memset(p, 0, n); }      // WebAssembly: one thread\n" + text[b:]
log.append("zeroMem")

sub_once("    while (goInfinite && !stopFlag) this_thread::sleep_for(chrono::milliseconds(1));\n", "    // WebAssembly: go infinite is refused by kestrel_uci\n", "no sleep")

# ---- one search copy ---------------------------------------------------------------------------------------------------------------------------------------------------
a = text.index("namespace S1 {\n")
b = text.index("using namespace S0;\n")
text = text[:a] + text[b:]
sub_once("const int MAXTH = 8;", "const int MAXTH = 1;", "MAXTH")
sub_once("static void (*helperFns[MAXTH])(int) = { nullptr, S1::helperMain, S2::helperMain, S3::helperMain, S4::helperMain, S5::helperMain, S6::helperMain, S7::helperMain };",
         "static void (*helperFns[MAXTH])(int) = { nullptr };", "helperFns")
sub_once("static long long* nodeCtr[MAXTH] = { &S0::nodes_, &S1::nodes_, &S2::nodes_, &S3::nodes_, &S4::nodes_, &S5::nodes_, &S6::nodes_, &S7::nodes_ };",
         "static long long* nodeCtr[MAXTH] = { &S0::nodes_ };", "nodeCtr")
sub_once("static void (*clearFns[MAXTH])() = { S0::clearTables, S1::clearTables, S2::clearTables, S3::clearTables, S4::clearTables, S5::clearTables, S6::clearTables, S7::clearTables };",
         "static void (*clearFns[MAXTH])() = { S0::clearTables };", "clearFns")
sub_once("option name Threads type spin default 1 min 1 max 8", "option name Threads type spin default 1 min 1 max 1", "option Threads")
assert not re.search(r"\bS[1-7]::", text), "a reference to a removed search copy"

# ---- main() and the stdin loop -> exported functions ---------------------------------------------------------------------------------------------------------------
a = text.index("int main(int argc, char** argv) {\n")
head, tail = text[:a], text[a:]
loop_start = "    KThread worker;\n    string line;\n    while (getline(cin, line)) {\n        istringstream is(line); string cmd; is >> cmd;\n        try {\n"
loop_end = "        } catch (const std::exception& ex) { printf(\"info string error: %s\\n\", ex.what()); fflush(stdout); }\n    }\n    if (worker.joinable()) { stopFlag = true; worker.join(); }\n    saveGame();\n    return 0;\n}\n"
i0 = tail.index(loop_start) + len(loop_start)
i1 = tail.index(loop_end)
assert tail[i1 + len(loop_end):].strip() == "", "text after main"
body = tail[i0:i1]
assert body.count("saveGame(); break; }") == 1
body = body.replace("saveGame(); break; }", "saveGame(); return false; }")
wasm_tail = r'''// ======================================================================== WebAssembly interface (generated by scripts/make_wasm.py) ========================================================
#define KEXPORT(n) extern "C" __attribute__((export_name(#n)))
static KThread worker;
static bool uciHandle(const string& line) {           // one UCI command (the body of the loop of main() in 3.2); false after quit
    istringstream is(line); string cmd; is >> cmd;
    {
''' + body + r'''    }
    return true;
}
static char gCmdBuf[1 << 16];
KEXPORT(kestrel_cmd_buffer) char* kestrel_cmd_buffer() { return gCmdBuf; }
KEXPORT(kestrel_net_buffer) unsigned char* kestrel_net_buffer(size_t len) {          // memory for the network file (64-byte aligned); the page copies the file into it, then calls kestrel_init
    static unsigned char* p = nullptr;
    if (p) { free(p); p = nullptr; kestrel_net_data = kestrel_net_end = nullptr; }
    p = (unsigned char*)aligned_alloc(64, (len + 63) & ~(size_t)63);
    if (p) { kestrel_net_data = p; kestrel_net_end = p + len; }
    return p;
}
KEXPORT(kestrel_init) int kestrel_init(int hashMegabytes) {          // 1 = network accepted
    hashMB = max(1, min(1024, hashMegabytes));
    initTables(); initPext(); initEval(); initNet(); resizeTT(hashMB); pos.refresh();
    rngState ^= (U64)chrono::steady_clock::now().time_since_epoch().count();
    initLmr();
    pos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    return netLoaded ? 1 : 0;
}
static bool hasWord(const string& s, const char* w) { return (" " + s + " ").find(" " + string(w) + " ") != string::npos; }
KEXPORT(kestrel_uci) int kestrel_uci() {          // runs the command in the command buffer; the replies are printed to stdout; 0 after quit
    gCmdBuf[sizeof gCmdBuf - 1] = 0;
    const string line(gCmdBuf);
    if (line.compare(0, 2, "go") == 0 && !hasWord(line, "perft") &&
        (hasWord(line, "infinite") || hasWord(line, "ponder") || !(hasWord(line, "movetime") || hasWord(line, "depth") || hasWord(line, "nodes") || hasWord(line, "wtime") || hasWord(line, "btime") || hasWord(line, "mate")))) {
        printf("info string error: this build needs a limit (movetime, depth, nodes or a clock) after go\n"); printf("bestmove 0000\n"); fflush(stdout); return 1;
    }
    const bool more = uciHandle(line);
    fflush(stdout);
    return more ? 1 : 0;
}
KEXPORT(kestrel_bench) long long kestrel_bench(int bdepth) {          // the nodes of the 3.2 bench (8 positions) to the given depth; 859,322 at depth 12
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "r1bq1rk1/pp2ppbp/2np1np1/8/3NP3/2N1BP2/PPPQ2PP/R3KB1R w KQ - 0 9",
        "2rq1rk1/pp1bppbp/3p1np1/8/2PNP3/2N1B3/PP2BPPP/R2Q1RK1 b - - 0 11",
        "8/5pk1/6p1/8/8/6P1/5PK1/3R4 w - - 0 1",
        "4r1k1/pp3ppp/2p5/8/3P4/2P2N2/PP3PPP/4R1K1 w - - 0 1"};
    long long totalNodes = 0;
    for (const char* f : fens) {
        pos.setFen(f); resizeTT(hashMB); memset(history, 0, sizeof history);
        softMs = hardMs = 0; nodeLimit = 0; maxDepthLimit = bdepth; useExp = false;
        stopFlag = false; t0 = chrono::steady_clock::now(); think();
        totalNodes += nodes_;
    }
    fflush(stdout);
    return totalNodes;
}
'''
text = head + wasm_tail
open(out_path, "w", encoding="utf-8", newline="").write(text)
print("wrote %s: %d lines, %d bytes; changes: %s" % (out_path, text.count("\n"), len(text.encode("utf-8")), ", ".join(log)))
