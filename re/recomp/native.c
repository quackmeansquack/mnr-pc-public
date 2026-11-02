/* Native versions of hot game kernels (hooks_v111.txt "native"): the render queue's geometry work, which the PS3 ran
 * on its SPUs and the recompiled PowerPC code ran at a fraction of the speed (half the CPU time of a race).
 * Each must give exactly the original's results: MNR_NATIVE_VERIFY=1 runs both on the same input and reports any
 * difference (the original is kept as f_<entry>_orig). MNR_NATIVE_OFF=1 uses the originals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "runtime.h"

void f_007c08f8_orig(ctx_t *restrict c);
void f_007c0aa8_orig(ctx_t *restrict c);

static int has_fma;
static int native_mode(void) {   /* 0 originals, 1 native, 2 verify */
    static int m = -1;
    if (m < 0) { has_fma = __builtin_cpu_supports("fma") && !getenv("MNR_NATIVE_NOFMA"); }
    if (m < 0) m = getenv("MNR_NATIVE_OFF") ? 0 : getenv("MNR_NATIVE_VERIFY") ? 2 : 1;
    return m;
}
static void verify_report(const char *what, long *calls, long *bad, int ok) {
    InterlockedIncrement(calls);
    if (!ok && InterlockedIncrement(bad) <= 20) printf("[native] %s: MISMATCH (call %ld)\n", what, *calls);
    if (*calls % 20000 == 0) printf("[native] %s: %ld calls checked, %ld mismatched\n", what, *calls, *bad);
}

static inline __m128 ldg(const uint8_t *p) { return _mm_castsi128_ps(vx_bsw(_mm_loadu_si128((const __m128i *)p))); }   /* 4 guest floats */
static inline void stg(uint8_t *p, __m128 v) { _mm_storeu_si128((__m128i *)p, vx_bsw(_mm_castps_si128(v))); }
#define SPLAT(v, i) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(i, i, i, i))

/* 0x7C08F8 (mat r4, points r5, count r6): each 4-float point p (in place, groups of four) becomes r = p.x*m0 +
 * p.y*m1 + p.z*m2 + p.w*m3 (vmaddfp chain), then the lanes the mask at 0xDFF9C0 selects are divided by r.w
 * (vrefp, then a multiply-add with 0), the others keep r */
/* vmaddfp is fused (one rounding); vx_fma emulates it with doubles, the same but in vanishingly rare ties, so the
 * FMA instruction is used where the CPU has it */
static void project_points(uint8_t *pts, uint32_t mat, uint32_t groups) {
    const __m128 m0 = ldg(gp(mat)), m1 = ldg(gp(mat + 16)), m2 = ldg(gp(mat + 32)), m3 = ldg(gp(mat + 48)), zero = _mm_setzero_ps();
    const __m128 mask = ldg(gp(0xDFF9C0u));
    for (uint32_t i = 0; i < groups * 4; i++, pts += 16) {
        __m128 p = ldg(pts);
        __m128 t = vx_fma(SPLAT(p, 0), m0, zero);
        t = vx_fma(SPLAT(p, 1), m1, t);
        t = vx_fma(SPLAT(p, 2), m2, t);
        __m128 r = vx_fma(SPLAT(p, 3), m3, t);
        __m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), r); inv = SPLAT(inv, 3);
        __m128 s = vx_fma(r, inv, zero);
        stg(pts, _mm_or_ps(_mm_andnot_ps(mask, r), _mm_and_ps(s, mask)));
    }
}
__attribute__((target("fma"))) static void project_points_fma(uint8_t *pts, uint32_t mat, uint32_t groups) {
    const __m128 m0 = ldg(gp(mat)), m1 = ldg(gp(mat + 16)), m2 = ldg(gp(mat + 32)), m3 = ldg(gp(mat + 48)), zero = _mm_setzero_ps();
    const __m128 mask = ldg(gp(0xDFF9C0u));
    for (uint32_t i = 0; i < groups * 4; i++, pts += 16) {
        __m128 p = ldg(pts);
        __m128 t = _mm_fmadd_ps(SPLAT(p, 0), m0, zero);
        t = _mm_fmadd_ps(SPLAT(p, 1), m1, t);
        t = _mm_fmadd_ps(SPLAT(p, 2), m2, t);
        __m128 r = _mm_fmadd_ps(SPLAT(p, 3), m3, t);
        __m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), r); inv = SPLAT(inv, 3);
        __m128 s = _mm_fmadd_ps(r, inv, zero);
        stg(pts, _mm_or_ps(_mm_andnot_ps(mask, r), _mm_and_ps(s, mask)));
    }
}
void native_project_points(ctx_t *c) {
    int m = native_mode();
    if (m == 0) { f_007c08f8_orig(c); return; }
    uint32_t mat = (uint32_t)c->r[4], pts = (uint32_t)c->r[5] & ~15u, groups = ((uint32_t)c->r[6] + 3) >> 2;
    if (m == 1) { if (groups) { if (has_fma) project_points_fma(gp(pts), mat, groups); else project_points(gp(pts), mat, groups); } return; }
    static long calls, bad;
    uint8_t *mine = malloc((size_t)groups * 64 + 1);
    memcpy(mine, gp(pts), (size_t)groups * 64);
    if (groups) { if (has_fma) project_points_fma(mine, mat, groups); else project_points(mine, mat, groups); }
    f_007c08f8_orig(c);
    verify_report("project points 0x7C08F8", &calls, &bad, !memcmp(mine, gp(pts), (size_t)groups * 64));
    free(mine);
}

/* 0x7C0AA8 (flags r4, triangles r5, out r6, in r7, vertices r8) -> r3 kept: for each triangle (three u16 vertex
 * indices, 16-byte projected vertices) kept if any w <= 0; else dropped when all x > hi, all x < lo, all y > hi,
 * all y < lo, or when its signed area * scale (absolute unless flags & 1) is below the threshold. hi, lo, scale and
 * threshold come from the table at 0x7C0A98. Kept triangles' indices are written to out. */
static uint32_t cull_triangles(uint32_t flags, uint32_t n, uint8_t *out, uint32_t in, uint32_t verts) {
    const double scale = ld_f32(0x7C0A98u), thresh = ld_f32(0x7C0AA4u);
    const float hi = ld_f32(0x7C0A9Cu), lo = ld_f32(0x7C0AA0u);
    uint32_t kept = 0;
    for (uint32_t t = 0; t < n; t++, in += 6) {
        uint32_t i0 = ld_u16(in), i1 = ld_u16(in + 2), i2 = ld_u16(in + 4);
        uint32_t a = verts + i0 * 16, b = verts + i1 * 16, d = verts + i2 * 16;
        float ax = ld_f32(a), ay = ld_f32(a + 4), aw = ld_f32(a + 12);
        float bx = ld_f32(b), by = ld_f32(b + 4), bw = ld_f32(b + 12);
        float dx = ld_f32(d), dy = ld_f32(d + 4), dw = ld_f32(d + 12);
        /* the scalar area exactly as the original's double-precision registers (each result rounded to single) */
        double f3 = ax, f4 = ay, f5 = by, f6 = dx, f7 = bx, f8 = dy;
        f5 = (double)(float)(f5 - f4);
        f6 = (double)(float)(f6 - f3);
        f3 = (double)(float)(f7 - f3);
        f4 = (double)(float)(f8 - f4);
        f5 = (double)(float)(f6 * f5);
        f3 = (double)(float)(f3 * f4 - f5);
        f3 = (double)(float)(f3 * scale);
        if (!(flags & 1)) f3 = fabs(f3);
        int keep;
        if (!(aw > 0.0f) || !(bw > 0.0f) || !(dw > 0.0f)) keep = 1;
        else if (ax > hi && bx > hi && dx > hi) keep = 0;
        else if (!(ax >= lo) && !(bx >= lo) && !(dx >= lo)) keep = 0;
        else if (ay > hi && by > hi && dy > hi) keep = 0;
        else if (!(ay >= lo) && !(by >= lo) && !(dy >= lo)) keep = 0;
        else keep = !(f3 < thresh);
        if (keep) {
            uint16_t w[3] = {(uint16_t)__builtin_bswap16((uint16_t)i0), (uint16_t)__builtin_bswap16((uint16_t)i1), (uint16_t)__builtin_bswap16((uint16_t)i2)};
            memcpy(out + 6 * kept, w, 6); kept++;
        }
    }
    return kept;
}
void native_cull_triangles(ctx_t *c) {
    int m = native_mode();
    if (m == 0) { f_007c0aa8_orig(c); return; }
    uint32_t flags = (uint32_t)c->r[4], n = (uint32_t)c->r[5], out = (uint32_t)c->r[6], in = (uint32_t)c->r[7], verts = (uint32_t)c->r[8];
    if (m == 1) { c->r[3] = cull_triangles(flags, n, gp(out), in, verts); return; }
    static long calls, bad;
    uint8_t *mine = malloc((size_t)n * 6 + 1);
    uint32_t k = cull_triangles(flags, n, mine, in, verts);
    f_007c0aa8_orig(c);
    int ok = (uint32_t)c->r[3] == k && !memcmp(mine, gp(out), (size_t)k * 6);
    if (!ok && bad < 5) printf("[native] cull: native kept %u, original %u (of %u)\n", k, (uint32_t)c->r[3], n);
    verify_report("cull triangles 0x7C0AA8", &calls, &bad, ok);
    free(mine);
}

/* 0x7C0F1C (dst r4, src r5, bytes r6, format r7, stride r8): vertex positions to float4, in chunks of at most the
 * word at 0xE000A8 bytes, each of chunk / stride vertices. Format 2: three half floats (and 0x3C00, 1.0) become four
 * floats through the mask/shift/bias vectors at 0xDFF9F0..0xDFFA6F (sign, exponent, mantissa); otherwise three floats
 * are copied with w = 1.0. Each chunk starts at the previous chunk's start + its size; dst runs on. */
void f_007c0f1c_orig(ctx_t *restrict c);
static inline __m128i bsw32(__m128i v) { return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)); }
static inline __m128i ldv(uint32_t a) { return bsw32(_mm_loadu_si128((const __m128i *)gp(a))); }   /* 4 guest words, host order */
static uint32_t convert_verts(uint8_t *out, uint32_t src, uint32_t bytes, uint32_t fmt, uint32_t stride) {   /* -> bytes written */
    /* half path, per lane as the vector code: e = ((h & m1536) >> 10) - 15 + c1504, w = (h & m1552) << s1456 |
     * (e & m1488) << s1472 | (h & m1520) << s1440; the left shifts by per-lane amounts as multiplies by 1 << s */
    const __m128i m1552 = ldv(0xDFF9F0u), m1536 = ldv(0xDFFA00u), m1520 = ldv(0xDFFA10u), c1504 = ldv(0xDFFA20u), m1488 = ldv(0xDFFA30u);
    const __m128i m31 = _mm_set1_epi32(31), m15 = _mm_set1_epi32(-15);
    __m128i p1472, p1456, p1440;
    { uint32_t s1472[4], s1456[4], s1440[4], q[3][4];
      _mm_storeu_si128((__m128i *)s1472, _mm_and_si128(ldv(0xDFFA40u), m31)); _mm_storeu_si128((__m128i *)s1456, _mm_and_si128(ldv(0xDFFA50u), m31));
      _mm_storeu_si128((__m128i *)s1440, _mm_and_si128(ldv(0xDFFA60u), m31));
      for (int i = 0; i < 4; i++) { q[0][i] = 1u << s1472[i]; q[1][i] = 1u << s1456[i]; q[2][i] = 1u << s1440[i]; }
      p1472 = _mm_loadu_si128((__m128i *)q[0]); p1456 = _mm_loadu_si128((__m128i *)q[1]); p1440 = _mm_loadu_si128((__m128i *)q[2]); }
    const __m128i expo = _mm_set1_epi32(0x7F800000), mant = _mm_set1_epi32(0x7FFFFF), qbit = _mm_set1_epi32(0x400000);
    uint32_t chunk_max = ld_u32(0xE000A8u), o = 0;
    int32_t left = (int32_t)bytes;
    while (left != 0) {
        uint32_t n = (int64_t)left - (int64_t)chunk_max < 0 ? (uint32_t)left : chunk_max;
        uint32_t cnt = div_u32(n, stride);
        const uint8_t *s = gp(src);
        for (uint32_t k = 0; k < cnt; k++, o += 16) {
            __m128i w;
            if (fmt == 2) {
                uint16_t h[3]; memcpy(h, s, 6); s += 6;
                __m128i in = _mm_setr_epi32(__builtin_bswap16(h[0]), __builtin_bswap16(h[1]), __builtin_bswap16(h[2]), 15360);
                __m128i e = _mm_add_epi32(_mm_add_epi32(_mm_srli_epi32(_mm_and_si128(in, m1536), 10), m15), c1504);
                e = _mm_mullo_epi32(_mm_and_si128(e, m1488), p1472);
                w = _mm_or_si128(_mm_or_si128(_mm_mullo_epi32(_mm_and_si128(in, m1552), p1456), e), _mm_mullo_epi32(_mm_and_si128(in, m1520), p1440));
            } else {   /* through lfs/stfs: a signalling NaN comes back quiet */
                uint32_t f[4]; memcpy(f, s, 12); f[3] = 0x0000803Fu; s += 12;
                w = bsw32(_mm_loadu_si128((const __m128i *)f));
                __m128i nan = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(w, mant), _mm_setzero_si128()), _mm_cmpeq_epi32(_mm_and_si128(w, expo), expo));
                w = _mm_or_si128(w, _mm_and_si128(nan, qbit));
            }
            _mm_storeu_si128((__m128i *)(out + o), bsw32(w));
        }
        left -= (int32_t)n;
        src += n;
    }
    return o;
}
void native_convert_verts(ctx_t *c) {
    int m = native_mode();
    if (m == 0) { f_007c0f1c_orig(c); return; }
    uint32_t dst = (uint32_t)c->r[4] & ~15u, src = (uint32_t)c->r[5], bytes = (uint32_t)c->r[6], fmt = (uint32_t)c->r[7], stride = (uint32_t)c->r[8];
    if (m == 1) { convert_verts(gp(dst), src, bytes, fmt, stride); return; }
    static long calls, bad;
    uint32_t total = 0; { int32_t left = (int32_t)bytes; uint32_t cm = ld_u32(0xE000A8u);
        while (left != 0) { uint32_t n = (int64_t)left - (int64_t)cm < 0 ? (uint32_t)left : cm; total += div_u32(n, stride) * 16; left -= (int32_t)n; } }
    uint8_t *mine = malloc(total + 16);
    convert_verts(mine, src, bytes, fmt, stride);
    f_007c0f1c_orig(c);
    int ok = !memcmp(mine, gp(dst), total);
    if (!ok && bad < 5) printf("[native] convert: fmt %u stride %u bytes %u -> %u output bytes differ\n", fmt, stride, bytes, total);
    verify_report("convert vertices 0x7C0F1C", &calls, &bad, ok);
    free(mine);
}

/* 0x97A290 (f1 x, r4 0 sin / 1 cos) -> f1: the SDK's sinf/cosf, a fifth of the main thread's work in races.
 * x outside the range at toc+9456..9464 is first reduced by another function, and an infinite x sets errno: both
 * (rare) run the original. Otherwise as the original, operation by operation: k = x * 2/pi rounded half away from
 * zero, r = x - k*pi/2 in two parts, then the sine or cosine polynomial by quadrant (k + r4) & 3. */
void f_0097a290_orig(ctx_t *restrict c);
void native_sincos(ctx_t *restrict c) {
    int m = native_mode();
    if (m == 0) { f_0097a290_orig(c); return; }
    uint32_t toc = (uint32_t)c->r[2];
    float xf = (float)c->f[1].d; uint32_t xb; memcpy(&xb, &xf, 4);
    uint32_t q4 = (uint32_t)c->r[4];
    double res;
    if ((xb & 0x7F800000u) == 0x7F800000u) { f_0097a290_orig(c); return; }   /* infinity (errno) or NaN */
    if (!(xb & 0x7FFFFFFFu)) res = q4 ? ld_f32(toc + 9448) : xf;            /* +-0: cos 1, sin x */
    else {
        double x = xf, lo, hi; uint64_t lb = ld_u64(toc + 9456), hb = ld_u64(toc + 9464);
        memcpy(&lo, &lb, 8); memcpy(&hi, &hb, 8);
        if (x < lo || x > hi) { f_0097a290_orig(c); return; }
        double t = (double)(float)(x * (double)ld_f32(toc + 9476)), half = ld_f32(toc + 9480);
        int32_t k = t > (double)ld_f32(toc + 9520) ? cvt_d2i32((double)(float)(t + half), 1) : cvt_d2i32((double)(float)(t - half), 1);
        double kf = (double)(float)(double)(int64_t)k;
        double r = (double)(float)(kf * (double)ld_f32(toc + 9484) + x);
        r = (double)(float)(kf * (double)ld_f32(toc + 9488) + r);
        double thr = ld_f32(ld_u32(toc + 9492));
        uint32_t q = ((uint32_t)k & 3) + q4;
        if (-thr < r && r < thr) res = (q & 1) ? (double)ld_f32(toc + 9448) : r;
        else {
            double r2 = (double)(float)(r * r);
            if (q & 1) {
                double p = (double)(float)(r2 * (double)ld_f32(toc + 9496) + (double)ld_f32(toc + 9500));
                p = (double)(float)(r2 * p - (double)ld_f32(toc + 9504));
                res = (double)(float)(r2 * p + (double)ld_f32(toc + 9448));
            } else {
                double r3 = (double)(float)(r * r2);
                double p = (double)(float)(r2 * (double)ld_f32(toc + 9508) + (double)ld_f32(toc + 9512));
                p = (double)(float)(r2 * p - (double)ld_f32(toc + 9516));
                res = (double)(float)(r3 * p + r);
            }
        }
        if (q & 2) res = -res;
    }
    if (m == 1) { st_f32((uint32_t)c->r[1] + 48, xf); c->f[1].d = res; return; }
    static long calls, bad;
    f_0097a290_orig(c);
    int ok = !memcmp(&res, &c->f[1].d, 8);
    if (!ok && bad < 5) printf("[native] sincos(%a, %u): native %a, original %a\n", (double)xf, q4, res, c->f[1].d);
    verify_report("sinf/cosf 0x97A290", &calls, &bad, ok);
}


/* The studios' "may this creation be remixed" checks (1 = yes): 0x703F88 (Track Studio), 0x7A8218 (Kart Studio),
 * 0x6EF134 (Mod Studio). For a creation by someone else they give the creator's choice, which the game keeps with the
 * creation and the launcher can't change; refused, the studio shows MOD_STUDIO_NO_PERMISSION_TO_REMIX. MNR_ALLOW_REMIX=1
 * (launcher setting): always yes, but only online on a server of our own (net_own_server): on MNR Revival, or offline,
 * the creators' choices stand, as in the real game. */
void f_00703f88_orig(ctx_t *restrict c);
void f_007a8218_orig(ctx_t *restrict c);
void f_006ef134_orig(ctx_t *restrict c);
int net_own_server(void);
static int allow_remix(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MNR_ALLOW_REMIX");
        on = e && *e == '1' && !getenv("MNR_REMIX_OFF") && net_own_server();   /* (MNR_REMIX_OFF: tests) */
        if (e && *e == '1') printf("[native] remixing anyone's creations: %s\n", on ? "on (own server)" : "off (MNR Revival or offline: the creators' choices)");
    }
    return on;
}
void remix_check_track(ctx_t *restrict c) { if (allow_remix()) c->r[3] = 1; else f_00703f88_orig(c); }
void remix_check_kart(ctx_t *restrict c) { if (allow_remix()) c->r[3] = 1; else f_007a8218_orig(c); }
void remix_check_mod(ctx_t *restrict c) { if (allow_remix()) c->r[3] = 1; else f_006ef134_orig(c); }
