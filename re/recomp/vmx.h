/* AltiVec / VMX (Cell PPU) helpers for recompiled code.
 * Vector registers are 16 bytes in guest (big-endian) byte order: byte i is element byte i, so
 * byte-granular ops (vperm, vsldoi, lvsl/lvsr, Cell lvlx/lvrx/stvlx/stvrx) index directly, and
 * word/float lanes are read and written through byte-swapping accessors. Every op computes into a
 * temporary first, so destination aliasing a source is safe. */
#pragma once
#include <immintrin.h>
#define VINL static inline __attribute__((always_inline))

typedef struct { uint8_t b[16]; } vec_t;

VINL uint32_t vw_get(const uint8_t *v, int i) { uint32_t x; memcpy(&x, v + 4 * i, 4); return __builtin_bswap32(x); }
VINL void vw_set(uint8_t *v, int i, uint32_t x) { x = __builtin_bswap32(x); memcpy(v + 4 * i, &x, 4); }
VINL uint16_t vh_get(const uint8_t *v, int i) { uint16_t x; memcpy(&x, v + 2 * i, 2); return __builtin_bswap16(x); }
VINL void vh_set(uint8_t *v, int i, uint16_t x) { x = __builtin_bswap16(x); memcpy(v + 2 * i, &x, 2); }
VINL float vf_get(const uint8_t *v, int i) { uint32_t x = vw_get(v, i); float f; memcpy(&f, &x, 4); return f; }
VINL void vf_set(uint8_t *v, int i, float f) { uint32_t x; memcpy(&x, &f, 4); vw_set(v, i, x); }

#define VR(n) (c->v[n])
#define V_PUT(d, t) memcpy(c->v[d], (t).b, 16)

/* --- per-word / per-float templates */
#define V_WORD_OP(name, expr) \
    VINL void name(ctx_t *c, int d, int a, int b) { vec_t t; \
        for (int i = 0; i < 4; i++) { uint32_t x = vw_get(VR(a), i), y = vw_get(VR(b), i); vw_set(t.b, i, (uint32_t)(expr)); } \
        V_PUT(d, t); }
#define V_FLOAT_OP(name, expr) \
    VINL void name(ctx_t *c, int d, int a, int b) { vec_t t; \
        for (int i = 0; i < 4; i++) { float x = vf_get(VR(a), i), y = vf_get(VR(b), i); vf_set(t.b, i, (float)(expr)); } \
        V_PUT(d, t); }
#define V_HALF_OP(name, expr) \
    VINL void name(ctx_t *c, int d, int a, int b) { vec_t t; \
        for (int i = 0; i < 8; i++) { uint16_t x = vh_get(VR(a), i), y = vh_get(VR(b), i); vh_set(t.b, i, (uint16_t)(expr)); } \
        V_PUT(d, t); }
#define V_BYTE_OP(name, expr) \
    VINL void name(ctx_t *c, int d, int a, int b) { vec_t t; \
        for (int i = 0; i < 16; i++) { uint8_t x = VR(a)[i], y = VR(b)[i]; t.b[i] = (uint8_t)(expr); } \
        V_PUT(d, t); }

VINL int32_t sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }
VINL uint32_t satu32(int64_t v) { return v > (int64_t)UINT32_MAX ? UINT32_MAX : v < 0 ? 0 : (uint32_t)v; }

V_FLOAT_OP(v_vmaxfp, (x > y || y != y) ? x : y)
V_FLOAT_OP(v_vminfp, (x < y || y != y) ? x : y)
V_WORD_OP(v_vadduws, satu32((int64_t)x + y))
V_WORD_OP(v_vsubuws, satu32((int64_t)x - (int64_t)y))
V_WORD_OP(v_vaddsws, sat32((int64_t)(int32_t)x + (int32_t)y))
V_WORD_OP(v_vsubsws, sat32((int64_t)(int32_t)x - (int32_t)y))
V_WORD_OP(v_vminuw, x < y ? x : y)
V_WORD_OP(v_vmaxuw, x > y ? x : y)
V_WORD_OP(v_vrlw, (y & 31) ? (x << (y & 31)) | (x >> (32 - (y & 31))) : x)
V_HALF_OP(v_vadduhm, x + y)
V_HALF_OP(v_vsubuhm, x - y)
V_HALF_OP(v_vadduhs, (x + y) > 0xFFFF ? 0xFFFF : x + y)
V_HALF_OP(v_vslh, x << (y & 15))
V_HALF_OP(v_vsrh, x >> (y & 15))
V_BYTE_OP(v_vaddubm, x + y)
V_BYTE_OP(v_vsububm, x - y)

VINL void v_vrsqrtefp(ctx_t *c, int d, int b) { vec_t t; for (int i = 0; i < 4; i++) vf_set(t.b, i, 1.0f / sqrtf(vf_get(VR(b), i))); V_PUT(d, t); }
VINL void v_vexptefp(ctx_t *c, int d, int b) { vec_t t; for (int i = 0; i < 4; i++) vf_set(t.b, i, exp2f(vf_get(VR(b), i))); V_PUT(d, t); }
VINL void v_vlogefp(ctx_t *c, int d, int b) { vec_t t; for (int i = 0; i < 4; i++) vf_set(t.b, i, log2f(vf_get(VR(b), i))); V_PUT(d, t); }
VINL void v_vrfi(ctx_t *c, int d, int b, int mode) {  /* 0 nearest, 1 toward zero, 2 +inf, 3 -inf */
    vec_t t;
    for (int i = 0; i < 4; i++) {
        float x = vf_get(VR(b), i);
        vf_set(t.b, i, mode == 0 ? nearbyintf(x) : mode == 1 ? truncf(x) : mode == 2 ? ceilf(x) : floorf(x));
    }
    V_PUT(d, t);
}
VINL void v_vcfsx(ctx_t *c, int d, int b, int uimm) {
    vec_t t; for (int i = 0; i < 4; i++) vf_set(t.b, i, (float)(int32_t)vw_get(VR(b), i) / (float)(1u << uimm)); V_PUT(d, t);
}
VINL void v_vcfux(ctx_t *c, int d, int b, int uimm) {
    vec_t t; for (int i = 0; i < 4; i++) vf_set(t.b, i, (float)vw_get(VR(b), i) / (float)(1u << uimm)); V_PUT(d, t);
}
VINL void v_vctsxs(ctx_t *c, int d, int b, int uimm) {
    vec_t t;
    for (int i = 0; i < 4; i++) {
        double x = (double)vf_get(VR(b), i) * (double)(1u << uimm);
        vw_set(t.b, i, (uint32_t)(x != x ? 0 : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : (int32_t)x));
    }
    V_PUT(d, t);
}
VINL void v_vctuxs(ctx_t *c, int d, int b, int uimm) {
    vec_t t;
    for (int i = 0; i < 4; i++) {
        double x = (double)vf_get(VR(b), i) * (double)(1u << uimm);
        vw_set(t.b, i, x != x || x <= 0 ? 0u : x >= 4294967295.0 ? UINT32_MAX : (uint32_t)x);
    }
    V_PUT(d, t);
}

/* --- compares (record forms set CR6: 8 = all true, 2 = all false) */
VINL void v_cr6(ctx_t *c, const vec_t *t, int rc) {
    if (!rc) return;
    __m128i v = _mm_loadu_si128((const __m128i *)t->b);
    int all = _mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(-1))) == 0xFFFF, none = _mm_testz_si128(v, v);
    set_cr_field(c, 6, (all ? 8 : 0) | (none ? 2 : 0));
}
#define V_CMP_F(name, expr) \
    VINL void name(ctx_t *c, int d, int a, int b, int rc) { vec_t t; \
        for (int i = 0; i < 4; i++) { float x = vf_get(VR(a), i), y = vf_get(VR(b), i); vw_set(t.b, i, (expr) ? 0xFFFFFFFFu : 0); } \
        v_cr6(c, &t, rc); V_PUT(d, t); }
#define V_CMP_W(name, expr) \
    VINL void name(ctx_t *c, int d, int a, int b, int rc) { vec_t t; \
        for (int i = 0; i < 4; i++) { uint32_t x = vw_get(VR(a), i), y = vw_get(VR(b), i); vw_set(t.b, i, (expr) ? 0xFFFFFFFFu : 0); } \
        v_cr6(c, &t, rc); V_PUT(d, t); }

/* --- SSE forms of the hot ops. Registers stay in guest byte order: bitwise and byte-shuffle ops work on it
 * directly; word and float lanes are byte-swapped in and out with pshufb. */
VINL __m128i vx_ld(ctx_t *c, int r) { return _mm_loadu_si128((const __m128i *)c->v[r]); }
VINL void vx_st(ctx_t *c, int r, __m128i x) { _mm_storeu_si128((__m128i *)c->v[r], x); }
VINL __m128i vx_bsw(__m128i x) { return _mm_shuffle_epi8(x, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)); }
VINL __m128i vx_w(ctx_t *c, int r) { return vx_bsw(vx_ld(c, r)); }                       /* host-order words */
VINL void vx_stw(ctx_t *c, int r, __m128i x) { vx_st(c, r, vx_bsw(x)); }
VINL __m128 vx_f(ctx_t *c, int r) { return _mm_castsi128_ps(vx_w(c, r)); }
VINL void vx_stf(ctx_t *c, int r, __m128 x) { vx_stw(c, r, _mm_castps_si128(x)); }

VINL void v_vand(ctx_t *c, int d, int a, int b) { vx_st(c, d, _mm_and_si128(vx_ld(c, a), vx_ld(c, b))); }
VINL void v_vandc(ctx_t *c, int d, int a, int b) { vx_st(c, d, _mm_andnot_si128(vx_ld(c, b), vx_ld(c, a))); }
VINL void v_vor(ctx_t *c, int d, int a, int b) { vx_st(c, d, _mm_or_si128(vx_ld(c, a), vx_ld(c, b))); }
VINL void v_vnor(ctx_t *c, int d, int a, int b) { vx_st(c, d, _mm_xor_si128(_mm_or_si128(vx_ld(c, a), vx_ld(c, b)), _mm_set1_epi8(-1))); }
VINL void v_vxor(ctx_t *c, int d, int a, int b) { vx_st(c, d, _mm_xor_si128(vx_ld(c, a), vx_ld(c, b))); }
VINL void v_vsel(ctx_t *c, int d, int a, int b, int cc) { __m128i m = vx_ld(c, cc); vx_st(c, d, _mm_or_si128(_mm_andnot_si128(m, vx_ld(c, a)), _mm_and_si128(m, vx_ld(c, b)))); }
VINL void v_vperm(ctx_t *c, int d, int a, int b, int cc) {   /* byte i = (a:b)[cc[i] & 31] */
    __m128i ix = _mm_and_si128(vx_ld(c, cc), _mm_set1_epi8(15)), hi = _mm_slli_epi16(vx_ld(c, cc), 3);   /* bit 4 -> bit 7 */
    __m128i ra = _mm_shuffle_epi8(vx_ld(c, a), ix), rb = _mm_shuffle_epi8(vx_ld(c, b), ix);
    vx_st(c, d, _mm_blendv_epi8(ra, rb, hi));
}
VINL void v_vspltw(ctx_t *c, int d, int b, int u) {
    __m128i m = _mm_add_epi8(_mm_set1_epi8((char)(4 * (u & 3))), _mm_setr_epi8(0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3));
    vx_st(c, d, _mm_shuffle_epi8(vx_ld(c, b), m));
}
VINL void v_vspltisw(ctx_t *c, int d, int s) { vx_stw(c, d, _mm_set1_epi32(s)); }
VINL void v_vmrghw(ctx_t *c, int d, int a, int b) { vx_st(c, d, _mm_unpacklo_epi32(vx_ld(c, a), vx_ld(c, b))); }
VINL void v_vmrglw(ctx_t *c, int d, int a, int b) { vx_st(c, d, _mm_unpackhi_epi32(vx_ld(c, a), vx_ld(c, b))); }

VINL void v_vadduwm(ctx_t *c, int d, int a, int b) { vx_stw(c, d, _mm_add_epi32(vx_w(c, a), vx_w(c, b))); }
VINL void v_vsubuwm(ctx_t *c, int d, int a, int b) { vx_stw(c, d, _mm_sub_epi32(vx_w(c, a), vx_w(c, b))); }
/* Per-lane shifts: AVX2 has them; the baseline build (any x86-64 with SSE4.2) shifts lane by lane. */
#ifdef __AVX2__
VINL void v_vslw(ctx_t *c, int d, int a, int b) { vx_stw(c, d, _mm_sllv_epi32(vx_w(c, a), _mm_and_si128(vx_w(c, b), _mm_set1_epi32(31)))); }
VINL void v_vsrw(ctx_t *c, int d, int a, int b) { vx_stw(c, d, _mm_srlv_epi32(vx_w(c, a), _mm_and_si128(vx_w(c, b), _mm_set1_epi32(31)))); }
VINL void v_vsraw(ctx_t *c, int d, int a, int b) { vx_stw(c, d, _mm_srav_epi32(vx_w(c, a), _mm_and_si128(vx_w(c, b), _mm_set1_epi32(31)))); }
#else
VINL void v_vslw(ctx_t *c, int d, int a, int b) { for (int i = 0; i < 4; i++) vw_set(c->v[d], i, vw_get(c->v[a], i) << (vw_get(c->v[b], i) & 31)); }
VINL void v_vsrw(ctx_t *c, int d, int a, int b) { for (int i = 0; i < 4; i++) vw_set(c->v[d], i, vw_get(c->v[a], i) >> (vw_get(c->v[b], i) & 31)); }
VINL void v_vsraw(ctx_t *c, int d, int a, int b) { for (int i = 0; i < 4; i++) vw_set(c->v[d], i, (uint32_t)((int32_t)vw_get(c->v[a], i) >> (vw_get(c->v[b], i) & 31))); }
#endif
VINL void v_vminsw(ctx_t *c, int d, int a, int b) { vx_stw(c, d, _mm_min_epi32(vx_w(c, a), vx_w(c, b))); }
VINL void v_vmaxsw(ctx_t *c, int d, int a, int b) { vx_stw(c, d, _mm_max_epi32(vx_w(c, a), vx_w(c, b))); }

VINL void v_vaddfp(ctx_t *c, int d, int a, int b) { vx_stf(c, d, _mm_add_ps(vx_f(c, a), vx_f(c, b))); }
VINL void v_vsubfp(ctx_t *c, int d, int a, int b) { vx_stf(c, d, _mm_sub_ps(vx_f(c, a), vx_f(c, b))); }
/* Fused multiply-add (one rounding, as the Cell). Without FMA hardware (the baseline build): in double precision,
 * where the product of two floats is exact, so only the sum is rounded (twice, to double then float: the same
 * result except in vanishingly rare ties). */
#ifdef __FMA__
VINL __m128 vx_fma(__m128 a, __m128 b, __m128 c) { return _mm_fmadd_ps(a, b, c); }
#else
VINL __m128 vx_fma(__m128 a, __m128 b, __m128 c) {
    __m128d lo = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(a), _mm_cvtps_pd(b)), _mm_cvtps_pd(c));
    __m128 ah = _mm_movehl_ps(a, a), bh = _mm_movehl_ps(b, b), ch = _mm_movehl_ps(c, c);
    __m128d hi = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(ah), _mm_cvtps_pd(bh)), _mm_cvtps_pd(ch));
    return _mm_movelh_ps(_mm_cvtpd_ps(lo), _mm_cvtpd_ps(hi));
}
#endif
VINL void v_vmaddfp(ctx_t *c, int d, int a, int cc, int b) { vx_stf(c, d, vx_fma(vx_f(c, a), vx_f(c, cc), vx_f(c, b))); }   /* a*c + b, single rounding */
VINL void v_vnmsubfp(ctx_t *c, int d, int a, int cc, int b) {   /* -(a*c - b) = b - a*c */
    vx_stf(c, d, vx_fma(_mm_xor_ps(vx_f(c, a), _mm_set1_ps(-0.0f)), vx_f(c, cc), vx_f(c, b)));
}
VINL void v_vrefp(ctx_t *c, int d, int b) { vx_stf(c, d, _mm_div_ps(_mm_set1_ps(1.0f), vx_f(c, b))); }

VINL void vx_cmp(ctx_t *c, int d, __m128i m, int rc) {   /* m: host-order lane masks (byte-order free: all ones or zero) */
    vx_st(c, d, m);
    if (rc) { int all = _mm_movemask_epi8(m) == 0xFFFF, none = _mm_testz_si128(m, m); set_cr_field(c, 6, (all ? 8 : 0) | (none ? 2 : 0)); }
}
VINL void v_vcmpeqfp(ctx_t *c, int d, int a, int b, int rc) { vx_cmp(c, d, _mm_castps_si128(_mm_cmpeq_ps(vx_f(c, a), vx_f(c, b))), rc); }
VINL void v_vcmpgefp(ctx_t *c, int d, int a, int b, int rc) { vx_cmp(c, d, _mm_castps_si128(_mm_cmpge_ps(vx_f(c, a), vx_f(c, b))), rc); }
VINL void v_vcmpgtfp(ctx_t *c, int d, int a, int b, int rc) { vx_cmp(c, d, _mm_castps_si128(_mm_cmpgt_ps(vx_f(c, a), vx_f(c, b))), rc); }
VINL void v_vcmpequw(ctx_t *c, int d, int a, int b, int rc) { vx_cmp(c, d, _mm_cmpeq_epi32(vx_ld(c, a), vx_ld(c, b)), rc); }
VINL void v_vcmpgtsw(ctx_t *c, int d, int a, int b, int rc) { vx_cmp(c, d, _mm_cmpgt_epi32(vx_w(c, a), vx_w(c, b)), rc); }
VINL void v_vcmpgtuw(ctx_t *c, int d, int a, int b, int rc) {
    __m128i k = _mm_set1_epi32((int)0x80000000);
    vx_cmp(c, d, _mm_cmpgt_epi32(_mm_xor_si128(vx_w(c, a), k), _mm_xor_si128(vx_w(c, b), k)), rc);
}
VINL void v_vcmpgtuh(ctx_t *c, int d, int a, int b, int rc) {
    vec_t t; for (int i = 0; i < 8; i++) vh_set(t.b, i, vh_get(VR(a), i) > vh_get(VR(b), i) ? 0xFFFF : 0); v_cr6(c, &t, rc); V_PUT(d, t);
}
VINL void v_vcmpequb(ctx_t *c, int d, int a, int b, int rc) {
    vec_t t; for (int i = 0; i < 16; i++) t.b[i] = VR(a)[i] == VR(b)[i] ? 0xFF : 0; v_cr6(c, &t, rc); V_PUT(d, t);
}
VINL void v_vcmpbfp(ctx_t *c, int d, int a, int b, int rc) {  /* bounds: bit0 = !(a <= b), bit1 = !(a >= -b) */
    vec_t t; int in = 1;
    for (int i = 0; i < 4; i++) {
        float x = vf_get(VR(a), i), y = vf_get(VR(b), i);
        uint32_t r = (!(x <= y) ? 0x80000000u : 0) | (!(x >= -y) ? 0x40000000u : 0);
        if (r) in = 0;
        vw_set(t.b, i, r);
    }
    if (rc) set_cr_field(c, 6, in ? 2 : 0);
    V_PUT(d, t);
}

/* --- permutes, splats, merges, packs */
VINL void v_vsldoi(ctx_t *c, int d, int a, int b, int sh) {
    vec_t t; uint8_t src[32]; memcpy(src, VR(a), 16); memcpy(src + 16, VR(b), 16);
    memcpy(t.b, src + sh, 16); V_PUT(d, t);
}
VINL void v_vsplth(ctx_t *c, int d, int b, int u) { vec_t t; uint16_t x = vh_get(VR(b), u & 7); for (int i = 0; i < 8; i++) vh_set(t.b, i, x); V_PUT(d, t); }
VINL void v_vspltb(ctx_t *c, int d, int b, int u) { vec_t t; memset(t.b, VR(b)[u & 15], 16); V_PUT(d, t); }
VINL void v_vspltish(ctx_t *c, int d, int s) { vec_t t; for (int i = 0; i < 8; i++) vh_set(t.b, i, (uint16_t)(int16_t)s); V_PUT(d, t); }
VINL void v_vspltisb(ctx_t *c, int d, int s) { vec_t t; memset(t.b, (uint8_t)(int8_t)s, 16); V_PUT(d, t); }
VINL void v_vmrghh(ctx_t *c, int d, int a, int b) { vec_t t; for (int i = 0; i < 4; i++) { vh_set(t.b, 2 * i, vh_get(VR(a), i)); vh_set(t.b, 2 * i + 1, vh_get(VR(b), i)); } V_PUT(d, t); }
VINL void v_vmrglh(ctx_t *c, int d, int a, int b) { vec_t t; for (int i = 0; i < 4; i++) { vh_set(t.b, 2 * i, vh_get(VR(a), i + 4)); vh_set(t.b, 2 * i + 1, vh_get(VR(b), i + 4)); } V_PUT(d, t); }
VINL void v_vmrghb(ctx_t *c, int d, int a, int b) { vec_t t; for (int i = 0; i < 8; i++) { t.b[2 * i] = VR(a)[i]; t.b[2 * i + 1] = VR(b)[i]; } V_PUT(d, t); }
VINL void v_vmrglb(ctx_t *c, int d, int a, int b) { vec_t t; for (int i = 0; i < 8; i++) { t.b[2 * i] = VR(a)[i + 8]; t.b[2 * i + 1] = VR(b)[i + 8]; } V_PUT(d, t); }
VINL void v_vpkuwum(ctx_t *c, int d, int a, int b) {
    vec_t t; for (int i = 0; i < 4; i++) { vh_set(t.b, i, (uint16_t)vw_get(VR(a), i)); vh_set(t.b, i + 4, (uint16_t)vw_get(VR(b), i)); } V_PUT(d, t);
}
VINL void v_vpkuhum(ctx_t *c, int d, int a, int b) {
    vec_t t; for (int i = 0; i < 8; i++) { t.b[i] = (uint8_t)vh_get(VR(a), i); t.b[i + 8] = (uint8_t)vh_get(VR(b), i); } V_PUT(d, t);
}
VINL void v_vmulosh(ctx_t *c, int d, int a, int b) {  /* odd signed halfwords -> words */
    vec_t t; for (int i = 0; i < 4; i++) vw_set(t.b, i, (uint32_t)((int32_t)(int16_t)vh_get(VR(a), 2 * i + 1) * (int16_t)vh_get(VR(b), 2 * i + 1))); V_PUT(d, t);
}
VINL void v_vmulouh(ctx_t *c, int d, int a, int b) {
    vec_t t; for (int i = 0; i < 4; i++) vw_set(t.b, i, (uint32_t)vh_get(VR(a), 2 * i + 1) * vh_get(VR(b), 2 * i + 1)); V_PUT(d, t);
}
VINL void v_vmsumshm(ctx_t *c, int d, int a, int b, int cc) {
    vec_t t;
    for (int i = 0; i < 4; i++) {
        int32_t s = (int32_t)vw_get(VR(cc), i);
        for (int j = 0; j < 2; j++) s += (int32_t)(int16_t)vh_get(VR(a), 2 * i + j) * (int16_t)vh_get(VR(b), 2 * i + j);
        vw_set(t.b, i, (uint32_t)s);
    }
    V_PUT(d, t);
}
VINL void v_vmsumuhm(ctx_t *c, int d, int a, int b, int cc) {
    vec_t t;
    for (int i = 0; i < 4; i++) {
        uint32_t s = vw_get(VR(cc), i);
        for (int j = 0; j < 2; j++) s += (uint32_t)vh_get(VR(a), 2 * i + j) * vh_get(VR(b), 2 * i + j);
        vw_set(t.b, i, s);
    }
    V_PUT(d, t);
}
VINL void v_vsumsws(ctx_t *c, int d, int a, int b) {
    vec_t t; int64_t s = (int32_t)vw_get(VR(b), 3);
    for (int i = 0; i < 4; i++) s += (int32_t)vw_get(VR(a), i);
    memset(t.b, 0, 16); vw_set(t.b, 3, (uint32_t)sat32(s)); V_PUT(d, t);
}
VINL void v_vupkhsh(ctx_t *c, int d, int b) { vec_t t; for (int i = 0; i < 4; i++) vw_set(t.b, i, (uint32_t)(int32_t)(int16_t)vh_get(VR(b), i)); V_PUT(d, t); }
VINL void v_vupklsh(ctx_t *c, int d, int b) { vec_t t; for (int i = 0; i < 4; i++) vw_set(t.b, i, (uint32_t)(int32_t)(int16_t)vh_get(VR(b), i + 4)); V_PUT(d, t); }

/* --- memory: aligned element forms, permute controls, Cell unaligned left/right forms */
VINL void v_lvsl(ctx_t *c, int d, uint32_t ea) { for (int i = 0; i < 16; i++) VR(d)[i] = (uint8_t)((ea & 15) + i); }
VINL void v_lvsr(ctx_t *c, int d, uint32_t ea) { for (int i = 0; i < 16; i++) VR(d)[i] = (uint8_t)(16 - (ea & 15) + i); }
VINL void v_lve(ctx_t *c, int d, uint32_t ea, unsigned size) {  /* lvebx/lvehx/lvewx */
    ea &= ~(size - 1); memcpy(VR(d) + (ea & 15), gp(ea), size);
}
VINL void v_stve(ctx_t *c, int s, uint32_t ea, unsigned size) {  /* stvebx/stvehx/stvewx */
    ea &= ~(size - 1); memcpy(gp(ea), VR(s) + (ea & 15), size);
}
/* lvlx/lvrx: one aligned 16-byte load (the block holding ea is always mapped) and a byte shuffle; the variable-length
 * memcpy/memset these were made of were 12% of all game-code time in races */
VINL void v_lvlx(ctx_t *c, int d, uint32_t ea) {   /* bytes ea..end of its block, to the front, zero-filled */
    __m128i blk = _mm_loadu_si128((const __m128i *)gp(ea & ~15u));
    __m128i idx = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i m = _mm_add_epi8(idx, _mm_set1_epi8((char)(ea & 15)));
    m = _mm_or_si128(m, _mm_cmpgt_epi8(m, _mm_set1_epi8(15)));   /* past the block: 0x80 lanes give zero */
    _mm_storeu_si128((__m128i *)VR(d), _mm_shuffle_epi8(blk, m));
}
VINL void v_lvrx(ctx_t *c, int d, uint32_t ea) {   /* the block's first (ea & 15) bytes, to the end, zero-filled */
    __m128i blk = _mm_loadu_si128((const __m128i *)gp(ea & ~15u));
    __m128i idx = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i m = _mm_add_epi8(idx, _mm_set1_epi8((char)((int)(ea & 15) - 16)));   /* negative lanes give zero */
    _mm_storeu_si128((__m128i *)VR(d), _mm_shuffle_epi8(blk, m));
}
VINL void v_stvlx(ctx_t *c, int s, uint32_t ea) { unsigned sh = ea & 15; memcpy(gp(ea), VR(s), 16 - sh); }
VINL void v_stvrx(ctx_t *c, int s, uint32_t ea) { unsigned sh = ea & 15; if (sh) memcpy(gp(ea & ~15u), VR(s) + 16 - sh, sh); }
