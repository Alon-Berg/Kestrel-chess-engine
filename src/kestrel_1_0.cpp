// Claude 1.0 - a from-scratch UCI chess engine (bitboards, alpha-beta, hand-made eval).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <immintrin.h>
#include <cpuid.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
using namespace std;
typedef unsigned long long U64;

static inline int lsb(U64 b) { return __builtin_ctzll(b); }
static inline int popc(U64 b) { return __builtin_popcountll(b); }
static inline U64 bswap(U64 b) { return __builtin_bswap64(b); }

enum { CAP = 1 << 15, EPF = 1 << 16, CAS = 1 << 17, DPP = 1 << 18 };
static bool chess960 = false;                      // UCI_Chess960
static int g960Rook[4] = {7, 0, 63, 56};           // castling-rook squares (K, Q, k, q) of the current game, for writing castling moves
enum { EMPTYP = 12 };
const U64 FILE_A = 0x0101010101010101ULL, FILE_H = 0x8080808080808080ULL;

U64 KnightA[64], KingA[64], PawnA[2][64], FileBB[8], AdjFiles[8], PassedMask[2][64], ShieldMask[2][64];
U64 FileM[64], DiagM[64], AntiM[64];
unsigned char RankAtt[8][256];
U64 ZP[12][64], ZC[16], ZE[8], ZS;
int CMask[64];

static U64 rngState = 0x9E3779B97F4A7C15ULL;
static U64 rnd() { rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17; return rngState * 0x2545F4914F6CDD1DULL; }

void initTables() {
    for (int f = 0; f < 8; f++) FileBB[f] = FILE_A << f;
    for (int f = 0; f < 8; f++) { U64 a = 0; if (f > 0) a |= FileBB[f - 1]; if (f < 7) a |= FileBB[f + 1]; AdjFiles[f] = a; }
    int dr[8] = {2, 2, 1, 1, -2, -2, -1, -1}, df[8] = {1, -1, 2, -2, 1, -1, 2, -2};
    for (int sq = 0; sq < 64; sq++) {
        int r = sq >> 3, f = sq & 7;
        U64 k = 0, kg = 0;
        for (int i = 0; i < 8; i++) { int nr = r + dr[i], nf = f + df[i]; if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) k |= 1ULL << (nr * 8 + nf); }
        for (int a = -1; a <= 1; a++) for (int b = -1; b <= 1; b++) { if (!a && !b) continue; int nr = r + a, nf = f + b; if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) kg |= 1ULL << (nr * 8 + nf); }
        KnightA[sq] = k; KingA[sq] = kg;
        U64 pw = 0, pb = 0;
        if (r < 7) { if (f > 0) pw |= 1ULL << (sq + 7); if (f < 7) pw |= 1ULL << (sq + 9); }
        if (r > 0) { if (f > 0) pb |= 1ULL << (sq - 9); if (f < 7) pb |= 1ULL << (sq - 7); }
        PawnA[0][sq] = pw; PawnA[1][sq] = pb;
        FileM[sq] = FileBB[f] & ~(1ULL << sq);
        U64 dm = 0, am = 0;
        for (int d = 1; d < 8; d++) {
            if (r + d < 8 && f + d < 8) dm |= 1ULL << ((r + d) * 8 + f + d);
            if (r - d >= 0 && f - d >= 0) dm |= 1ULL << ((r - d) * 8 + f - d);
            if (r + d < 8 && f - d >= 0) am |= 1ULL << ((r + d) * 8 + f - d);
            if (r - d >= 0 && f + d < 8) am |= 1ULL << ((r - d) * 8 + f + d);
        }
        DiagM[sq] = dm; AntiM[sq] = am;
        U64 p0 = 0, p1 = 0, s0 = 0, s1 = 0;
        for (int rr = r + 1; rr < 8; rr++) for (int ff = max(0, f - 1); ff <= min(7, f + 1); ff++) p0 |= 1ULL << (rr * 8 + ff);
        for (int rr = r - 1; rr >= 0; rr--) for (int ff = max(0, f - 1); ff <= min(7, f + 1); ff++) p1 |= 1ULL << (rr * 8 + ff);
        PassedMask[0][sq] = p0; PassedMask[1][sq] = p1;
        for (int d = 1; d <= 2; d++) for (int ff = max(0, f - 1); ff <= min(7, f + 1); ff++) {
            if (r + d < 8) s0 |= 1ULL << ((r + d) * 8 + ff);
            if (r - d >= 0) s1 |= 1ULL << ((r - d) * 8 + ff);
        }
        ShieldMask[0][sq] = s0; ShieldMask[1][sq] = s1;
    }
    for (int f = 0; f < 8; f++) for (int occ = 0; occ < 256; occ++) {
        int a = 0;
        for (int x = f + 1; x < 8; x++) { a |= 1 << x; if ((occ >> x) & 1) break; }
        for (int x = f - 1; x >= 0; x--) { a |= 1 << x; if ((occ >> x) & 1) break; }
        RankAtt[f][occ] = (unsigned char)a;
    }
    for (int i = 0; i < 12; i++) for (int j = 0; j < 64; j++) ZP[i][j] = rnd();
    for (int i = 0; i < 16; i++) ZC[i] = rnd();
    for (int i = 0; i < 8; i++) ZE[i] = rnd();
    ZS = rnd();
    for (int i = 0; i < 64; i++) CMask[i] = 15;
    CMask[4] &= ~3; CMask[0] &= ~2; CMask[7] &= ~1; CMask[60] &= ~12; CMask[56] &= ~8; CMask[63] &= ~4;
}

static inline U64 slide(U64 occ, int sq, U64 mask) {
    U64 r = 1ULL << sq, f = occ & mask, rv = bswap(f);
    f -= r; rv -= bswap(r); f ^= bswap(rv);
    return f & mask;
}
static inline U64 bishopAttOld(int sq, U64 occ) { return slide(occ, sq, DiagM[sq]) | slide(occ, sq, AntiM[sq]); }
static inline U64 rookAttOld(int sq, U64 occ) {
    int rank = sq >> 3, file = sq & 7;
    return ((U64)RankAtt[file][(occ >> (rank * 8)) & 0xFF] << (rank * 8)) | slide(occ, sq, FileM[sq]);
}
static bool usePext = false;
static U64 rookTabP[102400], bishopTabP[5248], rookMaskP[64], bishopMaskP[64]; static int rookOffP[64], bishopOffP[64];
static bool pextIsFast() {                    // PEXT is one cheap instruction on Intel and on AMD Zen 3+; Zen 1 / 2 (family 0x17) emulate it in microcode
    if (const char* e = getenv("CLAUDE_NO_PEXT")) if (e[0] == '1') return false;
    if (const char* e = getenv("CLAUDE_FORCE_PEXT")) if (e[0] == '1') return true;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0, &a, &b, &c, &d)) return false;
    const bool intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e, amd = b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    unsigned fam = (a >> 8) & 0xF; if (fam == 0xF) fam += (a >> 20) & 0xFF;
    return intel || (amd && fam >= 0x19);
}
static void initPext() {
    usePext = pextIsFast();
    if (!usePext) return;
    int ro = 0, bo = 0;
    for (int sq = 0; sq < 64; sq++) {
        int r = sq >> 3, f = sq & 7;
        const U64 edgeF = FILE_A | FILE_H, edgeR = 0xFFULL | (0xFFULL << 56);
        U64 rm = (((0xFFULL << (r * 8)) & ~edgeF) | (FileBB[f] & ~edgeR)) & ~(1ULL << sq);
        U64 bm = (DiagM[sq] | AntiM[sq]) & ~(edgeF | edgeR);
        rookMaskP[sq] = rm; bishopMaskP[sq] = bm; rookOffP[sq] = ro; bishopOffP[sq] = bo;
        U64 x = 0; do { rookTabP[ro + _pext_u64(x, rm)] = rookAttOld(sq, x); x = (x - rm) & rm; } while (x);
        ro += 1 << popc(rm);
        x = 0; do { bishopTabP[bo + _pext_u64(x, bm)] = bishopAttOld(sq, x); x = (x - bm) & bm; } while (x);
        bo += 1 << popc(bm);
    }
}
static inline U64 bishopAtt(int sq, U64 occ) { return usePext ? bishopTabP[bishopOffP[sq] + _pext_u64(occ, bishopMaskP[sq])] : bishopAttOld(sq, occ); }
static inline U64 rookAtt(int sq, U64 occ) { return usePext ? rookTabP[rookOffP[sq] + _pext_u64(occ, rookMaskP[sq])] : rookAttOld(sq, occ); }
static inline U64 queenAtt(int sq, U64 occ) { return bishopAtt(sq, occ) | rookAtt(sq, occ); }

// ---------------------------------------------------------------- neural network evaluation (NNUE-style)
#if __has_include("net_weights.h")
#include "net_weights.h"
#define HAVE_NET 1
#endif
#if __has_include("net_weights2.h")
#include "net_weights2.h"
#define HAVE_NET2 1
#endif
#ifdef NET_KB
const int NN_KB = NET_KB;
#else
const int NN_KB = 1;
#endif
#ifdef HAVE_NET2
#ifdef NET2_KB
static_assert(NET2_KB == NN_KB, "both nets must use the same number of king buckets");
#else
static_assert(NN_KB == 1, "both nets must use the same number of king buckets");
#endif
#endif
#ifdef NET_KBMAP
static const unsigned char nnKBMap[32] = NET_KBMAP;       // (king rank) * 4 + (king file after mirroring) -> bucket
#else
static const unsigned char nnKBMap[32] = {0};
#endif
const int NN_H = 64, NN_IN = 768, NN_INB = 768 * NN_KB;
alignas(32) static int16_t nnW1[NN_INB][NN_H];
alignas(32) static int32_t nnB1[NN_H];
alignas(32) static float nnW2[2 * NN_H];
alignas(32) static int16_t nnW2q[8][2 * NN_H];
static const float NN_QB = 1024.0f;
static float nnB2[8] = {0};
alignas(32) static int32_t nnPS[NN_IN];
static bool nnueReady = false, netLoaded = false;
alignas(32) static int16_t nnW1b[NN_INB][NN_H];
alignas(32) static int32_t nnB1b[NN_H];
alignas(32) static int16_t nnW2qb[8][2 * NN_H];
static float nnB2b[8] = {0};
alignas(32) static int32_t nnPSb[NN_IN];
static bool net2Loaded = false, useNet2 = true;
static const float NN_QA = 256.0f;
#ifdef NET_SCRELU
static const bool nnSq1 = true;
#else
static const bool nnSq1 = false;
#endif
#ifdef NET2_SCRELU
static const bool nnSq2 = true;
#else
static const bool nnSq2 = false;
#endif
#ifdef NET_NB
static const int nnNB1 = NET_NB;
#define NET_B2_AT(b) NET_B2[b]
#else
static const int nnNB1 = 1;
#define NET_B2_AT(b) NET_B2
#endif
#ifdef NET2_NB
static const int nnNB2 = NET2_NB;
#define NET2_B2_AT(b) NET2_B2[b]
#else
static const int nnNB2 = 1;
#define NET2_B2_AT(b) NET2_B2
#endif
static void initNet() {
#ifdef HAVE_NET2
    for (int i = 0; i < NN_INB; i++) for (int j = 0; j < NN_H; j++) nnW1b[i][j] = NET2_W1[i * NN_H + j];
    for (int j = 0; j < NN_H; j++) nnB1b[j] = NET2_B1[j];
    for (int b = 0; b < nnNB2; b++) { for (int j = 0; j < 2 * NN_H; j++) nnW2qb[b][j] = (int16_t)lrintf(NET2_W2[b * 2 * NN_H + j] * NN_QB); nnB2b[b] = NET2_B2_AT(b); }
    for (int i = 0; i < NN_IN; i++) nnPSb[i] = NET2_PS[i];
    net2Loaded = true;
#endif
#ifdef HAVE_NET
    for (int i = 0; i < NN_INB; i++) for (int j = 0; j < NN_H; j++) nnW1[i][j] = NET_W1[i * NN_H + j];
    for (int j = 0; j < NN_H; j++) nnB1[j] = NET_B1[j];
    for (int j = 0; j < 2 * NN_H; j++) nnW2[j] = NET_W2[j];
    for (int b = 0; b < nnNB1; b++) { for (int j = 0; j < 2 * NN_H; j++) nnW2q[b][j] = (int16_t)lrintf(NET_W2[b * 2 * NN_H + j] * NN_QB); nnB2[b] = NET_B2_AT(b); }
    for (int i = 0; i < NN_IN; i++) nnPS[i] = NET_PS[i];
    netLoaded = true; nnueReady = true;
#endif
}
// feature index of piece pc (0..11, color*6+type) on square sq from perspective persp (0 = white, 1 = black)
static inline int featIdx(int persp, int pc, int sq) {
    int c = pc >= 6, t = pc % 6;
    return (((c == persp) ? 0 : 6) + t) * 64 + (persp == 0 ? sq : (sq ^ 56));
}
// the same feature (0..767) with the board mirrored horizontally when mir is set
static inline int nnBase(int persp, int pc, int sq, int mir) {
    int c = pc >= 6, t = pc % 6, q = (persp == 0 ? sq : (sq ^ 56)) ^ (mir ? 7 : 0);
    return (((c == persp) ? 0 : 6) + t) * 64 + q;
}

extern int Wmg[485], Weg[485];
static inline void ttPrefetch(U64 hash);
struct Undo { int move, cap, castle, ep, half; U64 hash; };

struct Pos {
    U64 P[12], Occ[2], All;
    alignas(32) int32_t accRing[256][2][NN_H];       // accumulators of the last 256 plies: make() fills the next slot, unmake() steps back
    alignas(32) int32_t acc2Ring[256][2][NN_H];
    int32_t (*acc)[NN_H] = accRing[0]; int32_t (*acc2)[NN_H] = acc2Ring[0]; int ai = 0;      // the current slot
    int32_t psq[2];
    int32_t psq2[2];
    Pos() = default;
    Pos(const Pos& o) { std::memcpy((void*)this, (const void*)&o, sizeof(Pos)); acc = accRing[ai]; acc2 = acc2Ring[ai]; }
    Pos& operator=(const Pos& o) { if (this != &o) { std::memcpy((void*)this, (const void*)&o, sizeof(Pos)); acc = accRing[ai]; acc2 = acc2Ring[ai]; } return *this; }
    inline void setSlot(int i) { ai = i; acc = accRing[i]; acc2 = acc2Ring[i]; }
    inline void copySlot(int from, int to) { std::memcpy(accRing[to], accRing[from], sizeof accRing[0]); std::memcpy(acc2Ring[to], acc2Ring[from], sizeof acc2Ring[0]); }
    int kOff[2] = {0, 0}, kMir[2] = {0, 0};    // king bucket offset (bucket * 768) and horizontal mirroring of each perspective
    int pstMg = 0, pstEg = 0, phaseAcc = 0;
    int B[64];
    int cRook[4] = {7, 0, 63, 56}, cKing[2] = {4, 60}, cmask[64];      // castling: rook squares (K, Q, k, q), king squares, rights lost when a square is touched
    int side, castle, ep, half, ply, rootPly = 0;      // rootPly: ply of the root position of the current search (set by think())
    U64 hash;
    Undo U[4096];
    U64 HH[4096];
    struct AccSave { int32_t psq[2], psq2[2]; int pstMg, pstEg, phaseAcc, kOff[2], kMir[2]; };
    AccSave accSave[256];            // make() saves the small scalar state here and unmake() restores it (make / unmake are strictly nested, depth < 256)

    void clear() {
        memset(P, 0, sizeof P); Occ[0] = Occ[1] = 0; All = 0;
        for (int i = 0; i < 64; i++) B[i] = EMPTYP;
        side = 0; castle = 0; ep = -1; half = 0; ply = 0; hash = 0; rootPly = 0;
        cRook[0] = 7; cRook[1] = 0; cRook[2] = 63; cRook[3] = 56; cKing[0] = 4; cKing[1] = 60;
        for (int i = 0; i < 64; i++) cmask[i] = 15;
        cmask[4] &= ~3; cmask[0] &= ~2; cmask[7] &= ~1; cmask[60] &= ~12; cmask[56] &= ~8; cmask[63] &= ~4;
        kOff[0] = kOff[1] = 0; kMir[0] = kMir[1] = 0; setSlot(0);
    }
    template<bool NN = true> inline void add(int pc, int sq) {
        U64 b = 1ULL << sq; P[pc] |= b; Occ[pc >= 6] |= b; All |= b; B[sq] = pc; hash ^= ZP[pc][sq];
        { int t = pc % 6, ix = t * 64 + (pc < 6 ? sq : (sq ^ 56)), sg = pc < 6 ? 1 : -1;
          pstMg += sg * Wmg[ix]; pstEg += sg * Weg[ix]; phaseAcc += (t == 1 || t == 2) ? 1 : (t == 3 ? 2 : (t == 4 ? 4 : 0)); }
        if (pc % 6 == 5) orient(pc / 6, sq);          // a king arrives: from now on its side sees the board from this king square
        if (NN && nnueReady) { int i0 = nnBase(0, pc, sq, kMir[0]), i1 = nnBase(1, pc, sq, kMir[1]); const int16_t* w0 = nnW1[kOff[0] + i0]; const int16_t* w1 = nnW1[kOff[1] + i1];
            psq[0] += nnPS[i0]; psq[1] += nnPS[i1];
            for (int j = 0; j < NN_H; j++) { acc[0][j] += w0[j]; acc[1][j] += w1[j]; }
            if (net2Loaded) { const int16_t* v0 = nnW1b[kOff[0] + i0]; const int16_t* v1 = nnW1b[kOff[1] + i1]; psq2[0] += nnPSb[i0]; psq2[1] += nnPSb[i1];
                for (int j = 0; j < NN_H; j++) { acc2[0][j] += v0[j]; acc2[1][j] += v1[j]; } } }
    }
    template<bool NN = true> inline void rem(int pc, int sq) {
        U64 b = 1ULL << sq; P[pc] ^= b; Occ[pc >= 6] ^= b; All ^= b; B[sq] = EMPTYP; hash ^= ZP[pc][sq];
        { int t = pc % 6, ix = t * 64 + (pc < 6 ? sq : (sq ^ 56)), sg = pc < 6 ? 1 : -1;
          pstMg -= sg * Wmg[ix]; pstEg -= sg * Weg[ix]; phaseAcc -= (t == 1 || t == 2) ? 1 : (t == 3 ? 2 : (t == 4 ? 4 : 0)); }
        if (NN && nnueReady) { int i0 = nnBase(0, pc, sq, kMir[0]), i1 = nnBase(1, pc, sq, kMir[1]); const int16_t* w0 = nnW1[kOff[0] + i0]; const int16_t* w1 = nnW1[kOff[1] + i1];
            psq[0] -= nnPS[i0]; psq[1] -= nnPS[i1];
            for (int j = 0; j < NN_H; j++) { acc[0][j] -= w0[j]; acc[1][j] -= w1[j]; }
            if (net2Loaded) { const int16_t* v0 = nnW1b[kOff[0] + i0]; const int16_t* v1 = nnW1b[kOff[1] + i1]; psq2[0] -= nnPSb[i0]; psq2[1] -= nnPSb[i1];
                for (int j = 0; j < NN_H; j++) { acc2[0][j] -= v0[j]; acc2[1][j] -= v1[j]; } } }
    }
    inline void addBB(int pc, int sq) { U64 b = 1ULL << sq; P[pc] |= b; Occ[pc >= 6] |= b; All |= b; B[sq] = pc; }
    inline void remBB(int pc, int sq) { U64 b = 1ULL << sq; P[pc] ^= b; Occ[pc >= 6] ^= b; All ^= b; B[sq] = EMPTYP; }
    // one fused pass over the accumulators for an ordinary move (not a king move, not castling): + the piece on its new square, - the piece on its old square,
    // - a captured piece.  The integer sums are exactly those of the separate add / rem calls.
    inline void nnDelta(int pai, int nai, int pcAdd, int sqAdd, int pcFrom, int sqFrom, int pcCap, int sqCap) {
        for (int p = 0; p < 2; p++) {
            const int ia = nnBase(p, pcAdd, sqAdd, kMir[p]), ifr = nnBase(p, pcFrom, sqFrom, kMir[p]);
            const int16_t* wa = nnW1[kOff[p] + ia]; const int16_t* wf = nnW1[kOff[p] + ifr];
            if (pcCap != EMPTYP) {
                const int ic = nnBase(p, pcCap, sqCap, kMir[p]); const int16_t* wc = nnW1[kOff[p] + ic];
                psq[p] += nnPS[ia] - nnPS[ifr] - nnPS[ic];
                for (int j = 0; j < NN_H; j++) accRing[nai][p][j] = accRing[pai][p][j] + (int32_t)wa[j] - wf[j] - wc[j];
            } else {
                psq[p] += nnPS[ia] - nnPS[ifr];
                for (int j = 0; j < NN_H; j++) accRing[nai][p][j] = accRing[pai][p][j] + (int32_t)wa[j] - wf[j];
            }
            if (net2Loaded) {
                const int16_t* va = nnW1b[kOff[p] + ia]; const int16_t* vf = nnW1b[kOff[p] + ifr];
                if (pcCap != EMPTYP) {
                    const int ic = nnBase(p, pcCap, sqCap, kMir[p]); const int16_t* vc = nnW1b[kOff[p] + ic];
                    psq2[p] += nnPSb[ia] - nnPSb[ifr] - nnPSb[ic];
                    for (int j = 0; j < NN_H; j++) acc2Ring[nai][p][j] = acc2Ring[pai][p][j] + (int32_t)va[j] - vf[j] - vc[j];
                } else {
                    psq2[p] += nnPSb[ia] - nnPSb[ifr];
                    for (int j = 0; j < NN_H; j++) acc2Ring[nai][p][j] = acc2Ring[pai][p][j] + (int32_t)va[j] - vf[j];
                }
            }
        }
    }
    void refreshPst() {
        pstMg = pstEg = phaseAcc = 0;
        for (int sq = 0; sq < 64; sq++) if (B[sq] != EMPTYP) { int pc = B[sq], t = pc % 6, ix = t * 64 + (pc < 6 ? sq : (sq ^ 56)), sg = pc < 6 ? 1 : -1;
            pstMg += sg * Wmg[ix]; pstEg += sg * Weg[ix]; phaseAcc += (t == 1 || t == 2) ? 1 : (t == 3 ? 2 : (t == 4 ? 4 : 0)); }
    }
    inline void orient(int c, int ksq) {
        int rel = c == 0 ? ksq : (ksq ^ 56), file = rel & 7, rank = rel >> 3;
        int mir = (NN_KB > 1 && file >= 4) ? 1 : 0, f = mir ? 7 - file : file;
        kMir[c] = mir; kOff[c] = NN_KB > 1 ? nnKBMap[rank * 4 + (f & 3)] * NN_IN : 0;
    }
    inline int kKey(int c) const { return kOff[c] * 2 + kMir[c]; }
    void refreshPersp(int p) {                       // rebuild one side's accumulators from scratch (its king orientation is current)
        for (int j = 0; j < NN_H; j++) acc[p][j] = nnB1[j];
        psq[p] = 0;
        if (net2Loaded) { for (int j = 0; j < NN_H; j++) acc2[p][j] = nnB1b[j]; psq2[p] = 0; }
        for (int sq = 0; sq < 64; sq++) if (B[sq] != EMPTYP) {
            int i = nnBase(p, B[sq], sq, kMir[p]); const int16_t* w = nnW1[kOff[p] + i];
            psq[p] += nnPS[i];
            for (int j = 0; j < NN_H; j++) acc[p][j] += w[j];
            if (net2Loaded) { const int16_t* v = nnW1b[kOff[p] + i]; psq2[p] += nnPSb[i]; for (int j = 0; j < NN_H; j++) acc2[p][j] += v[j]; }
        }
    }
    void refresh() {
        for (int c = 0; c < 2; c++) { if (P[c * 6 + 5]) orient(c, lsb(P[c * 6 + 5])); else { kOff[c] = 0; kMir[c] = 0; } }
        if (!nnueReady) return;
        refreshPersp(0); refreshPersp(1);
    }
    U64 computeHash() {
        U64 h = 0;
        for (int s = 0; s < 64; s++) if (B[s] != EMPTYP) h ^= ZP[B[s]][s];
        h ^= ZC[castle]; if (ep >= 0) h ^= ZE[ep & 7]; if (side) h ^= ZS;
        return h;
    }
    void setFen(const string& fen) {
        clear();
        istringstream is(fen);
        string board, stm, cast, eps; int hm = 0, fm = 1;
        is >> board >> stm >> cast >> eps >> hm >> fm;
        int sq = 56;
        for (char c : board) {
            if (c == '/') { sq -= 16; continue; }
            if (c >= '1' && c <= '8') { sq += c - '0'; continue; }
            const char* s = strchr("PNBRQKpnbrqk", c);
            if (s) { int pc = (int)(s - "PNBRQKpnbrqk"); P[pc] |= 1ULL << sq; Occ[pc >= 6] |= 1ULL << sq; All |= 1ULL << sq; B[sq] = pc; sq++; }
        }
        side = (stm == "b");
        for (int c = 0; c < 2; c++) { int base = c * 56; for (int f = 0; f < 8; f++) if (B[base + f] == c * 6 + 5) { cKing[c] = base + f; break; } }
        for (char ch : cast) {
            int c = (ch >= 'A' && ch <= 'Z') ? 0 : (ch >= 'a' && ch <= 'z') ? 1 : -1;
            if (c < 0) continue;
            const int base = c * 56, ks = cKing[c];
            if (B[ks] != c * 6 + 5 || (ks >> 3) != (c ? 7 : 0)) continue;             // no king on its back rank: no castling right
            if (!chess960 && ks != base + 4) continue;                                  // standard chess: the king must stand on e1 / e8
            const char u = (char)(ch & ~32);
            int rsq = -1, right = 0;
            if (u == 'K') { for (int s = base + 7; s > ks; s--) if (B[s] == c * 6 + 3) { rsq = s; break; } right = 0; }
            else if (u == 'Q') { for (int s = base; s < ks; s++) if (B[s] == c * 6 + 3) { rsq = s; break; } right = 1; }
            else if (u >= 'A' && u <= 'H') { int s = base + (u - 'A'); if (B[s] == c * 6 + 3) { rsq = s; right = s > ks ? 0 : 1; } }
            if (rsq < 0 || rsq == ks) continue;
            if (!chess960 && rsq != base + (right == 0 ? 7 : 0)) continue;              // standard chess: rooks on a / h
            castle |= (c == 0 ? 1 : 4) << right; cRook[c * 2 + right] = rsq;
        }
        for (int i = 0; i < 64; i++) cmask[i] = 15;
        cmask[cKing[0]] &= ~3; cmask[cKing[1]] &= ~12; cmask[cRook[0]] &= ~1; cmask[cRook[1]] &= ~2; cmask[cRook[2]] &= ~4; cmask[cRook[3]] &= ~8;
        ep = (eps != "-" && eps.size() == 2) ? (eps[0] - 'a') + (eps[1] - '1') * 8 : -1;
        // castling rights and the en passant square must match the pieces: a right without its king and rook (or an en passant square
        // without the pawn that just moved) would make move generation remove a piece that is not there
        if (ep >= 0 && !(ep < 64 && ((side == 0 && (ep >> 3) == 5 && B[ep] == EMPTYP && B[ep - 8] == 6) || (side == 1 && (ep >> 3) == 2 && B[ep] == EMPTYP && B[ep + 8] == 0)))) ep = -1;
        half = hm; ply = 0; hash = computeHash(); HH[0] = hash; refreshPst(); refresh();
    }
    inline bool attacked(int sq, int by) const {
        if (PawnA[by ^ 1][sq] & P[by * 6]) return true;
        if (KnightA[sq] & P[by * 6 + 1]) return true;
        if (KingA[sq] & P[by * 6 + 5]) return true;
        if (bishopAtt(sq, All) & (P[by * 6 + 2] | P[by * 6 + 4])) return true;
        if (rookAtt(sq, All) & (P[by * 6 + 3] | P[by * 6 + 4])) return true;
        return false;
    }
    inline int kingSq(int c) const { return lsb(P[c * 6 + 5]); }
    inline bool inCheck() const { return attacked(kingSq(side), side ^ 1); }

    inline void castleRook(int m, int us, int& rs, int& rt) const {      // origin and target square of the rook in castling move m
        const bool qs = (((m >> 6) & 63) & 7) < 4;
        rs = cRook[us * 2 + (qs ? 1 : 0)]; rt = us * 56 + (qs ? 3 : 5);
    }
    inline U64 keyAfter(int m) const {                  // the hash of the position after move m (the same XORs as make), to prefetch its table entry early
        int from = m & 63, to = (m >> 6) & 63, promo = (m >> 12) & 7, pc = B[from], us = side;
        U64 h = hash;
        if (ep >= 0) h ^= ZE[ep & 7];
        if (m & EPF) { int cs = to + (us == 0 ? -8 : 8); h ^= ZP[B[cs]][cs]; }
        else if (m & CAP) h ^= ZP[B[to]][to];
        h ^= ZP[pc][from] ^ ZP[promo ? us * 6 + promo : pc][to];
        if (m & CAS) { int rs, rt; castleRook(m, us, rs, rt); h ^= ZP[us * 6 + 3][rs] ^ ZP[us * 6 + 3][rt]; }
        if (m & DPP) h ^= ZE[((from + to) >> 1) & 7];
        h ^= ZC[castle] ^ ZC[castle & cmask[from] & cmask[to]] ^ ZS;
        return h;
    }
    void make(int m) {
        ttPrefetch(keyAfter(m));
        Undo& u = U[ply];
        u.move = m; u.castle = castle; u.ep = ep; u.half = half; u.hash = hash;
        const int pai = ai, nai = (ai + 1) & 255;
        { AccSave& sv = accSave[ply & 255];
          sv.psq[0] = psq[0]; sv.psq[1] = psq[1]; sv.psq2[0] = psq2[0]; sv.psq2[1] = psq2[1];
          sv.pstMg = pstMg; sv.pstEg = pstEg; sv.phaseAcc = phaseAcc; sv.kOff[0] = kOff[0]; sv.kOff[1] = kOff[1]; sv.kMir[0] = kMir[0]; sv.kMir[1] = kMir[1]; }
        int from = m & 63, to = (m >> 6) & 63, promo = (m >> 12) & 7;
        int pc = B[from], us = side;
        int kk0 = kKey(us);
        if (ep >= 0) hash ^= ZE[ep & 7];
        int cap = EMPTYP, capSq = -1;
        const bool fuse = nnueReady && (pc % 6) != 5 && !(m & CAS);          // ordinary move: one fused pass over the accumulators
        if (!fuse) { if (nnueReady) copySlot(pai, nai); setSlot(nai); }      // king move / castling / no net: update the new slot in place
        if (m & EPF) { capSq = to + (us == 0 ? -8 : 8); cap = B[capSq]; if (fuse) rem<false>(cap, capSq); else rem(cap, capSq); }
        else if (m & CAP) { capSq = to; cap = B[to]; if (fuse) rem<false>(cap, to); else rem(cap, to); }
        u.cap = cap;
        const int pcTo = promo ? us * 6 + promo : pc;
        if (fuse) { rem<false>(pc, from); add<false>(pcTo, to); nnDelta(pai, nai, pcTo, to, pc, from, cap, capSq); setSlot(nai); }
        else if (m & CAS) { int rs, rt; castleRook(m, us, rs, rt); rem(pc, from); rem(us * 6 + 3, rs); add(pc, to); add(us * 6 + 3, rt); }      // both pieces off, then both on (overlaps)
        else { rem(pc, from); add(pcTo, to); }
        if (nnueReady && kKey(us) != kk0) refreshPersp(us);
        ep = -1;
        if (m & DPP) { ep = (from + to) >> 1; hash ^= ZE[ep & 7]; }
        hash ^= ZC[castle]; castle &= cmask[from] & cmask[to]; hash ^= ZC[castle];
        half = (pc % 6 == 0 || cap != EMPTYP) ? 0 : half + 1;
        side ^= 1; hash ^= ZS;
        ply++; HH[ply] = hash;
    }
    void unmake(int m) {
        ply--;
        Undo& u = U[ply];
        side ^= 1; int us = side;
        int from = m & 63, to = (m >> 6) & 63, promo = (m >> 12) & 7;
        int np = B[to], pc = promo ? us * 6 : np;
        if (m & CAS) { int rs, rt; castleRook(m, us, rs, rt); remBB(np, to); remBB(us * 6 + 3, rt); addBB(pc, from); addBB(us * 6 + 3, rs); }
        else { remBB(np, to); addBB(pc, from); }
        if (u.cap != EMPTYP) { if (m & EPF) addBB(u.cap, to + (us == 0 ? -8 : 8)); else addBB(u.cap, to); }
        { const AccSave& sv = accSave[ply & 255];                       // the material tables and king orientation come back unchanged; the accumulators are in the parent's slot
          setSlot((ai + 255) & 255);
          psq[0] = sv.psq[0]; psq[1] = sv.psq[1]; psq2[0] = sv.psq2[0]; psq2[1] = sv.psq2[1];
          pstMg = sv.pstMg; pstEg = sv.pstEg; phaseAcc = sv.phaseAcc; kOff[0] = sv.kOff[0]; kOff[1] = sv.kOff[1]; kMir[0] = sv.kMir[0]; kMir[1] = sv.kMir[1]; }
        castle = u.castle; ep = u.ep; half = u.half; hash = u.hash;
    }
    void makeNull() {
        Undo& u = U[ply];
        u.ep = ep; u.hash = hash; u.half = half; u.castle = castle; u.move = 0;
        if (ep >= 0) hash ^= ZE[ep & 7];
        ep = -1; side ^= 1; hash ^= ZS; half++;
        ply++; HH[ply] = hash;
        ttPrefetch(hash);
    }
    void unmakeNull() { ply--; Undo& u = U[ply]; side ^= 1; ep = u.ep; hash = u.hash; half = u.half; }

    static int addPromos(int* m, int n, int from, int to, int flags, bool onlyQ) {
        m[n++] = from | (to << 6) | (4 << 12) | flags;
        if (!onlyQ) { m[n++] = from | (to << 6) | (3 << 12) | flags; m[n++] = from | (to << 6) | (2 << 12) | flags; m[n++] = from | (to << 6) | (1 << 12) | flags; }
        return n;
    }
    int gen(int* m, bool caps) const {
        int n = 0, us = side;
        U64 own = Occ[us], opp = Occ[us ^ 1], all = All;
        U64 pw = P[us * 6];
        int push = us == 0 ? 8 : -8, promoR = us == 0 ? 7 : 0, startR = us == 0 ? 1 : 6;
        while (pw) {
            int from = lsb(pw); pw &= pw - 1;
            int to = from + push;
            if (!((all >> to) & 1)) {
                if ((to >> 3) == promoR) n = addPromos(m, n, from, to, 0, caps);
                else if (!caps) {
                    m[n++] = from | (to << 6);
                    int to2 = to + push;
                    if ((from >> 3) == startR && !((all >> to2) & 1)) m[n++] = from | (to2 << 6) | DPP;
                }
            }
            U64 att = PawnA[us][from] & opp;
            while (att) {
                int t = lsb(att); att &= att - 1;
                if ((t >> 3) == promoR) n = addPromos(m, n, from, t, CAP, caps); else m[n++] = from | (t << 6) | CAP;
            }
            if (ep >= 0 && ((PawnA[us][from] >> ep) & 1)) m[n++] = from | (ep << 6) | CAP | EPF;
        }
        for (int type = 1; type <= 4; type++) {
            U64 b = P[us * 6 + type];
            while (b) {
                int from = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[from] : type == 2 ? bishopAtt(from, all) : type == 3 ? rookAtt(from, all) : queenAtt(from, all);
                att &= caps ? opp : ~own;
                while (att) { int t = lsb(att); att &= att - 1; m[n++] = from | (t << 6) | (((opp >> t) & 1) ? CAP : 0); }
            }
        }
        {
            int from = lsb(P[us * 6 + 5]);
            U64 att = KingA[from] & (caps ? opp : ~own);
            while (att) { int t = lsb(att); att &= att - 1; m[n++] = from | (t << 6) | (((opp >> t) & 1) ? CAP : 0); }
            if (!caps) {
                for (int q = 0; q < 2; q++) if (castle & ((us == 0 ? 1 : 4) << q)) {      // kingside first, then queenside, as before
                    const int ks = cKing[us], rs = cRook[us * 2 + q], base = us * 56;
                    const int kt = base + (q ? 2 : 6), rt = base + (q ? 3 : 5);
                    const U64 span1 = (2ULL << std::max(ks, kt)) - (1ULL << std::min(ks, kt)), span2 = (2ULL << std::max(rs, rt)) - (1ULL << std::min(rs, rt));
                    if (all & ((span1 | span2) & ~((1ULL << ks) | (1ULL << rs)))) continue;                 // everything between must be empty except king and rook
                    bool safe = true;
                    for (U64 pth = span1; pth; pth &= pth - 1) if (attacked(lsb(pth), us ^ 1)) { safe = false; break; }       // the king's path incl. start and target
                    if (safe) m[n++] = ks | (kt << 6) | CAS;
                }
            }
        }
        return n;
    }
    bool insufficient() const {
        if (P[0] | P[6] | P[3] | P[9] | P[4] | P[10]) return false;
        if (popc(P[1] | P[2] | P[7] | P[8]) <= 1) return true;
        if (P[1] | P[7]) return false;                                                     // 36.3: only bishops, all on squares of one colour = dead position
        const U64 bishops = P[2] | P[8], dark = 0xAA55AA55AA55AA55ULL;
        return !(bishops & dark) || !(bishops & ~dark);
    }
    bool isDraw() const {
        if (half >= 100) return true;
        int before = 0;
        for (int i = ply - 2; i >= 0 && i >= ply - half; i -= 2) if (HH[i] == hash) {
            if (i > rootPly) return true;           // repeated inside the search tree: a draw
            if (++before >= 2) return true;          // occurred twice before the search started: threefold
        }
        return insufficient();
    }
    bool legalMove(int m) {
        int us = side; make(m); bool ok = !attacked(kingSq(us), us ^ 1); unmake(m); return ok;
    }
};

string moveStr(int m) {
    int from = m & 63, to = (m >> 6) & 63, promo = (m >> 12) & 7;
    if (chess960 && (m & CAS)) to = g960Rook[(to >> 5) * 2 + ((to & 7) < 4 ? 1 : 0)];        // Chess960: castling is written 'king takes own rook'
    string s; s += char('a' + (from & 7)); s += char('1' + (from >> 3)); s += char('a' + (to & 7)); s += char('1' + (to >> 3));
    if (promo) s += "?nbrq"[promo];
    return s;
}

long long perftRec(Pos& p, int depth) {
    int mv[256]; int n = p.gen(mv, false); long long t = 0; int us = p.side;
    for (int i = 0; i < n; i++) {
        p.make(mv[i]);
        if (!p.attacked(p.kingSq(us), us ^ 1)) t += depth == 1 ? 1 : perftRec(p, depth - 1);
        p.unmake(mv[i]);
    }
    return t;
}

// ---------------------------------------------------------------- evaluation (feature based, tunable)
const int MatMg[6] = {82, 337, 365, 477, 1025, 0}, MatEg[6] = {94, 281, 297, 512, 936, 0};
const int PhaseW[6] = {0, 1, 1, 2, 4, 0};
const int PassMgI[8] = {0, 5, 10, 20, 35, 60, 100, 0}, PassEgI[8] = {0, 10, 20, 40, 70, 120, 180, 0};
const int AttW[6] = {0, 2, 2, 3, 5, 0};
const U64 DARKSQ = 0xAA55AA55AA55AA55ULL;
U64 RankBB[8];

enum {
    F_PST = 0,                 // 6*64
    F_BPAIR = 384,
    F_PASSED = 385,            // 8
    F_ISO = 393, F_DOUBLED = 394,
    F_CONN = 395,              // 8
    F_MOBN = 403,              // 9
    F_MOBB = 412,              // 14
    F_MOBR = 426,              // 15
    F_MOBQ = 441,              // 28
    F_ROPEN = 469, F_RSEMI = 470, F_SHIELD = 471,
    F_TPM = 472, F_TPR = 473, F_TPQ = 474, F_TMR = 475, F_TMQ = 476, F_TRQ = 477,
    F_HANG = 478, F_OUTN = 479, F_OUTB = 480,
    F_PKD = 481, F_PBLK = 482, F_TEMPO = 483, F_BADB = 484,
    NF = 485
};
int Wmg[NF], Weg[NF];
int PstMg[12][64], PstEg[12][64];  // kept for default weight construction

struct Feat { int idx, v; };

void initEval() {
    for (int r = 0; r < 8; r++) RankBB[r] = 0xFFULL << (r * 8);
    for (int sq = 0; sq < 64; sq++) {
        int r = sq >> 3, f = sq & 7, cf = min(f, 7 - f), cr = min(r, 7 - r), cd = cf + cr;
        int mg[6], eg[6];
        mg[0] = (r == 0 || r == 7) ? 0 : (r - 1) * 2 + (cf == 3 ? 8 : cf == 2 ? 4 : cf == 1 ? 0 : -4);
        eg[0] = (r == 0 || r == 7) ? 0 : (r - 1) * 8 + (cf == 0 ? -3 : 0);
        mg[1] = cd * 7 - 21; eg[1] = cd * 5 - 15;
        mg[2] = cd * 3 - 9 + ((f == r || f == 7 - r) ? 4 : 0); eg[2] = cd * 3 - 9;
        mg[3] = (cf >= 2 ? 4 : 0) - 2 + (r == 6 ? 12 : 0); eg[3] = (r == 6 ? 8 : 0);
        mg[4] = cd * 2 - 6; eg[4] = cd * 3 - 9;
        if (r == 0) mg[5] = f == 6 ? 30 : f == 1 ? 25 : (f == 7 || f == 2) ? 10 : (f >= 3 && f <= 5) ? -20 : 0;
        else mg[5] = -40 - r * 10;
        eg[5] = cd * 11 - 33;
        for (int t = 0; t < 6; t++) { Wmg[F_PST + t * 64 + sq] = mg[t] + MatMg[t]; Weg[F_PST + t * 64 + sq] = eg[t] + MatEg[t]; }
    }
    Wmg[F_BPAIR] = 30; Weg[F_BPAIR] = 50;
    for (int r = 0; r < 8; r++) { Wmg[F_PASSED + r] = PassMgI[r]; Weg[F_PASSED + r] = PassEgI[r]; }
    Wmg[F_ISO] = -10; Weg[F_ISO] = -12; Wmg[F_DOUBLED] = -10; Weg[F_DOUBLED] = -20;
    for (int c = 0; c < 9; c++) { Wmg[F_MOBN + c] = (c - 4) * 4; Weg[F_MOBN + c] = (c - 4) * 4; }
    for (int c = 0; c < 14; c++) { Wmg[F_MOBB + c] = (c - 6) * 3; Weg[F_MOBB + c] = (c - 6) * 4; }
    for (int c = 0; c < 15; c++) { Wmg[F_MOBR + c] = (c - 7) * 2; Weg[F_MOBR + c] = (c - 7) * 4; }
    for (int c = 0; c < 28; c++) { Wmg[F_MOBQ + c] = (c - 14); Weg[F_MOBQ + c] = (c - 14) * 2; }
    Wmg[F_ROPEN] = 22; Weg[F_ROPEN] = 12; Wmg[F_RSEMI] = 10; Weg[F_RSEMI] = 8;
    Wmg[F_SHIELD] = 8; Weg[F_SHIELD] = 0;
    Wmg[F_TEMPO] = 12; Weg[F_TEMPO] = 12;
#if __has_include("weights_tuned.h")
#include "weights_tuned.h"
    for (int i = 0; i < NF; i++) { Wmg[i] = TUNED_MG[i]; Weg[i] = TUNED_EG[i]; }
#endif
}
static inline U64 pawnAtts(U64 p, int c) {
    if (c == 0) return ((p << 7) & ~FILE_H) | ((p << 9) & ~FILE_A);
    return ((p >> 7) & ~FILE_A) | ((p >> 9) & ~FILE_H);
}
static inline int cheb(int a, int b) { return max(abs((a & 7) - (b & 7)), abs((a >> 3) - (b >> 3))); }

// Returns evaluation from White's point of view in centipawns (includes tempo for side to move).
// If DUMP: appends every (feature, value) to *out and the non-linear mg-only king-danger term to *fixedMg.
template <bool DUMP>
int evalCore(const Pos& pos, vector<Feat>* out, int* fixedMg, int* phaseOut) {
    int mg = 0, eg = 0, phase = 0, fix = 0;
#define ADD(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; if (DUMP) out->push_back({I, _v}); } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    for (int pc = 0; pc < 12; pc++) {
        U64 b = pos.P[pc]; int type = pc % 6, c = pc / 6;
        while (b) {
            int sq = lsb(b); b &= b - 1;
            ADD(F_PST + type * 64 + (c == 0 ? sq : sq ^ 56), c == 0 ? 1 : -1);
            phase += PhaseW[type];
        }
    }
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADD(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            bool passed = !(PassedMask[c][sq] & their);
            if (passed) {
                ADD(F_PASSED + rr, sign);
                int stop = sq + (c == 0 ? 8 : -8);
                ADD(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
                if ((all >> stop) & 1) ADD(F_PBLK, sign);
            }
            if (!(AdjFiles[f] & my)) ADD(F_ISO, sign);
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) ADD(F_DOUBLED, sign);
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) ADD(F_CONN + rr, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADD(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADD(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADD(F_MOBR + min(cnt, 14), sign); break;
                    default: ADD(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADD(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADD(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADD(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADD(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADD(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADD(F_TPM, sign * popc(attPawn[c] & tm)); ADD(F_TPR, sign * popc(attPawn[c] & tr)); ADD(F_TPQ, sign * popc(attPawn[c] & tq));
        ADD(F_TMR, sign * popc(attMinor[c] & tr)); ADD(F_TMQ, sign * popc(attMinor[c] & tq)); ADD(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADD(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADD(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    if (DUMP) { *fixedMg = fix; *phaseOut = phase; }
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (!DUMP && sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADD
}

// ---------------------------------------------------------------- shared search state (Lazy SMP)
const int MAXTH = 8;
#ifndef DEFAULT_THREADS
#define DEFAULT_THREADS 1
#endif
static int numThreads = DEFAULT_THREADS;
Pos gRoot;                     // root position handed to the helper threads
static int lazyMargin = 0;
static int nnBlend = 70;
static int evalFlags = 1;
static int net2Share = 70;
const int MAXPLY = 128, INF = 32000, MATE = 30000, MATE_BOUND = MATE - 256;
enum { TT_NONE, TT_EXACT, TT_LOWER, TT_UPPER };
struct TTE { std::atomic<U64> k, d; };
static const int NOEVAL = 32767;
struct TTHit { int move, score, depth, flag, eval; };
std::unique_ptr<TTE[]> tt; U64 ttMask = 0;
static TTE* ttBase = nullptr;          // 64-byte aligned start of the table (buckets of 4 entries = one cache line)
static unsigned ttGen = 0;             // generation (0..3) of the current search
int lmrT[64][64];
atomic<bool> stopFlag(false);
atomic<bool> goInfinite(false);
long long softMs = 0, hardMs = 0, nodeLimit = 0;
int maxDepthLimit = 64;
chrono::steady_clock::time_point t0;
int hashMB = 64;
static long long moveOverhead = 5;
vector<int> rootBan;
long long elapsedMs() { return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count(); }
static bool ttDirty = true;          // a search may have written to the table since it was last cleared
static int ttRequestedMB = 0;        // the size asked for (the table can be smaller if the machine could not provide it)
void resizeTT(int mb) {
    // After this call the table is empty.  Same size as before: clear it in place, and not at all if no search has touched it
    // (v30 re-allocated and re-zeroed it on every ucinewgame: 1.6 s at 4096 MB, and with the old table still allocated while the
    // new one was requested it needed twice the memory, so a machine that could hold one 4 GB table but not two silently ended up
    // with a smaller table).  A different size: release the old table FIRST.
    // If the machine cannot provide the requested size, fall back to smaller tables instead of crashing.
    if (tt && mb == ttRequestedMB) {
        if (ttDirty) { memset((void*)ttBase, 0, (size_t)(ttMask + 1) * sizeof(TTE)); ttDirty = false; }
        return;
    }
    tt.reset();
    const int requested = mb;
    while (true) {
        size_t entries = (size_t)mb * 1024 * 1024 / sizeof(TTE);
        size_t p = 1; while (p * 2 <= entries) p *= 2;
        try { tt.reset(new TTE[p + 4]()); ttBase = (TTE*)(((uintptr_t)tt.get() + 63) & ~(uintptr_t)63); ttMask = p - 1; ttRequestedMB = requested; ttDirty = false; return; }
        catch (const std::exception&) { tt.reset(); if (mb <= 1) throw; mb /= 2; }
    }
}
static inline void ttPrefetch(U64 hash) { _mm_prefetch((const char*)&ttBase[(hash & ttMask) & ~(U64)3], _MM_HINT_T0); }
static inline int toTT(int s, int ply) { return s >= MATE_BOUND ? s + ply : s <= -MATE_BOUND ? s - ply : s; }
static inline int fromTT(int s, int ply) { return s >= MATE_BOUND ? s - ply : s <= -MATE_BOUND ? s + ply : s; }
static inline int ttPriority(U64 d) {            // replacement priority of an entry: depth minus 8 per generation of age; an empty entry is the lowest
    if (((d >> 44) & 3) == 0) return -100000;
    return (int)((d >> 36) & 0xFF) - 8 * (int)((ttGen - (unsigned)(d >> 62)) & 3);
}
static inline bool ttRead(U64 hash, TTHit& h, int* slotDepth = nullptr) {
    const TTE* b = &ttBase[(hash & ttMask) & ~(U64)3];
    int vp = 1 << 30, vd = 0;                     // the entry that a write of another position would replace (lowest priority) and its depth
    const int start = (int)(hash >> 2) & 3;       // scan from a position-dependent slot so that ties are not always resolved in favour of slot 0
    for (int kk = 0; kk < 4; kk++) {
        const int i = (start + kk) & 3;
        U64 d = b[i].d.load(std::memory_order_relaxed), k = b[i].k.load(std::memory_order_relaxed);
        if ((k ^ d) == hash && ((d >> 44) & 3) != 0) {
            if (slotDepth) *slotDepth = (int)((d >> 36) & 0xFF);
            h.move = (int)(d & 0xFFFFF); h.score = (int)((d >> 20) & 0xFFFF) - 32768; h.depth = (int)((d >> 36) & 0xFF);
            h.flag = (int)((d >> 44) & 3); h.eval = (int)((d >> 46) & 0xFFFF) - 32768;
            return true;
        }
        if (slotDepth) { int pr = ttPriority(d); if (pr < vp) { vp = pr; vd = ((d >> 44) & 3) == 0 ? 0 : (int)((d >> 36) & 0xFF); } }
    }
    if (slotDepth) *slotDepth = vd;
    return false;
}
static inline void ttWrite(U64 hash, int move, int score, int depth, int flag, int eval) {
    U64 d = (U64)(move & 0xFFFFF) | ((U64)(score + 32768) << 20) | ((U64)(depth & 0xFF) << 36) | ((U64)flag << 44) | ((U64)(eval + 32768) << 46) | ((U64)ttGen << 62);
    TTE* b = &ttBase[(hash & ttMask) & ~(U64)3];
    int vi = 0, vp = 1 << 30;
    const int start = (int)(hash >> 2) & 3;       // scan from a position-dependent slot: equal priorities are then replaced in rotation, not always slot 0
    for (int kk = 0; kk < 4; kk++) {
        const int i = (start + kk) & 3;
        U64 od = b[i].d.load(std::memory_order_relaxed), ok = b[i].k.load(std::memory_order_relaxed);
        if ((ok ^ od) == hash && ((od >> 44) & 3) != 0) { vi = i; break; }       // the same position: replace it in place
        int pr = ttPriority(od);
        if (pr < vp) { vp = pr; vi = i; }
    }
    b[vi].d.store(d, std::memory_order_relaxed); b[vi].k.store(hash ^ d, std::memory_order_relaxed);
}
static int corrFlags = 2;
static int v31Flags = 0, v31HP = 2500, v31CutR = 1, v31DE = 20;
static int qsTT = 1;
static int evalCache = 1;

namespace S0 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
namespace S1 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
namespace S2 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
namespace S3 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
namespace S4 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
namespace S5 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
namespace S6 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
namespace S7 {
struct PawnEntry { U64 w = 1, b = 1; int mg = 0, eg = 0; U64 passed[2] = {0, 0}; };
static PawnEntry pawnTT[1 << 15];
static void computePawnEntry(const Pos& pos, PawnEntry& pe) {
    pe.mg = pe.eg = 0; pe.passed[0] = pe.passed[1] = 0;
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6], pb = my;
        while (pb) {
            int sq = lsb(pb); pb &= pb - 1;
            int f = sq & 7, r = sq >> 3, rr = c == 0 ? r : 7 - r;
            if (!(PassedMask[c][sq] & their)) { pe.mg += sign * Wmg[F_PASSED + rr]; pe.eg += sign * Weg[F_PASSED + rr]; pe.passed[c] |= 1ULL << sq; }
            if (!(AdjFiles[f] & my)) { pe.mg += sign * Wmg[F_ISO]; pe.eg += sign * Weg[F_ISO]; }
            U64 ahead = c == 0 ? (~0ULL << (sq + 1)) : ((1ULL << sq) - 1);
            if (FileBB[f] & my & ahead) { pe.mg += sign * Wmg[F_DOUBLED]; pe.eg += sign * Weg[F_DOUBLED]; }
            if ((PawnA[c ^ 1][sq] & my) || (AdjFiles[f] & my & RankBB[r])) { pe.mg += sign * Wmg[F_CONN + rr]; pe.eg += sign * Weg[F_CONN + rr]; }
        }
    }
}

int evalFast(const Pos& pos) {
    int mg = pos.pstMg, eg = pos.pstEg, phase = pos.phaseAcc, fix = 0;
#define ADDF(I, V) do { int _v = (V); if (_v) { mg += Wmg[I] * _v; eg += Weg[I] * _v; } } while (0)
    U64 all = pos.All;
    U64 attAll[2] = {0, 0}, attMinor[2] = {0, 0}, attRook[2] = {0, 0}, attPawn[2];
    attPawn[0] = pawnAtts(pos.P[0], 0); attPawn[1] = pawnAtts(pos.P[6], 1);
    PawnEntry& pe = pawnTT[((pos.P[0] * 0x9E3779B97F4A7C15ULL) ^ (pos.P[6] * 0xC2B2AE3D27D4EB4FULL)) >> 49];
    if (pe.w != pos.P[0] || pe.b != pos.P[6]) { pe.w = pos.P[0]; pe.b = pos.P[6]; computePawnEntry(pos, pe); }
    mg += pe.mg; eg += pe.eg;
    if (lazyMargin > 0) { int p0 = phase > 24 ? 24 : phase; int cheap = (mg * p0 + eg * (24 - p0)) / 24; if (cheap > lazyMargin || cheap < -lazyMargin) return cheap; }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1;
        U64 my = pos.P[c * 6], their = pos.P[(c ^ 1) * 6];
        if (popc(pos.P[c * 6 + 2]) >= 2) ADDF(F_BPAIR, sign);
        int ek = pos.kingSq(c ^ 1), ok = pos.kingSq(c);
        U64 pp = pe.passed[c];
        while (pp) {
            int sq = lsb(pp); pp &= pp - 1;
            int rr = c == 0 ? sq >> 3 : 7 - (sq >> 3), stop = sq + (c == 0 ? 8 : -8);
            ADDF(F_PKD, sign * (cheb(ek, stop) - cheb(ok, stop)) * rr);
            if ((all >> stop) & 1) ADDF(F_PBLK, sign);
        }
        U64 zone = KingA[ek] | (1ULL << ek), area = ~pos.Occ[c] & ~attPawn[c ^ 1];
        int attackers = 0, weight = 0;
        for (int type = 1; type <= 4; type++) {
            U64 b = pos.P[c * 6 + type];
            while (b) {
                int sq = lsb(b); b &= b - 1;
                U64 att = type == 1 ? KnightA[sq] : type == 2 ? bishopAtt(sq, all) : type == 3 ? rookAtt(sq, all) : queenAtt(sq, all);
                attAll[c] |= att;
                int cnt = popc(att & area);
                switch (type) {
                    case 1: attMinor[c] |= att; ADDF(F_MOBN + min(cnt, 8), sign); break;
                    case 2: attMinor[c] |= att; ADDF(F_MOBB + min(cnt, 13), sign); break;
                    case 3: attRook[c] |= att; ADDF(F_MOBR + min(cnt, 14), sign); break;
                    default: ADDF(F_MOBQ + min(cnt, 27), sign); break;
                }
                int z = popc(att & zone);
                if (z > 0) { attackers++; weight += AttW[type] * z; }
                if (type == 3) {
                    int f = sq & 7;
                    if (!(FileBB[f] & (my | their))) ADDF(F_ROPEN, sign);
                    else if (!(FileBB[f] & my)) ADDF(F_RSEMI, sign);
                }
                if (type == 1 || type == 2) {
                    int f = sq & 7, rr = c == 0 ? sq >> 3 : 7 - (sq >> 3);
                    if (rr >= 3 && rr <= 5 && (PawnA[c ^ 1][sq] & my) && !(PassedMask[c][sq] & AdjFiles[f] & their))
                        ADDF(type == 1 ? F_OUTN : F_OUTB, sign);
                    if (type == 2) ADDF(F_BADB, sign * popc(my & (((DARKSQ >> sq) & 1) ? DARKSQ : ~DARKSQ)));
                }
            }
        }
        attAll[c] |= attPawn[c] | KingA[ok];
        if (attackers >= 2) fix += sign * min(weight * weight / 10, 400);
        ADDF(F_SHIELD, sign * popc(ShieldMask[c][ok] & my));
    }
    for (int c = 0; c < 2; c++) {
        int sign = c == 0 ? 1 : -1, t = c ^ 1;
        U64 tm = pos.P[t * 6 + 1] | pos.P[t * 6 + 2], tr = pos.P[t * 6 + 3], tq = pos.P[t * 6 + 4];
        ADDF(F_TPM, sign * popc(attPawn[c] & tm)); ADDF(F_TPR, sign * popc(attPawn[c] & tr)); ADDF(F_TPQ, sign * popc(attPawn[c] & tq));
        ADDF(F_TMR, sign * popc(attMinor[c] & tr)); ADDF(F_TMQ, sign * popc(attMinor[c] & tq)); ADDF(F_TRQ, sign * popc(attRook[c] & tq));
        U64 pieces = tm | tr | tq;
        ADDF(F_HANG, sign * popc(attAll[c] & pieces & ~attAll[t]));
    }
    ADDF(F_TEMPO, pos.side == 0 ? 1 : -1);
    if (phase > 24) phase = 24;
    mg += fix;
    int sc2 = (mg * phase + eg * (24 - phase)) / 24;
    if (sc2 != 0) {
        int winner = sc2 > 0 ? 0 : 1, loser = winner ^ 1;
        if (!pos.P[winner * 6]) {
            int wm = 3 * popc(pos.P[winner * 6 + 1] | pos.P[winner * 6 + 2]) + 5 * popc(pos.P[winner * 6 + 3]) + 9 * popc(pos.P[winner * 6 + 4]);
            int lm = 3 * popc(pos.P[loser * 6 + 1] | pos.P[loser * 6 + 2]) + 5 * popc(pos.P[loser * 6 + 3]) + 9 * popc(pos.P[loser * 6 + 4]);
            if (wm - lm <= 3) sc2 /= 8;
        }
        if (popc(pos.P[2]) == 1 && popc(pos.P[8]) == 1 && !(pos.P[1] | pos.P[7] | pos.P[3] | pos.P[9] | pos.P[4] | pos.P[10])) {
            int bw = lsb(pos.P[2]), bb = lsb(pos.P[8]);
            if (((DARKSQ >> bw) & 1) != ((DARKSQ >> bb) & 1)) sc2 /= 2;
        }
    }
    return sc2;
#undef ADDF
}

struct alignas(64) PadInt { int v; char pad[60]; };   // each thread's copy on its own cache line (it is written by every evaluation)
static PadInt nn2ResP;
#define nn2Res nn2ResP.v
static int evalNN(const Pos& pos) {
    const int32_t* S = pos.acc[pos.side];
    const int32_t* O = pos.acc[pos.side ^ 1];
    int32_t sum = 0;
    const int bk1 = nnNB1 > 1 ? std::min(nnNB1 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
    const int16_t* W2r = nnW2q[bk1];
    for (int j = 0; j < NN_H; j++) {
        int32_t s = S[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
        int32_t o = O[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
        if (nnSq1) { s = (s * s) >> 8; o = (o * o) >> 8; }
        sum += s * (int32_t)W2r[j] + o * (int32_t)W2r[NN_H + j];
    }
    float out = nnB2[bk1] + (float)pos.psq[pos.side] * (1.0f / 1024.0f) + (float)sum * (1.0f / (NN_QA * NN_QB));
    if (net2Loaded && useNet2) {
        const int32_t* S2 = pos.acc2[pos.side]; const int32_t* O2 = pos.acc2[pos.side ^ 1];
        int32_t sum2 = 0;
        const int bk2 = nnNB2 > 1 ? std::min(nnNB2 - 1, std::max(0, (popc(pos.All) - 2) / 4)) : 0;
        const int16_t* W2r2 = nnW2qb[bk2];
        for (int j = 0; j < NN_H; j++) {
            int32_t s = S2[j]; s = s < 0 ? 0 : (s > 256 ? 256 : s);
            int32_t o = O2[j]; o = o < 0 ? 0 : (o > 256 ? 256 : o);
            if (nnSq2) { s = (s * s) >> 8; o = (o * o) >> 8; }
            sum2 += s * (int32_t)W2r2[j] + o * (int32_t)W2r2[NN_H + j];
        }
        float out2 = nnB2b[bk2] + (float)pos.psq2[pos.side] * (1.0f / 1024.0f) + (float)sum2 * (1.0f / (NN_QA * NN_QB));
        float rc = out2 * 173.7f; if (rc > 3000.0f) rc = 3000.0f; else if (rc < -3000.0f) rc = -3000.0f;
        nn2Res = (int)rc;
    }
    float cp = out * 173.7f;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}

static int evaluateRaw(const Pos& pos) {
    if (nnueReady) {
        if (nnBlend <= 0) { int s0 = evalFast(pos); return pos.side == 0 ? s0 : -s0; }
        int nn = evalNN(pos);
        int s = evalFast(pos);
        int h = pos.side == 0 ? s : -s;
        if (net2Loaded && useNet2) nn = (nn * (100 - net2Share) + (h + nn2Res) * net2Share) / 100;
        if (nnBlend >= 100) return nn;
        return (nn * nnBlend + h * (100 - nnBlend)) / 100;
    }
    int s = evalCore<false>(pos, nullptr, nullptr, nullptr);
    return pos.side == 0 ? s : -s;
}

static inline int evalDamp(int e, const Pos& p) {
    if (evalFlags & 1) e = e * (200 - (p.half < 100 ? p.half : 100)) / 200;
    return e;
}
int evaluate(const Pos& pos) {
    return evalDamp(evaluateRaw(pos), pos);
}

// ---------------------------------------------------------------- search

Pos pos;
int moves_[MAXPLY][256], scores_[MAXPLY][256];
int pvTable[MAXPLY][MAXPLY], pvLen[MAXPLY];
int killers[MAXPLY][2];
int history[2][64][64];
long long nodes_ = 0;

void checkTime() {
    if (hardMs > 0 && elapsedMs() >= hardMs) stopFlag = true;
    if (nodeLimit > 0 && nodes_ >= nodeLimit) stopFlag = true;
}
const int PV_[6] = {100, 320, 330, 500, 900, 20000};

const int SEEV[6] = {100, 320, 330, 500, 900, 20000};
short contHist[768][768];
int counterMv[768];
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4];
int corrHist[2][16384];
int corrNP[2][2][16384];
static inline int npKeyIdx(const Pos& p, int col) {
    U64 k = (p.P[col * 6 + 1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[col * 6 + 2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[col * 6 + 3] * 0x165667B19E3779F9ULL) ^
            (p.P[col * 6 + 4] * 0x27D4EB2F165667C5ULL) ^ (p.P[col * 6 + 5] * 0x94D049BB133111EBULL);
    return (int)((k >> 50) & 16383);
}
static inline int pawnKeyIdx(const Pos& p) {
    U64 k = (p.P[0] * 0x9E3779B97F4A7C15ULL) ^ (p.P[6] * 0xC2B2AE3D27D4EB4FULL);
    return (int)((k >> 50) & 16383);
}

static inline U64 attackersTo(const Pos& p, int sq, U64 occ) {
    return (PawnA[1][sq] & p.P[0]) | (PawnA[0][sq] & p.P[6]) | (KnightA[sq] & (p.P[1] | p.P[7])) |
           (KingA[sq] & (p.P[5] | p.P[11])) | (bishopAtt(sq, occ) & (p.P[2] | p.P[8] | p.P[4] | p.P[10])) |
           (rookAtt(sq, occ) & (p.P[3] | p.P[9] | p.P[4] | p.P[10]));
}

// Static exchange evaluation: does move m gain at least `thr` centipawns by exchanges on the target square?
bool seeGE(const Pos& p, int m, int thr) {
    if ((m & CAS) || ((m >> 12) & 7)) return 0 >= thr;
    int from = m & 63, to = (m >> 6) & 63, moved = p.B[from] % 6;
    int victim = (m & EPF) ? 0 : (p.B[to] == EMPTYP ? -1 : p.B[to] % 6);
    int swap = (victim < 0 ? 0 : SEEV[victim]) - thr;
    if (swap < 0) return false;
    swap = SEEV[moved] - swap;
    if (swap <= 0) return true;
    U64 occ = p.All ^ (1ULL << from) ^ (1ULL << to);
    if (m & EPF) occ ^= 1ULL << (to + (p.side == 0 ? -8 : 8));
    int stm = p.side;
    U64 att = attackersTo(p, to, occ);
    U64 bq = p.P[2] | p.P[8] | p.P[4] | p.P[10], rq = p.P[3] | p.P[9] | p.P[4] | p.P[10];
    int res = 1;
    while (true) {
        stm ^= 1; att &= occ;
        U64 mine = att & p.Occ[stm];
        if (!mine) break;
        res ^= 1;
        U64 bb;
        if ((bb = mine & p.P[stm * 6])) { if ((swap = SEEV[0] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 1])) { if ((swap = SEEV[1] - swap) < res) break; occ ^= bb & -bb; }
        else if ((bb = mine & p.P[stm * 6 + 2])) { if ((swap = SEEV[2] - swap) < res) break; occ ^= bb & -bb; att |= bishopAtt(to, occ) & bq; }
        else if ((bb = mine & p.P[stm * 6 + 3])) { if ((swap = SEEV[3] - swap) < res) break; occ ^= bb & -bb; att |= rookAtt(to, occ) & rq; }
        else if ((bb = mine & p.P[stm * 6 + 4])) { if ((swap = SEEV[4] - swap) < res) break; occ ^= bb & -bb; att |= (bishopAtt(to, occ) & bq) | (rookAtt(to, occ) & rq); }
        else return (att & ~p.Occ[stm]) ? (res ^ 1) : res;
    }
    return res;
}

static inline void updHist(short& h, int bonus) {
    h += bonus - h * abs(bonus) / 16384;
}
static inline int quietScore(const Pos& p, int m, int ply) {
    int from = m & 63, to = (m >> 6) & 63, pc = p.B[from];
    int s = history[p.side][from][to];
    if (ply >= 1 && pieceAt[ply - 1] >= 0) s += contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + to];
    if (ply >= 2 && pieceAt[ply - 2] >= 0) s += contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + to] / 2;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        return (promo || seeGE(p, m, 0)) ? 1000000 + mvv : -200000 + mvv;
    }
    if (promo) return 900000 + promo;
    if (m == killers[ply][0]) return 800000;
    if (m == killers[ply][1]) return 790000;
    if (ply >= 1 && pieceAt[ply - 1] >= 0 && counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] == m) return 700000;
    return quietScore(p, m, ply);
}

int qsearch(int alpha, int beta, int ply) {
    pvLen[ply] = ply;
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    int origAlpha = alpha, qBest = 0, qTtMove = 0, qTtEval = NOEVAL;
    if (qsTT) {
        TTHit qh;
        if (ttRead(pos.hash, qh)) {
            qTtMove = qh.move; qTtEval = qh.eval;
            int qs = fromTT(qh.score, ply);
            if (beta - alpha == 1 && (qh.flag == TT_EXACT || (qh.flag == TT_LOWER && qs >= beta) || (qh.flag == TT_UPPER && qs <= alpha))) return qs;
        }
    }
    int stand = -INF, standU = NOEVAL;
    if (!inChk) {
        standU = (evalCache && qTtEval != NOEVAL) ? qTtEval : evaluateRaw(pos);
        stand = evalDamp(standU, pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    int* mv = moves_[ply]; int* sc = scores_[ply];
    int n = pos.gen(mv, !inChk);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], qTtMove, ply);
    int legal = 0, best = inChk ? -INF : stand, us = pos.side;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
        swap(mv[i], mv[bi]); swap(sc[i], sc[bi]);
        int m = mv[i];
        if (!inChk && (m & CAP) && !((m >> 12) & 7)) {
            int victim = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
            if (stand + PV_[victim] + 200 < alpha) continue;
            if (sc[i] < 0) continue;  // losing capture by SEE
        }
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        int s = -qsearch(-beta, -alpha, ply + 1);
        pos.unmake(m);
        if (stopFlag) return 0;
        if (s > best) { best = s; qBest = m; if (s > alpha) { alpha = s; if (s >= beta) break; } }
    }
    if (inChk && legal == 0) return -MATE + ply;
    if (qsTT) {
        // quiescence entries have depth 0 and never evict a deeper entry
        TTHit qc; int qslot = 0; bool qsame = ttRead(pos.hash, qc, &qslot);
        if (qslot <= 0) {
            int keepMove = qBest ? qBest : (qsame ? qc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), 0, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, standU);
        }
    }
    return best;
}

int search(int depth, int alpha, int beta, int ply, bool allowNull, int excluded = 0, bool cutNode = false) {
    pvLen[ply] = ply;
    if (ply > 0 && pos.isDraw()) {
        // fifty-move boundary: a checkmate delivered on the 100th half-move still wins, so only call it a draw if a legal move exists
        if (pos.half < 100 || !pos.inCheck()) return 0;
        int tm[256]; int tn = pos.gen(tm, false); bool anyLegal = false;
        for (int i = 0; i < tn; i++) if (pos.legalMove(tm[i])) { anyLegal = true; break; }
        if (anyLegal) return 0;
    }
    if (ply >= MAXPLY - 1) return evaluate(pos);
    bool inChk = pos.inCheck();
    if (inChk) depth++;
    if (depth <= 0) return qsearch(alpha, beta, ply);
    nodes_++;
    if ((nodes_ & 2047) == 0) checkTime();
    if (stopFlag) return 0;
    alpha = max(alpha, -MATE + ply); beta = min(beta, MATE - ply - 1);
    if (alpha >= beta) return alpha;
    bool pv = beta - alpha > 1;
    int ttMove = 0, ttScore = 0, ttDepth = -1, ttFlag = TT_NONE, ttEval = NOEVAL;
    if (!excluded) {
        TTHit th;
        if (ttRead(pos.hash, th)) {
            ttMove = th.move; ttDepth = th.depth; ttFlag = th.flag; ttScore = fromTT(th.score, ply); ttEval = th.eval;
            if (ttDepth >= depth && ply > 0 && !pv) {
                if (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore >= beta) || (ttFlag == TT_UPPER && ttScore <= alpha)) return ttScore;
            }
        }
    }
    int us = pos.side;
    int rawU = inChk ? NOEVAL : ((evalCache && ttEval != NOEVAL) ? ttEval : evaluateRaw(pos));
    int rawEval = inChk ? -INF : evalDamp(rawU, pos);
    int cidx = pawnKeyIdx(pos);
    int ciw = 0, cib = 0, corr = corrHist[us][cidx];
    if (corrFlags) {
        ciw = npKeyIdx(pos, 0); cib = npKeyIdx(pos, 1);
        if (corrFlags == 1) corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) / 4;
        else corr = (2 * corr + corrNP[us][0][ciw] + corrNP[us][1][cib]) * 3 / 8;
    }
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + 300 < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= 7 && peval - 80 * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = 3 + depth / 4 + min((peval - beta) / 200, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= 5 && abs(beta) < MATE_BOUND) {
            int pb = beta + 170;
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
                pos.make(m);
                if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
                int s = -qsearch(-pb, -pb + 1, ply + 1);
                if (s >= pb) s = -search(depth - 4, -pb, -pb + 1, ply + 1, true, 0, !cutNode);
                pos.unmake(m);
                if (stopFlag) return 0;
                if (s >= pb) return s;
            }
        }
    }
    if (depth >= 4 && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64];
    int selDone = 0;                    // moves 0 .. selDone-1 are already in their final (selection-sorted) place
    for (int i = 0; i < n; i++) {
        if (i >= selDone) {
            int bi = i;
            for (int j = i + 1; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i], mv[bi]); swap(sc[i], sc[bi]); selDone = i + 1;
        }
        int m = mv[i];
        if (m == excluded) continue;
        if (ply == 0 && !rootBan.empty()) { bool bn = false; for (int x : rootBan) if (x == m) { bn = true; break; } if (bn) continue; }
        bool quiet = !(m & CAP) && !((m >> 12) & 7);
        if (!pv && !inChk && legal > 0 && best > -MATE_BOUND) {
            if (quiet) {
                qSeen++;
                if (depth <= 6 && qSeen > ((3 + depth * depth) >> (improving ? 0 : 1))) continue;
                if (depth <= 7 && peval + 90 + 90 * depth <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if (depth <= 6 && !seeGE(pos, m, quiet ? -30 * depth * depth : -90 * depth)) continue;
        }
        int ext = 0;
        if (depth >= 7 && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - 3 &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - 2 * depth;
            int s2 = search((depth - 1) / 2, sb - 1, sb, ply, false, m, cutNode);
            if (stopFlag) return 0;
            if (s2 < sb) { ext = 1; if ((v31Flags & 8) && !pv && s2 < sb - v31DE) ext = 2; }
            else if (sb >= beta) return sb;
            else if ((v31Flags & 8) && ttScore >= beta) ext = -1;
        }
        if (i + 1 < n && selDone == i + 1) {      // select the NEXT move now (same order as before) and prefetch its table entry while this one is searched
            int bi = i + 1;
            for (int j = i + 2; j < n; j++) if (sc[j] > sc[bi]) bi = j;
            swap(mv[i + 1], mv[bi]); swap(sc[i + 1], sc[bi]); selDone = i + 2;
            ttPrefetch(pos.keyAfter(mv[i + 1]));
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= 4) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / 6000));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) s = -search(nd, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (quiet && nq < 64) quiets[nq++] = m;
        if (s > best) {
            best = s;
            if (s > alpha) {
                bestMove = m; alpha = s;
                pvTable[ply][ply] = m;
                for (int k = ply + 1; k < pvLen[ply + 1]; k++) pvTable[ply][k] = pvTable[ply + 1][k];
                pvLen[ply] = max(ply + 1, pvLen[ply + 1]);
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        if (ply >= 1 && pieceAt[ply - 1] >= 0) counterMv[pieceAt[ply - 1] * 64 + toAt[ply - 1]] = m;
                        int bonus = min(depth * depth, 400);
                        for (int k = 0; k < nq; k++) {
                            int qm = quiets[k], sgn = (qm == m) ? 1 : -1;
                            int b = sgn * bonus;
                            int& h = history[us][qm & 63][(qm >> 6) & 63];
                            h += b - h * abs(b) / 16384;
                            int pc = pos.B[qm & 63];
                            // piece for conthist: moved piece before unmake is gone, so look it up from the board now
                            if (pc == EMPTYP) pc = pieceAt[ply];
                            if (ply >= 1 && pieceAt[ply - 1] >= 0) updHist(contHist[pieceAt[ply - 1] * 64 + toAt[ply - 1]][pc * 64 + ((qm >> 6) & 63)], b);
                            if (ply >= 2 && pieceAt[ply - 2] >= 0) updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((v31Flags & 128) && best >= beta && abs(best) < MATE_BOUND && abs(beta) < MATE_BOUND && abs(alpha) < MATE_BOUND && depth > 1) best = (best * depth + beta) / (depth + 1);
    if (!excluded && !inChk && abs(best) < MATE_BOUND &&
        (bestMove == 0 || ((bestMove & CAP) == 0 && ((bestMove >> 12) & 7) == 0)) &&
        !((best >= beta && best <= eval) || (best <= origAlpha && best >= eval))) {
        int diff = (best - rawEval) * 32, wgt = min(depth + 1, 16);
        int& c = corrHist[us][cidx];
        c = (c * (256 - wgt) + diff * wgt) / 256;
        c = max(-32 * 80, min(32 * 80, c));
        if (corrFlags) {
            int& cw = corrNP[us][0][ciw]; cw = (cw * (256 - wgt) + diff * wgt) / 256; cw = max(-32 * 80, min(32 * 80, cw));
            int& cb = corrNP[us][1][cib]; cb = (cb * (256 - wgt) + diff * wgt) / 256; cb = max(-32 * 80, min(32 * 80, cb));
        }
    }
    if (!excluded) {
        TTHit wc; bool wsame = ttRead(pos.hash, wc);
        if (!wsame || depth >= wc.depth - 2) {
            int keepMove = bestMove ? bestMove : (wsame ? wc.move : 0);
            ttWrite(pos.hash, keepMove, toTT(best, ply), depth, best >= beta ? TT_LOWER : best > origAlpha ? TT_EXACT : TT_UPPER, rawU);
        }
    }
    return best;
}



void clearTables() {
    memset(history, 0, sizeof history); memset(contHist, 0, sizeof contHist); memset(counterMv, 0, sizeof counterMv);
    memset(corrHist, 0, sizeof corrHist); memset(corrNP, 0, sizeof corrNP); memset(killers, 0, sizeof killers);
}
// helper thread: iterative deepening on a private copy of the root position; no output, stops with stopFlag
void helperMain(int id) {
    pos = gRoot; nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestScore = 0;
    for (int d = 1; d <= maxDepthLimit && !stopFlag; d++) {
        static const int skipSize[16] = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4}, skipPhase[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
        const int hk = (id - 1) & 15;
        if (((d + skipPhase[hk]) / skipSize[hk]) % 2) continue;             // each helper skips its own pattern of depths, so the threads work on different depths
        int sd = d;
        int alpha = -INF, beta = INF, delta = 25;
        if (sd >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int sc;
        while (true) {
            sc = search(sd, alpha, beta, 0, false);
            if (stopFlag) break;
            if (sc <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (sc >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        bestScore = sc;
    }
}
}
using namespace S0;
static void (*helperFns[MAXTH])(int) = { nullptr, S1::helperMain, S2::helperMain, S3::helperMain, S4::helperMain, S5::helperMain, S6::helperMain, S7::helperMain };
static long long* nodeCtr[MAXTH] = { &S0::nodes_, &S1::nodes_, &S2::nodes_, &S3::nodes_, &S4::nodes_, &S5::nodes_, &S6::nodes_, &S7::nodes_ };
static long long totalNodes() { long long t = 0; for (int k = 0; k < numThreads && k < MAXTH; k++) t += *nodeCtr[k]; return t; }
static void clearAllTables() { S0::clearTables(); S1::clearTables(); S2::clearTables(); S3::clearTables(); S4::clearTables(); S5::clearTables(); S6::clearTables(); S7::clearTables(); }

// ---------------------------------------------------------------- experience learning (persistent across games)
struct ExpRec { int w = 0, d = 0, l = 0; };
unordered_map<U64, unordered_map<int, ExpRec>> expDB;
bool useExp = true;
string expPath = "kestrel_experience_v1.dat";
#ifdef _WIN32
// 37.1: file names are UTF-8 inside the engine; the wide-character API opens them (the ANSI code page cannot hold every folder name)
static std::wstring winPath(const string& s) {
    UINT cp = CP_UTF8; DWORD fl = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, fl, s.c_str(), -1, nullptr, 0);
    if (n <= 0) { cp = CP_ACP; fl = 0; n = MultiByteToWideChar(cp, fl, s.c_str(), -1, nullptr, 0); }      // not valid UTF-8: an old GUI sending the ANSI code page
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0'); MultiByteToWideChar(cp, fl, s.c_str(), -1, &w[0], n); w.resize((size_t)n - 1);
    return w;
}
static string exeDirUtf8() {                     // the folder of the running exe with its closing backslash, UTF-8; "" if Windows does not tell
    static wchar_t wb[32768]; DWORD n = GetModuleFileNameW(nullptr, wb, 32768);
    if (n == 0 || n >= 32768) return string();
    std::wstring w(wb, n); size_t sl = w.find_last_of(L"\\/"); if (sl == std::wstring::npos) return string();
    w.resize(sl + 1);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); if (m <= 0) return string();
    string out((size_t)m, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], m, nullptr, nullptr); out.resize((size_t)m - 1);
    return out;
}
static FILE* openFile(const string& p, const char* mode) { std::wstring wm; for (const char* c = mode; *c; c++) wm += (wchar_t)*c; return _wfopen(winPath(p).c_str(), wm.c_str()); }
#else
static FILE* openFile(const string& p, const char* mode) { return fopen(p.c_str(), mode); }
#endif
struct GameRec { U64 hash; int move; };
vector<GameRec> gameRecs;
int lastScoreCp = 0; bool haveScore = false;
const int EXP_MAX_PLY = 30, EXP_LOSS_TAIL = 4;

void loadExp() {
    expDB.clear();
    if (!useExp) return;
#ifdef _WIN32
    {   // a device name such as CON / COM1 / PRN is not an experience file: opening it for reading would block (36.2)
        HANDLE hd = CreateFileW(winPath(expPath).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hd == INVALID_HANDLE_VALUE) return;
        DWORD ft = GetFileType(hd); CloseHandle(hd);
        if (ft != FILE_TYPE_DISK) return;
    }
#endif
    FILE* f = openFile(expPath, "rb");
    if (!f) return;
    string data; { static char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n); }
    fclose(f);
    // records are "<16 hex digits> <move> <result 0..2>"; read them as token triples and skip a token at a time over anything that does not fit,
    // so a damaged or merged line only costs the records it touches, not everything after it
    vector<pair<size_t, size_t>> tk;
    for (size_t i = 0; i < data.size();) {
        while (i < data.size() && (unsigned char)data[i] <= ' ') i++;
        size_t b = i; while (i < data.size() && (unsigned char)data[i] > ' ') i++;
        if (i > b) tk.push_back({b, i - b});
    }
    auto isNum = [&](const pair<size_t, size_t>& t, bool hex) {
        for (size_t k = 0; k < t.second; k++) { char c = data[t.first + k]; if (!(c >= '0' && c <= '9') && !(hex && c >= 'a' && c <= 'f')) return false; }
        return true;
    };
    for (size_t i = 0; i + 2 < tk.size();) {
        if (tk[i].second != 16 || !isNum(tk[i], true) || tk[i + 1].second > 9 || !isNum(tk[i + 1], false) || tk[i + 2].second != 1 || !isNum(tk[i + 2], false) || data[tk[i + 2].first] > '2') { i++; continue; }
        unsigned long long h = strtoull(data.substr(tk[i].first, 16).c_str(), nullptr, 16); int mv = atoi(data.substr(tk[i + 1].first, tk[i + 1].second).c_str()); int res = data[tk[i + 2].first] - '0';
        i += 3;
        if (mv <= 0) continue;
        ExpRec& r = expDB[h][mv];
        if (res == 2) r.w++; else if (res == 0) r.l++; else r.d++;
    }
}
static bool appendToFile(const string& path, const string& data) {     // one write call, positioned at the end of the file by the system itself
#ifdef _WIN32
    HANDLE h = CreateFileW(winPath(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    if (GetFileType(h) != FILE_TYPE_DISK) { CloseHandle(h); return false; }          // 36.2: never write the experience to a device
    DWORD w = 0; BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    CloseHandle(h);
    return ok && w == data.size();
#else
    FILE* f = fopen(path.c_str(), "ab"); if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size(); fclose(f); return ok;
#endif
}
void saveGame() {
    if (!useExp || gameRecs.empty()) { gameRecs.clear(); haveScore = false; return; }
    int res = !haveScore ? 1 : lastScoreCp >= 300 ? 2 : lastScoreCp <= -300 ? 0 : 1;
    vector<GameRec> rec;
    if (res == 2) rec = gameRecs;
    else if (res == 0) { int k = (int)gameRecs.size(); for (int i = max(0, k - EXP_LOSS_TAIL); i < k; i++) rec.push_back(gameRecs[i]); }
    if (!rec.empty()) {
        string out; char ln[64];
        for (auto& r : rec) { snprintf(ln, sizeof ln, "%016llx %d %d\n", r.hash, r.move, res); out += ln; }
        appendToFile(expPath, out);
    }
    gameRecs.clear(); haveScore = false;
}


// ---------------------------------------------------------------- opening book (built from the tournament games)
struct BookEntry { string mv; int n; double pts; };
unordered_map<string, vector<BookEntry>> bookDB;
bool useBook = true;
string bookPath = "claude_book.txt";
int parseMove(const string& s);

void loadBook() {
    bookDB.clear();
    FILE* f = openFile(bookPath, "r");
    if (!f) return;
    static char buf[8192];
    while (fgets(buf, sizeof buf, f)) {
        { size_t bl = strlen(buf); if (bl == sizeof buf - 1 && buf[bl - 1] != '\n') { int ch; while ((ch = fgetc(f)) != EOF && ch != '\n') {} continue; } }   // over-long line: skip it whole
        string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        size_t tab = line.find('\t');
        if (tab == string::npos) continue;
        string key = line.substr(0, tab);
        istringstream is(line.substr(tab + 1));
        string tok; vector<BookEntry> v;
        while (is >> tok) {
            size_t a = tok.find(':'), b = tok.find(':', a + 1);
            if (a == string::npos || b == string::npos) continue;
            v.push_back({tok.substr(0, a), atoi(tok.substr(a + 1, b - a - 1).c_str()), atof(tok.substr(b + 1).c_str())});
        }
        if (!v.empty()) bookDB[key] = v;
    }
    fclose(f);
}

string bookKey(const Pos& p) {
    string s;
    for (int r = 7; r >= 0; r--) {
        int e = 0;
        for (int f = 0; f < 8; f++) {
            int pc = p.B[r * 8 + f];
            if (pc == EMPTYP) e++;
            else { if (e) { s += char('0' + e); e = 0; } s += "PNBRQKpnbrqk"[pc]; }
        }
        if (e) s += char('0' + e);
        if (r) s += '/';
    }
    s += p.side ? " b " : " w ";
    string c;
    if (p.castle & 1) c += 'K';
    if (p.castle & 2) c += 'Q';
    if (p.castle & 4) c += 'k';
    if (p.castle & 8) c += 'q';
    s += c.empty() ? "-" : c;
    return s;
}

// returns a move from the book for the current position, or 0
int bookMove() {
    if (!useBook) return 0;
    auto it = bookDB.find(bookKey(pos));
    if (it == bookDB.end()) return 0;
    vector<pair<int, double>> cand; double total = 0;
    for (auto& e : it->second) {
        if (e.n < 4) continue;
        double avg = e.pts / e.n - 0.30;
        double w = e.n * (avg > 0 ? avg * avg : 0.0);
        if (w <= 0) continue;
        int m = parseMove(e.mv);
        if (!m) continue;
        cand.push_back({m, w}); total += w;
    }
    if (cand.empty() || total <= 0) return 0;
    double r = (double)(rnd() % 1000000) / 1000000.0 * total;
    for (auto& c : cand) { r -= c.second; if (r <= 0) return c.first; }
    return cand.back().first;
}

static int multiPV = 1; static bool uciPonder = false, limitStrength = false; static int uciElo = 3200;
static bool showWDL = false, uciAnalyse = false;       // UCI_ShowWDL, UCI_AnalyseMode
static int mateLimit = 0; static vector<int> searchOnlyBan;       // 36.2: go mate N / go searchmoves (the moves that are NOT to be searched at the root)
static atomic<bool> pondering(false); static long long pendSoft = 0, pendHard = 0;
static bool limitedGo = false;                         // 38.1: `go depth / nodes / mate` without a clock or a move time = an analysis, not a game move
static bool fixedTime = false;                         // 37.1: `go movetime`: the time is used to the end, no early stop of the iterations
static long long perftStop(Pos& p, int depth) {       // 36.3: like perftRec, but ends at once when `stop` / `quit` arrives
    if (stopFlag) return 0;
    int mv[256]; int n = p.gen(mv, false); long long t = 0; int us = p.side;
    for (int i = 0; i < n && !stopFlag; i++) {
        p.make(mv[i]);
        if (!p.attacked(p.kingSq(us), us ^ 1)) t += depth == 1 ? 1 : perftStop(p, depth - 1);
        p.unmake(mv[i]);
    }
    return t;
}
static void perftWorker(int depth) {                   // go perft N: the node count of every root move, then the total (Stockfish's format)
    try {
        std::unique_ptr<Pos> pp(new Pos(pos)); Pos& p = *pp; int mv[256]; int n = p.gen(mv, false), us = p.side; long long total = 0;
        for (int i = 0; i < n && !stopFlag; i++) {
            p.make(mv[i]);
            if (!p.attacked(p.kingSq(us), us ^ 1)) {
                long long c = depth <= 1 ? 1 : perftStop(p, depth - 1);
                if (stopFlag) { p.unmake(mv[i]); break; }
                total += c; printf("%s: %lld\n", moveStr(mv[i]).c_str(), c);
            }
            p.unmake(mv[i]);
        }
        if (!stopFlag) { printf("\nNodes searched: %lld\n\n", total); }
        fflush(stdout);
    } catch (...) { fflush(stdout); }
}
static int hashfull() {
    U64 n = std::min<U64>(1000, ttMask + 1); int c = 0;
    for (U64 i = 0; i < n; i++) if (((ttBase[i].d.load(std::memory_order_relaxed) >> 44) & 3) != 0) c++;
    return (int)(c * 1000 / (int)n);
}
static long long eloNodes(int elo) { double n = 4000.0 * pow(2.0, (elo - 2400) / 100.0); if (n < 1) n = 1; if (n > 4e8) n = 4e8; return (long long)n; }
void think() {
    ttDirty = true;
    pos.rootPly = pos.ply;
    ttGen = (ttGen + 1) & 3;                // a new search = a new generation for the replacement policy
    nodes_ = 0;
    memset(killers, 0, sizeof killers);
    for (int c = 0; c < 2; c++) for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) history[c][a][b] /= 4;
    int bestMove = 0, bestScore = 0;
    int rootMoves[256]; int rn = pos.gen(rootMoves, false), legalCount = 0, onlyMove = 0, fbMove = 0, fbScore = -2000000000;
    for (int i = 0; i < rn; i++) if (pos.legalMove(rootMoves[i])) {
        legalCount++; onlyMove = rootMoves[i];
        int ms = moveScore(pos, rootMoves[i], 0, 0);                  // fallback if the search is cancelled before depth 1 completes:
        if (ms > fbScore) { fbScore = ms; fbMove = rootMoves[i]; }    // the best-ordered legal move, not the last one generated
    }
    if (legalCount == 0) { printf("bestmove 0000\n"); fflush(stdout); return; }
    if (legalCount == 1 && softMs > 0) bestMove = onlyMove;
    rootBan = searchOnlyBan;
    if (useBook && !uciAnalyse && !limitedGo && !chess960 && pos.ply <= 24 && !goInfinite && multiPV <= 1 && searchOnlyBan.empty()) {
        int bm = bookMove();
        if (bm) {
            printf("info string book move\n");
            printf("bestmove %s\n", moveStr(bm).c_str()); fflush(stdout);
            return;
        }
    }
    if (useExp && !uciAnalyse && !limitedGo && !goInfinite && multiPV <= 1 && searchOnlyBan.empty()) {
        auto it = expDB.find(pos.hash);
        if (it != expDB.end()) {
            int bestW = 0, exploit = 0; vector<int> bans;
            for (int i = 0; i < rn; i++) {
                int m = rootMoves[i];
                auto jt = it->second.find(m);
                if (jt == it->second.end() || !pos.legalMove(m)) continue;
                const ExpRec& r = jt->second;
                if (r.w >= 1 && r.l == 0 && r.w > bestW) { bestW = r.w; exploit = m; }
                if (r.l >= 1 && r.w == 0) bans.push_back(m);
            }
            if (exploit) {
                printf("info string experience: replaying a move that won before\n");
                if (pos.ply <= EXP_MAX_PLY) gameRecs.push_back({pos.hash, exploit});
                printf("bestmove %s\n", moveStr(exploit).c_str()); fflush(stdout);
                return;
            }
            if (!bans.empty() && (int)bans.size() < legalCount) {
                rootBan = bans;
                printf("info string experience: avoiding %d move(s) that lost before\n", (int)bans.size());
            }
        }
    }
    int stableIt = 0, ponderMv = 0;
    std::thread helperTh[MAXTH]; const int nth = (multiPV > 1) ? 1 : max(1, min(MAXTH, numThreads));
    struct HelperJoin { std::thread* h; int n; ~HelperJoin() { stopFlag = true; for (int k = 1; k < n; k++) if (h[k].joinable()) h[k].join(); } } helperJoin{helperTh, nth};
    if (nth > 1) {
        gRoot = pos;
        try { for (int k = 1; k < nth; k++) helperTh[k] = std::thread(helperFns[k], k); }
        catch (const std::exception&) {}              // fewer threads than asked for
    }
    for (int d = 1; d <= maxDepthLimit; d++) {
        int alpha = -INF, beta = INF, delta = 25;
        if (d >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        int s;
        while (true) {
            s = search(d, alpha, beta, 0, false);
            if (stopFlag) break;
            if (s <= alpha) { alpha = max(-INF, alpha - delta); delta *= 2; if (delta > 800) alpha = -INF; }
            else if (s >= beta) { beta = min(INF, beta + delta); delta *= 2; if (delta > 800) beta = INF; }
            else break;
        }
        if (stopFlag) break;
        int prevScoreIt = bestScore, prevBestIt = bestMove;
        bestScore = s;
        if (pvLen[0] > 0) bestMove = pvTable[0][0];
        ponderMv = pvLen[0] > 1 ? pvTable[0][1] : 0;
        if (d >= 2 && bestMove == prevBestIt) stableIt++; else stableIt = 0;
        bool scoreDrop = d >= 6 && s < prevScoreIt - 25;
        long long el = elapsedMs();
        auto fmtLine = [&](int sc_) {
            char b1[64], b2[160];
            if (abs(sc_) >= MATE_BOUND) snprintf(b1, sizeof b1, "mate %d", sc_ > 0 ? (MATE - sc_ + 1) / 2 : -(MATE + sc_ + 1) / 2); else snprintf(b1, sizeof b1, "cp %d", sc_);
            snprintf(b2, sizeof b2, " nodes %lld nps %lld hashfull %d time %lld pv", totalNodes(), el > 0 ? totalNodes() * 1000 / el : totalNodes(), hashfull(), el);
            std::string wdl;
            if (showWDL) {                       // logistic model of the score: win = 1/(1+exp((a-x)/b)), loss = the mirror, draw = the rest (per mille)
                int ww, ll;
                if (sc_ >= MATE_BOUND) { ww = 1000; ll = 0; } else if (sc_ <= -MATE_BOUND) { ww = 0; ll = 1000; }
                else { double x = max(-1500, min(1500, sc_)); ww = (int)(1000.0 / (1.0 + exp((140.0 - x) / 100.0)) + 0.5); ll = (int)(1000.0 / (1.0 + exp((140.0 + x) / 100.0)) + 0.5); }
                char wb[64]; snprintf(wb, sizeof wb, " wdl %d %d %d", ww, max(0, 1000 - ww - ll), ll); wdl = wb;
            }
            std::string ln = std::string("score ") + b1 + wdl + b2;
            for (int k = 0; k < pvLen[0]; k++) ln += " " + moveStr(pvTable[0][k]);
            return ln;
        };
        if (multiPV <= 1 || legalCount <= 1) {
            printf("info depth %d %s\n", d, fmtLine(s).c_str()); fflush(stdout);
        } else {
            struct PvLine { int score, first, second; std::string text; };
            std::vector<PvLine> lines; lines.push_back({s, bestMove, ponderMv, fmtLine(s)});
            std::vector<int> baseBan = rootBan, banned = rootBan; banned.push_back(bestMove);
            for (int pvi = 2; pvi <= std::min(multiPV, legalCount); pvi++) {
                rootBan = banned;
                int s2 = search(d, -INF, INF, 0, false);
                if (stopFlag || pvLen[0] <= 0) break;
                lines.push_back({s2, pvTable[0][0], pvLen[0] > 1 ? pvTable[0][1] : 0, fmtLine(s2)});
                banned.push_back(pvTable[0][0]);
            }
            rootBan = baseBan;
            std::stable_sort(lines.begin(), lines.end(), [](const PvLine& a, const PvLine& b) { return a.score > b.score; });
            for (size_t i = 0; i < lines.size(); i++) printf("info depth %d multipv %d %s\n", d, (int)i + 1, lines[i].text.c_str());
            fflush(stdout);
            bestMove = lines[0].first; bestScore = lines[0].score; ponderMv = lines[0].second;   // the top-ranked line is the best move
        }
        if (softMs > 0 && !fixedTime) {
            long long thr = softMs * 6 / 10;
            if (scoreDrop || stableIt == 0) thr = softMs * 9 / 10;
            if (stableIt >= 4 && !scoreDrop) thr = softMs * 4 / 10;
            if (el >= thr) break;
        }
        if (legalCount == 1 && softMs > 0 && d >= 4) break;
        if (mateLimit > 0 && s >= MATE_BOUND && (MATE - s + 1) / 2 <= mateLimit) break;       // 36.2: go mate N found
        if (abs(s) >= MATE_BOUND && d >= 8) break;
    }
    if (!bestMove) bestMove = fbMove;
    if (useExp && !uciAnalyse && !limitedGo && bestMove && !goInfinite && !pondering) { if (pos.ply <= EXP_MAX_PLY) gameRecs.push_back({pos.hash, bestMove}); lastScoreCp = bestScore; haveScore = true; }
    // UCI: after "go infinite" the engine must not report a result until it receives "stop" (also when it already found a mate)
    while (goInfinite && !stopFlag) this_thread::sleep_for(chrono::milliseconds(1));
    stopFlag = true;                                  // the helpers stop with the main search
    for (int k = 1; k < nth; k++) if (helperTh[k].joinable()) helperTh[k].join();
    if (uciPonder && ponderMv) printf("bestmove %s ponder %s\n", moveStr(bestMove).c_str(), moveStr(ponderMv).c_str());
    else printf("bestmove %s\n", moveStr(bestMove).c_str());
    fflush(stdout);
}


static Pos gThinkRoot;
static void fallbackBestmove() {
    int mv[256]; int n = pos.gen(mv, false), out = 0;
    for (int i = 0; i < n && !out; i++) if (pos.legalMove(mv[i])) out = mv[i];
    printf("bestmove %s\n", out ? moveStr(out).c_str() : "0000"); fflush(stdout);
}
static void thinkSafe() {
    gThinkRoot = pos;
    try { think(); }
    catch (...) { stopFlag = true; pos = gThinkRoot; fallbackBestmove(); }
}

static long long accMismatch = 0, accChecked = 0;
static void accWalk(Pos& p, int depth) {
    int mv[256]; int n = p.gen(mv, false); int us = p.side;
    for (int i = 0; i < n; i++) {
        p.make(mv[i]);
        if (!p.attacked(p.kingSq(us), us ^ 1)) {
            U64 hk = p.hash; int pm = p.pstMg, pe = p.pstEg, ph = p.phaseAcc;
            int32_t a0[NN_H], a1[NN_H], q0 = p.psq[0], q1 = p.psq[1];
            memcpy(a0, p.acc[0], sizeof a0); memcpy(a1, p.acc[1], sizeof a1);
            int32_t b0[NN_H], b1[NN_H], r0 = p.psq2[0], r1 = p.psq2[1];
            memcpy(b0, p.acc2[0], sizeof b0); memcpy(b1, p.acc2[1], sizeof b1);
            p.refresh();
            p.refreshPst();
            if (hk != p.computeHash() || pm != p.pstMg || pe != p.pstEg || ph != p.phaseAcc) {
                if (accMismatch < 5) printf("MISMATCH hash/pst after move %s\n", moveStr(mv[i]).c_str());
                accMismatch++;
            }
            accChecked++;
            if (memcmp(a0, p.acc[0], sizeof a0) || memcmp(a1, p.acc[1], sizeof a1) || q0 != p.psq[0] || q1 != p.psq[1] ||
                (net2Loaded && (memcmp(b0, p.acc2[0], sizeof b0) || memcmp(b1, p.acc2[1], sizeof b1) || r0 != p.psq2[0] || r1 != p.psq2[1]))) {
                if (accMismatch < 5) printf("MISMATCH after move %s (flags %d)\n", moveStr(mv[i]).c_str(), mv[i] >> 15);
                accMismatch++;
            }
            if (depth > 1) accWalk(p, depth - 1);
        }
        p.unmake(mv[i]);
    }
}
static void sync960() { for (int i = 0; i < 4; i++) g960Rook[i] = pos.cRook[i]; }       // the castling-rook squares of the position just set, for moveStr
int parseMove(const string& s) {
    int mv[256]; int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) if (moveStr(mv[i]) == s && pos.legalMove(mv[i])) return mv[i];
    return 0;
}

int main(int argc, char** argv) {
    initTables(); initPext(); initEval(); initNet(); resizeTT(hashMB); pos.refresh();
    { string dir;
#ifdef _WIN32
      dir = exeDirUtf8();                                                 // 37.1: not argv[0] (ANSI code page, and no folder at all when started by a bare name)
#endif
      if (dir.empty()) { string a0 = argc > 0 && argv[0] ? argv[0] : ""; size_t sl = a0.find_last_of("\\/"); dir = sl == string::npos ? string() : a0.substr(0, sl + 1); }
      expPath = dir + "kestrel_experience_v1.dat"; loadExp();
      bookPath = dir + "claude_book.txt"; loadBook(); rngState ^= (U64)chrono::steady_clock::now().time_since_epoch().count(); }
    for (int d = 1; d < 64; d++) for (int m = 1; m < 64; m++) lmrT[d][m] = (int)(0.8 + log((double)d) * log((double)m) / 2.2);
    pos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    if (argc > 1 && string(argv[1]) == "perft") {
        struct T { const char* fen; int d; long long n; } tests[] = {
            {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609},
            {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
            {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
            {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
            {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
            {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594}};
        for (auto& t : tests) {
            pos.setFen(t.fen);
            auto a = chrono::steady_clock::now();
            long long n = perftRec(pos, t.d);
            auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - a).count();
            printf("%s depth %d got %lld expected %lld (%lld ms)\n", n == t.n ? "OK  " : "FAIL", t.d, n, t.n, (long long)ms);
        }
        return 0;
    }

    if (argc > 1 && string(argv[1]) == "bench") {
        const char* fens[] = {
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
            "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
            "r1bq1rk1/pp2ppbp/2np1np1/8/3NP3/2N1BP2/PPPQ2PP/R3KB1R w KQ - 0 9",
            "2rq1rk1/pp1bppbp/3p1np1/8/2PNP3/2N1B3/PP2BPPP/R2Q1RK1 b - - 0 11",
            "8/5pk1/6p1/8/8/6P1/5PK1/3R4 w - - 0 1",
            "4r1k1/pp3ppp/2p5/8/3P4/2P2N2/PP3PPP/4R1K1 w - - 0 1"};
        int bdepth = argc > 2 ? atoi(argv[2]) : 12;
        long long totalNodes = 0, totalMs = 0;
        for (const char* f : fens) {
            pos.setFen(f); resizeTT(hashMB); memset(history, 0, sizeof history);
            softMs = hardMs = 0; nodeLimit = 0; maxDepthLimit = bdepth; useExp = false;
            stopFlag = false; t0 = chrono::steady_clock::now(); think();
            totalNodes += nodes_; totalMs += elapsedMs();
        }
        printf("bench: nodes %lld time %lld ms nps %lld\n", totalNodes, totalMs, totalMs > 0 ? totalNodes * 1000 / totalMs : 0);
        return 0;
    }
    if (argc > 1 && string(argv[1]) == "acctest") {
        const char* fens[] = {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
            "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
            "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"};
        for (const char* f : fens) { pos.setFen(f); accWalk(pos, 3); }
        printf("acctest: checked %lld positions, mismatches %lld\n", accChecked, accMismatch);
        return 0;
    }
    if (argc > 3 && string(argv[1]) == "perftfile") {   // perftfile depth file : one FEN per line (text after a TAB is ignored), prints the node count per line
        int d = atoi(argv[2]); FILE* in = fopen(argv[3], "r"); static char buf[1024];
        if (!in) return 1;
        while (fgets(buf, sizeof buf, in)) {
            string l(buf); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            size_t t = l.find('\t'); if (t != string::npos) l = l.substr(0, t);
            pos.setFen(l); printf("%lld\n", perftRec(pos, d));
        }
        fclose(in); return 0;
    }
    if (argc > 3 && string(argv[1]) == "evalfile") {
        FILE* in = fopen(argv[2], "r"); FILE* o = fopen(argv[3], "w");
        static char buf[1024]; int lim = argc > 4 ? atoi(argv[4]) : 1000000;
        if (argc > 5) nnBlend = atoi(argv[5]);
        while (lim-- > 0 && fgets(buf, sizeof buf, in)) {
            string line(buf); size_t tab = line.find('\t'); pos.setFen(line.substr(0, tab));
            fprintf(o, "%d\n", evaluate(pos));
        }
        fclose(in); fclose(o); return 0;
    }
    if (argc > 3 && string(argv[1]) == "nnuedump") {
        FILE* in = fopen(argv[2], "r"); FILE* o = fopen(argv[3], "wb");
        static char buf[1024]; long long cnt = 0;
        while (fgets(buf, sizeof buf, in)) {
            string line(buf); size_t tab = line.find('\t'); string fen = line.substr(0, tab);
            pos.setFen(fen);
            int stm = pos.side; short fs[32], fo[32]; int n = 0;
            for (int sq = 0; sq < 64 && n < 32; sq++) if (pos.B[sq] != EMPTYP) {
                fs[n] = (short)(pos.kOff[stm] + nnBase(stm, pos.B[sq], sq, pos.kMir[stm])); fo[n] = (short)(pos.kOff[stm ^ 1] + nnBase(stm ^ 1, pos.B[sq], sq, pos.kMir[stm ^ 1])); n++;
            }
            short nn = (short)n; fwrite(&nn, 2, 1, o); fwrite(fs, 2, n, o); fwrite(fo, 2, n, o); cnt++;
        }
        fclose(in); fclose(o); printf("nnuedumped %lld positions\n", cnt); return 0;
    }
    if (argc > 3 && string(argv[1]) == "dump") {
        FILE* in = fopen(argv[2], "r"); FILE* o = fopen(argv[3], "wb");
        static char buf[1024]; long long cnt = 0;
        while (fgets(buf, sizeof buf, in)) {
            string line(buf); size_t tab = line.find('\t'); string fen = line.substr(0, tab);
            pos.setFen(fen);
            vector<Feat> fs; int fixedMg = 0, ph = 0;
            evalCore<true>(pos, &fs, &fixedMg, &ph);
            int acc[NF]; memset(acc, 0, sizeof acc);
            for (auto& f : fs) acc[f.idx] += f.v;
            vector<int> rec; int n = 0;
            for (int i = 0; i < NF; i++) if (acc[i]) { rec.push_back(i); rec.push_back(acc[i]); n++; }
            int hdr[4] = {n, ph, fixedMg, pos.side};
            fwrite(hdr, 4, 4, o); fwrite(rec.data(), 4, rec.size(), o); cnt++;
        }
        fclose(in); fclose(o); printf("dumped %lld positions, NF=%d\n", cnt, (int)NF); return 0;
    }
    if (argc > 2 && string(argv[1]) == "weights") {
        FILE* o = fopen(argv[2], "w");
        fprintf(o, "static const int TUNED_MG[%d] = {", (int)NF);
        for (int i = 0; i < NF; i++) fprintf(o, "%d%s", Wmg[i], i + 1 < NF ? "," : "");
        fprintf(o, "};\nstatic const int TUNED_EG[%d] = {", (int)NF);
        for (int i = 0; i < NF; i++) fprintf(o, "%d%s", Weg[i], i + 1 < NF ? "," : "");
        fprintf(o, "};\n"); fclose(o); return 0;
    }
    thread worker;
    string line;
    while (getline(cin, line)) {
        istringstream is(line); string cmd; is >> cmd;
        try {
        if (cmd == "uci") {
            printf("id name Kestrel 1.0\nid author Claude\noption name Hash type spin default 64 min 1 max 4096\noption name Threads type spin default 1 min 1 max 8\noption name NNBlend type spin default 70 min 0 max 100\noption name LazyMargin type spin default 0 min 0 max 3000\noption name OwnBook type check default true\noption name Net2Share type spin default 70 min 0 max 100\noption name UseNet2 type check default true\noption name EvalFlags type spin default 1 min 0 max 255\noption name QsTT type spin default 1 min 0 max 1\noption name CorrFlags type spin default 2 min 0 max 2\noption name Move Overhead type spin default 5 min 0 max 5000\noption name EvalCache type spin default 1 min 0 max 1\noption name V31Flags type spin default 0 min 0 max 255\noption name V31HistPrune type spin default 2500 min 0 max 20000\noption name V31CutR type spin default 1 min 0 max 3\noption name V31DE type spin default 20 min 0 max 200\noption name MultiPV type spin default 1 min 1 max 8\noption name Ponder type check default false\noption name Clear Hash type button\noption name UCI_LimitStrength type check default false\noption name UCI_Elo type spin default 3200 min 1320 max 3200\noption name UseNNUE type check default true\noption name Experience type check default true\noption name ExperienceFile type string default kestrel_experience_v1.dat\noption name UCI_ShowWDL type check default false\noption name UCI_AnalyseMode type check default false\noption name UCI_Chess960 type check default false\nuciok\n"); fflush(stdout);
        } else if (cmd == "isready") { printf("readyok\n"); fflush(stdout); }
        else if (cmd == "ucinewgame") {
            { stopFlag = true; goInfinite = false; if (worker.joinable()) worker.join(); }
            saveGame(); loadExp();
            resizeTT(hashMB); clearAllTables();
            pos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        } else if (cmd == "setoption") {
            string tok, name, value; is >> tok;
            while (is >> tok && tok != "value") name += (name.empty() ? "" : " ") + tok;
            { string rest; getline(is, rest); size_t a = rest.find_first_not_of(" " + string(1, 9)); value = a == string::npos ? string() : rest.substr(a); while (!value.empty() && (value.back() == 13 || value.back() == 32)) value.pop_back(); }
            { stopFlag = true; goInfinite = false; if (worker.joinable()) worker.join(); }
            {   // 36.3: UCI option names and check values are not case sensitive
                static const char* declared[] = {"Hash", "Threads", "NNBlend", "LazyMargin", "OwnBook", "Net2Share", "UseNet2", "EvalFlags", "QsTT", "CorrFlags", "Move Overhead", "EvalCache",
                                                 "V31Flags", "V31HistPrune", "V31CutR", "V31DE", "MultiPV", "Ponder", "Clear Hash", "UCI_LimitStrength", "UCI_Elo", "UseNNUE", "Experience", "ExperienceFile", "UCI_ShowWDL", "UCI_AnalyseMode", "UCI_Chess960"};
                auto low = [](string s) { for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32); return s; };
                string ln = low(name);
                for (const char* d : declared) if (low(d) == ln) { name = d; break; }
                string lv = low(value); if (name != "ExperienceFile") value = lv == "true" ? string("true") : lv == "false" ? string("false") : value;
            }
            if (name == "UCI_ShowWDL") showWDL = (value == "true");
            if (name == "UCI_AnalyseMode") uciAnalyse = (value == "true");
            if (name == "UCI_Chess960") chess960 = (value == "true");
            if (name == "LazyMargin") lazyMargin = max(0, atoi(value.c_str()));
            if (name == "OwnBook") useBook = (value == "true");
            if (name == "CorrFlags") corrFlags = max(0, min(2, atoi(value.c_str())));
            if (name == "Move Overhead") moveOverhead = max(0, min(5000, atoi(value.c_str())));
            if (name == "EvalCache") evalCache = max(0, min(1, atoi(value.c_str())));
            if (name == "V31Flags") v31Flags = max(0, min(255, atoi(value.c_str())));
            if (name == "V31HistPrune") v31HP = max(0, min(20000, atoi(value.c_str())));
            if (name == "V31CutR") v31CutR = max(0, min(3, atoi(value.c_str())));
            if (name == "V31DE") v31DE = max(0, min(200, atoi(value.c_str())));
            if (name == "Threads") numThreads = max(1, min(MAXTH, atoi(value.c_str())));
            if (name == "MultiPV") multiPV = max(1, min(8, atoi(value.c_str())));
            if (name == "Ponder") uciPonder = (value == "true");
            if (name == "Clear Hash") { resizeTT(hashMB); clearAllTables(); }
            if (name == "UCI_LimitStrength") limitStrength = (value == "true");
            if (name == "UCI_Elo") uciElo = max(1320, min(3200, atoi(value.c_str())));
            if (name == "QsTT") qsTT = max(0, min(1, atoi(value.c_str())));
            if (name == "EvalFlags") evalFlags = max(0, min(255, atoi(value.c_str())));
            if (name == "NNBlend") nnBlend = max(0, min(100, atoi(value.c_str())));
            if (name == "Net2Share") net2Share = max(0, min(100, atoi(value.c_str())));
            if (name == "UseNet2") useNet2 = (value == "true");
            if (name == "UseNNUE") { nnueReady = netLoaded && value == "true"; pos.refresh(); }
            if (name == "Experience") { useExp = (value == "true"); loadExp(); }
            else if (name == "ExperienceFile") { expPath = value; loadExp(); }
            if (name == "Hash") { hashMB = max(1, min(4096, atoi(value.c_str()))); resizeTT(hashMB); }
        } else if (cmd == "position") {
            { stopFlag = true; goInfinite = false; if (worker.joinable()) worker.join(); }
            string tok; is >> tok;
            if (tok == "startpos") { pos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); sync960(); is >> tok; }
            bool fenOk = true;
            if (tok == "fen") {
                string fen; while (is >> tok && tok != "moves") fen += tok + " ";
                // an illegal position (no king, two kings, pawns on the back ranks, the side not to move in check, too many pieces)
                // would index outside the board tables: check it first and keep the previous position if it is not legal
                static Pos fenTest;
                fenTest.setFen(fen);
                fenOk = popc(fenTest.P[5]) == 1 && popc(fenTest.P[11]) == 1 && !((fenTest.P[0] | fenTest.P[6]) & (RankBB[0] | RankBB[7])) &&
                        popc(fenTest.Occ[0]) <= 16 && popc(fenTest.Occ[1]) <= 16 && popc(fenTest.P[0]) <= 8 && popc(fenTest.P[6]) <= 8 &&
                        !fenTest.attacked(fenTest.kingSq(fenTest.side ^ 1), fenTest.side);
                if (fenOk) { pos.setFen(fen); sync960(); }
                else { printf("info string illegal position ignored\n"); fflush(stdout); }
            }
            if (fenOk && tok == "moves") { while (is >> tok) { if (pos.ply >= 3900) break; int m = parseMove(tok); if (!m) break; pos.make(m); } }      // 36.2: stop at the first illegal move
        } else if (cmd == "go" && (" " + line + " ").find(" perft ") != string::npos) {          // 36.3: go perft N
            { stopFlag = true; goInfinite = false; if (worker.joinable()) worker.join(); }
            int pn = 0; { istringstream is3(line); string t3; while (is3 >> t3) if (t3 == "perft") is3 >> pn; }
            pn = max(1, min(pn, 30));
            stopFlag = false; goInfinite = false; pondering = false;
            try { worker = thread(perftWorker, pn); } catch (const std::exception&) { stopFlag = true; }
        } else if (cmd == "go") {
            { stopFlag = true; goInfinite = false; if (worker.joinable()) worker.join(); }
            long long wt = 0, bt = 0, wi = 0, bi = 0, mt = 0, mtg = 0; int depth = 0; long long nl = 0; bool inf = false, ponderGo = false;
            int mateN = 0; bool smMode = false; vector<int> smList; mateLimit = 0; searchOnlyBan.clear();
            string tok;
            while (is >> tok) {
                if (tok == "wtime" || tok == "btime" || tok == "winc" || tok == "binc" || tok == "movetime" || tok == "movestogo" || tok == "depth" || tok == "nodes" ||
                    tok == "infinite" || tok == "ponder" || tok == "mate" || tok == "searchmoves") smMode = false;       // 36.2: the move list of searchmoves ends at the next keyword
                if (tok == "wtime") is >> wt; else if (tok == "btime") is >> bt; else if (tok == "winc") is >> wi;
                else if (tok == "binc") is >> bi; else if (tok == "movetime") is >> mt; else if (tok == "movestogo") is >> mtg;
                else if (tok == "depth") is >> depth; else if (tok == "nodes") is >> nl; else if (tok == "infinite") inf = true; else if (tok == "ponder") ponderGo = true;
                else if (tok == "mate") is >> mateN; else if (tok == "searchmoves") smMode = true;
                else if (smMode) { int smv = parseMove(tok); if (smv) smList.push_back(smv); }
            }
            { bool anyLimit = false; istringstream is2(line); string t2; is2 >> t2;
              while (is2 >> t2) if (t2 == "wtime" || t2 == "btime" || t2 == "movetime" || t2 == "depth" || t2 == "nodes" || t2 == "infinite" || t2 == "ponder" || t2 == "mate") anyLimit = true;
              if (!anyLimit) inf = true; }
            mateLimit = mateN > 0 ? mateN : 0;
            if (!smList.empty()) {                       // searchmoves: ban every legal root move that is not in the list (only if at least one listed move is legal)
                int rmv[256]; int rcnt = pos.gen(rmv, false), allowed = 0; vector<int> ban;
                for (int i = 0; i < rcnt; i++) if (pos.legalMove(rmv[i])) { bool in = false; for (int x : smList) if (x == rmv[i]) in = true; if (in) allowed++; else ban.push_back(rmv[i]); }
                if (allowed > 0) searchOnlyBan = ban;
            }
            softMs = hardMs = 0; nodeLimit = nl; maxDepthLimit = depth > 0 ? depth : 64;
            if (limitStrength) { long long en = eloNodes(uciElo); if (nodeLimit == 0 || en < nodeLimit) nodeLimit = en; }
            fixedTime = mt > 0;
            limitedGo = (depth > 0 || nl > 0 || mateN > 0) && mt <= 0 && (pos.side == 0 ? wt : bt) <= 0;
            if (mt > 0) { long long ov = moveOverhead > 5 ? min(moveOverhead, mt / 2) : 0; softMs = mt; hardMs = max(1LL, mt - max(min(30LL, mt / 4), ov)); }
            else if (!inf && ((depth == 0 && nl == 0 && mateN == 0) || (pos.side == 0 ? wt : bt) > 0)) {      // 36.1: a clock also counts next to a depth / node limit
                long long myT = max(1LL, (pos.side == 0 ? wt : bt) - moveOverhead), myI = pos.side == 0 ? wi : bi;
                long long mtgv = mtg > 0 ? min(mtg, 40LL) : 28;
                softMs = max(10LL, myT / mtgv + myI * 3 / 4);
                hardMs = min(softMs * 4, max(10LL, myT / 3));
                hardMs = max(hardMs, 10LL);
                hardMs = min(hardMs, max(1LL, myT * 3 / 4));
            }
            if (ponderGo) { pendSoft = softMs; pendHard = hardMs; softMs = hardMs = 0; pondering = true; inf = true; } else pondering = false;
            t0 = chrono::steady_clock::now();
            stopFlag = false; goInfinite = inf;
            try { worker = thread(thinkSafe); }
            catch (const std::exception&) { stopFlag = true; fallbackBestmove(); }
        } else if (cmd == "ponderhit") {
            if (pondering) { softMs = pendSoft; hardMs = pendHard; t0 = chrono::steady_clock::now(); pondering = false; goInfinite = false; }
        } else if (cmd == "stop") { stopFlag = true; if (worker.joinable()) worker.join(); }
        else if (cmd == "quit") { stopFlag = true; if (worker.joinable()) worker.join(); saveGame(); break; }
        } catch (const std::exception& ex) { printf("info string error: %s\n", ex.what()); fflush(stdout); }
    }
    if (worker.joinable()) { stopFlag = true; worker.join(); }
    saveGame();
    return 0;
}
