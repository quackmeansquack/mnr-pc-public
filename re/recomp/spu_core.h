/* SPU core (spu_core.h): state, MFC channels and the instruction semantics, shared by the
 * interpreter (spu.c) and the ahead-of-time compiled programs (spu_progs.c, from spu2c.py).
 *
 * Originally: SPU interpreter for the game's small SPURS task programs (skinning, morphing, ...).
 *
 * A task runs synchronously on the calling host thread: the ELF's segments are copied into a fresh
 * 256 KB local store, r3 gets the 16-byte task argument and execution starts at the entry point
 * until the program executes `stop`. MFC DMA commands (channels 16..23) copy directly between local
 * store and guest memory, so results land where the PPU expects them; tag status reads report every
 * transfer as complete. These task programs don't call into the SPURS kernel (nothing below LS
 * 0x3000), which keeps the environment small. */
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "runtime.h"
#include "spu.h"
#pragma once

#define LS_SIZE 0x40000
#define LS_MASK 0x3FFFF
typedef struct { uint32_t w[4]; } q_t;   /* w[0] = bytes 0..3 (preferred slot), big-endian numeric words */

typedef struct {
    uint8_t ls[LS_SIZE];
    q_t r[128];
    uint32_t pc;
    uint32_t lsa, eah, eal, size, tag, tag_mask;
    uint32_t stop_code;
    uint32_t out_mbox;   /* SPU_WrOutMbox: data1 of the next sys_spu_thread_send_event */
    int stopped;
    uint64_t steps;
    uint32_t rsv_ea, atomic_stat;   /* lock-line reservation (GETLLAR/PUTLLC) */
    uint8_t rsv[128];
} spu_t;

/* local store is big-endian: whole words/quadwords with a byte swap (a byte at a time was a large share of SPU time) */
static inline uint32_t rd32(const spu_t *s, uint32_t a) { uint32_t v; memcpy(&v, s->ls + (a & LS_MASK), 4); return __builtin_bswap32(v); }
static inline void wr32(spu_t *s, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(s->ls + (a & LS_MASK), &v, 4); }
static inline q_t ldq(const spu_t *s, uint32_t a) {
    __m128i x = _mm_loadu_si128((const __m128i *)(s->ls + (a & LS_MASK & ~15u)));
    x = _mm_shuffle_epi8(x, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    q_t q; memcpy(&q, &x, 16); return q;
}
static inline void stq(spu_t *s, uint32_t a, q_t q) {
    __m128i x; memcpy(&x, &q, 16);
    x = _mm_shuffle_epi8(x, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    _mm_storeu_si128((__m128i *)(s->ls + (a & LS_MASK & ~15u)), x);
}
static inline uint8_t qb(const q_t *q, int i) { return (uint8_t)(q->w[i >> 2] >> (24 - 8 * (i & 3))); }
static inline void qsb(q_t *q, int i, uint8_t v) { int sh = 24 - 8 * (i & 3); q->w[i >> 2] = (q->w[i >> 2] & ~(0xFFu << sh)) | (uint32_t)v << sh; }
static inline uint16_t qh(const q_t *q, int i) { return (uint16_t)(q->w[i >> 1] >> (i & 1 ? 0 : 16)); }
static inline void qsh(q_t *q, int i, uint16_t v) { int sh = i & 1 ? 0 : 16; q->w[i >> 1] = (q->w[i >> 1] & ~(0xFFFFu << sh)) | (uint32_t)v << sh; }
/* SPU single precision has no Inf/NaN/denormals: exponent 255 is an ordinary (huge) number, results
 * saturate at the largest magnitude, denormals are zero. Approximated with IEEE floats: reads map
 * exponent 255 to +-FLT_MAX and denormals to 0; writes saturate Inf to +-FLT_MAX (NaN, which only an
 * Inf could produce, becomes 0) and flush denormals. Without this, frest(0) * 0 = NaN poisons whole mixes. */
static inline float qf(const q_t *q, int i) {
    uint32_t u = q->w[i], e = u & 0x7F800000u;
    if (e == 0x7F800000u) u = (u & 0x80000000u) | 0x7F7FFFFFu;
    else if (e == 0) u &= 0x80000000u;
    float f; memcpy(&f, &u, 4); return f;
}
static inline uint32_t f2w(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t e = u & 0x7F800000u;
    if (e == 0x7F800000u) u = (u & 0x007FFFFFu) ? 0 : (u & 0x80000000u) | 0x7F7FFFFFu;
    else if (e == 0) u &= 0x80000000u;
    return u;
}
/* the same, four lanes at once (the per-lane branches were most of the SPU float work in race profiles) */
static inline __m128 qf4(const q_t *q) {
    __m128i u; memcpy(&u, q, 16);
    const __m128i em = _mm_set1_epi32(0x7F800000), sm = _mm_set1_epi32((int)0x80000000u);
    __m128i e = _mm_and_si128(u, em), sg = _mm_and_si128(u, sm);
    u = _mm_blendv_epi8(u, _mm_or_si128(sg, _mm_set1_epi32(0x7F7FFFFF)), _mm_cmpeq_epi32(e, em));
    u = _mm_blendv_epi8(u, sg, _mm_cmpeq_epi32(e, _mm_setzero_si128()));
    return _mm_castsi128_ps(u);
}
static inline void f2w4(q_t *out, __m128 f) {
    __m128i u = _mm_castps_si128(f);
    const __m128i em = _mm_set1_epi32(0x7F800000), sm = _mm_set1_epi32((int)0x80000000u), z = _mm_setzero_si128();
    __m128i e = _mm_and_si128(u, em), sg = _mm_and_si128(u, sm), infnan = _mm_cmpeq_epi32(e, em);
    __m128i nan = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(u, _mm_set1_epi32(0x007FFFFF)), z), infnan);
    u = _mm_blendv_epi8(u, _mm_or_si128(sg, _mm_set1_epi32(0x7F7FFFFF)), infnan);
    u = _mm_blendv_epi8(u, z, nan);
    u = _mm_blendv_epi8(u, sg, _mm_cmpeq_epi32(e, z));
    memcpy(out, &u, 16);
}
static inline void m2w4(q_t *out, __m128 m) { memcpy(out, &m, 16); }
/* uniform byte/halfword lane ops in SSE (each lane's position in the word doesn't matter for them) */
static inline __m128i QV(const q_t *q) { __m128i x; memcpy(&x, q, 16); return x; }
static inline void QS(q_t *q, __m128i x) { memcpy(q, &x, 16); }
static inline __m128i cmpgtu8(__m128i a, __m128i b) { const __m128i k = _mm_set1_epi8((char)0x80); return _mm_cmpgt_epi8(_mm_xor_si128(a, k), _mm_xor_si128(b, k)); }
static inline __m128i cmpgtu16(__m128i a, __m128i b) { const __m128i k = _mm_set1_epi16((short)0x8000); return _mm_cmpgt_epi16(_mm_xor_si128(a, k), _mm_xor_si128(b, k)); }
static inline q_t fsm_bytes(uint32_t m) {   /* byte i = 0xFF where bit 15 - i of m is set */
    q_t r;
    for (int k = 0; k < 4; k++) { uint32_t n = (m >> (12 - 4 * k)) & 15;
        r.w[k] = (n & 8 ? 0xFF000000u : 0) | (n & 4 ? 0xFF0000u : 0) | (n & 2 ? 0xFF00u : 0) | (n & 1 ? 0xFFu : 0); }
    return r;
}
static inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }
static q_t splat(uint32_t v) { q_t q = {{v, v, v, v}}; return q; }
static inline double qd(const q_t *q, int i) { uint64_t u = (uint64_t)q->w[2 * i] << 32 | q->w[2 * i + 1]; double d; memcpy(&d, &u, 8); return d; }
static inline void sd(q_t *q, int i, double d) { uint64_t u; memcpy(&u, &d, 8); q->w[2 * i] = (uint32_t)(u >> 32); q->w[2 * i + 1] = (uint32_t)u; }

/* ------------------------------------------------------------------ MFC */
void spu_atomic_lock(int on);   /* spu.c: serialises lock-line stores between SPU programs on different threads */
/* spu.c: when set (voice_celp.c's firmware codecs), stores to guest memory outside [lo, hi) are dropped and reported */
extern __thread uint32_t spu_guard_lo, spu_guard_hi;
void spu_guard_report(spu_t *s, uint32_t op, uint32_t ea, uint32_t size);
static inline int spu_guard_ok(spu_t *s, uint32_t op, uint32_t ea, uint32_t size) {
    if (!spu_guard_hi || (ea >= spu_guard_lo && ea + size <= spu_guard_hi && ea + size >= ea)) return 1;
    spu_guard_report(s, op, ea, size); return 0;
}
extern int spu_dma_log, spu_dma_log_n;   /* MNR_JOB_DMALOG: trace PUTs of SPU jobs (spu.c) */
void spu_dma_trace(spu_t *s, uint32_t ea, uint32_t lsa, uint32_t size);
extern volatile long spu_dma_count[4];
extern __thread int spu_func_trace; void spu_func_dma(spu_t *s, uint32_t op, uint32_t ea, uint32_t lsa, uint32_t size);   /* GET, PUT, list, atomic: debug counters (spu.c) */
static void mfc_cmd(spu_t *s, uint32_t cmd) {
    uint32_t op = cmd & 0xFF;
    spu_dma_count[(op & 0xF0) == 0x40 ? 0 : (op & 0xF0) == 0x20 ? ((op & 4) ? 2 : 1) : 3]++;
    uint32_t ea = s->eal, lsa = s->lsa & LS_MASK, size = s->size;
    if (spu_func_trace) spu_func_dma(s, op, ea, lsa, size);
    switch (op) {
    case 0x40: case 0x41: case 0x42:   /* GET, GETB, GETF */
        if (size) memcpy(s->ls + lsa, gp(ea), size);
        break;
    case 0x20: case 0x21: case 0x22:   /* PUT, PUTB, PUTF */
        if (size && spu_guard_ok(s, op, ea, size)) memcpy(gp(ea), s->ls + lsa, size);
        if (spu_dma_log && spu_dma_log_n < 3000) spu_dma_trace(s, ea, lsa, size);
        break;
    case 0x44: case 0x45: case 0x46:   /* GETL: list at LS s->eal (8-byte elements {u32 size, u32 eal}) */
    case 0x24: case 0x25: case 0x26: { /* PUTL */
        uint32_t list = s->eal & LS_MASK & ~7u, n = size / 8;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ts = rd32(s, list + 8 * i) & 0x7FFF, el = rd32(s, list + 8 * i + 4);
            if (op & 0x40) memcpy(s->ls + lsa, gp(el), ts); else if (spu_guard_ok(s, op, el, ts)) memcpy(gp(el), s->ls + lsa, ts);
            lsa = (lsa + ts + 15) & ~15u & LS_MASK;   /* elements start on 16-byte LS boundaries (low bits follow EA for small ones) */
            if (rd32(s, list + 8 * i) & 0x80000000u) break;   /* stall-and-notify: treat as end */
        }
        break;
    }
    case 0xD0: {                        /* GETLLAR: 128-byte line, read once (8-byte atomic loads) and kept as the reservation */
        const uint64_t *src = (const uint64_t *)gp(ea & ~127u);
        for (int i = 0; i < 16; i++) { uint64_t v = __atomic_load_n(src + i, __ATOMIC_SEQ_CST); memcpy(s->rsv + 8 * i, &v, 8); }
        memcpy(s->ls + (lsa & ~127u), s->rsv, 128); s->rsv_ea = ea & ~127u;
        s->atomic_stat = 4;
        break;
    }
    case 0xB4: {                        /* PUTLLC: only if the line still matches the reservation. PPU code
                                         * updates the same lines with 4/8-byte compare-and-swap (stwcx/stdcx),
                                         * so only the 8-byte chunks this SPU changed are swapped in, each
                                         * against its reserved value; a failed swap undoes the earlier ones. */
        if (!spu_guard_ok(s, op, ea & ~127u, 128)) { s->rsv_ea = 0xFFFFFFFF; s->atomic_stat = 1; break; }
        spu_atomic_lock(1);
        uint64_t *mem = (uint64_t *)gp(ea & ~127u);
        const uint8_t *nl = s->ls + (lsa & ~127u);
        int ok = s->rsv_ea == (ea & ~127u) && !memcmp(mem, s->rsv, 128), done[16], nd = 0;
        for (int i = 0; ok && i < 16; i++) {
            uint64_t o, v; memcpy(&o, s->rsv + 8 * i, 8); memcpy(&v, nl + 8 * i, 8);
            if (o == v) continue;
            uint64_t e = o;
            if (__atomic_compare_exchange_n(mem + i, &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) done[nd++] = i; else ok = 0;
        }
        if (!ok) for (int k = 0; k < nd; k++) {
            uint64_t o, v; memcpy(&o, s->rsv + 8 * done[k], 8); memcpy(&v, nl + 8 * done[k], 8);
            __atomic_compare_exchange_n(mem + done[k], &v, o, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        }
        spu_atomic_lock(0);
        s->rsv_ea = 0xFFFFFFFF; s->atomic_stat = ok ? 0 : 1;
        break;
    }
    case 0xB0: case 0xB8:               /* PUTLLUC, PUTQLLUC */
        if (!spu_guard_ok(s, op, ea & ~127u, 128)) { s->atomic_stat = 2; break; }
        spu_atomic_lock(1);
        memcpy(gp(ea & ~127u), s->ls + (lsa & ~127u), 128);
        spu_atomic_lock(0);
        s->atomic_stat = 2;
        break;
    default: break;                     /* barriers, sync, SNDSIG...: nothing to do */
    }
}
int spurs_send_event(uint32_t port, uint32_t data0, uint32_t data1);   /* host.c */
static uint32_t rdch(spu_t *s, uint32_t ch) {
    switch (ch) {
    case 29: return 0;             /* SPU_RdInMbox: sys_spu_thread_send_event result (CELL_OK) */
    case 24: return s->tag_mask;   /* MFC_RdTagStat: everything done */
    case 27: return s->atomic_stat;   /* MFC_RdAtomicStat: 1 = PUTLLC failed, 2 = PUTLLUC done, 4 = GETLLAR done */
    case 8: { static uint32_t dec = 0xFFFFFFFF; return dec -= 1000; }   /* decrementer */
    default: return 0;
    }
}
static void wrch(spu_t *s, uint32_t ch, uint32_t v) {
    switch (ch) {
    case 16: s->lsa = v; break;
    case 17: s->eah = v; break;
    case 18: s->eal = v; break;
    case 19: s->size = v; break;
    case 20: s->tag = v; break;
    case 21: mfc_cmd(s, v); break;
    case 22: s->tag_mask = v; break;
    case 28: s->out_mbox = v; break;                                       /* SPU_WrOutMbox */
    case 30: spurs_send_event(v >> 24, v & 0xFFFFFF, s->out_mbox); break;  /* SPU_WrOutIntrMbox: send_event(port, data0) */
    default: break;
    }
}

/* ------------------------------------------------------------------ helpers */
#ifdef __SSSE3__
#include <tmmintrin.h>
/* q_t words are host-endian numbers holding big-endian bytes: SPU byte i sits at host byte i ^ 3.
 * Byte permutations go through a "big-endian byte vector" (byte i at position i) and pshufb. */
static inline __attribute__((always_inline)) __m128i q_be(q_t q) { return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)q.w), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)); }
static inline __attribute__((always_inline)) q_t be_q(__m128i v) { q_t q; _mm_storeu_si128((__m128i *)q.w, _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12))); return q; }
static inline __attribute__((always_inline)) q_t shufb(q_t a, q_t b, q_t c) {
    __m128i A = q_be(a), B = q_be(b), C = q_be(c);
    __m128i sel = _mm_and_si128(C, _mm_set1_epi8(0x1F)), b4 = _mm_set1_epi8(0x10), hi = _mm_set1_epi8((char)0x80);
    __m128i fromB = _mm_cmpeq_epi8(_mm_and_si128(sel, b4), b4);                       /* index 16..31 */
    __m128i ra = _mm_shuffle_epi8(A, _mm_or_si128(sel, _mm_and_si128(fromB, hi)));     /* zero where from B */
    __m128i rb = _mm_shuffle_epi8(B, _mm_or_si128(sel, _mm_andnot_si128(fromB, hi)));  /* zero where from A */
    __m128i r = _mm_or_si128(ra, rb);
    __m128i m80 = _mm_cmplt_epi8(C, _mm_setzero_si128());                                /* 1xxxxxxx: constants */
    __m128i c0 = _mm_set1_epi8((char)0xC0), e0 = _mm_set1_epi8((char)0xE0);
    __m128i mC0 = _mm_cmpeq_epi8(_mm_and_si128(C, c0), c0), mE0 = _mm_cmpeq_epi8(_mm_and_si128(C, e0), e0);
    __m128i k = _mm_or_si128(_mm_andnot_si128(mE0, mC0), _mm_and_si128(mE0, hi));       /* 10: 00, 110: FF, 111: 80 */
    return be_q(_mm_or_si128(_mm_andnot_si128(m80, r), k));
}
static inline q_t qperm_bytes(q_t a, __m128i idx) { return be_q(_mm_shuffle_epi8(q_be(a), idx)); }   /* idx bit 7 = zero */
static inline q_t qshl_bytes(q_t a, int n) {
    __m128i i = _mm_add_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15), _mm_set1_epi8((char)(n > 16 ? 16 : n)));
    return qperm_bytes(a, _mm_or_si128(i, _mm_cmpgt_epi8(i, _mm_set1_epi8(15))));
}
static inline q_t qshr_bytes(q_t a, int n) {
    __m128i i = _mm_sub_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15), _mm_set1_epi8((char)(n > 16 ? 16 : n)));
    return qperm_bytes(a, _mm_or_si128(i, _mm_cmplt_epi8(i, _mm_setzero_si128())));
}
static inline q_t qrot_bytes(q_t a, int n) {
    __m128i i = _mm_and_si128(_mm_add_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15), _mm_set1_epi8((char)(n & 15))), _mm_set1_epi8(15));
    return qperm_bytes(a, i);
}
#else
static q_t shufb(q_t a, q_t b, q_t c) {
    q_t r;
    for (int i = 0; i < 16; i++) {
        uint8_t x = qb(&c, i), v;
        if ((x & 0xC0) == 0x80) v = 0;
        else if ((x & 0xE0) == 0xC0) v = 0xFF;
        else if ((x & 0xE0) == 0xE0) v = 0x80;
        else { x &= 0x1F; v = x < 16 ? qb(&a, x) : qb(&b, x - 16); }
        qsb(&r, i, v);
    }
    return r;
}
static q_t qshl_bytes(q_t a, int n) { q_t r; for (int i = 0; i < 16; i++) qsb(&r, i, i + n < 16 ? qb(&a, i + n) : 0); return r; }
static q_t qshr_bytes(q_t a, int n) { q_t r; for (int i = 0; i < 16; i++) qsb(&r, i, i - n >= 0 ? qb(&a, i - n) : 0); return r; }
static q_t qrot_bytes(q_t a, int n) { q_t r; for (int i = 0; i < 16; i++) qsb(&r, i, qb(&a, (i + n) & 15)); return r; }
#endif
static q_t qshl_bits(q_t a, int n) {   /* n in 0..7 */
    q_t r; if (!n) return a;
    for (int i = 0; i < 4; i++) r.w[i] = a.w[i] << n | (i < 3 ? a.w[i + 1] >> (32 - n) : 0);
    return r;
}
static q_t qshr_bits(q_t a, int n) {
    q_t r; if (!n) return a;
    for (int i = 0; i < 4; i++) r.w[i] = a.w[i] >> n | (i > 0 ? a.w[i - 1] << (32 - n) : 0);
    return r;
}
static q_t qrot_bits(q_t a, int n) {
    q_t r; if (!n) return a;
    for (int i = 0; i < 4; i++) r.w[i] = a.w[i] << n | a.w[(i + 1) & 3] >> (32 - n);
    return r;
}
static inline q_t gen_ctl(uint32_t t, int size) {   /* cbd/chd/cwd/cdd: insertion control for an element at byte t */
    q_t r = {{0x10111213u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu}};
    if (size == 1) { int sh = 24 - 8 * (int)(t & 3); r.w[(t >> 2) & 3] = (r.w[(t >> 2) & 3] & ~(0xFFu << sh)) | 0x03u << sh; }
    else if (size == 2) { int sh = (t & 2) ? 0 : 16; r.w[(t >> 2) & 3] = (r.w[(t >> 2) & 3] & ~(0xFFFFu << sh)) | 0x0203u << sh; }
    else if (size == 4) r.w[(t >> 2) & 3] = 0x00010203u;
    else { r.w[(t >> 2) & 2] = 0x00010203u; r.w[((t >> 2) & 2) + 1] = 0x04050607u; }
    return r;
}
static inline double p2i(int e) { union { uint64_t u; double d; } v; v.u = (uint64_t)(e + 1023) << 52; return v.d; }   /* 2^e, |e| < 1000 */
static int32_t cflts(float f, int scale) {
    double d = (double)f * p2i(scale);
    if (d != d) return 0;
    if (d >= 2147483647.0) return 0x7FFFFFFF;
    if (d <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)d;
}
static uint32_t cfltu(float f, int scale) {
    double d = (double)f * p2i(scale);
    if (d != d || d <= 0) return 0;
    if (d >= 4294967295.0) return 0xFFFFFFFF;
    return (uint32_t)d;
}

/* ------------------------------------------------------------------ interpreter */
static int unknown_reported;
static inline __attribute__((always_inline)) int spu_exec(spu_t *s, uint32_t w, uint32_t pc) {
    uint32_t rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F, rc;
    q_t *T = &s->r[rt], A = s->r[ra], B = s->r[rb], R;
    uint32_t next = (pc + 4) & LS_MASK;
    uint32_t op4 = w >> 28, op7 = w >> 25, op8 = w >> 24, op9 = w >> 23, op10 = w >> 22, op11 = w >> 21;
    int32_t i10 = sext((w >> 14) & 0x3FF, 10), i16 = sext((w >> 7) & 0xFFFF, 16), i7 = sext((w >> 14) & 0x7F, 7);
    uint32_t u16 = (w >> 7) & 0xFFFF;

    /* RRR */
    switch (op4) {
    case 0x8: rc = rt; rt = (w >> 21) & 0x7F; { q_t C = s->r[rc]; for (int i = 0; i < 4; i++) R.w[i] = (C.w[i] & B.w[i]) | (~C.w[i] & A.w[i]); s->r[rt] = R; } goto done;   /* selb */
    case 0xB: rc = rt; rt = (w >> 21) & 0x7F; s->r[rt] = shufb(A, B, s->r[rc]); goto done;
    case 0xC: rc = rt; rt = (w >> 21) & 0x7F; { q_t C = s->r[rc]; for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)((int32_t)(int16_t)A.w[i] * (int32_t)(int16_t)B.w[i] + (int32_t)C.w[i]); s->r[rt] = R; } goto done;   /* mpya */
    case 0xD: case 0xE: case 0xF: {    /* fnms, fma, fms */
        rc = rt; rt = (w >> 21) & 0x7F; q_t C = s->r[rc];
        __m128 p = _mm_mul_ps(qf4(&A), qf4(&B)), cv = qf4(&C);
        f2w4(&R, op4 == 0xE ? _mm_add_ps(p, cv) : op4 == 0xF ? _mm_sub_ps(p, cv) : _mm_sub_ps(cv, p));
        s->r[rt] = R; goto done;
    }
    }
    /* RI18 */
    switch (op7) {
    case 0x08: case 0x09: goto done;                        /* hbra, hbrr */
    case 0x21: *T = splat((w >> 7) & 0x3FFFF); goto done;   /* ila */
    }
    /* RI10 */
    switch (op8) {
    case 0x04: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] | (uint32_t)i10; *T = R; goto done;   /* ori */
    case 0x05: QS(&R, _mm_or_si128(QV(&A), _mm_set1_epi16((short)i10))); *T = R; goto done;
    case 0x06: QS(&R, _mm_or_si128(QV(&A), _mm_set1_epi8((char)i10))); *T = R; goto done;
    case 0x0C: for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)i10 - A.w[i]; *T = R; goto done;   /* sfi */
    case 0x0D: QS(&R, _mm_sub_epi16(_mm_set1_epi16((short)i10), QV(&A))); *T = R; goto done;
    case 0x14: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] & (uint32_t)i10; *T = R; goto done;   /* andi */
    case 0x15: QS(&R, _mm_and_si128(QV(&A), _mm_set1_epi16((short)i10))); *T = R; goto done;
    case 0x16: QS(&R, _mm_and_si128(QV(&A), _mm_set1_epi8((char)i10))); *T = R; goto done;
    case 0x1C: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] + (uint32_t)i10; *T = R; goto done;   /* ai */
    case 0x1D: QS(&R, _mm_add_epi16(QV(&A), _mm_set1_epi16((short)i10))); *T = R; goto done;
    case 0x24: stq(s, A.w[0] + (uint32_t)(i10 * 16), *T); goto done;   /* stqd */
    case 0x34: *T = ldq(s, A.w[0] + (uint32_t)(i10 * 16)); goto done;  /* lqd */
    case 0x44: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] ^ (uint32_t)i10; *T = R; goto done;
    case 0x45: QS(&R, _mm_xor_si128(QV(&A), _mm_set1_epi16((short)i10))); *T = R; goto done;
    case 0x46: QS(&R, _mm_xor_si128(QV(&A), _mm_set1_epi8((char)i10))); *T = R; goto done;
    case 0x4C: for (int i = 0; i < 4; i++) R.w[i] = (int32_t)A.w[i] > i10 ? ~0u : 0; *T = R; goto done;   /* cgti */
    case 0x4D: QS(&R, _mm_cmpgt_epi16(QV(&A), _mm_set1_epi16((short)i10))); *T = R; goto done;
    case 0x4E: QS(&R, _mm_cmpgt_epi8(QV(&A), _mm_set1_epi8((char)i10))); *T = R; goto done;
    case 0x4F: if ((int32_t)A.w[0] > i10) { s->stopped = 2; return 0; } goto done;   /* hgti */
    case 0x5C: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] > (uint32_t)i10 ? ~0u : 0; *T = R; goto done;   /* clgti */
    case 0x5D: QS(&R, cmpgtu16(QV(&A), _mm_set1_epi16((short)i10))); *T = R; goto done;
    case 0x5E: QS(&R, cmpgtu8(QV(&A), _mm_set1_epi8((char)i10))); *T = R; goto done;
    case 0x5F: if (A.w[0] > (uint32_t)i10) { s->stopped = 2; return 0; } goto done;
    case 0x74: for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)((int32_t)(int16_t)A.w[i] * (int32_t)(int16_t)i10); *T = R; goto done;   /* mpyi */
    case 0x75: for (int i = 0; i < 4; i++) R.w[i] = (A.w[i] & 0xFFFF) * ((uint32_t)i10 & 0xFFFF); *T = R; goto done;               /* mpyui */
    case 0x7C: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] == (uint32_t)i10 ? ~0u : 0; *T = R; goto done;   /* ceqi */
    case 0x7D: QS(&R, _mm_cmpeq_epi16(QV(&A), _mm_set1_epi16((short)i10))); *T = R; goto done;
    case 0x7E: QS(&R, _mm_cmpeq_epi8(QV(&A), _mm_set1_epi8((char)i10))); *T = R; goto done;
    case 0x7F: if (A.w[0] == (uint32_t)i10) { s->stopped = 2; return 0; } goto done;   /* heqi */
    }
    /* RI16 */
    switch (op9) {
    case 0x40: if (T->w[0] == 0) next = (pc + (uint32_t)(i16 << 2)) & LS_MASK; goto done;   /* brz */
    case 0x42: if (T->w[0] != 0) next = (pc + (uint32_t)(i16 << 2)) & LS_MASK; goto done;   /* brnz */
    case 0x44: if ((T->w[0] & 0xFFFF) == 0) next = (pc + (uint32_t)(i16 << 2)) & LS_MASK; goto done;
    case 0x46: if ((T->w[0] & 0xFFFF) != 0) next = (pc + (uint32_t)(i16 << 2)) & LS_MASK; goto done;
    case 0x41: stq(s, (uint32_t)(i16 << 2), *T); goto done;                        /* stqa */
    case 0x47: stq(s, pc + (uint32_t)(i16 << 2), *T); goto done;                   /* stqr */
    case 0x60: next = (uint32_t)(i16 << 2) & LS_MASK; goto done;                    /* bra */
    case 0x61: *T = ldq(s, (uint32_t)(i16 << 2)); goto done;                       /* lqa */
    case 0x62: *T = splat(0); T->w[0] = next; next = (uint32_t)(i16 << 2) & LS_MASK; goto done;   /* brasl */
    case 0x64: next = (pc + (uint32_t)(i16 << 2)) & LS_MASK; goto done;             /* br */
    case 0x65: R = fsm_bytes(u16); *T = R; goto done;   /* fsmbi */
    case 0x66: *T = splat(0); T->w[0] = next; next = (pc + (uint32_t)(i16 << 2)) & LS_MASK; goto done;   /* brsl */
    case 0x67: *T = ldq(s, pc + (uint32_t)(i16 << 2)); goto done;                  /* lqr */
    case 0x81: *T = splat((uint32_t)i16); goto done;                                /* il */
    case 0x82: *T = splat(u16 << 16); goto done;                                    /* ilhu */
    case 0x83: *T = splat(u16 << 16 | u16); goto done;                              /* ilh */
    case 0xC1: for (int i = 0; i < 4; i++) T->w[i] |= u16; goto done;               /* iohl */
    }
    /* RI8 */
    switch (op10) {
    case 0x1D8: { int sc = 173 - (int)((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)cflts(qf(&A, i), sc); *T = R; goto done; }
    case 0x1D9: { int sc = 173 - (int)((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) R.w[i] = cfltu(qf(&A, i), sc); *T = R; goto done; }
    case 0x1DA: { int sc = 155 - (int)((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) R.w[i] = f2w((float)((double)(int32_t)A.w[i] * p2i(-sc))); *T = R; goto done; }
    case 0x1DB: { int sc = 155 - (int)((w >> 14) & 0xFF); for (int i = 0; i < 4; i++) R.w[i] = f2w((float)((double)A.w[i] * p2i(-sc))); *T = R; goto done; }
    }
    /* RR / RI7 */
    switch (op11) {
    case 0x000: s->stop_code = w & 0x3FFF; s->stopped = 1; return 0;   /* stop */
    case 0x001: case 0x002: case 0x003: case 0x201: goto done;          /* lnop, sync, dsync, nop */
    case 0x140: s->stop_code = 0x3FFF; s->stopped = 1; return 0;        /* stopd */
    case 0x00C: *T = splat(0); goto done;                               /* mfspr */
    case 0x10C: goto done;                                               /* mtspr */
    case 0x00D: *T = splat(0); T->w[0] = rdch(s, ra); goto done;
    case 0x00F: *T = splat(0); T->w[0] = 1; goto done;                   /* rchcnt: ready */
    case 0x10D: wrch(s, ra, T->w[0]); goto done;
    case 0x040: for (int i = 0; i < 4; i++) R.w[i] = B.w[i] - A.w[i]; *T = R; goto done;   /* sf */
    case 0x041: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] | B.w[i]; *T = R; goto done;
    case 0x042: for (int i = 0; i < 4; i++) R.w[i] = B.w[i] >= A.w[i] ? 1 : 0; *T = R; goto done;   /* bg */
    case 0x048: QS(&R, _mm_sub_epi16(QV(&B), QV(&A))); *T = R; goto done;
    case 0x049: for (int i = 0; i < 4; i++) R.w[i] = ~(A.w[i] | B.w[i]); *T = R; goto done;
    case 0x053: for (int i = 0; i < 16; i++) { int d = (int)qb(&A, i) - (int)qb(&B, i); qsb(&R, i, (uint8_t)(d < 0 ? -d : d)); } *T = R; goto done;   /* absdb */
    case 0x058: for (int i = 0; i < 4; i++) { uint32_t n = B.w[i] & 31; R.w[i] = n ? A.w[i] << n | A.w[i] >> (32 - n) : A.w[i]; } *T = R; goto done;   /* rot */
    case 0x059: for (int i = 0; i < 4; i++) { uint32_t n = (0u - B.w[i]) & 63; R.w[i] = n < 32 ? A.w[i] >> n : 0; } *T = R; goto done;   /* rotm */
    case 0x05A: for (int i = 0; i < 4; i++) { uint32_t n = (0u - B.w[i]) & 63; R.w[i] = (uint32_t)((int32_t)A.w[i] >> (n < 32 ? n : 31)); } *T = R; goto done;   /* rotma */
    case 0x05B: for (int i = 0; i < 4; i++) { uint32_t n = B.w[i] & 63; R.w[i] = n < 32 ? A.w[i] << n : 0; } *T = R; goto done;   /* shl */
    case 0x05C: for (int i = 0; i < 8; i++) { uint32_t n = qh(&B, i) & 15; uint16_t v = qh(&A, i); qsh(&R, i, (uint16_t)(n ? v << n | v >> (16 - n) : v)); } *T = R; goto done;
    case 0x05D: for (int i = 0; i < 8; i++) { uint32_t n = (0u - qh(&B, i)) & 31; qsh(&R, i, (uint16_t)(n < 16 ? qh(&A, i) >> n : 0)); } *T = R; goto done;
    case 0x05E: for (int i = 0; i < 8; i++) { uint32_t n = (0u - qh(&B, i)) & 31; qsh(&R, i, (uint16_t)((int16_t)qh(&A, i) >> (n < 16 ? n : 15))); } *T = R; goto done;
    case 0x05F: for (int i = 0; i < 8; i++) { uint32_t n = qh(&B, i) & 31; qsh(&R, i, (uint16_t)(n < 16 ? qh(&A, i) << n : 0)); } *T = R; goto done;
    case 0x078: { uint32_t n = (uint32_t)i7 & 31; for (int i = 0; i < 4; i++) R.w[i] = n ? A.w[i] << n | A.w[i] >> (32 - n) : A.w[i]; *T = R; goto done; }   /* roti */
    case 0x079: { uint32_t n = (0u - (uint32_t)i7) & 63; for (int i = 0; i < 4; i++) R.w[i] = n < 32 ? A.w[i] >> n : 0; *T = R; goto done; }   /* rotmi */
    case 0x07A: { uint32_t n = (0u - (uint32_t)i7) & 63; for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)((int32_t)A.w[i] >> (n < 32 ? n : 31)); *T = R; goto done; }
    case 0x07B: { uint32_t n = (uint32_t)i7 & 63; for (int i = 0; i < 4; i++) R.w[i] = n < 32 ? A.w[i] << n : 0; *T = R; goto done; }   /* shli */
    case 0x07C: { uint32_t n = (uint32_t)i7 & 15; for (int i = 0; i < 8; i++) { uint16_t v = qh(&A, i); qsh(&R, i, (uint16_t)(n ? v << n | v >> (16 - n) : v)); } *T = R; goto done; }
    case 0x07D: { uint32_t n = (0u - (uint32_t)i7) & 31; for (int i = 0; i < 8; i++) qsh(&R, i, (uint16_t)(n < 16 ? qh(&A, i) >> n : 0)); *T = R; goto done; }
    case 0x07E: { uint32_t n = (0u - (uint32_t)i7) & 31; for (int i = 0; i < 8; i++) qsh(&R, i, (uint16_t)((int16_t)qh(&A, i) >> (n < 16 ? n : 15))); *T = R; goto done; }
    case 0x07F: { uint32_t n = (uint32_t)i7 & 31; for (int i = 0; i < 8; i++) qsh(&R, i, (uint16_t)(n < 16 ? qh(&A, i) << n : 0)); *T = R; goto done; }
    case 0x0C0: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] + B.w[i]; *T = R; goto done;   /* a */
    case 0x0C1: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] & B.w[i]; *T = R; goto done;
    case 0x0C2: for (int i = 0; i < 4; i++) R.w[i] = (uint64_t)A.w[i] + B.w[i] > 0xFFFFFFFFull ? 1 : 0; *T = R; goto done;   /* cg */
    case 0x0C8: QS(&R, _mm_add_epi16(QV(&A), QV(&B))); *T = R; goto done;
    case 0x0C9: for (int i = 0; i < 4; i++) R.w[i] = ~(A.w[i] & B.w[i]); *T = R; goto done;
    case 0x0D3: for (int i = 0; i < 16; i++) qsb(&R, i, (uint8_t)((qb(&A, i) + qb(&B, i) + 1) >> 1)); *T = R; goto done;   /* avgb */
    case 0x128: if (T->w[0] == 0) next = A.w[0] & LS_MASK & ~3u; goto done;            /* biz */
    case 0x129: if (T->w[0] != 0) next = A.w[0] & LS_MASK & ~3u; goto done;            /* binz */
    case 0x12A: if ((T->w[0] & 0xFFFF) == 0) next = A.w[0] & LS_MASK & ~3u; goto done;
    case 0x12B: if ((T->w[0] & 0xFFFF) != 0) next = A.w[0] & LS_MASK & ~3u; goto done;
    case 0x144: stq(s, A.w[0] + B.w[0], *T); goto done;                                 /* stqx */
    case 0x1A8: case 0x1AA: next = A.w[0] & LS_MASK & ~3u; goto done;                   /* bi, iret */
    case 0x1A9: case 0x1AB: { uint32_t t = A.w[0] & LS_MASK & ~3u; *T = splat(0); T->w[0] = next; next = t; goto done; }   /* bisl */
    case 0x1AC: goto done;                                                              /* hbr */
    case 0x1B0: { uint32_t v = 0; for (int i = 0; i < 4; i++) v = v << 1 | (A.w[i] & 1); *T = splat(0); T->w[0] = v; goto done; }   /* gb */
    case 0x1B1: { uint32_t v = 0; for (int i = 0; i < 8; i++) v = v << 1 | (qh(&A, i) & 1); *T = splat(0); T->w[0] = v; goto done; }
    case 0x1B2: { uint32_t v = 0; for (int i = 0; i < 16; i++) v = v << 1 | (qb(&A, i) & 1); *T = splat(0); T->w[0] = v; goto done; }
    case 0x1B4: { uint32_t m = A.w[0]; for (int i = 0; i < 4; i++) R.w[i] = (m >> (3 - i)) & 1 ? ~0u : 0; *T = R; goto done; }   /* fsm */
    case 0x1B5: { uint32_t m = A.w[0]; for (int i = 0; i < 8; i++) qsh(&R, i, (m >> (7 - i)) & 1 ? 0xFFFF : 0); *T = R; goto done; }
    case 0x1B6: { R = fsm_bytes(A.w[0] & 0xFFFF); *T = R; goto done; }
    case 0x1B8: f2w4(&R, _mm_div_ps(_mm_set1_ps(1.0f), qf4(&A))); *T = R; goto done;           /* frest (exact) */
    case 0x1B9: for (int i = 0; i < 4; i++) R.w[i] = f2w(1.0f / sqrtf(fabsf(qf(&A, i)))); *T = R; goto done;   /* frsqest (exact) */
    case 0x1C4: *T = ldq(s, A.w[0] + B.w[0]); goto done;                                /* lqx */
    case 0x1CC: *T = qrot_bytes(A, (int)((B.w[0] >> 3) & 15)); goto done;               /* rotqbybi */
    case 0x1CD: { uint32_t n = (0u - (B.w[0] >> 3)) & 31; *T = n < 16 ? qshr_bytes(A, (int)n) : splat(0); goto done; }   /* rotqmbybi */
    case 0x1CF: { uint32_t n = (B.w[0] >> 3) & 31; *T = n < 16 ? qshl_bytes(A, (int)n) : splat(0); goto done; }          /* shlqbybi */
    case 0x1D4: *T = gen_ctl(A.w[0] + B.w[0], 1); goto done;
    case 0x1D5: *T = gen_ctl(A.w[0] + B.w[0], 2); goto done;
    case 0x1D6: *T = gen_ctl(A.w[0] + B.w[0], 4); goto done;
    case 0x1D7: *T = gen_ctl(A.w[0] + B.w[0], 8); goto done;
    case 0x1D8: *T = qrot_bits(A, (int)(B.w[0] & 7)); goto done;                         /* rotqbi */
    case 0x1D9: *T = qshr_bits(A, (int)((0u - B.w[0]) & 7)); goto done;                  /* rotqmbi */
    case 0x1DB: *T = qshl_bits(A, (int)(B.w[0] & 7)); goto done;                         /* shlqbi */
    case 0x1DC: *T = qrot_bytes(A, (int)(B.w[0] & 15)); goto done;                       /* rotqby */
    case 0x1DD: { uint32_t n = (0u - B.w[0]) & 31; *T = n < 16 ? qshr_bytes(A, (int)n) : splat(0); goto done; }   /* rotqmby */
    case 0x1DF: { uint32_t n = B.w[0] & 31; *T = n < 16 ? qshl_bytes(A, (int)n) : splat(0); goto done; }          /* shlqby */
    case 0x1F0: { uint32_t v = A.w[0] | A.w[1] | A.w[2] | A.w[3]; *T = splat(0); T->w[0] = v; goto done; }         /* orx */
    case 0x1F4: *T = gen_ctl(A.w[0] + (uint32_t)i7, 1); goto done;   /* cbd */
    case 0x1F5: *T = gen_ctl(A.w[0] + (uint32_t)i7, 2); goto done;
    case 0x1F6: *T = gen_ctl(A.w[0] + (uint32_t)i7, 4); goto done;
    case 0x1F7: *T = gen_ctl(A.w[0] + (uint32_t)i7, 8); goto done;
    case 0x1F8: *T = qrot_bits(A, (int)((uint32_t)i7 & 7)); goto done;                  /* rotqbii */
    case 0x1F9: *T = qshr_bits(A, (int)((0u - (uint32_t)i7) & 7)); goto done;           /* rotqmbii */
    case 0x1FB: *T = qshl_bits(A, (int)((uint32_t)i7 & 7)); goto done;                  /* shlqbii */
    case 0x1FC: *T = qrot_bytes(A, (int)((uint32_t)i7 & 15)); goto done;                /* rotqbyi */
    case 0x1FD: { uint32_t n = (0u - (uint32_t)i7) & 31; *T = n < 16 ? qshr_bytes(A, (int)n) : splat(0); goto done; }   /* rotqmbyi */
    case 0x1FF: { uint32_t n = (uint32_t)i7 & 31; *T = n < 16 ? qshl_bytes(A, (int)n) : splat(0); goto done; }          /* shlqbyi */
    case 0x240: for (int i = 0; i < 4; i++) R.w[i] = (int32_t)A.w[i] > (int32_t)B.w[i] ? ~0u : 0; *T = R; goto done;   /* cgt */
    case 0x241: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] ^ B.w[i]; *T = R; goto done;
    case 0x248: QS(&R, _mm_cmpgt_epi16(QV(&A), QV(&B))); *T = R; goto done;
    case 0x249: for (int i = 0; i < 4; i++) R.w[i] = ~(A.w[i] ^ B.w[i]); *T = R; goto done;   /* eqv */
    case 0x250: QS(&R, _mm_cmpgt_epi8(QV(&A), QV(&B))); *T = R; goto done;
    case 0x253: for (int i = 0; i < 4; i++) {   /* sumb: hi half = sum of B's bytes, lo half = sum of A's bytes */
        uint32_t sa = 0, sb = 0;
        for (int k = 0; k < 4; k++) { sa += qb(&A, 4 * i + k); sb += qb(&B, 4 * i + k); }
        R.w[i] = sb << 16 | (sa & 0xFFFF);
    } *T = R; goto done;
    case 0x258: if ((int32_t)A.w[0] > (int32_t)B.w[0]) { s->stopped = 2; return 0; } goto done;   /* hgt */
    case 0x2A5: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] ? (uint32_t)__builtin_clz(A.w[i]) : 32; *T = R; goto done;   /* clz */
    case 0x2A6: for (int i = 0; i < 2; i++) { int64_t v = (int32_t)A.w[2 * i + 1]; R.w[2 * i] = (uint32_t)(v >> 32); R.w[2 * i + 1] = (uint32_t)v; } *T = R; goto done;   /* xswd */
    case 0x2AE: for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)(int32_t)(int16_t)A.w[i]; *T = R; goto done;   /* xshw */
    case 0x2B4: for (int i = 0; i < 16; i++) qsb(&R, i, (uint8_t)__builtin_popcount(qb(&A, i))); *T = R; goto done;   /* cntb */
    case 0x2B6: for (int i = 0; i < 8; i++) qsh(&R, i, (uint16_t)(int16_t)(int8_t)(qh(&A, i) & 0xFF)); *T = R; goto done;   /* xsbh */
    case 0x2C0: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] > B.w[i] ? ~0u : 0; *T = R; goto done;   /* clgt */
    case 0x2C1: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] & ~B.w[i]; *T = R; goto done;          /* andc */
    case 0x2C2: m2w4(&R, _mm_cmpgt_ps(qf4(&A), qf4(&B))); *T = R; goto done;   /* fcgt */
    case 0x2C4: f2w4(&R, _mm_add_ps(qf4(&A), qf4(&B))); *T = R; goto done;   /* fa */
    case 0x2C5: f2w4(&R, _mm_sub_ps(qf4(&A), qf4(&B))); *T = R; goto done;   /* fs */
    case 0x2C6: f2w4(&R, _mm_mul_ps(qf4(&A), qf4(&B))); *T = R; goto done;   /* fm */
    /* double precision: two doublewords, slot i = w[2i] (high) : w[2i+1] */
    #define QD(q, i) qd(&(q), i)
    #define SD(i, v) sd(&R, i, v)
    case 0x2CC: for (int i = 0; i < 2; i++) SD(i, QD(A, i) + QD(B, i)); *T = R; goto done;   /* dfa */
    case 0x2CD: for (int i = 0; i < 2; i++) SD(i, QD(A, i) - QD(B, i)); *T = R; goto done;   /* dfs */
    case 0x2CE: for (int i = 0; i < 2; i++) SD(i, QD(A, i) * QD(B, i)); *T = R; goto done;   /* dfm */
    case 0x35C: for (int i = 0; i < 2; i++) SD(i, QD(A, i) * QD(B, i) + QD(*T, i)); *T = R; goto done;     /* dfma */
    case 0x35D: for (int i = 0; i < 2; i++) SD(i, QD(A, i) * QD(B, i) - QD(*T, i)); *T = R; goto done;     /* dfms */
    case 0x35E: for (int i = 0; i < 2; i++) SD(i, QD(*T, i) - QD(A, i) * QD(B, i)); *T = R; goto done;     /* dfnms */
    case 0x35F: for (int i = 0; i < 2; i++) SD(i, -(QD(A, i) * QD(B, i) + QD(*T, i))); *T = R; goto done;  /* dfnma */
    case 0x3C3: for (int i = 0; i < 2; i++) R.w[2 * i] = R.w[2 * i + 1] = QD(A, i) == QD(B, i) ? ~0u : 0; *T = R; goto done;   /* dfceq */
    case 0x2C3: for (int i = 0; i < 2; i++) R.w[2 * i] = R.w[2 * i + 1] = QD(A, i) > QD(B, i) ? ~0u : 0; *T = R; goto done;    /* dfcgt */
    case 0x3CB: for (int i = 0; i < 2; i++) R.w[2 * i] = R.w[2 * i + 1] = fabs(QD(A, i)) == fabs(QD(B, i)) ? ~0u : 0; *T = R; goto done;   /* dfcmeq */
    case 0x2CB: for (int i = 0; i < 2; i++) R.w[2 * i] = R.w[2 * i + 1] = fabs(QD(A, i)) > fabs(QD(B, i)) ? ~0u : 0; *T = R; goto done;    /* dfcmgt */
    case 0x3B8: for (int i = 0; i < 2; i++) SD(i, (double)qf(&A, 2 * i)); *T = R; goto done;   /* fesd */
    case 0x3B9: for (int i = 0; i < 2; i++) { R.w[2 * i] = f2w((float)QD(A, i)); R.w[2 * i + 1] = 0; } *T = R; goto done;   /* frds */
    #undef QD
    #undef SD
    case 0x2C8: QS(&R, cmpgtu16(QV(&A), QV(&B))); *T = R; goto done;   /* clgth */
    case 0x2C9: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] | ~B.w[i]; *T = R; goto done;          /* orc */
    case 0x2CA: for (int i = 0; i < 4; i++) R.w[i] = fabsf(qf(&A, i)) > fabsf(qf(&B, i)) ? ~0u : 0; *T = R; goto done;   /* fcmgt */
    case 0x2D0: QS(&R, cmpgtu8(QV(&A), QV(&B))); *T = R; goto done;   /* clgtb */
    case 0x2D8: if (A.w[0] > B.w[0]) { s->stopped = 2; return 0; } goto done;   /* hlgt */
    case 0x340: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] + B.w[i] + (T->w[i] & 1); *T = R; goto done;   /* addx */
    case 0x341: for (int i = 0; i < 4; i++) R.w[i] = B.w[i] - A.w[i] - ((T->w[i] & 1) ? 0 : 1); *T = R; goto done;   /* sfx */
    case 0x342: for (int i = 0; i < 4; i++) R.w[i] = (uint64_t)A.w[i] + B.w[i] + (T->w[i] & 1) > 0xFFFFFFFFull ? 1 : 0; *T = R; goto done;   /* cgx */
    case 0x343: for (int i = 0; i < 4; i++) R.w[i] = (int64_t)B.w[i] - A.w[i] - ((T->w[i] & 1) ? 0 : 1) >= 0 ? 1 : 0; *T = R; goto done;   /* bgx */
    case 0x346: for (int i = 0; i < 4; i++) T->w[i] += (uint32_t)((int32_t)(int16_t)(A.w[i] >> 16) * (int32_t)(int16_t)(B.w[i] >> 16)); goto done;   /* mpyhha */
    case 0x34E: for (int i = 0; i < 4; i++) T->w[i] += (A.w[i] >> 16) * (B.w[i] >> 16); goto done;   /* mpyhhau */
    case 0x398: *T = splat(0); goto done;   /* fscrrd */
    case 0x3BA: goto done;                  /* fscrwr */
    case 0x3C0: for (int i = 0; i < 4; i++) R.w[i] = A.w[i] == B.w[i] ? ~0u : 0; *T = R; goto done;   /* ceq */
    case 0x3C2: m2w4(&R, _mm_cmpeq_ps(qf4(&A), qf4(&B))); *T = R; goto done;   /* fceq */
    case 0x3C4: for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)((int32_t)(int16_t)A.w[i] * (int32_t)(int16_t)B.w[i]); *T = R; goto done;   /* mpy */
    case 0x3C5: for (int i = 0; i < 4; i++) R.w[i] = ((A.w[i] >> 16) * (B.w[i] & 0xFFFF)) << 16; *T = R; goto done;   /* mpyh */
    case 0x3C6: for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)((int32_t)(int16_t)(A.w[i] >> 16) * (int32_t)(int16_t)(B.w[i] >> 16)); *T = R; goto done;   /* mpyhh */
    case 0x3C7: for (int i = 0; i < 4; i++) R.w[i] = (uint32_t)(((int32_t)(int16_t)A.w[i] * (int32_t)(int16_t)B.w[i]) >> 16); *T = R; goto done;   /* mpys */
    case 0x3C8: QS(&R, _mm_cmpeq_epi16(QV(&A), QV(&B))); *T = R; goto done;   /* ceqh */
    case 0x3CA: for (int i = 0; i < 4; i++) R.w[i] = fabsf(qf(&A, i)) == fabsf(qf(&B, i)) ? ~0u : 0; *T = R; goto done;   /* fcmeq */
    case 0x3CC: for (int i = 0; i < 4; i++) R.w[i] = (A.w[i] & 0xFFFF) * (B.w[i] & 0xFFFF); *T = R; goto done;   /* mpyu */
    case 0x3CE: for (int i = 0; i < 4; i++) R.w[i] = (A.w[i] >> 16) * (B.w[i] >> 16); *T = R; goto done;   /* mpyhhu */
    case 0x3D0: QS(&R, _mm_cmpeq_epi8(QV(&A), QV(&B))); *T = R; goto done;   /* ceqb */
    case 0x3D4: *T = B; goto done;   /* fi: frest/frsqest already return exact values */
    case 0x3D8: if (A.w[0] == B.w[0]) { s->stopped = 2; return 0; } goto done;   /* heq */
    }
    if (unknown_reported++ < 20) fprintf(stderr, "[spu] unknown instruction %08x at 0x%05x\n", w, pc);
    s->stopped = 3;
    return 0;
done:
    s->pc = next;
    return 1;
}

static int step(spu_t *s) { return spu_exec(s, rd32(s, s->pc), s->pc); }
