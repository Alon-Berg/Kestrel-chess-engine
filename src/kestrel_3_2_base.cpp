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
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#ifdef __BMI2__
#include <immintrin.h>                               // _pext_u64
#endif
#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
using namespace std;
typedef unsigned long long U64;
#ifdef _WIN32
using KThread = std::thread;
#else
#include <pthread.h>
#include <stdexcept>
#include <exception>
struct KThread {      // 40.1: std::thread with an 8 MB stack (the default thread stack is 128 KB with musl and 512 KB on macOS; one search frame is ~2.7 KB and a search can reach 128 plies)
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
#endif

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
#ifdef __BMI2__
static inline U64 pext64(U64 v, U64 m) { return _pext_u64(v, m); }
#else
static inline U64 pext64(U64 v, U64 m) {            // 40.1: this build does not target BMI2; plain-C PEXT, never called (usePext stays false)
    U64 r = 0; for (U64 b = 1; m; b += b) { U64 l = m & (0 - m); if (v & l) r |= b; m &= m - 1; }
    return r;
}
#endif
static bool usePext = false;
static U64 rookTabP[102400], bishopTabP[5248], rookMaskP[64], bishopMaskP[64]; static int rookOffP[64], bishopOffP[64];
static bool pextIsFast() {                    // PEXT is one cheap instruction on Intel and on AMD Zen 3+; Zen 1 / 2 (family 0x17) emulate it in microcode
    if (const char* e = getenv("CLAUDE_NO_PEXT")) if (e[0] == '1') return false;
#if defined(__BMI2__) && (defined(__x86_64__) || defined(__i386__))
    if (const char* e = getenv("CLAUDE_FORCE_PEXT")) if (e[0] == '1') return true;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0, &a, &b, &c, &d)) return false;
    const bool intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e, amd = b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    unsigned fam = (a >> 8) & 0xF; if (fam == 0xF) fam += (a >> 20) & 0xFF;
    return intel || (amd && fam >= 0x19);
#else
    return false;                                    // 40.1: generic x86-64, ARM, ...: the shift-based attack tables
#endif
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
        U64 x = 0; do { rookTabP[ro + pext64(x, rm)] = rookAttOld(sq, x); x = (x - rm) & rm; } while (x);
        ro += 1 << popc(rm);
        x = 0; do { bishopTabP[bo + pext64(x, bm)] = bishopAttOld(sq, x); x = (x - bm) & bm; } while (x);
        bo += 1 << popc(bm);
    }
}
static inline U64 bishopAtt(int sq, U64 occ) { return usePext ? bishopTabP[bishopOffP[sq] + pext64(occ, bishopMaskP[sq])] : bishopAttOld(sq, occ); }
static inline U64 rookAtt(int sq, U64 occ) { return usePext ? rookTabP[rookOffP[sq] + pext64(occ, rookMaskP[sq])] : rookAttOld(sq, occ); }
static inline U64 queenAtt(int sq, U64 occ) { return bishopAtt(sq, occ) | rookAtt(sq, occ); }

// ---------------------------------------------------------------- neural network evaluation (Kestrel 2: one large network)
#if defined(__AVX2__)
#include <immintrin.h>
#endif
// File format (kestrel_net.bin): 64-byte header ("KSTRNET2", int32 version, L1, inputs, output buckets, QA, L2, L3, float centipawns per output unit), then
//   int16 ftW[inputs][L1] (king-bucket factoriser already folded in), int16 ftB[L1], float W2[NOB][L1][L2], b2[NOB][L2], W3[NOB][2*L2][L3], b3[NOB][L3], W4[NOB][L3], b4[NOB]
const int NN_KB = 32, NN_IN = 768, NN_INB = NN_IN * NN_KB;
#ifndef KESTREL_H1
#define KESTREL_H1 1536
#endif
static_assert(KESTREL_H1 % 64 == 0, "the network width must be a multiple of 64");
const int NN_H = KESTREL_H1, NN_H2 = NN_H / 2, NN_L2 = 16, NN_L3 = 32, NN_NOB = 8, NN_QA = 181;
static const unsigned char nnKBMap[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};    // (king rank) * 4 + (king file after mirroring) -> bucket
#if defined(__APPLE__)
#define KNET_SYM(x) "_" #x
#define KNET_SECTION ".section __DATA,__const"
#elif defined(_WIN32)
#define KNET_SYM(x) #x
#define KNET_SECTION ".section .rdata,\"dr\""
#else
#define KNET_SYM(x) #x
#define KNET_SECTION ".section .rodata"
#endif
__asm__(KNET_SECTION "\n.globl " KNET_SYM(kestrel_net_data) "\n.p2align 6\n" KNET_SYM(kestrel_net_data) ":\n.incbin \"kestrel_net.bin\"\n.globl " KNET_SYM(kestrel_net_end) "\n" KNET_SYM(kestrel_net_end) ":\n.byte 0\n.text\n");
extern "C" const unsigned char kestrel_net_data[];
extern "C" const unsigned char kestrel_net_end[];
static const int16_t* nnW1 = nullptr;      // [NN_INB][NN_H]
static const int16_t* nnB1 = nullptr;      // [NN_H]
static const float *nnHW2 = nullptr, *nnHb2 = nullptr, *nnHW3 = nullptr, *nnHb3 = nullptr, *nnHW4 = nullptr, *nnHb4 = nullptr;
static float nnCpPerUnit = 173.7f;
static float nnScale = 1.0f;                // UCI option EvalScale (percent): multiplies the network output
alignas(64) static int16_t nnW2q[NN_NOB][NN_L2][NN_H];      // W2 transposed and quantized (scale NN_W2S) for the integer dot products of layer 2
static const float NN_W2S = 512.0f;
alignas(64) static int16_t nnW2pm[NN_NOB][NN_H2][NN_L2][2];     // the same weights pair-major: for each pair of layer-2 inputs (2j, 2j + 1) the 16 outputs side by side (sparse kernel)
static unsigned nnGen = 1;                      // changes whenever a net is attached: the king-refresh caches of every position become invalid
#if defined(__AVX2__)
alignas(16) static uint16_t nnzLut[256][8];     // for a mask of non-zero input pairs: the indices of its set bits
static void nnzInit() { for (int m = 0; m < 256; m++) { int n = 0; for (int b = 0; b < 8; b++) if ((m >> b) & 1) nnzLut[m][n++] = (uint16_t)b; while (n < 8) nnzLut[m][n++] = 0; } }
#endif
#ifdef KSTATS
static long long nzPairs = 0, nzEvals = 0;
static void kstatsReport() { printf("non-zero input pairs per evaluation: %.1f of %d (%lld evaluations)\n", nzEvals ? (double)nzPairs / (double)nzEvals : 0.0, NN_H2, nzEvals); }
#endif
static bool nnueReady = false, netLoaded = false;
static bool net2Loaded = false, useNet2 = false;      // (the second network of 1.x no longer exists)
static unsigned char* nnExternal = nullptr;           // a net loaded with the EvalFile option
static bool nnAttach(const unsigned char* d, size_t len) {
    const size_t nft = (size_t)NN_INB * NN_H, nhead = (size_t)NN_NOB * (NN_H * NN_L2 + NN_L2 + 2 * NN_L2 * NN_L3 + NN_L3 + NN_L3) + NN_NOB;
    if (len < 64 + (nft + NN_H) * 2 + nhead * 4 || memcmp(d, "KSTRNET2", 8) != 0) return false;
    int32_t h[7]; memcpy(h, d + 8, sizeof h);
    if (h[0] != 1 || h[1] != NN_H || h[2] != NN_INB || h[3] != NN_NOB || h[4] != NN_QA || h[5] != NN_L2 || h[6] != NN_L3) return false;
    memcpy(&nnCpPerUnit, d + 40, 4);
    nnW1 = (const int16_t*)(d + 64); nnB1 = nnW1 + nft;
    const float* f = (const float*)(d + 64 + (nft + NN_H) * 2);
    nnHW2 = f; f += (size_t)NN_NOB * NN_H * NN_L2; nnHb2 = f; f += NN_NOB * NN_L2; nnHW3 = f; f += (size_t)NN_NOB * 2 * NN_L2 * NN_L3; nnHb3 = f; f += NN_NOB * NN_L3;
    nnHW4 = f; f += NN_NOB * NN_L3; nnHb4 = f;
    for (int ob = 0; ob < NN_NOB; ob++) for (int k = 0; k < NN_L2; k++) for (int i = 0; i < NN_H; i++) {
        float w = nnHW2[((size_t)ob * NN_H + i) * NN_L2 + k] * NN_W2S; w = w > 32767.0f ? 32767.0f : (w < -32767.0f ? -32767.0f : w); nnW2q[ob][k][i] = (int16_t)lrintf(w); }
    for (int ob = 0; ob < NN_NOB; ob++) for (int j = 0; j < NN_H2; j++) for (int k = 0; k < NN_L2; k++) { nnW2pm[ob][j][k][0] = nnW2q[ob][k][2 * j]; nnW2pm[ob][j][k][1] = nnW2q[ob][k][2 * j + 1]; }
    nnGen++;
#if defined(__AVX2__)
    nnzInit();
#endif
    return true;
}
static void initNet() {
    netLoaded = nnAttach(kestrel_net_data, (size_t)(kestrel_net_end - kestrel_net_data));
    nnueReady = netLoaded;
}
static bool nnLoadFile(const string& path) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 64) { fclose(f); return false; }
    unsigned char* buf = (unsigned char*)malloc((size_t)len + 64); if (!buf) { fclose(f); return false; }
    unsigned char* al = (unsigned char*)(((uintptr_t)buf + 63) & ~(uintptr_t)63);
    const bool ok = fread(al, 1, (size_t)len, f) == (size_t)len; fclose(f);
    if (!ok || !nnAttach(al, (size_t)len)) { free(buf); initNet(); return false; }
    if (nnExternal) free(nnExternal);
    nnExternal = buf; netLoaded = nnueReady = true; return true;
}
#if defined(__AVX2__) && defined(__FMA__)
// layers 3 and 4 from the integer layer-2 sums: z2 -> u = [clip(z2), clip(z2)^2] -> z3 = b3 + u W3 (four registers, summed in the order j = 0..31) -> out (sequential fused chain)
static inline int netTail(const float* accf, int ob, float sc) {
    const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f), scv = _mm256_set1_ps(sc);
    alignas(32) float u[2 * NN_L2];
    for (int h = 0; h < 2; h++) {
        const __m256 z = _mm256_fmadd_ps(_mm256_load_ps(accf + 8 * h), scv, _mm256_loadu_ps(nnHb2 + ob * NN_L2 + 8 * h));
        const __m256 a = _mm256_min_ps(_mm256_max_ps(z, zero), one);
        _mm256_store_ps(u + 8 * h, a); _mm256_store_ps(u + NN_L2 + 8 * h, _mm256_mul_ps(a, a));
    }
    const float* w3 = nnHW3 + (size_t)ob * 2 * NN_L2 * NN_L3;
    __m256 z30 = _mm256_loadu_ps(nnHb3 + ob * NN_L3), z31 = _mm256_loadu_ps(nnHb3 + ob * NN_L3 + 8), z32 = _mm256_loadu_ps(nnHb3 + ob * NN_L3 + 16), z33 = _mm256_loadu_ps(nnHb3 + ob * NN_L3 + 24);
    for (int j = 0; j < 2 * NN_L2; j++) {
        const __m256 uj = _mm256_broadcast_ss(u + j); const float* w = w3 + j * NN_L3;
        z30 = _mm256_fmadd_ps(uj, _mm256_loadu_ps(w), z30); z31 = _mm256_fmadd_ps(uj, _mm256_loadu_ps(w + 8), z31);
        z32 = _mm256_fmadd_ps(uj, _mm256_loadu_ps(w + 16), z32); z33 = _mm256_fmadd_ps(uj, _mm256_loadu_ps(w + 24), z33);
    }
    alignas(32) float z3[NN_L3];
    _mm256_store_ps(z3, _mm256_min_ps(_mm256_max_ps(z30, zero), one)); _mm256_store_ps(z3 + 8, _mm256_min_ps(_mm256_max_ps(z31, zero), one));
    _mm256_store_ps(z3 + 16, _mm256_min_ps(_mm256_max_ps(z32, zero), one)); _mm256_store_ps(z3 + 24, _mm256_min_ps(_mm256_max_ps(z33, zero), one));
    float out = nnHb4[ob];
    for (int l = 0; l < NN_L3; l++) out = __builtin_fmaf(z3[l], nnHW4[ob * NN_L3 + l], out);
    float cp = out * nnCpPerUnit * nnScale;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
}
#endif
// the network output for the side to move, in centipawns: S / O = accumulators of the side to move / of the other side
#ifdef OVFCHECK2
static long long ovfN = 0, ovfBad = 0, ovfBeyond = 0;
struct OvfPrinter { ~OvfPrinter() { fprintf(stderr, "OVF2: neuron sums %lld, float sum differs from the exact 64-bit sum: %lld, exact sums beyond int32 (handled): %lld\n", ovfN, ovfBad, ovfBeyond); } } ovfPrinter;
#endif
static int netEval(const int16_t* S, const int16_t* O, int npieces) {
#if defined(__AVX2__)
    alignas(32) int16_t x[NN_H];                 // pairwise products of the clipped accumulators, at most 181 * 181 = 32761 (exact in int16)
    alignas(16) uint16_t nz[NN_H2 + 8];          // indices of the pairs (x[2j], x[2j + 1]) that are not both zero
    int cnt = 0;
    {
        const __m256i zero = _mm256_setzero_si256(), qa = _mm256_set1_epi16(NN_QA);
        __m128i base = _mm_setzero_si128(); const __m128i eight = _mm_set1_epi16(8);
        for (int h = 0; h < 2; h++) {
            const int16_t* T = h ? O : S;
            for (int i = 0; i < NN_H2; i += 16) {
                const __m256i a = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256((const __m256i*)(T + i)), zero), qa);
                const __m256i b = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256((const __m256i*)(T + i + NN_H2)), zero), qa);
                const __m256i pr = _mm256_mullo_epi16(a, b);
                _mm256_store_si256((__m256i*)(x + h * NN_H2 + i), pr);
                const unsigned m = ~(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(pr, zero))) & 255u;
                _mm_storeu_si128((__m128i*)(nz + cnt), _mm_add_epi16(_mm_load_si128((const __m128i*)nnzLut[m]), base));
                cnt += __builtin_popcount(m); base = _mm_add_epi16(base, eight);
            }
        }
    }
#ifdef KSTATS
    nzPairs += cnt; nzEvals++;
#endif
    const int ob = npieces <= 2 ? 0 : std::min(NN_NOB - 1, (npieces - 2) / 4);
    const float sc = 1.0f / ((float)(NN_QA * NN_QA) * NN_W2S);
    const int16_t* wb = &nnW2pm[ob][0][0][0];
    __m256 a0 = _mm256_setzero_ps(), a1 = a0, b0 = a0, b1 = a0;           // 2.3: float accumulators (an int32 sum can overflow for a strongly negative neuron)
    int c = 0;
    for (; c + 1 < cnt; c += 2) {
        const int j0 = nz[c], j1 = nz[c + 1]; int t0, t1;
        memcpy(&t0, x + 2 * j0, 4); memcpy(&t1, x + 2 * j1, 4);
        const __m256i v0 = _mm256_set1_epi32(t0), v1 = _mm256_set1_epi32(t1);
        const __m256i* w0 = (const __m256i*)(wb + j0 * 32); const __m256i* w1 = (const __m256i*)(wb + j1 * 32);
        a0 = _mm256_add_ps(a0, _mm256_cvtepi32_ps(_mm256_madd_epi16(v0, _mm256_load_si256(w0))));
        a1 = _mm256_add_ps(a1, _mm256_cvtepi32_ps(_mm256_madd_epi16(v0, _mm256_load_si256(w0 + 1))));
        b0 = _mm256_add_ps(b0, _mm256_cvtepi32_ps(_mm256_madd_epi16(v1, _mm256_load_si256(w1))));
        b1 = _mm256_add_ps(b1, _mm256_cvtepi32_ps(_mm256_madd_epi16(v1, _mm256_load_si256(w1 + 1))));
    }
    if (c < cnt) {
        const int j0 = nz[c]; int t0; memcpy(&t0, x + 2 * j0, 4);
        const __m256i v0 = _mm256_set1_epi32(t0); const __m256i* w0 = (const __m256i*)(wb + j0 * 32);
        a0 = _mm256_add_ps(a0, _mm256_cvtepi32_ps(_mm256_madd_epi16(v0, _mm256_load_si256(w0))));
        a1 = _mm256_add_ps(a1, _mm256_cvtepi32_ps(_mm256_madd_epi16(v0, _mm256_load_si256(w0 + 1))));
    }
    alignas(32) float accf[NN_L2];
    _mm256_store_ps(accf, _mm256_add_ps(a0, b0)); _mm256_store_ps(accf + 8, _mm256_add_ps(a1, b1));
#if defined(__FMA__)
    return netTail(accf, ob, sc);
#else
    float z2[NN_L2];
    for (int k = 0; k < NN_L2; k++) z2[k] = nnHb2[ob * NN_L2 + k] + accf[k] * sc;
#endif
#else
    alignas(64) int16_t x[NN_H];
    for (int i = 0; i < NN_H2; i++) {
        int a = S[i], b = S[i + NN_H2], c = O[i], d = O[i + NN_H2];
        a = a < 0 ? 0 : (a > NN_QA ? NN_QA : a); b = b < 0 ? 0 : (b > NN_QA ? NN_QA : b); c = c < 0 ? 0 : (c > NN_QA ? NN_QA : c); d = d < 0 ? 0 : (d > NN_QA ? NN_QA : d);
        x[i] = (int16_t)(a * b); x[NN_H2 + i] = (int16_t)(c * d);          // at most 181 * 181 = 32761
    }
    const int ob = npieces <= 2 ? 0 : std::min(NN_NOB - 1, (npieces - 2) / 4);
    const float sc = 1.0f / ((float)(NN_QA * NN_QA) * NN_W2S);
    uint16_t nzp[NN_H2]; int cnt = 0;                      // 2.3: the pairs (x[2j], x[2j + 1]) that are not both zero, in increasing order (the AVX2 code lists them the same way)
    for (int j = 0; j < NN_H2; j++) if (x[2 * j] | x[2 * j + 1]) nzp[cnt++] = (uint16_t)j;
    float z2[NN_L2];
    for (int k = 0; k < NN_L2; k++) {
        const int16_t* w = nnW2q[ob][k]; float fa = 0.0f, fb = 0.0f;       // float sums in the order of the AVX2 code (two streams): a 32-bit integer sum can overflow for a strongly negative neuron
        for (int c = 0; c < cnt; c++) { const int j = nzp[c]; const int t = (int)x[2 * j] * w[2 * j] + (int)x[2 * j + 1] * w[2 * j + 1]; if (c & 1) fb += (float)t; else fa += (float)t; }
#ifdef OVFCHECK2
        { int64_t a64 = 0; for (int i = 0; i < NN_H; i++) a64 += (int64_t)x[i] * w[i]; const double ex = (double)a64, got = (double)(fa + fb); ovfN++; if (fabs(ex - got) > 1e-6 * fabs(ex) + 64.0) ovfBad++; if (a64 > INT32_MAX || a64 < INT32_MIN) ovfBeyond++; }
#endif
        z2[k] = nnHb2[ob * NN_L2 + k] + (fa + fb) * sc;
    }
#endif
#if !(defined(__AVX2__) && defined(__FMA__))
    float u[2 * NN_L2];
    for (int k = 0; k < NN_L2; k++) { const float a = z2[k] < 0.0f ? 0.0f : (z2[k] > 1.0f ? 1.0f : z2[k]); u[k] = a; u[NN_L2 + k] = a * a; }
    const float* w3 = nnHW3 + (size_t)ob * 2 * NN_L2 * NN_L3;
    float z3[NN_L3];
    for (int l = 0; l < NN_L3; l++) z3[l] = nnHb3[ob * NN_L3 + l];
    for (int j = 0; j < 2 * NN_L2; j++) { const float uj = u[j]; for (int l = 0; l < NN_L3; l++) z3[l] += uj * w3[j * NN_L3 + l]; }
    float out = nnHb4[ob];
    for (int l = 0; l < NN_L3; l++) { const float a = z3[l] < 0.0f ? 0.0f : (z3[l] > 1.0f ? 1.0f : z3[l]); out += a * nnHW4[ob * NN_L3 + l]; }
    float cp = out * nnCpPerUnit * nnScale;
    if (cp > 3000.0f) cp = 3000.0f; else if (cp < -3000.0f) cp = -3000.0f;
    return (int)cp;
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
    alignas(32) int16_t accRing[256][2][NN_H];       // accumulators of the last 256 plies: make() fills the next slot, unmake() steps back
    int16_t (*acc)[NN_H] = accRing[0]; int ai = 0;      // the current slot
    int32_t psq[2];                                    // (unused since 2.0, kept so that the saved state keeps its shape)
    int32_t psq2[2];
    Pos() = default;
    Pos(const Pos& o) { copyFrom(o); }
    Pos& operator=(const Pos& o) { copyFrom(o); return *this; }
    inline void setSlot(int i) { ai = i; acc = accRing[i]; }
    inline void copySlot(int from, int to) { std::memcpy(accRing[to], accRing[from], sizeof accRing[0]); }
    int kOff[2] = {0, 0}, kMir[2] = {0, 0};    // king bucket offset (bucket * 768) and horizontal mirroring of each perspective
    int pstMg = 0, pstEg = 0, phaseAcc = 0;
    int B[64];
    int cRook[4] = {7, 0, 63, 56}, cKing[2] = {4, 60}, cmask[64];      // castling: rook squares (K, Q, k, q), king squares, rights lost when a square is touched
    int side, castle, ep, half, ply, rootPly = 0;      // rootPly: ply of the root position of the current search (set by think())
    U64 hash;
    Undo U[4096];
    U64 HH[4096];
    struct AccSave { int32_t psq[2], psq2[2]; int pstMg, pstEg, phaseAcc, kOff[2], kMir[2]; };
    struct FinnyEnt { unsigned gen = 0; U64 bb[12]; alignas(32) int16_t acc[NN_H]; };      // king-refresh cache: the accumulators of one side for one king square, with the pieces they were built from
    FinnyEnt fin[2][64];
    struct LzRec { int8_t pcA, pcF, pcC, pcR, sqA, sqF, sqC, rs, rt, ref; };      // what make() changed in a slot; ref: bit p = side p must be rebuilt from the board (its king changed bucket, or no net)
    LzRec lz[256];
    uint8_t ok[256];                                                              // bit p: the accumulator of side p is built in this slot
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
        if (NN && nnueReady) { const int16_t* w0 = nnW1 + (size_t)(kOff[0] + nnBase(0, pc, sq, kMir[0])) * NN_H; const int16_t* w1 = nnW1 + (size_t)(kOff[1] + nnBase(1, pc, sq, kMir[1])) * NN_H;
            for (int j = 0; j < NN_H; j++) { acc[0][j] = (int16_t)(acc[0][j] + w0[j]); acc[1][j] = (int16_t)(acc[1][j] + w1[j]); } }
    }
    template<bool NN = true> inline void rem(int pc, int sq) {
        U64 b = 1ULL << sq; P[pc] ^= b; Occ[pc >= 6] ^= b; All ^= b; B[sq] = EMPTYP; hash ^= ZP[pc][sq];
        { int t = pc % 6, ix = t * 64 + (pc < 6 ? sq : (sq ^ 56)), sg = pc < 6 ? 1 : -1;
          pstMg -= sg * Wmg[ix]; pstEg -= sg * Weg[ix]; phaseAcc -= (t == 1 || t == 2) ? 1 : (t == 3 ? 2 : (t == 4 ? 4 : 0)); }
        if (NN && nnueReady) { const int16_t* w0 = nnW1 + (size_t)(kOff[0] + nnBase(0, pc, sq, kMir[0])) * NN_H; const int16_t* w1 = nnW1 + (size_t)(kOff[1] + nnBase(1, pc, sq, kMir[1])) * NN_H;
            for (int j = 0; j < NN_H; j++) { acc[0][j] = (int16_t)(acc[0][j] - w0[j]); acc[1][j] = (int16_t)(acc[1][j] - w1[j]); } }
    }
    inline void addBB(int pc, int sq) { U64 b = 1ULL << sq; P[pc] |= b; Occ[pc >= 6] |= b; All |= b; B[sq] = pc; }
    inline void remBB(int pc, int sq) { U64 b = 1ULL << sq; P[pc] ^= b; Occ[pc >= 6] ^= b; All ^= b; B[sq] = EMPTYP; }
    // one fused pass over the accumulators for an ordinary move (not a king move, not castling): + the piece on its new square, - the piece on its old square,
    // - a captured piece.  The integer sums are exactly those of the separate add / rem calls.
    inline const int16_t* nnRow(int p, int pc, int sq) const { return nnW1 + (size_t)(kOff[p] + nnBase(p, pc, sq, kMir[p])) * NN_H; }
    static inline void accAddSub(int16_t* __restrict dst, const int16_t* __restrict src, const int16_t* wa, const int16_t* wf, const int16_t* wc) {      // dst = src + wa - wf (- wc)
        if (wc) { for (int j = 0; j < NN_H; j++) dst[j] = (int16_t)(src[j] + wa[j] - wf[j] - wc[j]); }
        else { for (int j = 0; j < NN_H; j++) dst[j] = (int16_t)(src[j] + wa[j] - wf[j]); }
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
        psq[p] = 0; psq2[p] = 0;
        for (int sq = 0; sq < 64; sq++) if (B[sq] != EMPTYP) {
            const int16_t* w = nnW1 + (size_t)(kOff[p] + nnBase(p, B[sq], sq, kMir[p])) * NN_H;
            for (int j = 0; j < NN_H; j++) acc[p][j] = (int16_t)(acc[p][j] + w[j]);
        }
    }
    void refresh() {
        for (int c = 0; c < 2; c++) { if (P[c * 6 + 5]) orient(c, lsb(P[c * 6 + 5])); else { kOff[c] = 0; kMir[c] = 0; } }
        if (!nnueReady) return;
        refreshPersp(0); refreshPersp(1); ok[ai] = 3;
    }
    template<int NA, int NR> static inline void rowsPass(int16_t* __restrict a, const int16_t* const* ad, const int16_t* const* rm) {      // a += up to two rows, -= up to two rows, in one pass
        for (int j = 0; j < NN_H; j++) {
            int v = a[j];
            if constexpr (NA > 0) v += ad[0][j];
            if constexpr (NA > 1) v += ad[1][j];
            if constexpr (NR > 0) v -= rm[0][j];
            if constexpr (NR > 1) v -= rm[1][j];
            a[j] = (int16_t)v;
        }
    }
    void finnyRefresh(int p, int16_t* dst) {         // dst = the accumulators of side p for the current board, from the cache entry of its king square: the same sums as refreshPersp
        FinnyEnt& e = fin[p][lsb(P[p * 6 + 5])];
        if (e.gen != nnGen) { memcpy(e.acc, nnB1, sizeof e.acc); for (int i = 0; i < 12; i++) e.bb[i] = 0; e.gen = nnGen; }
        const int16_t *ad[64], *rm[64]; int na = 0, nr = 0;      // the rows to add / to remove; their first cache lines are requested while the list is built
        for (int pc = 0; pc < 12; pc++) {
            U64 x = P[pc] & ~e.bb[pc], y = e.bb[pc] & ~P[pc];
            while (x) { ad[na] = nnRow(p, pc, lsb(x)); __builtin_prefetch(ad[na]); __builtin_prefetch((const char*)ad[na] + 64); na++; x &= x - 1; }
            while (y) { rm[nr] = nnRow(p, pc, lsb(y)); __builtin_prefetch(rm[nr]); __builtin_prefetch((const char*)rm[nr] + 64); nr++; y &= y - 1; }
            e.bb[pc] = P[pc];
        }
        int i = 0, k = 0;
        while (i < na || k < nr) {
            const int xa = std::min(2, na - i), xr = std::min(2, nr - k);
            switch (xa * 3 + xr) {
                case 1: rowsPass<0, 1>(e.acc, ad + i, rm + k); break;
                case 2: rowsPass<0, 2>(e.acc, ad + i, rm + k); break;
                case 3: rowsPass<1, 0>(e.acc, ad + i, rm + k); break;
                case 4: rowsPass<1, 1>(e.acc, ad + i, rm + k); break;
                case 5: rowsPass<1, 2>(e.acc, ad + i, rm + k); break;
                case 6: rowsPass<2, 0>(e.acc, ad + i, rm + k); break;
                case 7: rowsPass<2, 1>(e.acc, ad + i, rm + k); break;
                default: rowsPass<2, 2>(e.acc, ad + i, rm + k); break;
            }
            i += xa; k += xr;
        }
        memcpy(dst, e.acc, sizeof e.acc);
    }
    inline void prefetchRows(int m) const {        // start loading the feature rows an ordinary move will add and remove (called when the move is selected, before it is searched)
        if (!nnueReady) return;
        const int from = m & 63, to = (m >> 6) & 63, promo = (m >> 12) & 7, pc = B[from];
        if (pc == EMPTYP || pc % 6 == 5 || (m & CAS)) return;
        const int pcTo = promo ? side * 6 + promo : pc;
        const int capSq = (m & EPF) ? to + (side == 0 ? -8 : 8) : to;
        const int cap = (m & (CAP | EPF)) ? B[capSq] : (int)EMPTYP;
        for (int p = 0; p < 2; p++) {
            const char* r[3] = {(const char*)nnRow(p, pcTo, to), (const char*)nnRow(p, pc, from), cap != EMPTYP ? (const char*)nnRow(p, cap, capSq) : nullptr};
            for (const char* q : r) if (q) { __builtin_prefetch(q); __builtin_prefetch(q + 64); __builtin_prefetch(q + 128); __builtin_prefetch(q + 1536); __builtin_prefetch(q + 3008); }
        }
    }
    void ensureAcc(int p) {         // build the accumulator of side p in the current slot
        const unsigned bit = 1u << p;
        if (ok[ai] & bit) return;
        int k = ai, n = 0;
        for (;;) {
            if (ok[k] & bit) break;
            if ((lz[k].ref & bit) || n >= 255) { if (P[p * 6 + 5]) finnyRefresh(p, accRing[ai][p]); else refreshPersp(p); ok[ai] |= (uint8_t)bit; return; }
            k = (k + 255) & 255; n++;
        }
        for (int j = k, c = 0; c < n; c++) {      // n recorded changes lead from the built slot k to the current one
            const int prev = j; j = (j + 1) & 255; const LzRec& r = lz[j];
            accAddSub(accRing[j][p], accRing[prev][p], nnRow(p, r.pcA, r.sqA), nnRow(p, r.pcF, r.sqF), r.pcC != EMPTYP ? nnRow(p, r.pcC, r.sqC) : nullptr);
            if (r.pcR != EMPTYP) { int16_t* a = accRing[j][p]; const int16_t* wa = nnRow(p, r.pcR, r.rt); const int16_t* wf = nnRow(p, r.pcR, r.rs); for (int x = 0; x < NN_H; x++) a[x] = (int16_t)(a[x] + wa[x] - wf[x]); }
            ok[j] |= (uint8_t)bit;
        }
    }
    inline void ensureBoth() {
        if ((ok[ai] & 3) == 3) return;
        const LzRec& r = lz[ai];      // request the rows of both sides first, so that the memory latency of the second side overlaps the work on the first
        for (int p = 0; p < 2; p++) { __builtin_prefetch(nnRow(p, r.pcA, r.sqA)); __builtin_prefetch(nnRow(p, r.pcF, r.sqF)); if (r.pcC != EMPTYP) __builtin_prefetch(nnRow(p, r.pcC, r.sqC)); }
        ensureAcc(0); ensureAcc(1);
    }
    void copyFrom(const Pos& o) {      // the position with its history; of the big arrays only the used parts (accumulator slot ai, undo / hash history up to ply), the refresh caches stay as they are
        if (this == &o) return;
        if (nnueReady) const_cast<Pos&>(o).ensureBoth();
        std::memcpy((void*)this, (const void*)&o, offsetof(Pos, accRing));
        std::memcpy((char*)(void*)this + offsetof(Pos, acc), (const char*)(const void*)&o + offsetof(Pos, acc), offsetof(Pos, U) - offsetof(Pos, acc));
        acc = accRing[ai];
        std::memcpy((void*)accRing[ai], (const void*)o.accRing[o.ai], sizeof accRing[0]);
        const size_t nh = (size_t)o.ply + 1;
        std::memcpy((void*)U, (const void*)o.U, nh * sizeof(Undo)); std::memcpy((void*)HH, (const void*)o.HH, nh * sizeof(U64));
        std::memcpy((void*)accSave, (const void*)o.accSave, sizeof accSave);
        std::memset(ok, 0, sizeof ok); ok[ai] = nnueReady ? 3 : 0;
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
        const int nai = (ai + 1) & 255;
        { AccSave& sv = accSave[ply & 255];
          sv.psq[0] = psq[0]; sv.psq[1] = psq[1]; sv.psq2[0] = psq2[0]; sv.psq2[1] = psq2[1];
          sv.pstMg = pstMg; sv.pstEg = pstEg; sv.phaseAcc = phaseAcc; sv.kOff[0] = kOff[0]; sv.kOff[1] = kOff[1]; sv.kMir[0] = kMir[0]; sv.kMir[1] = kMir[1]; }
        int from = m & 63, to = (m >> 6) & 63, promo = (m >> 12) & 7;
        int pc = B[from], us = side;
        int kk0 = kKey(us);
        if (ep >= 0) hash ^= ZE[ep & 7];
        int cap = EMPTYP, capSq = -1;
        if (m & EPF) { capSq = to + (us == 0 ? -8 : 8); cap = B[capSq]; rem<false>(cap, capSq); }
        else if (m & CAP) { capSq = to; cap = B[to]; rem<false>(cap, to); }
        u.cap = cap;
        const int pcTo = promo ? us * 6 + promo : pc;
        LzRec& lr = lz[nai];      // the accumulators are built later, when (and if) the position is evaluated: only the change is recorded
        lr.pcA = (int8_t)pcTo; lr.sqA = (int8_t)to; lr.pcF = (int8_t)pc; lr.sqF = (int8_t)from; lr.pcC = (int8_t)cap; lr.sqC = (int8_t)capSq; lr.pcR = (int8_t)EMPTYP; lr.ref = nnueReady ? 0 : 3; ok[nai] = 0;
        if (m & CAS) { int rs, rt; castleRook(m, us, rs, rt); rem<false>(pc, from); rem<false>(us * 6 + 3, rs); add<false>(pc, to); add<false>(us * 6 + 3, rt);      // both pieces off, then both on (overlaps)
            lr.pcR = (int8_t)(us * 6 + 3); lr.rs = (int8_t)rs; lr.rt = (int8_t)rt; }
        else { rem<false>(pc, from); add<false>(pcTo, to); }
        if ((pc % 6) == 5 && kKey(us) != kk0) lr.ref |= (int8_t)(1 << us);
        setSlot(nai);
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

// ---------------------------------------------------------------- Syzygy tablebase probing
// The code of this block is Fathom (https://github.com/jdart1/Fathom), (c) 2013-2018 Ronald de Man, (c) 2015 basil00, (c) 2016-2025 Jon Dart, MIT licence:
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// Only the probing library is Fathom's; it is used through the namespace fathom (tb_init, tb_probe_wdl, tb_probe_root_dtz, ...).
#include <assert.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif
#if defined(__GNUC__) && defined(__x86_64__) && defined(__SSE4_2__)
#include <popcntintrin.h>
#endif
#define TB_NO_HELPER_API
#ifndef _STDENDIAN_H_
#define _STDENDIAN_H_
/* from https://gist.github.com/michaeljclark/3b4fd912f6fa8bb598b3 */
/* modified to use functions not macros for bswap */
/* and added a fix for Cygwin */
/*
 *   stdendian.h
 *
 *   This header defines the following endian macros as defined here:
 *   http://austingroupbugs.net/view.php?id=162
 *
 *     BYTE_ORDER         this macro shall have a value equal to one
 *                        of the *_ENDIAN macros in this header.
 *     LITTLE_ENDIAN      if BYTE_ORDER == LITTLE_ENDIAN, the host
 *                        byte order is from least significant to
 *                        most significant.
 *     BIG_ENDIAN         if BYTE_ORDER == BIG_ENDIAN, the host byte
 *                        order is from most significant to least
 *                        significant.
 *
 *   The following are defined as macros:
 *
 *     uint16_t bswap16(uint16_t x);
 *     uint32_t bswap32(uint32_t x);
 *     uint64_t bswap64(uint64_t x);

 *     uint16_t htobe16(uint16_t x);
 *     uint16_t htole16(uint16_t x);
 *     uint16_t be16toh(uint16_t x);
 *     uint16_t le16toh(uint16_t x);
 *
 *     uint32_t htobe32(uint32_t x);
 *     uint32_t htole32(uint32_t x);
 *     uint32_t be32toh(uint32_t x);
 *     uint32_t le32toh(uint32_t x);
 *
 *     uint64_t htobe64(uint64_t x);
 *     uint64_t htole64(uint64_t x);
 *     uint64_t be64toh(uint64_t x);
 *     uint64_t le64toh(uint64_t x);
 *
 *   The header defines the following macro for OpenCL compatibility
 *   https://www.khronos.org/registry/cl/sdk/2.0/docs/man/xhtml/preprocessorDirectives.html
 *
 *     __ENDIAN_LITTLE__  if BYTE_ORDER == LITTLE_ENDIAN then this
 *                        macro is present for OpenCL compatibility
 *
 *   The implementation provides a uniform interface to endian macros using only
 *   system headers on recent Linux, Darwin, FreeBSD, Solaris and Windows systems. 
 *
 *   This approach is intended to avoid the need for preflight configure scripts.
 *   An alternative approach would be to test compiler CPU architecture marcros.
 *
 *   This header has had *limited* testing on recent C11/C++11 compilers and is
 *   based on the austin bug tracker interface, manpages, and headers present in
 *   Linux, FreeBSD, Windows, Solaris and Darwin.
 *
 *   The header uses __builtin_bswapXX intrinsic with GCC/Clang (__GNUC__) on
 *   platforms that do not provide bswap16, bswap32, bswap64 (Darwin)
 *
 *   Public Domain.
 */

/* requires C11 or C++11 */
#if defined (__cplusplus)
#include <cstdint>
#elif !defined (__OPENCL_VERSION__)
#include <stdint.h>
#endif

/* Linux / GLIBC */
#if defined(__linux__) || defined(__GLIBC__) || defined(__CYGWIN__)
#include <endian.h>
#include <byteswap.h>
#define __ENDIAN_DEFINED        1
#define __BSWAP_DEFINED         1
#define __HOSTSWAP_DEFINED      1
// NDK defines _BYTE_ORDER etc
#ifndef _BYTE_ORDER
#define _BYTE_ORDER             __BYTE_ORDER
#define _LITTLE_ENDIAN          __LITTLE_ENDIAN
#define _BIG_ENDIAN             __BIG_ENDIAN
#endif
#define bswap16(x)              bswap_16(x)
#define bswap32(x)              bswap_32(x)
#define bswap64(x)              bswap_64(x)
#endif /* __linux__ || __GLIBC__ */

/* BSD */
#if defined(__FreeBSD__) || defined(__NetBSD__) || \
  defined(__DragonFly__) || defined(__OpenBSD__)
#include <sys/endian.h>
#define __ENDIAN_DEFINED        1
#define __BSWAP_DEFINED         1
#define __HOSTSWAP_DEFINED      1
#endif /* BSD */

/* Solaris */
#if defined (sun)
#include <sys/isa_defs.h>
/* sun headers don't set a value for _LITTLE_ENDIAN or _BIG_ENDIAN */
#if defined(_LITTLE_ENDIAN)
#undef _LITTLE_ENDIAN
#define _LITTLE_ENDIAN          1234
#define _BIG_ENDIAN             4321
#define _BYTE_ORDER             _LITTLE_ENDIAN
#elif defined(_BIG_ENDIAN)
#undef _BIG_ENDIAN
#define _LITTLE_ENDIAN          1234
#define _BIG_ENDIAN             4321
#define _BYTE_ORDER             _BIG_ENDIAN
#endif
#define __ENDIAN_DEFINED        1
#endif /* sun */

/* Windows */
/* assumes all Microsoft targets are little endian. */
/* Emscripten (emcc) also currently assumes little endian. */
#if defined(_WIN32) || defined(_MSC_VER) || defined(__EMSCRIPTEN__)
#define _LITTLE_ENDIAN          1234
#define _BIG_ENDIAN             4321
#define _BYTE_ORDER             _LITTLE_ENDIAN
#define __ENDIAN_DEFINED        1
#endif /* _MSC_VER */

/* OS X */
#if defined(__APPLE__)
#include <machine/endian.h>
#define _BYTE_ORDER             BYTE_ORDER
#define _LITTLE_ENDIAN          LITTLE_ENDIAN
#define _BIG_ENDIAN             BIG_ENDIAN
#define __ENDIAN_DEFINED        1
#endif /* __APPLE__ */

/* OpenCL */
#if defined (__OPENCL_VERSION__)
#define _LITTLE_ENDIAN          1234
#define __BIG_ENDIAN            4321
#if defined (__ENDIAN_LITTLE__)
#define _BYTE_ORDER             _LITTLE_ENDIAN
#else
#define _BYTE_ORDER             _BIG_ENDIAN
#endif
#define bswap16(x)              as_ushort(as_uchar2(ushort(x)).s1s0)
#define bswap32(x)              as_uint(as_uchar4(uint(x)).s3s2s1s0)
#define bswap64(x)              as_ulong(as_uchar8(ulong(x)).s7s6s5s4s3s2s1s0)
#define __ENDIAN_DEFINED        1
#define __BSWAP_DEFINED         1
#endif

/* Unknown */
#if !__ENDIAN_DEFINED
#error Could not determine CPU byte order
#endif

/* POSIX - http://austingroupbugs.net/view.php?id=162 */
#ifndef BYTE_ORDER
#define BYTE_ORDER _BYTE_ORDER
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN _LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN _BIG_ENDIAN
#endif

/* OpenCL compatibility - define __ENDIAN_LITTLE__ on little endian systems */
#if _BYTE_ORDER == _LITTLE_ENDIAN
#if !defined (__ENDIAN_LITTLE__)
#define __ENDIAN_LITTLE__   1
#endif
#endif

/* Byte swap macros */
#if !__BSWAP_DEFINED

#ifndef bswap16
/* handle missing __builtin_bswap16 https://gcc.gnu.org/bugzilla/show_bug.cgi?id=52624 */
#if defined __GNUC__
#define bswap16(x) __builtin_bswap16(x)
#else
inline uint16_t bswap16(uint16_t x) {
  return (uint16_t)((((uint16_t) (x) & 0xff00) >> 8) | \
                    (((uint16_t) (x) & 0x00ff) << 8));
}
#endif
#endif

#ifndef bswap32
#if defined __GNUC__
#define bswap32(x) __builtin_bswap32(x)
#else
inline uint32_t bswap32(uint32_t x) {
  return (( x & 0xff000000) >> 24) | \
  (( x & 0x00ff0000) >> 8) | \
  (( x & 0x0000ff00) << 8) | \
  (( x & 0x000000ff) << 24);
}
#endif
#endif

#ifndef bswap64
#if defined __GNUC__
#define bswap64(x) __builtin_bswap64(x)
#else
inline uint64_t bswap64(uint64_t x) {
    return (( x & 0xff00000000000000ull) >> 56) | \
  (( x & 0x00ff000000000000ull) >> 40) | \
  (( x & 0x0000ff0000000000ull) >> 24) | \
  (( x & 0x000000ff00000000ull) >> 8) | \
  (( x & 0x00000000ff000000ull) << 8) | \
  (( x & 0x0000000000ff0000ull) << 24) | \
  (( x & 0x000000000000ff00ull) << 40) | \
  (( x & 0x00000000000000ffull) << 56);
}
#endif
#endif

#endif

/* Host swap macros */
#ifndef __HOSTSWAP_DEFINED
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htobe16(x)              bswap16((x))
#define htole16(x)              ((uint16_t)(x))
#define be16toh(x)              bswap16((x))
#define le16toh(x)              ((uint16_t)(x))

#define htobe32(x)              bswap32((x))
#define htole32(x)              ((uint32_t)(x))
#define be32toh(x)              bswap32((x))
#define le32toh(x)              ((uint32_t)(x))

#define htobe64(x)              bswap64((x))
#define htole64(x)              ((uint64_t)(x))
#define be64toh(x)              bswap64((x))
#define le64toh(x)              ((uint64_t)(x))
#elif __BYTE_ORDER == __BIG_ENDIAN
#define htobe16(x)              ((uint16_t)(x))
#define htole16(x)              bswap16((x))
#define be16toh(x)              ((uint16_t)(x))
#define le16toh(x)              bswap16((x))

#define htobe32(x)              ((uint32_t)(x))
#define htole32(x)              bswap32((x))
#define be32toh(x)              ((uint32_t)(x))
#define le64toh(x)              bswap64((x))

#define htobe64(x)              ((uint64_t)(x))
#define htole64(x)              bswap64((x))
#define be64toh(x)              ((uint64_t)(x))
#define le32toh(x)              bswap32((x))
#endif
#endif

/*

#include <stdio.h>
#include <stdendian.h>

int main()
{

#if BYTE_ORDER == LITTLE_ENDIAN
printf("little endian\n");
#endif

#if BYTE_ORDER == BIG_ENDIAN
printf("big endian\n");
#endif

printf("bswap16(%04x) %04x\n", 0xf0e0, bswap16(0xf0e0));
printf("htobe16(%04x) %04x\n", 0xf0e0, htobe16(0xf0e0));
printf("htole16(%04x) %04x\n", 0xf0e0, htole16(0xf0e0));

printf("bswap32(%08x) %08x\n", 0xf0e0d0c0, bswap32(0xf0e0d0c0));
printf("htobe32(%08x) %08x\n", 0xf0e0d0c0, htobe32(0xf0e0d0c0));
printf("htole32(%08x) %08x\n", 0xf0e0d0c0, htole32(0xf0e0d0c0));

printf("bswap64(%016llx) %016llx\n", 0xf0e0d0c0b0a09080ULL, bswap64(0xf0e0d0c0b0a09080ULL));
printf("htobe64(%016llx) %016llx\n", 0xf0e0d0c0b0a09080ULL, htobe64(0xf0e0d0c0b0a09080ULL));
printf("htole64(%016llx) %016llx\n", 0xf0e0d0c0b0a09080ULL, htole64(0xf0e0d0c0b0a09080ULL));
}

*/
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
namespace fathom {
/*
 * tbconfig.h
 * (C) 2015 basil, all rights reserved,
 * Modifications Copyright 2016-2017 Jon Dart
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef TBCONFIG_H
#define TBCONFIG_H

/****************************************************************************/
/* BUILD CONFIG:                                                            */
/****************************************************************************/

/*
 * Define TB_CUSTOM_POP_COUNT to override the internal popcount
 * implementation. To do this supply a macro or function definition
 * here:
 */
/* #define TB_CUSTOM_POP_COUNT(x) <DEFINITION> */

/*
 * Define TB_CUSTOM_LSB to override the internal lsb
 * implementation. To do this supply a macro or function definition
 * here:
 */
/* #define TB_CUSTOM_LSB(x) <DEFINITION> */

/*
 * Define TB_NO_STDINT if you do not want to use <stdint.h> or it is not
 * available.
 */
/* #define TB_NO_STDINT */

/*
 * Define TB_NO_STDBOOL if you do not want to use <stdbool.h> or it is not
 * available or unnecessary (e.g. C++).
 */
/* #define TB_NO_STDBOOL */

/*
 * Define TB_NO_THREADS if your program is not multi-threaded.
 */
/* #define TB_NO_THREADS */

/*
 * Define TB_NO_HELPER_API if you do not need the helper API.
 */
/* #define TB_NO_HELPER_API */

/*
 * Define TB_NO_HW_POP_COUNT if there is no hardware popcount instruction.
 *
 * Note: if defined, TB_CUSTOM_POP_COUNT is always used in preference
 * to any built-in popcount functions.
 *
 * If no custom popcount function is defined, and if the following
 * define is not set, the code will attempt to use an available hardware
 * popcnt (currently supported on x86_64 architecture only) and otherwise
 * will fall back to a software implementation.
 */
/* #define TB_NO_HW_POP_COUNT */

/***************************************************************************/
/* SCORING CONSTANTS                                                       */
/***************************************************************************/
/*
 * Fathom can produce scores for tablebase moves. These depend on the
 * value of a pawn, and the magnitude of mate scores. The following
 * constants are representative values but will likely need
 * modification to adapt to an engine's own internal score values.
 */
#define TB_VALUE_PAWN 100  /* value of pawn in endgame */
#define TB_VALUE_MATE 32000
#define TB_VALUE_INFINITE 32767 /* value above all normal score values */
#define TB_VALUE_DRAW 0
#define TB_MAX_MATE_PLY 255

/***************************************************************************/
/* ENGINE INTEGRATION CONFIG                                               */
/***************************************************************************/

/*
 * If you are integrating tbprobe into an engine, you can replace some of
 * tbprobe's built-in functionality with that already provided by the engine.
 * This is OPTIONAL.  If no definition are provided then tbprobe will use its
 * own internal defaults.  That said, for engines it is generally a good idea
 * to avoid redundancy.
 */

/*
 * Define TB_KING_ATTACKS(square) to return the king attacks bitboard for a
 * king at `square'.
 */
/* #define TB_KING_ATTACKS(square)          <DEFINITION> */

/*
 * Define TB_KNIGHT_ATTACKS(square) to return the knight attacks bitboard for
 * a knight at `square'.
 */
/* #define TB_KNIGHT_ATTACKS(square)        <DEFINITION> */

/*
 * Define TB_ROOK_ATTACKS(square, occ) to return the rook attacks bitboard
 * for a rook at `square' assuming the given `occ' occupancy bitboard.
 */
/* #define TB_ROOK_ATTACKS(square, occ)     <DEFINITION> */

/*
 * Define TB_BISHOP_ATTACKS(square, occ) to return the bishop attacks bitboard
 * for a bishop at `square' assuming the given `occ' occupancy bitboard.
 */
/* #define TB_BISHOP_ATTACKS(square, occ)   <DEFINITION> */

/*
 * Define TB_QUEEN_ATTACKS(square, occ) to return the queen attacks bitboard
 * for a queen at `square' assuming the given `occ' occupancy bitboard.
 * NOTE: If no definition is provided then tbprobe will use:
 *       TB_ROOK_ATTACKS(square, occ) | TB_BISHOP_ATTACKS(square, occ)
 */
/* #define TB_QUEEN_ATTACKS(square, occ)    <DEFINITION> */

/*
 * Define TB_PAWN_ATTACKS(square, color) to return the pawn attacks bitboard
 * for a `color' pawn at `square'.
 * NOTE: This definition must work for pawns on ranks 1 and 8.  For example,
 *       a white pawn on e1 attacks d2 and f2.  A black pawn on e1 attacks
 *       nothing.  Etc.
 * NOTE: This definition must not include en passant captures.
 */
/* #define TB_PAWN_ATTACKS(square, color)   <DEFINITION> */

#endif
/*
 * tbprobe.h
 * (C) 2015 basil, all rights reserved,
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef TBPROBE_H
#define TBPROBE_H



#ifndef TB_NO_STDINT
#else
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned uint32_t;
typedef long long unsigned uint64_t;
typedef char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
#endif

#ifndef TB_NO_STDBOOL
#else
#ifndef __cplusplus
typedef uint8_t bool;
#define true    1
#define false   0
#endif
#endif

/*
 * Internal definitions.  Do not call these functions directly.
 */
extern bool tb_init_impl(const char *_path);
extern unsigned tb_probe_wdl_impl(
    uint64_t _white,
    uint64_t _black,
    uint64_t _kings,
    uint64_t _queens,
    uint64_t _rooks,
    uint64_t _bishops,
    uint64_t _knights,
    uint64_t _pawns,
    unsigned _ep,
    bool     _turn);
extern unsigned tb_probe_root_impl(
    uint64_t _white,
    uint64_t _black,
    uint64_t _kings,
    uint64_t _queens,
    uint64_t _rooks,
    uint64_t _bishops,
    uint64_t _knights,
    uint64_t _pawns,
    unsigned _rule50,
    unsigned _ep,
    bool     _turn,
    unsigned *_results);

/****************************************************************************/
/* MAIN API                                                                 */
/****************************************************************************/

#define TB_MAX_MOVES                (192+1)
#define TB_MAX_CAPTURES             64
#define TB_MAX_PLY                  256
#define TB_CASTLING_K               0x1     /* White king-side. */
#define TB_CASTLING_Q               0x2     /* White queen-side. */
#define TB_CASTLING_k               0x4     /* Black king-side. */
#define TB_CASTLING_q               0x8     /* Black queen-side. */

#define TB_LOSS                     0       /* LOSS */
#define TB_BLESSED_LOSS             1       /* LOSS but 50-move draw */
#define TB_DRAW                     2       /* DRAW */
#define TB_CURSED_WIN               3       /* WIN but 50-move draw  */
#define TB_WIN                      4       /* WIN  */

#define TB_PROMOTES_NONE            0
#define TB_PROMOTES_QUEEN           1
#define TB_PROMOTES_ROOK            2
#define TB_PROMOTES_BISHOP          3
#define TB_PROMOTES_KNIGHT          4

#define TB_RESULT_WDL_MASK          0x0000000F
#define TB_RESULT_TO_MASK           0x000003F0
#define TB_RESULT_FROM_MASK         0x0000FC00
#define TB_RESULT_PROMOTES_MASK     0x00070000
#define TB_RESULT_EP_MASK           0x00080000
#define TB_RESULT_DTZ_MASK          0xFFF00000
#define TB_RESULT_WDL_SHIFT         0
#define TB_RESULT_TO_SHIFT          4
#define TB_RESULT_FROM_SHIFT        10
#define TB_RESULT_PROMOTES_SHIFT    16
#define TB_RESULT_EP_SHIFT          19
#define TB_RESULT_DTZ_SHIFT         20

#define TB_GET_WDL(_res)                        \
    (((_res) & TB_RESULT_WDL_MASK) >> TB_RESULT_WDL_SHIFT)
#define TB_GET_TO(_res)                         \
    (((_res) & TB_RESULT_TO_MASK) >> TB_RESULT_TO_SHIFT)
#define TB_GET_FROM(_res)                       \
    (((_res) & TB_RESULT_FROM_MASK) >> TB_RESULT_FROM_SHIFT)
#define TB_GET_PROMOTES(_res)                   \
    (((_res) & TB_RESULT_PROMOTES_MASK) >> TB_RESULT_PROMOTES_SHIFT)
#define TB_GET_EP(_res)                         \
    (((_res) & TB_RESULT_EP_MASK) >> TB_RESULT_EP_SHIFT)
#define TB_GET_DTZ(_res)                        \
    (((_res) & TB_RESULT_DTZ_MASK) >> TB_RESULT_DTZ_SHIFT)

#define TB_SET_WDL(_res, _wdl)                  \
    (((_res) & ~TB_RESULT_WDL_MASK) |           \
     (((_wdl) << TB_RESULT_WDL_SHIFT) & TB_RESULT_WDL_MASK))
#define TB_SET_TO(_res, _to)                    \
    (((_res) & ~TB_RESULT_TO_MASK) |            \
     (((_to) << TB_RESULT_TO_SHIFT) & TB_RESULT_TO_MASK))
#define TB_SET_FROM(_res, _from)                \
    (((_res) & ~TB_RESULT_FROM_MASK) |          \
     (((_from) << TB_RESULT_FROM_SHIFT) & TB_RESULT_FROM_MASK))
#define TB_SET_PROMOTES(_res, _promotes)        \
    (((_res) & ~TB_RESULT_PROMOTES_MASK) |      \
     (((_promotes) << TB_RESULT_PROMOTES_SHIFT) & TB_RESULT_PROMOTES_MASK))
#define TB_SET_EP(_res, _ep)                    \
    (((_res) & ~TB_RESULT_EP_MASK) |            \
     (((_ep) << TB_RESULT_EP_SHIFT) & TB_RESULT_EP_MASK))
#define TB_SET_DTZ(_res, _dtz)                  \
    (((_res) & ~TB_RESULT_DTZ_MASK) |           \
     (((_dtz) << TB_RESULT_DTZ_SHIFT) & TB_RESULT_DTZ_MASK))

#define TB_RESULT_CHECKMATE         TB_SET_WDL(0, TB_WIN)
#define TB_RESULT_STALEMATE         TB_SET_WDL(0, TB_DRAW)
#define TB_RESULT_FAILED            0xFFFFFFFF

/*
 * The tablebase can be probed for any position where #pieces <= TB_LARGEST.
 */
extern unsigned TB_LARGEST;

/*
 * Initialize the tablebase.
 *
 * PARAMETERS:
 * - path:
 *   The tablebase PATH string.
 *
 * RETURN:
 * - true=success, false=failed.  The TB_LARGEST global will also be
 *   initialized.  If no tablebase files are found, then `true' is returned
 *   and TB_LARGEST is set to zero.
 */
bool tb_init(const char *_path);

/*
 * Free any resources allocated by tb_init
 */
void tb_free(void);

/*
 * Probe the Win-Draw-Loss (WDL) table.
 *
 * PARAMETERS:
 * - white, black, kings, queens, rooks, bishops, knights, pawns:
 *   The current position (bitboards).
 * - rule50:
 *   The 50-move half-move clock.
 * - castling:
 *   Castling rights.  Set to zero if no castling is possible.
 * - ep:
 *   The en passant square (if exists).  Set to zero if there is no en passant
 *   square.
 * - turn:
 *   true=white, false=black
 *
 * RETURN:
 * - One of {TB_LOSS, TB_BLESSED_LOSS, TB_DRAW, TB_CURSED_WIN, TB_WIN}.
 *   Otherwise returns TB_RESULT_FAILED if the probe failed.
 *
 * NOTES:
 * - Engines should use this function during search.
 * - This function is thread safe assuming TB_NO_THREADS is disabled.
 */
static inline unsigned tb_probe_wdl(
    uint64_t _white,
    uint64_t _black,
    uint64_t _kings,
    uint64_t _queens,
    uint64_t _rooks,
    uint64_t _bishops,
    uint64_t _knights,
    uint64_t _pawns,
    unsigned _rule50,
    unsigned _castling,
    unsigned _ep,
    bool     _turn)
{
    if (_castling != 0)
        return TB_RESULT_FAILED;
    if (_rule50 != 0)
        return TB_RESULT_FAILED;
    return tb_probe_wdl_impl(_white, _black, _kings, _queens, _rooks,
        _bishops, _knights, _pawns, _ep, _turn);
}

/*
 * Probe the Distance-To-Zero (DTZ) table.
 *
 * PARAMETERS:
 * - white, black, kings, queens, rooks, bishops, knights, pawns:
 *   The current position (bitboards).
 * - rule50:
 *   The 50-move half-move clock.
 * - castling:
 *   Castling rights.  Set to zero if no castling is possible.
 * - ep:
 *   The en passant square (if exists).  Set to zero if there is no en passant
 *   square.
 * - turn:
 *   true=white, false=black
 * - results (OPTIONAL):
 *   Alternative results, one for each possible legal move.  The passed array
 *   must be TB_MAX_MOVES in size.
 *   If alternative results are not desired then set results=NULL.
 *
 * RETURN:
 * - A TB_RESULT value comprising:
 *   1) The WDL value (TB_GET_WDL)
 *   2) The suggested move (TB_GET_FROM, TB_GET_TO, TB_GET_PROMOTES, TB_GET_EP)
 *   3) The DTZ value (TB_GET_DTZ)
 *   The suggested move is guaranteed to preserved the WDL value.
 *
 *   Otherwise:
 *   1) TB_RESULT_STALEMATE is returned if the position is in stalemate.
 *   2) TB_RESULT_CHECKMATE is returned if the position is in checkmate.
 *   3) TB_RESULT_FAILED is returned if the probe failed.
 *
 *   If results!=NULL, then a TB_RESULT for each legal move will be generated
 *   and stored in the results array.  The results array will be terminated
 *   by TB_RESULT_FAILED.
 *
 * NOTES:
 * - Engines can use this function to probe at the root.  This function should
 *   not be used during search.
 * - DTZ tablebases can suggest unnatural moves, especially for losing
 *   positions.  Engines may prefer to traditional search combined with WDL
 *   move filtering using the alternative results array.
 * - This function is NOT thread safe.  For engines this function should only
 *   be called once at the root per search.
 */
static inline unsigned tb_probe_root(
    uint64_t _white,
    uint64_t _black,
    uint64_t _kings,
    uint64_t _queens,
    uint64_t _rooks,
    uint64_t _bishops,
    uint64_t _knights,
    uint64_t _pawns,
    unsigned _rule50,
    unsigned _castling,
    unsigned _ep,
    bool     _turn,
    unsigned *_results)
{
    if (_castling != 0)
        return TB_RESULT_FAILED;
    return tb_probe_root_impl(_white, _black, _kings, _queens, _rooks,
        _bishops, _knights, _pawns, _rule50, _ep, _turn, _results);
}

typedef uint16_t TbMove;

#define TB_MOVE_FROM(move)                                                 \
    (((move) >> 6) & 0x3F)
#define TB_MOVE_TO(move)                                                   \
    ((move) & 0x3F)
#define TB_MOVE_PROMOTES(move)                                             \
    (((move) >> 12) & 0x7)

struct TbRootMove {
  TbMove move;
  TbMove pv[TB_MAX_PLY];
  unsigned pvSize;
  int32_t tbScore, tbRank;
};

struct TbRootMoves {
  unsigned size;
  struct TbRootMove moves[TB_MAX_MOVES];
};

/*
 * Use the DTZ tables to rank and score all root moves.
 * INPUT: as for tb_probe_root
 * OUTPUT: TbRootMoves structure is filled in. This contains
 * an array of TbRootMove structures.
 * Each structure instance contains a rank, a score, and a
 * predicted principal variation.
 * RETURN VALUE:
 *   non-zero if ok, 0 means not all probes were successful
 *
 */
int tb_probe_root_dtz(
    uint64_t _white,
    uint64_t _black,
    uint64_t _kings,
    uint64_t _queens,
    uint64_t _rooks,
    uint64_t _bishops,
    uint64_t _knights,
    uint64_t _pawns,
    unsigned _rule50,
    unsigned _castling,
    unsigned _ep,
    bool     _turn,
    bool hasRepeated,
    bool useRule50,
    struct TbRootMoves *_results);

/*
// Use the WDL tables to rank and score all root moves.
// This is a fallback for the case that some or all DTZ tables are missing.
 * INPUT: as for tb_probe_root
 * OUTPUT: TbRootMoves structure is filled in. This contains
 * an array of TbRootMove structures.
 * Each structure instance contains a rank, a score, and a
 * predicted principal variation.
 * RETURN VALUE:
 *   non-zero if ok, 0 means not all probes were successful
 *
 */
int tb_probe_root_wdl(uint64_t _white,
    uint64_t _black,
    uint64_t _kings,
    uint64_t _queens,
    uint64_t _rooks,
    uint64_t _bishops,
    uint64_t _knights,
    uint64_t _pawns,
    unsigned _rule50,
    unsigned _castling,
    unsigned _ep,
    bool     _turn,
    bool useRule50,
    struct TbRootMoves *_results);

/****************************************************************************/
/* HELPER API                                                               */
/****************************************************************************/

/*
 * The HELPER API provides some useful additional functions.  It is optional
 * and can be disabled by defining TB_NO_HELPER_API.  Engines should disable
 * the HELPER API.
 */

#ifndef TB_NO_HELPER_API

extern unsigned tb_pop_count(uint64_t _bb);
extern unsigned tb_lsb(uint64_t _bb);
extern uint64_t tb_pop_lsb(uint64_t _bb);
extern uint64_t tb_king_attacks(unsigned _square);
extern uint64_t tb_queen_attacks(unsigned _square, uint64_t _occ);
extern uint64_t tb_rook_attacks(unsigned _square, uint64_t _occ);
extern uint64_t tb_bishop_attacks(unsigned _square, uint64_t _occ);
extern uint64_t tb_knight_attacks(unsigned _square);
extern uint64_t tb_pawn_attacks(unsigned _square, bool _color);

#endif


#endif
/*
Copyright (c) 2013-2018 Ronald de Man
Copyright (c) 2015 basil00
Modifications Copyright (c) 2016-2024 by Jon Dart

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifdef __cplusplus
#else
#endif
#ifdef TB_NO_STDBOOL
#typedef uint8 bool
#else
#endif

#define TB_PIECES 7
#define TB_HASHBITS  (TB_PIECES < 7 ?  11 : 12)
#define TB_MAX_PIECE (TB_PIECES < 7 ? 254 : 650)
#define TB_MAX_PAWN  (TB_PIECES < 7 ? 256 : 861)
#define TB_MAX_SYMS  4096

#ifndef _WIN32
#define SEP_CHAR ':'
#define FD int
#define FD_ERR -1
typedef size_t map_t;
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SEP_CHAR ';'
#define FD HANDLE
#define FD_ERR INVALID_HANDLE_VALUE
typedef HANDLE map_t;
#endif

// This must be after the inclusion of Windows headers, because otherwise
// std::byte conflicts with "byte" in rpcndr.h . The error occurs if C++
// standard is at lest 17, as std::byte was introduced in C++17.
#ifdef __cplusplus
using namespace std;
#endif

#define DECOMP64

// Threading support
#ifndef TB_NO_THREADS
#if defined(__cplusplus) && (__cplusplus >= 201103L)

#define LOCK_T std::mutex
#define LOCK_INIT(x)
#define LOCK_DESTROY(x)
#define LOCK(x) x.lock()
#define UNLOCK(x) x.unlock()

#else
#ifndef _WIN32
#define LOCK_T pthread_mutex_t
#define LOCK_INIT(x) pthread_mutex_init(&(x), NULL)
#define LOCK_DESTROY(x) pthread_mutex_destroy(&(x))
#define LOCK(x) pthread_mutex_lock(&(x))
#define UNLOCK(x) pthread_mutex_unlock(&(x))
#else
#define LOCK_T HANDLE
#define LOCK_INIT(x) do { x = CreateMutex(NULL, FALSE, NULL); } while (0)
#define LOCK_DESTROY(x) CloseHandle(x)
#define LOCK(x) WaitForSingleObject(x, INFINITE)
#define UNLOCK(x) ReleaseMutex(x)
#endif

#endif
#else /* TB_NO_THREADS */
#define LOCK_T          int
#define LOCK_INIT(x)    /* NOP */
#define LOCK_DESTROY(x) /* NOP */
#define LOCK(x)         /* NOP */
#define UNLOCK(x)       /* NOP */
#endif

// population count implementation
#undef TB_SOFTWARE_POP_COUNT

#if defined(TB_CUSTOM_POP_COUNT)
#define popcount(x) TB_CUSTOM_POP_COUNT(x)
#elif defined(TB_NO_HW_POP_COUNT)
#define TB_SOFTWARE_POP_COUNT
#elif defined (__GNUC__) && defined(__x86_64__) && defined(__SSE4_2__)
#define popcount(x)             (int)_mm_popcnt_u64((x))
#elif defined(_MSC_VER) && (_MSC_VER >= 1500) && defined(_M_AMD64)
#define popcount(x)             (int)_mm_popcnt_u64((x))
#else
// try to use a builtin
#if defined (__has_builtin)
#if __has_builtin(__builtin_popcountll)
#define popcount(x) __builtin_popcountll((x))
#else
#define TB_SOFTWARE_POP_COUNT
#endif
#else
#define TB_SOFTWARE_POP_COUNT
#endif
#endif

#ifdef TB_SOFTWARE_POP_COUNT
// Not a recognized compiler/architecture that has popcount, and
// no builtin available: fall back to a software popcount. This one
// is still reasonably fast.
static inline unsigned tb_software_popcount(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (x * 0x0101010101010101ull) >> 56;
}
#define popcount(x) tb_software_popcount(x)
#endif

// LSB (least-significant bit) implementation
#ifdef TB_CUSTOM_LSB
#define lsb(b) TB_CUSTOM_LSB(b)
#else
#if defined(__GNUC__)
static inline unsigned lsb(uint64_t b) {
    assert(b != 0);
    return __builtin_ffsll(b)-1;
}
#elif defined(_MSC_VER)
static inline unsigned lsb(uint64_t b) {
    assert(b != 0);
    DWORD index;
#ifdef _WIN64
    _BitScanForward64(&index,b);
    return (unsigned)index;
#else
    if (b & 0xffffffffULL) {
      _BitScanForward(&index,(unsigned long)(b & 0xffffffffULL));
      return (unsigned)index;
    }
    else {
      _BitScanForward(&index,(unsigned long)(b >> 32));
      return 32 + (unsigned)index;
    }
#endif
}
#else
/* not a compiler/architecture with recognized builtins */
static uint32_t get_bit32(uint64_t x) {
  return (uint32_t)(((int32_t)(x))&-((int32_t)(x)));
}
static const unsigned MAGIC32 = 0xe89b2be;
static const uint32_t MagicTable32[32] = {31,0,9,1,10,20,13,2,7,11,21,23,17,14,3,25,30,8,19,12,6,22,16,24,29,18,5,15,28,4,27,26};
static unsigned lsb(uint64_t b) {
  if (b & 0xffffffffULL)
    return MagicTable32[(get_bit32(b & 0xffffffffULL)*MAGIC32)>>27];
  else
    return MagicTable32[(get_bit32(b >> 32)*MAGIC32)>>27]+32;
}
#endif
#endif

#define max(a,b) a > b ? a : b
#define min(a,b) a < b ? a : b


#if _BYTE_ORDER == _BIG_ENDIAN

/* (unused)
static uint64_t from_le_u64(uint64_t input) {
  return bswap64(input);
}
*/

static uint32_t from_le_u32(uint32_t input) {
  return bswap32(input);
}

static uint16_t from_le_u16(uint16_t input) {
  return bswap16(input);
}

static uint64_t from_be_u64(uint64_t x) {
  return x;
}

static uint32_t from_be_u32(uint32_t x) {
  return x;
}

/* (unused)
  static uint16_t from_be_u16(uint16_t x) {
  return x;
 }*/

#else

/* (unused)
static uint64_t from_le_u64(uint64_t x) {
  return x;
}
*/

static uint32_t from_le_u32(uint32_t x) {
  return x;
}

static uint16_t from_le_u16(uint16_t x) {
  return x;
}

static uint64_t from_be_u64(uint64_t input) {
  return bswap64(input);
}

static uint32_t from_be_u32(uint32_t input) {
  return bswap32(input);
}

/* (unused)
static uint16_t from_be_u16(const uint16_t input) {
  return bswap16(input);
}
*/

#endif

inline static uint32_t read_le_u32(void *p)
{
  // input may be unaligned, so read into stack allocated
  // buffer, which should be aligned
  // (prevents runtime errors from glibc)
  unsigned char buffer[4];
  memcpy(buffer, p, 4);
  return from_le_u32(*(uint32_t *)buffer);
}

inline static uint16_t read_le_u16(void *p)
{
  // input may be unaligned, so read into stack allocated
  // buffer, which should be aligned
  // (prevents runtime errors from glibc)
  unsigned char buffer[2];
  buffer[0] = *((unsigned char*)p);
  buffer[1] = *(((unsigned char*)p)+1);
  return from_le_u16(*(uint16_t *)buffer);
}

static size_t file_size(FD fd) {
#ifdef _WIN32
  LARGE_INTEGER fileSize;
  if (GetFileSizeEx(fd, &fileSize)==0) {
    return 0;
  }
  return (size_t)fileSize.QuadPart;
#else
  struct stat buf;
  if (fstat(fd,&buf)) {
    return 0;
  } else {
    return buf.st_size;
  }
#endif
}

#ifndef TB_NO_THREADS
static LOCK_T tbMutex;
#endif
static int initialized = 0;
static int numPaths = 0;
static char *pathString = NULL;
static char **paths = NULL;

static FD open_tb(const char *str, const char *suffix)
{
  int i;
  FD fd;
  char *file;

  for (i = 0; i < numPaths; i++) {
    file = (char*)malloc(strlen(paths[i]) + strlen(str) +
                         strlen(suffix) + 2);
    strcpy(file, paths[i]);
#ifdef _WIN32
    strcat(file,"\\");
#else
    strcat(file,"/");
#endif
    strcat(file, str);
    strcat(file, suffix);
#ifndef _WIN32
    fd = open(file, O_RDONLY);
#else
#ifdef _UNICODE
    wchar_t ucode_name[4096];
    size_t len;
    mbstowcs_s(&len, ucode_name, 4096, file, _TRUNCATE);
    /* use FILE_FLAG_RANDOM_ACCESS because we are likely to access this file
       randomly, so prefetch is not helpful. See
       https://github.com/official-stockfish/Stockfish/pull/1829 */
    fd = CreateFile(ucode_name, GENERIC_READ, FILE_SHARE_READ, NULL,
			  OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
#else
    fd = CreateFile(file, GENERIC_READ, FILE_SHARE_READ, NULL,
			  OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
#endif
#endif
    free(file);
    if (fd != FD_ERR) {
      return fd;
    }
  }
  return FD_ERR;
}

static void close_tb(FD fd)
{
#ifndef _WIN32
  close(fd);
#else
  CloseHandle(fd);
#endif
}

static void *map_file(FD fd, map_t *mapping)
{
#ifndef _WIN32
  struct stat statbuf;
  if (fstat(fd, &statbuf)) {
    perror("fstat");
    close_tb(fd);
    return NULL;
  }
  *mapping = statbuf.st_size;
  void *data = mmap(NULL, statbuf.st_size, PROT_READ,
			      MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }
#ifdef POSIX_MADV_RANDOM
  /* Advise the kernel that we are likely to access this data
     region randomly, so prefetch is not helpful. See
     https://github.com/official-stockfish/Stockfish/pull/1829 */
  posix_madvise(data, statbuf.st_size, POSIX_MADV_RANDOM);
#endif
#else
  DWORD size_low, size_high;
  size_low = GetFileSize(fd, &size_high);
  HANDLE map = CreateFileMapping(fd, NULL, PAGE_READONLY, size_high, size_low,
				  NULL);
  if (map == NULL) {
    fprintf(stderr,"CreateFileMapping() failed, error = %lu.\n", GetLastError());
    return NULL;
  }
  *mapping = (map_t)map;
  void *data = (void *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
  if (data == NULL) {
    fprintf(stderr,"MapViewOfFile() failed, error = %lu.\n", GetLastError());
  }
#endif
  return data;
}

#ifndef _WIN32
static void unmap_file(void *data, map_t size)
{
  if (!data) return;
  if (munmap(data, size) != 0) {
      perror("munmap");
  }
}
#else
static void unmap_file(void *data, map_t mapping)
{
  if (!data) return;
  if (!UnmapViewOfFile(data)) {
	  fprintf(stderr, "unmap failed, error code %lu\n", GetLastError());
  }
  if (!CloseHandle((HANDLE)mapping)) {
	  fprintf(stderr, "CloseHandle failed, error code %lu\n", GetLastError());
  }
}
#endif

#define poplsb(x)               ((x) & ((x) - 1))

int TB_MaxCardinality = 0, TB_MaxCardinalityDTM = 0;
unsigned TB_LARGEST = 0;
//extern int TB_CardinalityDTM;

static const char *tbSuffix[] = { ".rtbw", ".rtbm", ".rtbz" };
static uint32_t tbMagic[] = { 0x5d23e871, 0x88ac504b, 0xa50c66d7 };

enum { WDL, DTM, DTZ };
enum { PIECE_ENC, FILE_ENC, RANK_ENC };

// Attack and move generation code
/*
Copyright (c) 2015 basil00
Modifications Copyright (c) 2016-2020 by Jon Dart

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define TB_PAWN 1
#define TB_KNIGHT 2
#define TB_BISHOP 3
#define TB_ROOK 4
#define TB_QUEEN 5
#define TB_KING 6

#define TB_WPAWN TB_PAWN
#define TB_BPAWN (TB_PAWN | 8)

#define WHITE_KING              (TB_WPAWN + 5)
#define WHITE_QUEEN             (TB_WPAWN + 4)
#define WHITE_ROOK              (TB_WPAWN + 3)
#define WHITE_BISHOP            (TB_WPAWN + 2)
#define WHITE_KNIGHT            (TB_WPAWN + 1)
#define WHITE_PAWN              TB_WPAWN
#define BLACK_KING              (TB_BPAWN + 5)
#define BLACK_QUEEN             (TB_BPAWN + 4)
#define BLACK_ROOK              (TB_BPAWN + 3)
#define BLACK_BISHOP            (TB_BPAWN + 2)
#define BLACK_KNIGHT            (TB_BPAWN + 1)
#define BLACK_PAWN              TB_BPAWN

#define PRIME_WHITE_QUEEN       11811845319353239651ull
#define PRIME_WHITE_ROOK        10979190538029446137ull
#define PRIME_WHITE_BISHOP      12311744257139811149ull
#define PRIME_WHITE_KNIGHT      15202887380319082783ull
#define PRIME_WHITE_PAWN        17008651141875982339ull
#define PRIME_BLACK_QUEEN       15484752644942473553ull
#define PRIME_BLACK_ROOK        18264461213049635989ull
#define PRIME_BLACK_BISHOP      15394650811035483107ull
#define PRIME_BLACK_KNIGHT      13469005675588064321ull
#define PRIME_BLACK_PAWN        11695583624105689831ull

#define BOARD_RANK_EDGE         0x8181818181818181ull
#define BOARD_FILE_EDGE         0xFF000000000000FFull
#define BOARD_EDGE              (BOARD_RANK_EDGE | BOARD_FILE_EDGE)
#define BOARD_RANK_1            0x00000000000000FFull
#define BOARD_FILE_A            0x8080808080808080ull

#define KEY_KvK                 0

#define BEST_NONE               0xFFFF
#define SCORE_ILLEGAL           0x7FFF

// Note: WHITE, BLACK values are reverse of Stockfish
#ifdef __cplusplus
namespace {
enum Color { BLACK, WHITE };
enum PieceType { PAWN=1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum Piece {
  W_PAWN = 1, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
  B_PAWN = 9, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING };
#else
typedef enum Color { BLACK, WHITE } Color;
typedef enum PieceType { PAWN=1, KNIGHT, BISHOP, ROOK, QUEEN, KING } PieceType;
typedef enum Piece {
  W_PAWN = 1, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
  B_PAWN = 9, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING
} Piece;
#endif

static inline Color ColorOfPiece(int piece) {
  return (Color)(!(piece >> 3));
}

static inline PieceType TypeOfPiece(int piece) {
  return (PieceType)(piece & 7);
}

typedef int32_t Value;

typedef struct Pos
{
    uint64_t white;
    uint64_t black;
    uint64_t kings;
    uint64_t queens;
    uint64_t rooks;
    uint64_t bishops;
    uint64_t knights;
    uint64_t pawns;
    uint8_t rule50;
    uint8_t ep;
    bool turn;
} Pos;

static inline uint64_t pieces_by_type(const Pos *pos, Color c, PieceType p) {
  uint64_t mask = (c == WHITE) ? pos->white : pos->black;
  switch(p) {
  case PAWN:
    return pos->pawns & mask;
  case KNIGHT:
    return pos->knights & mask;
  case BISHOP:
    return pos->bishops & mask;
  case ROOK:
    return pos->rooks & mask;
  case QUEEN:
    return pos->queens & mask;
  case KING:
    return pos->kings & mask;
  default:
    assert(0);
    return 0;
  }
}

static const char piece_to_char[] = " PNBRQK  pnbrqk";
  
// map upper-case characters to piece types
static PieceType char_to_piece_type(char c) {
    for (int i = PAWN; i <= KING; i++)
       if (c == piece_to_char[i]) {
         return (PieceType)i;
       }
    return (PieceType)0;
}

#define rank(s)                 ((s) >> 3)
#define file(s)                 ((s) & 0x07)
#define board(s)                ((uint64_t)1 << (s))
#define square(r, f)            (8 * (r) + (f))

#ifdef TB_KING_ATTACKS
#define king_attacks(s)         TB_KING_ATTACKS(s)
#define king_attacks_init()     /* NOP */
#else       /* TB_KING_ATTACKS */

static uint64_t king_attacks_table[64];

#define king_attacks(s)         king_attacks_table[(s)]

static void king_attacks_init(void)
{
    for (unsigned s = 0; s < 64; s++)
    {
        unsigned r = rank(s);
        unsigned f = file(s);
        uint64_t b = 0;
        if (r != 0 && f != 0)
            b |= board(square(r-1, f-1));
        if (r != 0)
            b |= board(square(r-1, f));
        if (r != 0 && f != 7)
            b |= board(square(r-1, f+1));
        if (f != 7)
            b |= board(square(r, f+1));
        if (r != 7 && f != 7)
            b |= board(square(r+1, f+1));
        if (r != 7)
            b |= board(square(r+1, f));
        if (r != 7 && f != 0)
            b |= board(square(r+1, f-1));
        if (f != 0)
            b |= board(square(r, f-1));
        king_attacks_table[s] = b;
    }
}

#endif      /* TB_KING_ATTACKS */

#ifdef TB_KNIGHT_ATTACKS
#define knight_attacks(s)       TB_KNIGHT_ATTACKS(s)
#define knight_attacks_init()   /* NOP */
#else       /* TB_KNIGHT_ATTACKS */

static uint64_t knight_attacks_table[64];

#define knight_attacks(s)       knight_attacks_table[(s)]

static void knight_attacks_init(void)
{
    for (unsigned s = 0; s < 64; s++)
    {
        int r1, r = rank(s);
        int f1, f = file(s);
        uint64_t b = 0;
        r1 = r-1; f1 = f-2;
        if (r1 >= 0 && f1 >= 0)
            b |= board(square(r1, f1));
        r1 = r-1; f1 = f+2;
        if (r1 >= 0 && f1 <= 7)
            b |= board(square(r1, f1));
        r1 = r-2; f1 = f-1;
        if (r1 >= 0 && f1 >= 0)
            b |= board(square(r1, f1));
        r1 = r-2; f1 = f+1;
        if (r1 >= 0 && f1 <= 7)
            b |= board(square(r1, f1));
        r1 = r+1; f1 = f-2;
        if (r1 <= 7 && f1 >= 0)
            b |= board(square(r1, f1));
        r1 = r+1; f1 = f+2;
        if (r1 <= 7 && f1 <= 7)
            b |= board(square(r1, f1));
        r1 = r+2; f1 = f-1;
        if (r1 <= 7 && f1 >= 0)
            b |= board(square(r1, f1));
        r1 = r+2; f1 = f+1;
        if (r1 <= 7 && f1 <= 7)
            b |= board(square(r1, f1));
        knight_attacks_table[s] = b;
    }
}

#endif      /* TB_KNIGHT_ATTACKS */

#ifdef TB_BISHOP_ATTACKS
#define bishop_attacks(s, occ)  TB_BISHOP_ATTACKS(s, occ)
#define bishop_attacks_init()   /* NOP */
#else       /* TB_BISHOP_ATTACKS */

static uint64_t diag_attacks_table[64][64];
static uint64_t anti_attacks_table[64][64];

static const unsigned square2diag_table[64] =
{
    0,  1,  2,  3,  4,  5,  6,  7,
    14, 0,  1,  2,  3,  4,  5,  6,
    13, 14, 0,  1,  2,  3,  4,  5,
    12, 13, 14, 0,  1,  2,  3,  4,
    11, 12, 13, 14, 0,  1,  2,  3,
    10, 11, 12, 13, 14, 0,  1,  2,
    9,  10, 11, 12, 13, 14, 0,  1,
    8,  9,  10, 11, 12, 13, 14, 0
};

static const unsigned square2anti_table[64] =
{
    8,  9,  10, 11, 12, 13, 14, 0,
    9,  10, 11, 12, 13, 14, 0,  1,
    10, 11, 12, 13, 14, 0,  1,  2,
    11, 12, 13, 14, 0,  1,  2,  3,
    12, 13, 14, 0,  1,  2,  3,  4,
    13, 14, 0,  1,  2,  3,  4,  5,
    14, 0,  1,  2,  3,  4,  5,  6,
    0,  1,  2,  3,  4,  5,  6,  7
};

static const uint64_t diag2board_table[15] =
{
    0x8040201008040201ull,
    0x0080402010080402ull,
    0x0000804020100804ull,
    0x0000008040201008ull,
    0x0000000080402010ull,
    0x0000000000804020ull,
    0x0000000000008040ull,
    0x0000000000000080ull,
    0x0100000000000000ull,
    0x0201000000000000ull,
    0x0402010000000000ull,
    0x0804020100000000ull,
    0x1008040201000000ull,
    0x2010080402010000ull,
    0x4020100804020100ull,
};

static const uint64_t anti2board_table[15] =
{
    0x0102040810204080ull,
    0x0204081020408000ull,
    0x0408102040800000ull,
    0x0810204080000000ull,
    0x1020408000000000ull,
    0x2040800000000000ull,
    0x4080000000000000ull,
    0x8000000000000000ull,
    0x0000000000000001ull,
    0x0000000000000102ull,
    0x0000000000010204ull,
    0x0000000001020408ull,
    0x0000000102040810ull,
    0x0000010204081020ull,
    0x0001020408102040ull,
};

static inline size_t diag2index(uint64_t b)
{
    b *= 0x0101010101010101ull;
    b >>= 56;
    b >>= 1;
    return (size_t)b;
}

static inline size_t anti2index(uint64_t b)
{
    return diag2index(b);
}

#define diag(s)                 square2diag_table[(s)]
#define anti(s)                 square2anti_table[(s)]
#define diag2board(d)           diag2board_table[(d)]
#define anti2board(a)           anti2board_table[(a)]

static uint64_t bishop_attacks(unsigned sq, uint64_t occ)
{
    occ &= ~board(sq);
    unsigned d = diag(sq), a = anti(sq);
    uint64_t d_occ = occ & (diag2board(d) & ~BOARD_EDGE);
    uint64_t a_occ = occ & (anti2board(a) & ~BOARD_EDGE);
    size_t d_idx = diag2index(d_occ);
    size_t a_idx = anti2index(a_occ);
    uint64_t d_attacks = diag_attacks_table[sq][d_idx];
    uint64_t a_attacks = anti_attacks_table[sq][a_idx];
    return d_attacks | a_attacks;
}

static void bishop_attacks_init(void)
{
    for (unsigned idx = 0; idx < 64; idx++)
    {
        unsigned idx1 = idx << 1;
        for (unsigned s = 0; s < 64; s++)
        {
            int r = rank(s);
            int f = file(s);
            uint64_t b = 0;
            for (int i = -1; f + i >= 0 && r + i >= 0; i--)
            {
                unsigned occ = (1 << (f + i));
                b |= board(square(r + i, f + i));
                if (idx1 & occ)
                    break;
            }
            for (int i = 1; f + i <= 7 && r + i <= 7; i++)
            {
                unsigned occ = (1 << (f + i));
                b |= board(square(r + i, f + i));
                if (idx1 & occ)
                    break;
            }
            diag_attacks_table[s][idx] = b;
        }
    }

    for (unsigned idx = 0; idx < 64; idx++)
    {
        unsigned idx1 = idx << 1;
        for (unsigned s = 0; s < 64; s++)
        {
            int r = rank(s);
            int f = file(s);
            uint64_t b = 0;
            for (int i = -1; f + i >= 0 && r - i <= 7; i--)
            {
                unsigned occ = (1 << (f + i));
                b |= board(square(r - i, f + i));
                if (idx1 & occ)
                    break;
            }
            for (int i = 1; f + i <= 7 && r - i >= 0; i++)
            {
                unsigned occ = (1 << (f + i));
                b |= board(square(r - i, f + i));
                if (idx1 & occ)
                    break;
            }
            anti_attacks_table[s][idx] = b;
        }
    }
}

#endif      /* TB_BISHOP_ATTACKS */

#ifdef TB_ROOK_ATTACKS
#define rook_attacks(s, occ)    TB_ROOK_ATTACKS(s, occ)
#define rook_attacks_init()     /* NOP */
#else       /* TB_ROOK_ATTACKS */

static uint64_t rank_attacks_table[64][64];
static uint64_t file_attacks_table[64][64];

static inline size_t rank2index(uint64_t b, unsigned r)
{
    b >>= (8 * r);
    b >>= 1;
    return (size_t)b;
}

static inline size_t file2index(uint64_t b, unsigned f)
{
    b >>= f;
    b *= 0x0102040810204080ull;
    b >>= 56;
    b >>= 1;
    return (size_t)b;
}

#define rank2board(r)           (0xFFull << (8 * (r)))
#define file2board(f)           (0x0101010101010101ull << (f))

static uint64_t rook_attacks(unsigned sq, uint64_t occ)
{
    occ &= ~board(sq);
    unsigned r = rank(sq), f = file(sq);
    uint64_t r_occ = occ & (rank2board(r) & ~BOARD_RANK_EDGE);
    uint64_t f_occ = occ & (file2board(f) & ~BOARD_FILE_EDGE);
    size_t r_idx = rank2index(r_occ, r);
    size_t f_idx = file2index(f_occ, f);
    uint64_t r_attacks = rank_attacks_table[sq][r_idx];
    uint64_t f_attacks = file_attacks_table[sq][f_idx];
    return r_attacks | f_attacks;
}

static void rook_attacks_init(void)
{
    for (unsigned idx = 0; idx < 64; idx++)
    {
        unsigned idx1 = idx << 1, occ;
        for (int f = 0; f <= 7; f++)
        {
            uint64_t b = 0;
            if (f > 0)
            {
                int i = f-1;
                do
                {
                    occ = (1 << i);
                    b |= board(square(0, i));
                    i--;
                }
                while (!(idx1 & occ) && i >= 0);
            }
            if (f < 7)
            {
                int i = f+1;
                do
                {
                    occ = (1 << i);
                    b |= board(square(0, i));
                    i++;
                }
                while (!(idx1 & occ) && i <= 7);
            }
            for (int r = 0; r <= 7; r++)
            {
                rank_attacks_table[square(r, f)][idx] = b;
                b <<= 8;
            }
        }
    }
    for (unsigned idx = 0; idx < 64; idx++)
    {
        unsigned idx1 = idx << 1, occ;
        for (int r = 0; r <= 7; r++)
        {
            uint64_t b = 0;
            if (r > 0)
            {
                int i = r-1;
                do
                {
                    occ = (1 << i);
                    b |= board(square(i, 0));
                    i--;
                }
                while (!(idx1 & occ) && i >= 0);
            }
            if (r < 7)
            {
                int i = r+1;
                do
                {
                    occ = (1 << i);
                    b |= board(square(i, 0));
                    i++;
                }
                while (!(idx1 & occ) && i <= 7);
            }
            for (int f = 0; f <= 7; f++)
            {
                file_attacks_table[square(r, f)][idx] = b;
                b <<= 1;
            }
        }
    }
}

#endif      /* TB_ROOK_ATTACKS */

#ifdef TB_QUEEN_ATTACKS
#define queen_attacks(s, occ)   TB_QUEEN_ATTACKS(s, occ)
#else       /* TB_QUEEN_ATTACKS */
#define queen_attacks(s, occ)   \
    (rook_attacks((s), (occ)) | bishop_attacks((s), (occ)))
#endif      /* TB_QUEEN_ATTACKS */

#ifdef TB_PAWN_ATTACKS
#define pawn_attacks(s, c)      TB_PAWN_ATTACKS(s, c)
#define pawn_attacks_init()     /* NOP */
#else       /* TB_PAWN_ATTACKS */

static uint64_t pawn_attacks_table[2][64];

#define pawn_attacks(s, c)      pawn_attacks_table[(c)][(s)]

static void pawn_attacks_init(void)
{
    for (unsigned s = 0; s < 64; s++)
    {
        int r = rank(s);
        int f = file(s);

        uint64_t b = 0;
        if (r != 7)
        {
            if (f != 0)
                b |= board(square(r+1, f-1));
            if (f != 7)
                b |= board(square(r+1, f+1));
        }
        pawn_attacks_table[1][s] = b;

        b = 0;
        if (r != 0)
        {
            if (f != 0)
                b |= board(square(r-1, f-1));
            if (f != 7)
                b |= board(square(r-1, f+1));
        }
        pawn_attacks_table[0][s] = b;
    }
}

#endif      /* TB_PAWN_ATTACKS */

/*
 * Given a position, produce a 64-bit material signature key.
 */
static uint64_t calc_key(const Pos *pos, bool mirror)
{
    uint64_t white = pos->white, black = pos->black;
    if (mirror)
    {
        uint64_t tmp = white;
        white = black;
        black = tmp;
    }
    return popcount(white & pos->queens)  * PRIME_WHITE_QUEEN +
           popcount(white & pos->rooks)   * PRIME_WHITE_ROOK +
           popcount(white & pos->bishops) * PRIME_WHITE_BISHOP +
           popcount(white & pos->knights) * PRIME_WHITE_KNIGHT +
           popcount(white & pos->pawns)   * PRIME_WHITE_PAWN +
           popcount(black & pos->queens)  * PRIME_BLACK_QUEEN +
           popcount(black & pos->rooks)   * PRIME_BLACK_ROOK +
           popcount(black & pos->bishops) * PRIME_BLACK_BISHOP +
           popcount(black & pos->knights) * PRIME_BLACK_KNIGHT +
           popcount(black & pos->pawns)   * PRIME_BLACK_PAWN;
}

// Produce a 64-bit material key corresponding to the material combination
// defined by pcs[16], where pcs[1], ..., pcs[6] are the number of white
// pawns, ..., kings and pcs[9], ..., pcs[14] are the number of black
// pawns, ..., kings.
static uint64_t calc_key_from_pcs(int *pcs, int mirror)
{
    mirror = (mirror? 8: 0);
    return pcs[WHITE_QUEEN ^ mirror] * PRIME_WHITE_QUEEN +
           pcs[WHITE_ROOK ^ mirror] * PRIME_WHITE_ROOK +
           pcs[WHITE_BISHOP ^ mirror] * PRIME_WHITE_BISHOP +
           pcs[WHITE_KNIGHT ^ mirror] * PRIME_WHITE_KNIGHT +
           pcs[WHITE_PAWN ^ mirror] * PRIME_WHITE_PAWN +
           pcs[BLACK_QUEEN ^ mirror] * PRIME_BLACK_QUEEN +
           pcs[BLACK_ROOK ^ mirror] * PRIME_BLACK_ROOK +
           pcs[BLACK_BISHOP ^ mirror] * PRIME_BLACK_BISHOP +
           pcs[BLACK_KNIGHT ^ mirror] * PRIME_BLACK_KNIGHT +
           pcs[BLACK_PAWN ^ mirror] * PRIME_BLACK_PAWN;
}

// Produce a 64-bit material key corresponding to the material combination
// piece[0], ..., piece[num - 1], where each value corresponds to a piece
// (1-6 for white pawn-king, 9-14 for black pawn-king).
static uint64_t calc_key_from_pieces(uint8_t *piece, int num)
{
    uint64_t key = 0;
    static const uint64_t keys[16] = {0,PRIME_WHITE_PAWN,PRIME_WHITE_KNIGHT,
                                      PRIME_WHITE_BISHOP,PRIME_WHITE_ROOK,
                                      PRIME_WHITE_QUEEN,0,0,PRIME_BLACK_PAWN,
                                      PRIME_BLACK_KNIGHT,PRIME_BLACK_BISHOP,
                                      PRIME_BLACK_ROOK,PRIME_BLACK_QUEEN,0};
    for (int i = 0; i < num; i++) {
      assert(piece[i]<16);
      key += keys[piece[i]];
    }
    return key;
}

#define make_move(promote, from, to)                                    \
    ((((promote) & 0x7) << 12) | (((from) & 0x3F) << 6) | ((to) & 0x3F))
#define move_from(move)                                                 \
    (((move) >> 6) & 0x3F)
#define move_to(move)                                                   \
    ((move) & 0x3F)
#define move_promotes(move)                                             \
    (((move) >> 12) & 0x7)

static inline int type_of_piece_moved(Pos *pos, TbMove move) {
  for (int i = PAWN; i <= KING; i++) {
    if ((pieces_by_type(pos,(Color)(pos->turn == WHITE),(PieceType)i) & board(move_from(move))) != 0) {
      return i;
    }
  }
  assert(0);
  return 0;
}

#define MAX_MOVES               TB_MAX_MOVES
#define MOVE_STALEMATE          0xFFFF
#define MOVE_CHECKMATE          0xFFFE

static TbMove *add_move(TbMove *moves, bool promotes, unsigned from,
    unsigned to)
{
    if (!promotes)
        *moves++ = make_move(TB_PROMOTES_NONE, from, to);
    else
    {
        *moves++ = make_move(TB_PROMOTES_QUEEN, from, to);
        *moves++ = make_move(TB_PROMOTES_KNIGHT, from, to);
        *moves++ = make_move(TB_PROMOTES_ROOK, from, to);
        *moves++ = make_move(TB_PROMOTES_BISHOP, from, to);
    }
    return moves;
}

/*
 * Generate all captures, including all underpomotions
 */
static TbMove *gen_captures(const Pos *pos, TbMove *moves)
{
    uint64_t occ = pos->white | pos->black;
    uint64_t us = (pos->turn? pos->white: pos->black),
             them = (pos->turn? pos->black: pos->white);
    uint64_t b, att;
    {
        unsigned from = lsb(pos->kings & us);
        assert(from < 64);
        for (att = king_attacks(from) & them; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->queens; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = queen_attacks(from, occ) & them; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->rooks; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = rook_attacks(from, occ) & them; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->bishops; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = bishop_attacks(from, occ) & them; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->knights; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = knight_attacks(from) & them; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->pawns; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        att = pawn_attacks(from, pos->turn);
        if (pos->ep != 0 && ((att & board(pos->ep)) != 0))
        {
            unsigned to = pos->ep;
            moves = add_move(moves, false, from, to);
        }
        for (att = att & them; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, (rank(to) == 7 || rank(to) == 0), from,
                to);
        }
    }
    return moves;
}

/*
 * Generate all moves.
 */
static TbMove *gen_moves(const Pos *pos, TbMove *moves)
{
    uint64_t occ = pos->white | pos->black;
    uint64_t us = (pos->turn? pos->white: pos->black),
             them = (pos->turn? pos->black: pos->white);
    uint64_t b, att;
    
    {
        unsigned from = lsb(pos->kings & us);
        for (att = king_attacks(from) & ~us; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->queens; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = queen_attacks(from, occ) & ~us; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->rooks; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = rook_attacks(from, occ) & ~us; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->bishops; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = bishop_attacks(from, occ) & ~us; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->knights; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        for (att = knight_attacks(from) & ~us; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, false, from, to);
        }
    }
    for (b = us & pos->pawns; b; b = poplsb(b))
    {
        unsigned from = lsb(b);
        unsigned next = from + (pos->turn? 8: -8);
        att = pawn_attacks(from, pos->turn);
        if (pos->ep != 0 && ((att & board(pos->ep)) != 0))
        {
            unsigned to = pos->ep;
            moves = add_move(moves, false, from, to);
        }
        att &= them;
        if ((board(next) & occ) == 0)
        {
            att |= board(next);
            unsigned next2 = from + (pos->turn? 16: -16);
            if ((pos->turn? rank(from) == 1: rank(from) == 6) &&
                    ((board(next2) & occ) == 0))
                att |= board(next2);
        }
        for (; att; att = poplsb(att))
        {
            unsigned to = lsb(att);
            moves = add_move(moves, (rank(to) == 7 || rank(to) == 0), from,
                to);
        }
    }
    return moves;
}

/*
 * Test if the given move is an en passant capture.
 */
static bool is_en_passant(const Pos *pos, TbMove move)
{
    uint16_t from = move_from(move);
    uint16_t to   = move_to(move);
    uint64_t us = (pos->turn? pos->white: pos->black);
    if (pos->ep == 0)
        return false;
    if (to != pos->ep)
        return false;
    if ((board(from) & us & pos->pawns) == 0)
        return false;
    return true;
}


/*
 * Test if the given move is a capture.
 */
static bool is_capture(const Pos *pos, TbMove move)
{
   uint16_t to   = move_to(move);
   uint64_t them = (pos->turn? pos->black: pos->white);
   return (them & board(to)) != 0 || is_en_passant(pos,move);
}


/*
 * Test if the given position is legal.
 * (Pawns on backrank? Can the king be captured?)
 */
static bool is_legal(const Pos *pos)
{
    uint64_t occ = pos->white | pos->black;
    uint64_t us = (pos->turn? pos->black: pos->white),
             them = (pos->turn? pos->white: pos->black);
    uint64_t king = pos->kings & us;
    if (!king)
        return false;
    unsigned sq = lsb(king);
    if (king_attacks(sq) & (pos->kings & them))
        return false;
    uint64_t ratt = rook_attacks(sq, occ);
    uint64_t batt = bishop_attacks(sq, occ);
    if (ratt & (pos->rooks & them))
        return false;
    if (batt & (pos->bishops & them))
        return false;
    if ((ratt | batt) & (pos->queens & them))
        return false;
    if (knight_attacks(sq) & (pos->knights & them))
        return false;
    if (pawn_attacks(sq, !pos->turn) & (pos->pawns & them))
        return false;
    return true;
}

/*
 * Test if the king is in check.
 */
static bool is_check(const Pos *pos)
{
    uint64_t occ = pos->white | pos->black;
    uint64_t us = (pos->turn? pos->white: pos->black),
             them = (pos->turn? pos->black: pos->white);
    uint64_t king = pos->kings & us;
    assert(king != 0);
    unsigned sq = lsb(king);
    uint64_t ratt = rook_attacks(sq, occ);
    uint64_t batt = bishop_attacks(sq, occ);
    if (ratt & (pos->rooks & them))
        return true;
    if (batt & (pos->bishops & them))
        return true;
    if ((ratt | batt) & (pos->queens & them))
        return true;
    if (knight_attacks(sq) & (pos->knights & them))
        return true;
    if (pawn_attacks(sq, pos->turn) & (pos->pawns & them))
        return true;
    return false;
}

/*
 * Test if the position is valid.
 */
static bool is_valid(const Pos *pos)
{
    if (popcount(pos->kings) != 2)
        return false;
    if (popcount(pos->kings & pos->white) != 1)
        return false;
    if (popcount(pos->kings & pos->black) != 1)
        return false;
    if ((pos->white & pos->black) != 0)
        return false;
    if ((pos->kings & pos->queens) != 0)
        return false;
    if ((pos->kings & pos->rooks) != 0)
        return false;
    if ((pos->kings & pos->bishops) != 0)
        return false;
    if ((pos->kings & pos->knights) != 0)
        return false;
    if ((pos->kings & pos->pawns) != 0)
        return false;
    if ((pos->queens & pos->rooks) != 0)
        return false;
    if ((pos->queens & pos->bishops) != 0)
        return false;
    if ((pos->queens & pos->knights) != 0)
        return false;
    if ((pos->queens & pos->pawns) != 0)
        return false;
    if ((pos->rooks & pos->bishops) != 0)
        return false;
    if ((pos->rooks & pos->knights) != 0)
        return false;
    if ((pos->rooks & pos->pawns) != 0)
        return false;
    if ((pos->bishops & pos->knights) != 0)
        return false;
    if ((pos->bishops & pos->pawns) != 0)
        return false;
    if ((pos->knights & pos->pawns) != 0)
        return false;
    if (pos->pawns & BOARD_FILE_EDGE)
        return false;
    if ((pos->white | pos->black) !=
        (pos->kings | pos->queens | pos->rooks | pos->bishops | pos->knights |
         pos->pawns))
        return false;
    return is_legal(pos);
}

#define do_bb_move(b, from, to)                                         \
    (((b) & (~board(to)) & (~board(from))) |                            \
        ((((b) >> (from)) & 0x1) << (to)))

static bool do_move(Pos *pos, const Pos *pos0, TbMove move)
{
    unsigned from = move_from(move); 
    unsigned to = move_to(move);  
    unsigned promotes = move_promotes(move);  
    pos->turn = !pos0->turn;
    pos->white = do_bb_move(pos0->white, from, to);
    pos->black = do_bb_move(pos0->black, from, to);
    pos->kings = do_bb_move(pos0->kings, from, to);
    pos->queens = do_bb_move(pos0->queens, from, to); 
    pos->rooks = do_bb_move(pos0->rooks, from, to);
    pos->bishops = do_bb_move(pos0->bishops, from, to);  
    pos->knights = do_bb_move(pos0->knights, from, to);  
    pos->pawns = do_bb_move(pos0->pawns, from, to);
    pos->ep = 0;
    if (promotes != TB_PROMOTES_NONE) 
    {  
        pos->pawns &= ~board(to);       // Promotion
        switch (promotes)
        { 
            case TB_PROMOTES_QUEEN:
                pos->queens |= board(to); break;
            case TB_PROMOTES_ROOK: 
                pos->rooks |= board(to); break; 
            case TB_PROMOTES_BISHOP:  
                pos->bishops |= board(to); break;  
            case TB_PROMOTES_KNIGHT:  
                pos->knights |= board(to); break;  
        }
        pos->rule50 = 0;
    }
    else if ((board(from) & pos0->pawns) != 0)
    {
        pos->rule50 = 0;                // Pawn move
        if (rank(from) == 1 && rank(to) == 3 &&
            (pawn_attacks(from+8, true) & pos0->pawns & pos0->black) != 0)
            pos->ep = from+8;
        else if (rank(from) == 6 && rank(to) == 4 &&
            (pawn_attacks(from-8, false) & pos0->pawns & pos0->white) != 0)
            pos->ep = from-8;
        else if (to == pos0->ep)
        {
            unsigned ep_to = (pos0->turn? to-8: to+8);
            uint64_t ep_mask = ~board(ep_to);
            pos->white &= ep_mask;
            pos->black &= ep_mask;
            pos->pawns &= ep_mask;
        }
    }
    else if ((board(to) & (pos0->white | pos0->black)) != 0)
        pos->rule50 = 0;                // Capture
    else
        pos->rule50 = pos0->rule50 + 1; // Normal move
    if (!is_legal(pos))
        return false;
    return true;
}

static bool legal_move(const Pos *pos, TbMove move) {
   struct Pos pos1;
   return do_move(&pos1, pos, move);
}

/*
 * Test if the king is in checkmate.
 */
static bool is_mate(const Pos *pos)
{
    if (!is_check(pos))
        return false;
    uint16_t moves0[MAX_MOVES];
    uint16_t *moves = moves0;
    uint16_t *end = gen_moves(pos, moves);
    for (; moves < end; moves++)
    {
        Pos pos1;
        if (do_move(&pos1, pos, *moves))
            return false;
    }
    return true;
}

/*
 * Generate all legal moves.
 */
static TbMove *gen_legal(const Pos *pos, TbMove *moves)
{
  TbMove pl_moves[TB_MAX_MOVES];
  TbMove *end = gen_moves(pos, pl_moves);
  TbMove *results = moves;
  for (TbMove *m = pl_moves; m < end; m++) {
    if (legal_move(pos,*m)) {
      *results++ = *m;
    }
  }
  return results;
}

#ifdef __cplusplus
};
#endif


struct PairsData {
  uint8_t *indexTable;
  uint16_t *sizeTable;
  uint8_t *data;
  uint16_t *offset;
  uint8_t *symLen;
  uint8_t *symPat;
  uint8_t blockSize;
  uint8_t idxBits;
  uint8_t minLen;
  uint8_t constValue[2];
  uint64_t base[1];
};

struct EncInfo {
  struct PairsData *precomp;
  size_t factor[TB_PIECES];
  uint8_t pieces[TB_PIECES];
  uint8_t norm[TB_PIECES];
};

struct BaseEntry {
  uint64_t key;
  uint8_t *data[3];
  map_t mapping[3];
#ifdef __cplusplus
#if __cplusplus >= 202002L
  atomic<bool> ready[3]{false, false, false};
#else
  atomic<bool> ready[3];
#endif
#else
  atomic_bool ready[3];
#endif
  uint8_t num;
  bool symmetric, hasPawns, hasDtm, hasDtz;
  union {
    bool kk_enc;
    uint8_t pawns[2];
  };
  bool dtmLossOnly;
};

struct PieceEntry {
  struct BaseEntry be;
  struct EncInfo ei[5]; // 2 + 2 + 1
  uint16_t *dtmMap;
  uint16_t dtmMapIdx[2][2];
  void *dtzMap;
  uint16_t dtzMapIdx[4];
  uint8_t dtzFlags;
};

struct PawnEntry {
  struct BaseEntry be;
  struct EncInfo ei[24]; // 4 * 2 + 6 * 2 + 4
  uint16_t *dtmMap;
  uint16_t dtmMapIdx[6][2][2];
  void *dtzMap;
  uint16_t dtzMapIdx[4][4];
  uint8_t dtzFlags[4];
  bool dtmSwitched;
};

struct TbHashEntry {
  uint64_t key;
  struct BaseEntry *ptr;
#ifdef __cplusplus
  atomic<bool> error;
#else
  atomic_bool error;
#endif
};

static int tbNumPiece, tbNumPawn;
static int numWdl, numDtm, numDtz;

static struct PieceEntry *pieceEntry;
static struct PawnEntry *pawnEntry;
static struct TbHashEntry tbHash[1 << TB_HASHBITS];

static void init_indices(void);

// Forward declarations. These functions without the tb_
// prefix take a pos structure as input.
static int probe_wdl(Pos *pos, int *success);
static int probe_dtz(Pos *pos, int *success);
static int root_probe_wdl(const Pos *pos, bool useRule50, struct TbRootMoves *rm);
static int root_probe_dtz(const Pos *pos, bool hasRepeated, bool useRule50, struct TbRootMoves *rm);
static uint16_t probe_root(Pos *pos, int *score, unsigned *results);

unsigned tb_probe_wdl_impl(
    uint64_t white,
    uint64_t black,
    uint64_t kings,
    uint64_t queens,
    uint64_t rooks,
    uint64_t bishops,
    uint64_t knights,
    uint64_t pawns,
    unsigned ep,
    bool turn)
{
    Pos pos =
    {
        white,
        black,
        kings,
        queens,
        rooks,
        bishops,
        knights,
        pawns,
        0,
        (uint8_t)ep,
        turn
    };
    int success;
    int v = probe_wdl(&pos, &success);
    if (success == 0)
        return TB_RESULT_FAILED;
    return (unsigned)(v + 2);
}

static unsigned dtz_to_wdl(int cnt50, int dtz)
{
    int wdl = 0;
    if (dtz > 0)
        wdl = (dtz + cnt50 <= 100? 2: 1);
    else if (dtz < 0)
        wdl = (-dtz + cnt50 <= 100? -2: -1);
    return wdl + 2;
}

unsigned tb_probe_root_impl(
    uint64_t white,
    uint64_t black,
    uint64_t kings,
    uint64_t queens,
    uint64_t rooks,
    uint64_t bishops,
    uint64_t knights,
    uint64_t pawns,
    unsigned rule50,
    unsigned ep,
    bool turn,
    unsigned *results)
{
    Pos pos =
    {
        white,
        black,
        kings,
        queens,
        rooks,
        bishops,
        knights,
        pawns,
        (uint8_t)rule50,
        (uint8_t)ep,
        turn
    };
    int dtz;
    if (!is_valid(&pos))
        return TB_RESULT_FAILED;
    TbMove move = probe_root(&pos, &dtz, results);
    if (move == 0)
        return TB_RESULT_FAILED;
    if (move == MOVE_CHECKMATE)
        return TB_RESULT_CHECKMATE;
    if (move == MOVE_STALEMATE)
        return TB_RESULT_STALEMATE;
    unsigned res = 0;
    res = TB_SET_WDL(res, dtz_to_wdl(rule50, dtz));
    res = TB_SET_DTZ(res, (dtz < 0? -dtz: dtz));
    res = TB_SET_FROM(res, move_from(move));
    res = TB_SET_TO(res, move_to(move));
    res = TB_SET_PROMOTES(res, move_promotes(move));
    res = TB_SET_EP(res, is_en_passant(&pos, move));
    return res;
}

int tb_probe_root_dtz(
    uint64_t white,
    uint64_t black,
    uint64_t kings,
    uint64_t queens,
    uint64_t rooks,
    uint64_t bishops,
    uint64_t knights,
    uint64_t pawns,
    unsigned rule50,
    unsigned castling,
    unsigned ep,
    bool     turn,
    bool     hasRepeated,
    bool     useRule50,
    struct TbRootMoves *results) {
    Pos pos =
    {
        white,
        black,
        kings,
        queens,
        rooks,
        bishops,
        knights,
        pawns,
        (uint8_t)rule50,
        (uint8_t)ep,
        turn
    };
    if (castling != 0) return 0;
    return root_probe_dtz(&pos, hasRepeated, useRule50, results);
}

int tb_probe_root_wdl(
    uint64_t white,
    uint64_t black,
    uint64_t kings,
    uint64_t queens,
    uint64_t rooks,
    uint64_t bishops,
    uint64_t knights,
    uint64_t pawns,
    unsigned rule50,
    unsigned castling,
    unsigned ep,
    bool     turn,
    bool     useRule50,
    struct TbRootMoves *results) {
    Pos pos =
    {
        white,
        black,
        kings,
        queens,
        rooks,
        bishops,
        knights,
        pawns,
        (uint8_t)rule50,
        (uint8_t)ep,
        turn
    };
    if (castling != 0) return 0;
    return root_probe_wdl(&pos, useRule50, results);
}

// Given a position, produce a text string of the form KQPvKRP, where
// "KQP" represents the white pieces if flip == false and the black pieces
// if flip == true.
static void prt_str(const Pos *pos, char *str, bool flip)
{
  int color = flip ? BLACK : WHITE;

  for (int pt = KING; pt >= PAWN; pt--)
    for (int i = popcount(pieces_by_type(pos, (Color)color, (PieceType)pt)); i > 0; i--)
      *str++ = piece_to_char[pt];
  *str++ = 'v';
  color ^= 1;
  for (int pt = KING; pt >= PAWN; pt--)
    for (int i = popcount(pieces_by_type(pos, (Color)color, (PieceType)pt)); i > 0; i--)
      *str++ = piece_to_char[pt];
  *str++ = 0;
}

static bool test_tb(const char *str, const char *suffix)
{
  FD fd = open_tb(str, suffix);
  if (fd != FD_ERR) {
    size_t size = file_size(fd);
    close_tb(fd);
    if ((size & 63) != 16) {
      fprintf(stderr, "Incomplete tablebase file %s.%s\n", str, suffix);
      printf("info string Incomplete tablebase file %s.%s\n", str, suffix);
      fd = FD_ERR;
    }
  }
  return fd != FD_ERR;
}

static void *map_tb(const char *name, const char *suffix, map_t *mapping)
{
  FD fd = open_tb(name, suffix);
  if (fd == FD_ERR)
    return NULL;

  void *data = map_file(fd, mapping);
  if (data == NULL) {
    fprintf(stderr, "Could not map %s%s into memory.\n", name, suffix);
    exit(EXIT_FAILURE);
  }

  close_tb(fd);

  return data;
}

static void add_to_hash(struct BaseEntry *ptr, uint64_t key)
{
  int idx;

  idx = key >> (64 - TB_HASHBITS);
  while (tbHash[idx].ptr)
    idx = (idx + 1) & ((1 << TB_HASHBITS) - 1);

  tbHash[idx].key = key;
  tbHash[idx].ptr = ptr;
  atomic_init(&tbHash[idx].error, false);
}

#define pchr(i) piece_to_char[QUEEN - (i)]
#define Swap(a,b) {int tmp=a;a=b;b=tmp;}

static void init_tb(char *str)
{
  if (!test_tb(str, tbSuffix[WDL]))
    return;

  int pcs[16];
  for (int i = 0; i < 16; i++)
    pcs[i] = 0;
  int color = 0;
  for (char *s = str; *s; s++)
    if (*s == 'v')
      color = 8;
    else {
      int piece_type = char_to_piece_type(*s);
      if (piece_type) {
        assert((piece_type | color) < 16);
        pcs[piece_type | color]++;
      }
    }

  uint64_t key = calc_key_from_pcs(pcs, false);
  uint64_t key2 = calc_key_from_pcs(pcs, true);

  bool hasPawns = pcs[W_PAWN] || pcs[B_PAWN];

  struct BaseEntry *be = hasPawns ? &pawnEntry[tbNumPawn++].be
                                  : &pieceEntry[tbNumPiece++].be;
  be->hasPawns = hasPawns;
  be->key = key;
  be->symmetric = key == key2;
  be->num = 0;
  for (int i = 0; i < 16; i++)
    be->num += pcs[i];

  numWdl++;
  numDtm += be->hasDtm = test_tb(str, tbSuffix[DTM]);
  numDtz += be->hasDtz = test_tb(str, tbSuffix[DTZ]);

  if (be->num > TB_MaxCardinality) {
    TB_MaxCardinality = be->num;
  }
  if (be->hasDtm)
    if (be->num > TB_MaxCardinalityDTM) {
      TB_MaxCardinalityDTM = be->num;
    }

#if !defined(__cplusplus) || (__cplusplus < 202002L)
  for (int type = 0; type < 3; type++)
    atomic_init(&be->ready[type], false);
#endif

  if (!be->hasPawns) {
    int j = 0;
    for (int i = 0; i < 16; i++)
      if (pcs[i] == 1) j++;
    be->kk_enc = j == 2;
  } else {
    be->pawns[0] = pcs[W_PAWN];
    be->pawns[1] = pcs[B_PAWN];
    if (pcs[B_PAWN] && (!pcs[W_PAWN] || pcs[W_PAWN] > pcs[B_PAWN]))
      Swap(be->pawns[0], be->pawns[1]);
  }

  add_to_hash(be, key);
  if (key != key2)
    add_to_hash(be, key2);
}

#define PIECE(x) ((struct PieceEntry *)(x))
#define PAWN(x) ((struct PawnEntry *)(x))

int num_tables(struct BaseEntry *be, const int type)
{
  return be->hasPawns ? type == DTM ? 6 : 4 : 1;
}

struct EncInfo *first_ei(struct BaseEntry *be, const int type)
{
  return  be->hasPawns
        ? &PAWN(be)->ei[type == WDL ? 0 : type == DTM ? 8 : 20]
        : &PIECE(be)->ei[type == WDL ? 0 : type == DTM ? 2 : 4];
}

static void free_tb_entry(struct BaseEntry *be)
{
  for (int type = 0; type < 3; type++) {
    if (atomic_load_explicit(&be->ready[type], memory_order_relaxed)) {
      unmap_file((void*)(be->data[type]), be->mapping[type]);
      int num = num_tables(be, type);
      struct EncInfo *ei = first_ei(be, type);
      for (int t = 0; t < num; t++) {
        free(ei[t].precomp);
        if (type != DTZ)
          free(ei[num + t].precomp);
      }
      atomic_store_explicit(&be->ready[type], false, memory_order_relaxed);
    }
  }
}

bool tb_init(const char *path)
{
  if (!initialized) {
    init_indices();
    king_attacks_init();
    knight_attacks_init();
    bishop_attacks_init();
    rook_attacks_init();
    pawn_attacks_init();
    initialized = 1;
  }

  // if pathString is set, we need to clean up first.
  if (pathString) {
    free(pathString);
    free(paths);

    for (int i = 0; i < tbNumPiece; i++)
      free_tb_entry((struct BaseEntry *)&pieceEntry[i]);
    for (int i = 0; i < tbNumPawn; i++)
      free_tb_entry((struct BaseEntry *)&pawnEntry[i]);

    LOCK_DESTROY(tbMutex);

    pathString = NULL;
    numWdl = numDtm = numDtz = 0;
  }

  TB_LARGEST = 0;

  // if path is an empty string or equals "<empty>", we are done.
  const char *p = path;
  if (strlen(p) == 0 || !strcmp(p, "<empty>")) {
    return true;
  }

  pathString = (char*)malloc(strlen(p) + 1);
  strcpy(pathString, p);
  numPaths = 0;
  for (int i = 0;; i++) {
    if (pathString[i] != SEP_CHAR)
      numPaths++;
    while (pathString[i] && pathString[i] != SEP_CHAR)
      i++;
    if (!pathString[i]) break;
    pathString[i] = 0;
  }
  paths = (char**)malloc(numPaths * sizeof(*paths));
  for (int i = 0, j = 0; i < numPaths; i++) {
    while (!pathString[j]) j++;
    paths[i] = &pathString[j];
    while (pathString[j]) j++;
  }

  LOCK_INIT(tbMutex);

  tbNumPiece = tbNumPawn = 0;
  TB_MaxCardinality = TB_MaxCardinalityDTM = 0;

  if (!pieceEntry) {
#if defined(__cplusplus) && (__cplusplus >= 202002L)
    /* Fix crash with -std=c++20 by being consistent with BaseEntry initialization:

        #if __cplusplus >= 202002L
            atomic<bool> ready[3]{false, false, false};
        #else
            ...

        C++ initialization does not work with malloc.
        Also: with new, if the allocation fails we get std::bad_alloc exception,
        which is better library behavior than just calling exit.
     */

    pieceEntry = new PieceEntry[TB_MAX_PIECE]();
    pawnEntry = new PawnEntry[TB_MAX_PAWN]();
#else
    pieceEntry = (struct PieceEntry*)malloc(TB_MAX_PIECE * sizeof(*pieceEntry));
    pawnEntry = (struct PawnEntry*)malloc(TB_MAX_PAWN * sizeof(*pawnEntry));
#endif
    if (!pieceEntry || !pawnEntry) {
      fprintf(stderr, "Out of memory.\n");
      exit(EXIT_FAILURE);
    }
  }

  for (int i = 0; i < (1 << TB_HASHBITS); i++) {
    tbHash[i].key = 0;
    tbHash[i].ptr = NULL;
  }

  char str[16];
  int i, j, k, l, m;

  for (i = 0; i < 5; i++) {
    snprintf(str, 16, "K%cvK", pchr(i));
    init_tb(str);
  }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++) {
      snprintf(str, 16, "K%cvK%c", pchr(i), pchr(j));
      init_tb(str);
    }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++) {
      snprintf(str, 16, "K%c%cvK", pchr(i), pchr(j));
      init_tb(str);
    }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = 0; k < 5; k++) {
        snprintf(str, 16, "K%c%cvK%c", pchr(i), pchr(j), pchr(k));
        init_tb(str);
      }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++) {
        snprintf(str, 16, "K%c%c%cvK", pchr(i), pchr(j), pchr(k));
        init_tb(str);
      }

  // 6- and 7-piece TBs make sense only with a 64-bit address space
  if (sizeof(size_t) < 8 || TB_PIECES < 6)
    goto finished;

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = i; k < 5; k++)
        for (l = (i == k) ? j : k; l < 5; l++) {
          snprintf(str, 16, "K%c%cvK%c%c", pchr(i), pchr(j), pchr(k), pchr(l));
          init_tb(str);
        }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = 0; l < 5; l++) {
          snprintf(str, 16, "K%c%c%cvK%c", pchr(i), pchr(j), pchr(k), pchr(l));
          init_tb(str);
        }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = k; l < 5; l++) {
          snprintf(str, 16, "K%c%c%c%cvK", pchr(i), pchr(j), pchr(k), pchr(l));
          init_tb(str);
        }

  if (TB_PIECES < 7)
    goto finished;

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = k; l < 5; l++)
          for (m = l; m < 5; m++) {
            snprintf(str, 16, "K%c%c%c%c%cvK", pchr(i), pchr(j), pchr(k), pchr(l), pchr(m));
            init_tb(str);
          }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = k; l < 5; l++)
          for (m = 0; m < 5; m++) {
            snprintf(str, 16, "K%c%c%c%cvK%c", pchr(i), pchr(j), pchr(k), pchr(l), pchr(m));
            init_tb(str);
          }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = 0; l < 5; l++)
          for (m = l; m < 5; m++) {
            snprintf(str, 16, "K%c%c%cvK%c%c", pchr(i), pchr(j), pchr(k), pchr(l), pchr(m));
            init_tb(str);
          }

finished:
  /* TBD - assumes UCI
  printf("info string Found %d WDL, %d DTM and %d DTZ tablebase files.\n",
      numWdl, numDtm, numDtz);
  fflush(stdout);
  */
  // Set TB_LARGEST, for backward compatibility with pre-7-man Fathom
  TB_LARGEST = (unsigned)TB_MaxCardinality;
  if ((unsigned)TB_MaxCardinalityDTM > TB_LARGEST) {
    TB_LARGEST = TB_MaxCardinalityDTM;
  }
  return true;
}

void tb_free(void)
{
  tb_init("");
#if defined __cplusplus && __cplusplus >= 202002L
  delete[] pieceEntry;
  delete[] pawnEntry;
#else
  free(pieceEntry);
  free(pawnEntry);
#endif
  pieceEntry = NULL;
  pawnEntry = NULL;
}

static const int8_t OffDiag[] = {
  0,-1,-1,-1,-1,-1,-1,-1,
  1, 0,-1,-1,-1,-1,-1,-1,
  1, 1, 0,-1,-1,-1,-1,-1,
  1, 1, 1, 0,-1,-1,-1,-1,
  1, 1, 1, 1, 0,-1,-1,-1,
  1, 1, 1, 1, 1, 0,-1,-1,
  1, 1, 1, 1, 1, 1, 0,-1,
  1, 1, 1, 1, 1, 1, 1, 0
};

static const uint8_t Triangle[] = {
  6, 0, 1, 2, 2, 1, 0, 6,
  0, 7, 3, 4, 4, 3, 7, 0,
  1, 3, 8, 5, 5, 8, 3, 1,
  2, 4, 5, 9, 9, 5, 4, 2,
  2, 4, 5, 9, 9, 5, 4, 2,
  1, 3, 8, 5, 5, 8, 3, 1,
  0, 7, 3, 4, 4, 3, 7, 0,
  6, 0, 1, 2, 2, 1, 0, 6
};

static const uint8_t FlipDiag[] = {
   0,  8, 16, 24, 32, 40, 48, 56,
   1,  9, 17, 25, 33, 41, 49, 57,
   2, 10, 18, 26, 34, 42, 50, 58,
   3, 11, 19, 27, 35, 43, 51, 59,
   4, 12, 20, 28, 36, 44, 52, 60,
   5, 13, 21, 29, 37, 45, 53, 61,
   6, 14, 22, 30, 38, 46, 54, 62,
   7, 15, 23, 31, 39, 47, 55, 63
};

static const uint8_t Lower[] = {
  28,  0,  1,  2,  3,  4,  5,  6,
   0, 29,  7,  8,  9, 10, 11, 12,
   1,  7, 30, 13, 14, 15, 16, 17,
   2,  8, 13, 31, 18, 19, 20, 21,
   3,  9, 14, 18, 32, 22, 23, 24,
   4, 10, 15, 19, 22, 33, 25, 26,
   5, 11, 16, 20, 23, 25, 34, 27,
   6, 12, 17, 21, 24, 26, 27, 35
};

static const uint8_t Diag[] = {
   0,  0,  0,  0,  0,  0,  0,  8,
   0,  1,  0,  0,  0,  0,  9,  0,
   0,  0,  2,  0,  0, 10,  0,  0,
   0,  0,  0,  3, 11,  0,  0,  0,
   0,  0,  0, 12,  4,  0,  0,  0,
   0,  0, 13,  0,  0,  5,  0,  0,
   0, 14,  0,  0,  0,  0,  6,  0,
  15,  0,  0,  0,  0,  0,  0,  7
};

static const uint8_t Flap[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  6, 12, 18, 18, 12,  6,  0,
     1,  7, 13, 19, 19, 13,  7,  1,
     2,  8, 14, 20, 20, 14,  8,  2,
     3,  9, 15, 21, 21, 15,  9,  3,
     4, 10, 16, 22, 22, 16, 10,  4,
     5, 11, 17, 23, 23, 17, 11,  5,
     0,  0,  0,  0,  0,  0,  0,  0  },
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  1,  2,  3,  3,  2,  1,  0,
     4,  5,  6,  7,  7,  6,  5,  4,
     8,  9, 10, 11, 11, 10,  9,  8,
    12, 13, 14, 15, 15, 14, 13, 12,
    16, 17, 18, 19, 19, 18, 17, 16,
    20, 21, 22, 23, 23, 22, 21, 20,
     0,  0,  0,  0,  0,  0,  0,  0  }
};

static const uint8_t PawnTwist[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 35, 23, 11, 10, 22, 34, 46,
    45, 33, 21,  9,  8, 20, 32, 44,
    43, 31, 19,  7,  6, 18, 30, 42,
    41, 29, 17,  5,  4, 16, 28, 40,
    39, 27, 15,  3,  2, 14, 26, 38,
    37, 25, 13,  1,  0, 12, 24, 36,
     0,  0,  0,  0,  0,  0,  0,  0 },
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 45, 43, 41, 40, 42, 44, 46,
    39, 37, 35, 33, 32, 34, 36, 38,
    31, 29, 27, 25, 24, 26, 28, 30,
    23, 21, 19, 17, 16, 18, 20, 22,
    15, 13, 11,  9,  8, 10, 12, 14,
     7,  5,  3,  1,  0,  2,  4,  6,
     0,  0,  0,  0,  0,  0,  0,  0 }
};

static const int16_t KKIdx[10][64] = {
  { -1, -1, -1,  0,  1,  2,  3,  4,
    -1, -1, -1,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33,
    34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 53, 54, 55, 56, 57 },
  { 58, -1, -1, -1, 59, 60, 61, 62,
    63, -1, -1, -1, 64, 65, 66, 67,
    68, 69, 70, 71, 72, 73, 74, 75,
    76, 77, 78, 79, 80, 81, 82, 83,
    84, 85, 86, 87, 88, 89, 90, 91,
    92, 93, 94, 95, 96, 97, 98, 99,
   100,101,102,103,104,105,106,107,
   108,109,110,111,112,113,114,115},
  {116,117, -1, -1, -1,118,119,120,
   121,122, -1, -1, -1,123,124,125,
   126,127,128,129,130,131,132,133,
   134,135,136,137,138,139,140,141,
   142,143,144,145,146,147,148,149,
   150,151,152,153,154,155,156,157,
   158,159,160,161,162,163,164,165,
   166,167,168,169,170,171,172,173 },
  {174, -1, -1, -1,175,176,177,178,
   179, -1, -1, -1,180,181,182,183,
   184, -1, -1, -1,185,186,187,188,
   189,190,191,192,193,194,195,196,
   197,198,199,200,201,202,203,204,
   205,206,207,208,209,210,211,212,
   213,214,215,216,217,218,219,220,
   221,222,223,224,225,226,227,228 },
  {229,230, -1, -1, -1,231,232,233,
   234,235, -1, -1, -1,236,237,238,
   239,240, -1, -1, -1,241,242,243,
   244,245,246,247,248,249,250,251,
   252,253,254,255,256,257,258,259,
   260,261,262,263,264,265,266,267,
   268,269,270,271,272,273,274,275,
   276,277,278,279,280,281,282,283 },
  {284,285,286,287,288,289,290,291,
   292,293, -1, -1, -1,294,295,296,
   297,298, -1, -1, -1,299,300,301,
   302,303, -1, -1, -1,304,305,306,
   307,308,309,310,311,312,313,314,
   315,316,317,318,319,320,321,322,
   323,324,325,326,327,328,329,330,
   331,332,333,334,335,336,337,338 },
  { -1, -1,339,340,341,342,343,344,
    -1, -1,345,346,347,348,349,350,
    -1, -1,441,351,352,353,354,355,
    -1, -1, -1,442,356,357,358,359,
    -1, -1, -1, -1,443,360,361,362,
    -1, -1, -1, -1, -1,444,363,364,
    -1, -1, -1, -1, -1, -1,445,365,
    -1, -1, -1, -1, -1, -1, -1,446 },
  { -1, -1, -1,366,367,368,369,370,
    -1, -1, -1,371,372,373,374,375,
    -1, -1, -1,376,377,378,379,380,
    -1, -1, -1,447,381,382,383,384,
    -1, -1, -1, -1,448,385,386,387,
    -1, -1, -1, -1, -1,449,388,389,
    -1, -1, -1, -1, -1, -1,450,390,
    -1, -1, -1, -1, -1, -1, -1,451 },
  {452,391,392,393,394,395,396,397,
    -1, -1, -1, -1,398,399,400,401,
    -1, -1, -1, -1,402,403,404,405,
    -1, -1, -1, -1,406,407,408,409,
    -1, -1, -1, -1,453,410,411,412,
    -1, -1, -1, -1, -1,454,413,414,
    -1, -1, -1, -1, -1, -1,455,415,
    -1, -1, -1, -1, -1, -1, -1,456 },
  {457,416,417,418,419,420,421,422,
    -1,458,423,424,425,426,427,428,
    -1, -1, -1, -1, -1,429,430,431,
    -1, -1, -1, -1, -1,432,433,434,
    -1, -1, -1, -1, -1,435,436,437,
    -1, -1, -1, -1, -1,459,438,439,
    -1, -1, -1, -1, -1, -1,460,440,
    -1, -1, -1, -1, -1, -1, -1,461 }
};

static const uint8_t FileToFile[] = { 0, 1, 2, 3, 3, 2, 1, 0 };
static const int WdlToMap[5] = { 1, 3, 0, 2, 0 };
static const uint8_t PAFlags[5] = { 8, 0, 0, 0, 4 };

static size_t Binomial[7][64];
static size_t PawnIdx[2][6][24];
static size_t PawnFactorFile[6][4];
static size_t PawnFactorRank[6][6];

static void init_indices(void)
{
  int i, j, k;

  // Binomial[k][n] = Bin(n, k)
  for (i = 0; i < 7; i++)
    for (j = 0; j < 64; j++) {
      size_t f = 1;
      size_t l = 1;
      for (k = 0; k < i; k++) {
        f *= (j - k);
        l *= (k + 1);
      }
      Binomial[i][j] = f / l;
    }

  for (i = 0; i < 6; i++) {
    size_t s = 0;
    for (j = 0; j < 24; j++) {
      PawnIdx[0][i][j] = s;
      s += Binomial[i][PawnTwist[0][(1 + (j % 6)) * 8 + (j / 6)]];
      if ((j + 1) % 6 == 0) {
        PawnFactorFile[i][j / 6] = s;
        s = 0;
      }
    }
  }

  for (i = 0; i < 6; i++) {
    size_t s = 0;
    for (j = 0; j < 24; j++) {
      PawnIdx[1][i][j] = s;
      s += Binomial[i][PawnTwist[1][(1 + (j / 4)) * 8 + (j % 4)]];
      if ((j + 1) % 4 == 0) {
        PawnFactorRank[i][j / 4] = s;
        s = 0;
      }
    }
  }
}

int leading_pawn(int *p, struct BaseEntry *be, const int enc)
{
  for (int i = 1; i < be->pawns[0]; i++)
    if (Flap[enc-1][p[0]] > Flap[enc-1][p[i]])
      Swap(p[0], p[i]);

  return enc == FILE_ENC ? FileToFile[p[0] & 7] : (p[0] - 8) >> 3;
}

size_t encode(int *p, struct EncInfo *ei, struct BaseEntry *be,
    const int enc)
{
  int n = be->num;
  size_t idx;
  int k;

  if (p[0] & 0x04)
    for (int i = 0; i < n; i++)
      p[i] ^= 0x07;

  if (enc == PIECE_ENC) {
    if (p[0] & 0x20)
      for (int i = 0; i < n; i++)
        p[i] ^= 0x38;

    for (int i = 0; i < n; i++)
      if (OffDiag[p[i]]) {
        if (OffDiag[p[i]] > 0 && i < (be->kk_enc ? 2 : 3))
          for (int j = 0; j < n; j++)
            p[j] = FlipDiag[p[j]];
        break;
      }

    if (be->kk_enc) {
      idx = KKIdx[Triangle[p[0]]][p[1]];
      k = 2;
    } else {
      int s1 = (p[1] > p[0]);
      int s2 = (p[2] > p[0]) + (p[2] > p[1]);

      if (OffDiag[p[0]])
        idx = Triangle[p[0]] * 63*62 + (p[1] - s1) * 62 + (p[2] - s2);
      else if (OffDiag[p[1]])
        idx = 6*63*62 + Diag[p[0]] * 28*62 + Lower[p[1]] * 62 + p[2] - s2;
      else if (OffDiag[p[2]])
        idx = 6*63*62 + 4*28*62 + Diag[p[0]] * 7*28 + (Diag[p[1]] - s1) * 28 + Lower[p[2]];
      else
        idx = 6*63*62 + 4*28*62 + 4*7*28 + Diag[p[0]] * 7*6 + (Diag[p[1]] - s1) * 6 + (Diag[p[2]] - s2);
      k = 3;
    }
    idx *= ei->factor[0];
  } else {
    for (int i = 1; i < be->pawns[0]; i++)
      for (int j = i + 1; j < be->pawns[0]; j++)
        if (PawnTwist[enc-1][p[i]] < PawnTwist[enc-1][p[j]])
          Swap(p[i], p[j]);

    k = be->pawns[0];
    idx = PawnIdx[enc-1][k-1][Flap[enc-1][p[0]]];
    for (int i = 1; i < k; i++)
      idx += Binomial[k-i][PawnTwist[enc-1][p[i]]];
    idx *= ei->factor[0];

    // Pawns of other color
    if (be->pawns[1]) {
      int t = k + be->pawns[1];
      for (int i = k; i < t; i++)
        for (int j = i + 1; j < t; j++)
          if (p[i] > p[j]) Swap(p[i], p[j]);
      size_t s = 0;
      for (int i = k; i < t; i++) {
        int sq = p[i];
        int skips = 0;
        for (int j = 0; j < k; j++)
          skips += (sq > p[j]);
        s += Binomial[i - k + 1][sq - skips - 8];
      }
      idx += s * ei->factor[k];
      k = t;
    }
  }

  for (; k < n;) {
    int t = k + ei->norm[k];
    for (int i = k; i < t; i++)
      for (int j = i + 1; j < t; j++)
        if (p[i] > p[j]) Swap(p[i], p[j]);
    size_t s = 0;
    for (int i = k; i < t; i++) {
      int sq = p[i];
      int skips = 0;
      for (int j = 0; j < k; j++)
        skips += (sq > p[j]);
      s += Binomial[i - k + 1][sq - skips];
    }
    idx += s * ei->factor[k];
    k = t;
  }

  return idx;
}

static size_t encode_piece(int *p, struct EncInfo *ei, struct BaseEntry *be)
{
  return encode(p, ei, be, PIECE_ENC);
}

static size_t encode_pawn_f(int *p, struct EncInfo *ei, struct BaseEntry *be)
{
  return encode(p, ei, be, FILE_ENC);
}

static size_t encode_pawn_r(int *p, struct EncInfo *ei, struct BaseEntry *be)
{
  return encode(p, ei, be, RANK_ENC);
}

// Count number of placements of k like pieces on n squares
static size_t subfactor(size_t k, size_t n)
{
  size_t f = n;
  size_t l = 1;
  for (size_t i = 1; i < k; i++) {
    f *= n - i;
    l *= i + 1;
  }

  return f / l;
}

static size_t init_enc_info(struct EncInfo *ei, struct BaseEntry *be,
    uint8_t *tb, int shift, int t, const int enc)
{
  bool morePawns = enc != PIECE_ENC && be->pawns[1] > 0;

  for (int i = 0; i < be->num; i++) {
    ei->pieces[i] = (tb[i + 1 + morePawns] >> shift) & 0x0f;
    ei->norm[i] = 0;
  }

  int order = (tb[0] >> shift) & 0x0f;
  int order2 = morePawns ? (tb[1] >> shift) & 0x0f : 0x0f;

  int k = ei->norm[0] =  enc != PIECE_ENC ? be->pawns[0]
                       : be->kk_enc ? 2 : 3;

  if (morePawns) {
    ei->norm[k] = be->pawns[1];
    k += ei->norm[k];
  }

  for (int i = k; i < be->num; i += ei->norm[i])
    for (int j = i; j < be->num && ei->pieces[j] == ei->pieces[i]; j++)
      ei->norm[i]++;

  int n = 64 - k;
  size_t f = 1;

  for (int i = 0; k < be->num || i == order || i == order2; i++) {
    if (i == order) {
      ei->factor[0] = f;
      f *=  enc == FILE_ENC ? PawnFactorFile[ei->norm[0] - 1][t]
          : enc == RANK_ENC ? PawnFactorRank[ei->norm[0] - 1][t]
          : be->kk_enc ? 462 : 31332;
    } else if (i == order2) {
      ei->factor[ei->norm[0]] = f;
      f *= subfactor(ei->norm[ei->norm[0]], 48 - ei->norm[0]);
    } else {
      ei->factor[k] = f;
      f *= subfactor(ei->norm[k], n);
      n -= ei->norm[k];
      k += ei->norm[k];
    }
  }

  return f;
}

static void calc_symLen(struct PairsData *d, uint32_t s, char *tmp)
{
  uint8_t *w = d->symPat + 3 * s;
  uint32_t s2 = (w[2] << 4) | (w[1] >> 4);
  if (s2 == 0x0fff)
    d->symLen[s] = 0;
  else {
    uint32_t s1 = ((w[1] & 0xf) << 8) | w[0];
    if (!tmp[s1]) calc_symLen(d, s1, tmp);
    if (!tmp[s2]) calc_symLen(d, s2, tmp);
    d->symLen[s] = d->symLen[s1] + d->symLen[s2] + 1;
  }
  tmp[s] = 1;
}

static struct PairsData *setup_pairs(uint8_t **ptr, size_t tb_size,
    size_t *size, uint8_t *flags, int type)
{
  struct PairsData *d;
  uint8_t *data = *ptr;

  *flags = data[0];
  if (data[0] & 0x80) {
    d = (struct PairsData*)malloc(sizeof(struct PairsData));
    d->idxBits = 0;
    d->constValue[0] = type == WDL ? data[1] : 0;
    d->constValue[1] = 0;
    *ptr = data + 2;
    size[0] = size[1] = size[2] = 0;
    return d;
  }

  uint8_t blockSize = data[1];
  uint8_t idxBits = data[2];
  uint32_t realNumBlocks = read_le_u32(data+4);
  uint32_t numBlocks = realNumBlocks + data[3];
  int maxLen = data[8];
  int minLen = data[9];
  int h = maxLen - minLen + 1;
  uint32_t numSyms = (uint32_t)read_le_u16(data + 10 + 2 * h);
  d = (struct PairsData*)malloc(sizeof(struct PairsData) + h * sizeof(uint64_t) + numSyms);
  d->blockSize = blockSize;
  d->idxBits = idxBits;
  d->offset = (uint16_t *)(&data[10]);
  d->symLen = (uint8_t *)d + sizeof(struct PairsData) + h * sizeof(uint64_t);
  d->symPat = &data[12 + 2 * h];
  d->minLen = minLen;
  *ptr = &data[12 + 2 * h + 3 * numSyms + (numSyms & 1)];

  size_t num_indices = (tb_size + (1ULL << idxBits) - 1) >> idxBits;
  size[0] = 6ULL * num_indices;
  size[1] = 2ULL * numBlocks;
  size[2] = (size_t)realNumBlocks << blockSize;

  assert(numSyms < TB_MAX_SYMS);
  char tmp[TB_MAX_SYMS];
  memset(tmp, 0, numSyms);
  for (uint32_t s = 0; s < numSyms; s++)
    if (!tmp[s])
      calc_symLen(d, s, tmp);

  d->base[h - 1] = 0;
  for (int i = h - 2; i >= 0; i--)
    d->base[i] = (d->base[i + 1] + read_le_u16((uint8_t *)(d->offset + i)) - read_le_u16((uint8_t *)(d->offset + i + 1))) / 2;
#ifdef DECOMP64
  for (int i = 0; i < h; i++)
    d->base[i] <<= 64 - (minLen + i);
#else
  for (int i = 0; i < h; i++)
    d->base[i] <<= 32 - (minLen + i);
#endif
  d->offset -= d->minLen;

  return d;
}

static bool init_table(struct BaseEntry *be, const char *str, int type)
{
  uint8_t *data = (uint8_t*)map_tb(str, tbSuffix[type], &be->mapping[type]);
  if (!data) return false;

  if (read_le_u32(data) != tbMagic[type]) {
    fprintf(stderr, "Corrupted table.\n");
    unmap_file((void*)data, be->mapping[type]);
    return false;
  }

  be->data[type] = data;

  bool split = type != DTZ && (data[4] & 0x01);
  if (type == DTM)
    be->dtmLossOnly = data[4] & 0x04;

  data += 5;

  size_t tb_size[6][2];
  int num = num_tables(be, type);
  struct EncInfo *ei = first_ei(be, type);
  int enc = !be->hasPawns ? PIECE_ENC : type != DTM ? FILE_ENC : RANK_ENC;

  for (int t = 0; t < num; t++) {
    tb_size[t][0] = init_enc_info(&ei[t], be, data, 0, t, enc);
    if (split)
      tb_size[t][1] = init_enc_info(&ei[num + t], be, data, 4, t, enc);
    data += be->num + 1 + (be->hasPawns && be->pawns[1]);
  }
  data += (uintptr_t)data & 1;

  size_t size[6][2][3];
  for (int t = 0; t < num; t++) {
    uint8_t flags;
    ei[t].precomp = setup_pairs(&data, tb_size[t][0], size[t][0], &flags, type);
    if (type == DTZ) {
      if (!be->hasPawns)
        PIECE(be)->dtzFlags = flags;
      else
        PAWN(be)->dtzFlags[t] = flags;
    }
    if (split)
      ei[num + t].precomp = setup_pairs(&data, tb_size[t][1], size[t][1], &flags, type);
    else if (type != DTZ)
      ei[num + t].precomp = NULL;
  }

  if (type == DTM && !be->dtmLossOnly) {
    uint16_t *map = (uint16_t *)data;
    *(be->hasPawns ? &PAWN(be)->dtmMap : &PIECE(be)->dtmMap) = map;
    uint16_t (*mapIdx)[2][2] = be->hasPawns ? &PAWN(be)->dtmMapIdx[0]
                                             : &PIECE(be)->dtmMapIdx;
    for (int t = 0; t < num; t++) {
      for (int i = 0; i < 2; i++) {
        mapIdx[t][0][i] = (uint16_t)(data + 1 - (uint8_t*)map);
        data += 2 + 2 * read_le_u16(data);
      }
      if (split) {
        for (int i = 0; i < 2; i++) {
          mapIdx[t][1][i] = (uint16_t)(data + 1 - (uint8_t*)map);
          data += 2 + 2 * read_le_u16(data);
        }
      }
    }
  }

  if (type == DTZ) {
    void *map = data;
    *(be->hasPawns ? &PAWN(be)->dtzMap : &PIECE(be)->dtzMap) = map;
    uint16_t (*mapIdx)[4] = be->hasPawns ? &PAWN(be)->dtzMapIdx[0]
                                          : &PIECE(be)->dtzMapIdx;
    uint8_t *flags = be->hasPawns ? &PAWN(be)->dtzFlags[0]
                                  : &PIECE(be)->dtzFlags;
    for (int t = 0; t < num; t++) {
      if (flags[t] & 2) {
        if (!(flags[t] & 16)) {
          for (int i = 0; i < 4; i++) {
            mapIdx[t][i] = (uint16_t)(data + 1 - (uint8_t *)map);
            data += 1 + data[0];
          }
        } else {
          data += (uintptr_t)data & 0x01;
          for (int i = 0; i < 4; i++) {
            mapIdx[t][i] = (uint16_t)((uint16_t*)data + 1 - (uint16_t *)map);
            data += 2 + 2 * read_le_u16(data);
          }
        }
      }
    }
    data += (uintptr_t)data & 0x01;
  }

  for (int t = 0; t < num; t++) {
    ei[t].precomp->indexTable = data;
    data += size[t][0][0];
    if (split) {
      ei[num + t].precomp->indexTable = data;
      data += size[t][1][0];
    }
  }

  for (int t = 0; t < num; t++) {
    ei[t].precomp->sizeTable = (uint16_t *)data;
    data += size[t][0][1];
    if (split) {
      ei[num + t].precomp->sizeTable = (uint16_t *)data;
      data += size[t][1][1];
    }
  }

  for (int t = 0; t < num; t++) {
    data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~0x3f);
    ei[t].precomp->data = data;
    data += size[t][0][2];
    if (split) {
      data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~0x3f);
      ei[num + t].precomp->data = data;
      data += size[t][1][2];
    }
  }

  if (type == DTM && be->hasPawns)
    PAWN(be)->dtmSwitched =
      calc_key_from_pieces(ei[0].pieces, be->num) != be->key;

  return true;
}

static uint8_t *decompress_pairs(struct PairsData *d, size_t idx)
{
  if (!d->idxBits)
    return d->constValue;

  uint32_t mainIdx = (uint32_t)(idx >> d->idxBits);
  int litIdx = (idx & (((size_t)1 << d->idxBits) - 1)) - ((size_t)1 << (d->idxBits - 1));
  uint32_t block;
  memcpy(&block, d->indexTable + 6 * mainIdx, sizeof(block));
  block = from_le_u32(block);

  uint16_t idxOffset = *(uint16_t *)(d->indexTable + 6 * mainIdx + 4);
  litIdx += from_le_u16(idxOffset);

  if (litIdx < 0)
    while (litIdx < 0)
      litIdx += d->sizeTable[--block] + 1;
  else
    while (litIdx > d->sizeTable[block])
      litIdx -= d->sizeTable[block++] + 1;

  uint32_t *ptr = (uint32_t *)(d->data + ((size_t)block << d->blockSize));

  int m = d->minLen;
  uint16_t *offset = d->offset;
  uint64_t *base = d->base - m;
  uint8_t *symLen = d->symLen;
  uint32_t sym, bitCnt;

#ifdef DECOMP64
  uint64_t code = from_be_u64(*(uint64_t *)ptr);

  ptr += 2;
  bitCnt = 0; // number of "empty bits" in code
  for (;;) {
    int l = m;
    while (code < base[l]) l++;
    sym = from_le_u16(offset[l]);
    sym += (uint32_t)((code - base[l]) >> (64 - l));
    if (litIdx < (int)symLen[sym] + 1) break;
    litIdx -= (int)symLen[sym] + 1;
    code <<= l;
    bitCnt += l;
    if (bitCnt >= 32) {
      bitCnt -= 32;
      uint32_t tmp = from_be_u32(*ptr++);
      code |= (uint64_t)tmp << bitCnt;
    }
  }
#else
  uint32_t next = 0;
  uint32_t data = *ptr++;
  uint32_t code = from_be_u32(data);
  bitCnt = 0; // number of bits in next
  for (;;) {
    int l = m;
    while (code < base[l]) l++;
    sym = offset[l] + ((code - base[l]) >> (32 - l));
    if (litIdx < (int)symLen[sym] + 1) break;
    litIdx -= (int)symLen[sym] + 1;
    code <<= l;
    if (bitCnt < l) {
      if (bitCnt) {
	code |= (next >> (32 - l));
	l -= bitCnt;
      }
      data = *ptr++;
      next = from_be_u32(data);
      bitCnt = 32;
    }
    code |= (next >> (32 - l));
    next <<= l;
    bitCnt -= l;
  }
#endif
  uint8_t *symPat = d->symPat;
  while (symLen[sym] != 0) {
    uint8_t *w = symPat + (3 * sym);
    int s1 = ((w[1] & 0xf) << 8) | w[0];
    if (litIdx < (int)symLen[s1] + 1)
      sym = s1;
    else {
      litIdx -= (int)symLen[s1] + 1;
      sym = (w[2] << 4) | (w[1] >> 4);
    }
  }

  return &symPat[3 * sym];
}

// p[i] is to contain the square 0-63 (A1-H8) for a piece of type
// pc[i] ^ flip, where 1 = white pawn, ..., 14 = black king and pc ^ flip
// flips between white and black if flip == true.
// Pieces of the same type are guaranteed to be consecutive.
inline static int fill_squares(const Pos *pos, uint8_t *pc, bool flip, int mirror, int *p,
    int i)
{
  Color color = ColorOfPiece(pc[i]);
  if (flip) color = (Color)(!(int)color);
  uint64_t bb = pieces_by_type(pos, color, TypeOfPiece(pc[i]));
  unsigned sq;
  do {
    sq = lsb(bb);
    p[i++] = sq ^ mirror;
    bb = poplsb(bb);
  } while (bb);
  return i;
}

static int probe_table(const Pos *pos, int s, int *success, const int type)
{
  // Obtain the position's material-signature key
  uint64_t key = calc_key(pos,false);

  // Test for KvK
  // Note: Cfish has key == 2ULL for KvK but we have 0
  if (type == WDL && key == 0ULL)
    return 0;

  int hashIdx = key >> (64 - TB_HASHBITS);
  while (tbHash[hashIdx].key && tbHash[hashIdx].key != key)
    hashIdx = (hashIdx + 1) & ((1 << TB_HASHBITS) - 1);
  if (!tbHash[hashIdx].ptr || atomic_load_explicit(&tbHash[hashIdx].error, memory_order_relaxed)) {
    *success = 0;
    return 0;
  }

  struct BaseEntry *be = tbHash[hashIdx].ptr;
  if ((type == DTM && !be->hasDtm) || (type == DTZ && !be->hasDtz)) {
    *success = 0;
    return 0;
  }

  // Use double-checked locking to reduce locking overhead
  if (!atomic_load_explicit(&be->ready[type], memory_order_acquire)) {
    LOCK(tbMutex);
    if (atomic_load_explicit(&tbHash[hashIdx].error, memory_order_relaxed)) {
      *success = 0;
      UNLOCK(tbMutex);
      return 0;
    }
    if (!atomic_load_explicit(&be->ready[type], memory_order_relaxed)) {
      char str[16];
      prt_str(pos, str, be->key != key);
      if (!init_table(be, str, type)) {
        atomic_store_explicit(&tbHash[hashIdx].error, true, memory_order_relaxed);
        *success = 0;
        UNLOCK(tbMutex);
        return 0;
      }
      atomic_store_explicit(&be->ready[type], true, memory_order_release);
    }
    UNLOCK(tbMutex);
  }

  bool bside, flip;
  if (!be->symmetric) {
    flip = key != be->key;
    bside = (pos->turn == WHITE) == flip;
    if (type == DTM && be->hasPawns && PAWN(be)->dtmSwitched) {
      flip = !flip;
      bside = !bside;
    }
  } else {
    flip = pos->turn != WHITE;
    bside = false;
  }

  struct EncInfo *ei = first_ei(be, type);
  int p[TB_PIECES];
  size_t idx;
  int t = 0;
  uint8_t flags = 0; // initialize to fix GCC warning

  if (!be->hasPawns) {
    if (type == DTZ) {
      flags = PIECE(be)->dtzFlags;
      if ((flags & 1) != bside && !be->symmetric) {
        *success = -1;
        return 0;
      }
    }
    ei = type != DTZ ? &ei[bside] : ei;
    for (int i = 0; i < be->num;)
      i = fill_squares(pos, ei->pieces, flip, 0, p, i);
    idx = encode_piece(p, ei, be);
  } else {
    int i = fill_squares(pos, ei->pieces, flip, flip ? 0x38 : 0, p, 0);
    t = leading_pawn(p, be, type != DTM ? FILE_ENC : RANK_ENC);
    if (type == DTZ) {
      flags = PAWN(be)->dtzFlags[t];
      if ((flags & 1) != bside && !be->symmetric) {
        *success = -1;
        return 0;
      }
    }
    ei =  type == WDL ? &ei[t + 4 * bside]
        : type == DTM ? &ei[t + 6 * bside] : &ei[t];
    while (i < be->num)
      i = fill_squares(pos, ei->pieces, flip, flip ? 0x38 : 0, p, i);
    idx = type != DTM ? encode_pawn_f(p, ei, be) : encode_pawn_r(p, ei, be);
  }

  uint8_t *w = decompress_pairs(ei->precomp, idx);

  if (type == WDL)
    return (int)w[0] - 2;

  int v = w[0] + ((w[1] & 0x0f) << 8);

  if (type == DTM) {
    if (!be->dtmLossOnly)
      v = (int)from_le_u16(be->hasPawns
                       ? PAWN(be)->dtmMap[PAWN(be)->dtmMapIdx[t][bside][s] + v]
                        : PIECE(be)->dtmMap[PIECE(be)->dtmMapIdx[bside][s] + v]);
  } else {
    if (flags & 2) {
      int m = WdlToMap[s + 2];
      if (!(flags & 16))
        v =  be->hasPawns
           ? ((uint8_t *)PAWN(be)->dtzMap)[PAWN(be)->dtzMapIdx[t][m] + v]
           : ((uint8_t *)PIECE(be)->dtzMap)[PIECE(be)->dtzMapIdx[m] + v];
      else
        v = (int)from_le_u16(be->hasPawns
                         ? ((uint16_t *)PAWN(be)->dtzMap)[PAWN(be)->dtzMapIdx[t][m] + v]
                          : ((uint16_t *)PIECE(be)->dtzMap)[PIECE(be)->dtzMapIdx[m] + v]);
    }
    if (!(flags & PAFlags[s + 2]) || (s & 1))
      v *= 2;
  }

  return v;
}

static int probe_wdl_table(const Pos *pos, int *success)
{
  return probe_table(pos, 0, success, WDL);
}

static int probe_dtm_table(const Pos *pos, int won, int *success)
{
  return probe_table(pos, won, success, DTM);
}

static int probe_dtz_table(const Pos *pos, int wdl, int *success)
{
  return probe_table(pos, wdl, success, DTZ);
}

// probe_ab() is not called for positions with en passant captures.
static int probe_ab(const Pos *pos, int alpha, int beta, int *success)
{
  assert(pos->ep == 0);

  TbMove moves0[TB_MAX_CAPTURES];
  TbMove *m = moves0;
  // Generate (at least) all legal captures including (under)promotions.
  // It is OK to generate more, as long as they are filtered out below.
  TbMove *end = gen_captures(pos, m);
  for (; m < end; m++) {
    Pos pos1;
    TbMove move = *m;
    if (!is_capture(pos, move))
      continue;
    if (!do_move(&pos1, pos, move))
      continue; // illegal move
    int v = -probe_ab(&pos1, -beta, -alpha, success);
    if (*success == 0) return 0;
    if (v > alpha) {
      if (v >= beta)
        return v;
      alpha = v;
    }
  }

  int v = probe_wdl_table(pos, success);

  return alpha >= v ? alpha : v;
}

// Probe the WDL table for a particular position.
//
// If *success != 0, the probe was successful.
//
// If *success == 2, the position has a winning capture, or the position
// is a cursed win and has a cursed winning capture, or the position
// has an ep capture as only best move.
// This is used in probe_dtz().
//
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
int probe_wdl(Pos *pos, int *success)
{
  *success = 1;

  // Generate (at least) all legal captures including (under)promotions.
  TbMove moves0[TB_MAX_CAPTURES];
  TbMove *m = moves0;
  TbMove *end = gen_captures(pos, m);
  int bestCap = -3, bestEp = -3;

  // We do capture resolution, letting bestCap keep track of the best
  // capture without ep rights and letting bestEp keep track of still
  // better ep captures if they exist.

  for (; m < end; m++) {
    Pos pos1;
    TbMove move = *m;
    if (!is_capture(pos, move))
      continue;
    if (!do_move(&pos1, pos, move))
      continue; // illegal move
    int v = -probe_ab(&pos1, -2, -bestCap, success);
    if (*success == 0) return 0;
    if (v > bestCap) {
      if (v == 2) {
        *success = 2;
        return 2;
      }
      if (!is_en_passant(pos,move))
        bestCap = v;
      else if (v > bestEp)
        bestEp = v;
    }
  }

  int v = probe_wdl_table(pos, success);
  if (*success == 0) return 0;

  // Now max(v, bestCap) is the WDL value of the position without ep rights.
  // If the position without ep rights is not stalemate or no ep captures
  // exist, then the value of the position is max(v, bestCap, bestEp).
  // If the position without ep rights is stalemate and bestEp > -3,
  // then the value of the position is bestEp (and we will have v == 0).

  if (bestEp > bestCap) {
    if (bestEp > v) { // ep capture (possibly cursed losing) is best.
      *success = 2;
      return bestEp;
    }
    bestCap = bestEp;
  }

  // Now max(v, bestCap) is the WDL value of the position unless
  // the position without ep rights is stalemate and bestEp > -3.

  if (bestCap >= v) {
    // No need to test for the stalemate case here: either there are
    // non-ep captures, or bestCap == bestEp >= v anyway.
    *success = 1 + (bestCap > 0);
    return bestCap;
  }

  // Now handle the stalemate case.
  if (bestEp > -3 && v == 0) {
    TbMove moves[TB_MAX_MOVES];
    TbMove *end2 = gen_moves(pos, moves);
    // Check for stalemate in the position with ep captures.
    for (m = moves; m < end2; m++) {
      if (!is_en_passant(pos,*m) && legal_move(pos, *m)) break;
    }
    if (m == end2 && !is_check(pos)) {
      // stalemate score from tb (w/o e.p.), but an en-passant capture
      // is possible.
      *success = 2;
      return bestEp;
    }
  }
  // Stalemate / en passant not an issue, so v is the correct value.

  return v;
}

#if 0
// This will not be called for positions with en passant captures
static Value probe_dtm_dc(const Pos *pos, int won, int *success)
{
  assert(ep_square() == 0);

  Value v, bestCap = -TB_VALUE_INFINITE;

  TbMove moves0[TB_MAX_CAPTURES];
  TbMove *end, *m = moves0;

  // Generate at least all legal captures including (under)promotions
  end = gen_captures(pos, m);
  Pos pos1;
  for (; m < end; m++) {
    TbMove move = m->move;
    if (!is_capture(pos, move))
      continue;
    if (!do_move(&pos1, pos, move))
      continue;
    if (!won)
      v = -probe_dtm_dc(&pos1, 1, success) + 1;
    else if (probe_ab(&pos1, -1, 0, success) < 0 && *success)
      v = -probe_dtm_dc(&pos1, 0, success) - 1;
    else
      v = -TB_VALUE_INFINITE;
    bestCap = max(bestCap,v);
    if (*success == 0) return 0;
  }

  int dtm = probe_dtm_table(pos, won, success);
  v = won ? TB_VALUE_MATE - 2 * dtm + 1 : -TB_VALUE_MATE + 2 * dtm;

  return max(bestCap,v);
}
#endif

static Value probe_dtm_win(const Pos *pos, int *success);

// Probe a position known to lose by probing the DTM table and looking
// at captures.
static Value probe_dtm_loss(const Pos *pos, int *success)
{
  Value v, best = -TB_VALUE_INFINITE, numEp = 0;

  TbMove moves0[TB_MAX_CAPTURES];
  // Generate at least all legal captures including (under)promotions
  TbMove *end, *m = moves0;
  end = gen_captures(pos, m);

  Pos pos1;
  for (; m < end; m++) {
    TbMove move = *m;
    if (!is_capture(pos, move) || !legal_move(pos, move))
      continue;
    if (is_en_passant(pos, move))
      numEp++;
    do_move(&pos1, pos, move);
    v = -probe_dtm_win(&pos1, success) + 1;
    if (v > best) {
      best = v;
    }
    if (*success == 0)
      return 0;
  }

  // If there are en passant captures, the position without ep rights
  // may be a stalemate. If it is, we must avoid probing the DTM table.
  if (numEp != 0 && gen_legal(pos, m) == m + numEp)
    return best;

  v = -TB_VALUE_MATE + 2 * probe_dtm_table(pos, 0, success);
  return best > v ? best : v;
}

static Value probe_dtm_win(const Pos *pos, int *success)
{
  Value v, best = -TB_VALUE_INFINITE;

  // Generate all moves
  TbMove moves0[TB_MAX_CAPTURES];
  TbMove *m = moves0;
  TbMove *end = gen_moves(pos, m);
  // Perform a 1-ply search
  Pos pos1;
  for (; m < end; m++) {
    TbMove move = *m;
    if (do_move(&pos1, pos, move)) {
      // not legal
      continue;
    }
    if ((pos1.ep > 0  ? probe_wdl(&pos1, success)
         : probe_ab(&pos1, -1, 0, success)) < 0
        && *success)
      v = -probe_dtm_loss(&pos1, success) - 1;
    else
      v = -TB_VALUE_INFINITE;
    if (v > best) {
      best = v;
    }
    if (*success == 0) return 0;
  }

  return best;
}

Value TB_probe_dtm(const Pos *pos, int wdl, int *success)
{
  assert(wdl != 0);

  *success = 1;

  return wdl > 0 ? probe_dtm_win(pos, success)
                 : probe_dtm_loss(pos, success);
}

#if 0
// To be called only for non-drawn positions.
Value TB_probe_dtm2(const Pos *pos, int wdl, int *success)
{
  assert(wdl != 0);

  *success = 1;
  Value v, bestCap = -TB_VALUE_INFINITE, bestEp = -TB_VALUE_INFINITE;

  TbMove moves0[TB_MAX_CAPTURES];
  TbMove *end, *m = moves0;

  // Generate at least all legal captures including (under)promotions
  end = gen_captures(pos, m);
  Pos pos0 = *pos;

  // Resolve captures, letting bestCap keep track of the best non-ep
  // capture and letting bestEp keep track of the best ep capture.
  Pos pos1;
  for (; m < end; m++) {
    TbMove move = *m;
    if (!is_capture(pos, move))
      continue;
    if (!do_move(&pos1, pos, move))
      continue;
    if (wdl < 0)
      v = -probe_dtm_dc(&pos1, 1, success) + 1;
    else if (probe_ab(&pos1, -1, 0, success) < 0 && *success)
      v = -probe_dtm_dc(&pos1, 0, success) - 1;
    else
      v = -TB_VALUE_MATE;
    if (is_en_passant(&pos1, move))
      bestEp = max(bestEp,v);
    else
      bestCap = max(bestCap,v);
    if (*success == 0)
      return 0;
  }

  // If there are en passant captures, we have to determine the WDL value
  // for the position without ep rights if it might be different.
  if (bestEp > -TB_VALUE_INFINITE && (bestEp < 0 || bestCap < 0)) {
    assert(ep_square() != 0);
    uint8_t s = pos->st->epSquare;
    pos->st->epSquare = 0;
    wdl = probe_ab(pos, -2, 2, success);
    pos->st->epSquare = s;
    if (*success == 0)
      return 0;
    if (wdl == 0)
      return bestEp;
  }

  bestCap = max(bestCap,v);
  int dtm = probe_dtm_table(pos, wdl > 0, success);
  v = wdl > 0 ? TB_VALUE_MATE - 2 * dtm + 1 : -TB_VALUE_MATE + 2 * dtm;
  return max(bestCap,v);
}
#endif

static int WdlToDtz[] = { -1, -101, 0, 101, 1 };

// Probe the DTZ table for a particular position.
// If *success != 0, the probe was successful.
// The return value is from the point of view of the side to move:
//         n < -100 : loss, but draw under 50-move rule
// -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
//         0        : draw
//     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
//   100 < n        : win, but draw under 50-move rule
//
// If the position mate, -1 is returned instead of 0.
//
// The return value n can be off by 1: a return value -n can mean a loss
// in n+1 ply and a return value +n can mean a win in n+1 ply. This
// cannot happen for tables with positions exactly on the "edge" of
// the 50-move rule.
//
// This means that if dtz > 0 is returned, the position is certainly
// a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
// picks moves that preserve dtz + 50-move-counter <= 99.
//
// If n = 100 immediately after a capture or pawn move, then the position
// is also certainly a win, and during the whole phase until the next
// capture or pawn move, the inequality to be preserved is
// dtz + 50-movecounter <= 100.
//
// In short, if a move is available resulting in dtz + 50-move-counter <= 99,
// then do not accept moves leading to dtz + 50-move-counter == 100.
//
int probe_dtz(Pos *pos, int *success)
{
  int wdl = probe_wdl(pos, success);
  if (*success == 0) return 0;

  // If draw, then dtz = 0.
  if (wdl == 0) return 0;

  // Check for winning capture or en passant capture as only best move.
  if (*success == 2)
    return WdlToDtz[wdl + 2];

  TbMove moves[TB_MAX_MOVES];
  TbMove *m = moves, *end = NULL;
  Pos pos1;

  // If winning, check for a winning pawn move.
  if (wdl > 0) {
    // Generate at least all legal non-capturing pawn moves
    // including non-capturing promotions.
    // (The following call in fact generates all moves.)
    end = gen_legal(pos, moves);

    for (m = moves; m < end; m++) {
      TbMove move = *m;
      if (type_of_piece_moved(pos,move) != PAWN || is_capture(pos, move))
         continue;
      if (!do_move(&pos1, pos, move))
         continue; // not legal
      int v = -probe_wdl(&pos1, success);
      if (*success == 0) return 0;
      if (v == wdl) {
        assert(wdl < 3);
        return WdlToDtz[wdl + 2];
      }
    }
  }

  // If we are here, we know that the best move is not an ep capture.
  // In other words, the value of wdl corresponds to the WDL value of
  // the position without ep rights. It is therefore safe to probe the
  // DTZ table with the current value of wdl.

  int dtz = probe_dtz_table(pos, wdl, success);
  if (*success >= 0)
    return WdlToDtz[wdl + 2] + ((wdl > 0) ? dtz : -dtz);

  // *success < 0 means we need to probe DTZ for the other side to move.
  int best;
  if (wdl > 0) {
    best = INT32_MAX;
  } else {
    // If (cursed) loss, the worst case is a losing capture or pawn move
    // as the "best" move, leading to dtz of -1 or -101.
    // In case of mate, this will cause -1 to be returned.
    best = WdlToDtz[wdl + 2];
    // If wdl < 0, we still have to generate all moves.
    end = gen_moves(pos, m);
  }
  assert(end != NULL);

  for (m = moves; m < end; m++) {
    TbMove move = *m;
    // We can skip pawn moves and captures.
    // If wdl > 0, we already caught them. If wdl < 0, the initial value
    // of best already takes account of them.
    if (is_capture(pos, move) || type_of_piece_moved(pos, move) == PAWN)
      continue;
    if (!do_move(&pos1, pos, move)) {
      // move was not legal
      continue;
    }
    int v = -probe_dtz(&pos1, success);
    // Check for the case of mate in 1
    if (v == 1 && is_mate(&pos1))
      best = 1;
    else if (wdl > 0) {
      if (v > 0 && v + 1 < best)
        best = v + 1;
    } else {
      if (v - 1 < best)
        best = v - 1;
    }
    if (*success == 0) return 0;
  }
  return best;
}

// Use the DTZ tables to rank and score all root moves in the list.
// A return value of 0 means that not all probes were successful.
static int root_probe_dtz(const Pos *pos, bool hasRepeated, bool useRule50, struct TbRootMoves *rm)
{
  int v, success;

  // Obtain 50-move counter for the root position.
  int cnt50 = pos->rule50;

  // The border between draw and win lies at rank 1 or rank 900, depending
  // on whether the 50-move rule is used.
  int bound = useRule50 ? 900 : 1;

  // Probe, rank and score each move.
  TbMove rootMoves[TB_MAX_MOVES];
  TbMove * end = gen_legal(pos,rootMoves);
  rm->size = (unsigned)(end-rootMoves);
  Pos pos1;
  for (unsigned i = 0; i < rm->size; i++) {
    struct TbRootMove *m = &(rm->moves[i]);
    m->move = rootMoves[i];
    do_move(&pos1, pos, m->move);

    // Calculate dtz for the current move counting from the root position.
    if (pos1.rule50 == 0) {
      // If the move resets the 50-move counter, dtz is -101/-1/0/1/101.
      v = -probe_wdl(&pos1, &success);
      assert(v < 3);
      v = WdlToDtz[v + 2];
    } else {
      // Otherwise, take dtz for the new position and correct by 1 ply.
      v = -probe_dtz(&pos1, &success);
      if (v > 0) v++;
      else if (v < 0) v--;
    }
    // Make sure that a mating move gets value 1.
    if (v == 2 && is_mate(&pos1)) {
      v = 1;
    }

    if (!success) return 0;

    // Better moves are ranked higher. Guaranteed wins are ranked equally.
    // Losing moves are ranked equally unless a 50-move draw is in sight.
    // Note that moves ranked 900 have dtz + cnt50 == 100, which in rare
    // cases may be insufficient to win as dtz may be one off (see the
    // comments before TB_probe_dtz()).
    int r =  v > 0 ? (v + cnt50 <= 99 && !hasRepeated ? 1000 : 1000 - (v + cnt50))
           : v < 0 ? (-v * 2 + cnt50 < 100 ? -1000 : -1000 + (-v + cnt50))
           : 0;
    m->tbRank = r;

    // Determine the score to be displayed for this move. Assign at least
    // 1 cp to cursed wins and let it grow to 49 cp as the position gets
    // closer to a real win.
    m->tbScore =  r >= bound ? TB_VALUE_MATE - TB_MAX_MATE_PLY - 1
                : r >  0     ? max( 3, r - 800) * TB_VALUE_PAWN / 200
                : r == 0     ? TB_VALUE_DRAW
                : r > -bound ? min(-3, r + 800) * TB_VALUE_PAWN / 200
                :             -TB_VALUE_MATE + TB_MAX_MATE_PLY + 1;
  }
  return 1;
}

// Use the WDL tables to rank all root moves in the list.
// This is a fallback for the case that some or all DTZ tables are missing.
// A return value of 0 means that not all probes were successful.
int root_probe_wdl(const Pos *pos, bool useRule50, struct TbRootMoves *rm)
{
  static int WdlToRank[] = { -1000, -899, 0, 899, 1000 };
  static Value WdlToValue[] = {
    -TB_VALUE_MATE + TB_MAX_MATE_PLY + 1,
    TB_VALUE_DRAW - 2,
    TB_VALUE_DRAW,
    TB_VALUE_DRAW + 2,
    TB_VALUE_MATE - TB_MAX_MATE_PLY - 1
  };

  int v, success;

  // Probe, rank and score each move.
  TbMove moves[TB_MAX_MOVES];
  TbMove *end = gen_legal(pos,moves);
  rm->size = (unsigned)(end-moves);
  Pos pos1;
  for (unsigned i = 0; i < rm->size; i++) {
    struct TbRootMove *m = &rm->moves[i];
    m->move = moves[i];
    do_move(&pos1, pos, m->move);
    v = -probe_wdl(&pos1, &success);
    if (!success) return 0;
    if (!useRule50)
      v = v > 0 ? 2 : v < 0 ? -2 : 0;
    m->tbRank = WdlToRank[v + 2];
    m->tbScore = WdlToValue[v + 2];
  }

  return 1;
}

// Use the DTM tables to find mate scores.
// Either DTZ or WDL must have been probed successfully earlier.
// A return value of 0 means that not all probes were successful.
#if defined(__cplusplus) && __cplusplus >= 201703L
[[maybe_unused]]
#endif
int root_probe_dtm(const Pos *pos, struct TbRootMoves *rm)
{
  int success;
  Value tmpScore[TB_MAX_MOVES];

  // Probe each move.
  for (unsigned i = 0; i < rm->size; i++) {
    Pos pos1;
    struct TbRootMove *m = &rm->moves[i];

    // Use tbScore to find out if the position is won or lost.
    int wdl =  m->tbScore >  TB_VALUE_PAWN ?  2
             : m->tbScore < -TB_VALUE_PAWN ? -2 : 0;

    if (wdl == 0)
      tmpScore[i] = 0;
    else {
      // Probe and adjust mate score by 1 ply.
      do_move(&pos1, pos, m->pv[0]);
      Value v = -TB_probe_dtm(&pos1, -wdl, &success);
      tmpScore[i] = wdl > 0 ? v - 1 : v + 1;
      if (success == 0)
        return 0;
    }
  }

  // All probes were successful. Now adjust TB scores and ranks.
  for (unsigned i = 0; i < rm->size; i++) {
    struct TbRootMove *m = &rm->moves[i];

    m->tbScore = tmpScore[i];

    // Let rank correspond to mate score, except for critical moves
    // ranked 900, which we rank below all other mates for safety.
    // By ranking mates above 1000 or below -1000, we let the search
    // know it need not search those moves.
    m->tbRank = m->tbRank == 900 ? 1001 : m->tbScore;
  }

  return 1;
}

// Use the DTM tables to complete a PV with mate score.
#if defined(__cplusplus) && __cplusplus >= 201703L
[[maybe_unused]]
#endif
void tb_expand_mate(Pos *pos, struct TbRootMove *move, Value moveScore, unsigned cardinalityDTM)
{
  int success = 1, chk = 0;
  Value v = moveScore, w = 0;
  int wdl = v > 0 ? 2 : -2;

  if (move->pvSize == TB_MAX_PLY)
    return;

  Pos root = *pos;
  // First get to the end of the incomplete PV.
  for (unsigned i = 0; i < move->pvSize; i++) {
    v = v > 0 ? -v - 1 : -v + 1;
    wdl = -wdl;
    Pos pos0 = *pos;
    do_move(pos, &pos0, move->pv[i]);
  }

  // Now try to expand until the actual mate.
  if (popcount(pos->white | pos->black) <= cardinalityDTM) {
    while (v != -TB_VALUE_MATE && move->pvSize < TB_MAX_PLY) {
      v = v > 0 ? -v - 1 : -v + 1;
      wdl = -wdl;
      TbMove moves[TB_MAX_MOVES];
      TbMove *end = gen_legal(pos, moves);
      TbMove *m = moves;
      for (; m < end; m++) {
        Pos pos1;
        do_move(&pos1, pos, *m);
        if (wdl < 0)
          chk = probe_wdl(&pos1, &success); // verify that move wins
        w =  success && (wdl > 0 || chk < 0)
           ? TB_probe_dtm(&pos1, wdl, &success)
           : 0;
        if (!success || v == w) break;
      }
      if (!success || v != w)
        break;
      move->pv[move->pvSize++] = *m;
      Pos pos0 = *pos;
      do_move(pos, &pos0, *m);
    }
  }
  // Get back to the root position.
  *pos = root;
}

static const int wdl_to_dtz[] =
{
    -1, -101, 0, 101, 1
};

// This supports the original Fathom root probe API
static uint16_t probe_root(Pos *pos, int *score, unsigned *results)
{
    int success;
    int dtz = probe_dtz(pos, &success);
    if (!success)
        return 0;

    int16_t scores[MAX_MOVES];
    uint16_t moves0[MAX_MOVES];
    uint16_t *moves = moves0;
    uint16_t *end = gen_moves(pos, moves);
    size_t len = end - moves;
    size_t num_draw = 0;
    unsigned j = 0;
    for (unsigned i = 0; i < len; i++)
    {
        Pos pos1;
        if (!do_move(&pos1, pos, moves[i]))
        {
            scores[i] = SCORE_ILLEGAL;
            continue;
        }
        int v = 0;
        //        print_move(pos,moves[i]);
        if (dtz > 0 && is_mate(&pos1))
            v = 1;
        else
        {
            if (pos1.rule50 != 0)
            {
                v = -probe_dtz(&pos1, &success);
                if (v > 0)
                    v++;
                else if (v < 0)
                    v--;
            }
            else
            {
                v = -probe_wdl(&pos1, &success);
                v = wdl_to_dtz[v + 2];
            }
        }
        num_draw += (v == 0);
        if (!success)
            return 0;
        scores[i] = v;
        if (results != NULL)
        {
            unsigned res = 0;
            res = TB_SET_WDL(res, dtz_to_wdl(pos->rule50, v));
            res = TB_SET_FROM(res, move_from(moves[i]));
            res = TB_SET_TO(res, move_to(moves[i]));
            res = TB_SET_PROMOTES(res, move_promotes(moves[i]));
            res = TB_SET_EP(res, is_en_passant(pos, moves[i]));
            res = TB_SET_DTZ(res, (v < 0? -v: v));
            results[j++] = res;
        }
    }
    if (results != NULL)
        results[j++] = TB_RESULT_FAILED;
    if (score != NULL)
        *score = dtz;

    // Now be a bit smart about filtering out moves.
    if (dtz > 0)        // winning (or 50-move rule draw)
    {
        int best = BEST_NONE;
        uint16_t best_move = 0;
        for (unsigned i = 0; i < len; i++)
        {
            int v = scores[i];
            if (v == SCORE_ILLEGAL)
                continue;
            if (v > 0 && v < best)
            {
                best = v;
                best_move = moves[i];
            }
        }
        return (best == BEST_NONE? 0: best_move);
    }
    else if (dtz < 0)   // losing (or 50-move rule draw)
    {
        int best = 0;
        uint16_t best_move = 0;
        for (unsigned i = 0; i < len; i++)
        {
            int v = scores[i];
            if (v == SCORE_ILLEGAL)
                continue;
            if (v < best)
            {
                best = v;
                best_move = moves[i];
            }
        }
        return (best == 0? MOVE_CHECKMATE: best_move);
    }
    else                // drawing
    {
        // Check for stalemate:
        if (num_draw == 0)
            return MOVE_STALEMATE;

        // Select a "random" move that preserves the draw.
        // Uses calc_key as the PRNG.
        size_t count = calc_key(pos, !pos->turn) % num_draw;
        for (unsigned i = 0; i < len; i++)
        {
            int v = scores[i];
            if (v == SCORE_ILLEGAL)
                continue;
            if (v == 0)
            {
                if (count == 0)
                    return moves[i];
                count--;
            }
        }
        return 0;
    }
}

#ifndef TB_NO_HELPER_API

unsigned tb_pop_count(uint64_t bb)
{
    return popcount(bb);
}

unsigned tb_lsb(uint64_t bb)
{
    return lsb(bb);
}

uint64_t tb_pop_lsb(uint64_t bb)
{
    return poplsb(bb);
}

uint64_t tb_king_attacks(unsigned sq)
{
    return king_attacks(sq);
}

uint64_t tb_queen_attacks(unsigned sq, uint64_t occ)
{
    return queen_attacks(sq, occ);
}

uint64_t tb_rook_attacks(unsigned sq, uint64_t occ)
{
    return rook_attacks(sq, occ);
}

uint64_t tb_bishop_attacks(unsigned sq, uint64_t occ)
{
    return bishop_attacks(sq, occ);
}

uint64_t tb_knight_attacks(unsigned sq)
{
    return knight_attacks(sq);
}

uint64_t tb_pawn_attacks(unsigned sq, bool color)
{
    return pawn_attacks(sq, color);
}

#endif      /* TB_NO_HELPER_API */
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef BEST_NONE
#undef BLACK_BISHOP
#undef BLACK_KING
#undef BLACK_KNIGHT
#undef BLACK_PAWN
#undef BLACK_QUEEN
#undef BLACK_ROOK
#undef BOARD_EDGE
#undef BOARD_FILE_A
#undef BOARD_FILE_EDGE
#undef BOARD_RANK_1
#undef BOARD_RANK_EDGE
#undef DECOMP64
#undef FD
#undef FD_ERR
#undef KEY_KvK
#undef LOCK
#undef LOCK_DESTROY
#undef LOCK_INIT
#undef LOCK_T
#undef MAX_MOVES
#undef MOVE_CHECKMATE
#undef MOVE_STALEMATE
#undef PAWN
#undef PIECE
#undef PRIME_BLACK_BISHOP
#undef PRIME_BLACK_KNIGHT
#undef PRIME_BLACK_PAWN
#undef PRIME_BLACK_QUEEN
#undef PRIME_BLACK_ROOK
#undef PRIME_WHITE_BISHOP
#undef PRIME_WHITE_KNIGHT
#undef PRIME_WHITE_PAWN
#undef PRIME_WHITE_QUEEN
#undef PRIME_WHITE_ROOK
#undef SCORE_ILLEGAL
#undef SEP_CHAR
#undef Swap
#undef UNLOCK
#undef WHITE_BISHOP
#undef WHITE_KING
#undef WHITE_KNIGHT
#undef WHITE_PAWN
#undef WHITE_QUEEN
#undef WHITE_ROOK
#undef anti
#undef anti2board
#undef bishop_attacks
#undef bishop_attacks_init
#undef board
#undef diag
#undef diag2board
#undef do_bb_move
#undef file
#undef file2board
#undef king_attacks
#undef king_attacks_init
#undef knight_attacks
#undef knight_attacks_init
#undef lsb
#undef make_move
#undef max
#undef min
#undef move_from
#undef move_promotes
#undef move_to
#undef pawn_attacks
#undef pawn_attacks_init
#undef pchr
#undef popcount
#undef poplsb
#undef queen_attacks
#undef rank
#undef rank2board
#undef rook_attacks
#undef rook_attacks_init
#undef square
// ---------------------------------------------------------------- end of Fathom

// ---------------------------------------------------------------- 3.0: Syzygy tablebases in the search
// Options: SyzygyPath (folders, ';' on Windows and ':' elsewhere), SyzygyProbeDepth, Syzygy50MoveRule, SyzygyProbeLimit.  Interior nodes are probed (WDL) right after a capture or pawn move
// (fifty-move counter 0, no castling rights); the root is probed with the DTZ tables (the WDL tables if DTZ files are missing) and only the best-ranked moves are searched.
static std::string tbPath = "<empty>";
static int tbProbeDepth = 1, tbProbeLimit = 7;
static bool tbRule50 = true;
static unsigned tbMen = 0;                                   // men of the largest table that was found (0: no tablebases)
static std::atomic<long long> tbHitsTotal(0);                // successful probes of the running search (reported as tbhits)
const int TBWIN = 28000, TB_BOUND = TBWIN - 256;             // a tablebase win scores TBWIN - ply: above every evaluation, below every mate score
static inline unsigned tbProbeWdl(const Pos& p) {
    return fathom::tb_probe_wdl(p.Occ[0], p.Occ[1], p.P[5] | p.P[11], p.P[4] | p.P[10], p.P[3] | p.P[9], p.P[2] | p.P[8], p.P[1] | p.P[7], p.P[0] | p.P[6],
                                0, 0, p.ep >= 0 ? (unsigned)p.ep : 0u, p.side == 0);
}
static bool tbSetPath(const std::string& path) {
    tbMen = 0;
    const std::string pth = (path == "<empty>") ? std::string() : path;
    if (!fathom::tb_init(pth.c_str())) return false;
    tbMen = fathom::TB_LARGEST > 7 ? 0 : fathom::TB_LARGEST; return true;
}
// moves that must not be searched at a root inside the tablebases; false = no information (position outside the tablebases, a probe failed)
static bool tbRootBans(Pos& p, std::vector<int>& bans) {
    if (!tbMen || p.castle || popc(p.All) > (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) return false;
    static fathom::TbRootMoves rm;                                         // 100 KB: not on the stack
    const uint64_t kings = p.P[5] | p.P[11], queens = p.P[4] | p.P[10], rooks = p.P[3] | p.P[9], bishops = p.P[2] | p.P[8], knights = p.P[1] | p.P[7], pawns = p.P[0] | p.P[6];
    const unsigned ep = p.ep >= 0 ? (unsigned)p.ep : 0u; const bool white = p.side == 0;
    int ok = fathom::tb_probe_root_dtz(p.Occ[0], p.Occ[1], kings, queens, rooks, bishops, knights, pawns, (unsigned)p.half, 0, ep, white, false, tbRule50, &rm);
    if (!ok) ok = fathom::tb_probe_root_wdl(p.Occ[0], p.Occ[1], kings, queens, rooks, bishops, knights, pawns, (unsigned)p.half, 0, ep, white, tbRule50, &rm);
    if (!ok || rm.size == 0) return false;
    int bestRank = -2000000000; for (unsigned i = 0; i < rm.size; i++) bestRank = std::max(bestRank, (int)rm.moves[i].tbRank);
    // A won position (rank 1000: every move of that rank still wins before the fifty-move rule): all these moves have the same tablebase score, so the search cannot tell them apart and may
    // shuffle until the fifty-move budget is used up (a won ending of the conversion test took 100 plies).  Keep only the ones with the smallest DTZ; a move that keeps the win and resets
    // the counter (a capture or a pawn move) has DTZ 1, so has a mating move.  Moves with DTZ <= 3 (a mate in two, a capture next move) stay as well, so that the search can still prefer
    // a short mate it sees; they cannot lead to shuffling because the counter resets or the game ends within 3 plies.
    // Needs the DTZ files (tb_probe_root fails without them: then every best-ranked move stays).
    static unsigned dtzRes[TB_MAX_MOVES + 8];
    bool haveDtz = false;
    if (bestRank == 1000) {
        const unsigned r = fathom::tb_probe_root(p.Occ[0], p.Occ[1], kings, queens, rooks, bishops, knights, pawns, (unsigned)p.half, 0, ep, white, dtzRes);
        haveDtz = r != TB_RESULT_FAILED && r != TB_RESULT_CHECKMATE && r != TB_RESULT_STALEMATE;
    }
    int mv[256]; const int n = p.gen(mv, false); int kept = 0; std::vector<int> out;
    int ranked[256], dtzOf[256], nr = 0, minDtz = 1 << 30;             // the moves with the best rank and their DTZ
    for (int i = 0; i < n; i++) {
        const int m = mv[i]; if (!p.legalMove(m)) continue;
        const int from = m & 63, to = (m >> 6) & 63, pr = (m >> 12) & 7, tpr = pr == 4 ? 1 : pr == 3 ? 2 : pr == 2 ? 3 : pr == 1 ? 4 : 0;      // engine: 1 n, 2 b, 3 r, 4 q  ->  Fathom: 1 q, 2 r, 3 b, 4 n
        bool good = false;
        for (unsigned j = 0; j < rm.size && !good; j++)
            good = (int)TB_MOVE_FROM(rm.moves[j].move) == from && (int)TB_MOVE_TO(rm.moves[j].move) == to && (int)TB_MOVE_PROMOTES(rm.moves[j].move) == tpr && (int)rm.moves[j].tbRank == bestRank;
        int d = 1 << 29;
        if (good && haveDtz) {
            bool found = false;
            for (unsigned j = 0; dtzRes[j] != TB_RESULT_FAILED && j < TB_MAX_MOVES && !found; j++)
                if ((int)TB_GET_FROM(dtzRes[j]) == from && (int)TB_GET_TO(dtzRes[j]) == to && (int)TB_GET_PROMOTES(dtzRes[j]) == tpr) { d = (int)TB_GET_DTZ(dtzRes[j]); found = true; }
            if (!found) haveDtz = false;                                  // the two tablebase interfaces disagree about the move list: no DTZ filter
        }
        if (good) { ranked[nr] = m; dtzOf[nr++] = d; minDtz = std::min(minDtz, d); } else out.push_back(m);
    }
    for (int i = 0; i < nr; i++) {
        if (haveDtz && dtzOf[i] > std::max(minDtz, 3)) out.push_back(ranked[i]); else kept++;
    }
    if (kept == 0) return false;                                           // the tables and the move generator disagree: restrict nothing
    bans = out; return true;
}
// ---------------------------------------------------------------- shared search state (Lazy SMP)
const int MAXTH = 8;
#ifndef DEFAULT_THREADS
#define DEFAULT_THREADS 1
#endif
static int numThreads = DEFAULT_THREADS;
Pos gRoot;                     // root position handed to the helper threads
#ifdef KESTREL_TUNE_OPTIONS
#define KVAR static int          // development builds: switches and search constants are variables, the options change them
#else
#define KVAR static const int    // release builds: compile-time constants
#endif
static int lazyMargin = 0;
KVAR nnBlend = 100;
KVAR evalFlags = 1;
static int net2Share = 70;
const int MAXPLY = 128, INF = 32000, MATE = 30000, MATE_BOUND = MATE - 256;
enum { TT_NONE, TT_EXACT, TT_LOWER, TT_UPPER };
struct TTE { std::atomic<U64> k, d; };
static const int NOEVAL = 32767;
struct TTHit { int move, score, depth, flag, eval; };
std::unique_ptr<TTE[]> tt; U64 ttMask = 0;
static TTE* ttBase = nullptr;          // 64-byte aligned start of the table (buckets of 4 entries = one cache line)
static unsigned ttGen = 0;             // generation (0..3) of the current search
int lmrT[64][64], lmrT100[64][64];
atomic<bool> stopFlag(false);
atomic<bool> goInfinite(false);
long long softMs = 0, hardMs = 0, nodeLimit = 0;
int maxDepthLimit = 64;
chrono::steady_clock::time_point t0;
int hashMB = 256;                      // 2.1: default 256 MB (2.0: 64), as the CCRL testers prefer
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
static inline void ttPrefetch(U64 hash) { __builtin_prefetch((const char*)&ttBase[(hash & ttMask) & ~(U64)3], 0, 3); }
static inline int toTT(int s, int ply) { return s >= MATE_BOUND ? s + ply : s <= -MATE_BOUND ? s - ply : s >= TB_BOUND ? s + ply : s <= -TB_BOUND ? s - ply : s; }
static inline int fromTT(int s, int ply) { return s >= MATE_BOUND ? s - ply : s <= -MATE_BOUND ? s + ply : s >= TB_BOUND ? s - ply : s <= -TB_BOUND ? s + ply : s; }
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
KVAR corrFlags = 2;
KVAR v31Flags = 0, v31HP = 2500, v31CutR = 1, v31DE = 20;
// ---- 2.2: new search features (bits of K22Flags) and the search constants as options (the defaults are the values the search has always used)
KVAR k22Flags = 0;
KVAR k31Flags = 0;        // 3.1: 1 = late-move pruning with the reduced depth, 2 = prior-move continuation bonus after a fail low, 4 = fractional LMR (hundredths of a ply)
KVAR tLmrPv = 100, tLmrImp = 100, tLmrChk = 100, tLmrKil = 100, tLmrCut = 0, tLmrMin = 4, tPriorB = 50;
#ifdef KESTREL_TUNE_OPTIONS
const int PAWNH_N = 512, CORRX_N = 16384;      // sizes of the pawn history and of the minor / major correction tables
#else
const int PAWNH_N = 1, CORRX_N = 1;            // release builds: the ideas are switched off, the tables keep one row (a power of two for the index masks)
#endif
KVAR k32Flags = 8;        // 3.2: 1 = pawn-structure history, 4 = continuation correction history, 8 = smooth time management
KVAR tPawnW = 100, tCorrCM = 100, tPcI = 0, tCorrMin = 100, tCorrMaj = 100;
KVAR tTmBase = 60, tTmSt0 = 150, tTmStStep = 20, tTmStMin = 60, tTmDrop = 8, tTmDropMv = 5, tTmEffHi = 90, tTmEffMul = 80;
static int tmPrevScore = 0, tmPrevPly = -100;      // the score of the last timed search and the ply it started at (K32Flags 8)
KVAR tRazor = 300, tRfpD = 7, tRfpM = 80, tNmpB = 3, tNmpD = 4, tNmpE = 200, tPcD = 5, tPcM = 170, tIirD = 4, tLmpB = 3, tLmpD = 6, tFutD = 7, tFutB = 90, tFutM = 90, tSeeD = 6, tSeeQ = 30, tSeeC = 90,
           tSingD = 7, tSingTD = 3, tSingM = 2, tLmrB = 80, tLmrDv = 220, tLmrH = 6000, tHbM = 10, tHbC = 400, tAsp = 25, tQsD = 200, tCapFutB = 150, tCapFutM = 90, tTmStab = 70, tTmLow = 130;
#ifdef KESTREL_TUNE_OPTIONS
struct K22Tun { const char* name; int* v; int def, lo, hi; };
static const K22Tun k22Tun[] = {
    {"T_Razor", &tRazor, 300, 50, 800}, {"T_RfpD", &tRfpD, 7, 2, 12}, {"T_RfpM", &tRfpM, 80, 20, 200}, {"T_NmpB", &tNmpB, 3, 1, 6}, {"T_NmpD", &tNmpD, 4, 2, 8}, {"T_NmpE", &tNmpE, 200, 50, 600},
    {"T_PcD", &tPcD, 5, 3, 10}, {"T_PcM", &tPcM, 170, 50, 400}, {"T_IirD", &tIirD, 4, 2, 10}, {"T_LmpB", &tLmpB, 3, 0, 10}, {"T_LmpD", &tLmpD, 6, 2, 12}, {"T_FutD", &tFutD, 7, 2, 12}, {"T_FutB", &tFutB, 90, 0, 300},
    {"T_FutM", &tFutM, 90, 20, 250}, {"T_SeeD", &tSeeD, 6, 2, 12}, {"T_SeeQ", &tSeeQ, 30, 5, 100}, {"T_SeeC", &tSeeC, 90, 20, 250}, {"T_SingD", &tSingD, 7, 4, 12}, {"T_SingTD", &tSingTD, 3, 1, 6}, {"T_SingM", &tSingM, 2, 1, 6},
    {"T_LmrB", &tLmrB, 80, 0, 200}, {"T_LmrDv", &tLmrDv, 220, 100, 400}, {"T_LmrH", &tLmrH, 6000, 1500, 20000}, {"T_HbM", &tHbM, 10, 3, 30}, {"T_HbC", &tHbC, 400, 100, 1600}, {"T_Asp", &tAsp, 25, 5, 80}, {"T_QsD", &tQsD, 200, 50, 500},
    {"T_CapFutB", &tCapFutB, 150, 0, 500}, {"T_CapFutM", &tCapFutM, 90, 20, 250}, {"T_TmStab", &tTmStab, 70, 30, 100}, {"T_TmLow", &tTmLow, 130, 100, 250},
    {"T_LmrPv", &tLmrPv, 100, 0, 300}, {"T_LmrImp", &tLmrImp, 100, 0, 300}, {"T_LmrChk", &tLmrChk, 100, 0, 300}, {"T_LmrKil", &tLmrKil, 100, 0, 300}, {"T_LmrCut", &tLmrCut, 0, 0, 300},
    {"T_LmrMin", &tLmrMin, 4, 1, 8}, {"T_PriorB", &tPriorB, 50, 0, 300},
    {"T_PawnW", &tPawnW, 100, 0, 300}, {"T_CorrCM", &tCorrCM, 100, 0, 300}, {"T_PcI", &tPcI, 0, 0, 150}, {"T_CorrMin", &tCorrMin, 100, 0, 300}, {"T_CorrMaj", &tCorrMaj, 100, 0, 300}, {"T_TmBase", &tTmBase, 60, 30, 100}, {"T_TmSt0", &tTmSt0, 150, 100, 250}, {"T_TmStStep", &tTmStStep, 20, 0, 50},
    {"T_TmStMin", &tTmStMin, 60, 30, 100}, {"T_TmDrop", &tTmDrop, 8, 0, 40}, {"T_TmDropMv", &tTmDropMv, 5, 0, 40}, {"T_TmEffHi", &tTmEffHi, 90, 60, 99}, {"T_TmEffMul", &tTmEffMul, 80, 40, 100} };
#endif
static void initLmr() { for (int d = 1; d < 64; d++) for (int m = 1; m < 64; m++) { const double b = tLmrB / 100.0 + log((double)d) * log((double)m) / (tLmrDv / 100.0); lmrT[d][m] = (int)b; lmrT100[d][m] = (int)(b * 100.0); } }
KVAR qsTT = 1;
KVAR evalCache = 1;

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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int evalNN(const Pos& pos) { const_cast<Pos&>(pos).ensureBoth(); return netEval(pos.acc[pos.side], pos.acc[pos.side ^ 1], popc(pos.All)); }

static inline bool knightPairVsBareKing(const Pos& p) {        // 40.1: K + N + N against K: no forced mate, a draw (but not a dead position: the search still finds a mate the opponent allows)
    if (p.P[0] | p.P[6] | p.P[2] | p.P[8] | p.P[3] | p.P[9] | p.P[4] | p.P[10]) return false;
    const int nw = popc(p.P[1]), nb = popc(p.P[7]);
    return (nw == 2 && nb == 0) || (nw == 0 && nb == 2);
}
static int evaluateRaw(const Pos& pos) {
    if (knightPairVsBareKing(pos)) return 0;
    if (nnueReady) {
        if (nnBlend >= 100 && !net2Loaded) return evalNN(pos);
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
short capHist[768][6];            // 2.2: capture history [moved piece * 64 + target][captured kind]
short contHist4[768][768];        // 2.2: continuation history 4 plies back
short pawnHist[PAWNH_N][12][64];      // 3.2 (K32Flags 1): history by pawn structure [pawn key & 511][moved piece][target]
int corrCM[2][768];               // 3.2 (K32Flags 4): evaluation correction by the previous move [side][piece * 64 + target]
int corrMin[2][CORRX_N], corrMaj[2][CORRX_N];      // 3.2 (K32Flags 32 / 64): corrections by the minor / major pieces of both sides
long long rootEff[4096];          // 2.2: nodes spent below each root move (by from/to) in the current iteration
int pieceAt[MAXPLY + 4], toAt[MAXPLY + 4], evalAt[MAXPLY + 4], quietAt[MAXPLY + 4];
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
static inline int minKeyIdx(const Pos& p) {         // 3.2: all knights and bishops
    U64 k = (p.P[1] * 0x9E3779B97F4A7C15ULL) ^ (p.P[2] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[7] * 0x165667B19E3779F9ULL) ^ (p.P[8] * 0x27D4EB2F165667C5ULL);
    return (int)((k >> 50) & 16383);
}
static inline int majKeyIdx(const Pos& p) {         // 3.2: all rooks and queens
    U64 k = (p.P[3] * 0x9E3779B97F4A7C15ULL) ^ (p.P[4] * 0xC2B2AE3D27D4EB4FULL) ^ (p.P[9] * 0x165667B19E3779F9ULL) ^ (p.P[10] * 0x27D4EB2F165667C5ULL);
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
    if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) s += contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + to] / 2;
    if (k32Flags & 1) s += pawnHist[pawnKeyIdx(p) & (PAWNH_N - 1)][pc][to] * tPawnW / 100;
    return s;
}

int moveScore(const Pos& p, int m, int ttMove, int ply) {
    if (m == ttMove) return 10000000;
    int to = (m >> 6) & 63, from = m & 63, promo = (m >> 12) & 7;
    if (m & CAP) {
        int victim = (m & EPF) ? 0 : p.B[to] % 6;
        int mvv = PV_[victim] * 10 - PV_[p.B[from] % 6] / 10 + (promo ? 5000 : 0);
        if (k22Flags & 1) mvv += capHist[p.B[from] * 64 + to][victim] / 8;
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
            if (stand + PV_[victim] + tQsD < alpha) continue;
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
    if (tbMen && !excluded && ply > 0 && depth >= tbProbeDepth && pos.half == 0 && !pos.castle && popc(pos.All) <= (int)std::min<unsigned>(tbMen, (unsigned)tbProbeLimit)) {
        const unsigned tw = tbProbeWdl(pos);                    // 3.0: tablebase probe (right after a capture or a pawn move, as nothing else can have a zero fifty-move counter)
        if (tw != TB_RESULT_FAILED) {
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            const int wdl = (int)tw - 2, dr = tbRule50 ? 1 : 0, tv = TBWIN - ply;
            const int tbv = wdl < -dr ? -tv : (wdl > dr ? tv : 2 * wdl * dr);
            ttWrite(pos.hash, 0, toTT(tbv, ply), std::min(depth + 6, 250), TT_EXACT, NOEVAL);
            return tbv;
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
    const int cci = (ply >= 1 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 1] < 12) ? pieceAt[ply - 1] * 64 + toAt[ply - 1] : -1;      // 3.2 (K32Flags 4): the previous move
    if ((k32Flags & 4) && cci >= 0) corr += corrCM[us][cci] * 3 * tCorrCM / 800;
    const int cmi = (k32Flags & 32) ? (minKeyIdx(pos) & (CORRX_N - 1)) : 0, cji = (k32Flags & 64) ? (majKeyIdx(pos) & (CORRX_N - 1)) : 0;
    if (k32Flags & 32) corr += corrMin[us][cmi] * 3 * tCorrMin / 800;
    if (k32Flags & 64) corr += corrMaj[us][cji] * 3 * tCorrMaj / 800;
    int eval = rawEval;
    if (!inChk) eval = max(-MATE_BOUND + 1, min(MATE_BOUND - 1, rawEval + corr / 32));
    evalAt[ply] = eval;
    bool improving = !inChk && ply >= 2 && evalAt[ply - 2] != -INF && eval > evalAt[ply - 2];
    int peval = eval;
    if ((v31Flags & 2) && !inChk && ttFlag != TT_NONE && abs(ttScore) < MATE_BOUND &&
        (ttFlag == TT_EXACT || (ttFlag == TT_LOWER && ttScore > eval) || (ttFlag == TT_UPPER && ttScore < eval))) peval = ttScore;
    if (!pv && !inChk && !excluded) {
        if (depth <= 2 && peval + tRazor < alpha) { int q = qsearch(alpha, beta, ply); if (q <= alpha) return q; }
        if (depth <= tRfpD && peval - tRfpM * (depth - improving) >= beta && beta > -MATE_BOUND && peval < MATE_BOUND) return (peval + beta) / 2;
        bool nonPawn = pos.Occ[us] & ~(pos.P[us * 6] | pos.P[us * 6 + 5]);
        if (allowNull && depth >= 3 && peval >= beta && nonPawn &&
            !((v31Flags & 32) && ttFlag == TT_UPPER && ttScore < beta && ttDepth >= depth - 3)) {
            int R = tNmpB + depth / tNmpD + min((peval - beta) / tNmpE, 2);
            pieceAt[ply] = -1;
            pos.makeNull();
            int s = -search(depth - 1 - R, -beta, -beta + 1, ply + 1, false, 0, !cutNode);
            pos.unmakeNull();
            if (stopFlag) return 0;
            if (s >= beta) return s >= MATE_BOUND ? beta : s;
        }
        if (depth >= tPcD && abs(beta) < MATE_BOUND && !((k32Flags & 16) && ttFlag != TT_NONE && ttDepth >= depth - 3 && abs(ttScore) < MATE_BOUND && ttScore < beta + tPcM - (improving ? tPcI : 0))) {
            int pb = beta + tPcM - (improving ? tPcI : 0);
            int pm[256], ps[256];
            int pn = pos.gen(pm, true);
            for (int i = 0; i < pn; i++) ps[i] = moveScore(pos, pm[i], ttMove, ply);
            for (int i = 0; i < pn; i++) {
                int bi = i;
                for (int j = i + 1; j < pn; j++) if (ps[j] > ps[bi]) bi = j;
                swap(pm[i], pm[bi]); swap(ps[i], ps[bi]);
                int m = pm[i];
                if (!seeGE(pos, m, pb - eval)) continue;
                pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = 0;
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
    if (depth >= tIirD && !ttMove && !inChk && !excluded) depth--;
    if ((v31Flags & 64) && cutNode && depth >= 6 && !ttMove && !inChk && !excluded) depth--;
    int mv[256], sc[256];
    int n = pos.gen(mv, false);
    for (int i = 0; i < n; i++) sc[i] = moveScore(pos, mv[i], ttMove, ply);
    int best = -INF, bestMove = 0, legal = 0, origAlpha = alpha, nq = 0, qSeen = 0;
    int quiets[64]; int caps[32]; int ncap = 0;
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
            int lmrD = depth;      // 3.1 (K31Flags 1): quiet moves are pruned by the depth that is left after the late-move reduction of their move number
            if ((k31Flags & 1) && quiet) lmrD = max(0, depth - lmrT[min(depth, 63)][min(legal + 1, 63)]);
            if (quiet) {
                qSeen++;
                if (depth <= tLmpD && qSeen > ((tLmpB + depth * depth) >> (improving ? 0 : 1))) continue;
                if (lmrD <= tFutD && peval + tFutB + tFutM * lmrD <= alpha) continue;
                if ((v31Flags & 4) && depth <= 4 && sc[i] < -v31HP * depth) continue;
            }
            if ((k22Flags & 8) && (m & CAP) && !((m >> 12) & 7) && depth <= 5) {          // 2.2: capture futility
                int cv = (m & EPF) ? 0 : pos.B[(m >> 6) & 63] % 6;
                if (eval + tCapFutB + PV_[cv] + tCapFutM * depth <= alpha) continue;
            }
            if ((quiet ? lmrD : depth) <= tSeeD && !seeGE(pos, m, quiet ? -tSeeQ * lmrD * lmrD : -tSeeC * depth)) continue;
        }
        int ext = 0;
        if (depth >= tSingD && m == ttMove && !excluded && ply > 0 && ttDepth >= depth - tSingTD &&
            (ttFlag == TT_LOWER || ttFlag == TT_EXACT) && abs(ttScore) < MATE_BOUND) {
            int sb = ttScore - tSingM * depth;
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
            ttPrefetch(pos.keyAfter(mv[i + 1])); pos.prefetchRows(mv[i + 1]);
        }
        pieceAt[ply] = pos.B[m & 63]; toAt[ply] = (m >> 6) & 63; quietAt[ply] = quiet;
        long long nodes0 = nodes_;
        pos.make(m);
        if (pos.attacked(pos.kingSq(us), us ^ 1)) { pos.unmake(m); continue; }
        legal++;
        bool givesCheck = pos.inCheck();
        int s, nd = depth - 1 + ext;
        if (legal == 1) s = -search(nd, -beta, -alpha, ply + 1, true, 0, pv ? false : !cutNode);
        else {
            int R = 0;
            if (depth >= 3 && quiet && legal >= tLmrMin && (k31Flags & 4)) {            // 3.1: the reduction in hundredths of a ply, rounded at the end
                int r100 = lmrT100[min(depth, 63)][min(legal, 63)];
                if (pv) r100 -= tLmrPv;
                if (!improving) r100 += tLmrImp;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) r100 += 100;
                if (cutNode) r100 += ((v31Flags & 1) ? v31CutR * 100 : 0) + tLmrCut;
                if (givesCheck || inChk) r100 -= tLmrChk;
                if (m == killers[ply][0] || m == killers[ply][1]) r100 -= tLmrKil;
                r100 -= max(-200, min(200, sc[i] * 100 / tLmrH));
                R = max(0, min((r100 + 50) / 100, nd - 1));
            }
            else if (depth >= 3 && quiet && legal >= tLmrMin) {
                R = lmrT[min(depth, 63)][min(legal, 63)];
                if (pv) R--;
                if (!improving) R++;
                if ((k22Flags & 4) && ttMove && (ttMove & CAP)) R++;           // 2.2: the TT move is a capture: the quiet moves matter less
                if ((v31Flags & 1) && cutNode) R += v31CutR;
                if (givesCheck || inChk) R--;
                if (m == killers[ply][0] || m == killers[ply][1]) R--;
                R -= max(-2, min(2, sc[i] / tLmrH));
                R = max(0, min(R, nd - 1));
            }
            else if ((v31Flags & 16) && depth >= 3 && !quiet && sc[i] < 0 && legal >= 2) R = max(0, min(1 + (depth >= 5), nd - 1));
            s = -search(nd - R, -alpha - 1, -alpha, ply + 1, true, 0, R > 0 ? true : !cutNode);
            if (s > alpha && R > 0) {
                int nd2 = nd;
                if (k22Flags & 4) { if (s > best + 70 + 2 * nd) nd2 = nd + 1; else if (s < best + nd) nd2 = nd - 1; }     // 2.2: deeper / shallower re-search
                s = -search(nd2, -alpha - 1, -alpha, ply + 1, true, 0, !cutNode);
            }
            if (s > alpha && s < beta) s = -search(nd, -beta, -alpha, ply + 1, true, 0, false);
        }
        pos.unmake(m);
        if (stopFlag) return 0;
        if (ply == 0) rootEff[m & 4095] += nodes_ - nodes0;
        if (quiet && nq < 64) quiets[nq++] = m;
        else if ((m & CAP) && !((m >> 12) & 7) && ncap < 32) caps[ncap++] = m;
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
                        int bonus = min(depth * depth * tHbM / 10, tHbC);
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
                            if ((k22Flags & 2) && ply >= 4 && pieceAt[ply - 4] >= 0) updHist(contHist4[pieceAt[ply - 4] * 64 + toAt[ply - 4]][pc * 64 + ((qm >> 6) & 63)], b / 2);
                            if ((k32Flags & 1) && pc >= 0 && pc < 12) updHist(pawnHist[pawnKeyIdx(pos) & (PAWNH_N - 1)][pc][(qm >> 6) & 63], b);
                        }
                    }
                    else if ((k22Flags & 1) && (m & CAP) && !((m >> 12) & 7)) {          // 2.2: capture history: reward the capture that cut off, punish the captures tried before it
                        int bc = min(depth * depth, 400);
                        for (int k = 0; k < ncap; k++) {
                            int cm = caps[k], cv = (cm & EPF) ? 0 : pos.B[(cm >> 6) & 63] % 6, cp = pos.B[cm & 63];
                            if (cp < 0 || cp >= 12) continue;
                            updHist(capHist[cp * 64 + ((cm >> 6) & 63)][cv], cm == m ? bc : -bc);
                        }
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return excluded ? alpha : (inChk ? -MATE + ply : 0);
    if ((k31Flags & 2) && bestMove == 0 && !excluded && !inChk && depth >= 3 && ply >= 2 && pieceAt[ply - 1] >= 0 && pieceAt[ply - 2] >= 0 && quietAt[ply - 1])      // 3.1: the opponent's previous quiet move was good
        updHist(contHist[pieceAt[ply - 2] * 64 + toAt[ply - 2]][pieceAt[ply - 1] * 64 + toAt[ply - 1]], min(depth * depth * tPriorB / 100, tHbC));
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
        if ((k32Flags & 4) && cci >= 0) { int& cc = corrCM[us][cci]; cc = (cc * (256 - wgt) + diff * wgt) / 256; cc = max(-32 * 80, min(32 * 80, cc)); }
        if (k32Flags & 32) { int& cx = corrMin[us][cmi]; cx = (cx * (256 - wgt) + diff * wgt) / 256; cx = max(-32 * 80, min(32 * 80, cx)); }
        if (k32Flags & 64) { int& cy = corrMaj[us][cji]; cy = (cy * (256 - wgt) + diff * wgt) / 256; cy = max(-32 * 80, min(32 * 80, cy)); }
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
    memset(capHist, 0, sizeof capHist); memset(contHist4, 0, sizeof contHist4);
    memset(pawnHist, 0, sizeof pawnHist); memset(corrCM, 0, sizeof corrCM); memset(corrMin, 0, sizeof corrMin); memset(corrMaj, 0, sizeof corrMaj);
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
        int alpha = -INF, beta = INF, delta = tAsp;
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
static int searchThreads = 1;           // threads of the running (or last) search: the node counters of the others are stale
static long long totalNodes() { long long t = 0; for (int k = 0; k < searchThreads && k < MAXTH; k++) t += *nodeCtr[k]; return t; }
static void clearAllTables() { S0::clearTables(); S1::clearTables(); S2::clearTables(); S3::clearTables(); S4::clearTables(); S5::clearTables(); S6::clearTables(); S7::clearTables(); }

// ---------------------------------------------------------------- 3.0: Windows power throttling
static void winFullSpeed() {            // asks Windows not to put this process in "efficiency mode" (EcoQoS): full speed also when the engine is not in the foreground
#ifdef _WIN32
    typedef BOOL (WINAPI *SetProcInfoFn)(HANDLE, int, LPVOID, DWORD);
    HMODULE km = GetModuleHandleW(L"kernel32.dll");
    SetProcInfoFn f = km ? (SetProcInfoFn)(void*)GetProcAddress(km, "SetProcessInformation") : nullptr;
    if (f) { struct { ULONG version, control, state; } st = {1, 1, 0}; f(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st); }       // control = execution speed, state = off
#endif
}

// ---------------------------------------------------------------- experience learning (persistent across games)
struct ExpRec { int w = 0, d = 0, l = 0; };
unordered_map<U64, unordered_map<int, ExpRec>> expDB;
bool useExp = false;                  // 2.1: position learning off by default (2.0: on)
string expPath = "kestrel_experience_v2.dat";
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
bool useBook = false;                 // 2.1: own opening book off by default (2.0: on)
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
    winFullSpeed();
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
    tbHitsTotal = 0; int tbKept = legalCount;
    if (tbMen && multiPV <= 1 && searchOnlyBan.empty() && legalCount > 1) {         // 3.0: a root inside the tablebases searches only the moves with the best tablebase rank
        std::vector<int> tbBans;
        if (tbRootBans(pos, tbBans) && !tbBans.empty()) {
            for (int bm : tbBans) rootBan.push_back(bm);
            tbKept = legalCount - (int)tbBans.size();
            tbHitsTotal.fetch_add(1, std::memory_order_relaxed);
            printf("info string Syzygy: root position in the tablebases, %d of %d moves excluded\n", (int)tbBans.size(), legalCount); fflush(stdout);
        }
    }
    int stableIt = 0, ponderMv = 0;
    KThread helperTh[MAXTH]; const int nth = (multiPV > 1) ? 1 : max(1, min(MAXTH, numThreads));
    struct HelperJoin { KThread* h; int n; ~HelperJoin() { stopFlag = true; for (int k = 1; k < n; k++) if (h[k].joinable()) h[k].join(); } } helperJoin{helperTh, nth};
    searchThreads = 1;
    if (nth > 1) {
        gRoot = pos;
        try { for (int k = 1; k < nth; k++) { helperTh[k] = KThread(helperFns[k], k); searchThreads = k + 1; } }
        catch (const std::exception&) {}              // fewer threads than asked for
    }
    for (int d = 1; d <= maxDepthLimit; d++) {
        int alpha = -INF, beta = INF, delta = tAsp;
        if (d >= 5) { alpha = max(-INF, bestScore - delta); beta = min(INF, bestScore + delta); }
        memset(rootEff, 0, sizeof rootEff); long long nodesIt0 = nodes_;
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
            char tbs[40] = ""; if (tbMen) snprintf(tbs, sizeof tbs, " tbhits %lld", (long long)tbHitsTotal.load());
            snprintf(b2, sizeof b2, " nodes %lld nps %lld hashfull %d%s time %lld pv", totalNodes(), el > 0 ? totalNodes() * 1000 / el : totalNodes(), hashfull(), tbs, el);
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
            if (k22Flags & 16) {                // 2.2: most of the effort on the best move -> stop sooner; effort spread over many moves -> think longer
                long long tot = nodes_ - nodesIt0; double fr = tot > 0 ? (double)rootEff[bestMove & 4095] / (double)tot : 0.5;
                if (fr > 0.85 && !scoreDrop) thr = thr * tTmStab / 100; else if (fr < 0.35) thr = thr * tTmLow / 100;
            }
            if (k32Flags & 8) {          // 3.2: smooth time management (see the header of dev\patch_k32.py)
                double f = tTmBase / 100.0 * (double)max(tTmStMin, tTmSt0 - tTmStStep * stableIt) / 100.0;
                if (d >= 6) f *= 1.0 + max(0, min(80, (prevScoreIt - s) * tTmDrop / 10)) / 100.0;                           // this iteration scores lower than the last one
                if (d >= 6 && tmPrevPly == pos.rootPly - 2) f *= 1.0 + max(-10, min(50, (tmPrevScore - s) * tTmDropMv / 10)) / 100.0;       // lower than the last search of this game
                { long long tot = nodes_ - nodesIt0; if (tot > 0 && !scoreDrop && (double)rootEff[bestMove & 4095] * 100.0 / (double)tot > tTmEffHi) f *= tTmEffMul / 100.0; }
                thr = (long long)((double)softMs * f);
            }
            if (el >= thr) break;
        }
        if (legalCount == 1 && softMs > 0 && d >= 4) break;
        if (tbKept == 1 && softMs > 0 && d >= 4) break;           // 3.0: a single move is left after the tablebase filter
        if (mateLimit > 0 && s >= MATE_BOUND && (MATE - s + 1) / 2 <= mateLimit) break;       // 36.2: go mate N found
        if (abs(s) >= MATE_BOUND && d >= 8) break;
    }
    if (!bestMove) bestMove = fbMove;
    if (softMs > 0 && !fixedTime && !limitedGo && !goInfinite) { tmPrevScore = bestScore; tmPrevPly = pos.rootPly; }          // 3.2 (K32Flags 8): for the next search of this game
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
            int16_t a0[NN_H] = {}, a1[NN_H] = {}; int32_t q0 = p.psq[0], q1 = p.psq[1];
            static unsigned rngLz = 12345u; rngLz = rngLz * 1664525u + 1013904223u; const bool chk = (rngLz >> 28) < 11;
            if (chk) { p.ensureBoth(); memcpy(a0, p.acc[0], sizeof a0); memcpy(a1, p.acc[1], sizeof a1); p.refresh(); }
            p.refreshPst();
            if (hk != p.computeHash() || pm != p.pstMg || pe != p.pstEg || ph != p.phaseAcc) {
                if (accMismatch < 5) printf("MISMATCH hash/pst after move %s\n", moveStr(mv[i]).c_str());
                accMismatch++;
            }
            accChecked++;
            if (chk && (memcmp(a0, p.acc[0], sizeof a0) || memcmp(a1, p.acc[1], sizeof a1) || q0 != p.psq[0] || q1 != p.psq[1])) {
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
    winFullSpeed();
    initTables(); initPext(); initEval(); initNet(); resizeTT(hashMB); pos.refresh();
    { string dir;
#ifdef _WIN32
      dir = exeDirUtf8();                                                 // 37.1: not argv[0] (ANSI code page, and no folder at all when started by a bare name)
#endif
#if defined(__linux__)
      { char lb[4096]; ssize_t ln = readlink("/proc/self/exe", lb, sizeof lb - 1);           // 40.1: the real folder of the exe, also when started by a bare name through PATH
        if (ln > 0) { lb[ln] = 0; string s(lb); size_t sl = s.find_last_of('/'); if (sl != string::npos) dir = s.substr(0, sl + 1); } }
#elif defined(__APPLE__)
      { uint32_t sz = 0; _NSGetExecutablePath(nullptr, &sz);
        if (sz > 0 && sz < 65536) { vector<char> lb(sz + 1, 0); if (_NSGetExecutablePath(lb.data(), &sz) == 0) { string s(lb.data()); size_t sl = s.find_last_of('/'); if (sl != string::npos) dir = s.substr(0, sl + 1); } } }
#endif
      if (dir.empty()) { string a0 = argc > 0 && argv[0] ? argv[0] : ""; size_t sl = a0.find_last_of("\\/"); dir = sl == string::npos ? string() : a0.substr(0, sl + 1); }
      expPath = dir + "kestrel_experience_v2.dat"; loadExp();
      bookPath = dir + "claude_book.txt"; loadBook(); rngState ^= (U64)chrono::steady_clock::now().time_since_epoch().count(); }
    initLmr();
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
        if (argc > 3 && !nnLoadFile(argv[3])) { printf("could not load the net %s\n", argv[3]); return 1; }
#ifdef KSTATS
        atexit(kstatsReport);
#endif
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
        if (argc > 2 && !nnLoadFile(argv[2])) { printf("could not load the net %s\n", argv[2]); return 1; }
        const char* fens[] = {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
            "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
            "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"};
        for (const char* f : fens) { pos.setFen(f); accWalk(pos, argc > 3 ? atoi(argv[3]) : 3); }
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
#ifdef KESTREL_TUNE_OPTIONS
        if (argc > 5) nnBlend = atoi(argv[5]);
#endif
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
    KThread worker;
    string line;
    while (getline(cin, line)) {
        istringstream is(line); string cmd; is >> cmd;
        try {
        if (cmd == "uci") {
            printf("id name Kestrel 3.0\nid author Claude\noption name Hash type spin default 256 min 1 max 4096\noption name Threads type spin default 1 min 1 max 8\noption name OwnBook type check default false\noption name Move Overhead type spin default 5 min 0 max 5000\noption name MultiPV type spin default 1 min 1 max 8\noption name Ponder type check default false\noption name Clear Hash type button\noption name UCI_LimitStrength type check default false\noption name UCI_Elo type spin default 3200 min 1320 max 3200\noption name UseNNUE type check default true\noption name Experience type check default false\noption name EvalFile type string default <internal>\noption name EvalScale type spin default 100 min 50 max 200\noption name ExperienceFile type string default kestrel_experience_v2.dat\noption name UCI_ShowWDL type check default false\noption name UCI_AnalyseMode type check default false\noption name UCI_Chess960 type check default false\noption name SyzygyPath type string default <empty>\noption name SyzygyProbeDepth type spin default 1 min 1 max 100\noption name Syzygy50MoveRule type check default true\noption name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
#ifdef KESTREL_TUNE_OPTIONS
            printf("option name K22Flags type spin default %d min 0 max 255\n", k22Flags);
            printf("option name K31Flags type spin default %d min 0 max 255\n", k31Flags);
            printf("option name K32Flags type spin default %d min 0 max 255\n", k32Flags);
            printf("option name NNBlend type spin default 100 min 0 max 100\noption name LazyMargin type spin default 0 min 0 max 3000\noption name EvalFlags type spin default 1 min 0 max 255\noption name QsTT type spin default 1 min 0 max 1\n"
                   "option name CorrFlags type spin default 2 min 0 max 2\noption name EvalCache type spin default 1 min 0 max 1\noption name V31Flags type spin default %d min 0 max 255\noption name V31HistPrune type spin default 2500 min 0 max 20000\n"
                   "option name V31CutR type spin default 1 min 0 max 3\noption name V31DE type spin default 20 min 0 max 200\n", v31Flags);
            for (const auto& tn : k22Tun) printf("option name %s type spin default %d min %d max %d\n", tn.name, tn.def, tn.lo, tn.hi);
#endif
            printf("uciok\n"); fflush(stdout);
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
                                                 "K22Flags", "V31Flags", "V31HistPrune", "V31CutR", "V31DE", "MultiPV", "Ponder", "Clear Hash", "UCI_LimitStrength", "UCI_Elo", "UseNNUE", "Experience", "ExperienceFile", "EvalFile", "EvalScale", "SyzygyPath", "SyzygyProbeDepth", "Syzygy50MoveRule", "SyzygyProbeLimit", "UCI_ShowWDL", "UCI_AnalyseMode", "UCI_Chess960"};
                auto low = [](string s) { for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32); return s; };
                string ln = low(name);
                for (const char* d : declared) if (low(d) == ln) { name = d; break; }
#ifdef KESTREL_TUNE_OPTIONS
                for (const auto& tn : k22Tun) if (low(tn.name) == ln) { name = tn.name; break; }
#endif
                string lv = low(value); if (name != "ExperienceFile") value = lv == "true" ? string("true") : lv == "false" ? string("false") : value;
            }
            if (name == "UCI_ShowWDL") showWDL = (value == "true");
            if (name == "UCI_AnalyseMode") uciAnalyse = (value == "true");
            if (name == "UCI_Chess960") chess960 = (value == "true");
            if (name == "LazyMargin") lazyMargin = max(0, atoi(value.c_str()));
            if (name == "OwnBook") useBook = (value == "true");
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "CorrFlags") corrFlags = max(0, min(2, atoi(value.c_str())));
#endif
            if (name == "Move Overhead") moveOverhead = max(0, min(5000, atoi(value.c_str())));
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "EvalCache") evalCache = max(0, min(1, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "V31Flags") v31Flags = max(0, min(255, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "K22Flags") k22Flags = max(0, min(255, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "K31Flags") k31Flags = max(0, min(255, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "K32Flags") k32Flags = max(0, min(255, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            for (const auto& tn : k22Tun) if (name == tn.name) { *tn.v = max(tn.lo, min(tn.hi, atoi(value.c_str()))); if (name == "T_LmrB" || name == "T_LmrDv") initLmr(); }
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "V31HistPrune") v31HP = max(0, min(20000, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "V31CutR") v31CutR = max(0, min(3, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "V31DE") v31DE = max(0, min(200, atoi(value.c_str())));
#endif
            if (name == "Threads") numThreads = max(1, min(MAXTH, atoi(value.c_str())));
            if (name == "MultiPV") multiPV = max(1, min(8, atoi(value.c_str())));
            if (name == "Ponder") uciPonder = (value == "true");
            if (name == "Clear Hash") { resizeTT(hashMB); clearAllTables(); }
            if (name == "UCI_LimitStrength") limitStrength = (value == "true");
            if (name == "UCI_Elo") uciElo = max(1320, min(3200, atoi(value.c_str())));
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "QsTT") qsTT = max(0, min(1, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "EvalFlags") evalFlags = max(0, min(255, atoi(value.c_str())));
#endif
#ifdef KESTREL_TUNE_OPTIONS
            if (name == "NNBlend") nnBlend = max(0, min(100, atoi(value.c_str())));
#endif
            if (name == "Net2Share") net2Share = max(0, min(100, atoi(value.c_str())));
            if (name == "UseNet2") useNet2 = (value == "true");
            if (name == "UseNNUE") { nnueReady = netLoaded && value == "true"; pos.refresh(); }
            if (name == "EvalFile") { if (value == "<internal>" || value.empty()) initNet(); else if (!nnLoadFile(value)) printf("info string could not load the net file %s\n", value.c_str()); pos.refresh(); }
            if (name == "EvalScale") nnScale = (float)max(50, min(200, atoi(value.c_str()))) * 0.01f;
            if (name == "Experience") { useExp = (value == "true"); loadExp(); }
            else if (name == "ExperienceFile") { expPath = value; loadExp(); }
            if (name == "Hash") { hashMB = max(1, min(4096, atoi(value.c_str()))); resizeTT(hashMB); }
            if (name == "SyzygyPath") {
                tbPath = value;
                if (!tbSetPath(value)) printf("info string could not read the tablebase path %s\n", value.c_str());
                else if (tbMen) printf("info string Syzygy: tablebases up to %u men found\n", tbMen);
                else if (!value.empty() && value != "<empty>") printf("info string Syzygy: no tablebase files found in %s\n", value.c_str());
            }
            if (name == "SyzygyProbeDepth") tbProbeDepth = max(1, min(100, atoi(value.c_str())));
            if (name == "Syzygy50MoveRule") tbRule50 = (value == "true");
            if (name == "SyzygyProbeLimit") tbProbeLimit = max(0, min(7, atoi(value.c_str())));
        } else if (cmd == "tbprobe") {          // 3.0 (test aid): the tablebase result of the current position (whatever its fifty-move counter) and the root moves that would be searched
            if (!tbMen) printf("tbprobe: no tablebases\n");
            else if (pos.castle || popc(pos.All) > (int)tbMen) printf("tbprobe: outside the tablebases\n");
            else {
                const unsigned w = tbProbeWdl(pos);
                if (w == TB_RESULT_FAILED) printf("tbprobe: failed\n"); else printf("tbprobe: wdl %d\n", (int)w - 2);
                std::vector<int> bans;
                if (tbRootBans(pos, bans)) {
                    int mv[256]; const int n = pos.gen(mv, false); std::string kept;
                    for (int i = 0; i < n; i++) if (pos.legalMove(mv[i]) && std::find(bans.begin(), bans.end(), mv[i]) == bans.end()) kept += " " + moveStr(mv[i]);
                    printf("tbroot: kept%s\n", kept.c_str());
                } else printf("tbroot: no information\n");
            }
            fflush(stdout);
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
            try { worker = KThread(perftWorker, pn); } catch (const std::exception&) { stopFlag = true; }
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
                long long myT = max(1LL, (pos.side == 0 ? wt : bt) - moveOverhead - (numThreads > 8 ? (numThreads - 8) / 8 : 0)), myI = pos.side == 0 ? wi : bi;      // 3.2: + 1 ms per 8 threads above 8
                long long mtgv = mtg > 0 ? min(mtg, 40LL) : 28;
                softMs = max(10LL, myT / mtgv + myI * 3 / 4);
                hardMs = min(softMs * 4, max(10LL, myT / 3));
                hardMs = max(hardMs, 10LL);
                hardMs = min(hardMs, max(1LL, myT * 3 / 4));
            }
            if (ponderGo) { pendSoft = softMs; pendHard = hardMs; softMs = hardMs = 0; pondering = true; inf = true; } else pondering = false;
            t0 = chrono::steady_clock::now();
            stopFlag = false; goInfinite = inf;
            try { worker = KThread(thinkSafe); }
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
