/* SPU task runner: compiled programs (spu_progs.c) when available, the interpreter otherwise. */
#include "spu_core.h"
typedef struct { uint32_t elf; void (*fn)(spu_t *); uint32_t lo, hi; } spu_prog_t;   /* lo..hi: its instructions' LS addresses */
extern const spu_prog_t spu_progs[]; extern const int spu_nprogs;
#include <windows.h>
#include "rsx.h"
#include "spu.h"
double spu_seconds; uint64_t spu_steps;
int spu_run_task_inner(uint32_t elf, const uint32_t arg[4], uint64_t taskset_arg, uint64_t max_steps);
__thread int spu_vmode; __thread uint64_t spu_vhash; __thread uint32_t spu_vn, spu_vch[32];
/* Run a compiled program. MNR_SPU_VERIFY=1 (debug, slow): first interpret it on a copy of the state, with its
 * stores to guest memory held back, then run the compiled code for real, and report any difference in the
 * registers, local store, stores or events (the compiled code must do exactly what the interpreter does). */
static void prog_call(const spu_prog_t *p, spu_t *s) {
    static int verify = -1; if (verify < 0) { const char *e = getenv("MNR_SPU_VERIFY"); verify = e ? atoi(e) : 0; }
    if (!verify) { p->fn(s); return; }
    static __thread spu_t *c; if (!c) c = malloc(sizeof *c);
    memcpy(c, s, sizeof *s);
    spu_vmode = 1; spu_vhash = 0xCBF29CE484222325ull; spu_vn = 0; memset(spu_vch, 0, sizeof spu_vch);
    uint64_t nsteps = 0;
    for (uint64_t n = 0; !c->stopped && c->pc >= p->lo && c->pc < p->hi && n < 200000000; n++) { step(c); nsteps++; }
    uint64_t h1 = spu_vhash; uint32_t n1 = spu_vn, ch1[32]; memcpy(ch1, spu_vch, sizeof ch1);
    spu_vmode = 2; spu_vhash = 0xCBF29CE484222325ull; spu_vn = 0; memset(spu_vch, 0, sizeof spu_vch);
    if (verify == 2) { for (uint64_t n = 0; !s->stopped && s->pc >= p->lo && s->pc < p->hi && n < 200000000; n++) step(s); }   /* =2: both on the interpreter (checks the check) */
    else p->fn(s);
    spu_vmode = 0;
    static volatile long runs, bad;
    static struct { uint32_t elf; volatile long runs, kind[6]; } per[64]; static const char *kinds[6] = {"pc/stop", "stores/events", "registers", "local store", "reads", ""};
    int kind = s->pc != c->pc || s->stopped != c->stopped || s->stop_code != c->stop_code ? 0 : n1 != spu_vn || h1 != spu_vhash ? 1
             : memcmp(s->r, c->r, sizeof s->r) ? 2 : memcmp(s->ls, c->ls, LS_SIZE) ? 3 : -1;
    int pi = (int)(p - spu_progs) & 63; per[pi].elf = p->elf; InterlockedIncrement(&per[pi].runs);
    if (kind >= 0) { InterlockedIncrement(&per[pi].kind[kind]); InterlockedIncrement(&bad); }
    if (kind >= 0 && bad <= 40) {
        int nr = 0, fr = -1; for (int k = 0; k < 128; k++) if (memcmp(&s->r[k], &c->r[k], 16)) { nr++; if (fr < 0) fr = k; }
        int fl = -1, nl = 0; for (int k = 0; k < LS_SIZE; k += 16) if (memcmp(s->ls + k, c->ls + k, 16)) { nl++; if (fl < 0) fl = k; }
        printf("[spu verify] program 0x%08x: %s differ (compiled/interpreted: pc %05x/%05x, stores %u/%u; %d registers differ, first r%d; %d LS lines, first 0x%x; %llu instructions)\n",
               p->elf, kinds[kind], s->pc, c->pc, spu_vn, n1, nr, fr, nl, fl, (unsigned long long)nsteps);
        printf("[spu verify]   channel writes compiled/interpreted:"); for (int k = 0; k < 32; k++) if (spu_vch[k] || ch1[k]) printf(" ch%d %u/%u", k, spu_vch[k], ch1[k]); printf("\n");
    }
    if (InterlockedIncrement(&runs) % 50000 == 0) {
        printf("[spu verify] %ld runs checked, %ld differed:\n", runs, bad);
        for (int i = 0; i < 64; i++) if (per[i].runs)
            printf("[spu verify]   0x%08x: %ld runs; pc/stop %ld, stores %ld, registers %ld, local store %ld, reads %ld\n", per[i].elf, per[i].runs, per[i].kind[0], per[i].kind[1], per[i].kind[2], per[i].kind[3], per[i].kind[4]);
    }
}
int spu_run_task(uint32_t elf, const uint32_t arg[4], uint64_t taskset_arg, uint64_t max_steps) {
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int r = spu_run_task_inner(elf, arg, taskset_arg, max_steps);
    QueryPerformanceCounter(&t1);
    spu_seconds += (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    return r;
}
volatile long spu_dma_count[4];
__thread int spu_func_trace;
static __thread struct { long nodes, items, big, puts, gets; } cull_acc;   /* MNR_OCC_RING: CullBucket output counts (spu_func_trace 2) */
void spu_func_dma(spu_t *s, uint32_t op, uint32_t ea, uint32_t lsa, uint32_t size) {   /* MNR_SPUFUNC_DMA: each DMA, with its first words */
    const uint8_t *p = (op & 0xF0) == 0x20 ? s->ls + lsa : (const uint8_t *)gp(ea);
    if (spu_func_trace == 2) {
        if ((op & 0xF0) == 0x20) { cull_acc.puts++; if (size == 0x80) { cull_acc.nodes++; cull_acc.items += (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7]; } if (size == 0x2000) cull_acc.big++; }
        else cull_acc.gets++;
        return;
    }
    uint32_t h = 2166136261u; for (uint32_t i = 0; i < size; i++) h = (h ^ p[i]) * 16777619u;   /* FNV-1a of the whole transfer */
    printf("[spudma]   op 0x%02x ea 0x%08x ls 0x%05x size 0x%x hash %08x:", op, ea, lsa, size, h);
    for (uint32_t i = 0; i < size && i < (getenv("MNR_SPUFUNC_DMA_FROM") ? 16u : 128u); i += 4) printf(" %08x", (uint32_t)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3]);
    printf("\n");
}
/* MNR_OCC_RING=1 (debug): keep the occlusion map and the four per-SPU depth buffers of the last 6 CreateOcclusionMap
 * calls; spu_occ_dump (called on a dark frame) writes them as 128x128 PGMs (depth >> 8) to the working folder */
#define OCC_SLOTS 6
static struct { long flip; uint32_t frame, map; uint8_t m[0x10000], b[4][0x10000], fd[0x60]; } *occ_ring;
static volatile long occ_pos; static SRWLOCK occ_lock = SRWLOCK_INIT;
static void spu_occ_note(const uint32_t regs[6]) {
    static int on = -1; if (on < 0) on = getenv("MNR_OCC_RING") != NULL;
    if (!on) return;
    AcquireSRWLockExclusive(&occ_lock);
    if (!occ_ring) occ_ring = calloc(OCC_SLOTS, sizeof *occ_ring);
    long k = occ_pos++ % OCC_SLOTS;
    occ_ring[k].flip = rsx_flips; occ_ring[k].frame = regs[2]; occ_ring[k].map = regs[3] - 0x10100;
    memcpy(occ_ring[k].m, gp(regs[3] - 0x10100), 0x10000);
    for (int i = 0; i < 4; i++) memcpy(occ_ring[k].b[i], gp(0x33981d00u + i * 0x10180u), 0x10000);
    memcpy(occ_ring[k].fd, gp(regs[2]), 0x60);
    ReleaseSRWLockExclusive(&occ_lock);
}
static void occ_pgm(const char *name, const uint8_t *p) {   /* 16384 big-endian u32 words, depth in the low 16 bits */
    FILE *f = fopen(name, "wb"); if (!f) return;
    fprintf(f, "P5 128 128 65535\n");   /* 16-bit PGM (big-endian): the depth as is */
    for (int i = 0; i < 16384; i++) { fputc(p[4 * i + 2], f); fputc(p[4 * i + 3], f); }
    fclose(f);
}
void spu_cull_dump(long flip);
void spu_occ_dump(long flip) {
    spu_cull_dump(flip);
    if (!occ_ring) return;
    AcquireSRWLockExclusive(&occ_lock);
    for (long j = occ_pos - OCC_SLOTS; j < occ_pos; j++) {
        if (j < 0) continue;
        long k = j % OCC_SLOTS; char n[96];
        snprintf(n, sizeof n, "occ_%ld_%ld_map.pgm", flip, occ_ring[k].flip); occ_pgm(n, occ_ring[k].m);
        for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "occ_%ld_%ld_spu%d.pgm", flip, occ_ring[k].flip, i); occ_pgm(n, occ_ring[k].b[i]); }
        printf("[occ] dark %ld: slot flip %ld frame %08x map %08x fd", flip, occ_ring[k].flip, occ_ring[k].frame, occ_ring[k].map);
        for (int w = 0; w < 24; w++) { const uint8_t *q = occ_ring[k].fd + 4 * w; printf(" %08x", (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]); }
        printf("\n");
    }
    ReleaseSRWLockExclusive(&occ_lock);
}
#define CULL_SLOTS 96
static struct { long flip, nodes, items, big, puts, gets; uint32_t r2, r3, r4; } cull_ring[CULL_SLOTS];
static volatile long cull_pos;
static void cull_note(const uint32_t regs[6]) {
    long k = InterlockedIncrement(&cull_pos) - 1;
    k %= CULL_SLOTS;
    cull_ring[k].flip = rsx_flips; cull_ring[k].nodes = cull_acc.nodes; cull_ring[k].items = cull_acc.items; cull_ring[k].big = cull_acc.big;
    cull_ring[k].puts = cull_acc.puts; cull_ring[k].gets = cull_acc.gets; cull_ring[k].r2 = regs[2]; cull_ring[k].r3 = regs[3]; cull_ring[k].r4 = regs[4];
}
/* per flip, calls of each SPU function (MNR_OCC_RING): printed for the flips before a dark frame */
#define FC_FLIPS 8
static struct { long flip; uint32_t entry[24]; long n[24]; } fc_ring[FC_FLIPS];
static SRWLOCK fc_lock = SRWLOCK_INIT;
static void fc_note(uint32_t entry) {
    AcquireSRWLockExclusive(&fc_lock);
    long f = rsx_flips; int k = (int)(f % FC_FLIPS);
    if (fc_ring[k].flip != f) { memset(&fc_ring[k], 0, sizeof fc_ring[k]); fc_ring[k].flip = f; }
    for (int i = 0; i < 24; i++) { if (fc_ring[k].entry[i] == entry) { fc_ring[k].n[i]++; break; } if (!fc_ring[k].entry[i]) { fc_ring[k].entry[i] = entry; fc_ring[k].n[i] = 1; break; } }
    ReleaseSRWLockExclusive(&fc_lock);
}
static void fc_dump(long flip) {
    AcquireSRWLockExclusive(&fc_lock);
    for (long f = flip - FC_FLIPS + 1; f <= flip; f++) {
        int k = (int)(f % FC_FLIPS); if (fc_ring[k].flip != f) continue;
        printf("[fcount] dark %ld: flip %ld", flip, f);
        for (int i = 0; i < 24 && fc_ring[k].entry[i]; i++) printf(" %x:%ld", fc_ring[k].entry[i], fc_ring[k].n[i]);
        printf("\n");
    }
    ReleaseSRWLockExclusive(&fc_lock);
}
void spu_cull_dump(long flip) {
    fc_dump(flip);
    for (long j = cull_pos - CULL_SLOTS; j < cull_pos; j++) {
        if (j < 0) continue;
        long k = j % CULL_SLOTS;
        printf("[cull] dark %ld: flip %ld frame %08x bucket %08x out %08x: %ld nodes %ld items %ld big, %ld puts %ld gets\n", flip, cull_ring[k].flip,
               cull_ring[k].r2, cull_ring[k].r3, cull_ring[k].r4, cull_ring[k].nodes, cull_ring[k].items, cull_ring[k].big, cull_ring[k].puts, cull_ring[k].gets);
    }
}
static SRWLOCK spu_atomic_srw = SRWLOCK_INIT;
__thread uint32_t spu_guard_lo, spu_guard_hi;
void spu_guard_report(spu_t *s, uint32_t op, uint32_t ea, uint32_t size) {
    static volatile long n;
    if (InterlockedIncrement(&n) <= 30) printf("[spu] guarded program at pc %05x: DMA store op 0x%02x to 0x%08x (0x%x bytes) outside its memory, dropped\n", s->pc, op, ea, size);
}
void spu_atomic_lock(int on) { if (on) AcquireSRWLockExclusive(&spu_atomic_srw); else ReleaseSRWLockExclusive(&spu_atomic_srw); }

static int spu_load_elf(spu_t *s, uint32_t elf) {
    memset(s->ls, 0, LS_SIZE);
    memset(s->r, 0, sizeof s->r);
    s->stopped = 0; s->steps = 0; s->tag_mask = 0; s->stop_code = 0; s->rsv_ea = 0xFFFFFFFF; s->atomic_stat = 0;
    if (ld_u32(elf) != 0x7F454C46) return -1;
    uint32_t phoff = ld_u32(elf + 0x1C), phnum = ld_u16(elf + 0x2C);
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t ph = elf + phoff + 32 * i;
        if (ld_u32(ph) != 1) continue;
        uint32_t off = ld_u32(ph + 4), va = ld_u32(ph + 8), fsz = ld_u32(ph + 16), msz = ld_u32(ph + 20);
        if (va + msz > LS_SIZE) return -2;
        memcpy(s->ls + va, gp(elf + off), fsz);
        memset(s->ls + va + fsz, 0, msz - fsz);
    }
    return 0;
}
/* interpreted SPU instructions per program: reported every 200M (the hot ones belong in spu2c.py's AOT list) */
static void spu_interp_note(uint32_t elf, uint64_t steps) {
    static struct { uint32_t elf; volatile long long steps, reported; } t[64]; static volatile long n;
    int i = 0; while (i < n && t[i].elf != elf) i++;
    if (i == n) { if (n >= 64) return; i = (int)InterlockedIncrement(&n) - 1; if (i >= 64) return; t[i].elf = elf; }
    long long v = InterlockedAdd64(&t[i].steps, (long long)steps);
    if (v - t[i].reported >= 200000000) { t[i].reported = v; printf("[spu] interpreted program 0x%08x: %lld M instructions so far\n", elf, v / 1000000); }
}
static int spu_run_from(spu_t *s, uint32_t elf, uint32_t pc, uint64_t max_steps) {   /* until it returns to 0 or stops */
    s->r[0].w[0] = 0; s->pc = pc; s->stopped = 0;
    static int noaot = -1;   /* MNR_SPU_NOAOT=1: interpret even compiled programs (the step limit then catches hangs) */
    if (noaot < 0) { const char *e = getenv("MNR_SPU_NOAOT"); noaot = e && *e == '1'; }
    for (int i = 0; i < spu_nprogs && !noaot; i++)
        if (spu_progs[i].elf == elf) {
            prog_call(&spu_progs[i], s);
            if (!s->stopped && s->pc == 0) s->stopped = 1;
            return s->stopped == 1 ? 0 : -10 - s->stopped;
        }
    uint64_t start = s->steps;
    while (!s->stopped && s->steps - start < max_steps) {
        if (s->pc == 0) { s->stopped = 1; break; }
        step(s); s->steps++;
    }
    spu_steps += s->steps - start; spu_interp_note(elf, s->steps - start);
    return s->stopped == 1 && s->pc == 0 ? 0 : s->stopped ? -10 - s->stopped : -9;
}
/* One function of an SPU ELF, the way a qTask's SPU side calls it: fresh LS image, static constructors
 * (.init at the ELF's 0x3010), then entry(r3..r8 = regs[0..5]) on a stack at the top of LS. `scratch`
 * (an LS address) is zeroed first: the caller passes it in a register for the function's results.
 * Each host thread acts as its own SPU: the SPURS kernel context's SPU number (LS 0x1C8, which SPU code
 * reads with lqa 0x1C0) is distinct per thread, and regs[0] (the qTask ctx argument, also the SPU
 * number) is replaced by it, so per-SPU buffers in main memory are not shared between threads. */
int spu_run_func(uint32_t elf, uint32_t init, uint32_t entry, const uint32_t regs[6], uint32_t scratch, uint64_t max_steps) {
    static __thread spu_t *s;
    static __thread int spu_num = -1;
    static volatile long next_num;
    if (!s) s = malloc(sizeof *s);
    if (spu_num < 0) { long k = InterlockedIncrement(&next_num) - 1; spu_num = (int)(k % 6); if (getenv("MNR_SPUNUM_LOG")) printf("[spunum] host thread %lu gets SPU number %d (caller #%ld)\n", GetCurrentThreadId(), spu_num, k); }
    static int serial = -1; static SRWLOCK serial_lock = SRWLOCK_INIT;   /* MNR_SPU_SERIAL=1: one SPU function at a time (debug) */
    if (serial < 0) serial = getenv("MNR_SPU_SERIAL") && *getenv("MNR_SPU_SERIAL") == '1';
    if (serial) AcquireSRWLockExclusive(&serial_lock);
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    {   /* MNR_SPU_DELAY_US=<us>[,<entry hex>]: sleep before the function (default entry 0x5a10, CullBucket) - timing test */
        static long us = -1, only; if (us < 0) { const char *e = getenv("MNR_SPU_DELAY_US"); us = e ? atol(e) : 0; only = e && strchr(e, ',') ? (long)strtoul(strchr(e, ',') + 1, NULL, 16) : 0x5a10; }
        if (us && (!only || entry == (uint32_t)only)) { LARGE_INTEGER a, b, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a); do { SwitchToThread(); QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000000 / fq.QuadPart < us); }
    }
    int r = spu_load_elf(s, elf);
    wr32(s, 0x1C8, (uint32_t)spu_num); wr32(s, 0x1CC, 31 - spu_num);   /* SpursKernelContext.spuNum, dmaTagId */
    if (!r && init) { s->r[1].w[0] = 0x3FE00; r = spu_run_from(s, elf, init, max_steps); }
    if (!r) {
        memset(s->r, 0, sizeof s->r);
        if (scratch) memset(s->ls + scratch, 0, 256);
        for (int i = 0; i < 6; i++) s->r[3 + i].w[0] = regs[i];
        s->r[3].w[0] = (uint32_t)spu_num;
        s->r[1].w[0] = 0x3FE00;   /* stack below the scratch area the caller passes (0x3FF00) */
        { static int tr = -1; static volatile long ntr; if (tr < 0) { const char *e = getenv("MNR_SPUFUNC_DMA"); tr = e ? atoi(e) : 0; }
          static long only = -1; if (only < 0) { const char *e = getenv("MNR_SPUFUNC_DMA_ENTRY"); only = e ? (long)strtoul(e, NULL, 16) : 0; }
          spu_func_trace = tr && (!only || (uint32_t)only == entry) && ntr++ < tr;
          static long from = -2; if (from == -2) { const char *e = getenv("MNR_SPUFUNC_DMA_FROM"); from = e ? atol(e) : -1; }   /* every call of the occlusion functions from flip <n> on */
          if (from >= 0 && rsx_flips >= from && (entry == 0x5a10 || entry == 0x7fc0 || entry == 0x74f8)) spu_func_trace = 1;
          if (spu_func_trace) printf("[spucall] flip %ld thread %lu entry 0x%x regs %08x %08x %08x %08x %08x %08x\n", (long)rsx_flips, GetCurrentThreadId(), entry, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]); }
        { static int on = -1; if (on < 0) on = getenv("MNR_OCC_RING") != NULL;
          if (on && !spu_func_trace && entry == 0x5a10) { spu_func_trace = 2; memset(&cull_acc, 0, sizeof cull_acc); } }
        long d0 = spu_dma_count[0], d1 = spu_dma_count[1], d2 = spu_dma_count[2], d3 = spu_dma_count[3];
        r = spu_run_from(s, elf, entry, max_steps);
        if (entry == 0x7fc0) spu_occ_note(regs);
        if (spu_func_trace == 2) cull_note(regs);
        { static int on = -1; if (on < 0) on = getenv("MNR_OCC_RING") != NULL; if (on) fc_note(entry); }
        static int lg = -1;
        if (lg < 0) { const char *e = getenv("MNR_SPUFUNC_LOG"); lg = e && *e == '1'; }
        static struct { uint32_t elf, entry; volatile long n; } per[64];   /* first 8 calls of each function */
        long nth = 0;
        for (int i = 0; i < 64; i++) {
            if (per[i].elf == elf && per[i].entry == entry) { nth = InterlockedIncrement(&per[i].n); break; }
            if (!per[i].elf) { per[i].elf = elf; per[i].entry = entry; nth = InterlockedIncrement(&per[i].n); break; }
        }
        if (lg && nth <= 8)
            printf("[spufunc] 0x%x(%08x %08x %08x %08x %08x %08x) -> %d, %llu steps, pc 0x%x, dma get %ld put %ld list %ld atomic %ld\n", entry,
                   regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], r, (unsigned long long)s->steps, s->pc,
                   spu_dma_count[0] - d0, spu_dma_count[1] - d1, spu_dma_count[2] - d2, spu_dma_count[3] - d3);
        spu_func_trace = 0;
        if (r) {   /* halt / stop / step limit: where, with the registers the condition usually tests */
            static volatile long nfail;
            if (InterlockedIncrement(&nfail) <= 8)
                printf("[spufunc] 0x%x of ELF 0x%08x stopped at pc 0x%x (%d, stop code 0x%x): r2 %08x r3 %08x r4 %08x r5 %08x lr %08x sp %08x\n",
                       entry, elf, s->pc, r, s->stop_code, s->r[2].w[0], s->r[3].w[0], s->r[4].w[0], s->r[5].w[0], s->r[0].w[0], s->r[1].w[0]);
        }
    }
    QueryPerformanceCounter(&t1);
    spu_seconds += (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    {   /* MNR_SPUFUNC_TIME=1 (debug): time per SPU function, every 10 s */
        static int on = -1; if (on < 0) on = getenv("MNR_SPUFUNC_TIME") != NULL;
        static struct { uint32_t elf, entry; double s, mx; long n; } t[64]; static int nt; static double last; static SRWLOCK l = SRWLOCK_INIT;
        if (on) {
            double dt = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart, now = (double)t1.QuadPart / f.QuadPart;
            AcquireSRWLockExclusive(&l);
            int i = 0; while (i < nt && !(t[i].elf == elf && t[i].entry == entry)) i++;
            if (i == nt && nt < 64) { t[nt].elf = elf; t[nt].entry = entry; nt++; }
            if (i < 64) { t[i].s += dt; t[i].n++; if (dt > t[i].mx) t[i].mx = dt; }
            if (now - last > 10) { if (last) for (int k = 0; k < nt; k++) if (t[k].n) printf("[spufunctime] elf %08x entry 0x%x: %.2f s over %ld calls (max %.2f ms)\n", t[k].elf, t[k].entry, t[k].s, t[k].n, t[k].mx * 1000);
                for (int k = 0; k < nt; k++) { t[k].s = t[k].mx = 0; t[k].n = 0; } last = now; }
            ReleaseSRWLockExclusive(&l);
        }
    }
    if (serial) ReleaseSRWLockExclusive(&serial_lock);
    return r;
}

int spu_run_task_inner(uint32_t elf, const uint32_t arg[4], uint64_t taskset_arg, uint64_t max_steps) {
    static __thread spu_t *s;
    if (!s) s = malloc(sizeof *s);
    memset(s->ls, 0, LS_SIZE);
    memset(s->r, 0, sizeof s->r);
    s->stopped = 0; s->steps = 0; s->tag_mask = 0; s->stop_code = 0; s->rsv_ea = 0xFFFFFFFF; s->atomic_stat = 0;
    /* ELF32: program headers */
    if (ld_u32(elf) != 0x7F454C46) return -1;
    uint32_t entry = ld_u32(elf + 0x18), phoff = ld_u32(elf + 0x1C);
    uint32_t phnum = ld_u16(elf + 0x2C);
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t ph = elf + phoff + 32 * i;
        if (ld_u32(ph) != 1) continue;
        uint32_t off = ld_u32(ph + 4), va = ld_u32(ph + 8), fsz = ld_u32(ph + 16), msz = ld_u32(ph + 20);
        if (va + msz > LS_SIZE) return -2;
        memcpy(s->ls + va, gp(elf + off), fsz);
        memset(s->ls + va + fsz, 0, msz - fsz);
    }
    for (int i = 0; i < 4; i++) s->r[3].w[i] = arg[i];
    s->r[4].w[0] = (uint32_t)(taskset_arg >> 32); s->r[4].w[1] = (uint32_t)taskset_arg;
    s->r[1].w[0] = 0x3FFF0;   /* stack (crt0 sets its own as well) */
    s->r[0].w[0] = 0;          /* return to 0: stop */
    s->pc = entry;
    {   /* MNR_TASK_DMA=<elf hex>: print the DMA of that task's first 3 runs (debug) */
        static long want = -2; static volatile long runs; if (want == -2) { const char *e = getenv("MNR_TASK_DMA"); want = e ? (long)strtoul(e, NULL, 16) : -1; }
        spu_func_trace = want >= 0 && elf == (uint32_t)want && InterlockedIncrement(&runs) <= 3;
        if (spu_func_trace) printf("[spudma] task 0x%08x args %08x %08x %08x %08x\n", elf, arg[0], arg[1], arg[2], arg[3]);
    }
    for (int i = 0; i < spu_nprogs; i++)
        if (spu_progs[i].elf == elf) {
            prog_call(&spu_progs[i], s);
            if (!s->stopped && s->pc == 0) s->stopped = 1;
            return s->stopped == 1 ? 0 : -10 - s->stopped;
        }
    while (!s->stopped && s->steps < max_steps) {
        if (s->pc == 0) { s->stopped = 1; break; }   /* returned from main without exit */
        step(s); s->steps++;
    }
    spu_steps += s->steps; spu_interp_note(elf, s->steps);
    return s->stopped == 1 ? 0 : s->stopped ? -10 - s->stopped : -9;   /* -9: step limit */
}

/* SPURS job (job chain command JOB), "jobbin2" binaries: descriptor = CellSpursJobHeader {u64 eaBinary;
 * u16 sizeBinary (16 B units); u16 sizeDmaList (bytes); u32 eaHighInput, useInOutBuffer, sizeInOrInOut,
 * sizeOut; u16 sizeStack, sizeScratch (16 B units); u32 eaHighCache, sizeCacheDmaList} + work area: input
 * DMA list (u64 {u32 size, u32 eal} each), cache DMA list, user data.
 * The job manager's work, done here: load the binary, GET the input list into the I/O buffer (each
 * element at a 16-byte boundary plus the low 4 bits of its address, as GETL places them) and the cache
 * list into cache buffers, build the CellSpursJobContext2 {ioBuffer +0, cacheBuffer[4] +4, u16 numIoBuffer
 * +0x18, u16 numCacheBuffer +0x1A, oBuffer +0x1C, sBuffer +0x20, dmaTag +0x24, eaJobDescriptor +0x28}, and call the entry (binary + 0x10) with r3 = context,
 * r4 = descriptor in LS. The job PUTs its own output; the binaries are position independent. */
#define JOB_CTX 0x100
#define JOB_DESC 0x200
#define JOB_BIN 0x1000
static uint32_t job_get_list(spu_t *s, uint32_t lsa, uint32_t list_ea, uint32_t n, uint32_t *ptrs, int maxp) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sz = ld_u32(list_ea + 8 * i) & 0x7FFFF, ea = ld_u32(list_ea + 8 * i + 4);
        uint32_t dst = (lsa & ~15u) | (ea & 15);
        if (dst + sz > LS_SIZE) return 0;
        memcpy(s->ls + dst, gp(ea), sz);
        if ((int)i < maxp) ptrs[i] = dst;
        lsa = (dst + sz + 15) & ~15u;
    }
    return lsa;
}
uint64_t spu_jobs; double spu_job_seconds;
int spu_dma_log, spu_dma_log_n; static uint32_t cur_job_bin;
void spu_dma_trace(spu_t *s, uint32_t ea, uint32_t lsa, uint32_t size) {   /* PUT: who, where, and the float magnitude written */
    static long watch = -1;
    if (watch < 0) { const char *e = getenv("MNR_JOB_DMAWATCH"); watch = e ? (long)strtoul(e, NULL, 16) : 0; }
    if (watch) {   /* MNR_JOB_DMAWATCH=<hex address>: every PUT covering that word, with the value it writes */
        if ((uint32_t)watch >= ea && (uint32_t)watch < ea + size) {
            uint32_t o = lsa + ((uint32_t)watch - ea);
            printf("[jobdma] bin 0x%08x PUT 0x%08x+%u writes %08x %08x %08x %08x at 0x%08lx\n", cur_job_bin, ea, size, rd32(s, o), rd32(s, o + 4), rd32(s, o + 8), rd32(s, o + 12), watch);
        }
        return;
    }
    float mx = 0; uint32_t nan = 0;
    for (uint32_t k = 0; k + 4 <= size; k += 4) { float v; uint32_t u = rd32(s, lsa + k); memcpy(&v, &u, 4); if (v != v) nan++; else if (fabsf(v) > mx) mx = fabsf(v); }
    if (mx == 0 && !nan) return;
    spu_dma_log_n++;
        printf("[jobdma] bin 0x%08x PUT 0x%08x <- LS 0x%05x, %u bytes, max |float| %g%s\n", cur_job_bin, ea, lsa, size, mx, nan ? " (NaN)" : "");
}
int spu_run_job(uint32_t job, uint32_t desc_size) {
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    static __thread spu_t *s;
    if (!s) s = malloc(sizeof *s);
    memset(s->ls, 0, LS_SIZE);
    memset(s->r, 0, sizeof s->r);
    s->stopped = 0; s->steps = 0; s->tag_mask = 0; s->stop_code = 0; s->out_mbox = 0;
    uint32_t bin = (uint32_t)ld_u64(job) & ~0xFu, bsz = ld_u16(job + 8) * 16u, ndma = ld_u16(job + 10) / 8;
    cur_job_bin = bin;
    { static int want = -1; if (want < 0) want = getenv("MNR_JOB_DMALOG") || getenv("MNR_JOB_DMAWATCH"); if (want && !spu_dma_log) spu_dma_log = 1; }   /* (read once: this runs per job) */
    {   /* first sighting of each job binary (MNR_JOBCHAIN_LOG) */
        static uint32_t seen[64]; static int nseen;
        int k = 0; while (k < nseen && seen[k] != bin) k++;
        if (k == nseen && nseen < 64) { seen[nseen++] = bin; if (getenv("MNR_JOBCHAIN_LOG")) printf("[job] binary 0x%08x, %u bytes\n", bin, bsz); }
    }
    uint32_t inout = ld_u32(job + 16), insz = ld_u32(job + 20), outsz = ld_u32(job + 24);
    uint32_t stack = ld_u16(job + 28) * 16u, scratch = ld_u16(job + 30) * 16u, ncache = ld_u32(job + 36) / 8;
    if (!desc_size || desc_size > 0xE00) desc_size = 256;
    if (JOB_BIN + bsz > LS_SIZE) return -1;
    memcpy(s->ls + JOB_DESC, gp(job), desc_size);
    memcpy(s->ls + JOB_BIN, gp(bin), bsz);
    uint32_t p = (JOB_BIN + bsz + 127) & ~127u, io = p;
    uint32_t dummy[1];
    p = job_get_list(s, p, job + 48, ndma, dummy, 0);   /* input elements, in order */
    if (!p) return -2;
    if (p < io + insz) p = io + insz;
    p = (p + 127) & ~127u;
    uint32_t ob = inout ? io : p;
    if (!inout) p = (p + outsz + 127) & ~127u;
    uint32_t cache[4] = {0};
    if (ncache) { p = job_get_list(s, p, job + 48 + 8 * ndma, ncache, cache, 4); if (!p) return -3; p = (p + 127) & ~127u; }
    uint32_t sb = p; p = (p + scratch + 127) & ~127u;
    if (p + stack > LS_SIZE - 16) return -4;
    uint8_t *c = s->ls + JOB_CTX;
    #define W32(o, v) do { uint32_t v_ = (v); c[o] = (uint8_t)(v_ >> 24); c[(o) + 1] = (uint8_t)(v_ >> 16); c[(o) + 2] = (uint8_t)(v_ >> 8); c[(o) + 3] = (uint8_t)v_; } while (0)
    W32(0, io);
    for (int i = 0; i < 4; i++) W32(4 + 4 * i, cache[i]);
    c[0x18] = (uint8_t)(ndma >> 8); c[0x19] = (uint8_t)ndma;       /* numIoBuffer (u16): jobs mask input pointers beyond it */
    c[0x1A] = (uint8_t)(ncache >> 8); c[0x1B] = (uint8_t)ncache;   /* numCacheBuffer (u16) */
    W32(0x1C, ob); W32(0x20, sb); W32(0x24, 1); W32(0x28, 0); W32(0x2C, job);
    #undef W32
    s->r[3].w[0] = JOB_CTX; s->r[4].w[0] = JOB_DESC;
    s->r[1].w[0] = LS_SIZE - 16;
    s->r[0].w[0] = 0;
    s->pc = JOB_BIN + 0x10;
    for (int i = 0; i < spu_nprogs; i++)
        if (spu_progs[i].elf == bin) {   /* compiled ahead of time (spu2c.py): runs until the PC leaves the binary */
            prog_call(&spu_progs[i], s);
            if (!s->stopped && s->pc == 0) s->stopped = 1;
            break;
        }
    while (!s->stopped && s->steps < 200000000) {
        if (s->pc == 0) { s->stopped = 1; break; }
        step(s); s->steps++;
    }
    spu_steps += s->steps; spu_jobs++; if (s->steps) spu_interp_note(bin, s->steps);
    if (s->stopped != 1) {
        static int reported;
        if (reported++ < 3) {
            fprintf(stderr, "[job] 0x%08x (binary 0x%08x) halted at LS 0x%05x (binary +0x%x), state %d; io 0x%x out 0x%x\n", job, bin, s->pc, s->pc - JOB_BIN, s->stopped, io, ob);
            for (int k = 0; k < (int)desc_size; k += 16) fprintf(stderr, "  desc+%02x: %08x %08x %08x %08x\n", k, ld_u32(job + k), ld_u32(job + k + 4), ld_u32(job + k + 8), ld_u32(job + k + 12));
            for (int k = 0; k < 0x60; k += 16) fprintf(stderr, "  LS %05x: %08x %08x %08x %08x\n", io + k, rd32(s, io + k), rd32(s, io + k + 4), rd32(s, io + k + 8), rd32(s, io + k + 12));
            for (int r = 0; r < 48; r++) fprintf(stderr, "  r%-2d %08x %08x %08x %08x%s", r, s->r[r].w[0], s->r[r].w[1], s->r[r].w[2], s->r[r].w[3], r % 2 ? "\n" : "");
        }
    }
    QueryPerformanceCounter(&t1);
    spu_job_seconds += (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    return s->stopped == 1 ? 0 : s->stopped ? -10 - s->stopped : -9;
}
