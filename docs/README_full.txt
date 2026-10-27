Kestrel 1.1 - UCI chess engine, single-file C++17 (generated) + two embedded neural networks (king-bucketed inputs, as in 32)
=============================================================================================================================
Kestrel 1.1 (internal development version 40.1) is a BUG-FIX RELEASE on top of Kestrel 1.0 (40.0).  The networks, the book, the search and the evaluation of normal positions are UNCHANGED: in 800 fixed-node
searches of Kestrel 1.0 against 1.1 (400 positions, 60,000 nodes and depth 9 each, tests/identity_1_0_vs_1_1_output.txt) 797 give exactly the same score, node count, best move and principal variation;
the 3 that differ are K+N+N against K endings (see 4).  Bench depth 12: 439247 nodes, the same as 1.0.  No strength match was played: there is no change that could make the engine stronger or weaker
in a normal game.  The generated source differs from 1.0 in 133 changed lines (the fixes below plus the version name and the experience-file name kestrel_experience_v1_1.dat: old experience files are NOT read).
THE BUG SEARCH of 2026-10-27 ("search for bugs"): tests/audit_probe.py (83 black-box UCI cases: CRLF input, closed stdin, move lists of 3,000 / 12,000 / 10,000 plies, huge / negative / junk numbers, 24 odd options,
  19 broken positions, 2 MB lines, binary garbage, back-to-back and overlapping commands) and tests/audit_sem.py (31 rules / protocol checks against python-chess: no-legal-move answers, the 50-move rule next to mate,
  insufficient material, searchmoves, go mate, MultiPV, info-line sanity, pondering, UCI_LimitStrength, promotions, en passant, castling), then a port of the source to Linux, macOS and ARM.  Kestrel 1.0 passed
  all of audit_probe (after correcting four of the audit's own expectations: an unusable limit such as `go depth -3` means "no limit" and the search runs until `stop`, as for a bare `go`; a Ctrl-Z byte ends a text pipe on
  Windows for every program) and audit_sem except for the evaluation of K+N+N v K.  What it found:
  1. PORTABILITY (affects building on other systems, not the Windows exe).  The source only compiled with BMI2 enabled: `_pext_u64` was called unguarded, <immintrin.h> and <cpuid.h> do not exist on ARM, and the PEXT
     switch treated every Intel CPU as having BMI2.  Now PEXT code exists only when the compiler targets BMI2 (`__BMI2__`), CPUID only on x86, the transposition-table prefetch is __builtin_prefetch (the same
     prefetcht0).  The source builds unpatched, without a warning, for x86-64, x86-64-v2 and x86-64-v3 with MinGW on Windows (tests/kestrel_features_test_401.py: depth-12 bench 439247 nodes on every level) and compiles
     for Linux (musl) x86-64 / ARM64 and macOS x86-64 / ARM64 with Zig (never run: no such machine).  The Windows x86-64-v3 exe is instruction-for-instruction what it was.
  2. THREAD STACKS (affects Linux with musl / Alpine and macOS).  Searches run in worker threads; musl gives a thread 128 KB and macOS 512 KB, one search frame is about 2.7 KB and a search can go 128 plies deep.
     A test build with a 128 KB stack died with a stack overflow in 6 of 8 endgame / middlegame test positions, at depth 21 - 31 (tests/stack_probe.py).  Every non-Windows build now starts its threads with an
     8 MB stack (`KThread`, a small pthread wrapper; on Windows it is std::thread as before).  Tested on Windows with the pthread wrapper switched on, 1 and 4 threads, a 128 KB main stack.
  3. FILES NEXT TO THE EXE on Linux and macOS.  The folder of the book and of the experience file came from argv[0] only: started by a bare name through PATH, or through a symlink, the engine looked in the current
     directory.  Now /proc/self/exe (Linux) and _NSGetExecutablePath (macOS) are used first, argv[0] is the fallback.  (Compiled for Linux and macOS, never run there: no such machine was available.)
  4. K+N+N AGAINST K (affects play).  Two knights against a bare king were evaluated about +3.4 pawns (+3.36 at depth 10) although there is no forced mate: the engine could trade into the ending believing it was winning.
     The static evaluation of exactly that material is now 0.  It is not a dead position, so `isDraw` is unchanged and the search still finds a mate the opponent allows (tests: mate in 1 found, K+N+N+P v K is
     still +6.4).
  Not bugs (looked at and left alone): at halfmove clock 100 a mate in one is still reported as a mate (Stockfish does the same: the draw rule is applied below the root); an illegal FEN in `position` is
  ignored and the previous position stays; `go depth 0`-like and negative limits mean no limit.
VERIFICATION: 27 checks, 0 failed (GCC, AddressSanitizer + UBSan, 1 and 4 threads; tests/v41_verify_log.txt)
LONG FUZZ CAMPAIGN on the 41 builds (tests/v41_fuzz_campaign_log.txt, tests/fuzz_campaign.sh; 15 runs, none with a failure): UCI fuzzer 4 x 1,500 sequences (GCC) + 1,000 (4 threads)
  + 2 x 700 (ASan/UBSan) + 500 (4 threads, ASan/UBSan); structure-aware FEN fuzzer 1,500 + 800 positions (ASan/UBSan); perft vs python-chess (depth 4 on 450 positions, depth 3 on 4,500);
  evaluation mirror symmetry 4,000 positions; PV / mate legality 1,200 positions (+600 with 4 threads).
EXTRA TESTS (all passed): odd-FEN battery, FEN fuzzer 300 positions, protocol tests 12/12, shared experience file 320/320, soak test flat, clock test 9/9, features tests of 36.2 (31/31),
  36.3 (17/17), 36.5 (11/11), 37.1 (27/27) and 38.1 (16/16), python-chess interoperability test (18/18), Chess960 test (5/5), repetition unit test (9/9), edge-case harness cases 303 | problems 0,
  the new 40.1 features test (20/20: tests/kestrel_features_test_401.py), audit_probe 83/83, audit_sem 31/31, identity test against 1.0 (above).
FEATURES (all inherited): Chess960 (UCI_Chess960), Stockfish's repetition rule, UCI_ShowWDL, UCI_AnalyseMode, 4-way bucketed transposition table with ageing, exact speed-ups, Unicode file names,
  `go movetime` with Move Overhead, no book / experience shortcuts in analysis-style searches.  NOT available: Syzygy tablebases, NUMA / large pages; still ignored: register, debug.
  Known and left alone: the table size is rounded DOWN to a power of two (Hash 3000 gives 2048 MB), the largest Hash is 4096 MB, at most 8 threads.
Files: kestrel_final.exe (x86-64-v3, static), kestrel_1_1.cpp (generated source), net_weights.h / net_weights2.h (the networks of 1.0), claude_book.txt (old file name, unchanged),
  chain/ (ALL patch scripts + base source + trainers + the unattended loops + the build / packaging scripts of the other platforms), tests/ (logs and test scripts).
Rebuilding: g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel_final.exe kestrel_1_1.cpp   (net headers + book next to it; any -march works now, no patch step is needed)
  Regenerate the source: python chain/build_chain.py kestrel_1_1.cpp 8 40.1   (the net headers are read by the C++ compiler, not by the chain)
