/* Software renderer for the RSX 3D engine: vertex fetch, vertex-program interpreter, primitive
 * assembly, near-plane clipping, perspective-correct rasterisation, fragment-program interpreter,
 * texture sampling and the output merger (depth, alpha test, blending, colour mask).
 *
 * Everything reads and writes guest memory directly, so render targets that are later sampled as
 * textures just work. Accuracy first, speed later: rasterisation is split across worker threads by
 * horizontal bands. MNR_RSX_NODRAW=1 skips all drawing. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include "runtime.h"
#include "host.h"
#include "rsx.h"
#include "rsx_shader.h"
#include "rsx_d3d.h"

static int ww_nregions;                 /* the memory the per-frame scan covers (ww_scan_all_locked) */
static uint32_t ww_regions[65][2];
uint32_t rsx_fp_addr(void);
void rsx_hlsl_test(const uint32_t (*vp)[4], uint32_t vp_start, const fp_insn_t *fp, int nfp, uint32_t ctl, const uint8_t dims[16]);
#define REG(m) rsx_reg(0, (m))

typedef struct { float v[4]; } vec4;
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float half2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, u;
    if (e == 0) { if (!m) u = s << 31; else { float f = ldexpf((float)m, -24); return s ? -f : f; } }
    else if (e == 31) u = s << 31 | 0x7F800000 | m << 13;
    else u = s << 31 | (e + 112) << 23 | m << 13;
    return u2f(u);
}
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

/* ------------------------------------------------------------------ memory locations */
static uint32_t loc_addr(uint32_t offset, int main_mem) { return main_mem ? gcm_io_to_ea_pub(offset) : RSX_LOCAL_BASE + offset; }

/* ------------------------------------------------------------------ vertex fetch */
#define MAXV 0x10000
typedef struct { vec4 out[17]; } vtx_t;   /* VP outputs used for rasterisation: o[0] .. o[16] */

static void fetch_attr(int a, uint32_t index, vec4 *o) {
    uint32_t f = REG(0x1740 + 4 * a), size = (f >> 4) & 15, type = f & 15, stride = (f >> 8) & 255;
    o->v[0] = o->v[1] = o->v[2] = 0; o->v[3] = 1;
    if (!size) {   /* constant attribute: VERTEX_DATA4F_M */
        for (int k = 0; k < 4; k++) o->v[k] = u2f(REG(0x1C00 + 16 * a + 4 * k));
        return;
    }
    uint32_t off = REG(0x1680 + 4 * a);
    uint32_t base = loc_addr(((off & 0x7FFFFFFF) + REG(0x1738)), off >> 31);
    if (!base) return;
    uint32_t p = base + stride * index;
    switch (type) {
    case 1: for (uint32_t k = 0; k < size; k++) o->v[k] = clampf(ld_s16(p + 2 * k) / 32767.0f, -1, 1); break;   /* S1 */
    case 2: for (uint32_t k = 0; k < size; k++) o->v[k] = ld_f32(p + 4 * k); break;                              /* F */
    case 3: for (uint32_t k = 0; k < size; k++) o->v[k] = half2f(ld_u16(p + 2 * k)); break;                      /* SF */
    case 4: for (uint32_t k = 0; k < size; k++) o->v[k] = ld_u8(p + k) / 255.0f; break;                          /* UB */
    case 5: for (uint32_t k = 0; k < size; k++) o->v[k] = (float)ld_s16(p + 2 * k); break;                       /* S32K */
    case 6: {                                                                                                     /* CMP 11:11:10 */
        uint32_t w = ld_u32(p);
        int32_t x = (int32_t)(w << 21) >> 21, y = (int32_t)(w << 10) >> 21, z = (int32_t)w >> 22;
        o->v[0] = x / 1023.0f; o->v[1] = y / 1023.0f; o->v[2] = z / 511.0f;
        break;
    }
    case 7: for (uint32_t k = 0; k < size; k++) o->v[k] = (float)ld_u8(p + k); break;                            /* UB256 */
    }
}

/* ------------------------------------------------------------------ vertex program interpreter */
typedef struct { vp_insn_t insn[512]; uint32_t start; int valid; } vp_cache_t;
static vp_cache_t vpc_dec;
static uint32_t vp_gen, vp_dec_gen;
uint32_t rsx_vp_gen_pub;
void rsx_vp_dirty(void) { vp_gen++; rsx_vp_gen_pub++; }

static inline float vp_cmp(int cond, float x) {   /* cond: 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 TR, 0 FL */
    switch (cond) { case 0: return 0; case 1: return x < 0; case 2: return x == 0; case 3: return x <= 0;
    case 4: return x > 0; case 5: return x != 0; case 6: return x >= 0; default: return 1; }
}

static void run_vp(uint32_t index, vtx_t *out) {
    if (vp_dec_gen != vp_gen || !vpc_dec.valid) {
        for (int i = 0; i < 512; i++) vp_decode(rsx_vp[i], &vpc_dec.insn[i]);
        vpc_dec.valid = 1; vp_dec_gen = vp_gen;
    }
    vec4 in[16], r[48], o[32];
    uint32_t inmask = REG(0x1FF0);
    for (int a = 0; a < 16; a++) if (inmask >> a & 1) fetch_attr(a, index, &in[a]); else in[a] = (vec4){{0, 0, 0, 1}};
    memset(r, 0, sizeof r); memset(o, 0, sizeof o);
    o[0].v[3] = 1;
    for (int k = 1; k < 32; k++) o[k].v[3] = 1;
    int a0[2][4] = {{0}}; vec4 cc[2] = {{{0}}};
    uint32_t pc = REG(0x1EA0) & 511;
    for (int steps = 0; steps < 1024 && pc < 512; steps++) {
        const vp_insn_t *I = &vpc_dec.insn[pc];
        vec4 s[3];
        for (int n = 0; n < 3; n++) {
            const vp_src_t *S = &I->src[n];
            vec4 v = {{0, 0, 0, 0}};
            if (S->type == 1) v = r[S->index % 48];
            else if (S->type == 2) v = in[I->input_index];
            else if (S->type == 3) {
                int ci = I->const_index + (I->index_const ? a0[I->addr_reg][I->addr_swz] : 0);
                if (ci >= 0 && ci < 468) for (int k = 0; k < 4; k++) v.v[k] = u2f(rsx_vpc[ci][k]);
            }
            vec4 w;
            for (int k = 0; k < 4; k++) {
                float x = v.v[S->swz[k]];
                if (S->abs) x = fabsf(x);
                if (S->neg) x = -x;
                w.v[k] = x;
            }
            s[n] = w;
        }
        vec4 vr = {{0}}, sr = {{0}};
        const float *A = s[0].v, *B = s[1].v, *C = s[2].v;
        int vec_write = 1;
        switch (I->vec_op) {
        case 0: vec_write = 0; break;
        case 1: vr = s[0]; break;
        case 2: for (int k = 0; k < 4; k++) vr.v[k] = A[k] * B[k]; break;
        case 3: for (int k = 0; k < 4; k++) vr.v[k] = A[k] + C[k]; break;
        case 4: for (int k = 0; k < 4; k++) vr.v[k] = A[k] * B[k] + C[k]; break;
        case 5: { float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; for (int k = 0; k < 4; k++) vr.v[k] = d; break; }
        case 6: { float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + B[3]; for (int k = 0; k < 4; k++) vr.v[k] = d; break; }
        case 7: { float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3]; for (int k = 0; k < 4; k++) vr.v[k] = d; break; }
        case 8: vr.v[0] = 1; vr.v[1] = A[1] * B[1]; vr.v[2] = A[2]; vr.v[3] = B[3]; break;
        case 9: for (int k = 0; k < 4; k++) vr.v[k] = fminf(A[k], B[k]); break;
        case 10: for (int k = 0; k < 4; k++) vr.v[k] = fmaxf(A[k], B[k]); break;
        case 11: for (int k = 0; k < 4; k++) vr.v[k] = A[k] < B[k]; break;
        case 12: for (int k = 0; k < 4; k++) vr.v[k] = A[k] >= B[k]; break;
        case 13:   /* ARL */
            for (int k = 0; k < 4; k++) if (I->vec_mask >> (3 - k) & 1) a0[I->addr_reg][k] = (int)floorf(A[k]);
            vec_write = 0; break;
        case 14: for (int k = 0; k < 4; k++) vr.v[k] = A[k] - floorf(A[k]); break;
        case 15: for (int k = 0; k < 4; k++) vr.v[k] = floorf(A[k]); break;
        case 16: for (int k = 0; k < 4; k++) vr.v[k] = A[k] == B[k]; break;
        case 17: break;
        case 18: for (int k = 0; k < 4; k++) vr.v[k] = A[k] > B[k]; break;
        case 19: for (int k = 0; k < 4; k++) vr.v[k] = A[k] <= B[k]; break;
        case 20: for (int k = 0; k < 4; k++) vr.v[k] = A[k] != B[k]; break;
        case 21: for (int k = 0; k < 4; k++) vr.v[k] = 1; break;
        case 22: for (int k = 0; k < 4; k++) vr.v[k] = A[k] > 0 ? 1.f : A[k] < 0 ? -1.f : 0.f; break;
        default: vec_write = 0; break;
        }
        float x = C[0];
        int sca_write = 1, jump = -1;
        switch (I->sca_op) {
        case 0: sca_write = 0; break;
        case 1: sr = (vec4){{x, x, x, x}}; break;
        case 2: x = 1.0f / x; sr = (vec4){{x, x, x, x}}; break;
        case 3: x = 1.0f / x; x = x > 0 ? clampf(x, 5.42101e-20f, 1.884467e19f) : clampf(x, -1.884467e19f, -5.42101e-20f); sr = (vec4){{x, x, x, x}}; break;
        case 4: x = 1.0f / sqrtf(fabsf(x)); sr = (vec4){{x, x, x, x}}; break;
        case 5: { float fl = floorf(x); sr = (vec4){{exp2f(fl), x - fl, exp2f(x), 1}}; break; }
        case 6: { float ax = fabsf(x); int e; float m = frexpf(ax, &e); sr = (vec4){{(float)(e - 1), m * 2, log2f(ax), 1}}; break; }
        case 7: {   /* LIT on the swizzled src2 */
            float lx = fmaxf(C[0], 0), ly = fmaxf(C[1], 0), lw = clampf(C[3], -127.9961f, 127.9961f);
            sr = (vec4){{1, lx, lx > 0 ? powf(ly, lw) : 0, 1}}; break;
        }
        case 8: case 9: {   /* BRA / BRI: branch to the address in const_index-ish field; conditional */
            int ok = 1;
            if (I->cond_test) { for (int k = 0; k < 4; k++) if (!vp_cmp(I->cond, cc[I->cond_reg].v[I->cond_swz[k]])) ok = 0; }
            if (ok) jump = (int)(((rsx_vp[pc][2] >> 3) & 0x3F) | (((rsx_vp[pc][3] >> 21) & 0x7) << 6));  /* best effort */
            sca_write = 0; break;
        }
        case 13: x = log2f(fabsf(x)); sr = (vec4){{x, x, x, x}}; break;
        case 14: x = exp2f(x); sr = (vec4){{x, x, x, x}}; break;
        case 15: x = sinf(x); sr = (vec4){{x, x, x, x}}; break;
        case 16: x = cosf(x); sr = (vec4){{x, x, x, x}}; break;
        default: sca_write = 0; break;
        }
        int cond_ok[4] = {1, 1, 1, 1};
        if (I->cond_test) for (int k = 0; k < 4; k++) cond_ok[k] = (int)vp_cmp(I->cond, cc[I->cond_reg].v[I->cond_swz[k]]);
        if (vec_write) {
            if (I->saturate) for (int k = 0; k < 4; k++) vr.v[k] = clampf(vr.v[k], 0, 1);
            for (int k = 0; k < 4; k++) {
                if (!(I->vec_mask >> (3 - k) & 1) || !cond_ok[k]) continue;
                if (I->vec_tmp != 0x3F) r[I->vec_tmp % 48].v[k] = vr.v[k];
                if (I->vec_result && I->out_reg != 0x1F) o[I->out_reg].v[k] = vr.v[k];
                if (I->cond_update) cc[I->cond_reg].v[k] = vr.v[k];
            }
        }
        if (sca_write) {
            for (int k = 0; k < 4; k++) {
                if (!(I->sca_mask >> (3 - k) & 1) || !cond_ok[k]) continue;
                if (I->sca_tmp != 0x3F) r[I->sca_tmp % 48].v[k] = sr.v[k];
                if (!I->vec_result && I->out_reg != 0x1F) o[I->out_reg].v[k] = sr.v[k];
            }
        }
        if (I->end) break;
        pc = jump >= 0 ? (uint32_t)jump : pc + 1;
    }
    for (int k = 0; k < 17; k++) out->out[k] = o[k];
}

/* ------------------------------------------------------------------ textures */
typedef struct {
    uint32_t addr, fmt, w, h, pitch, linear, unnorm, dims, cube, remap, wrap_s, wrap_t, mag_linear, min_linear, levels;
    int enabled;
    const uint32_t *cache;   /* decoded level 0 as RGBA8 (r | g << 8 | b << 16 | a << 24), or NULL */
} tex_t;
static const uint32_t *tcache_get(const tex_t *t);

static uint32_t swz_index(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {   /* RSX swizzle: interleave from bit 0 */
    uint32_t off = 0, bit = 0, lmin = lw < lh ? lw : lh;
    for (uint32_t i = 0; i < lmin; i++) { off |= ((x >> i) & 1) << bit++; off |= ((y >> i) & 1) << bit++; }
    if (lw > lh) for (uint32_t i = lmin; i < lw; i++) off |= ((x >> i) & 1) << bit++;
    else for (uint32_t i = lmin; i < lh; i++) off |= ((y >> i) & 1) << bit++;
    return off;
}
static uint32_t ilog2(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }

static void tex_setup(int u, tex_t *t) {
    uint32_t base = 0x1A00 + 0x20 * u;
    uint32_t ctl0 = REG(base + 0x0C);
    t->enabled = ctl0 >> 31;
    if (!t->enabled) return;
    uint32_t fmt = REG(base + 4);
    t->addr = loc_addr(REG(base), (fmt & 3) == 2);
    uint32_t f = (fmt >> 8) & 0xFF;
    t->linear = (f & 0x20) != 0; t->unnorm = (f & 0x40) != 0; t->fmt = f & 0x9F;
    t->dims = (fmt >> 4) & 15; t->cube = (fmt >> 2) & 1; t->levels = fmt >> 16;
    uint32_t rect = REG(base + 0x18);
    t->w = rect >> 16; t->h = rect & 0xFFFF;
    if (!t->w) t->w = 1; if (!t->h) t->h = 1;
    t->pitch = REG(0x1840 + 4 * u) & 0xFFFFF;
    t->remap = REG(base + 0x10);
    uint32_t addr = REG(base + 8);
    t->wrap_s = addr & 15; t->wrap_t = (addr >> 8) & 15;
    uint32_t filt = REG(base + 0x14);
    t->mag_linear = ((filt >> 24) & 7) == 2;
    t->min_linear = ((filt >> 16) & 0xFF) != 1;
    t->cache = (t->addr && rsx_use_d3d != 1) ? tcache_get(t) : NULL;
}

static inline void unpack_565(uint16_t c, float *r, float *g, float *b) {
    *r = ((c >> 11) & 31) / 31.0f; *g = ((c >> 5) & 63) / 63.0f; *b = (c & 31) / 31.0f;
}
/* DXT block decode of one texel; blocks are stored little-endian like on PC */
static void dxt_texel(const tex_t *t, uint32_t x, uint32_t y, float argb[4]) {
    uint32_t bw = (t->w + 3) / 4, bs = t->fmt == 0x86 ? 8 : 16;
    uint32_t pitch = t->pitch ? t->pitch : bw * bs;
    uint32_t b = t->addr + (y / 4) * pitch + (x / 4) * bs;
    uint32_t px = x & 3, py = y & 3, ti = py * 4 + px;
    uint32_t cb = bs == 16 ? b + 8 : b;
    uint16_t c0 = ld_u16le(cb), c1 = ld_u16le(cb + 2);
    uint32_t bits = ld_u32le(cb + 4);
    uint32_t sel = (bits >> (2 * ti)) & 3;
    float r0, g0, b0, r1, g1, b1;
    unpack_565(c0, &r0, &g0, &b0); unpack_565(c1, &r1, &g1, &b1);
    float r, g, bb, a = 1;
    if (c0 > c1 || bs == 16) {
        switch (sel) {
        case 0: r = r0; g = g0; bb = b0; break;
        case 1: r = r1; g = g1; bb = b1; break;
        case 2: r = (2 * r0 + r1) / 3; g = (2 * g0 + g1) / 3; bb = (2 * b0 + b1) / 3; break;
        default: r = (r0 + 2 * r1) / 3; g = (g0 + 2 * g1) / 3; bb = (b0 + 2 * b1) / 3; break;
        }
    } else {
        switch (sel) {
        case 0: r = r0; g = g0; bb = b0; break;
        case 1: r = r1; g = g1; bb = b1; break;
        case 2: r = (r0 + r1) / 2; g = (g0 + g1) / 2; bb = (b0 + b1) / 2; break;
        default: r = g = bb = 0; a = 0; break;
        }
    }
    if (t->fmt == 0x87) {   /* DXT2/3: explicit 4-bit alpha */
        uint32_t ab = ld_u16le(b + 2 * py);
        a = ((ab >> (4 * px)) & 15) / 15.0f;
    } else if (t->fmt == 0x88) {   /* DXT4/5: interpolated alpha */
        uint32_t a0 = ld_u8(b), a1 = ld_u8(b + 1);
        uint64_t ab = 0;
        for (int k = 0; k < 6; k++) ab |= (uint64_t)ld_u8(b + 2 + k) << (8 * k);
        uint32_t as = (ab >> (3 * ti)) & 7;
        float fa0 = a0 / 255.0f, fa1 = a1 / 255.0f;
        if (as == 0) a = fa0; else if (as == 1) a = fa1;
        else if (a0 > a1) a = ((8 - as) * fa0 + (as - 1) * fa1) / 7;
        else if (as == 6) a = 0; else if (as == 7) a = 1;
        else a = ((6 - as) * fa0 + (as - 1) * fa1) / 5;
    }
    argb[0] = a; argb[1] = r; argb[2] = g; argb[3] = bb;
}

static uint32_t texel_bpp(uint32_t fmt) {
    switch (fmt) {
    case 0x81: return 1;
    case 0x82: case 0x83: case 0x84: case 0x8B: case 0x8F: case 0x92: case 0x93: case 0x94: case 0x97: case 0x98: case 0x99: case 0x9D: return 2;
    case 0x9A: return 8;
    case 0x9B: return 16;
    default: return 4;
    }
}

static void texel_at(uint32_t fmt, uint32_t a, float argb[4]) {   /* the (non-DXT) texel at guest address a */
    argb[0] = 1; argb[1] = argb[2] = argb[3] = 0;
    {
        switch (fmt) {
        case 0x81: { float v = ld_u8(a) / 255.0f; argb[0] = argb[1] = argb[2] = argb[3] = v; break; }
        case 0x82: { uint16_t c = ld_u16(a); argb[0] = (float)(c >> 15); argb[1] = ((c >> 10) & 31) / 31.0f; argb[2] = ((c >> 5) & 31) / 31.0f; argb[3] = (c & 31) / 31.0f; break; }
        case 0x83: { uint16_t c = ld_u16(a); argb[0] = (c >> 12) / 15.0f; argb[1] = ((c >> 8) & 15) / 15.0f; argb[2] = ((c >> 4) & 15) / 15.0f; argb[3] = (c & 15) / 15.0f; break; }
        case 0x84: { uint16_t c = ld_u16(a); unpack_565(c, &argb[1], &argb[2], &argb[3]); argb[0] = 1; break; }
        case 0x85: argb[0] = ld_u8(a) / 255.0f; argb[1] = ld_u8(a + 1) / 255.0f; argb[2] = ld_u8(a + 2) / 255.0f; argb[3] = ld_u8(a + 3) / 255.0f; break;
        case 0x8B: { float g = ld_u8(a) / 255.0f, b = ld_u8(a + 1) / 255.0f; argb[0] = g; argb[1] = b; argb[2] = g; argb[3] = b; break; }
        case 0x8F: { uint16_t c = ld_u16(a); argb[0] = 1; argb[1] = (c >> 10) / 63.0f; argb[2] = ((c >> 5) & 31) / 31.0f; argb[3] = (c & 31) / 31.0f; break; }
        case 0x90: case 0x91: { uint32_t d = ld_u32(a); float z = (d >> 8) / 16777215.0f; argb[0] = argb[1] = argb[2] = argb[3] = z; break; }
        case 0x92: case 0x93: { float z = ld_u16(a) / 65535.0f; argb[0] = argb[1] = argb[2] = argb[3] = z; break; }
        case 0x94: { float v = ld_u16(a) / 65535.0f; argb[0] = argb[1] = argb[2] = argb[3] = v; break; }
        case 0x95: { float y16 = ld_u16(a) / 65535.0f, x16 = ld_u16(a + 2) / 65535.0f; argb[0] = y16; argb[1] = x16; argb[2] = y16; argb[3] = x16; break; }
        case 0x97: { uint16_t c = ld_u16(a); argb[1] = (c >> 11) / 31.0f; argb[2] = ((c >> 6) & 31) / 31.0f; argb[3] = ((c >> 1) & 31) / 31.0f; argb[0] = (float)(c & 1); break; }
        case 0x98: { float h = ld_u8(a) / 255.0f, l = ld_u8(a + 1) / 255.0f; argb[0] = 1; argb[1] = h; argb[2] = l; argb[3] = 1; break; }
        case 0x99: { float h = (int8_t)ld_u8(a) / 127.0f, l = (int8_t)ld_u8(a + 1) / 127.0f; argb[0] = 1; argb[1] = h; argb[2] = l; argb[3] = 1; break; }
        case 0x9A: argb[1] = half2f(ld_u16(a)); argb[2] = half2f(ld_u16(a + 2)); argb[3] = half2f(ld_u16(a + 4)); argb[0] = half2f(ld_u16(a + 6)); break;
        case 0x9B: argb[1] = ld_f32(a); argb[2] = ld_f32(a + 4); argb[3] = ld_f32(a + 8); argb[0] = ld_f32(a + 12); break;
        case 0x9C: { float v = ld_f32(a); argb[0] = argb[1] = argb[2] = argb[3] = v; break; }
        case 0x9D: { uint16_t c = ld_u16(a); argb[0] = 1; argb[1] = ((c >> 10) & 31) / 31.0f; argb[2] = ((c >> 5) & 31) / 31.0f; argb[3] = (c & 31) / 31.0f; break; }
        case 0x9E: argb[0] = 1; argb[1] = ld_u8(a + 1) / 255.0f; argb[2] = ld_u8(a + 2) / 255.0f; argb[3] = ld_u8(a + 3) / 255.0f; break;
        case 0x9F: argb[1] = half2f(ld_u16(a)); argb[2] = half2f(ld_u16(a + 2)); argb[3] = 0; argb[0] = 1; break;
        default: argb[0] = 1; argb[1] = 1; argb[2] = 0; argb[3] = 1; break;   /* unknown: magenta */
        }
    }
}
static void texel_raw(const tex_t *t, int x, int y, float argb[4]) {   /* argb: A, R, G, B before remap */
    argb[0] = 1; argb[1] = argb[2] = argb[3] = 0;
    if (t->fmt >= 0x86 && t->fmt <= 0x88) { dxt_texel(t, (uint32_t)x, (uint32_t)y, argb); return; }
    uint32_t bpp = texel_bpp(t->fmt), a;
    if (t->linear) a = t->addr + (uint32_t)y * (t->pitch ? t->pitch : t->w * bpp) + (uint32_t)x * bpp;
    else a = t->addr + swz_index((uint32_t)x, (uint32_t)y, ilog2(t->w), ilog2(t->h)) * bpp;
    texel_at(t->fmt, a, argb);
}
/* one whole DXT block (the same arithmetic as dxt_texel, with the colour and alpha palettes worked out once) */
static void dxt_block(const tex_t *t, uint32_t bx, uint32_t by, float out[16][4]) {
    uint32_t bw = (t->w + 3) / 4, bs = t->fmt == 0x86 ? 8 : 16;
    uint32_t pitch = t->pitch ? t->pitch : bw * bs;
    uint32_t b = t->addr + by * pitch + bx * bs, cb = bs == 16 ? b + 8 : b;
    uint16_t c0 = ld_u16le(cb), c1 = ld_u16le(cb + 2);
    uint32_t bits = ld_u32le(cb + 4);
    float r0, g0, b0, r1, g1, b1, pr[4], pg[4], pb[4], pa[4] = {1, 1, 1, 1};
    unpack_565(c0, &r0, &g0, &b0); unpack_565(c1, &r1, &g1, &b1);
    pr[0] = r0; pg[0] = g0; pb[0] = b0; pr[1] = r1; pg[1] = g1; pb[1] = b1;
    if (c0 > c1 || bs == 16) {
        pr[2] = (2 * r0 + r1) / 3; pg[2] = (2 * g0 + g1) / 3; pb[2] = (2 * b0 + b1) / 3;
        pr[3] = (r0 + 2 * r1) / 3; pg[3] = (g0 + 2 * g1) / 3; pb[3] = (b0 + 2 * b1) / 3;
    } else {
        pr[2] = (r0 + r1) / 2; pg[2] = (g0 + g1) / 2; pb[2] = (b0 + b1) / 2;
        pr[3] = pg[3] = pb[3] = 0; pa[3] = 0;
    }
    float al[8] = {0}; uint64_t ab = 0;
    if (t->fmt == 0x88) {
        uint32_t a0 = ld_u8(b), a1 = ld_u8(b + 1);
        for (int k = 0; k < 6; k++) ab |= (uint64_t)ld_u8(b + 2 + k) << (8 * k);
        float fa0 = a0 / 255.0f, fa1 = a1 / 255.0f;
        for (uint32_t as = 0; as < 8; as++)
            al[as] = as == 0 ? fa0 : as == 1 ? fa1 : a0 > a1 ? ((8 - as) * fa0 + (as - 1) * fa1) / 7
                   : as == 6 ? 0 : as == 7 ? 1 : ((6 - as) * fa0 + (as - 1) * fa1) / 5;
    }
    for (uint32_t ti = 0; ti < 16; ti++) {
        uint32_t sel = (bits >> (2 * ti)) & 3, px = ti & 3, py = ti >> 2;
        float a = pa[sel];
        if (t->fmt == 0x87) a = ((ld_u16le(b + 2 * py) >> (4 * px)) & 15) / 15.0f;
        else if (t->fmt == 0x88) a = al[(ab >> (3 * ti)) & 7];
        out[ti][0] = a; out[ti][1] = pr[sel]; out[ti][2] = pg[sel]; out[ti][3] = pb[sel];
    }
}
static void texel(const tex_t *t, int x, int y, float out[4]) {   /* out: r, g, b, a after remap */
    float argb[4];
    texel_raw(t, x, y, argb);
    /* remap: selectors (bits 0..7, 2 bits each for A, R, G, B) and ops (bits 8..15: 0 zero, 1 one, 2 remap) */
    uint32_t rm = t->remap;
    float res[4];   /* A R G B */
    for (int c = 0; c < 4; c++) {
        uint32_t op = (rm >> (8 + 2 * c)) & 3, sel = (rm >> (2 * c)) & 3;
        res[c] = op == 0 ? 0.f : op == 1 ? 1.f : argb[sel];
    }
    out[0] = res[1]; out[1] = res[2]; out[2] = res[3]; out[3] = res[0];
}

static int wrap_coord(int i, int n, uint32_t mode) {
    switch (mode) {
    case 1: i %= n; return i < 0 ? i + n : i;   /* wrap */
    case 2: {                                      /* mirror */
        int p = i % (2 * n); if (p < 0) p += 2 * n;
        return p < n ? p : 2 * n - 1 - p;
    }
    default: return i < 0 ? 0 : i >= n ? n - 1 : i;   /* clamp variants */
    }
}

/* ---- decoded texture cache. Guest memory is reserved with MEM_WRITE_WATCH: any write to a texture's
 * pages (CPU uploads, render-to-texture by this renderer) makes the next draw that samples it re-decode. */
typedef struct {
    uint32_t addr, fmt, w, h, pitch, linear, remap, size;
    uint32_t *rgba;
    uint64_t last_use, gen;
} tcache_t;
static tcache_t tcache[256];
static uint64_t tcache_clock;
static uint32_t tex_bytes(const tex_t *t) {
    if (t->fmt >= 0x86 && t->fmt <= 0x88) {
        uint32_t bs = t->fmt == 0x86 ? 8 : 16, bw = (t->w + 3) / 4, bh = (t->h + 3) / 4;
        return (t->pitch ? t->pitch : bw * bs) * bh;
    }
    uint32_t bpp = texel_bpp(t->fmt);
    if (t->linear) return (t->pitch ? t->pitch : t->w * bpp) * t->h;
    return (1u << ilog2(t->w)) * (1u << ilog2(t->h)) * bpp;
}
/* per-page write generations: dirty pages reported by GetWriteWatch get a new generation number, so
 * textures sharing a page each see the write */
static uint32_t *page_gen;
static uint32_t *block_gen;   /* newest generation per 64 pages (256 KB): big ranges are scanned by block */
static uint64_t gen_clock = 1;
long rsx_rg_now_calls; double rsx_rg_now_s;   /* MNR_RSX_LOG: uncached range checks per frame and their time */
static uint64_t range_gen_raw(uint32_t addr, uint32_t size);
static uint64_t range_gen(uint32_t addr, uint32_t size) {
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    uint64_t r = range_gen_raw(addr, size);
    QueryPerformanceCounter(&t1); rsx_rg_now_calls++; rsx_rg_now_s += (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    return r;
}
static SRWLOCK ww_lock = SRWLOCK_INIT;   /* GetWriteWatch resets and page_gen updates: the RSX thread and the frame scanner */
static uint64_t range_gen_raw_locked(uint32_t addr, uint32_t size);
static uint64_t range_gen_raw(uint32_t addr, uint32_t size) {
    AcquireSRWLockExclusive(&ww_lock); uint64_t r = range_gen_raw_locked(addr, size); ReleaseSRWLockExclusive(&ww_lock); return r;
}
static uint64_t range_gen_raw_locked(uint32_t addr, uint32_t size) {
    if (!page_gen) page_gen = calloc(1u << 20, 4);
    if (!block_gen) block_gen = calloc(1u << 14, 4);
    uint32_t p0 = addr >> 12, p1 = (uint32_t)(((uint64_t)addr + size + 4095) >> 12);
    static void *hits[65536];
    ULONG_PTR count = 65536; ULONG gran;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, gp(p0 << 12), (SIZE_T)(p1 - p0) << 12, hits, &count, &gran) != 0) return ++gen_clock;
    if (count) {
        uint32_t g = (uint32_t)++gen_clock;
        for (ULONG_PTR i = 0; i < count; i++) { uint32_t pg = (uint32_t)(((uint8_t *)hits[i] - guest_mem) >> 12); page_gen[pg] = g; block_gen[pg >> 6] = g; }
    }
    uint64_t m = 0;
    for (uint32_t pg = p0; pg < p1; pg++) if (page_gen[pg] > m) m = page_gen[pg];
    return m;
}
static const uint32_t *tcache_get(const tex_t *t) {
    switch (t->fmt) {   /* float and depth formats keep the direct path */
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x9A: case 0x9B: case 0x9C: case 0x9F: return NULL;
    }
    if (t->w > 4096 || t->h > 4096) return NULL;
    uint32_t size = tex_bytes(t);
    tcache_t *hit = NULL, *victim = &tcache[0];
    for (int i = 0; i < 256; i++) {
        tcache_t *c = &tcache[i];
        if (c->rgba && c->addr == t->addr && c->fmt == t->fmt && c->w == t->w && c->h == t->h && c->pitch == t->pitch
            && c->linear == t->linear && c->remap == t->remap) { hit = c; break; }
        if (c->last_use < victim->last_use) victim = c;
    }
    uint64_t g = range_gen(t->addr, size);
    if (hit && g <= hit->gen) { hit->last_use = ++tcache_clock; return hit->rgba; }
    if (!hit) {
        hit = victim;
        free(hit->rgba);
        hit->addr = t->addr; hit->fmt = t->fmt; hit->w = t->w; hit->h = t->h; hit->pitch = t->pitch;
        hit->linear = t->linear; hit->remap = t->remap; hit->size = size;
        hit->rgba = malloc((size_t)t->w * t->h * 4);
    }
    hit->gen = g;
    for (uint32_t y = 0; y < t->h; y++)
        for (uint32_t x = 0; x < t->w; x++) {
            float c[4];
            texel(t, (int)x, (int)y, c);
            uint32_t r = (uint32_t)(clampf(c[0], 0, 1) * 255 + .5f), g = (uint32_t)(clampf(c[1], 0, 1) * 255 + .5f);
            uint32_t b = (uint32_t)(clampf(c[2], 0, 1) * 255 + .5f), a = (uint32_t)(clampf(c[3], 0, 1) * 255 + .5f);
            hit->rgba[y * t->w + x] = r | g << 8 | b << 16 | a << 24;
        }
    hit->last_use = ++tcache_clock;
    return hit->rgba;
}
static inline void cached_texel(const tex_t *t, int x, int y, float out[4]) {
    uint32_t v = t->cache[(uint32_t)y * t->w + (uint32_t)x];
    out[0] = (v & 255) * (1 / 255.0f); out[1] = ((v >> 8) & 255) * (1 / 255.0f);
    out[2] = ((v >> 16) & 255) * (1 / 255.0f); out[3] = (v >> 24) * (1 / 255.0f);
}

static void tex_sample(const tex_t *t, float s, float tt, float out[4]) {
    if (!t->enabled || !t->addr) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
    if (t->cache) {
        float u = t->unnorm ? s : s * (float)t->w, v = t->unnorm ? tt : tt * (float)t->h;
        if (!t->mag_linear) {
            cached_texel(t, wrap_coord((int)floorf(u), (int)t->w, t->wrap_s), wrap_coord((int)floorf(v), (int)t->h, t->wrap_t), out);
            return;
        }
        u -= 0.5f; v -= 0.5f;
        int x0 = (int)floorf(u), y0 = (int)floorf(v);
        float fx = u - x0, fy = v - y0;
        int xa = wrap_coord(x0, (int)t->w, t->wrap_s), xb = wrap_coord(x0 + 1, (int)t->w, t->wrap_s);
        int ya = wrap_coord(y0, (int)t->h, t->wrap_t), yb = wrap_coord(y0 + 1, (int)t->h, t->wrap_t);
        float c00[4], c10[4], c01[4], c11[4];
        cached_texel(t, xa, ya, c00); cached_texel(t, xb, ya, c10); cached_texel(t, xa, yb, c01); cached_texel(t, xb, yb, c11);
        for (int k = 0; k < 4; k++) out[k] = (c00[k] * (1 - fx) + c10[k] * fx) * (1 - fy) + (c01[k] * (1 - fx) + c11[k] * fx) * fy;
        return;
    }
    float u = t->unnorm ? s : s * (float)t->w, v = t->unnorm ? tt : tt * (float)t->h;
    if (!t->mag_linear) {
        int x = wrap_coord((int)floorf(u), (int)t->w, t->wrap_s), y = wrap_coord((int)floorf(v), (int)t->h, t->wrap_t);
        texel(t, x, y, out);
        return;
    }
    u -= 0.5f; v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    float c00[4], c10[4], c01[4], c11[4];
    int xa = wrap_coord(x0, (int)t->w, t->wrap_s), xb = wrap_coord(x0 + 1, (int)t->w, t->wrap_s);
    int ya = wrap_coord(y0, (int)t->h, t->wrap_t), yb = wrap_coord(y0 + 1, (int)t->h, t->wrap_t);
    texel(t, xa, ya, c00); texel(t, xb, ya, c10); texel(t, xa, yb, c01); texel(t, xb, yb, c11);
    for (int k = 0; k < 4; k++)
        out[k] = (c00[k] * (1 - fx) + c10[k] * fx) * (1 - fy) + (c01[k] * (1 - fx) + c11[k] * fx) * fy;
}

/* ------------------------------------------------------------------ fragment program interpreter */
typedef struct {
    fp_insn_t insn[512];
    int n;
    uint32_t addr, hash;
    uint32_t inputs;   /* bit i: input attribute i is read */
    int maxR, maxH;    /* highest temp registers touched, to clear only those per pixel */
    int has_kill;      /* KIL present: tests can't run before the program */
    uint32_t ctl;
} fp_prog_t;
static fp_prog_t fp_cache[64];
static int fp_cache_next;

static uint32_t fp_hash(uint32_t a) {   /* constants change in place, so hash the words up to the end bit */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 512 * 8; i += 4) {
        uint32_t w = ld_u32(a + 4 * (uint32_t)i), d = w >> 16 | w << 16;
        h = (h ^ w) * 16777619u;
        h = (h ^ ld_u32(a + 4 * (uint32_t)i + 4)) * 16777619u;
        h = (h ^ ld_u32(a + 4 * (uint32_t)i + 8)) * 16777619u;
        h = (h ^ ld_u32(a + 4 * (uint32_t)i + 12)) * 16777619u;
        if (d & 1) {   /* end bit; include a trailing constant block */
            h = (h ^ ld_u32(a + 4 * (uint32_t)i + 16)) * 16777619u;
            h = (h ^ ld_u32(a + 4 * (uint32_t)i + 20)) * 16777619u;
            h = (h ^ ld_u32(a + 4 * (uint32_t)i + 24)) * 16777619u;
            h = (h ^ ld_u32(a + 4 * (uint32_t)i + 28)) * 16777619u;
            break;
        }
    }
    return h;
}
static fp_prog_t *fp_get(uint32_t addr) {
    uint32_t h = fp_hash(addr);
    for (int i = 0; i < 64; i++) if (fp_cache[i].addr == addr && fp_cache[i].hash == h && fp_cache[i].n) return &fp_cache[i];
    fp_prog_t *p = &fp_cache[fp_cache_next++ & 63];
    p->addr = addr; p->hash = h;
    p->n = fp_decode(addr, p->insn, 512);
    p->inputs = 0; p->maxR = 4; p->maxH = 8; p->has_kill = 0;
    for (int i = 0; i < p->n; i++) if (p->insn[i].op == 0x12) p->has_kill = 1;
    for (int i = 0; i < p->n; i++) {
        const fp_insn_t *I = &p->insn[i];
        if (!I->no_dest) { if (I->dst_fp16) { if (I->dst > p->maxH) p->maxH = I->dst; } else if (I->dst > p->maxR) p->maxR = I->dst; }
        for (int k = 0; k < 3; k++) if (I->src[k].type == 0) { if (I->src[k].fp16) { if (I->src[k].index > p->maxH) p->maxH = I->src[k].index; } else if (I->src[k].index > p->maxR) p->maxR = I->src[k].index; }
    }
    if (p->maxR > 47) p->maxR = 47; if (p->maxH > 95) p->maxH = 95;
    for (int i = 0; i < p->n; i++)
        for (int s = 0; s < 3; s++) if (p->insn[i].src[s].type == 1) p->inputs |= 1u << p->insn[i].input;
    return p;
}

typedef struct {
    vec4 in[16];      /* fragment inputs: WPOS, COL0, COL1, FOGC, TEX0..9, SSA */
    vec4 out[5];      /* colour outputs and depth */
    float depth;
    int killed, depth_written;
} frag_t;

static inline int fp_cond(const fp_insn_t *I, const vec4 *cc, int k) {
    float v = cc[I->cond_reg].v[I->cond_swz[k]];
    if (I->exec_lt && I->exec_eq && I->exec_gt) return 1;
    return (I->exec_lt && v < 0) || (I->exec_eq && v == 0) || (I->exec_gt && v > 0);
}

static void run_fp(const fp_prog_t *P, frag_t *F, const tex_t *tex) {
    vec4 R[48], H[96], cc[2];
    memset(R, 0, sizeof(vec4) * (P->maxR + 1)); memset(H, 0, sizeof(vec4) * (P->maxH + 1)); memset(cc, 0, sizeof cc);
    for (int pc = 0; pc < P->n; pc++) {
        const fp_insn_t *I = &P->insn[pc];
        if (I->op & 0x40) continue;   /* flow control not implemented yet: straight-line */
        vec4 s[3];
        for (int n = 0; n < 3; n++) {
            const fp_src_t *S = &I->src[n];
            vec4 v = {{0}};
            if (S->type == 0) v = S->fp16 ? H[S->index % 96] : R[S->index % 48];
            else if (S->type == 1) v = F->in[I->input];
            else if (S->type == 2) memcpy(v.v, I->constant, 16);
            for (int k = 0; k < 4; k++) {
                float x = v.v[S->swz[k]];
                if (S->abs) x = fabsf(x);
                if (S->neg) x = -x;
                s[n].v[k] = x;
            }
        }
        if (I->bx2) for (int k = 0; k < 4; k++) s[0].v[k] = s[0].v[k] * 2 - 1;
        const float *A = s[0].v, *B = s[1].v, *C = s[2].v;
        vec4 r = {{0}};
        int write = 1;
        switch (I->op) {
        case 0x00: write = 0; break;
        case 0x01: r = s[0]; break;
        case 0x02: for (int k = 0; k < 4; k++) r.v[k] = A[k] * B[k]; break;
        case 0x03: for (int k = 0; k < 4; k++) r.v[k] = A[k] + B[k]; break;
        case 0x04: for (int k = 0; k < 4; k++) r.v[k] = A[k] * B[k] + C[k]; break;
        case 0x05: { float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; r = (vec4){{d, d, d, d}}; break; }
        case 0x06: { float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3]; r = (vec4){{d, d, d, d}}; break; }
        case 0x07: r = (vec4){{1, A[1] * B[1], A[2], B[3]}}; break;
        case 0x08: for (int k = 0; k < 4; k++) r.v[k] = fminf(A[k], B[k]); break;
        case 0x09: for (int k = 0; k < 4; k++) r.v[k] = fmaxf(A[k], B[k]); break;
        case 0x0A: for (int k = 0; k < 4; k++) r.v[k] = A[k] < B[k]; break;
        case 0x0B: for (int k = 0; k < 4; k++) r.v[k] = A[k] >= B[k]; break;
        case 0x0C: for (int k = 0; k < 4; k++) r.v[k] = A[k] <= B[k]; break;
        case 0x0D: for (int k = 0; k < 4; k++) r.v[k] = A[k] > B[k]; break;
        case 0x0E: for (int k = 0; k < 4; k++) r.v[k] = A[k] != B[k]; break;
        case 0x0F: for (int k = 0; k < 4; k++) r.v[k] = A[k] == B[k]; break;
        case 0x10: for (int k = 0; k < 4; k++) r.v[k] = A[k] - floorf(A[k]); break;
        case 0x11: for (int k = 0; k < 4; k++) r.v[k] = floorf(A[k]); break;
        case 0x12: {   /* KIL: discard where the condition passes */
            for (int k = 0; k < 4; k++) if (fp_cond(I, cc, k)) { F->killed = 1; return; }
            write = 0; break;
        }
        case 0x15: case 0x16: r = (vec4){{0}}; break;   /* DDX/DDY: no quads yet */
        case 0x17: case 0x18: case 0x19: case 0x2F: case 0x31: {   /* TEX, TXP, TXD, TXL, TXB */
            float u = A[0], v = A[1];
            if (I->op == 0x18 && A[3] != 0) { u /= A[3]; v /= A[3]; }
            tex_sample(&tex[I->tex], u, v, r.v);
            break;
        }
        case 0x1A: { float x = 1.0f / A[0]; r = (vec4){{x, x, x, x}}; break; }
        case 0x1B: { float x = 1.0f / sqrtf(fabsf(A[0])); r = (vec4){{x, x, x, x}}; break; }
        case 0x1C: { float x = exp2f(A[0]); r = (vec4){{x, x, x, x}}; break; }
        case 0x1D: { float x = log2f(fabsf(A[0])); r = (vec4){{x, x, x, x}}; break; }
        case 0x1E: { float lx = fmaxf(A[0], 0), ly = fmaxf(A[1], 0), lw = clampf(A[3], -127.9961f, 127.9961f);
                     r = (vec4){{1, lx, lx > 0 ? powf(ly, lw) : 0, 1}}; break; }
        case 0x1F: for (int k = 0; k < 4; k++) r.v[k] = A[k] * B[k] + (1 - A[k]) * C[k]; break;   /* LRP */
        case 0x20: r = (vec4){{1, 1, 1, 1}}; break;
        case 0x21: break;
        case 0x22: { float x = cosf(A[0]); r = (vec4){{x, x, x, x}}; break; }
        case 0x23: { float x = sinf(A[0]); r = (vec4){{x, x, x, x}}; break; }
        case 0x26: { float x = powf(A[0], B[0]); r = (vec4){{x, x, x, x}}; break; }
        case 0x2E: { float d = A[0] * B[0] + A[1] * B[1] + C[2]; r = (vec4){{d, d, d, d}}; break; }   /* DP2A */
        case 0x36: {   /* REFL: A - 2 (A.B / B.B) B, approximation */
            float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2], n = B[0] * B[0] + B[1] * B[1] + B[2] * B[2];
            float k2 = n ? 2 * d / n : 0;
            for (int k = 0; k < 3; k++) r.v[k] = A[k] - k2 * B[k];
            break;
        }
        case 0x38: { float d = A[0] * B[0] + A[1] * B[1]; r = (vec4){{d, d, d, d}}; break; }   /* DP2 */
        case 0x39: {   /* NRM */
            float l = sqrtf(A[0] * A[0] + A[1] * A[1] + A[2] * A[2]);
            float il = l ? 1 / l : 0;
            r = (vec4){{A[0] * il, A[1] * il, A[2] * il, A[3]}}; break;
        }
        case 0x3A: { float x = B[0]; for (int k = 0; k < 4; k++) r.v[k] = A[k] / x; break; }        /* DIV */
        case 0x3B: { float x = sqrtf(B[0]); for (int k = 0; k < 4; k++) r.v[k] = A[k] / x; break; } /* DIVSQ */
        case 0x3D: case 0x3E: write = 0; break;   /* FENCT/FENCB */
        default: r = s[0]; break;
        }
        if (!write) continue;
        static const float scales[8] = {1, 2, 4, 8, 1, 0.5f, 0.25f, 0.125f};
        if (I->scale) for (int k = 0; k < 4; k++) r.v[k] *= scales[I->scale];
        if (I->saturate) for (int k = 0; k < 4; k++) r.v[k] = clampf(r.v[k], 0, 1);
        else if (I->prec == 2) for (int k = 0; k < 4; k++) r.v[k] = clampf(r.v[k], -2, 2);
        else if (I->prec == 3) for (int k = 0; k < 4; k++) r.v[k] = clampf(r.v[k], -2, 2);
        for (int k = 0; k < 4; k++) {
            if (!(I->mask >> (3 - k) & 1)) continue;
            if (!fp_cond(I, cc, k)) continue;
            if (I->set_cond) cc[I->cond_mod_reg].v[k] = r.v[k];
            if (I->no_dest) continue;
            if (I->dst_fp16) H[I->dst % 96].v[k] = r.v[k];
            else R[I->dst % 48].v[k] = r.v[k];
        }
    }
    uint32_t ctl = P->ctl;
    if (ctl & 0x40) { F->out[0] = R[0]; F->out[1] = R[2]; F->out[2] = R[3]; F->out[3] = R[4]; }
    else { F->out[0] = H[0]; F->out[1] = H[4]; F->out[2] = H[6]; F->out[3] = H[8]; }
    if (ctl & 0x0E) { F->depth = R[1].v[2]; F->depth_written = 1; }
}

/* ------------------------------------------------------------------ output merger */
typedef struct {
    uint32_t color[4], cpitch[4], ctarget_count, cfmt, zaddr, zpitch, zfmt;
    int swizzled; uint32_t lw, lh;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int aa_x, aa_y;   /* multisampled surfaces store 2 (mode 3) or 2x2 (modes 4, 5) samples per pixel */
    int origin_bottom; uint32_t win_h;
    float vp_scale[4], vp_off[4];
    int depth_test, depth_write; uint32_t depth_func;
    int blend; uint32_t sfac, dfac, beq, bcolor;
    int alpha_test; uint32_t alpha_func; float alpha_ref;
    uint32_t color_mask;
    int cull; uint32_t cull_face, front_face;
    int stencil, two_sided;
    struct { uint32_t func, ref, mask, wmask, op_fail, op_zfail, op_zpass; } st[2];   /* front, back */
} om_t;

static void om_setup(om_t *o) {
    uint32_t fmt = REG(0x0208);
    o->cfmt = fmt & 31; o->zfmt = (fmt >> 5) & 7; o->swizzled = ((fmt >> 8) & 15) == 2;
    { uint32_t aa = (fmt >> 12) & 15; o->aa_x = aa >= 3 ? 2 : 1; o->aa_y = aa >= 4 ? 2 : 1; }
    o->lw = (fmt >> 16) & 0xFF; o->lh = (fmt >> 24) & 0xFF;
    static const uint32_t offs[4] = {0x0210, 0x0218, 0x0288, 0x028C}, pitches[4] = {0x020C, 0x021C, 0x0280, 0x0284}, dmas[4] = {0x0194, 0x018C, 0x01B4, 0x01B8};
    uint32_t target = REG(0x0220);
    static const uint8_t list[][4] = {{0}, {0}, {1}, {0}};
    (void)list;
    uint32_t idx[4], n = 0;
    switch (target) {
    case 0x01: idx[n++] = 0; break;
    case 0x02: idx[n++] = 1; break;
    case 0x13: idx[n++] = 0; idx[n++] = 1; break;
    case 0x17: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; break;
    case 0x1F: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; idx[n++] = 3; break;
    }
    o->ctarget_count = n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = idx[i];
        o->color[i] = loc_addr(REG(offs[k]), REG(dmas[k]) == 0xFEED0001);
        o->cpitch[i] = REG(pitches[k]);
    }
    o->zaddr = loc_addr(REG(0x0214), REG(0x0198) == 0xFEED0001);
    o->zpitch = REG(0x022C);
    uint32_t ch = REG(0x0200), cv = REG(0x0204);
    o->clip_x0 = ch & 0xFFFF; o->clip_x1 = o->clip_x0 + (int)(ch >> 16);
    o->clip_y0 = cv & 0xFFFF; o->clip_y1 = o->clip_y0 + (int)(cv >> 16);
    uint32_t sx = REG(0x08C0), sy = REG(0x08C4);
    if (sx >> 16) { int a = sx & 0xFFFF, b = a + (int)(sx >> 16); if (a > o->clip_x0) o->clip_x0 = a; if (b < o->clip_x1) o->clip_x1 = b; }
    if (sy >> 16) { int a = sy & 0xFFFF, b = a + (int)(sy >> 16); if (a > o->clip_y0) o->clip_y0 = a; if (b < o->clip_y1) o->clip_y1 = b; }
    o->clip_x0 *= o->aa_x; o->clip_x1 *= o->aa_x; o->clip_y0 *= o->aa_y; o->clip_y1 *= o->aa_y;   /* in samples */
    uint32_t sw = REG(0x1D88);
    o->win_h = sw & 0xFFF; o->origin_bottom = (sw >> 12) & 1;
    for (int k = 0; k < 4; k++) { o->vp_scale[k] = u2f(REG(0x0A30 + 4 * k)); o->vp_off[k] = u2f(REG(0x0A20 + 4 * k)); }
    o->depth_test = REG(0x0A74) & 1; o->depth_write = REG(0x0A70) & 1; o->depth_func = REG(0x0A6C);
    o->blend = REG(0x0310) & 1; o->sfac = REG(0x0314); o->dfac = REG(0x0318); o->beq = REG(0x0320); o->bcolor = REG(0x031C);
    o->alpha_test = REG(0x0304) & 1; o->alpha_func = REG(0x0308); o->alpha_ref = (REG(0x030C) & 0xFF) / 255.0f;
    o->color_mask = REG(0x0324);
    o->cull = REG(0x183C) & 1; o->cull_face = REG(0x1830); o->front_face = REG(0x1834);
    o->stencil = REG(0x0328) & 1; o->two_sided = REG(0x0348) & 1;
    for (int b = 0; b < 2; b++) {   /* front at 0x32C.. (enable 0x328), back at 0x34C.. (two-sided enable 0x348) */
        uint32_t base = b ? 0x034C : 0x032C;
        o->st[b].wmask = REG(base) & 0xFF; o->st[b].func = REG(base + 4); o->st[b].ref = REG(base + 8) & 0xFF;
        o->st[b].mask = REG(base + 12) & 0xFF; o->st[b].op_fail = REG(base + 16); o->st[b].op_zfail = REG(base + 20); o->st[b].op_zpass = REG(base + 24);
    }
}

static inline int cmp_func(uint32_t f, float a, float b) {   /* GL compare: a (incoming) vs b (stored) */
    switch (f & 0xF) { case 0: return 0; case 1: return a < b; case 2: return a == b; case 3: return a <= b;
    case 4: return a > b; case 5: return a != b; case 6: return a >= b; default: return 1; }
}

static uint32_t surf_addr(const om_t *o, uint32_t base, uint32_t pitch, int x, int y, uint32_t bpp) {
    if (o->swizzled) return base + swz_index((uint32_t)x, (uint32_t)y, o->lw, o->lh) * bpp;
    return base + (uint32_t)y * pitch + (uint32_t)x * bpp;
}

static float blend_factor(uint32_t f, int c, const float *src, const float *dst, const float *k) {
    switch (f) {
    case 0: return 0; case 1: return 1;
    case 0x300: return src[c]; case 0x301: return 1 - src[c];
    case 0x302: return src[3]; case 0x303: return 1 - src[3];
    case 0x304: return dst[3]; case 0x305: return 1 - dst[3];
    case 0x306: return dst[c]; case 0x307: return 1 - dst[c];
    case 0x308: return c == 3 ? 1 : fminf(src[3], 1 - dst[3]);
    case 0x8001: return k[c]; case 0x8002: return 1 - k[c];
    case 0x8003: return k[3]; case 0x8004: return 1 - k[3];
    default: return 1;
    }
}

static void read_color(const om_t *o, uint32_t a, float *c) {   /* rgba */
    switch (o->cfmt) {
    case 3: { uint16_t v = ld_u16(a); unpack_565(v, &c[0], &c[1], &c[2]); c[3] = 1; break; }
    case 0xB: for (int k = 0; k < 4; k++) c[k] = half2f(ld_u16(a + 2 * k)); break;
    case 0xC: for (int k = 0; k < 4; k++) c[k] = ld_f32(a + 4 * k); break;
    case 0xD: c[0] = ld_f32(a); c[1] = c[2] = 0; c[3] = 1; break;
    case 9: c[0] = c[1] = c[2] = ld_u8(a) / 255.0f; c[3] = 1; break;
    case 0xA: c[0] = ld_u8(a) / 255.0f; c[1] = ld_u8(a + 1) / 255.0f; c[2] = 0; c[3] = 1; break;
    case 0xE: case 0xF: case 0x10: c[3] = ld_u8(a) / 255.0f; c[2] = ld_u8(a + 1) / 255.0f; c[1] = ld_u8(a + 2) / 255.0f; c[0] = ld_u8(a + 3) / 255.0f; break;
    default: c[3] = ld_u8(a) / 255.0f; c[0] = ld_u8(a + 1) / 255.0f; c[1] = ld_u8(a + 2) / 255.0f; c[2] = ld_u8(a + 3) / 255.0f; break;
    }
}
static uint16_t f2half(float f) {
    uint32_t u = f2u(f), s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 255) - 112; uint32_t m = u & 0x7FFFFF;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    return (uint16_t)(s | (uint32_t)e << 10 | m >> 13);
}
static void write_color(const om_t *o, uint32_t a, const float *c) {
    uint32_t m = o->color_mask;   /* bit 24 A, 16 R, 8 G, 0 B */
    uint8_t q[4];
    for (int k = 0; k < 4; k++) q[k] = (uint8_t)(clampf(c[k], 0, 1) * 255.0f + 0.5f);
    switch (o->cfmt) {
    case 3: { uint16_t v = (uint16_t)((uint32_t)(clampf(c[0], 0, 1) * 31 + .5f) << 11 | (uint32_t)(clampf(c[1], 0, 1) * 63 + .5f) << 5 | (uint32_t)(clampf(c[2], 0, 1) * 31 + .5f)); st_u16(a, v); break; }
    case 0xB: for (int k = 0; k < 4; k++) st_u16(a + 2 * k, f2half(c[k])); break;
    case 0xC: for (int k = 0; k < 4; k++) st_f32(a + 4 * k, c[k]); break;
    case 0xD: st_f32(a, c[0]); break;
    case 9: st_u8(a, q[0]); break;
    case 0xA: st_u8(a, q[0]); st_u8(a + 1, q[1]); break;
    case 0xE: case 0xF: case 0x10:
        if (m >> 24 & 1) st_u8(a, q[3]); if (m >> 0 & 1) st_u8(a + 1, q[2]); if (m >> 8 & 1) st_u8(a + 2, q[1]); if (m >> 16 & 1) st_u8(a + 3, q[0]);
        break;
    default:
        if (m >> 24 & 1) st_u8(a, o->cfmt == 4 || o->cfmt == 5 ? 0xFF : q[3]);
        if (m >> 16 & 1) st_u8(a + 1, q[0]); if (m >> 8 & 1) st_u8(a + 2, q[1]); if (m & 1) st_u8(a + 3, q[2]);
        break;
    }
}
static uint32_t color_bpp(uint32_t f) { return f == 3 || f == 1 || f == 2 ? 2 : f == 9 ? 1 : f == 0xA ? 2 : f == 0xB ? 8 : f == 0xC ? 16 : 4; }

/* ------------------------------------------------------------------ rasteriser */
typedef struct { float x, y, z, w; float a[16][4]; } rv_t;   /* window position + 1/w, attributes / w */

typedef struct {
    om_t om; tex_t tex[16]; const fp_prog_t *fp; uint32_t inputs;
} draw_ctx_t;

static const int fp_in_from_vp[16] = {-1, 1, 2, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, -1, -1};  /* fragment input -> VP output */

/* apply a stencil op (which: 0 stencil fail, 1 depth fail, 2 pass) to a Z24S8 word; returns the new word */
static uint32_t stencil_update(const om_t *o, uint32_t zaddr, uint32_t zword, int sb, int which) {
    uint32_t op = which == 0 ? o->st[sb].op_fail : which == 1 ? o->st[sb].op_zfail : o->st[sb].op_zpass;
    uint32_t s = zword & 0xFF, n = s;
    switch (op) {
    case 0: n = 0; break;                                   /* ZERO */
    case 0x1E00: return zword;                              /* KEEP */
    case 0x1E01: n = o->st[sb].ref; break;                  /* REPLACE */
    case 0x1E02: n = s < 255 ? s + 1 : 255; break;          /* INCR */
    case 0x1E03: n = s > 0 ? s - 1 : 0; break;              /* DECR */
    case 0x150A: n = ~s & 0xFF; break;                      /* INVERT */
    case 0x8507: n = (s + 1) & 0xFF; break;                 /* INCR_WRAP */
    case 0x8508: n = (s - 1) & 0xFF; break;                 /* DECR_WRAP */
    }
    uint32_t w = o->st[sb].wmask;
    n = (s & ~w) | (n & w);
    uint32_t out = (zword & 0xFFFFFF00u) | n;
    if (out != zword) st_u32(zaddr, out);
    return out;
}

static void shade_pixel(const draw_ctx_t *D, int x, int y, float z, const float *bary_w, const rv_t *v[3], int front) {
    const om_t *o = &D->om;
    frag_t F;
    F.killed = 0; F.depth_written = 0;
    for (int i = 1; i < 14; i++) {
        if (!(D->inputs >> i & 1)) continue;
        for (int k = 0; k < 4; k++) F.in[i].v[k] = bary_w[0] * v[0]->a[i][k] + bary_w[1] * v[1]->a[i][k] + bary_w[2] * v[2]->a[i][k];
    }
    F.in[0] = (vec4){{x + 0.5f, y + 0.5f, z, 1}};
    F.in[14] = (vec4){{front ? 1.f : -1.f, 0, 0, 0}};
    uint32_t zaddr = 0, zold = 0;
    float zin = clampf(z, 0, 1);
    if (o->zfmt && o->zaddr) {
        uint32_t zb = o->zfmt == 1 ? 2 : 4;
        zaddr = surf_addr(o, o->zaddr, o->zpitch, x, y, zb);
        zold = zb == 2 ? ld_u16(zaddr) : ld_u32(zaddr);
    }
    float zstored = zaddr ? (o->zfmt == 1 ? zold / 65535.0f : (zold >> 8) / 16777215.0f) : 1.0f;
    int use_stencil = o->stencil && zaddr && o->zfmt == 2;
    int sb = (o->two_sided && !front) ? 1 : 0;
    /* tests may run before the program only when it can't discard or change depth, and alpha test is off */
    int early = !(D->fp->ctl & 0x0E) && !D->fp->has_kill && !o->alpha_test;
    int s_pass = 1, z_pass = 1;
    if (early) {
        if (use_stencil) s_pass = cmp_func(o->st[sb].func, (float)(o->st[sb].ref & o->st[sb].mask), (float)((zold & 0xFF) & o->st[sb].mask));
        if (s_pass && o->depth_test && zaddr) z_pass = cmp_func(o->depth_func, zin, zstored);
        if (!s_pass || !z_pass) { if (use_stencil) stencil_update(o, zaddr, zold, sb, !s_pass ? 0 : 1); return; }
    }
    run_fp(D->fp, &F, D->tex);
    if (F.killed) return;
    if (F.depth_written) zin = clampf(F.depth, 0, 1);
    if (!early) {
        if (o->alpha_test && !cmp_func(o->alpha_func, F.out[0].v[3], o->alpha_ref)) return;
        if (use_stencil) s_pass = cmp_func(o->st[sb].func, (float)(o->st[sb].ref & o->st[sb].mask), (float)((zold & 0xFF) & o->st[sb].mask));
        if (s_pass && o->depth_test && zaddr) z_pass = cmp_func(o->depth_func, zin, zstored);
        if (!s_pass || !z_pass) { if (use_stencil) stencil_update(o, zaddr, zold, sb, !s_pass ? 0 : 1); return; }
    }
    if (use_stencil) zold = stencil_update(o, zaddr, zold, sb, 2);
    for (uint32_t t = 0; t < o->ctarget_count; t++) {
        if (!o->color[t]) continue;
        uint32_t ca = surf_addr(o, o->color[t], o->cpitch[t], x, y, color_bpp(o->cfmt));
        float src[4]; memcpy(src, F.out[t].v, 16);
        if (o->blend) {
            float dst[4], k[4] = {((o->bcolor >> 16) & 255) / 255.0f, ((o->bcolor >> 8) & 255) / 255.0f, (o->bcolor & 255) / 255.0f, (o->bcolor >> 24) / 255.0f};
            read_color(o, ca, dst);
            float res[4];
            for (int ch = 0; ch < 4; ch++) {
                uint32_t sf = ch == 3 ? o->sfac >> 16 : o->sfac & 0xFFFF, df = ch == 3 ? o->dfac >> 16 : o->dfac & 0xFFFF;
                uint32_t eq = ch == 3 ? o->beq >> 16 : o->beq & 0xFFFF;
                float s = clampf(src[ch], 0, 1) * blend_factor(sf, ch, src, dst, k), d = dst[ch] * blend_factor(df, ch, src, dst, k);
                switch (eq) {
                case 0x8007: res[ch] = fminf(src[ch], dst[ch]); break;
                case 0x8008: res[ch] = fmaxf(src[ch], dst[ch]); break;
                case 0x800A: res[ch] = s - d; break;
                case 0x800B: res[ch] = d - s; break;
                default: res[ch] = s + d; break;
                }
            }
            write_color(o, ca, res);
        } else write_color(o, ca, src);
    }
    if (o->depth_write && zaddr) {
        if (o->zfmt == 1) st_u16(zaddr, (uint16_t)(zin * 65535.0f));
        else st_u32(zaddr, ((uint32_t)(zin * 16777215.0f) << 8) | (zold & 0xFF));
    }
}

static void raster_tri(const draw_ctx_t *D, const rv_t *a, const rv_t *b, const rv_t *c, int y0, int y1) {
    const om_t *o = &D->om;
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0 || area != area) return;
    int ccw = area > 0;   /* in window space with y down (memory rows) */
    /* GL front face is decided in a y-up space: flip the sense when rows go down */
    int front = o->front_face == 0x0900 ? !ccw : ccw;
    if (!o->origin_bottom) front = !front;   /* matches the D3D path, verified on the race scene */
    if (o->cull) {
        if (o->cull_face == 0x0408) return;
        if (o->cull_face == 0x0405 && !front) return;
        if (o->cull_face == 0x0404 && front) return;
    }
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), ya = (int)floorf(miny), yb = (int)ceilf(maxy);
    if (x0 < o->clip_x0) x0 = o->clip_x0; if (x1 > o->clip_x1) x1 = o->clip_x1;
    if (ya < o->clip_y0) ya = o->clip_y0; if (yb > o->clip_y1) yb = o->clip_y1;
    if (ya < y0) ya = y0; if (yb > y1) yb = y1;
    if (x0 >= x1 || ya >= yb) return;
    const rv_t *v[3] = {a, b, c};
    float inv = 1.0f / area;
    for (int y = ya; y < yb; y++) {
        float py = y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                /* top-left rule light: accept exact zeros only */
                if (w0 < -1e-7f || w1 < -1e-7f || w2 < -1e-7f) continue;
            }
            float z = w0 * a->z + w1 * b->z + w2 * c->z;
            float iw = w0 * a->w + w1 * b->w + w2 * c->w;
            float pw[3] = {w0 * a->w / iw, w1 * b->w / iw, w2 * c->w / iw};
            shade_pixel(D, x, y, z, pw, v, front);
        }
    }
}

/* ------------------------------------------------------------------ draw call */
static uint32_t *elems; static uint32_t nelems, cap_elems;
static unsigned cur_prim;
#include "rsx_d3d.h"
int rsx_use_d3d = -1;   /* MNR_RSX=d3d */
static int nodraw = -1;
static long draw_from = -1;
static int skip_draw(void) {   /* MNR_RSX_NODRAW=1: never draw; MNR_RSX_DRAW_FROM=<flip>: start drawing at that flip */
    if (rsx_use_d3d < 0) { const char *e = getenv("MNR_RSX"); rsx_use_d3d = !(e && !strcmp(e, "sw")); }   /* D3D11 unless MNR_RSX=sw */
    if (nodraw < 0) {
        const char *e = getenv("MNR_RSX_NODRAW"); nodraw = e && *e == '1';
        e = getenv("MNR_RSX_DRAW_FROM"); draw_from = e ? atol(e) : 0;
    }
    return nodraw || rsx_flips < draw_from || !rsx_heavy_started;
}
static uint32_t draw_count;

typedef struct { vtx_t v; uint32_t idx; int valid; } vcache_t;
static vcache_t *vcache;

static void to_window(const om_t *o, const vec4 *p, rv_t *r) {
    float iw = 1.0f / p->v[3];
    float nx = p->v[0] * iw, ny = p->v[1] * iw, nz = p->v[2] * iw;
    r->x = (nx * o->vp_scale[0] + o->vp_off[0]) * o->aa_x;   /* supersample into the multisampled layout */
    r->y = (ny * o->vp_scale[1] + o->vp_off[1]) * o->aa_y;
    r->z = nz * o->vp_scale[2] + o->vp_off[2];
    /* no origin flip here: the game's viewport (scale y < 0) already maps NDC top-down into memory rows */
    r->w = iw;
}

typedef struct { rv_t v[3]; } tri_t;
static tri_t *tris; static uint32_t ntris, cap_tris;
static void tri_push(const rv_t *a, const rv_t *b, const rv_t *c) {
    if (ntris == cap_tris) { cap_tris = cap_tris ? cap_tris * 2 : 1024; tris = realloc(tris, cap_tris * sizeof *tris); }
    tri_t *t = &tris[ntris++];
    t->v[0] = *a; t->v[1] = *b; t->v[2] = *c;
}

/* worker pool: each worker rasterises every triangle of the draw inside its own 8-row bands */
#define BAND 8
static int nworkers;
static HANDLE wk_go[32], wk_done[32];
static const draw_ctx_t *wk_draw;
static DWORD WINAPI raster_worker(void *p) {
    int id = (int)(intptr_t)p;
    for (;;) {
        WaitForSingleObject(wk_go[id], INFINITE);
        const draw_ctx_t *D = wk_draw;
        int y0 = D->om.clip_y0, y1 = D->om.clip_y1;
        for (int b = y0 / BAND + id; b * BAND < y1; b += nworkers) {
            int ba = b * BAND, bb = ba + BAND;
            if (ba < y0) ba = y0; if (bb > y1) bb = y1;
            for (uint32_t i = 0; i < ntris; i++) {
                const tri_t *t = &tris[i];
                float mny = fminf(t->v[0].y, fminf(t->v[1].y, t->v[2].y)), mxy = fmaxf(t->v[0].y, fmaxf(t->v[1].y, t->v[2].y));
                if (mxy < ba || mny > bb) continue;
                raster_tri(D, &t->v[0], &t->v[1], &t->v[2], ba, bb);
            }
        }
        SetEvent(wk_done[id]);
    }
    return 0;
}
static void raster_all(const draw_ctx_t *D) {
    if (!ntris) return;
    if (!nworkers) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        nworkers = (int)si.dwNumberOfProcessors - 2;
        if (nworkers < 1) nworkers = 1; if (nworkers > 32) nworkers = 32;
        for (int i = 0; i < nworkers; i++) {
            wk_go[i] = CreateEventA(NULL, FALSE, FALSE, NULL); wk_done[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
            CreateThread(NULL, 1 << 20, raster_worker, (void *)(intptr_t)i, 0, NULL);
        }
    }
    /* small draws: do them here */
    float area = 0;
    for (uint32_t i = 0; i < ntris && area < 20000; i++) {
        const tri_t *t = &tris[i];
        area += fabsf((t->v[1].x - t->v[0].x) * (t->v[2].y - t->v[0].y) - (t->v[1].y - t->v[0].y) * (t->v[2].x - t->v[0].x)) * 0.5f;
    }
    if (area < 20000 || nworkers == 1) {
        for (uint32_t i = 0; i < ntris; i++) raster_tri(D, &tris[i].v[0], &tris[i].v[1], &tris[i].v[2], D->om.clip_y0, D->om.clip_y1);
    } else {
        wk_draw = D;
        for (int i = 0; i < nworkers; i++) SetEvent(wk_go[i]);
        WaitForMultipleObjects((DWORD)nworkers, wk_done, TRUE, INFINITE);
    }
    ntris = 0;
}

static void emit_tri(const draw_ctx_t *D, const vtx_t *t[3]) {
    /* clip against w > epsilon (near), Sutherland-Hodgman in clip space */
    typedef struct { vec4 p; vec4 attr[16]; } cv_t;
    cv_t in[9], out[9];
    int n = 3;
    for (int i = 0; i < 3; i++) {
        in[i].p = t[i]->out[0];
        for (int k = 1; k < 14; k++) { int s = fp_in_from_vp[k]; if (s >= 0) in[i].attr[k] = t[i]->out[s]; }
    }
    const float eps = 1e-5f;
    int m = 0;
    for (int i = 0; i < n; i++) {
        const cv_t *A = &in[i], *B = &in[(i + 1) % n];
        float da = A->p.v[3] - eps, db = B->p.v[3] - eps;
        if (da >= 0) out[m++] = *A;
        if ((da >= 0) != (db >= 0)) {
            float tt = da / (da - db);
            cv_t *C = &out[m++];
            for (int k = 0; k < 4; k++) C->p.v[k] = A->p.v[k] + (B->p.v[k] - A->p.v[k]) * tt;
            for (int j = 1; j < 14; j++) for (int k = 0; k < 4; k++) C->attr[j].v[k] = A->attr[j].v[k] + (B->attr[j].v[k] - A->attr[j].v[k]) * tt;
        }
    }
    if (m < 3) return;
    rv_t rv[9];
    for (int i = 0; i < m; i++) {
        to_window(&D->om, &out[i].p, &rv[i]);
        for (int j = 1; j < 14; j++) for (int k = 0; k < 4; k++) rv[i].a[j][k] = out[i].attr[j].v[k];
    }
    for (int i = 1; i + 1 < m; i++) tri_push(&rv[0], &rv[i], &rv[i + 1]);
}

static const vtx_t *shade_vertex(uint32_t idx) {
    vcache_t *c = &vcache[idx & (MAXV - 1)];
    if (c->valid && c->idx == idx) return &c->v;
    run_vp(idx, &c->v);
    c->idx = idx; c->valid = 1;
    return &c->v;
}

double rsx_draw_seconds;
static void draw_flush_inner(void);
static void draw_flush(void) {
    LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0);
    draw_flush_inner();
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    rsx_draw_seconds += (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
}
static void draw_flush_inner(void) {
    if (skip_draw() || nelems < 1) { nelems = 0; return; }
    if (rsx_use_d3d == 1) { rsx_d3d_draw(cur_prim, elems, nelems); nelems = 0; return; }
    if (nelems < 3) { nelems = 0; return; }
    if (!vcache) vcache = calloc(MAXV, sizeof *vcache);
    for (uint32_t i = 0; i < MAXV; i++) vcache[i].valid = 0;
    draw_ctx_t *D = malloc(sizeof *D);
    om_setup(&D->om);
    for (int u = 0; u < 16; u++) tex_setup(u, &D->tex[u]);
    fp_prog_t *fp = fp_get(rsx_fp_addr());
    fp->ctl = REG(0x1D60);
    {   /* MNR_HLSL_TEST=1: translate and compile this draw's programs (rsx_hlsltest.c) */
        static int test = -1;
        if (test < 0) { const char *e = getenv("MNR_HLSL_TEST"); test = e && *e == '1'; }
        if (test) {
            uint8_t dims[16];
            for (int u = 0; u < 16; u++) { uint32_t f = REG(0x1A04 + 0x20 * u); dims[u] = (f >> 2 & 1) ? 6 : (uint8_t)((f >> 4) & 15); }
            rsx_hlsl_test((const uint32_t (*)[4])rsx_vp, REG(0x1EA0) & 511, fp->insn, fp->n, fp->ctl, dims);
        }
    }
    D->fp = fp;
    D->inputs = D->fp->inputs;
    ntris = 0;
    draw_count++;
    const vtx_t *t[3];
    switch (cur_prim) {
    case 5: for (uint32_t i = 0; i + 2 < nelems; i += 3) { t[0] = shade_vertex(elems[i]); t[1] = shade_vertex(elems[i + 1]); t[2] = shade_vertex(elems[i + 2]); emit_tri(D, t); } break;
    case 6: for (uint32_t i = 0; i + 2 < nelems; i++) {
        if (i & 1) { t[0] = shade_vertex(elems[i + 1]); t[1] = shade_vertex(elems[i]); }
        else { t[0] = shade_vertex(elems[i]); t[1] = shade_vertex(elems[i + 1]); }
        t[2] = shade_vertex(elems[i + 2]); emit_tri(D, t);
    } break;
    case 7: case 10: for (uint32_t i = 1; i + 1 < nelems; i++) { t[0] = shade_vertex(elems[0]); t[1] = shade_vertex(elems[i]); t[2] = shade_vertex(elems[i + 1]); emit_tri(D, t); } break;
    case 8: for (uint32_t i = 0; i + 3 < nelems; i += 4) {
        const vtx_t *q[4]; for (int k = 0; k < 4; k++) q[k] = shade_vertex(elems[i + k]);
        t[0] = q[0]; t[1] = q[1]; t[2] = q[2]; emit_tri(D, t);
        t[0] = q[0]; t[1] = q[2]; t[2] = q[3]; emit_tri(D, t);
    } break;
    case 9: for (uint32_t i = 0; i + 3 < nelems; i += 2) {
        const vtx_t *q[4]; for (int k = 0; k < 4; k++) q[k] = shade_vertex(elems[i + k]);
        t[0] = q[0]; t[1] = q[1]; t[2] = q[3]; emit_tri(D, t);
        t[0] = q[0]; t[1] = q[3]; t[2] = q[2]; emit_tri(D, t);
    } break;
    default: break;   /* points and lines: not drawn yet */
    }
    raster_all(D);
    free(D);
    nelems = 0;
}

static void push_elem(uint32_t e) {
    if (nelems == cap_elems) { cap_elems = cap_elems ? cap_elems * 2 : 4096; elems = realloc(elems, cap_elems * 4); }
    elems[nelems++] = e;
}

void rsx_draw_begin_end(uint32_t prim) {
    if (prim) { cur_prim = prim; nelems = 0; }
    else draw_flush();
}
void rsx_draw_arrays(uint32_t arg) {
    uint32_t first = arg & 0xFFFFFF, count = (arg >> 24) + 1;
    for (uint32_t i = 0; i < count; i++) push_elem(first + i);
}
void rsx_draw_index_array(uint32_t arg) {
    uint32_t first = arg & 0xFFFFFF, count = (arg >> 24) + 1;
    uint32_t dma = REG(0x1820), off = REG(0x181C);
    int u16 = ((dma >> 4) & 15) == 1;
    uint32_t base = loc_addr(off, (dma & 15) == 1);
    if (!base) return;
    uint32_t bi = REG(0x173C);
    int restart = REG(0x1DAC) & 1; uint32_t ridx = REG(0x1DB0);
    if (!restart) {   /* the common case: straight from the guest pointer, in a loop the compiler vectorises */
        while (nelems + count > cap_elems) { cap_elems = cap_elems ? cap_elems * 2 : 4096; elems = realloc(elems, cap_elems * 4); }
        uint32_t *out = elems + nelems;
        if (u16) { const uint16_t *src = (const uint16_t *)gp(base + 2 * first); for (uint32_t i = 0; i < count; i++) out[i] = (uint32_t)__builtin_bswap16(src[i]) + bi; }
        else { const uint32_t *src = (const uint32_t *)gp(base + 4 * first); for (uint32_t i = 0; i < count; i++) out[i] = __builtin_bswap32(src[i]) + bi; }
        nelems += count;
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = u16 ? ld_u16(base + 2 * (first + i)) : ld_u32(base + 4 * (first + i));
        if (restart && e == ridx) {   /* primitive restart: flush and keep the primitive */
            unsigned p = cur_prim; draw_flush(); cur_prim = p; continue;
        }
        push_elem(e + bi);
    }
}

/* CLEAR_SURFACE: bits 0 Z, 1 stencil, 4..7 R G B A */
void rsx_clear(uint32_t mask) {
    if (skip_draw()) return;
    if (rsx_use_d3d == 1) { rsx_d3d_clear(mask); return; }
    om_t o; om_setup(&o);
    uint32_t ch = REG(0x0200), cv = REG(0x0204);
    int x0 = ch & 0xFFFF, x1 = x0 + (int)(ch >> 16), y0 = cv & 0xFFFF, y1 = y0 + (int)(cv >> 16);
    uint32_t sx = REG(0x08C0), sy = REG(0x08C4);   /* clears respect the scissor */
    if (sx >> 16) { int a = sx & 0xFFFF, b = a + (int)(sx >> 16); if (a > x0) x0 = a; if (b < x1) x1 = b; }
    if (sy >> 16) { int a = sy & 0xFFFF, b = a + (int)(sy >> 16); if (a > y0) y0 = a; if (b < y1) y1 = b; }
    x0 *= o.aa_x; x1 *= o.aa_x; y0 *= o.aa_y; y1 *= o.aa_y;
    uint32_t cval = REG(0x1D90), zval = REG(0x1D8C);
    if (mask & 0xF0) {
        float col[4] = {((cval >> 16) & 255) / 255.0f, ((cval >> 8) & 255) / 255.0f, (cval & 255) / 255.0f, (cval >> 24) / 255.0f};
        o.color_mask = (mask & 0x10 ? 1u << 16 : 0) | (mask & 0x20 ? 1u << 8 : 0) | (mask & 0x40 ? 1u : 0) | (mask & 0x80 ? 1u << 24 : 0);
        for (uint32_t t = 0; t < o.ctarget_count; t++) {
            if (!o.color[t]) continue;
            uint32_t bpp = color_bpp(o.cfmt);
            if (!o.swizzled && (o.cfmt == 8 || o.cfmt == 4 || o.cfmt == 5) && (mask & 0xF0) == 0xF0 && x1 > x0) {
                uint32_t be = __builtin_bswap32(o.cfmt == 8 ? cval : cval | 0xFF000000u);
                for (int y = y0; y < y1; y++) {
                    uint32_t *row = (uint32_t *)gp(o.color[t] + (uint32_t)y * o.cpitch[t] + (uint32_t)x0 * 4);
                    for (int x = 0; x < x1 - x0; x++) row[x] = be;
                }
                continue;
            }
            for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) write_color(&o, surf_addr(&o, o.color[t], o.cpitch[t], x, y, bpp), col);
        }
    }
    if ((mask & 3) == 3 && o.zfmt == 2 && o.zaddr && !o.swizzled && x1 > x0) {
        uint32_t be = __builtin_bswap32(zval);
        for (int y = y0; y < y1; y++) {
            uint32_t *row = (uint32_t *)gp(o.zaddr + (uint32_t)y * o.zpitch + (uint32_t)x0 * 4);
            for (int x = 0; x < x1 - x0; x++) row[x] = be;
        }
        return;
    }
    if ((mask & 3) && o.zfmt && o.zaddr) {
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            if (o.zfmt == 1) { if (mask & 1) st_u16(surf_addr(&o, o.zaddr, o.zpitch, x, y, 2), zval & 0xFFFF); }
            else {
                uint32_t a = surf_addr(&o, o.zaddr, o.zpitch, x, y, 4), old = ld_u32(a), v = old;
                if (mask & 1) v = (v & 0xFF) | (zval & 0xFFFFFF00);
                if (mask & 2) v = (v & 0xFFFFFF00) | (zval & 0xFF);
                st_u32(a, v);
            }
        }
    }
}

/* ------------------------------------------------------------------ exports for the D3D11 backend */
#include "rsx_tex.h"
uint32_t rsx_tex_levels(int u);
uint32_t rsx_tex_total_bytes(int u);
/* the D3D backend asks for the same ranges thousands of times per frame: answer from a cache that is
 * cleared at every flip (CPU writes within a frame are picked up at the next one) */
static struct { uint32_t addr, size; uint64_t gen; long frame; } rg_cache[8192];
/* Once per frame, one GetWriteWatch over all of guest memory updates every written page's generation;
 * range queries in between only read page_gen (no syscall per range). */
static long ww_scan_frame = -1;
double rsx_ww_scan_s;
static void ww_scan_all_locked(void);
static void ww_scan_all(void) { AcquireSRWLockExclusive(&ww_lock); ww_scan_all_locked(); ReleaseSRWLockExclusive(&ww_lock); }
/* The scan for a frame starts on its own thread at the flip (rsx_ww_kick), overlapping the present and the next
 * frame's first commands; the first range query of the frame waits for it. MNR_WW_SYNC=1: scan on this thread. */
static volatile long ww_async_frame = -1, ww_done_frame = -1;
static HANDLE ww_ev_start, ww_ev_done;
static DWORD WINAPI ww_thread(void *p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(ww_ev_start, INFINITE);
        long f = ww_async_frame;
        ww_scan_all();
        ww_done_frame = f; SetEvent(ww_ev_done);
    }
    return 0;
}
void rsx_ww_kick(long frame) {
    static int sync = -1; if (sync < 0) sync = getenv("MNR_WW_SYNC") != NULL;
    if (sync) return;
    if (!ww_ev_start) { ww_ev_start = CreateEventA(NULL, FALSE, FALSE, NULL); ww_ev_done = CreateEventA(NULL, FALSE, FALSE, NULL); CreateThread(NULL, 1 << 16, ww_thread, NULL, 0, NULL); }
    ww_async_frame = frame; SetEvent(ww_ev_start);
}
/* Full scans, numbered, with the generation each gave its pages: a page write detected with a generation above that of
 * the first full scan started after some moment happened after that moment (the scan reset every GPU-visible page).
 * The renderer orders a CPU write against a surface's last draw with it (rsx_ww_gen_after). */
static volatile long ww_scans, ww_scans_done;
static uint32_t ww_scan_gen[4096];
long rsx_ww_scans(void) { return ww_scans; }
uint64_t rsx_ww_gen_after(long k) {   /* the generation of full scan k+1; UINT64_MAX while it hasn't finished (or is forgotten) */
    long n = k + 1;
    if (ww_scans_done < n || ww_scans_done - n >= 4096) return UINT64_MAX;
    return ww_scan_gen[n & 4095];
}
static void ww_scan_all_locked(void) {
    if (!page_gen) page_gen = calloc(1u << 20, 4);
    if (!block_gen) block_gen = calloc(1u << 14, 4);
    static void **hits; const ULONG_PTR cap = 1u << 18;
    if (!hits) hits = malloc(cap * sizeof *hits);
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    long scan = ++ww_scans;
    uint32_t g = (uint32_t)++gen_clock;
    /* only memory the GPU can read: RSX local memory and the main-memory ranges mapped for it (MNR_WW_FULL=1: all
     * committed memory). Asking about the whole 4 GB reservation every frame cost ~5 ms. */
    struct { uint32_t lo, hi; } regions[65];
    int nr = 0;
    {   uint32_t e[64], sz[64]; int n = getenv("MNR_WW_FULL") ? 0 : gcm_io_regions(e, sz, 64);
        regions[nr].lo = 0xC0000000u; regions[nr++].hi = 0xD0000000u;   /* RSX local memory */
        if (n > 0) for (int i = 0; i < n; i++) { regions[nr].lo = e[i]; regions[nr++].hi = e[i] + sz[i]; }   /* main memory the RSX maps */
        else { regions[nr].lo = 0x00000000u; regions[nr++].hi = 0x20000000u; regions[nr].lo = 0x30000000u; regions[nr++].hi = 0x40000000u; }
    }
    for (int r = 0; r < nr; r++) { ww_regions[r][0] = regions[r].lo; ww_regions[r][1] = regions[r].hi; }
    ww_nregions = nr;
    for (int r = 0; r < nr; r++)
        for (int pass = 0; pass < 64; pass++) {
            ULONG_PTR count = cap; ULONG gran;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, guest_mem + regions[r].lo, regions[r].hi - regions[r].lo, hits, &count, &gran) != 0) break;
            for (ULONG_PTR i = 0; i < count; i++) { uint32_t pg = (uint32_t)(((uint8_t *)hits[i] - guest_mem) >> 12); page_gen[pg] = g; block_gen[pg >> 6] = g; }
            if (count < cap) break;   /* more dirty pages than the buffer holds: the reset ones are gone, ask again */
        }
    ww_scan_gen[scan & 4095] = g; ww_scans_done = scan;
    QueryPerformanceCounter(&t1);
    rsx_ww_scan_s += (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
}
static uint64_t range_gen_scanned(uint32_t addr, uint32_t size) {
    if (ww_scan_frame != rsx_flips) {
        if (ww_async_frame == rsx_flips) while (ww_done_frame < rsx_flips) WaitForSingleObject(ww_ev_done, 2);
        else ww_scan_all();
        ww_scan_frame = rsx_flips;
    }
    uint32_t p0 = addr >> 12, p1 = (uint32_t)(((uint64_t)addr + size + 4095) >> 12);
    uint64_t m = 0;
    uint32_t pg = p0;
    for (; pg < p1 && (pg & 63); pg++) if (page_gen[pg] > m) m = page_gen[pg];
    for (; pg + 64 <= p1; pg += 64) if (block_gen[pg >> 6] > m) m = block_gen[pg >> 6];
    for (; pg < p1; pg++) if (page_gen[pg] > m) m = page_gen[pg];
    return m;
}
/* Scans also happen when ~100 ms passed without a flip: loading screens keep drawing (the loading number into a font
 * texture) without the flips that drive the per-frame scan, and the old glyphs stayed ("smearing"). The per-range
 * cache is keyed on the scan, not the flip. */
static long ww_epoch;
static ULONGLONG ww_last_tick;
uint64_t rsx_range_gen(uint32_t addr, uint32_t size) {
    /* main memory (not RSX local): its pages are asked about directly. The per-frame scan missed writes there (the
     * loading screen's font texture at 0x37FBE880 kept old glyphs, thumbnails stayed stale) while a direct query of
     * the same pages saw them. Like the scanned answers, it is asked once per frame per range (per draw it was the
     * graphics thread's biggest cost: a GetWriteWatch system call for every use of such a texture). */
    ULONGLONG now = GetTickCount64();
    if (ww_scan_frame == rsx_flips && now - ww_last_tick >= 100) { ww_scan_all(); ww_epoch++; ww_last_tick = now; }
    else if (ww_scan_frame != rsx_flips) ww_last_tick = now;
    unsigned h = ((addr >> 6) ^ (size * 2654435761u)) & 8191;
    long key = rsx_flips * 4096 + (ww_epoch & 4095);
    if (rg_cache[h].addr == addr && rg_cache[h].size == size && rg_cache[h].frame == key) return rg_cache[h].gen;
    uint64_t g = addr < 0xC0000000u ? range_gen(addr, size) : range_gen_scanned(addr, size);
    rg_cache[h].addr = addr; rg_cache[h].size = size; rg_cache[h].gen = g; rg_cache[h].frame = rsx_flips * 4096 + (ww_epoch & 4095);
    return g;
}
void *rsx_rg_callers[16]; long rsx_rg_caller_n[16];   /* debug: who makes the direct checks */
__attribute__((noinline)) uint64_t rsx_range_gen_now(uint32_t addr, uint32_t size) {
    void *ra = __builtin_return_address(0);
    for (int i = 0; i < 16; i++) { if (rsx_rg_callers[i] == ra) { rsx_rg_caller_n[i]++; break; } if (!rsx_rg_callers[i]) { rsx_rg_callers[i] = ra; rsx_rg_caller_n[i] = 1; break; } }
    return range_gen(addr, size);
}
/* debug: the runs of pages in a range written since generation `since` */
void rsx_range_report(uint32_t addr, uint32_t size, uint64_t since) {
    uint32_t p0 = addr >> 12, p1 = (uint32_t)(((uint64_t)addr + size + 4095) >> 12), n = 0;
    for (uint32_t pg = p0; pg < p1; pg++) {
        if (page_gen[pg] <= since) continue;
        uint32_t e = pg; while (e + 1 < p1 && page_gen[e + 1] > since) e++;
        if (n++ < 6) printf("    written: 0x%08x..0x%08x (%u pages)\n", pg << 12, (e + 1) << 12, e - pg + 1);
        pg = e;
    }
    if (n > 6) printf("    ... %u runs\n", n);
}
int rsx_tex_info(int u, rsx_texinfo_t *o) {
    tex_t t; tex_setup(u, &t);
    if (!t.enabled || !t.addr) return 0;
    o->addr = t.addr; o->fmt = t.fmt; o->w = t.w; o->h = t.h; o->pitch = t.pitch; o->linear = t.linear; o->unnorm = t.unnorm;
    o->dims = t.cube ? 6 : t.dims; o->remap = t.remap; o->wrap_s = t.wrap_s; o->wrap_t = t.wrap_t; o->wrap_r = (REG(0x1A08 + 0x20 * u) >> 16) & 15;
    o->mag_linear = t.mag_linear; o->min_linear = t.min_linear;
    o->min_filter = (REG(0x1A14 + 0x20 * u) >> 16) & 0xFF;
    o->levels = rsx_tex_levels(u); o->bytes = rsx_tex_total_bytes(u);
    o->depth = (REG(0x1840 + 4 * u) >> 20) ? (REG(0x1840 + 4 * u) >> 20) : 1;
    if (o->dims == 3) { o->levels = 1; o->bytes = (t.pitch ? t.pitch : t.w * 4) * t.h * o->depth; }
    o->is_float = t.fmt == 0x9A || t.fmt == 0x9B || t.fmt == 0x9C || t.fmt == 0x9F;
    return 1;
}
/* a tex_t describing mip level `level` (levels are packed after each other; linear pitched textures
 * only have level 0 here) */
static int tex_level(const tex_t *base, uint32_t level, tex_t *t) {
    *t = *base;
    for (uint32_t l = 0; l < level; l++) {
        if (t->w == 1 && t->h == 1) return 0;
        t->addr += tex_bytes(t);
        t->w = t->w > 1 ? t->w >> 1 : 1; t->h = t->h > 1 ? t->h >> 1 : 1;
        if (t->fmt >= 0x86 && t->fmt <= 0x88) t->pitch = 0;
    }
    return 1;
}
uint32_t rsx_tex_levels(int u) {
    tex_t t; tex_setup(u, &t);
    if (!t.enabled || t.linear && !(t.fmt >= 0x86 && t.fmt <= 0x88)) return 1;
    uint32_t n = t.levels ? t.levels : 1, maxl = 1, w = t.w, h = t.h;
    while (w > 1 || h > 1) { maxl++; w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1; }
    return n < maxl ? n : maxl;
}
uint32_t rsx_tex_total_bytes(int u) {   /* all levels, for change detection */
    tex_t t; tex_setup(u, &t);
    uint32_t n = rsx_tex_levels(u), sum = 0;
    for (uint32_t l = 0; l < n; l++) { tex_t lv; if (!tex_level(&t, l, &lv)) break; sum += tex_bytes(&lv); }
    return sum;
}
void rsx_tex_decode_level_off(int u, uint32_t level, uint32_t off, void *dst);
void rsx_tex_decode_level(int u, uint32_t level, void *dst) { rsx_tex_decode_level_off(u, level, 0, dst); }
void rsx_tex_decode_level_off(int u, uint32_t level, uint32_t off, void *dst) {   /* raw A,R,G,B of one level; off: byte offset of the image (cube faces) */
    tex_t base; tex_setup(u, &base); base.addr += off;
    tex_t t; if (!tex_level(&base, level, &t)) return;
    if (t.linear && (t.fmt == 0x81 || t.fmt == 0x85)) {   /* fast paths (movie frames are uploaded every frame): B8 -> broadcast, A8R8G8B8 -> as is */
        uint32_t bpp = t.fmt == 0x81 ? 1 : 4, pitch = t.pitch ? t.pitch : t.w * bpp;
        for (uint32_t y = 0; y < t.h; y++) {
            const uint8_t *src = gp(t.addr + y * pitch); uint8_t *d = (uint8_t *)dst + 4 * (size_t)y * t.w;
            if (bpp == 4) memcpy(d, src, 4 * (size_t)t.w);
            else for (uint32_t x = 0; x < t.w; x++) { uint8_t v = src[x]; d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = d[4 * x + 3] = v; }
        }
        return;
    }
    int fl = t.fmt == 0x9A || t.fmt == 0x9B || t.fmt == 0x9C || t.fmt == 0x9F;
    uint8_t *d8 = dst;
    static int verify = -1; if (verify < 0) verify = getenv("MNR_TEX_VERIFY") != NULL;   /* compare with the per-texel decode */
    if (verify) {
        static __thread int inner; size_t n = (size_t)t.w * t.h * (fl ? 16 : 4);
        if (!inner) {
            inner = 1; rsx_tex_decode_level_off(u, level, off, dst); inner = 0;
            uint8_t *ref = malloc(n);
            for (uint32_t y = 0; ref && y < t.h; y++)
                for (uint32_t x = 0; x < t.w; x++) {
                    float c[4]; texel_raw(&t, (int)x, (int)y, c);
                    if (fl) memcpy((float *)ref + 4 * ((size_t)y * t.w + x), c, 16);
                    else for (int k = 0; k < 4; k++) ref[4 * ((size_t)y * t.w + x) + k] = (uint8_t)(clampf(c[k], 0, 1) * 255 + .5f);
                }
            static volatile long checked, bad;
            if (ref && memcmp(ref, dst, n)) { if (InterlockedIncrement(&bad) <= 20) printf("[texverify] MISMATCH fmt 0x%02x %ux%u linear %u\n", t.fmt, t.w, t.h, t.linear); }
            if (InterlockedIncrement(&checked) % 500 == 0) printf("[texverify] %ld textures checked, %ld mismatched\n", checked, bad);
            free(ref); return;
        }
    }
#define TEX_PUT(i, c) do { if (fl) memcpy((float *)dst + 4 * (i), (c), 16); \
                           else for (int k_ = 0; k_ < 4; k_++) d8[4 * (i) + k_] = (uint8_t)(clampf((c)[k_], 0, 1) * 255 + .5f); } while (0)
    if (t.fmt >= 0x86 && t.fmt <= 0x88) {   /* a block at a time */
        for (uint32_t by = 0; by < (t.h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (t.w + 3) / 4; bx++) {
                float blk[16][4]; dxt_block(&t, bx, by, blk);
                for (uint32_t ti = 0; ti < 16; ti++) {
                    uint32_t x = bx * 4 + (ti & 3), y = by * 4 + (ti >> 2);
                    if (x < t.w && y < t.h) TEX_PUT((size_t)y * t.w + x, blk[ti]);
                }
            }
        return;
    }
    /* texel offsets: rows and columns separately (the swizzle interleaves x and y bits into disjoint positions) */
    uint32_t bpp = texel_bpp(t.fmt), *xo = malloc(4 * ((size_t)t.w + t.h)), *yo = xo + t.w;
    if (!xo) return;
    if (t.linear) {
        uint32_t pitch = t.pitch ? t.pitch : t.w * bpp;
        for (uint32_t x = 0; x < t.w; x++) xo[x] = x * bpp;
        for (uint32_t y = 0; y < t.h; y++) yo[y] = y * pitch;
    } else {
        uint32_t lw = ilog2(t.w), lh = ilog2(t.h);
        for (uint32_t x = 0; x < t.w; x++) xo[x] = swz_index(x, 0, lw, lh) * bpp;
        for (uint32_t y = 0; y < t.h; y++) yo[y] = swz_index(0, y, lw, lh) * bpp;
    }
    if (t.fmt == 0x85 || t.fmt == 0x9E) {   /* A8R8G8B8 (0x9E: alpha one): the bytes as they are */
        for (uint32_t y = 0; y < t.h; y++) {
            uint8_t *d = d8 + 4 * (size_t)y * t.w;
            for (uint32_t x = 0; x < t.w; x++, d += 4) {
                memcpy(d, gp(t.addr + yo[y] + xo[x]), 4);
                if (t.fmt == 0x9E) d[0] = 0xFF;
            }
        }
    } else if (t.fmt == 0x81) {
        for (uint32_t y = 0; y < t.h; y++) {
            uint8_t *d = d8 + 4 * (size_t)y * t.w;
            for (uint32_t x = 0; x < t.w; x++, d += 4) { uint8_t v = *gp(t.addr + yo[y] + xo[x]); d[0] = d[1] = d[2] = d[3] = v; }
        }
    } else {
        for (uint32_t y = 0; y < t.h; y++)
            for (uint32_t x = 0; x < t.w; x++) { float c[4]; texel_at(t.fmt, t.addr + yo[y] + xo[x], c); TEX_PUT((size_t)y * t.w + x, c); }
    }
#undef TEX_PUT
    free(xo);
}
void rsx_tex_decode_3d(int u, uint32_t depth, void *dst) {   /* linear 3D texture: slices stacked, raw A,R,G,B bytes */
    tex_t t; tex_setup(u, &t);
    t.h *= depth; t.linear = 1;
    for (uint32_t y = 0; y < t.h; y++)
        for (uint32_t x = 0; x < t.w; x++) {
            float c[4]; texel_raw(&t, (int)x, (int)y, c);
            for (int k = 0; k < 4; k++) ((uint8_t *)dst)[4 * ((size_t)y * t.w + x) + k] = (uint8_t)(clampf(c[k], 0, 1) * 255 + .5f);
        }
}
void rsx_tex_decode(int u, void *dst) { rsx_tex_decode_level_off(u, 0, 0, dst); }   /* level 0, raw A,R,G,B: bytes or floats */
/* the byte span [lo, hi) covering every page of the range written after generation <since>; 0 if none */
/* Marks in `out` (one byte per page of the range) the pages written after generation `since`, counting writes not yet
 * picked up; returns how many. */
int rsx_range_pages_after(uint32_t addr, uint32_t size, uint64_t since, uint8_t *out) {
    range_gen(addr, size);
    uint32_t p0 = addr >> 12, p1 = (uint32_t)(((uint64_t)addr + size + 4095) >> 12); int n = 0;
    for (uint32_t pg = p0; pg < p1; pg++) { out[pg - p0] = page_gen[pg] > since; n += out[pg - p0]; }
    return n;
}
int rsx_range_dirty_span(uint32_t addr, uint32_t size, uint64_t since, uint32_t *lo, uint32_t *hi) {
    uint32_t p0 = addr >> 12, p1 = (uint32_t)(((uint64_t)addr + size + 4095) >> 12), a = 0, b = 0; int any = 0;
    for (uint32_t pg = p0; pg < p1; pg++) if (page_gen[pg] > since) { if (!any) a = pg; b = pg; any = 1; }
    if (!any) return 0;
    *lo = a << 12; *hi = (b + 1) << 12;
    return 1;
}
