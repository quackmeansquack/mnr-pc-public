/* Runtime for code produced by ppu2c.py: Cell PPU guest state, big-endian guest memory,
 * condition-register helpers, dispatch and HLE hooks. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include "build_opts.h"   /* written by build.py (MNR_TRACE with --trace) */

typedef union { double d; uint64_t u; } fpr_t;

typedef struct ctx {
    uint64_t r[32];
    fpr_t f[32];
    uint32_t cr;
    uint64_t lr, ctr;
    uint8_t ca, so, ov;
    uint32_t fpscr, vrsave;
    uint8_t v[32][16];   /* AltiVec registers, kept in guest (big-endian) byte order */
    uint32_t res_addr;   /* lwarx/ldarx reservation: address and value seen */
    uint64_t res_val;
    uint64_t thread_id;
} ctx_t;

typedef struct { uint32_t addr; void (*fn)(ctx_t *); } fn_entry_t;

extern uint8_t *guest_mem;   /* base of the 4 GB guest address space */

#define R(n) (c->r[n])
#define F(n) (c->f[n].d)
#define FU(n) (c->f[n].u)
#define XER_CA (c->ca)
#define XER_SO (c->so)
#define CRBIT(i) ((c->cr >> (31 - (i))) & 1u)
#define SET_CRBIT(i, v) (c->cr = (c->cr & ~(1u << (31 - (i)))) | ((uint32_t)(v) << (31 - (i))))
#define JUMP_CTR() goto jump_ctr

static inline uint64_t XER_GET(ctx_t *c) { return ((uint64_t)c->so << 31) | ((uint64_t)c->ov << 30) | ((uint64_t)c->ca << 29); }
static inline void XER_SET(ctx_t *c, uint64_t v) { c->so = (v >> 31) & 1; c->ov = (v >> 30) & 1; c->ca = (v >> 29) & 1; }

/* --- guest memory (big-endian) */
static inline uint8_t *gp(uint32_t a) { return guest_mem + a; }
static inline uint8_t  ld_u8(uint32_t a)  { return *gp(a); }
static inline uint16_t ld_u16(uint32_t a) { uint16_t v; memcpy(&v, gp(a), 2); return __builtin_bswap16(v); }
static inline int16_t  ld_s16(uint32_t a) { return (int16_t)ld_u16(a); }
static inline uint32_t ld_u32(uint32_t a) { uint32_t v; memcpy(&v, gp(a), 4); return __builtin_bswap32(v); }
static inline int64_t  ld_s32(uint32_t a) { return (int32_t)ld_u32(a); }
static inline uint64_t ld_u64(uint32_t a) { uint64_t v; memcpy(&v, gp(a), 8); return __builtin_bswap64(v); }
static inline uint32_t ld_u32le(uint32_t a) { uint32_t v; memcpy(&v, gp(a), 4); return v; }
static inline uint16_t ld_u16le(uint32_t a) { uint16_t v; memcpy(&v, gp(a), 2); return v; }
static inline float    ld_f32(uint32_t a) { uint32_t v = ld_u32(a); float f; memcpy(&f, &v, 4); return f; }
static inline void st_u8(uint32_t a, uint64_t v)  { *gp(a) = (uint8_t)v; }
static inline void st_u16(uint32_t a, uint64_t v) { uint16_t x = __builtin_bswap16((uint16_t)v); memcpy(gp(a), &x, 2); }
static inline void st_u32(uint32_t a, uint64_t v) { uint32_t x = __builtin_bswap32((uint32_t)v); memcpy(gp(a), &x, 4); }
static inline void st_u64(uint32_t a, uint64_t v) { uint64_t x = __builtin_bswap64(v); memcpy(gp(a), &x, 8); }
static inline void st_u32le(uint32_t a, uint64_t v) { uint32_t x = (uint32_t)v; memcpy(gp(a), &x, 4); }
static inline void st_u16le(uint32_t a, uint64_t v) { uint16_t x = (uint16_t)v; memcpy(gp(a), &x, 2); }
static inline void st_f32(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); st_u32(a, v); }
static inline void mem_zero(uint32_t a, uint32_t n) { memset(gp(a), 0, n); }

/* --- load-reserve / store-conditional as compare-and-swap against the value seen by the load */
static inline uint64_t lwarx(struct ctx *c, uint32_t a) { c->res_addr = a; c->res_val = ld_u32(a); return c->res_val; }
static inline uint64_t ldarx(struct ctx *c, uint32_t a) { c->res_addr = a; c->res_val = ld_u64(a); return c->res_val; }
static inline int stwcx(struct ctx *c, uint32_t a, uint32_t v) {
    if (c->res_addr != a) return 0;
    c->res_addr = 1;   /* reservation consumed */
    uint32_t expect = __builtin_bswap32((uint32_t)c->res_val), want = __builtin_bswap32(v);
    return __atomic_compare_exchange_n((uint32_t *)gp(a), &expect, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline int stdcx(struct ctx *c, uint32_t a, uint64_t v) {
    if (c->res_addr != a) return 0;
    c->res_addr = 1;
    uint64_t expect = __builtin_bswap64(c->res_val), want = __builtin_bswap64(v);
    return __atomic_compare_exchange_n((uint64_t *)gp(a), &expect, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* --- condition register */
static inline void set_cr_field(ctx_t *c, int f, uint32_t v) {
    int sh = 28 - 4 * f;
    c->cr = (c->cr & ~(0xFu << sh)) | ((v & 0xF) << sh);
}
static inline void set_crf_s(ctx_t *c, int f, int64_t a, int64_t b) {
    set_cr_field(c, f, (a < b ? 8 : a > b ? 4 : 2) | c->so);
}
static inline void set_crf_u(ctx_t *c, int f, uint64_t a, uint64_t b) {
    set_cr_field(c, f, (a < b ? 8 : a > b ? 4 : 2) | c->so);
}
static inline void set_cr0(ctx_t *c, int64_t v) { set_crf_s(c, 0, v, 0); }
static inline void set_cr1(ctx_t *c) { set_cr_field(c, 1, 0); }
static inline void fcmp(ctx_t *c, int f, double a, double b) {
    set_cr_field(c, f, (a != a || b != b) ? 1 : a < b ? 8 : a > b ? 4 : 2);
}

/* --- integer helpers */
static inline uint64_t rotl64(uint64_t x, unsigned n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
static inline uint64_t rotl32d(uint64_t x, unsigned n) {  /* rotate the low word replicated into both halves */
    uint64_t lo = (uint32_t)x;
    return rotl64((lo << 32) | lo, n & 31);
}
static inline unsigned clz32(uint32_t x) { return x ? (unsigned)__builtin_clz(x) : 32; }
static inline unsigned clz64(uint64_t x) { return x ? (unsigned)__builtin_clzll(x) : 64; }
static inline int32_t  div_s32(int32_t a, int32_t b) { return (b == 0 || (a == INT32_MIN && b == -1)) ? 0 : a / b; }
static inline uint32_t div_u32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline int64_t  div_s64(int64_t a, int64_t b) { return (b == 0 || (a == INT64_MIN && b == -1)) ? 0 : a / b; }
static inline uint64_t div_u64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline void sraw(ctx_t *c, int ra, int rs, unsigned n) {
    int32_t v = (int32_t)c->r[rs];
    if (n & 32) { c->r[ra] = (uint64_t)(int64_t)(v >> 31); c->ca = v < 0; return; }
    n &= 31;
    int32_t r = v >> n;
    c->ca = v < 0 && n && ((uint32_t)v & ((1u << n) - 1));
    c->r[ra] = (uint64_t)(int64_t)r;
}
static inline void srad(ctx_t *c, int ra, int rs, unsigned n) {
    int64_t v = (int64_t)c->r[rs];
    if (n & 64) { c->r[ra] = (uint64_t)(v >> 63); c->ca = v < 0; return; }
    n &= 63;
    c->ca = v < 0 && n && ((uint64_t)v & ((1ULL << n) - 1));
    c->r[ra] = (uint64_t)(v >> n);
}
static inline int32_t cvt_d2i32(double d, int tr) {
    if (d != d) return INT32_MIN;
    double r = tr ? trunc(d) : nearbyint(d);
    return r >= 2147483647.0 ? INT32_MAX : r <= -2147483648.0 ? INT32_MIN : (int32_t)r;
}
static inline int64_t cvt_d2i64(double d, int tr) {
    if (d != d) return INT64_MIN;
    double r = tr ? trunc(d) : nearbyint(d);
    return r >= 9.2233720368547758e18 ? INT64_MAX : r <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)r;
}

/* --- function-entry trace, per host thread: ring of the last 32 (function, caller LR) pairs */
extern __thread uint64_t trace_ring[32];
extern __thread unsigned trace_pos;
/* Filled on every guest call, so it cost the call-heavy game code a thread-local write each time: only in builds
 * made with build.py --trace (crash dumps and the watchdog then show the last calls of each thread). */
#ifdef MNR_TRACE
#define FN_ENTER(a) (trace_ring[trace_pos++ & 31] = ((uint64_t)(a) << 32) | (uint32_t)c->lr)
#else
#define FN_ENTER(a) ((void)0)
#endif

/* --- provided by the host side (host.c) */
jmp_buf *host_jmpbuf(uint32_t guest_jmp_buf);   /* host setjmp buffer paired with a guest jmp_buf */
uint64_t host_timebase(void);
void dispatch(ctx_t *c, uint32_t target, uint32_t from);
void hle_call(ctx_t *c, const char *name, uint32_t stub);
void hle_syscall(ctx_t *c);
_Noreturn void unimpl(ctx_t *c, uint32_t addr, uint32_t insn);
void trap_always(ctx_t *c, uint32_t addr);

#include "vmx.h"
