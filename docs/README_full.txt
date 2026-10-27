Kestrel 1.0 - UCI chess engine, single-file C++17 (generated) + two embedded neural networks (king-bucketed inputs, as in 32)
=============================================================================================================================
Kestrel 1.0 is the FIRST RELEASE UNDER THE NAME KESTREL (internal development version 40.0).  Up to version 39 the engine was called "Claude"; the name was changed at the user's request, "only v40 and later is Kestrel".
KESTREL 1.0 IS A NETWORK RELEASE on top of Claude 39: the CODE (search, evaluation code, every fix and feature of 36.1 - 39) is IDENTICAL to Claude 39 - the generated source differs in 6 changed lines (the
`id name` line "Kestrel 1.0" and the default experience-file name kestrel_experience_v1.dat, which is also the default of the ExperienceFile option; old claude_experience_*.dat files are NOT read);
both embedded networks (net_weights.h, net_weights2.h) are new.  Same architecture, size and speed.  Bench depth 12: 439247 nodes.  The opening book keeps its old file name claude_book.txt (unchanged code).
HOW THE NETWORKS WERE MADE (chain/v40_run.py, v40d_run.py, v40e_run.py: unattended, resumable; the recipe of v33 - v39: self-play, Stockfish labels, warm-started kb6 networks, lambda 0.8, Adam with a cosine learning rate)
  Three attempts in a row, each judged on its own by the same rule against Claude 39 at 20,000 nodes per move (the first two were REJECTED and are not part of the engine, except that their data is in the training set):
  Attempt 1 (v40_run.py, 'engine games')  760,043 NEW positions (labelled with Stockfish depth 7) from 20,388 games of the 100-engine tournaments (5 s + 0.1 s, strong opponents, games BETWEEN OTHER ENGINES,
         plus 218 games of Claude 37.1 against them; duplicates and positions already in the set removed) were added to the 15.16 M positions of Claude 39; nets warm-started from the 39 nets, 10 / 8 epochs.  Result against Claude 39: -0.9 +/- 7.6 Elo (3,200 games): rejected (tests/v40_first_attempt_engine_games_*).
  Attempt 2 (v40d_run.py, 'depth 8')  40,000 self-play games of Claude 39 at 10,000 nodes per move from 40,000 fresh balanced openings (pool D), 1,486,294 positions labelled with Stockfish DEPTH 8 (v33 - v39 used
         depth 7), added to the attempt-1 set (17,403,029 positions in all); nets warm-started from the 39 nets, 10 / 8 epochs.  Result against Claude 39: +3.6 +/- 7.8 (3,200 games): rejected (tests/v40d_depth8_round_*).
  Attempt 3 (v40e_run.py, 'a little more and a little gambits' = Kestrel 1.0)  12,000 more self-play games of Claude 39 at 10,000 nodes per move: 6,000 from still unused pool-D openings and 6,000 from GAMBIT
         openings (chain/prep_pool_g.py: 44 well-known gambit lines, every position from ply 5 on plus 0 - 3 random plies, |evaluation| <= 350 cp: 18,269 openings), 420,420 positions labelled with Stockfish depth 8,
         added to the attempt-2 set (17,823,449 positions in all); both nets warm-started from the attempt-2 nets and trained 4 / 3 more epochs at half the learning rate (5e-4).  Test loss (own split) full net 0.016308,
         residual net 0.016156 (logs: tests/v40e_train_full_net.log, tests/v40e_train_residual_net.log; the attempt-2 nets had 0.016697 / 0.016409 on a different split).
  Rule   fixed before each run: release only if the candidate beats Claude 39 by >= +10 Elo with the 95 % interval above 0 at 20,000 nodes (3,200 games), or by >= +4 and then >= +5 with the interval above 0 pooled
         over a second 3,200 games, AND is positive at 60,000 nodes (1,600 games).  Attempt 3 met it on its first 3,200 games: +11.5 +/- 7.9 at 20,000 nodes, +5.0 +/- 9.9 at 60,000 nodes.
         HONEST CAVEAT: attempt 3 is the third candidate of one line of work (the earlier two measured -0.9 and +3.6), so its first number is the best of three and probably somewhat flattering;
         a second, independent 3,200 games were therefore played afterwards (below).
MEASURED - Kestrel 1.0 against Claude 39 (fastchess, 1 thread, Experience and OwnBook off; raw lines tests/v40_vs_39_raw.txt, tests/v40e_status.txt)
  equal nodes       20,000 nodes per move, first 3,200 games (the games the release decision was made on): +11.5 +/- 7.9 Elo (W 1072  L 966  D 1162);
                    second, independent 3,200 games (played afterwards): +8.8 +/- 8.0 Elo (W 1058  L 977  D 1165);
                    pooled 6,400 games: +10.2 +/- 6.8 (95 % interval, plain win / draw / loss) - the interval is above 0.
                    60,000 nodes per move, 1,600 games: +5.0 +/- 9.9 Elo (W 471  L 448  D 681).
  timed games       3+0.03, 64 MB, 800 games: -12.2 +/- 16.2 (W 243  L 271  D 286); 5+0.1 (the tournament time control), 400 games: +34.0 +/- 21.4 (W 139  L 100  D 161).  Run AFTER the equal-node tests as a sanity check; NOT part of the release rule.
  speed             pinned depth-14 bench (median of 7, tests/v40_bench_pin.txt): Kestrel 1.0 searches 1234988 nodes at 1500592 nodes/s, Claude 39 1110401 nodes at 1480534 nodes/s (same network size and evaluation code; the new nets give a slightly different tree).
  tests             27-check verification passed (tests/v40_verify_log.txt); the long fuzz campaign and the extra tests of 38.1 / 39 were run again on the Kestrel build, see below.
VERIFICATION: 27 checks, 0 failed (GCC, AddressSanitizer + UBSan, 1 and 4 threads; tests/v40_verify_log.txt)
LONG FUZZ CAMPAIGN on the 40 builds (tests/v40_fuzz_campaign_log.txt, tests/fuzz_campaign.sh; 15 runs, none with a failure): UCI fuzzer 4 x 1,500 sequences (GCC) + 1,000 (4 threads)
  + 2 x 700 (ASan/UBSan) + 500 (4 threads, ASan/UBSan); structure-aware FEN fuzzer 1,500 + 800 positions (ASan/UBSan); perft vs python-chess (depth 4 on 450 positions, depth 3 on 4,500);
  evaluation mirror symmetry 4,000 positions; PV / mate legality 1,200 positions (+600 with 4 threads).
EXTRA TESTS (all passed): odd-FEN battery, FEN fuzzer 300 positions, protocol tests 12/12, shared experience file 320/320, soak test flat, clock test 9/9, features tests of 36.2 (31/31),
  36.3 (17/17), 36.5 (11/11), 37.1 (27/27) and 38.1 (16/16), python-chess interoperability test (18/18), Chess960 test (5/5), repetition unit test (9/9), edge-case harness cases 303 | problems 0.
  The 30.1 protocol test, the 37.1 features test and the interoperability test only know the old names, so tests/kestrel_test_v301.py, kestrel_features_test_371.py and kestrel_interop_test.py are copies that look for
  kestrel_experience*, kestrel_final.exe and "Kestrel" in `id name`; everything else is unchanged.
FEATURES (all inherited): Chess960 (UCI_Chess960), Stockfish's repetition rule, UCI_ShowWDL, UCI_AnalyseMode, 4-way bucketed transposition table with ageing, exact speed-ups, Unicode file names,
  `go movetime` with Move Overhead, no book / experience shortcuts in analysis-style searches (see the READMEs of final_v36_5, final_v37_1 and final_v38_1).  NOT available: Syzygy tablebases, NUMA / large pages;
  still ignored: register, debug.  Known and left alone: the table size is rounded DOWN to a power of two (Hash 3000 gives 2048 MB), the largest Hash is 4096 MB, at most 8 threads.
Files: kestrel_final.exe (x86-64-v3, static), kestrel_1_0.cpp (generated source), net_weights.h / net_weights2.h (the NEW networks), claude_book.txt,
  chain/ (ALL patch scripts + base source + trainers + the unattended loops v37_run.py ... v40e_run.py), tests/ (logs and test scripts).
Tests that read training data need three files that are prefixes of the v36 training set (see tests/make_test_inputs.sh).
Rebuilding: g++ -std=c++17 -O3 -march=x86-64-v3 -static -pthread -DNDEBUG -o kestrel_final.exe kestrel_1_0.cpp   (net headers + book next to it)
  Regenerate the source: python chain/build_chain.py kestrel_1_0.cpp 8 40.0   (the net headers are read by the C++ compiler, not by the chain)
