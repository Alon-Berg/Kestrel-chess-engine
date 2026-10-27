r"""Prepares a copy of src/ for the builds that are NOT x86-64-v3 Windows (see docs/BUILDING.md).  src/ itself is never edited.
  --portable   no BMI2 instruction is ever executed: _pext_u64 is replaced by a plain-C function and pextIsFast() returns false
               (the v3 build uses PEXT only on Intel and AMD Zen 3+; everywhere else it already uses the shift-based attack tables, so the node counts do not change).
  --bigstack   std::thread is replaced by a pthread wrapper with an 8 MB stack (the default thread stack is 128 KB with musl and 512 KB on macOS;
               one search frame is about 2.7 KB and a search can go 128 plies deep).  For Linux and macOS.
  --shim       writes shim/immintrin.h and shim/cpuid.h for non-x86 targets (ARM): compile with  -I OUTDIR/shim
usage: python scripts/patch_source.py OUTDIR [--portable] [--bigstack] [--shim]"""
import os, shutil, sys

PEXT_SOFT = ('static inline U64 pextSoft(U64 v, U64 m) {                       // portable build: plain-C PEXT, only reachable when usePext is true, which never happens here\n'
             '    U64 r = 0; for (U64 b = 1; m; b += b) { U64 l = m & (0 - m); if (v & l) r |= b; m &= m - 1; }\n'
             '    return r;\n}\n')

def portable_source(text):
    nl = "\r\n" if "\r\n" in text else "\n"
    n = text.count("_pext_u64(")
    assert n == 4, n
    text = text.replace("_pext_u64(", "pextSoft(")
    anchor = "static bool usePext = false;" + nl
    assert text.count(anchor) == 1
    text = text.replace(anchor, PEXT_SOFT.replace("\n", nl) + anchor)
    anchor = '    if (const char* e = getenv("CLAUDE_NO_PEXT")) if (e[0] == \'1\') return false;' + nl
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + "    return false;                                // portable build: BMI2 is not assumed" + nl)
    return text

KTHREAD = r'''#include <pthread.h>
#include <stdexcept>
#include <exception>
struct KThread {      // non-Windows builds: std::thread with an 8 MB stack (the default thread stack is 128 KB with musl and 512 KB on macOS; one search frame is ~2.7 KB and a search can reach 128 plies)
    struct Fn { virtual void run() = 0; virtual ~Fn() {} };
    template <class B> struct Impl : Fn { B b; explicit Impl(B x) : b(x) {} void run() override { b(); } };
    pthread_t t; bool on = false; Fn* fn = nullptr;
    KThread() {}
    template <class F, class... A> explicit KThread(F f, A... a) {
        auto bound = [f, a...]() { f(a...); };
        fn = new Impl<decltype(bound)>(bound);
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, (size_t)8 << 20);
        int rc = pthread_create(&t, &at, [](void* p) -> void* { static_cast<Fn*>(p)->run(); return nullptr; }, fn);
        pthread_attr_destroy(&at);
        if (rc) { delete fn; fn = nullptr; throw std::runtime_error("cannot start thread"); }
        on = true;
    }
    KThread(KThread&& o) noexcept : t(o.t), on(o.on), fn(o.fn) { o.on = false; o.fn = nullptr; }
    KThread& operator=(KThread&& o) noexcept { if (on) std::terminate(); t = o.t; on = o.on; fn = o.fn; o.on = false; o.fn = nullptr; return *this; }
    bool joinable() const { return on; }
    void join() { if (on) { pthread_join(t, nullptr); on = false; delete fn; fn = nullptr; } }
    ~KThread() { if (on) std::terminate(); }
};
'''

def bigstack_source(text):
    nl = "\r\n" if "\r\n" in text else "\n"
    for old, new in (("    std::thread helperTh[MAXTH];", "    KThread helperTh[MAXTH];"),
                     ("struct HelperJoin { std::thread* h;", "struct HelperJoin { KThread* h;"),
                     ("helperTh[k] = std::thread(helperFns[k], k);", "helperTh[k] = KThread(helperFns[k], k);"),
                     ("    thread worker;", "    KThread worker;"),
                     ("worker = thread(perftWorker, pn);", "worker = KThread(perftWorker, pn);"),
                     ("worker = thread(thinkSafe);", "worker = KThread(thinkSafe);"),
                     ("typedef unsigned long long U64;" + nl, "typedef unsigned long long U64;" + nl + KTHREAD.replace("\n", nl))):
        assert text.count(old) == 1, old
        text = text.replace(old, new)
    return text

SHIM_IMMINTRIN = "#pragma once\n#define _MM_HINT_T0 3\nstatic inline void _mm_prefetch(const char* p, int) { __builtin_prefetch(p); }\n"
SHIM_CPUID = "#pragma once\nstatic inline int __get_cpuid(unsigned, unsigned* a, unsigned* b, unsigned* c, unsigned* d) { *a = *b = *c = *d = 0; return 0; }\n"

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]; flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if len(args) != 1 or not flags <= {"--portable", "--bigstack", "--shim"}: sys.exit(__doc__)
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"); out = args[0]
    os.makedirs(out, exist_ok=True)
    for f in ("net_weights.h", "net_weights2.h", "claude_book.txt"): shutil.copy(os.path.join(src, f), out)
    t = open(os.path.join(src, "kestrel_1_0.cpp"), encoding="utf-8", newline="").read()
    if "--portable" in flags: t = portable_source(t)
    if "--bigstack" in flags: t = bigstack_source(t)
    open(os.path.join(out, "kestrel_1_0.cpp"), "w", encoding="utf-8", newline="").write(t)
    if "--shim" in flags:
        os.makedirs(os.path.join(out, "shim"), exist_ok=True)
        open(os.path.join(out, "shim", "immintrin.h"), "w").write(SHIM_IMMINTRIN); open(os.path.join(out, "shim", "cpuid.h"), "w").write(SHIM_CPUID)
    print("written to", out)
