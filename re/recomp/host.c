/* Host side for the recompiled EBOOT code: guest memory, ELF loading, dispatch, HLE, and a
 * test harness that runs the game's own Lua 5.1 VM natively.
 *
 * Guest layout: ELF segments at their PS3 addresses (0x10000..0x1170000), host-function
 * code addresses at HOST_CODE (+4 per function) with OPDs at HOST_OPD, a bump heap at HEAP_LO,
 * and the stack below STACK_TOP. 512 MB of the 4 GB reservation is committed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include "runtime.h"
#include "host.h"
#include "present.h"
#include "net.h"
#include "rsx.h"
#include "rsx_d3d.h"
#include "spu.h"

uint8_t *guest_mem;
__thread uint64_t trace_ring[32];
__thread unsigned trace_pos;
static __thread ctx_t *crash_ctx;   /* per host thread, for crash reports */
static SRWLOCK alloc_lock = SRWLOCK_INIT;   /* guards the guest bump allocators and the jmp_buf table */
static uint32_t tls_va, tls_filesz, tls_memsz;   /* PT_TLS template */
static char mnr_root[MAX_PATH];                   /* the MNR folder (set in main) */
const char *mnr_root_dir(void) { return mnr_root[0] ? mnr_root : "."; }
const char *mnr_settings_path(void) {               /* the PC build's own settings (volume, push to talk) */
    static char p[MAX_PATH + 32];
    if (!p[0]) snprintf(p, sizeof p, "%s\\pc_settings.ini", mnr_root[0] ? mnr_root : ".");
    return p;
}
static int fs_host_path(const char *g, char *out, size_t cap);   /* cellFs path mapping (below) */
static int fs_host_path_read(const char *g, char *out, size_t cap);   /* the same, preferring files from enabled mods */
static void ww_touch(uint32_t addr, size_t n);   /* guest pages the kernel wrote (fread, recv): marked for the renderer */
static void fs_mkdirs(const char *path);
static int fs_writable(const char *host);   /* only hdd0/hdd1 may be modified */

#include "game.h"   /* per-build guest addresses (game_v100.h / game_v111.h) */
#define TOC        G_TOC
static void apply_patches(void);
#define HOST_CODE  0x0F000000u
#define HOST_OPD   0x0F100000u
#define HEAP_LO    0x10000000u
#define HEAP_HI    0x1F000000u
#define STACK_TOP  0x1FFF0000u
#define COMMIT     0x20000000u

/* Lua entry points identified in the EBOOT (see lua_api.tsv / FORMATS.md) */
#define LUA_NEWSTATE     0x37F40u
#define LUA_ATPANIC      0x1CB00u
#define LUAOPEN_BASE     0x21408u
#define LUAOPEN_STRING   0x386DCu
#define LUAOPEN_TABLE    0x3C6E0u
#define LUAL_LOADBUFFER  0x21100u
#define LUA_PCALL        0x1E47Cu
#define LUA_GETFIELD     0x1DBD4u
#define LUA_TONUMBER     0x1D288u
#define LUA_TOLSTRING    0x1D39Cu
#define LUA_TYPE         0x1CFE4u
#define LUA_SETTOP       0x1CBB0u
#define LUA_GLOBALSINDEX (-10002)

/* ------------------------------------------------------------------ guest heap (bump) */
static uint32_t heap_top = HEAP_LO;
static uint32_t gmalloc(uint32_t n) {
    AcquireSRWLockExclusive(&alloc_lock);
    uint32_t a = (heap_top + 16 + 15) & ~15u;
    if (a + n > HEAP_HI) { fprintf(stderr, "guest heap exhausted\n"); exit(2); }
    st_u32(a - 4, n);
    heap_top = a + n;
    ReleaseSRWLockExclusive(&alloc_lock);
    return a;
}
static uint32_t gstrdup(const char *s, size_t n) {
    uint32_t a = gmalloc((uint32_t)n + 1);
    memcpy(gp(a), s, n); *gp(a + (uint32_t)n) = 0;
    return a;
}

/* ------------------------------------------------------------------ host functions callable from guest */
typedef void (*host_fn)(ctx_t *);
static host_fn host_fns[64];
static const char *host_names[64];
static unsigned host_count;

static uint32_t host_register(host_fn fn, const char *name) {  /* returns the OPD address */
    unsigned i = host_count++;
    host_fns[i] = fn; host_names[i] = name;
    uint32_t opd = HOST_OPD + 8 * i;
    st_u32(opd, HOST_CODE + 4 * i);
    st_u32(opd + 4, TOC);
    return opd;
}

static unsigned long long alloc_calls, alloc_bytes;
static void host_lua_alloc(ctx_t *c) {   /* void *(*lua_Alloc)(void *ud, void *ptr, size_t osize, size_t nsize) */
    uint32_t ptr = (uint32_t)c->r[4], osize = (uint32_t)c->r[5], nsize = (uint32_t)c->r[6];
    alloc_calls++;
    if (nsize == 0) { c->r[3] = 0; return; }           /* free: bump allocator never reuses */
    uint32_t a = gmalloc(nsize);
    alloc_bytes += nsize;
    if (ptr) memcpy(gp(a), gp(ptr), osize < nsize ? osize : nsize);
    c->r[3] = a;
}
static void host_lua_panic(ctx_t *c) {
    fprintf(stderr, "lua panic\n");
    exit(3);
}

/* ------------------------------------------------------------------ dispatch */
/* indirect calls: a direct table over the code range, one slot per instruction word */
static void (**fn_direct)(ctx_t *); static uint32_t fn_direct_lo, fn_direct_n;
static void fn_direct_build(void) {
    fn_direct_lo = fn_table[0].addr;
    fn_direct_n = (fn_table[fn_table_count - 1].addr - fn_direct_lo) / 4 + 1;
    fn_direct = calloc(fn_direct_n, sizeof *fn_direct);
    for (unsigned i = 0; i < fn_table_count; i++) fn_direct[(fn_table[i].addr - fn_direct_lo) / 4] = fn_table[i].fn;
}
static const fn_entry_t *find_fn(uint32_t addr) {
    unsigned lo = 0, hi = fn_table_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (fn_table[mid].addr == addr) return &fn_table[mid];
        if (fn_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void dispatch(ctx_t *c, uint32_t target, uint32_t from) {
    if (target >= HOST_CODE && target < HOST_CODE + 4 * host_count) {
        host_fns[(target - HOST_CODE) / 4](c);
        return;
    }
    if (!fn_direct) fn_direct_build();
    uint32_t slot = (target - fn_direct_lo) / 4;
    if (!(target & 3) && slot < fn_direct_n && fn_direct[slot]) { fn_direct[slot](c); return; }
    const fn_entry_t *e = find_fn(target);
    if (!e) {
        fprintf(stderr, "dispatch: no recompiled function at 0x%08x (from 0x%08x)\n", target, from);
        exit(4);
    }
    e->fn(c);
}

static void call_guest(ctx_t *c, uint32_t addr) {
    c->lr = 0;
    c->r[2] = TOC;
    dispatch(c, addr, 0);
}

/* ------------------------------------------------------------------ setjmp/longjmp pairing */
#define MAX_JMPBUFS 256
static struct { uint32_t guest; jmp_buf buf; } jmpbufs[MAX_JMPBUFS];
jmp_buf *host_jmpbuf(uint32_t guest) {
    unsigned h = (guest >> 4) % MAX_JMPBUFS;
    AcquireSRWLockExclusive(&alloc_lock);
    for (unsigned i = 0; i < MAX_JMPBUFS; i++) {
        unsigned k = (h + i) % MAX_JMPBUFS;
        if (jmpbufs[k].guest == guest || jmpbufs[k].guest == 0) {
            jmpbufs[k].guest = guest;
            ReleaseSRWLockExclusive(&alloc_lock);
            return &jmpbufs[k].buf;
        }
    }
    fprintf(stderr, "too many live jmp_bufs\n");
    exit(10);
}

/* ------------------------------------------------------------------ guest user memory (sys_memory) */
#define USER_LO   0x30000000u   /* PS3 user memory region; committed lazily in 1 MB steps */
#define USER_HI   0x40000000u
static uint32_t user_top = USER_LO;
static uint32_t user_alloc(uint32_t size, uint32_t align) {
    AcquireSRWLockExclusive(&alloc_lock);
    uint32_t a = (user_top + align - 1) & ~(align - 1);
    if (a + size > USER_HI || !VirtualAlloc(guest_mem + a, size, MEM_COMMIT, PAGE_READWRITE)) a = 0;
    else user_top = a + size;
    ReleaseSRWLockExclusive(&alloc_lock);
    return a;
}

/* ------------------------------------------------------------------ HLE */
static int hle_lenient;
#define ARG(n) (c->r[3 + (n)])
#define ARG32(n) ((uint32_t)c->r[3 + (n)])
#define RET(v) (c->r[3] = (uint64_t)(int64_t)(v))

typedef void (*hle_fn)(ctx_t *);
static void hle_ok(ctx_t *c) { RET(0); }
/* cellSysutilGetBgmPlaybackStatus(2)(CellSysutilBgmPlaybackStatus(2) *): no PS3 music player here: playerState 1 (STOP),
 * enableState 0 (ENABLE: the game plays its own music). PLAY (0) made the game ask, on joining with voice chat on, whether
 * to turn voice chat off because PS3 music can't play alongside it. */
static void sysutil_bgm_status(ctx_t *c) { if (ARG32(0)) { mem_zero(ARG32(0), 8); st_u8(ARG32(0), 1); } RET(0); }
static void hle_fail_neg1(ctx_t *c) { RET(-1); }

static void sys_initialize_tls(ctx_t *c) {   /* (main thread id, tls seg addr, tls seg size, tls mem size) */
    uint32_t seg = ARG32(1), filesz = ARG32(2), memsz = ARG32(3);
    uint32_t tls = gmalloc(0x8000 + memsz);
    memcpy(gp(tls), gp(seg), filesz);
    memset(gp(tls + filesz), 0, memsz - filesz);
    c->r[13] = tls + 0x7030;
    RET(0);
}
static void sys_time_get_system_time(ctx_t *c) {   /* microseconds */
    RET(host_timebase() / 80);
}
static uint32_t lwmutex_next = 1;
static void sys_lwmutex_create(ctx_t *c) {   /* (sys_lwmutex_t *mutex, attr *) - single thread: just tag it */
    st_u32(ARG32(0) + 0x10, lwmutex_next++);   /* sleep_queue id field */
    RET(0);
}

static void cell_sync_mutex_init(ctx_t *c) { st_u32(ARG32(0), 0); RET(0); }   /* CellSyncMutex: 4 bytes */
/* CellSyncMutex {be16 rel, be16 acq}: locked while acq != rel. TryLock takes it (acq++) only when free, Unlock
 * releases (rel++). Both atomic on the guest word: SPUs and other PPU threads use the same mutex. (These were stubs
 * that always succeeded, so two threads could hold it at once.) */
static void cell_sync_mutex_trylock(ctx_t *c) {
    volatile LONG *w = (volatile LONG *)gp(ARG32(0));
    for (;;) {
        LONG raw = *w; uint32_t v = __builtin_bswap32((uint32_t)raw);
        uint16_t rel = (uint16_t)(v >> 16), acq = (uint16_t)v;
        if (rel != acq) { RET(0x8041010A); return; }   /* CELL_SYNC_ERROR_BUSY */
        uint32_t nv = (uint32_t)rel << 16 | (uint16_t)(acq + 1);
        if (InterlockedCompareExchange(w, (LONG)__builtin_bswap32(nv), raw) == raw) { RET(0); return; }
    }
}
static void cell_sync_mutex_unlock(ctx_t *c) {
    volatile LONG *w = (volatile LONG *)gp(ARG32(0));
    for (;;) {
        LONG raw = *w; uint32_t v = __builtin_bswap32((uint32_t)raw);
        uint32_t nv = (uint32_t)(uint16_t)((v >> 16) + 1) << 16 | (v & 0xFFFF);
        if (InterlockedCompareExchange(w, (LONG)__builtin_bswap32(nv), raw) == raw) { RET(0); return; }
    }
}

/* cellSysutil callbacks: up to 4 slots of (opd, userdata); events go out from cellSysutilCheckCallback
 * (sysutil_post, with the on-screen keyboard below). */
static uint32_t sysutil_cb[4][2];
static void cell_sysutil_register_callback(ctx_t *c) {   /* (slot, func opd, userdata) */
    if (ARG32(0) >= 4) { RET(0x8002b101); return; }       /* CELL_SYSUTIL_ERROR_VALUE */
    sysutil_cb[ARG32(0)][0] = ARG32(1); sysutil_cb[ARG32(0)][1] = ARG32(2);
    RET(0);
}

/* ------------------------------------------------------------------ threads and synchronisation
 * PPU threads are host threads (each with its own ctx_t, guest stack and TLS block). Guest sync
 * objects map to host ones, keyed by guest address (lw* objects) or by kernel id. */
#define CELL_EBUSY     0x8001000Au
#define CELL_ETIMEDOUT 0x8001000Bu
#define CELL_ESRCH     0x80010005u
#define CELL_EAGAIN    0x80010001u

typedef struct { ctx_t ctx; HANDLE h; uint32_t entry; uint64_t exit_value; char name[32]; } pthr_t;
#define MAX_KOBJ 65536
static void *kobj[MAX_KOBJ];            /* kernel objects by (id & 0xFFFF) */
static uint32_t kobj_next = 1;
static SRWLOCK kobj_lock = SRWLOCK_INIT;
static uint32_t kobj_new(void *p, uint32_t tag) {
    AcquireSRWLockExclusive(&kobj_lock);
    uint32_t i = kobj_next++ & (MAX_KOBJ - 1);
    kobj[i] = p;
    ReleaseSRWLockExclusive(&kobj_lock);
    return tag | i;
}
static void *kobj_get(uint32_t id) { return kobj[id & (MAX_KOBJ - 1)]; }

/* address-keyed host objects (lwmutex -> CRITICAL_SECTION, lwcond -> CONDITION_VARIABLE + its lwmutex) */
#define MAX_AOBJ 65536
#define AOBJ_DEAD 0xFFFFFFFFu   /* tombstone: slot freed by a destroy */
static struct { uint32_t addr; void *obj; } aobj[MAX_AOBJ];
static void *aobj_get(uint32_t addr, size_t size) {   /* creates (zeroed) on first use */
    if (addr == 0 || addr == AOBJ_DEAD) {
        fprintf(stderr, "[aobj] sync object at guest address 0x%08x; recent calls on this thread:", addr);
        for (unsigned i = 0; i < 12; i++) {
            uint64_t e = trace_ring[(trace_pos - 1 - i) & 31];
            fprintf(stderr, " %06x<-%06x", (unsigned)(e >> 32), (unsigned)e);
        }
        fprintf(stderr, "\n");
        exit(12);
    }
    AcquireSRWLockExclusive(&kobj_lock);
    unsigned h = (addr >> 3) % MAX_AOBJ, free_slot = MAX_AOBJ;
    void *r = NULL;
    for (unsigned i = 0; i < MAX_AOBJ; i++) {
        unsigned k = (h + i) % MAX_AOBJ;
        if (aobj[k].addr == addr) { r = aobj[k].obj; break; }
        if (aobj[k].addr == AOBJ_DEAD) { if (free_slot == MAX_AOBJ) free_slot = k; continue; }
        if (aobj[k].addr == 0) { if (free_slot == MAX_AOBJ) free_slot = k; break; }
    }
    if (!r && free_slot != MAX_AOBJ) {
        aobj[free_slot].addr = addr; aobj[free_slot].obj = r = calloc(1, size);
        static unsigned created;
        if (++created % 4096 == 0) fprintf(stderr, "[aobj] %u address-keyed objects created; latest 0x%08x size %u\n", created, addr, (unsigned)size);
    }
    ReleaseSRWLockExclusive(&kobj_lock);
    if (!r) { fprintf(stderr, "too many guest sync objects\n"); exit(11); }
    return r;
}
/* destroy: drop the address mapping. The host object is leaked on purpose (another thread may still
 * be returning from a wait on it); a later create at the same address gets a fresh one. */
static void aobj_del(uint32_t addr) {
    AcquireSRWLockExclusive(&kobj_lock);
    unsigned h = (addr >> 3) % MAX_AOBJ;
    for (unsigned i = 0; i < MAX_AOBJ; i++) {
        unsigned k = (h + i) % MAX_AOBJ;
        if (aobj[k].addr == addr) { aobj[k].addr = AOBJ_DEAD; break; }
        if (aobj[k].addr == 0) break;
    }
    ReleaseSRWLockExclusive(&kobj_lock);
}
static void sys_lw_destroy(ctx_t *c) { aobj_del(ARG32(0)); RET(0); }   /* sys_lwmutex_destroy / sys_lwcond_destroy */

typedef struct { CRITICAL_SECTION cs; int init; SRWLOCK once; } hmutex_t;
static CRITICAL_SECTION *lwmutex_cs(uint32_t addr) {
    hmutex_t *m = aobj_get(addr, sizeof(hmutex_t));
    AcquireSRWLockExclusive(&m->once);
    if (!m->init) { InitializeCriticalSection(&m->cs); m->init = 1; }
    ReleaseSRWLockExclusive(&m->once);
    return &m->cs;
}
typedef struct { CONDITION_VARIABLE cv; uint32_t lwmutex; } hlwcond_t;

static DWORD wait_ms(uint64_t usec) { return usec ? (DWORD)((usec + 999) / 1000) : INFINITE; }

/* registry of guest threads for the watchdog dump: each thread registers its ctx and trace ring */
typedef struct { ctx_t *ctx; uint64_t *ring; unsigned *pos; const char *name; HANDLE h;
                 const char *volatile hle; volatile uint32_t hle_lr; } treg_t;   /* hle: import/syscall in progress (profiler) */
static treg_t tregs[128];
static volatile long treg_count;
static __thread treg_t *my_treg;
static void thread_register(ctx_t *c, const char *name) {
    long i = InterlockedIncrement(&treg_count) - 1;
    if (i < 128) {
        my_treg = &tregs[i];
        tregs[i].ctx = c; tregs[i].ring = trace_ring; tregs[i].pos = &trace_pos; tregs[i].name = name;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &tregs[i].h,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    }
}

/* MNR_PROFILE=<seconds>: sample every guest thread's host instruction pointer each millisecond and write
 * the hottest addresses (as image-relative + 0x140000000, for llvm-addr2line -f -e out/mnr.exe) to
 * mnr_profile.txt every <seconds>. MNR_PROFILE_THREAD=<text> samples only the threads whose name contains it. */
#define PROF_SLOTS (1 << 16)
static struct { uint64_t rip; uint32_t n; } prof[PROF_SLOTS], cprof[PROF_SLOTS];
static struct { HMODULE mod; uint32_t n; uint64_t thread_mask; } modprof[64];
static ctx_t cp_ctx_prof;   /* the RSX command processor's registration (gcm_thread) */
static uint32_t thr_in[128], thr_out[128];   /* per guest thread: samples in mnr.exe / elsewhere */
static struct { long t; const char *hle; uint32_t lr, n; } waitprof[1024];   /* out-of-exe samples by (thread, HLE call, caller) */
static DWORD WINAPI profiler_thread(void *p) {
    unsigned every = (unsigned)(uintptr_t)p;
    uint64_t base = (uint64_t)GetModuleHandleA(NULL), total = 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uint64_t end = base + nt->OptionalHeader.SizeOfImage;   /* only samples inside mnr.exe count (not waits in system DLLs) */
    DWORD last = GetTickCount();
    const char *only = getenv("MNR_PROFILE_THREAD");   /* sample only threads whose name contains this */
    int unwind = getenv("MNR_PROFILE_UNWIND") != NULL; /* attribute out-of-exe samples to their caller in mnr.exe */
    for (;;) {
        Sleep(1);
        for (long t = 0; t < treg_count && t < 128; t++) {
            HANDLE h = tregs[t].h;
            if (only && !(tregs[t].name && strstr(tregs[t].name, only))) continue;
            if (!h || SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT cx; cx.ContextFlags = unwind ? CONTEXT_FULL : CONTEXT_CONTROL;
            if (GetThreadContext(h, &cx)) {
                uint64_t rip = cx.Rip;
                if ((rip < base || rip >= end) && unwind) {   /* charge the sample to the nearest caller inside mnr.exe */
                    CONTEXT u = cx;
                    for (int depth = 0; depth < 48; depth++) {
                        DWORD64 ib = 0; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &ib, NULL);
                        if (fe) { void *hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, fe, &u, &hd, &est, NULL); }
                        else { u.Rip = *(DWORD64 *)u.Rsp; u.Rsp += 8; }
                        if (!u.Rip) break;
                        if (u.Rip >= base && u.Rip < end) {
                            unsigned k = (unsigned)((u.Rip * 0x9E3779B97F4A7C15ull) >> 48) & (PROF_SLOTS - 1);
                            for (unsigned j = 0; j < 64; j++, k = (k + 1) & (PROF_SLOTS - 1)) {
                                if (cprof[k].rip == u.Rip) { cprof[k].n++; break; }
                                if (!cprof[k].rip) { cprof[k].rip = u.Rip; cprof[k].n = 1; break; }
                            }
                            break;
                        }
                    }
                }
                if (rip < base || rip >= end) {
                    thr_out[t]++;
                    const char *hn = tregs[t].hle; uint32_t lr = tregs[t].hle_lr;
                    if (tregs[t].ctx == &cp_ctx_prof) hn = (const char *)rsx_cp_state;   /* the command processor reports its own state */
                    for (int k = 0; k < 1024; k++) {
                        if (!waitprof[k].n) { waitprof[k].t = t; waitprof[k].hle = hn; waitprof[k].lr = lr; waitprof[k].n = 1; break; }
                        if (waitprof[k].t == t && waitprof[k].hle == hn && waitprof[k].lr == lr) { waitprof[k].n++; break; }
                    }
                } else thr_in[t]++;
                if (rip < base || rip >= end) {   /* outside mnr.exe: count per module (driver, ntdll waits, ...) */
                    HMODULE m = NULL;
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m);
                    int k = 0;
                    while (k < 64 && modprof[k].mod && modprof[k].mod != m) k++;
                    if (k < 64) { modprof[k].mod = m; modprof[k].n++; modprof[k].thread_mask |= 1ull << (t & 63); }
                    ResumeThread(h); continue;
                }
                unsigned k = (unsigned)((rip * 0x9E3779B97F4A7C15ull) >> 48) & (PROF_SLOTS - 1);
                for (unsigned j = 0; j < 64; j++, k = (k + 1) & (PROF_SLOTS - 1)) {
                    if (prof[k].rip == rip) { prof[k].n++; break; }
                    if (!prof[k].rip) { prof[k].rip = rip; prof[k].n = 1; break; }
                }
                total++;
            }
            ResumeThread(h);
        }
        if (GetTickCount() - last >= every * 1000) {
            last = GetTickCount();
            char pn[64]; static int keep = -1; if (keep < 0) keep = getenv("MNR_PROFILE_KEEP") != NULL;   /* one file per snapshot */
            if (keep) snprintf(pn, sizeof pn, "mnr_profile_%05lu.txt", (unsigned long)(GetTickCount64() / 1000 % 100000)); else strcpy(pn, "mnr_profile.txt");
            FILE *f = fopen(pn, "w");
            if (!f) continue;
            fprintf(f, "# %llu samples; columns: count, address for addr2line (image base 0x140000000)\n", (unsigned long long)total);
            for (int top = 0; top < 1500; top++) {   /* simple selection of the top entries */
                int best = -1;
                for (int k = 0; k < PROF_SLOTS; k++)
                    if (prof[k].rip && !(prof[k].n & 0x80000000u) && (best < 0 || prof[k].n > prof[best].n)) best = k;
                if (best < 0 || !prof[best].n) break;
                fprintf(f, "%u 0x%llx\n", prof[best].n, (unsigned long long)(prof[best].rip - base + 0x140000000ull));
                prof[best].n |= 0x80000000u;   /* mark as printed */
            }
            for (int k = 0; k < PROF_SLOTS; k++) prof[k].n &= 0x7FFFFFFFu;
            for (int top = 0; unwind && top < 200; top++) {   /* "# caller <count> <address>": out-of-exe time by calling site */
                int best = -1;
                for (int k = 0; k < PROF_SLOTS; k++)
                    if (cprof[k].rip && !(cprof[k].n & 0x80000000u) && (best < 0 || cprof[k].n > cprof[best].n)) best = k;
                if (best < 0 || !cprof[best].n) break;
                fprintf(f, "# caller %u 0x%llx\n", cprof[best].n, (unsigned long long)(cprof[best].rip - base + 0x140000000ull));
                cprof[best].n |= 0x80000000u;
            }
            for (int k = 0; k < PROF_SLOTS; k++) cprof[k].n &= 0x7FFFFFFFu;
            for (int k = 0; k < 64 && modprof[k].mod; k++) {
                char nm[MAX_PATH] = "?"; GetModuleFileNameA(modprof[k].mod, nm, sizeof nm);
                const char *bn = strrchr(nm, '\\'); bn = bn ? bn + 1 : nm;
                fprintf(f, "# module %s: %u samples, threads", bn, modprof[k].n);
                for (int t = 0; t < 64; t++) if (modprof[k].thread_mask >> t & 1) fprintf(f, " %s", tregs[t].name);
                fprintf(f, "\n");
            }
            for (long t = 0; t < treg_count && t < 128; t++)
                if (thr_in[t] + thr_out[t]) fprintf(f, "# thread %-40s %8u in mnr.exe %8u elsewhere\n", tregs[t].name ? tregs[t].name : "?", thr_in[t], thr_out[t]);
            for (int k = 0; k < 1024 && waitprof[k].n; k++)
                if (waitprof[k].n > 200) fprintf(f, "# wait %-32s %-28s lr 0x%08x %8u\n", tregs[waitprof[k].t].name ? tregs[waitprof[k].t].name : "?",
                                                 waitprof[k].hle ? waitprof[k].hle : "(no HLE call)", waitprof[k].lr, waitprof[k].n);
            if (getenv("MNR_PROFILE_DELTA")) { memset(thr_in, 0, sizeof thr_in); memset(thr_out, 0, sizeof thr_out); memset(waitprof, 0, sizeof waitprof); memset(cprof, 0, sizeof cprof); for (int k = 0; k < 64; k++) modprof[k].n = 0; }
            fclose(f);
        }
    }
    return 0;
}
/* MNR_TIMELINE=<s> (debug): <s> seconds after the race began, record 3 s of every blocking call over 50 us on every
 * guest thread (what, caller, how long) and the command processor's waits and flips, then write mnr_timeline.txt:
 * shows which wait each frame's critical path runs through */
typedef struct { LONGLONG t0, t1; const char *th, *what; uint32_t lr, arg; } tl_ev_t;
#define TL_MAX (1 << 20)
static tl_ev_t *tl_ev; static volatile long tl_n; volatile int tl_state;   /* 1: recording */
static LONGLONG tl_us50;
void tl_rec(const char *what, LONGLONG t0, LONGLONG t1, uint32_t lr, uint32_t arg) {
    if (tl_state != 1 || t1 - t0 < tl_us50) return;
    long i = InterlockedIncrement(&tl_n) - 1; if (i >= TL_MAX) return;
    tl_ev[i].t0 = t0; tl_ev[i].t1 = t1; tl_ev[i].th = my_treg && my_treg->name ? my_treg->name : "?"; tl_ev[i].what = what; tl_ev[i].lr = lr; tl_ev[i].arg = arg;
}
void tl_mark(const char *what, uint32_t arg) { LARGE_INTEGER t; QueryPerformanceCounter(&t); LONGLONG s = tl_us50; tl_us50 = 0; tl_rec(what, t.QuadPart, t.QuadPart, 0, arg); tl_us50 = s; }
static DWORD WINAPI tl_thread(void *p) {
    while (!race_began) Sleep(100);
    Sleep((DWORD)(uintptr_t)p * 1000);
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); tl_us50 = f.QuadPart / 20000;
    tl_ev = calloc(TL_MAX, sizeof *tl_ev); QueryPerformanceCounter(&t0); tl_state = 1;
    Sleep(3000); tl_state = 2; Sleep(50);
    FILE *o = fopen("mnr_timeline.txt", "w"); if (!o) return 0;
    long n = tl_n < TL_MAX ? tl_n : TL_MAX;
    for (long i = 0; i < n; i++) { tl_ev_t *e = &tl_ev[i];
        fprintf(o, "%.0f %.0f %s|%s|0x%08x|0x%x\n", (e->t0 - t0.QuadPart) * 1e6 / f.QuadPart, (e->t1 - t0.QuadPart) * 1e6 / f.QuadPart, e->th, e->what, e->lr, e->arg); }
    fclose(o); printf("[timeline] %ld events written to mnr_timeline.txt\n", n);
    return 0;
}
static void gcm_cp_report(void);
/* every guest thread's state: where it is, the import/syscall it waits in, its recent calls. F9 in the game
 * window writes this to <MNR>/logs/dump_<time>.txt (for hangs: press it while stuck) */
static volatile long gcm_flips;   /* (defined with the GCM state below) */
void mnr_dump_threads(FILE *o) {
    fprintf(o, "===== %ld guest threads, flip %ld =====\n", treg_count, (long)gcm_flips);
    fprintf(o, "-- RSX command processor: %s, get 0x%x put 0x%x\n", rsx_cp_state, ld_u32(0x0E010000u + 4), ld_u32(0x0E010000u));   /* GCM_CONTROL */
    for (long t = 0; t < treg_count && t < 128; t++) {
        treg_t *r = &tregs[t]; ctx_t *x = r->ctx;
        fprintf(o, "-- thread 0x%08llx '%s' lr=%08llx r3=%08llx in %s (from 0x%08x)\n   recent:", (unsigned long long)x->thread_id,
                r->name, (unsigned long long)x->lr, (unsigned long long)x->r[3], r->hle ? r->hle : "(game code)", r->hle_lr);
        unsigned pos = *r->pos;
        for (unsigned i = 16; i < 32; i++) { uint64_t e = r->ring[(pos + i) & 31]; fprintf(o, " %08x<-%08x", (uint32_t)(e >> 32), (uint32_t)e); }
        fprintf(o, "\n");
    }
    fflush(o);
}
static DWORD WINAPI dump_to_file(void *p) {
    (void)p;
    char dir[MAX_PATH], path[MAX_PATH]; SYSTEMTIME st; GetLocalTime(&st);
    snprintf(dir, sizeof dir, "%s\\logs", mnr_root); CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\dump_%04d%02d%02d_%02d%02d%02d.txt", dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE *f = fopen(path, "w");
    if (f) {
        mnr_dump_threads(f);
        log_state(f);
        /* the host call stack of every guest thread (where a thread blocked inside Windows: module+offset; for
         * mnr.exe, llvm-addr2line -f -e mnr.exe <0x140000000 + offset>) */
        fprintf(f, "\n===== native stacks =====\n");
        for (long t = 0; t < treg_count && t < 128; t++) {
            HANDLE h = tregs[t].h;
            if (!h || GetThreadId(h) == GetCurrentThreadId() || SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT cx; cx.ContextFlags = CONTEXT_FULL;
            fprintf(f, "-- %s:", tregs[t].name ? tregs[t].name : "?");
            if (GetThreadContext(h, &cx)) {
                for (int d = 0; d < 24 && cx.Rip; d++) {
                    HMODULE m = NULL; char mn[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)cx.Rip, &m) && m) {
                        GetModuleFileNameA(m, mn, sizeof mn); const char *b = strrchr(mn, '\\'); if (b) memmove(mn, b + 1, strlen(b));
                    }
                    fprintf(f, " %s+0x%llx", mn, (unsigned long long)(cx.Rip - (DWORD64)m));
                    DWORD64 ib = 0; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(cx.Rip, &ib, NULL);
                    if (fe) { void *hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, cx.Rip, fe, &cx, &hd, &est, NULL); }
                    else { cx.Rip = *(DWORD64 *)cx.Rsp; cx.Rsp += 8; }
                }
            }
            fprintf(f, "\n");
            ResumeThread(h);
        }
        fclose(f); printf("[dump] thread states written to %s\n", path);
    }
    return 0;
}
void mnr_request_dump(void) { CreateThread(NULL, 0, dump_to_file, NULL, 0, NULL); }
static DWORD WINAPI f9_thread(void *p) {   /* F9 with the console in front (another process's window, so polled) */
    (void)p; HWND con = GetConsoleWindow(); int was = 0;
    for (;;) {
        Sleep(50);
        HWND fg = GetForegroundWindow();
        int down = con && fg == con && (GetAsyncKeyState(VK_F9) & 0x8000);   /* the game window handles F9 itself (present.c) */
        if (down && !was) mnr_request_dump();
        was = down;
        static int tick; if (++tick % 20 == 0) { fflush(stdout); fflush(stderr); }   /* a killed run keeps its log */
    }
    return 0;
}
static DWORD WINAPI watchdog_thread(void *p) {   /* MNR_DUMP_AFTER=<s>, optionally MNR_DUMP_REPEAT=<s> */
    unsigned after = (unsigned)(uintptr_t)p, rep = getenv("MNR_DUMP_REPEAT") ? (unsigned)atoi(getenv("MNR_DUMP_REPEAT")) : 0;
    Sleep(after * 1000);
    for (;;) {
        fprintf(stderr, "\n===== watchdog: %ld guest threads after %u s =====\n", treg_count, after);
        gcm_cp_report();
        for (long t = 0; t < treg_count && t < 128; t++) {
            treg_t *r = &tregs[t];
            ctx_t *x = r->ctx;
            fprintf(stderr, "-- thread 0x%08llx '%s' lr=%08llx r1=%08llx r3=%08llx\n   recent:", (unsigned long long)x->thread_id,
                    r->name, (unsigned long long)x->lr, (unsigned long long)x->r[1], (unsigned long long)x->r[3]);
            unsigned pos = *r->pos;
            for (unsigned i = 24; i < 32; i++) {
                uint64_t e = r->ring[(pos + i) & 31];
                fprintf(stderr, " %08x<-%08x", (uint32_t)(e >> 32), (uint32_t)e);
            }
            fprintf(stderr, "\n   r23-r31:");
            for (int i = 23; i < 32; i++) fprintf(stderr, " %llx", (unsigned long long)x->r[i]);
            fprintf(stderr, "  f27 %g f31 %g\n", x->f[27].d, x->f[31].d);
        }
        fflush(stderr);
        if (!rep) break;
        Sleep(rep * 1000); after += rep;
    }
    return 0;
}

static DWORD WINAPI ppu_thread_main(void *p) {
    pthr_t *t = p;
    crash_ctx = &t->ctx;
    thread_register(&t->ctx, t->name);
    dispatch(&t->ctx, t->entry, 0);
    return 0;
}

static uint32_t ppu_thread_spawn(uint32_t opd, uint64_t arg, uint32_t stack, const char *name) {
    pthr_t *t = calloc(1, sizeof *t);
    uint32_t sp = gmalloc(stack + 0x100);
    t->entry = ld_u32(opd);
    t->ctx.r[1] = (sp + stack - 0x70) & ~15u;
    t->ctx.r[2] = ld_u32(opd + 4);
    t->ctx.r[3] = arg;
    uint32_t tls = gmalloc(0x8000 + tls_memsz);
    memcpy(gp(tls), gp(tls_va), tls_filesz);
    memset(gp(tls + tls_filesz), 0, tls_memsz - tls_filesz);
    t->ctx.r[13] = tls + 0x7030;
    uint32_t id = kobj_new(t, 0x01000000);
    t->ctx.thread_id = id;
    snprintf(t->name, sizeof t->name, "%s", name);
    printf("[thread] create 0x%08x '%s' entry 0x%08x stack 0x%x\n", id, name, t->entry, stack);
    t->h = CreateThread(NULL, 64 << 20, ppu_thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return t->h ? id : 0;
}
static void sys_ppu_thread_create(ctx_t *c) {   /* (u64 *id, opd entry, u64 arg, prio, stacksize, flags, name) */
    uint32_t opd = ARG32(1), stack = ARG32(4) ? ARG32(4) : 0x10000;
    const char *name = ARG32(6) ? (const char *)gp(ARG32(6)) : "";
    uint32_t id = ppu_thread_spawn(opd, ARG(2), stack, name);
    st_u64(ARG32(0), id);
    /* qTasks that would run on up to five SPUs share one PPU worker here; tasks that wait on each other
     * (or long data-parallel batches) need more. MNR_QTASK_WORKERS (default 4) workers run the same loop. */
    if (!strcmp(name, "qTaskManager_WorkerThread")) {
        const char *e = getenv("MNR_QTASK_WORKERS");   /* (8 measured the same as 4 in races at the 60 fps cap) */
        int n = e ? atoi(e) : 4;
        for (int i = 1; i < n && i < 16; i++) {
            char nm[32]; snprintf(nm, sizeof nm, "qTaskManager_Worker+%d", i);
            ppu_thread_spawn(opd, ARG(2), stack, nm);
        }
    }
    RET(id ? 0 : CELL_EAGAIN);
}
static void ppu_thread_exit(ctx_t *c) {   /* syscall 41 and sysPrxForUser::sys_ppu_thread_exit */
    if (c->thread_id == 0) { printf("main thread exit(0x%llx)\n", (unsigned long long)ARG(0)); fflush(stdout); exit(0); }
    ((pthr_t *)kobj_get((uint32_t)c->thread_id))->exit_value = ARG(0);
    printf("[thread] exit 0x%08llx\n", (unsigned long long)c->thread_id);
    ExitThread(0);
}
static void sys_ppu_thread_get_id_r(ctx_t *c) { st_u64(ARG32(0), c->thread_id ? c->thread_id : 0x01000500); RET(0); }

static void sys_lwmutex_lock(ctx_t *c) { EnterCriticalSection(lwmutex_cs(ARG32(0))); RET(0); }
static void sys_lwmutex_unlock(ctx_t *c) { LeaveCriticalSection(lwmutex_cs(ARG32(0))); RET(0); }
static void sys_lwmutex_trylock(ctx_t *c) { RET(TryEnterCriticalSection(lwmutex_cs(ARG32(0))) ? 0 : CELL_EBUSY); }
static void sys_lwcond_create(ctx_t *c) {   /* (lwcond *, lwmutex *, attr *) */
    hlwcond_t *cv = aobj_get(ARG32(0), sizeof(hlwcond_t));
    InitializeConditionVariable(&cv->cv);
    cv->lwmutex = ARG32(1);
    st_u32(ARG32(0), ARG32(1));   /* sys_lwcond_t: lwmutex pointer first */
    RET(0);
}
static void sys_lwcond_wait(ctx_t *c) {   /* (lwcond *, timeout usec) */
    hlwcond_t *cv = aobj_get(ARG32(0), sizeof(hlwcond_t));
    uint32_t mtx = cv->lwmutex ? cv->lwmutex : ld_u32(ARG32(0));
    BOOL ok = SleepConditionVariableCS(&cv->cv, lwmutex_cs(mtx), wait_ms(ARG(1)));
    RET(ok ? 0 : CELL_ETIMEDOUT);
}
static void sys_lwcond_signal(ctx_t *c) { WakeConditionVariable(&((hlwcond_t *)aobj_get(ARG32(0), sizeof(hlwcond_t)))->cv); RET(0); }
static void sys_lwcond_signal_all(ctx_t *c) { WakeAllConditionVariable(&((hlwcond_t *)aobj_get(ARG32(0), sizeof(hlwcond_t)))->cv); RET(0); }

/* kernel mutex / cond / semaphore objects (syscalls) */
typedef struct { CRITICAL_SECTION cs; } kmutex_t;
typedef struct { CONDITION_VARIABLE cv; kmutex_t *m; } kcond_t;
static void ksys_mutex_create(ctx_t *c) { kmutex_t *m = calloc(1, sizeof *m); InitializeCriticalSection(&m->cs); st_u32(ARG32(0), kobj_new(m, 0x85000000)); RET(0); }
static void ksys_mutex_lock(ctx_t *c) { kmutex_t *m = kobj_get(ARG32(0)); if (!m) { RET(CELL_ESRCH); return; } EnterCriticalSection(&m->cs); RET(0); }
static void ksys_mutex_trylock(ctx_t *c) { kmutex_t *m = kobj_get(ARG32(0)); RET(m && TryEnterCriticalSection(&m->cs) ? 0 : CELL_EBUSY); }
static void ksys_mutex_unlock(ctx_t *c) { kmutex_t *m = kobj_get(ARG32(0)); if (m) LeaveCriticalSection(&m->cs); RET(0); }
static void ksys_cond_create(ctx_t *c) {   /* (*cond_id, mutex_id, attr) */
    kcond_t *k = calloc(1, sizeof *k); InitializeConditionVariable(&k->cv); k->m = kobj_get(ARG32(1));
    st_u32(ARG32(0), kobj_new(k, 0x86000000)); RET(0);
}
static void ksys_cond_wait(ctx_t *c) {
    kcond_t *k = kobj_get(ARG32(0));
    if (!k || !k->m) { RET(CELL_ESRCH); return; }
    RET(SleepConditionVariableCS(&k->cv, &k->m->cs, wait_ms(ARG(1))) ? 0 : CELL_ETIMEDOUT);
}
typedef struct { HANDLE h; } ksema_t;
static void ksys_sema_create(ctx_t *c) {   /* (*sem_id, attr, initial, max) */
    ksema_t *s = calloc(1, sizeof *s);
    s->h = CreateSemaphoreA(NULL, (LONG)ARG32(2), (LONG)ARG32(3), NULL);
    st_u32(ARG32(0), kobj_new(s, 0x96000000)); RET(0);
}
static void ksys_sema_wait(ctx_t *c) {
    ksema_t *s = kobj_get(ARG32(0));
    if (!s) { RET(CELL_ESRCH); return; }
    RET(WaitForSingleObject(s->h, wait_ms(ARG(1))) == WAIT_OBJECT_0 ? 0 : CELL_ETIMEDOUT);
}

/* ------------------------------------------------------------------ video out */
static void video_out_get_state(ctx_t *c) {   /* (videoOut, deviceIndex, CellVideoOutState *) */
    uint32_t s = ARG32(2);
    mem_zero(s, 16);
    st_u8(s + 0, 0);          /* state: ENABLED */
    st_u8(s + 1, 1);          /* colorSpace: RGB */
    st_u8(s + 8, 2);          /* displayMode.resolutionId: 720p */
    st_u8(s + 9, 1);          /* scanMode: progressive */
    st_u8(s + 11, 2);         /* aspect: 16:9 */
    st_u16(s + 14, 0x04);     /* refreshRates: 59.94 Hz */
    RET(0);
}
static void video_out_get_resolution(ctx_t *c) {   /* (resolutionId, CellVideoOutResolution *) */
    static const uint16_t res[][2] = {{0, 0}, {1920, 1080}, {1280, 720}, {0, 0}, {720, 480}, {720, 576}};
    uint32_t id = ARG32(0) < 6 ? ARG32(0) : 2;
    st_u16(ARG32(1), res[id][0] ? res[id][0] : 1280);
    st_u16(ARG32(1) + 2, res[id][1] ? res[id][1] : 720);
    RET(0);
}
static void video_out_one(ctx_t *c) { RET(1); }

/* ------------------------------------------------------------------ GCM (RSX) - null backend
 * Enough of libgcm for the game to run without a GPU: command-buffer context with a wrap
 * callback, control register (put/get/ref), labels, reports, IO mapping, and a command
 * processor thread that follows jumps/calls and executes semaphore/reference methods (the
 * parts the CPU side waits on). Draw commands are skipped; flips are counted. */
#define RSX_LOCAL      0xC0000000u   /* RSX local memory (256 MB) */
#define RSX_LOCAL_SIZE 0x0F900000u
#define GCM_LABELS     0x0E000000u   /* 256 labels x 16 bytes */
#define GCM_CONTROL    0x0E010000u   /* CellGcmControl {put, get, ref} */
#define GCM_CONTEXT    0x0E010100u   /* CellGcmContextData {begin, end, current, callback} */
#define GCM_REPORTS    (RSX_LOCAL + 0x0FE00000u)   /* above the 249 MB the game may allocate (RSX_LOCAL_SIZE): at 0x0F800000 the occlusion reports overwrote a render target (black frames) */
static uint32_t gcm_io_base, gcm_io_size, gcm_io_map[4096];   /* io offset (1 MB pages) -> ea */
static uint16_t gcm_ea_io[4096];   /* reverse: ea >> 20 -> 1 + the lowest io page whose mapping starts in that megabyte (a hint, checked) */
static int gcm_ea_to_io_fast(uint32_t ea, uint32_t *io) {   /* the game converts addresses thousands of times a frame */
    /* the same answer as the scan below: the lowest io page that maps ea (memory mapped twice has two offsets) */
    uint32_t best = 0xFFFFFFFF;
    for (int d = 0; d < 2; d++) {
        uint32_t k = (ea >> 20) - (uint32_t)d;
        if (k >= 4096 || !gcm_ea_io[k]) continue;
        uint32_t i = gcm_ea_io[k] - 1u, e = gcm_io_map[i];
        if (e && ea - e < 0x100000 && i < best) best = i;
    }
    if (best != 0xFFFFFFFF) {   /* a lower page could still map it if the hint's mapping was replaced: scan just below */
        for (uint32_t i = 0; i < best; i++) if (gcm_io_map[i] && ea - gcm_io_map[i] < 0x100000) { best = i; break; }
        *io = (best << 20) + (ea - gcm_io_map[best]); return 1;
    }
    for (uint32_t i = 0; i < 4096; i++)
        if (gcm_io_map[i] && ea - gcm_io_map[i] < 0x100000) { *io = (i << 20) + (ea - gcm_io_map[i]); return 1; }
    return 0;
}
static uint32_t gcm_cb_opd;
static volatile long gcm_flips;

static uint32_t gcm_io_to_ea(uint32_t off) {
    uint32_t ea = gcm_io_map[(off >> 20) & 4095];
    return ea ? ea + (off & 0xFFFFF) : 0;
}
static void gcm_map(uint32_t ea, uint32_t io, uint32_t size) {
    for (uint32_t o = 0; o < size; o += 0x100000) { uint32_t i = ((io + o) >> 20) & 4095; gcm_io_map[i] = ea + o; uint16_t *h = &gcm_ea_io[(ea + o) >> 20]; if (!*h || i + 1 < *h) *h = (uint16_t)(i + 1); }
}

static uint32_t gcm_ea_to_io(uint32_t ea) { uint32_t io; return gcm_ea_to_io_fast(ea, &io) ? io : 0xFFFFFFFF; }
volatile int gcm_frame_ticked;   /* the game's frame lock (label 68) was paced here since the last flip */
static volatile LONGLONG gcm_wrap_qpc; static volatile long gcm_wraps;   /* time the CPU waited in ring wraps */
#define GCM_SEG 0x40000u
static void gcm_callback(ctx_t *c) {   /* context full: (context*, count) -> jump back to the start, like libgcm */
    uint32_t ctx = ARG32(0), begin = ld_u32(ctx), cur = ld_u32(ctx + 8);
    { static int n; static int on = -1; if (on < 0) on = getenv("MNR_GCM_CALLLOG") != NULL; if (on && n++ < 400) printf("[gcmwrap] flip %ld ctx 0x%x begin 0x%x end 0x%x cur 0x%x need %u\n", (long)rsx_flips, ctx, begin, ld_u32(ctx + 4), cur, ARG32(1)); }
    uint32_t begin_io = gcm_ea_to_io(begin), cur_io = gcm_ea_to_io(cur);
    LARGE_INTEGER w0; QueryPerformanceCounter(&w0);
    /* The main context's ring (2 MB) is used in 256 KB segments, as RPCS3 does: at each segment's end the commands
     * so far are flushed (put = the jump into the next segment) and the CPU waits only until the processor has left
     * the segment it is about to fill. Waiting for the whole ring to drain at every wrap made the CPU and the GPU
     * take turns once per busy frame. MNR_GCM_WHOLE_RING=1: the old behaviour (A/B tests). */
    static int whole = -1; if (whole < 0) whole = getenv("MNR_GCM_WHOLE_RING") != NULL;
    static uint32_t ring_b, ring_e;
    if (ctx == GCM_CONTEXT && !whole && cur_io != 0xFFFFFFFF) {
        if (!ring_b) { ring_b = begin; ring_e = ld_u32(ctx + 4); }
        uint32_t next = ring_b + ((cur - ring_b) / GCM_SEG + 1) * GCM_SEG;
        if (next + 0x10000 > ring_e) next = ring_b;   /* (a sliver left at the ring's end: wrap now) */
        uint32_t end = next + GCM_SEG - 4; if (end > ring_e) end = ring_e;
        uint32_t next_io = gcm_ea_to_io(next);
        st_u32(cur, next_io | 1);                        /* JUMP next segment */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        st_u32(GCM_CONTROL, cur_io);                     /* flush up to the jump (taken once the game flushes past it) */
        for (int i = 0; i < 20000; i++) {                /* the processor must be out of the segment to be overwritten */
            uint32_t g = ld_u32(GCM_CONTROL + 4);
            if (g - next_io >= GCM_SEG) break;
            if (i < 200) SwitchToThread(); else Sleep(1);
        }
        { LARGE_INTEGER w1; QueryPerformanceCounter(&w1); gcm_wrap_qpc += w1.QuadPart - w0.QuadPart; gcm_wraps++; }
        st_u32(ctx + 4, end); st_u32(ctx + 8, next);
        RET(0);
        return;
    }
    if (begin_io != 0xFFFFFFFF) {
        st_u32(cur, begin_io | 1);                       /* JUMP begin */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        /* First let the processor reach the jump: put = the jump's own offset. Setting put = begin straight away
         * lost a whole ring of commands when a frame wrapped twice: the processor, still parked at begin from the
         * first wrap, saw get == put and skipped everything written since (frames over 2 MB of commands went
         * black, with most of their draws and state missing). */
        if (cur_io != 0xFFFFFFFF && ld_u32(GCM_CONTROL + 4) != cur_io) {
            st_u32(GCM_CONTROL, cur_io);
            for (int i = 0; i < 20000 && ld_u32(GCM_CONTROL + 4) != cur_io; i++) { if (i < 200) SwitchToThread(); else Sleep(1); }
        }
        st_u32(GCM_CONTROL, begin_io);                   /* put = begin: the processor follows the jump */
        for (int i = 0; i < 5000 && ld_u32(GCM_CONTROL + 4) != begin_io; i++) { if (i < 200) SwitchToThread(); else Sleep(1); }   /* drained */
    }
    { LARGE_INTEGER w1; QueryPerformanceCounter(&w1); gcm_wrap_qpc += w1.QuadPart - w0.QuadPart; gcm_wraps++; }   /* ([perf] lines) */
    st_u32(ctx + 8, begin);                              /* current = begin */
    RET(0);
}

volatile long fs_movies_open;   /* Bink movie files open */
volatile ULONGLONG nis_hold_until;
volatile ULONGLONG autodrive_file_tick;   /* MNR_AUTODRIVE_AFTER=<file>:<s>: when that file was opened (0: not yet) */   /* MNR_AUTOPRESS_NOSKIP: no automatic presses until this tick (an in-engine cutscene began) */
volatile LONGLONG fs_movie_read_qpc;   /* last read from one: a movie plays while it keeps streaming */
static volatile long lbl_acq, lbl_wait, lbl_rel, lbl_rel_1337;   /* MNR_LABEL_STATS: label 68 traffic per second */
static void label_stats(void) {
    static int on = -1; if (on < 0) on = getenv("MNR_LABEL_STATS") != NULL;
    if (!on) return;
    static LARGE_INTEGER t0, f; LARGE_INTEGER t; if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    if (t.QuadPart - t0.QuadPart >= f.QuadPart) {
        printf("[label68] per s: %ld acquires (%ld waited), %ld releases (%ld of 0x1337), flips %ld\n", lbl_acq, lbl_wait, lbl_rel, lbl_rel_1337, (long)gcm_flips);
        lbl_acq = lbl_wait = lbl_rel = lbl_rel_1337 = 0; t0 = t;
    }
}
static void gcm_method(uint32_t m, uint32_t arg, uint32_t *sem_off) {
    label_stats();
    switch (m) {
    case 0x0050: st_u32(GCM_CONTROL + 8, arg); break;                     /* NV406E_SET_REFERENCE */
    case 0x0064: case 0x1d6c: *sem_off = arg; break;                      /* SEMAPHORE_OFFSET */
    case 0x0068: {                                                        /* SEMAPHORE_ACQUIRE: wait until the label holds arg */
        if (*sem_off == 0x440) { lbl_acq++; if (ld_u32(GCM_LABELS + *sem_off) != arg) lbl_wait++; }
        { static int off = -1; if (off < 0) off = getenv("MNR_NO_SEM_ACQUIRE") != NULL; if (off) break; }   /* A/B testing */
        /* While a Bink movie plays, its flips go ahead: our player needs about two loop iterations per movie
         * frame (on the console Bink decodes asynchronously on the SPUs), so the 30 Hz wait slowed movies ~3x.
         * The game's own rendering stays locked at 30. */
        {   static int keep = -1; if (keep < 0) keep = getenv("MNR_MOVIE_WAIT") != NULL;
            LARGE_INTEGER q, qf; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&qf);
            if (!keep && fs_movies_open > 0 && (q.QuadPart - fs_movie_read_qpc) < 2 * qf.QuadPart) { st_u32(GCM_LABELS + *sem_off, arg); break; }   /* read in the last 2 s (Bink reads in bursts) */
        }
        { static int n; if (*sem_off != 0x10 && n++ < 12) printf("[gcm] semaphore acquire (flip %ld): label 0x%x = 0x%x (now 0x%x)\n", *sem_off, arg, ld_u32(GCM_LABELS + *sem_off)); }
        /* The game's flips wait on label 68 = 0x1337, which its vblank handler writes on every second vblank:
         * that is the console's 30 fps lock for gameplay (videos and loading flip without it). Bounded so a
         * label nobody writes can't hang the command processor. */
        LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        static int handlers = -1; if (handlers < 0) handlers = getenv("MNR_VBLANK_HANDLER") != NULL;
        if (!handlers && ld_u32(GCM_LABELS + *sem_off) != arg) {
            /* without the game's vblank handler, the label it would set on every second vblank is set here, on a
             * 30 Hz grid: the flip waits for the next tick, then goes ahead (videos and loading never wait) */
            /* MNR_FPS_CAP (the launcher's frame-rate setting) sets the grid instead: the game measures its frame
             * time, so at 60 it runs at normal speed with twice the frames (like its handler writing every vblank) */
            static LONGLONG fps = -1; if (fps < 0) { const char *e = getenv("MNR_FPS_CAP"); fps = e && atoi(e) > 0 ? atoi(e) : 30; }
            static LARGE_INTEGER next; LONGLONG tick = f.QuadPart / fps;
            if (!next.QuadPart || t0.QuadPart - next.QuadPart > tick) next.QuadPart = t0.QuadPart;
            for (;;) {
                QueryPerformanceCounter(&t); if (t.QuadPart >= next.QuadPart) break;
                if (rsx_lazy_req) rsx_d3d_lazy_service();
                rsx_cp_state = "semaphore acquire (30 Hz tick)";
                if ((next.QuadPart - t.QuadPart) * 1000 / f.QuadPart > 1) Sleep(1); else SwitchToThread();
            }
            next.QuadPart += tick; gcm_frame_ticked = 1;   /* (rsx.c wait_vblank: this frame is paced already) */
            st_u32(GCM_LABELS + *sem_off, arg);
        }
        while (ld_u32(GCM_LABELS + *sem_off) != arg) {
            if (rsx_lazy_req) rsx_d3d_lazy_service();
            QueryPerformanceCounter(&t);
            if ((t.QuadPart - t0.QuadPart) * 1000 / f.QuadPart > 200) {
                static int warned; if (warned++ < 5) printf("[gcm] semaphore acquire timed out: label 0x%x = 0x%x, waiting for 0x%x\n", *sem_off, ld_u32(GCM_LABELS + *sem_off), arg);
                break;
            }
            rsx_cp_state = "semaphore acquire";
            SwitchToThread();
        }
        break;
    }
    case 0x006c: case 0x1d74: if (*sem_off == 0x440) { lbl_rel++; if (arg == 0x1337) lbl_rel_1337++; } st_u32(GCM_LABELS + *sem_off, arg);  /* SEMAPHORE_RELEASE */
        WakeByAddressAll(gp(GCM_LABELS + *sem_off)); break;   /* a thread may sleep on the label (sys_timer_usleep below) */
    case 0x1d70:                                                          /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        st_u32(GCM_LABELS + *sem_off, (arg & 0xff00ff00) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff));
        WakeByAddressAll(gp(GCM_LABELS + *sem_off));
        break;
    }
}

/* MNR_GCM_CALLLOG=<from flip>: per flip, each command-buffer CALL with the draws (BEGIN_END) inside it, for 300 flips (debug) */
static long cl_from = -2, cl_flip = -1; static uint32_t cl_target[8]; static unsigned cl_draws[8], cl_top_draws;
static int cl_on(void) { if (cl_from == -2) { const char *e = getenv("MNR_GCM_CALLLOG"); cl_from = e ? atol(e) : -1; } return cl_from >= 0 && rsx_flips >= cl_from && rsx_flips < cl_from + 300; }
static unsigned cl_bytes;
static void cl_newflip(void) { if (cl_flip != rsx_flips) { if (cl_flip >= 0) printf("[gcmcall] flip %ld end: %u draws outside calls, %u command bytes\n", cl_flip, cl_top_draws, cl_bytes); cl_flip = rsx_flips; cl_top_draws = 0; cl_bytes = 0; } }
static void gcm_calllog_call(uint32_t target, uint32_t sp) { if (!cl_on()) return; cl_newflip(); if (sp >= 1 && sp <= 8) { cl_target[sp - 1] = target; cl_draws[sp - 1] = 0; } }
static void gcm_calllog_ret(uint32_t sp) { if (!cl_on() || !sp || sp > 8) return; cl_newflip(); printf("[gcmcall] flip %ld depth %u call 0x%x: %u draws\n", (long)rsx_flips, sp, cl_target[sp - 1], cl_draws[sp - 1]); }
static int gcm_cl_sp;
static void gcm_calllog_draw(void) { if (!cl_on()) return; cl_newflip(); if (gcm_cl_sp) cl_draws[gcm_cl_sp - 1]++; else cl_top_draws++; }
static void gcm_calllog_overflow(void) { static int n; if (n++ < 5) printf("[gcm] CALL nested deeper than 8\n"); }
volatile const char *rsx_cp_state = "idle";
static void gcm_cp_report(void) {
    fprintf(stderr, "-- RSX command processor: %s, get 0x%x put 0x%x ref 0x%x\n", rsx_cp_state, ld_u32(GCM_CONTROL + 4), ld_u32(GCM_CONTROL), ld_u32(GCM_CONTROL + 8));
}
static DWORD WINAPI gcm_thread(void *p) {
    thread_register(&cp_ctx_prof, "RSX command processor");   /* for the profiler and dumps */
    uint32_t sem_off = 0, call_stack[8], sp = 0;
    /* get is kept here and published to the control block only when the processor catches up with put, before a
     * semaphore wait, and every 64 packets: storing it after every packet shared a cache line with the game's put
     * writes (the two threads kept taking the line from each other) */
    uint32_t get = ld_u32(GCM_CONTROL + 4), pub = get, packets = 0;
    for (;;) {
        if (rsx_lazy_req) rsx_d3d_lazy_service();   /* another thread touched a surface the GPU owns */
        { uint32_t g = ld_u32(GCM_CONTROL + 4); if (g != pub) get = pub = g; }   /* get set from outside (a ring reset): follow it */
        uint32_t put = ld_u32(GCM_CONTROL);
        if (get != pub && (get == put || !(++packets & 63))) st_u32(GCM_CONTROL + 4, pub = get);
        {   /* new commands published: SPU jobs launched before them (their output is vertex data) must be done */
            static uint32_t last_put = 0xFFFFFFFF; static long need; static int spins;
            if (put != last_put) { last_put = put; need = spu_async_launched; }
            if (spu_async_done < need) {   /* spin a little, then sleep until a worker finishes one (at most 1 ms: lazy requests) */
                rsx_cp_state = "waiting for SPU jobs";
                LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
                if (spins++ < 64) SwitchToThread();
                else { long cur = spu_async_done; if (cur < need) WaitOnAddress((volatile void *)&spu_async_done, &cur, sizeof cur, 1); }
                QueryPerformanceCounter(&b); tl_rec("rsx waits for SPU jobs", a.QuadPart, b.QuadPart, 0, 0);
                continue;
            }
            spins = 0;
        }
        if (get == put) { rsx_cp_state = "idle (get == put)"; LARGE_INTEGER a, b; QueryPerformanceCounter(&a); Sleep(1); QueryPerformanceCounter(&b); tl_rec("rsx idle", a.QuadPart, b.QuadPart, 0, get); continue; }
        rsx_cp_state = "processing";
        uint32_t ea = gcm_io_to_ea(get);
        if (!ea) { fprintf(stderr, "[gcm] bad get offset 0x%x\n", get); Sleep(100); continue; }
        uint32_t cmd = ld_u32(ea);
        if ((cmd & 0xE0000003) == 0x20000000) { get = cmd & 0x1FFFFFFC; continue; }  /* old jump */
        if ((cmd & 3) == 1) { get = cmd & 0xFFFFFFFC; continue; }                    /* jump */
        if ((cmd & 3) == 2) { if (sp < 8) call_stack[sp++] = get + 4; else gcm_calllog_overflow(); gcm_calllog_call(cmd & 0xFFFFFFFC, sp); get = cmd & 0xFFFFFFFC; continue; }
        if (cmd == 0x00020000) { gcm_calllog_ret(sp); get = sp ? call_stack[--sp] : get + 4; continue; }  /* return */
        uint32_t count = (cmd >> 18) & 0x7FF, method = cmd & 0x1FFC, ninc = cmd & 0x40000000, sc = (cmd >> 13) & 7;
        gcm_cl_sp = (int)sp; if (cl_on()) { cl_newflip(); cl_bytes += 4 + 4 * count; }
        if ((method <= 0x0068 && method + 4 * count > 0x0068) || (cmd & 0xFFFC) == 0xFEAC) st_u32(GCM_CONTROL + 4, pub = get);   /* (a semaphore or flip wait may follow: the CPU sees how far we got) */
        for (uint32_t i = 0; i < count; i++) {
            uint32_t m = ninc ? method : method + 4 * i, arg = ld_u32(ea + 4 + 4 * i);
            if ((cmd & 0xFFFC) == 0xFEAC) { tl_mark("rsx flip", (uint32_t)rsx_flips); LARGE_INTEGER a, b; QueryPerformanceCounter(&a); rsx_flip(arg); QueryPerformanceCounter(&b); tl_rec("rsx flip (pacing + present)", a.QuadPart, b.QuadPart, 0, 0); continue; }
            if (m == 0x1808 && arg) gcm_calllog_draw();
            rsx_method(sc, m, arg);          /* first: surface write-backs at semaphores must land before the label the CPU waits on */
            if (m == 0x0068 && tl_state == 1) { LARGE_INTEGER a, b; QueryPerformanceCounter(&a); gcm_method(m, arg, &sem_off); QueryPerformanceCounter(&b); tl_rec("rsx semaphore acquire", a.QuadPart, b.QuadPart, 0, sem_off); }
            else gcm_method(m, arg, &sem_off);
        }
        get += 4 + 4 * count;
    }
    return 0;
}

static void gcm_init_body(ctx_t *c) {   /* _cellGcmInitBody(CellGcmContextData **ctx, cmdSize, ioSize, ioAddress) */
    uint32_t cmd_size = ARG32(1), io_size = ARG32(2), io_addr = ARG32(3);
    VirtualAlloc(guest_mem + RSX_LOCAL, 0x10000000, MEM_COMMIT, PAGE_READWRITE);
    gcm_io_base = io_addr; gcm_io_size = io_size;
    gcm_map(io_addr, 0, io_size);
    gcm_cb_opd = host_register(gcm_callback, "gcm_callback");
    uint32_t begin = io_addr + 0x1000;   /* first 4 KB of the IO area is reserved by libgcm */
    st_u32(GCM_CONTEXT + 0, begin);
    st_u32(GCM_CONTEXT + 4, begin + cmd_size - 0x1004);
    st_u32(GCM_CONTEXT + 8, begin);
    st_u32(GCM_CONTEXT + 12, gcm_cb_opd);
    st_u32(ARG32(0), GCM_CONTEXT);
    st_u32(GCM_CONTROL + 0, 0x1000); st_u32(GCM_CONTROL + 4, 0x1000); st_u32(GCM_CONTROL + 8, 0xFFFFFFFF);
    printf("[gcm] init: cmd 0x%x, io 0x%x bytes at 0x%08x\n", cmd_size, io_size, io_addr);
    CreateThread(NULL, 1 << 20, gcm_thread, NULL, 0, NULL);
    RET(0);
}
static void gcm_get_configuration(ctx_t *c) {   /* CellGcmConfig* */
    uint32_t p = ARG32(0);
    st_u32(p + 0, RSX_LOCAL); st_u32(p + 4, gcm_io_base);
    st_u32(p + 8, RSX_LOCAL_SIZE); st_u32(p + 12, gcm_io_size);
    st_u32(p + 16, 650000000); st_u32(p + 20, 500000000);
    RET(0);
}
static void gcm_map_main_memory(ctx_t *c) {   /* (ea, size, u32 *offset) */
    static uint32_t next_io = 0;
    if (!next_io) next_io = (gcm_io_size + 0xFFFFF) & ~0xFFFFFu;
    gcm_map(ARG32(0), next_io, ARG32(1));
    st_u32(ARG32(2), next_io);
    next_io += (ARG32(1) + 0xFFFFF) & ~0xFFFFFu;
    RET(0);
}
static void gcm_map_ea_io(ctx_t *c) { gcm_map(ARG32(0), ARG32(1), ARG32(2)); RET(0); }   /* (ea, io, size) */
static void gcm_address_to_offset(ctx_t *c) {   /* (address, u32 *offset) */
    uint32_t a = ARG32(0), off = 0;
    if (a >= RSX_LOCAL) off = a - RSX_LOCAL;
    else {
        if (!gcm_ea_to_io_fast(a, &off)) { RET(0x80210002); return; }   /* CELL_GCM_ERROR_FAILURE */
    }
    st_u32(ARG32(1), off);
    RET(0);
}
static void gcm_get_control_register(ctx_t *c) { RET(GCM_CONTROL); }
static void gcm_get_label_address(ctx_t *c) { RET(GCM_LABELS + 16 * (ARG32(0) & 255)); }
static void gcm_get_report_address(ctx_t *c) { RET(GCM_REPORTS + 16 * ARG32(0)); }   /* (index, location) */
/* VBlank and flip handlers (cellGcmSetVBlankHandler / cellGcmSetFlipHandler). The game paces itself with
 * them (frames end on every second vblank: 30 fps on the console); left uncalled, light scenes ran at our
 * 60 Hz flip cap. A host thread with its own guest stack calls the vblank handler at 60 Hz, and first the
 * flip handler once per flip the RSX thread completed (game code never runs on the RSX thread). Both get the
 * display head (1). Off by default (MNR_VBLANK_HANDLERS=1 to try): run from our thread, the flip handler
 * corrupted rendering mid-race (textures turned black, the HUD's stencil mask failed). The 30 fps lock comes
 * from the flip pacing instead (rsx.c wait_vblank, MNR_FPS_CAP). */
static volatile uint32_t gcm_vblank_opd, gcm_flip_opd;
static volatile long gcm_flips_done;   /* flips the RSX thread presented (rsx_flip -> gcm_flip_presented) */
void gcm_flip_presented(void) {
    InterlockedIncrement(&gcm_flips_done);
#ifdef G_FLIP_COUNT
    /* what the game's flip handler does (it isn't run: game code on another thread broke rendering): count
     * the flip and record its time, which the game measures its frame time from */
    static int fh = -1; if (fh < 0) fh = getenv("MNR_FLIP_HANDLER") != NULL;
    if (!fh) { st_u32(G_FLIP_COUNT, ld_u32(G_FLIP_COUNT) + 1); st_u64(G_FLIP_TIME, host_timebase()); }
#endif
}
static void vblank_call(ctx_t *c, uint32_t opd, uint64_t a0, uint32_t sp) {
    c->r[1] = sp; c->r[2] = ld_u32(opd + 4); c->r[3] = a0; c->lr = 0;
    dispatch(c, ld_u32(opd), 0);
}
static DWORD WINAPI vblank_thread(void *p) {
    (void)p;
    static ctx_t vctx;
    uint32_t stack = 0x10000, sp = gmalloc(stack + 0x100);
    uint32_t tls = gmalloc(0x8000 + tls_memsz);
    memcpy(gp(tls), gp(tls_va), tls_filesz); memset(gp(tls + tls_filesz), 0, tls_memsz - tls_filesz);
    vctx.r[13] = tls + 0x7030;
    vctx.thread_id = kobj_new(NULL, 0x01000000);
    thread_register(&vctx, "_gcm_intr_thread");
    LARGE_INTEGER f, next, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&next);
    LONGLONG period = f.QuadPart / 60; long flips_seen = 0;
    uint32_t top = (sp + stack - 0x70) & ~15u;
    for (;;) {
        next.QuadPart += period;
        for (;;) { QueryPerformanceCounter(&now); LONGLONG left = next.QuadPart - now.QuadPart; if (left <= 0) break;
                   if (left * 1000 / f.QuadPart > 1) Sleep(1); else SwitchToThread(); }
        if (now.QuadPart - next.QuadPart > 4 * period) next = now;   /* fell far behind (debugger, load): don't burst */
        long done = gcm_flips_done;
        static int flip_cb = -1; if (flip_cb < 0) flip_cb = getenv("MNR_FLIP_HANDLER") != NULL;   /* normally done at the flip in host code (gcm_flip_presented) */
        if (flip_cb && gcm_flip_opd && done != flips_seen) { flips_seen = done; vblank_call(&vctx, gcm_flip_opd, 1, top); }
        else flips_seen = done;
        if (gcm_vblank_opd) vblank_call(&vctx, gcm_vblank_opd, 1, top);
    }
    return 0;
}
static void gcm_start_vblank_thread(void) {
    static volatile long started;
    if (!getenv("MNR_VBLANK_HANDLER") || InterlockedExchange(&started, 1)) return;   /* off by default: flicker at boot */   /* experimental, off by default: calling them broke rendering mid-race */
    CreateThread(NULL, 1 << 20, vblank_thread, NULL, 0, NULL);
}
static void gcm_set_vblank_handler(ctx_t *c) {   /* (void (*handler)(u32 head)) */
    gcm_vblank_opd = ARG32(0);
    printf("[gcm] vblank handler 0x%08x (code 0x%08x)\n", ARG32(0), ARG32(0) ? ld_u32(ARG32(0)) : 0);
    if (ARG32(0)) gcm_start_vblank_thread();
    RET(0);
}
static void gcm_set_flip_handler(ctx_t *c) {   /* (void (*handler)(u32 head)) */
    gcm_flip_opd = ARG32(0);
    printf("[gcm] flip handler 0x%08x (code 0x%08x)\n", ARG32(0), ARG32(0) ? ld_u32(ARG32(0)) : 0);
    if (ARG32(0)) gcm_start_vblank_thread();
    RET(0);
}
static void gcm_set_flip_mode(ctx_t *c) { printf("[gcm] flip mode %u\n", ARG32(0)); RET(0); }   /* 1 = vsync, 2 = hsync */
static void gcm_get_tiled_pitch(ctx_t *c) {
    static const uint32_t pitches[] = {0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0xA00, 0xC00, 0xD00, 0xE00,
                                       0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
                                       0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000};
    for (unsigned i = 0; i < sizeof pitches / 4; i++) if (pitches[i] >= ARG32(0)) { RET(pitches[i]); return; }
    RET(0);
}
/* [perf] lines (always on): every 30 s and for the whole session at exit - fps, slow frames, GPU time per frame,
 * time blocked in Present (GPU backlog), and the busiest guest threads by CPU time - so any machine's log shows its bottleneck */
typedef struct { uint32_t hist[256]; long frames; double t0, gpu, pres; long gpuf; uint64_t thr[128], proc; long shc, shus, shskip, shwait; } perf_win_t;
static perf_win_t perf_w, perf_s;   /* window, session */
static SRWLOCK perf_lock = SRWLOCK_INIT;
static double perf_now(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / f.QuadPart; }
static uint64_t ft64(FILETIME a) { return ((uint64_t)a.dwHighDateTime << 32) | a.dwLowDateTime; }
static uint64_t thr_cpu(HANDLE h) { FILETIME c, e, k, u; return h && GetThreadTimes(h, &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0; }
static uint64_t proc_cpu(void) { FILETIME c, e, k, u; return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0; }
static void perf_start(perf_win_t *w, double now) {
    memset(w->hist, 0, sizeof w->hist); w->frames = 0; w->t0 = now;
    w->gpu = rsx_d3d_gpu_ms; w->gpuf = rsx_d3d_gpu_frames; w->pres = rsx_d3d_present_ms; w->proc = proc_cpu();
    w->shc = rsx_d3d_sh_compiles; w->shus = rsx_d3d_sh_compile_us; w->shskip = rsx_d3d_sh_skipped; w->shwait = rsx_d3d_sh_waited;
    for (long t = 0; t < treg_count && t < 128; t++) w->thr[t] = thr_cpu(tregs[t].h);
}
static double perf_pct_ms(const perf_win_t *w, double frac) {   /* frame time (ms) that frac of the frames stay under */
    long want = (long)(w->frames * frac), n = 0;
    for (int i = 0; i < 256; i++) { n += w->hist[i]; if (n > want) return i + 0.5; }
    return 255;
}
static void perf_report(perf_win_t *w, double now, const char *what) {
    double secs = now - w->t0; if (secs <= 0 || w->frames < 2) return;
    long slow = 0; for (int i = 50; i < 256; i++) slow += w->hist[i];
    SYSTEM_INFO si; GetSystemInfo(&si);
    char busy[512]; int bn = 0;
    {   /* guest threads by name (the job workers share one), busiest first */
        struct { const char *name; double pct; int n; } g[32]; int ng = 0;
        for (long t = 0; t < treg_count && t < 128; t++) {
            double p = (double)(thr_cpu(tregs[t].h) - w->thr[t]) / (secs * 1e5);   /* 100 ns units -> % of one core */
            const char *nm = tregs[t].name ? tregs[t].name : "?"; int k = 0;
            while (k < ng && strcmp(g[k].name, nm)) k++;
            if (k == ng) { if (ng == 32) continue; g[ng].name = nm; g[ng].pct = 0; g[ng].n = 0; ng++; }
            g[k].pct += p; g[k].n++;
        }
        for (int shown = 0; shown < 5 && ng; shown++) {
            int b = 0; for (int k = 1; k < ng; k++) if (g[k].pct > g[b].pct) b = k;
            if (g[b].pct < 3) break;
            bn += snprintf(busy + bn, sizeof busy - bn, "%s%s", shown ? ", " : "", g[b].name);
            if (g[b].n > 1) bn += snprintf(busy + bn, sizeof busy - bn, " x%d", g[b].n);
            bn += snprintf(busy + bn, sizeof busy - bn, " %.0f%%", g[b].pct);
            g[b] = g[--ng];
        }
    }
    printf("[perf] %s %.0f s: %.1f fps (1%% low %.1f), %ld frames over 50 ms | Present %.1f ms/frame | "
           "CPU %.0f%% of %lu cores | busiest: %s\n",
           what, secs, w->frames / secs, 1000.0 / perf_pct_ms(w, 0.99), slow,
           (rsx_d3d_present_ms - w->pres) / w->frames,
           (double)(proc_cpu() - w->proc) / (secs * 1e5 * si.dwNumberOfProcessors),
           (unsigned long)si.dwNumberOfProcessors, bn ? busy : "-");
    {   /* shader compiles (not disk-cache hits) in the window, and the draws they skipped or held up */
        long n = rsx_d3d_sh_compiles - w->shc, us = rsx_d3d_sh_compile_us - w->shus, sk = rsx_d3d_sh_skipped - w->shskip, wt = rsx_d3d_sh_waited - w->shwait;
        static long w0; static LONGLONG q0; LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        if (gcm_wraps != w0) printf("[perf]   command ring: %ld segment switches, CPU waited %.0f ms for the GPU\n", gcm_wraps - w0, (gcm_wrap_qpc - q0) * 1000.0 / f.QuadPart);
        if (!strcmp(what, "last")) { w0 = gcm_wraps; q0 = gcm_wrap_qpc; }
        if (n || sk || wt) printf("[perf]   shaders: %ld compiled (%.0f ms), %ld draws skipped while compiling, %ld draws waited\n", n, us / 1000.0, sk, wt);
    }
}
void perf_frame(void) {   /* each displayed frame (present.c): frame times as seen on screen */
    static double last;
    double now = perf_now();
    AcquireSRWLockExclusive(&perf_lock);
    if (!last) { perf_start(&perf_w, now); perf_start(&perf_s, now); }
    else {
        int ms = (int)((now - last) * 1000.0); if (ms > 255) ms = 255;
        perf_w.hist[ms]++; perf_w.frames++; perf_s.hist[ms]++; perf_s.frames++;
        if (now - perf_w.t0 >= 30) { perf_report(&perf_w, now, "last"); perf_start(&perf_w, now); }
    }
    last = now;
    ReleaseSRWLockExclusive(&perf_lock);
}
static void perf_exit(void) { AcquireSRWLockExclusive(&perf_lock); perf_report(&perf_s, perf_now(), "session"); ReleaseSRWLockExclusive(&perf_lock); fflush(stdout); }

static void watch_init(void);
static void gcm_set_flip_command(ctx_t *c) {   /* _cellGcmSetFlipCommand(ctx, id): queue a flip in the stream */
    { static long wat = -2; if (wat == -2) { const char *e = getenv("MNR_WATCH_AT"); wat = e ? atol(e) : -1; }
      if (wat >= 0 && gcm_flips == wat) watch_init(); }
    uint32_t ctx = ARG32(0), cur = ld_u32(ctx + 8);
    if (cur + 8 > ld_u32(ctx + 4)) {                     /* no room: wrap like the context callback */
        ctx_t tmp = *c; tmp.r[3] = ctx; gcm_callback(&tmp);
        cur = ld_u32(ctx + 8);
    }
    st_u32(cur, (1u << 18) | 0xFEAC);                    /* GCM flip method, 1 argument */
    st_u32(cur + 4, ARG32(1));
    st_u32(ctx + 8, cur + 8);
    long n = InterlockedIncrement(&gcm_flips);
    {   static LARGE_INTEGER f0, fq; if (!f0.QuadPart) { QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&f0); }
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        if (n == 1 || n % 300 == 0) printf("[gcm] flip #%ld (buffer %u) at %.1f s\n", n, ARG32(1), (double)(t.QuadPart - f0.QuadPart) / fq.QuadPart); }
    {   /* MNR_MATHPROBE=<hex fn>: once, call that guest float function f(x) for a few x and print the results (debug) */
        static int done; const char *list = getenv("MNR_MATHPROBE");
        for (char *e = (char *)list; e && *e && !done && gcm_flips > 2000; ) {
            long fn = (long)strtoul(e, &e, 16); if (*e == ',') e++;
            printf("[mathprobe] function 0x%08lx\n", fn);
            static const double xs[] = {0, 0.5, 1, 1.5707963, 2, 3.1415927, -1, 10, 0.25};
            for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
                ctx_t t = *c; t.f[1].d = xs[i]; t.r[1] -= 1024; t.lr = 0;
                dispatch(&t, (uint32_t)fn, 0);
                printf("[mathprobe] f(%g) = %.7g   (sin %.7g cos %.7g tan %.7g exp %.7g log %.7g sqrt %.7g atan %.7g)\n", xs[i], t.f[1].d,
                       sin(xs[i]), cos(xs[i]), tan(xs[i]), exp(xs[i]), log(xs[i]), sqrt(xs[i]), atan(xs[i]));
            }
            if (!*e) done = 1;
        }
    }
    {   /* MNR_DUMPMEM=<flip>:<hex addr>:<hex len>[;...]: debug dump of guest memory at that flip */
        static const char *dm = (const char *)-1;
        if (dm == (const char *)-1) dm = getenv("MNR_DUMPMEM");
        for (const char *p = dm; p && *p; ) {
            char *e; long fl = strtol(p, &e, 10); uint32_t a = 0, l = 0;
            if (*e == ':') a = (uint32_t)strtoul(e + 1, &e, 16);
            if (*e == ':') l = (uint32_t)strtoul(e + 1, &e, 16);
            if (fl == n)
                for (uint32_t i = 0; i < l; i += 32) {
                    printf("[mem] %08x", a + i);
                    for (int k = 0; k < 8; k++) printf(" %08x", ld_u32(a + i + 4 * k));
                    printf("\n");
                }
            p = *e == ';' ? e + 1 : NULL;
        }
    }
    RET(0);
}
static void gcm_set_display_buffer(ctx_t *c) {   /* (id, offset, pitch, width, height) */
    uint32_t id = ARG32(0) & 7;
    rsx_display[id].offset = ARG32(1); rsx_display[id].pitch = ARG32(2);
    rsx_display[id].width = ARG32(3); rsx_display[id].height = ARG32(4);
    printf("[gcm] display buffer %u: offset 0x%x pitch %u %ux%u\n", id, ARG32(1), ARG32(2), ARG32(3), ARG32(4));
    RET(0);
}
uint32_t gcm_io_to_ea_pub(uint32_t off) { return gcm_io_to_ea(off); }
/* main-memory ranges the RSX can reach (cellGcmMapMainMemory / MapEaIoAddress), merged: with RSX local memory, the only
 * memory the GPU reads, so the only memory whose CPU writes the renderer has to notice */
int gcm_io_regions(uint32_t *ea, uint32_t *size, int max) {
    int n = 0;
    for (int i = 0; i < 4096; i++) {
        uint32_t e = gcm_io_map[i];
        if (!e) continue;
        int merged = 0;
        for (int k = 0; k < n; k++) if (ea[k] + size[k] == e) { size[k] += 0x100000; merged = 1; break; }
        if (!merged && n < max) { ea[n] = e; size[n] = 0x100000; n++; }
    }
    return n;
}

/* ------------------------------------------------------------------ SPURS lock-free queues (PPU side)
 * The game's qTaskManager worker threads pop {cmd, arg} records from these and run PPU task
 * functions directly, so a host-side FIFO per guest queue address is enough for PPU tasks. */
typedef struct {
    CRITICAL_SECTION cs; CONDITION_VARIABLE not_empty, not_full;
    uint32_t size, depth, head, count; uint8_t *data; int init;
} lfq_t;
static lfq_t *lfq_get(uint32_t q) { return aobj_get(q, sizeof(lfq_t)); }
static void spurs_lfq_init(ctx_t *c) {   /* (spurs/taskset, CellSpursLFQueue *q, buffer, size, depth, direction) */
    lfq_t *q = lfq_get(ARG32(1));
    if (!q->init) { InitializeCriticalSection(&q->cs); InitializeConditionVariable(&q->not_empty); InitializeConditionVariable(&q->not_full); }
    q->size = ARG32(3); q->depth = ARG32(4) ? ARG32(4) : 1;
    q->data = calloc(q->depth, q->size); q->head = q->count = 0; q->init = 1;
    RET(0);
}
static void spurs_lfq_push(ctx_t *c) {   /* (q, buffer, isBlocking) */
    lfq_t *q = lfq_get(ARG32(0));
    if (!q->init) { RET(0x80410902); return; }   /* CELL_SPURS_TASK_ERROR_INVAL-ish */
    EnterCriticalSection(&q->cs);
    while (q->count == q->depth) {
        if (!ARG32(2)) { LeaveCriticalSection(&q->cs); RET(0x80410901); return; }   /* EAGAIN */
        SleepConditionVariableCS(&q->not_full, &q->cs, INFINITE);
    }
    memcpy(q->data + ((q->head + q->count) % q->depth) * q->size, gp(ARG32(1)), q->size);
    q->count++;
    WakeConditionVariable(&q->not_empty);
    LeaveCriticalSection(&q->cs);
    RET(0);
}
/* hook for the qTask SPU launcher (0x7A7CD0): (table, slot, spu job, task, ppu queue, manager, 0).
 * Instead of starting an SPU task, queue the task for the PPU workers exactly like the manager does
 * for tasks without an SPU job ({0, task} record); the worker then runs the task's PPU function. */
static int qtask_log = -1;

/* ------------------------------------------------------------------ cellPngEnc / cellJpgEnc over imgenc.c */
/* The game encodes creation thumbnails (PNG) and photos (JPEG) with these; without them the Creation Station
 * studio sat on its loading screen. Encoding happens synchronously in EncodePicture; the Wait calls return at once. */
#include "imgenc.h"
typedef struct { int used, done; uint32_t state, loc, name, addr, limit, size, lines; uint64_t user; } imgenc_t;
static imgenc_t imgencs[16];
static imgenc_t *imgenc_of(uint32_t h) { return h >= 1 && h <= 16 && imgencs[h - 1].used ? &imgencs[h - 1] : NULL; }
static void enc_query_attr(ctx_t *c) {   /* (config*, attr* {u32 memSize, u8 cmdQueueDepth, u32 versionUpper, versionLower}) */
    uint32_t a = ARG32(1);
    st_u32(a, 0x10000); st_u8(a + 4, 1); st_u32(a + 8, 0x00420000); st_u32(a + 12, 0);
    RET(0);
}
static void enc_open(ctx_t *c) {   /* (config*, resource[Ex]*, u32 *handle) */
    for (int i = 0; i < 16; i++) if (!imgencs[i].used) {
        memset(&imgencs[i], 0, sizeof imgencs[i]); imgencs[i].used = 1;
        st_u32(ARG32(2), (uint32_t)i + 1); RET(0); return;
    }
    RET(0x80611292);
}
static void enc_close(ctx_t *c) { imgenc_t *e = imgenc_of(ARG32(0)); if (e) e->used = 0; RET(0); }
static void enc_wait_input(ctx_t *c) { RET(imgenc_of(ARG32(0)) ? 0 : 0x80611292); }
static void enc_wait_output(ctx_t *c) {   /* (handle, u32 *streamInfoNum, b8 block) */
    imgenc_t *e = imgenc_of(ARG32(0));
    if (!e) { RET(0x80611292); return; }
    if (ARG32(1)) st_u32(ARG32(1), e->done ? 1 : 0);
    RET(e->done ? 0 : 0x80611294);   /* ..._ERROR_EMPTY: nothing encoded yet */
}
/* guest picture -> RGBA8. fmt: 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA, 5 ARGB */
static uint8_t *enc_fetch(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, int fmt) {
    static const int bpp[6] = {0, 1, 2, 3, 4, 4};
    uint8_t *o = malloc((size_t)w * h * 4);
    if (!o) return NULL;
    if (!pitch) pitch = w * bpp[fmt];
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = gp(addr + y * pitch); uint8_t *d = o + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, s += bpp[fmt], d += 4)
            switch (fmt) {
            case 1: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
            case 2: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            case 3: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
            case 4: memcpy(d, s, 4); break;
            default: d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0]; break;
            }
    }
    return o;
}
static void enc_store(imgenc_t *e, uint32_t out, uint8_t *data, size_t n, uint32_t h, uint64_t user) {   /* outputParam {location, fileName, streamAddr, limitSize} */
    e->loc = ld_u32(out); e->name = ld_u32(out + 4); e->addr = ld_u32(out + 8); e->limit = ld_u32(out + 12);
    e->state = 0; e->size = 0; e->lines = h; e->user = user; e->done = 1;
    if (!data) { e->state = 0x80611293; return; }
    if (e->loc == 1) {   /* buffer */
        if (n > e->limit) { e->state = 0x80611293; printf("[imgenc] %zu-byte stream over the 0x%x limit\n", n, e->limit); }
        else { memcpy(gp(e->addr), data, n); e->size = (uint32_t)n; }
    } else {             /* file */
        char hp[MAX_PATH]; FILE *f = NULL;
        if (e->name && fs_host_path((const char *)gp(e->name), hp, sizeof hp)) f = fopen(hp, "wb");
        if (f) { fwrite(data, 1, n, f); fclose(f); e->size = (uint32_t)n; } else e->state = 0x80611293;
    }
    free(data);
}
static int enc_fmt(uint32_t cs, int fallback_bpp) {   /* colour-space enum -> enc_fetch format */
    switch (cs) {
    case 20: case 32: return 5;   /* ARGB */
    case 10: case 16: case 6: return 4;   /* RGBA */
    case 2: case 3: return 3;     /* RGB (YCbCr input treated as RGB) */
    case 8: return 2;
    case 1: case 4: return 1;     /* grayscale */
    }
    return fallback_bpp >= 4 ? 5 : fallback_bpp == 3 ? 3 : fallback_bpp == 2 ? 2 : 1;
}
static void pngenc_encode(ctx_t *c) {   /* (handle, picture*, encodeParam*, outputParam*) */
    imgenc_t *e = imgenc_of(ARG32(0));
    if (!e) { RET(0x80611292); return; }
    uint32_t p = ARG32(1), w = ld_u32(p), h = ld_u32(p + 4), pitch = ld_u32(p + 8), cs = ld_u32(p + 12), addr = ld_u32(p + 24);
    int fmt = enc_fmt(cs, w ? (int)(pitch / w) : 4);
    uint32_t ocs = ARG32(2) ? ld_u32(ARG32(2) + 4) : 0;
    int och = ocs == 2 ? 3 : (ocs == 20 || ocs == 32 || ocs == 16 || ocs == 6 || ocs == 10) ? 4 : ocs == 8 ? 2 : (ocs == 1 || ocs == 4) ? 1
            : (fmt == 5 ? 4 : fmt);
    {   /* the picture is often a render target (the player's avatar): bring guest memory up to date first */
        uint32_t bytes = (pitch ? pitch : w * 4) * h;
        uint64_t before = 0; for (uint32_t i = 0; i < bytes; i += 64) before += ld_u32(addr + i);
        rsx_d3d_flush_from = 5; rsx_d3d_flush_range(addr, bytes);
        uint64_t after = 0; for (uint32_t i = 0; i < bytes; i += 64) after += ld_u32(addr + i);
        printf("[imgenc] source 0x%08x pitch %u%s\n", addr, pitch, before != after ? " (updated from the GPU)" : "");
    }
    uint8_t *rgba = enc_fetch(addr, w, h, pitch, fmt); size_t n = 0;
    uint8_t *s = rgba ? png_encode(rgba, w, h, och, &n) : NULL;
    if (s && getenv("MNR_IMGENC_DUMP")) {   /* debug: the picture as read from memory, next to the surfaces dumped by the flush */
        static int k; char nm[64]; snprintf(nm, sizeof nm, "enc_%02d_mem_%08x_%ux%u.png", k++, addr, w, h);
        FILE *f = fopen(nm, "wb"); if (f) { fwrite(s, 1, n, f); fclose(f); }
    }
    free(rgba);
    printf("[imgenc] png %ux%u (cs %u -> %u) -> %zu bytes\n", w, h, cs, ocs, n);
    enc_store(e, ARG32(3), s, n, h, ld_u64(p + 32));
    RET(0);
}
static void jpgenc_encode(ctx_t *c) {   /* (handle, picture2* {w, h, pitch, colorSpace, addr}, encodeParam*, outputParam*) */
    imgenc_t *e = imgenc_of(ARG32(0));
    if (!e) { RET(0x80611292); return; }
    uint32_t p = ARG32(1), w = ld_u32(p), h = ld_u32(p + 4), pitch = ld_u32(p + 8), cs = ld_u32(p + 12), addr = ld_u32(p + 16);
    int q = ARG32(2) ? (int)ld_u32(ARG32(2) + 16) : 85;
    uint8_t *rgba = enc_fetch(addr, w, h, pitch, enc_fmt(cs, w ? (int)(pitch / w) : 4)); size_t n = 0;
    uint8_t *s = rgba ? jpg_encode(rgba, w, h, q, &n) : NULL;
    free(rgba);
    printf("[imgenc] jpeg %ux%u q%d -> %zu bytes\n", w, h, q, n);
    enc_store(e, ARG32(3), s, n, h, 0);
    RET(0);
}
static void enc_stream_info(ctx_t *c, int with_user) {   /* (handle, streamInfo* {state, location, fileName, streamAddr, limitSize, streamSize, processedLine, [u64 userData]}) */
    imgenc_t *e = imgenc_of(ARG32(0)); uint32_t i = ARG32(1);
    if (!e) { RET(0x80611292); return; }
    st_u32(i, e->state); st_u32(i + 4, e->loc); st_u32(i + 8, e->name); st_u32(i + 12, e->addr);
    st_u32(i + 16, e->limit); st_u32(i + 20, e->size); st_u32(i + 24, e->lines);
    if (with_user) st_u64(i + 32, e->user);
    e->done = 0;
    RET(0);
}
static void pngenc_stream_info(ctx_t *c) { enc_stream_info(c, 1); }
static void jpgenc_stream_info(ctx_t *c) { enc_stream_info(c, 0); }

/* ------------------------------------------------------------------ cellPngDec over png.c */
#include "png.h"
typedef struct { uint8_t *file; size_t size; png_info_t info; uint32_t out_cs, alpha_sel, alpha_val; int used; } pngsub_t;
static pngsub_t png_subs[64];
static void png_create(ctx_t *c) {   /* (u32 *mainHandle, threadInParam*, threadOutParam* {u32 version}) */
    st_u32(ARG32(0), 0x504E4701);
    if (ARG32(2)) st_u32(ARG32(2), 0x00420000);
    RET(0);
}
static void png_open(ctx_t *c) {   /* (main, u32 *subHandle, CellPngDecSrc*, openInfo*) */
    uint32_t src = ARG32(2), sel = ld_u32(src);
    int slot = -1;
    for (int i = 0; i < 64; i++) if (!png_subs[i].used) { slot = i; break; }
    if (slot < 0) { RET(0x80611202); return; }   /* CELL_PNGDEC_ERROR_BUSY-ish */
    pngsub_t *p = &png_subs[slot];
    memset(p, 0, sizeof *p);
    if (sel == 0) {   /* CELL_PNGDEC_FILE: {sel, fileName, s64 fileOffset, u32 fileSize, ...} */
        char hp[MAX_PATH];
        const char *g = (const char *)gp(ld_u32(src + 4));
        if (!fs_host_path_read(g, hp, sizeof hp)) { RET(0x80611201); return; }
        FILE *f = fopen(hp, "rb");
        if (!f) { RET(0x80611201); return; }
        uint64_t off = ld_u64(src + 8); uint32_t sz = ld_u32(src + 16);
        if (!sz) { fseek(f, 0, SEEK_END); sz = (uint32_t)(ftell(f) - (long)off); }
        fseek(f, (long)off, SEEK_SET);
        p->file = malloc(sz); p->size = fread(p->file, 1, sz, f);
        fclose(f);
    } else {          /* CELL_PNGDEC_BUFFER: streamPtr at +20, streamSize at +24 */
        uint32_t ptr = ld_u32(src + 20), sz = ld_u32(src + 24);
        p->file = malloc(sz); memcpy(p->file, gp(ptr), sz); p->size = sz;
    }
    if (png_header(p->file, p->size, &p->info)) { free(p->file); RET(0x80611201); return; }
    p->used = 1; p->out_cs = 10;
    st_u32(ARG32(1), (uint32_t)slot + 1);
    if (ARG32(3)) st_u32(ARG32(3), 0);
    RET(0);
}
static pngsub_t *png_sub(uint32_t h) { return h >= 1 && h <= 64 && png_subs[h - 1].used ? &png_subs[h - 1] : NULL; }
static void png_read_header(ctx_t *c) {   /* (main, sub, CellPngDecInfo*) */
    pngsub_t *p = png_sub(ARG32(1));
    if (!p) { RET(0x80611201); return; }
    static const uint32_t comps[7] = {1, 0, 3, 1, 2, 0, 4}, cs[7] = {1, 0, 2, 4, 9, 0, 10};
    int ct = p->info.color_type <= 6 ? p->info.color_type : 6;
    uint32_t o = ARG32(2);
    st_u32(o, p->info.width); st_u32(o + 4, p->info.height); st_u32(o + 8, comps[ct]);
    st_u32(o + 12, cs[ct]); st_u32(o + 16, (uint32_t)p->info.bit_depth); st_u32(o + 20, (uint32_t)p->info.interlace); st_u32(o + 24, 0);
    RET(0);
}
static uint32_t png_cs_comps(uint32_t cs) { return cs == 1 ? 1 : cs == 2 ? 3 : cs == 9 ? 2 : cs == 4 ? 1 : 4; }
static void png_set_parameter(ctx_t *c) {   /* (main, sub, inParam*, outParam*) */
    pngsub_t *p = png_sub(ARG32(1));
    if (!p) { RET(0x80611201); return; }
    uint32_t in = ARG32(2), out = ARG32(3);
    p->out_cs = ld_u32(in + 8); p->alpha_sel = ld_u32(in + 20); p->alpha_val = ld_u32(in + 24);
    uint32_t comps = png_cs_comps(p->out_cs);
    st_u64(out, (uint64_t)p->info.width * comps);
    st_u32(out + 8, p->info.width); st_u32(out + 12, p->info.height); st_u32(out + 16, comps);
    st_u32(out + 20, 8); st_u32(out + 24, ld_u32(in + 4)); st_u32(out + 28, p->out_cs); st_u32(out + 32, 0);
    RET(0);
}
static void png_decode_data(ctx_t *c) {   /* (main, sub, u8 *data, ctrl {u64 bytesPerLine}*, outInfo*) */
    pngsub_t *p = png_sub(ARG32(1));
    if (!p) { RET(0x80611201); return; }
    png_info_t info;
    uint8_t *rgba = png_decode_rgba(p->file, p->size, &info);
    uint32_t out_info = ARG32(4);
    if (out_info) mem_zero(out_info, 16);
    if (!rgba) { if (out_info) st_u32(out_info + 12, 1); RET(0x80611201); return; }   /* status: STOP */
    uint32_t comps = png_cs_comps(p->out_cs), w = info.width;
    uint64_t bpl = ARG32(3) ? ld_u64(ARG32(3)) : 0;
    if (!bpl) bpl = (uint64_t)w * comps;
    for (uint32_t y = 0; y < info.height; y++) {
        uint8_t *dst = gp(ARG32(2) + (uint32_t)(y * bpl));
        const uint8_t *s = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, s += 4) {
            uint8_t a = p->alpha_sel ? (uint8_t)p->alpha_val : s[3];
            switch (p->out_cs) {
            case 1: case 4: dst[x] = s[0]; break;
            case 2: dst[3 * x] = s[0]; dst[3 * x + 1] = s[1]; dst[3 * x + 2] = s[2]; break;
            case 9: dst[2 * x] = s[0]; dst[2 * x + 1] = a; break;
            case 20: dst[4 * x] = a; dst[4 * x + 1] = s[0]; dst[4 * x + 2] = s[1]; dst[4 * x + 3] = s[2]; break;
            default: dst[4 * x] = s[0]; dst[4 * x + 1] = s[1]; dst[4 * x + 2] = s[2]; dst[4 * x + 3] = a; break;
            }
        }
    }
    free(rgba);
    {   static int tl = -1; if (tl < 0) tl = getenv("MNR_THUMB_LOG") != NULL;   /* debug: where decoded pictures go */
        if (tl) printf("[thumb] png %ux%u cs %u -> 0x%08x (%llu bytes/line) flip %ld\n", info.width, info.height, p->out_cs, ARG32(2), (unsigned long long)bpl, (long)rsx_flips); }
    if (out_info) st_u32(out_info + 12, 0);   /* CELL_PNGDEC_DEC_STATUS_FINISH */
    RET(0);
}
static void png_close(ctx_t *c) {
    pngsub_t *p = png_sub(ARG32(1));
    if (p) { free(p->file); p->used = 0; }
    RET(0);
}

/* SPU DXT compression job, launched from 0x7A340C (composited character textures during loading).
 * Job block: [0] source A8R8G8B8 pixels, [4] destination, [8] source pitch (width * 4), [12] number of
 * 4x4 blocks, [16] mode 0 DXT1 / 1 DXT3 / 2 DXT5. The PPU waits for the word at r7 to become non-zero. */
static void dxt_color_block(const uint8_t px[16][4], uint8_t *out) {   /* px: r, g, b, a */
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) for (int k = 0; k < 3; k++) { if (px[i][k] < mn[k]) mn[k] = px[i][k]; if (px[i][k] > mx[k]) mx[k] = px[i][k]; }
    uint16_t c0 = (uint16_t)((mx[0] >> 3) << 11 | (mx[1] >> 2) << 5 | (mx[2] >> 3));
    uint16_t c1 = (uint16_t)((mn[0] >> 3) << 11 | (mn[1] >> 2) << 5 | (mn[2] >> 3));
    uint32_t idx = 0;
    if (c0 == c1) { if (c0) c1 = c0 - 1; else c0 = 1; }
    if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    float e0[3] = {(float)((c0 >> 11) * 255 / 31), (float)(((c0 >> 5) & 63) * 255 / 63), (float)((c0 & 31) * 255 / 31)};
    float e1[3] = {(float)((c1 >> 11) * 255 / 31), (float)(((c1 >> 5) & 63) * 255 / 63), (float)((c1 & 31) * 255 / 31)};
    float pal[4][3];
    for (int k = 0; k < 3; k++) { pal[0][k] = e0[k]; pal[1][k] = e1[k]; pal[2][k] = (2 * e0[k] + e1[k]) / 3; pal[3][k] = (e0[k] + 2 * e1[k]) / 3; }
    for (int i = 0; i < 16; i++) {
        int best = 0; float bd = 1e30f;
        for (int p = 0; p < 4; p++) {
            float d = 0;
            for (int k = 0; k < 3; k++) { float t = px[i][k] - pal[p][k]; d += t * t; }
            if (d < bd) { bd = d; best = p; }
        }
        idx |= (uint32_t)best << (2 * i);
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)idx; out[5] = (uint8_t)(idx >> 8); out[6] = (uint8_t)(idx >> 16); out[7] = (uint8_t)(idx >> 24);
}
static void dxt5_alpha_block(const uint8_t px[16][4], uint8_t *out) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) { if (px[i][3] < mn) mn = px[i][3]; if (px[i][3] > mx) mx = px[i][3]; }
    out[0] = (uint8_t)mx; out[1] = (uint8_t)mn;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int a = px[i][3], code;
        if (mx == mn) code = 0;
        else {   /* 8-level palette: 0 = max, 1 = min, 2..7 = interpolated from max to min */
            int best = 0, bd = 1 << 30;
            for (int p = 0; p < 8; p++) {
                int v = p == 0 ? mx : p == 1 ? mn : ((8 - p) * mx + (p - 1) * mn) / 7;
                int d = (a - v) * (a - v);
                if (d < bd) { bd = d; best = p; }
            }
            code = best;
        }
        bits |= (uint64_t)code << (3 * i);
    }
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}
static void spu_job_dxt_compress(uint32_t job, uint32_t done) {
    uint32_t src = ld_u32(job), dst = ld_u32(job + 4), pitch = ld_u32(job + 8), blocks = ld_u32(job + 12), mode = ld_u32(job + 16);
    uint32_t w = pitch / 4, bw = w / 4;
    if (!src || !dst || !bw) { if (done) st_u32(done, 1); return; }
    uint32_t bs = mode == 0 ? 8 : 16;
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t bx = b % bw, by = b / bw;
        uint8_t px[16][4];
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint32_t a = src + (by * 4 + (uint32_t)y) * pitch + (bx * 4 + (uint32_t)x) * 4;   /* A R G B */
            px[y * 4 + x][3] = ld_u8(a); px[y * 4 + x][0] = ld_u8(a + 1); px[y * 4 + x][1] = ld_u8(a + 2); px[y * 4 + x][2] = ld_u8(a + 3);
        }
        uint8_t out[16];
        if (mode == 0) dxt_color_block(px, out);
        else {
            if (mode == 1) {   /* DXT3: explicit 4-bit alpha */
                for (int i = 0; i < 8; i++) out[i] = (uint8_t)((px[2 * i][3] >> 4) | (px[2 * i + 1][3] & 0xF0));
            } else dxt5_alpha_block(px, out);
            dxt_color_block(px, out + 8);
        }
        memcpy(gp(dst + b * bs), out, bs);
    }
    if (done) st_u32(done, 1);
}

/* Asynchronous SPU jobs. The skinning jobs (launched from 0x7B0730) read their inputs and PUT skinned
 * vertices into RSX local memory, and nothing but the GPU reads them: the draws that use them come later in
 * the command buffer. On the PS3 they ran on SPUs beside the PPU; run inline here they cost the main thread
 * most of its frame. So they go to worker threads, and the RSX command processor, whenever the PPU has
 * published new commands, first waits until every job launched before that is done (spu_async_fence). */
#define ASYNC_RING 8192
static struct { uint32_t elf, arg[4], job, desc_size; volatile long *done; } async_q[ASYNC_RING];   /* job != 0: a SPURS job */
static volatile long async_head, async_tail;   /* head: next to take, tail: next free (both only grow) */
volatile long spu_async_launched, spu_async_done;
static SRWLOCK async_lock = SRWLOCK_INIT; static CONDITION_VARIABLE async_cv = CONDITION_VARIABLE_INIT;
static DWORD WINAPI spu_async_worker(void *p) {
    (void)p;
    static ctx_t prof_ctx[8]; static volatile long nw; long me = InterlockedIncrement(&nw) - 1;
    if (me < 8) thread_register(&prof_ctx[me], "SPU job worker");
    for (;;) {
        AcquireSRWLockExclusive(&async_lock);
        while (async_head == async_tail) SleepConditionVariableSRW(&async_cv, &async_lock, INFINITE, 0);
        long i = async_head++;
        uint32_t elf = async_q[i % ASYNC_RING].elf, arg[4], job = async_q[i % ASYNC_RING].job, dsz = async_q[i % ASYNC_RING].desc_size;
        volatile long *done = async_q[i % ASYNC_RING].done;
        memcpy(arg, async_q[i % ASYNC_RING].arg, sizeof arg);
        ReleaseSRWLockExclusive(&async_lock);
        int r = job ? spu_run_job(job, dsz) : spu_run_task(elf, arg, 0, 50000000);
        static volatile long fails; if (r && InterlockedIncrement(&fails) <= 10) printf("[spu] async %s 0x%08x failed (%d)\n", job ? "job" : "task", job ? job : elf, r);
        InterlockedIncrement(done);
        WakeByAddressAll((void *)done);   /* waiters sleep on the counter (jobchain_drain, the RSX command processor) */
    }
    return 0;
}
static int async_workers = -1;
static void spu_async_start(void) {
    if (async_workers >= 0) return;
    {   int workers;
        /* MNR_SPU_ASYNC=<workers>, 0 = run inline. Default: half the logical processors, 2..8 (ModSpot at 12 threads: 2 workers
         * 14.1 fps, 4: 17.7, 6: 18.4, 8: 18.5; the GPU side waits for these jobs before each command batch) */
        SYSTEM_INFO si; GetSystemInfo(&si);
        int def = (int)si.dwNumberOfProcessors / 2; if (def < 2) def = 2; if (def > 8) def = 8;
        const char *e = getenv("MNR_SPU_ASYNC"); workers = e ? atoi(e) : def;
        if (workers > 8) workers = 8;
        for (int i = 0; i < workers; i++) CreateThread(NULL, 1 << 20, spu_async_worker, NULL, 0, NULL);
        async_workers = workers;
    }
}
static void spu_async_push(uint32_t elf, const uint32_t *arg, uint32_t job, uint32_t desc_size, volatile long *done) {
    while (async_tail - async_head >= ASYNC_RING - 1) SwitchToThread();   /* full: let the workers catch up */
    AcquireSRWLockExclusive(&async_lock);
    long i = async_tail++;
    async_q[i % ASYNC_RING].elf = elf; async_q[i % ASYNC_RING].job = job; async_q[i % ASYNC_RING].desc_size = desc_size; async_q[i % ASYNC_RING].done = done;
    if (arg) memcpy(async_q[i % ASYNC_RING].arg, arg, 4 * sizeof *arg);
    ReleaseSRWLockExclusive(&async_lock);
    WakeConditionVariable(&async_cv);
}
static int spu_async_submit(uint32_t elf, const uint32_t arg[4]) {
    spu_async_start();
    if (!async_workers) return 0;
    InterlockedIncrement(&spu_async_launched);   /* before the PPU can publish commands that use the output */
    spu_async_push(elf, arg, 0, 0, &spu_async_done);
    return 1;
}

void hook_qtask_spu_launch(ctx_t *c) {
    {   /* debug: MNR_SPUJOB_LOG=1 - every SPU job launch: caller, program, arguments (DXT jobs: source and destination) */
        static int jl = -1; static volatile long n; if (jl < 0) jl = getenv("MNR_SPUJOB_LOG") != NULL;
        if (jl && InterlockedIncrement(&n) <= 3000) {
            printf("[spulaunch] flip %ld lr 0x%08x elf 0x%08x args %08x %08x %08x %08x", (long)rsx_flips, (uint32_t)c->lr, ARG32(2), ARG32(3), ARG32(4), ARG32(5), ARG32(6));
            if ((uint32_t)c->lr == G_LR_DXT_JOB) printf(" dxt src 0x%08x dst 0x%08x pitch %u blocks %u mode %u", ld_u32(ARG32(3)), ld_u32(ARG32(3) + 4), ld_u32(ARG32(3) + 8), ld_u32(ARG32(3) + 12), ld_u32(ARG32(3) + 16));
            printf("\n");
        }
    }
    if ((uint32_t)c->lr == G_LR_DXT_JOB) {   /* DXT compression job: run it here */
        if (qtask_log < 0) { const char *e = getenv("MNR_QTASK_LOG"); qtask_log = e && *e == '1'; }
        {   /* the source is usually a render target the GPU just composited: bring guest memory up to date */
            uint32_t src = ld_u32(ARG32(3)), pitch = ld_u32(ARG32(3) + 8), blocks = ld_u32(ARG32(3) + 12), w = pitch / 4;
            {   static int nl, on = -1; if (on < 0) on = getenv("MNR_DXT_LOG") != NULL; if (on && nl++ < 3000) printf("[dxtjob] flip %ld src 0x%08x dst 0x%08x pitch %u blocks %u -> flush 0x%x bytes\n", (long)rsx_flips, src, ld_u32(ARG32(3) + 4), pitch, blocks, w >= 4 ? pitch * ((blocks / (w / 4)) * 4) : 0); }
            if (src && w >= 4) { rsx_d3d_flush_from = 4; rsx_d3d_flush_range(src, pitch * ((blocks / (w / 4)) * 4)); }
        }
        if (qtask_log) {
            uint32_t src = ld_u32(ARG32(3)); uint64_t sum[4] = {0};
            for (uint32_t i = 0; i < 4096; i++) for (int k = 0; k < 4; k++) sum[k] += ld_u8(src + i * (ld_u32(ARG32(3) + 8) * (ld_u32(ARG32(3) + 12) / (ld_u32(ARG32(3) + 8) / 16)) * 4 / 4096 & ~3u) + k);
            printf("[spujob] (rsx flip %ld get 0x%x put 0x%x) ", (long)rsx_flips, ld_u32(GCM_CONTROL + 4), ld_u32(GCM_CONTROL)); printf("[spujob] DXT compress (flip %ld): src 0x%08x dst 0x%08x pitch %u blocks %u mode %u, source mean ARGB %llu %llu %llu %llu\n", gcm_flips,
                   src, ld_u32(ARG32(3) + 4), ld_u32(ARG32(3) + 8), ld_u32(ARG32(3) + 12), ld_u32(ARG32(3) + 16),
                   (unsigned long long)sum[0] / 4096, (unsigned long long)sum[1] / 4096, (unsigned long long)sum[2] / 4096, (unsigned long long)sum[3] / 4096);
        }
        {   /* MNR_DXT_DUMP=1: save each compression source as dxt_NNN_<addr>.bmp (debug) */
            static int on = -1, n; if (on < 0) on = getenv("MNR_DXT_DUMP") != NULL;
            uint32_t src = ld_u32(ARG32(3)), pitch = ld_u32(ARG32(3) + 8), blocks = ld_u32(ARG32(3) + 12), w = pitch / 4;
            if (on && w >= 4 && n < 60) {
                uint32_t h = blocks / (w / 4) * 4; char name[64]; snprintf(name, sizeof name, "dxt_%03d_%08x.bmp", n++, src);
                FILE *f = fopen(name, "wb");
                if (f) {
                    uint32_t sz = w * h * 4, hd[13] = {0}; uint16_t bm = 0x4D42;
                    hd[0] = 54 + sz; hd[2] = 54; hd[3] = 40; hd[4] = w; hd[5] = (uint32_t)-(int32_t)h; hd[6] = 1 | 32 << 16;
                    fwrite(&bm, 2, 1, f); fwrite(hd, 52, 1, f);
                    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
                        uint32_t a = src + y * pitch + x * 4; uint8_t px[4] = {ld_u8(a + 3), ld_u8(a + 2), ld_u8(a + 1), ld_u8(a)};
                        fwrite(px, 4, 1, f);
                    }
                    fclose(f);
                }
            }
        }
        {   static int tl = -1; if (tl < 0) tl = getenv("MNR_THUMB_LOG") != NULL;
            if (tl) printf("[thumb] dxt job src 0x%08x -> 0x%08x pitch %u blocks %u mode %u flip %ld\n", ld_u32(ARG32(3)), ld_u32(ARG32(3) + 4), ld_u32(ARG32(3) + 8), ld_u32(ARG32(3) + 12), ld_u32(ARG32(3) + 16), (long)rsx_flips); }
        spu_job_dxt_compress(ARG32(3), ARG32(4));
        RET(0);
        return;
    }
    if (qtask_log < 0) { const char *e = getenv("MNR_QTASK_LOG"); qtask_log = e && *e == '1'; }
    uint32_t from = (uint32_t)c->lr;   /* qTask manager call sites: 0x7B41F4, 0x7B4318 (x2), 0x7B4654 */
    int from_qtask = from == G_LR_QTASK_A || from == G_LR_QTASK_B || from == G_LR_QTASK_SYNC || from == G_LR_QTASK_D;
    if (!from_qtask || !ARG32(4)) {   /* other SPU job systems (0x7B0604 skinning, ...): run the SPU program */
        static int interp = -1;
        if (interp < 0) { const char *e = getenv("MNR_SPU"); interp = !(e && *e == '0'); }
        if (interp && ARG32(2)) {
            uint32_t arg[4] = {ARG32(3), ARG32(4), ARG32(5), ARG32(6)};
            if ((uint32_t)c->lr == G_LR_SKINNING && ld_u32(ARG32(3) + 8) >= RSX_LOCAL && spu_async_submit(ARG32(2), arg)) { RET(0); return; }   /* skinning into RSX memory */
            int r = spu_run_task(ARG32(2), arg, 0, 50000000);
            static long runs, fails;
            runs++;
            if (r && fails++ < 10) printf("[spu] task 0x%08x from 0x%08x failed (%d)\n", ARG32(2), (uint32_t)c->lr, r);
            if (qtask_log && runs % 500 == 1) printf("[spu] %ld SPU tasks run (%ld failed)\n", runs, fails);
            if (qtask_log) {   /* first launch of each program, with its caller */
                static uint32_t seen[64]; static int nseen;
                int k = 0; while (k < nseen && seen[k] != ARG32(2)) k++;
                if (k == nseen && nseen < 64) { seen[nseen++] = ARG32(2); printf("[spu] first run of SPU program 0x%08x from 0x%08x -> %d\n", ARG32(2), (uint32_t)c->lr, r); }
            }
            RET(0);
            return;
        }
        if (qtask_log) printf("[qtask] SPU launch without a PPU queue from 0x%08x (slot %u, arg 0x%08x 0x%08x): skipped\n",
                              (uint32_t)c->lr, ARG32(1), ARG32(2), ARG32(3));
        RET(0);
        return;
    }
    lfq_t *q = lfq_get(ARG32(4));
    if (qtask_log) {
        printf("[qtask] SPU launch from 0x%08x of task 0x%08x (slot %u, job 0x%08x, ppu fn 0x%08x) -> PPU queue; record:", (uint32_t)c->lr,
               ARG32(3), ARG32(1), ARG32(2), ld_u32(ARG32(3) + 0x1C));
        for (int i = 0; i < 0x48; i += 4) printf(" %08x", ld_u32(ARG32(3) + i));
        printf("\n");
    }
    if (!q->init) { RET(-1); return; }
    /* record {cmd, task}: cmd 0 runs the task; at 0x7B4630 ("PshSync": the task main is waiting on, in
     * mgr+0x100, just finished) it is 1 = SyncSET, which makes a worker set the event flag main sleeps on */
    uint8_t rec[16] = {0};
    uint32_t cmd = __builtin_bswap32(from == G_LR_QTASK_SYNC ? 1u : 0u), task = __builtin_bswap32(ARG32(3));
    memcpy(rec, &cmd, 4);
    memcpy(rec + 4, &task, 4);
    EnterCriticalSection(&q->cs);
    while (q->count == q->depth) SleepConditionVariableCS(&q->not_full, &q->cs, INFINITE);
    memcpy(q->data + ((q->head + q->count) % q->depth) * q->size, rec, q->size < 16 ? q->size : 16);
    q->count++;
    WakeConditionVariable(&q->not_empty);
    LeaveCriticalSection(&q->cs);
    RET(0);
}
static void spurs_lfq_pop(ctx_t *c) {    /* (q, buffer, isBlocking) */
    lfq_t *q = lfq_get(ARG32(0));
    if (!q->init) { RET(0x80410902); return; }
    EnterCriticalSection(&q->cs);
    while (q->count == 0) {
        if (!ARG32(2)) { LeaveCriticalSection(&q->cs); RET(0x80410901); return; }
        SleepConditionVariableCS(&q->not_empty, &q->cs, INFINITE);
    }
    memcpy(gp(ARG32(1)), q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->depth; q->count--;
    WakeConditionVariable(&q->not_full);
    LeaveCriticalSection(&q->cs);
    RET(0);
}

/* ------------------------------------------------------------------ event flags (syscalls 82..88, 118, 139) */
typedef struct { CRITICAL_SECTION cs; CONDITION_VARIABLE cv; uint64_t bits; } kflag_t;
static void ksys_eflag_create(ctx_t *c) {   /* (*id, attr, init) */
    kflag_t *f = calloc(1, sizeof *f);
    InitializeCriticalSection(&f->cs); InitializeConditionVariable(&f->cv); f->bits = ARG(2);
    st_u32(ARG32(0), kobj_new(f, 0x98000000)); RET(0);
}
static int eflag_match(uint64_t bits, uint64_t pat, uint32_t mode) { return (mode & 2) ? (bits & pat) != 0 : (bits & pat) == pat; }
static void ksys_eflag_wait(ctx_t *c, int try_only) {   /* (id, bitptn, mode, *result, timeout) */
    kflag_t *f = kobj_get(ARG32(0));
    if (!f) { RET(CELL_ESRCH); return; }
    uint64_t pat = ARG(1); uint32_t mode = ARG32(2);   /* mode: 1 AND, 2 OR; 0x10 clear all, 0x20 clear matched */
    EnterCriticalSection(&f->cs);
    while (!eflag_match(f->bits, pat, mode)) {
        if (try_only) { LeaveCriticalSection(&f->cs); RET(CELL_EBUSY); return; }
        if (!SleepConditionVariableCS(&f->cv, &f->cs, wait_ms(ARG(4)))) { LeaveCriticalSection(&f->cs); RET(CELL_ETIMEDOUT); return; }
    }
    if (ARG32(3)) st_u64(ARG32(3), f->bits);
    if (mode & 0x10) f->bits = 0; else if (mode & 0x20) f->bits &= ~pat;
    LeaveCriticalSection(&f->cs);
    RET(0);
}
static void ksys_eflag_set(ctx_t *c) {
    kflag_t *f = kobj_get(ARG32(0));
    if (!f) { RET(CELL_ESRCH); return; }
    EnterCriticalSection(&f->cs); f->bits |= ARG(1); WakeAllConditionVariable(&f->cv); LeaveCriticalSection(&f->cs);
    RET(0);
}
static void ksys_eflag_clear(ctx_t *c) {   /* (id, bitptn): bits &= bitptn */
    kflag_t *f = kobj_get(ARG32(0));
    if (!f) { RET(CELL_ESRCH); return; }
    EnterCriticalSection(&f->cs); f->bits &= ARG(1); LeaveCriticalSection(&f->cs);
    RET(0);
}

/* ------------------------------------------------------------------ event queues / ports (syscalls 128..139)
 * receive blocks until an event arrives and returns it in r4..r7 (source, data1..3), like lv2. */
typedef struct { uint64_t src, d1, d2, d3; } kevent_t;
typedef struct { CRITICAL_SECTION cs; CONDITION_VARIABLE cv; kevent_t ev[127]; uint32_t head, count, cap, id; } kqueue_t;
typedef struct { uint64_t name; kqueue_t *q; } kport_t;
static uint32_t eq_watch(void) {   /* MNR_EQ_WATCH=<queue id, hex> (debug): log who connects to and sends to that queue */
    static long w = -1; if (w < 0) { const char *e = getenv("MNR_EQ_WATCH"); w = e ? (long)strtoul(e, NULL, 16) : 0; }
    return (uint32_t)w;
}
static struct { uint64_t key; kqueue_t *q; } queue_keys[64]; static int nqueue_keys;   /* IPC keys (cellVoice finds queues by key) */
static kqueue_t *kqueue_by_key(uint64_t key) { for (int i = 0; i < nqueue_keys; i++) if (queue_keys[i].key == key) return queue_keys[i].q; return NULL; }
static void ksys_queue_create(ctx_t *c) {   /* (*id, attr, ipc_key, size) */
    kqueue_t *q = calloc(1, sizeof *q);
    InitializeCriticalSection(&q->cs); InitializeConditionVariable(&q->cv);
    q->cap = ARG32(3) && ARG32(3) <= 127 ? ARG32(3) : 127;
    q->id = kobj_new(q, 0x8d000000);
    if (ARG(2) && nqueue_keys < 64) { queue_keys[nqueue_keys].key = ARG(2); queue_keys[nqueue_keys++].q = q; }
    st_u32(ARG32(0), q->id); RET(0);
}
static int kqueue_push(kqueue_t *q, kevent_t e) {
    EnterCriticalSection(&q->cs);
    int ok = q->count < q->cap;
    if (ok) { q->ev[(q->head + q->count++) % q->cap] = e; WakeConditionVariable(&q->cv); }
    LeaveCriticalSection(&q->cs);
    return ok;
}
static void ksys_queue_receive(ctx_t *c, int try_only) {   /* (id, *event (unused), timeout) */
    kqueue_t *q = kobj_get(ARG32(0));
    if (!q) { RET(CELL_ESRCH); return; }
    if (eq_watch() && q->id == eq_watch()) {   /* the timeouts its waiters pass: the first 30, then unusual ones */
        static volatile long n; long k = InterlockedIncrement(&n); uint64_t us = ARG(2);
        if (k <= 30 || us == 0 || us > 1000000) printf("[eqwatch] receive #%ld on queue 0x%08x by %s (lr 0x%llx): timeout %llu us%s\n", k, q->id, my_treg ? my_treg->name : "?", (unsigned long long)c->lr, (unsigned long long)us, us == 0 ? " (forever)" : "");
    }
    EnterCriticalSection(&q->cs);
    while (q->count == 0) {
        if (try_only) { LeaveCriticalSection(&q->cs); RET(CELL_EBUSY); return; }
        if (!SleepConditionVariableCS(&q->cv, &q->cs, wait_ms(ARG(2)))) { LeaveCriticalSection(&q->cs); RET(CELL_ETIMEDOUT); return; }
    }
    kevent_t e = q->ev[q->head]; q->head = (q->head + 1) % q->cap; q->count--;
    LeaveCriticalSection(&q->cs);
    c->r[4] = e.src; c->r[5] = e.d1; c->r[6] = e.d2; c->r[7] = e.d3;
    RET(0);
}
static void ksys_port_create(ctx_t *c) {    /* (*id, port_type, name) */
    kport_t *p = calloc(1, sizeof *p); p->name = ARG(2);
    st_u32(ARG32(0), kobj_new(p, 0x0e000000)); RET(0);
}
static void ksys_port_connect(ctx_t *c) {   /* (port, queue) */
    kport_t *p = kobj_get(ARG32(0)); kqueue_t *q = kobj_get(ARG32(1));
    if (!p || !q) { RET(CELL_ESRCH); return; }
    if (eq_watch() && q->id == eq_watch()) printf("[eqwatch] port 0x%08x (name 0x%llx) connected to queue 0x%08x by %s (lr 0x%llx)\n", ARG32(0), (unsigned long long)p->name, q->id, my_treg ? my_treg->name : "?", (unsigned long long)c->lr);
    p->q = q; RET(0);
}
static void ksys_port_send(ctx_t *c) {      /* (port, d1, d2, d3) */
    kport_t *p = kobj_get(ARG32(0));
    if (!p || !p->q) { RET(CELL_ESRCH); return; }
    if (eq_watch() && p->q->id == eq_watch()) { static volatile long n; long k = InterlockedIncrement(&n);
        if (k <= 40 || k % 2000 == 0) printf("[eqwatch] send #%ld to queue 0x%08x by %s (lr 0x%llx): %llx %llx %llx, %u queued\n", k, p->q->id, my_treg ? my_treg->name : "?", (unsigned long long)c->lr, (unsigned long long)ARG(1), (unsigned long long)ARG(2), (unsigned long long)ARG(3), p->q->count); }
    kevent_t e = {p->name, ARG(1), ARG(2), ARG(3)};
    RET(kqueue_push(p->q, e) ? 0 : CELL_EBUSY);
}

/* ------------------------------------------------------------------ cellVoice (voice.c): ports, the microphone, the codec.
 * Events (service / microphone attached, ...) go to every queue the game registered with cellVoiceSetNotifyEventQueue
 * as (source, event, data2, 0). */
#include "voice.h"
static struct { kqueue_t *q; uint64_t source; } voice_queues[8]; static int nvoice_queues; static SRWLOCK voice_qlock = SRWLOCK_INIT;
static void voice_send_event(uint64_t event, uint64_t data2) {
    AcquireSRWLockShared(&voice_qlock);
    for (int i = 0; i < nvoice_queues; i++) { kevent_t e = {voice_queues[i].source, event, data2, 0}; kqueue_push(voice_queues[i].q, e); }
    ReleaseSRWLockShared(&voice_qlock);
}
static uint32_t voice_alloc_cb(uint32_t size, uint32_t align) { return user_alloc(size, align); }
static void hle_voice_init(ctx_t *c) { voice_notify = voice_send_event; voice_guest_alloc = voice_alloc_cb; RET(ARG32(0) ? voice_init() : 0x80310805); }
static void hle_voice_end(ctx_t *c) {
    RET(voice_stop(1));
    AcquireSRWLockExclusive(&voice_qlock); nvoice_queues = 0; ReleaseSRWLockExclusive(&voice_qlock);
}
static void hle_voice_start(ctx_t *c) { RET(voice_start()); }
static void hle_voice_stop(ctx_t *c) { RET(voice_stop(0)); }
static void hle_voice_create_queue(ctx_t *c) {   /* (*id, *key): a queue with a free key */
    for (uint64_t i = 0; i < 10; i++) {
        uint64_t key = 0x80004d494f323285ull + i;
        if (kqueue_by_key(key)) continue;
        kqueue_t *q = calloc(1, sizeof *q);
        InitializeCriticalSection(&q->cs); InitializeConditionVariable(&q->cv); q->cap = 0x40;
        q->id = kobj_new(q, 0x8d000000);
        if (nqueue_keys < 64) { queue_keys[nqueue_keys].key = key; queue_keys[nqueue_keys++].q = q; }
        st_u32(ARG32(0), q->id); st_u64(ARG32(1), key);
        RET(0); return;
    }
    RET(0x8031080f);
}
static void hle_voice_set_queue(ctx_t *c) {   /* (key, source) */
    kqueue_t *q = kqueue_by_key(ARG(0));
    if (!q) { printf("[voice] cellVoiceSetNotifyEventQueue: no queue with key 0x%llx\n", (unsigned long long)ARG(0)); RET(0x8031080f); return; }
    static uint32_t next_src;
    uint64_t src = ARG(1) ? ARG(1) : ((uint64_t)2 << 32) | (0x0e000000u + 0x100u * (next_src++ % 256));
    AcquireSRWLockExclusive(&voice_qlock);
    if (nvoice_queues < 8) { voice_queues[nvoice_queues].q = q; voice_queues[nvoice_queues++].source = src; }
    ReleaseSRWLockExclusive(&voice_qlock);
    RET(0);
}
static void hle_voice_remove_queue(ctx_t *c) {   /* (key) */
    kqueue_t *q = kqueue_by_key(ARG(0));
    AcquireSRWLockExclusive(&voice_qlock);
    for (int i = 0; i < nvoice_queues; ) if (voice_queues[i].q == q) voice_queues[i] = voice_queues[--nvoice_queues]; else i++;
    ReleaseSRWLockExclusive(&voice_qlock);
    RET(q ? 0 : 0x8031080f);
}
static void hle_voice_create_port(ctx_t *c) {   /* (*portId, CellVoicePortParam *) */
    uint32_t id = 0, r = ARG32(1) ? voice_create_port(ARG32(1), &id) : 0x80310805;
    if (!r) st_u32(ARG32(0), id);
    RET(r);
}
static void hle_voice_delete_port(ctx_t *c) { RET(voice_delete_port(ARG32(0))); }
static void hle_voice_connect(ctx_t *c) { RET(voice_connect(ARG32(0), ARG32(1), 1)); }
static void hle_voice_disconnect(ctx_t *c) { RET(voice_connect(ARG32(0), ARG32(1), 0)); }
static void hle_voice_port_info(ctx_t *c) { RET(voice_port_info(ARG32(0), ARG32(1))); }
static void hle_voice_write(ctx_t *c) {   /* (port, data, *size) (and the Ex forms: frames lost / gaps, unused) */
    if (!ARG32(2)) { RET(0x8031080a); return; }
    uint32_t n = ld_u32(ARG32(2)), r = voice_write(ARG32(0), ARG32(1), &n);
    if (!r) st_u32(ARG32(2), n);
    RET(r);
}
static void hle_voice_read(ctx_t *c) {   /* (port, data, *size) */
    if (!ARG32(2)) { RET(0); return; }
    uint32_t want = ld_u32(ARG32(2)), n = want, r = voice_read(ARG32(0), ARG32(1), &n);
    { static volatile long k; static uint32_t maxw; if (InterlockedIncrement(&k) <= 20 || want > maxw) { if (want > maxw) maxw = want; printf("[voice] game reads port 0x%x: asks %u bytes, gets %u (lr 0x%llx, buffer 0x%08x)\n", ARG32(0), want, n, (unsigned long long)c->lr, ARG32(1)); } }
    if (!r) st_u32(ARG32(2), n);
    RET(r);
}
static void hle_voice_set_volume(ctx_t *c) { RET(voice_set(ARG32(0), 0, 0, (float)F(1))); }
static void hle_voice_get_volume(ctx_t *c) { float v = 0; uint32_t r = voice_get(ARG32(0), 0, NULL, &v); if (!r) st_f32(ARG32(1), v); RET(r); }
static void hle_voice_set_mute(ctx_t *c) { RET(voice_set(ARG32(0), 1, ARG32(1) & 0xFFFF, 0)); }
static void hle_voice_get_mute(ctx_t *c) { uint32_t v = 0, r = voice_get(ARG32(0), 1, &v, NULL); if (!r) st_u16(ARG32(1), v); RET(r); }
static void hle_voice_mute_all(ctx_t *c) { RET(voice_all(1, ARG32(0) & 0xFFFF)); }
static void hle_voice_set_bitrate(ctx_t *c) { RET(voice_set(ARG32(0), 3, ARG32(1), 0)); }
static void hle_voice_get_bitrate(ctx_t *c) { uint32_t v = 0, r = voice_get(ARG32(0), 3, &v, NULL); if (!r) st_u32(ARG32(1), v); RET(r); }
static void hle_voice_pause(ctx_t *c) { RET(voice_set(ARG32(0), 2, 1, 0)); }
static void hle_voice_resume(ctx_t *c) { RET(voice_set(ARG32(0), 2, 0, 0)); }
static void hle_voice_pause_all(ctx_t *c) { RET(voice_all(2, 1)); }
static void hle_voice_resume_all(ctx_t *c) { RET(voice_all(2, 0)); }
static void hle_voice_reset_port(ctx_t *c) { RET(voice_set(ARG32(0), 4, 0, 0)); }
static void hle_voice_set_attr(ctx_t *c) {   /* (port, attr, *value) */
    if (!ARG32(2)) { RET(0x8031080a); return; }
    uint32_t v = ld_u32(ARG32(2)); RET(voice_attr(ARG32(0), ARG32(1), &v, 1));
}
static void hle_voice_get_attr(ctx_t *c) {
    if (!ARG32(2)) { RET(0x8031080a); return; }
    uint32_t v = 0, r = voice_attr(ARG32(0), ARG32(1), &v, 0); if (!r) st_u32(ARG32(2), v); RET(r);
}
static void hle_voice_signal_state(ctx_t *c) { RET(0x80310812); }   /* (port, attr, *value): no per-signal state */

/* ------------------------------------------------------------------ cellGame (PARAM.SFO from the disc) */
typedef struct { char key[32]; char str[256]; uint32_t num; int is_num; } sfo_entry_t;
static sfo_entry_t sfo[64];
static int sfo_count;
static void sfo_load(void) {   /* PSF format: magic "\0PSF", key table, data table, index entries */
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\extracted\\PS3_GAME\\PARAM.SFO", mnr_root);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static uint8_t b[65536];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < 20 || memcmp(b, "\0PSF", 4)) return;
    uint32_t keys, data, count;
    memcpy(&keys, b + 8, 4); memcpy(&data, b + 12, 4); memcpy(&count, b + 16, 4);   /* little-endian */
    for (uint32_t i = 0; i < count && i < 64; i++) {
        const uint8_t *e = b + 20 + 16 * i;
        uint16_t koff; uint16_t fmt; uint32_t len, off;
        memcpy(&koff, e, 2); memcpy(&fmt, e + 2, 2); memcpy(&len, e + 4, 4); memcpy(&off, e + 12, 4);
        sfo_entry_t *s = &sfo[sfo_count++];
        snprintf(s->key, sizeof s->key, "%s", (const char *)b + keys + koff);
        if (fmt == 0x0404) { memcpy(&s->num, b + data + off, 4); s->is_num = 1; }
        else snprintf(s->str, sizeof s->str, "%.*s", (int)len, (const char *)b + data + off);
    }
}
static const sfo_entry_t *sfo_find(const char *key) {
    for (int i = 0; i < sfo_count; i++) if (!strcmp(sfo[i].key, key)) return &sfo[i];
    return NULL;
}
static char game_content[128] = "/dev_bdvd/PS3_GAME";
static void cell_game_boot_check(ctx_t *c) {   /* (u32 *type, u32 *attributes, CellGameContentSize *size, char dirName[32]) */
    st_u32(ARG32(0), 1);                        /* CELL_GAME_GAMETYPE_DISC */
    snprintf(game_content, sizeof game_content, "/dev_bdvd/PS3_GAME");
    /* CELL_GAME_ATTRIBUTE_PATCH when the update is installed, as the PS3 reports a disc booted with its patch: the game
     * only then loads the patch's shader list (DATA\SHADERS\SHADERLIST_LOAD_01.XML), which brings the terrain, water and
     * sky shaders of track themes 6 and 7. Without it those tracks drew no ground (a black or blue screen). */
    {   char hp[MAX_PATH]; uint32_t attr = 0;
        if (fs_host_path("/dev_hdd0/game/BCUS98167/PARAM.SFO", hp, sizeof hp) && GetFileAttributesA(hp) != INVALID_FILE_ATTRIBUTES) attr = 1;
        const char *e = getenv("MNR_BOOT_ATTR"); if (e) attr = (uint32_t)strtoul(e, NULL, 0);   /* override (tests) */
        st_u32(ARG32(1), attr); }
    if (ARG32(2)) { st_u32(ARG32(2), 10 << 20); st_u32(ARG32(2) + 4, (uint32_t)-1); st_u32(ARG32(2) + 8, 0); }   /* hddFree 10 GB */
    if (ARG32(3)) mem_zero(ARG32(3), 32);
    RET(0);
}
/* cellGame state: which content ContentPermit hands back. BootCheck selects the disc; GameDataCheck(type 3)
 * selects /dev_hdd0/game/<dirName> (created by CreateGameData when missing). */
static void cell_game_data_check(ctx_t *c) {   /* (type, dirName, size *) */
    if (ARG32(2)) { st_u32(ARG32(2), 10 << 20); st_u32(ARG32(2) + 4, (uint32_t)-1); st_u32(ARG32(2) + 8, 0); }
    if (ARG32(0) != 3 || !ARG32(1)) {   /* 1 disc, 2 HDD boot game, 3 game data */ snprintf(game_content, sizeof game_content, "/dev_bdvd/PS3_GAME"); RET(0); return; }
    char g[128], hp[MAX_PATH];
    snprintf(g, sizeof g, "/dev_hdd0/game/%s", (const char *)gp(ARG32(1)));
    snprintf(game_content, sizeof game_content, "%s", g);
    printf("[game] data check %s\n", g);
    DWORD at = fs_host_path(g, hp, sizeof hp) ? GetFileAttributesA(hp) : INVALID_FILE_ATTRIBUTES;
    int found = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
    RET(found ? 0 : 2);   /* OK / CELL_GAME_RET_NONE */
}
static void game_get_size_kb(ctx_t *c) { if (ARG32(0)) st_u32(ARG32(0), 0); RET(0); }   /* (s32 *sizeKB) */
static void cell_game_content_permit(ctx_t *c) {   /* (char contentInfoPath[128], char usrdirPath[128]) */
    char b[160];
    snprintf(b, sizeof b, "%s/USRDIR", game_content);
    memcpy(gp(ARG32(0)), game_content, strlen(game_content) + 1);
    memcpy(gp(ARG32(1)), b, strlen(b) + 1);
    printf("[game] content permit %s\n", b);
    RET(0);
}
static void cell_game_get_param_int(ctx_t *c) {   /* (id, s32 *value) */
    const char *key = ARG32(0) == 102 ? "PARENTAL_LEVEL" : ARG32(0) == 103 ? "RESOLUTION" : ARG32(0) == 104 ? "SOUND_FORMAT" : "";
    const sfo_entry_t *e = sfo_find(key);
    st_u32(ARG32(1), e ? e->num : 0);
    RET(0);
}
static void cell_game_get_param_string(ctx_t *c) {   /* (id, char *buf, bufsize) */
    uint32_t id = ARG32(0);
    const char *key = id <= 20 ? "TITLE" : id == 100 ? "TITLE_ID" : id == 101 ? "VERSION" : id == 106 ? "APP_VER" : "";
    const sfo_entry_t *e = sfo_find(key);
    const char *s = e ? e->str : "";
    uint32_t cap = ARG32(2);
    if (cap) { size_t n = strlen(s); if (n >= cap) n = cap - 1; memcpy(gp(ARG32(1)), s, n); st_u8(ARG32(1) + (uint32_t)n, 0); }
    RET(0);
}
static void cell_user_info_get_list(ctx_t *c) {   /* (u32 *listNum, CellUserInfoUserList *list, u32 *currentUserId) */
    if (ARG32(0)) st_u32(ARG32(0), 1);
    if (ARG32(1)) { mem_zero(ARG32(1), 16 * 4); st_u32(ARG32(1), 1); }
    if (ARG32(2)) st_u32(ARG32(2), 1);
    RET(0);
}
static void cell_user_info_get_stat(ctx_t *c) {   /* (id, CellUserInfoUserStat {u32 id; char name[64];}) */
    mem_zero(ARG32(1), 4 + 64);
    st_u32(ARG32(1), 1);
    memcpy(gp(ARG32(1) + 4), "Player", 7);
    RET(0);
}
static void np_trophy_create(ctx_t *c) { st_u32(ARG32(0), 1); RET(0); }   /* context / handle id = 1 */
static void np_trophy_space(ctx_t *c) { if (ARG32(2)) st_u64(ARG32(2), 0); RET(0); }   /* (ctx, handle, u64 *reqspace, opt) */

/* ------------------------------------------------------------------ more system services */
static void sysutil_get_param_int(ctx_t *c) {   /* (id, s32 *value) */
    int32_t v = 0;
    switch (ARG32(0)) {
    case 0x111: v = 1; break;   /* LANG: English (US) */
    case 0x112: v = 1; break;   /* ENTER_BUTTON_ASSIGN: cross */
    case 0x114: v = 1; break;   /* DATE_FORMAT: DDMMYYYY */
    case 0x115: v = 1; break;   /* TIME_FORMAT: 24 hour */
    case 0x116: v = 0; break;   /* TIMEZONE (minutes) */
    case 0x117: v = 0; break;   /* SUMMERTIME */
    case 0x121: v = 9; break;   /* GAME_PARENTAL_LEVEL: off */
    case 0x141: v = net_enabled && getenv("MNR_NP_ID") && *getenv("MNR_NP_ID"); break;   /* CURRENT_USER_HAS_NP_ACCOUNT (the online boot flow needs it) */
    }
    st_u32(ARG32(1), (uint32_t)v);
    RET(0);
}
static uint64_t rtc_tick_now(void) {   /* CellRtcTick: microseconds since 0001-01-01 */
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t100 = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);   /* since 1601 */
    return t100 / 10 + 50491123200000000ULL;                                 /* 0001 -> 1601 */
}
static void rtc_get_current_tick(ctx_t *c) { st_u64(ARG32(0), rtc_tick_now()); RET(0); }
static void game_create_game_data(ctx_t *c) {   /* (init*, char tmpContentInfoPath[128], char tmpUsrdirPath[128]) */
    char b[160], hp[MAX_PATH];
    if (strncmp(game_content, "/dev_hdd0/game/", 15)) {
        const sfo_entry_t *id = sfo_find("TITLE_ID");
        snprintf(game_content, sizeof game_content, "/dev_hdd0/game/%s", id ? id->str : "BCUS98167");
    }
    snprintf(b, sizeof b, "%s/USRDIR", game_content);   /* created in place (no tmp + rename) */
    if (fs_host_path(b, hp, sizeof hp)) { fs_mkdirs(hp); CreateDirectoryA(hp, NULL); }
    memcpy(gp(ARG32(1)), game_content, strlen(game_content) + 1);
    memcpy(gp(ARG32(2)), b, strlen(b) + 1);
    printf("[game] create game data %s\n", b);
    RET(0);
}
static void msg_dialog_open(ctx_t *c) {   /* (type, msgString, callback, userData, extParam) */
    printf("[dialog] %s\n", ARG32(1) ? (const char *)gp(ARG32(1)) : "");
    RET(0);
}
/* ------------------------------------------------------------------ cellSaveData (auto save/load)
 * Saves live in hdd0/home/00000001/savedata/<dirName>/ (plus PARAM.BIN: the game's
 * CellSaveDataSystemFileParam, used for the stat callback). The game drives everything through two
 * callbacks: funcStat (sees CellSaveDataStatGet, may set the parameters) and funcFile (asks for one file
 * operation per call until it answers OK_LAST). */
static void call_opd(ctx_t *c, uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2) {   /* call a guest function pointer, keep our registers */
    ctx_t save = *c;
    c->r[3] = a0; c->r[4] = a1; c->r[5] = a2;
    c->r[2] = ld_u32(opd + 4); c->lr = 0;
    c->r[1] -= 256;   /* leave the caller's frame alone */
    dispatch(c, ld_u32(opd), 0);
    *c = save;
}
/* ------------------------------------------------------------------ sysutil events and the on-screen keyboard
 * Events are queued by the HLE (any thread) and delivered to the registered callbacks when the game calls
 * cellSysutilCheckCallback, as the firmware does. The on-screen keyboard (naming a creation, chat) is a box over
 * the game picture typed into with the keyboard: Enter accepts, Esc cancels, Ctrl+V pastes. */
static struct { uint32_t status, param; } sysutil_q[32];
static int sysutil_qn;
static SRWLOCK sysutil_qlock = SRWLOCK_INIT;
static void sysutil_post(uint32_t status, uint32_t param) {
    AcquireSRWLockExclusive(&sysutil_qlock);
    if (sysutil_qn < 32) { sysutil_q[sysutil_qn].status = status; sysutil_q[sysutil_qn].param = param; sysutil_qn++; }
    ReleaseSRWLockExclusive(&sysutil_qlock);
}
static void np_deliver(ctx_t *c);
static void cell_sysutil_check_callback(ctx_t *c) {
    for (;;) {
        uint32_t st, pa;
        AcquireSRWLockExclusive(&sysutil_qlock);
        if (!sysutil_qn) { ReleaseSRWLockExclusive(&sysutil_qlock); break; }
        st = sysutil_q[0].status; pa = sysutil_q[0].param;
        memmove(sysutil_q, sysutil_q + 1, --sysutil_qn * sizeof sysutil_q[0]);
        ReleaseSRWLockExclusive(&sysutil_qlock);
        printf("[sysutil] event 0x%04x\n", st);
        for (int i = 0; i < 4; i++) if (sysutil_cb[i][0]) call_opd(c, sysutil_cb[i][0], st, pa, sysutil_cb[i][1]);
    }
    np_deliver(c);
    RET(0);
}

#define OSK_LOADED 0x0502
#define OSK_FINISHED 0x0503
#define OSK_UNLOADED 0x0504
#define OSK_INPUT_CANCELED 0x0506
static struct { volatile long open; int result; uint32_t limit; WCHAR title[256], text[512]; } osk;
/* The text entry is drawn over the game picture (rsx_d3d.c, osk_overlay) and typed into the game window (present.c,
 * osk_key): a separate Windows text box stayed hidden behind the game in fullscreen. */
static volatile long osk_typing;   /* accepting keys */
static SRWLOCK osk_lock = SRWLOCK_INIT;
static void osk_finish(int result) {
    AcquireSRWLockExclusive(&osk_lock);
    int was = osk_typing; osk_typing = 0; osk.result = result;
    ReleaseSRWLockExclusive(&osk_lock);
    if (!was) return;
    printf("[osk] %s\n", result ? "canceled" : "entered");
    if (result) sysutil_post(OSK_INPUT_CANCELED, 0);
    sysutil_post(OSK_FINISHED, 0);
}
int osk_overlay(WCHAR *title, size_t tcap, WCHAR *text, size_t xcap) {   /* (render thread) 1 while typing, with the texts */
    if (!osk_typing) return 0;
    AcquireSRWLockShared(&osk_lock);
    wcsncpy(title, osk.title[0] ? osk.title : L"Enter text", tcap - 1); title[tcap - 1] = 0;
    wcsncpy(text, osk.text, xcap - 1); text[xcap - 1] = 0;
    ReleaseSRWLockShared(&osk_lock);
    return 1;
}
int osk_key(unsigned msg, unsigned wp) {   /* (window thread) WM_CHAR / WM_KEYDOWN; 1 when taken */
    if (!osk_typing) return 0;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) { osk_finish(0); return 1; }
        if (wp == VK_ESCAPE) { osk_finish(1); return 1; }
        if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && OpenClipboard(NULL)) {   /* Ctrl+V */
            HANDLE h = GetClipboardData(CF_UNICODETEXT); const WCHAR *t = h ? GlobalLock(h) : NULL;
            if (t) {
                AcquireSRWLockExclusive(&osk_lock);
                size_t n = wcslen(osk.text), lim = osk.limit ? osk.limit : 511;
                for (; *t && n < lim; t++) if (*t >= 32) osk.text[n++] = *t;
                osk.text[n] = 0;
                ReleaseSRWLockExclusive(&osk_lock);
                GlobalUnlock(h);
            }
            CloseClipboard(); return 1;
        }
        return 1;   /* nothing reaches the game's keyboard controls while typing */
    }
    if (msg == WM_CHAR) {
        AcquireSRWLockExclusive(&osk_lock);
        size_t n = wcslen(osk.text), lim = osk.limit ? osk.limit : 511;
        if (wp == 8) { if (n) osk.text[n - 1] = 0; }
        else if (wp == 0x7F) { while (n && osk.text[n - 1] == ' ') n--; while (n && osk.text[n - 1] != ' ') n--; osk.text[n] = 0; }   /* Ctrl+Backspace: a word */
        else if (wp >= 32 && n < lim) { osk.text[n] = (WCHAR)wp; osk.text[n + 1] = 0; }
        ReleaseSRWLockExclusive(&osk_lock);
        return 1;
    }
    return 0;
}
static DWORD WINAPI osk_thread(void *p) {
    (void)p;
    {   /* MNR_OSK_AUTO (tests): entered at once - the game's suggested text, or this text when not empty */
        const char *e = getenv("MNR_OSK_AUTO");
        if (e) {
            Sleep(500);
            if (*e) MultiByteToWideChar(CP_UTF8, 0, e, -1, osk.text, 512);
            printf("[osk] entered automatically: %ls\n", osk.text);
            osk_finish(0);
            return 0;
        }
    }
    if (rsx_window) SetForegroundWindow(rsx_window);   /* the keys go to the game window */
    return 0;
}
static void be_wstr_in(uint32_t a, WCHAR *out, size_t cap) {   /* guest big-endian UTF-16 -> host */
    size_t i = 0;
    if (a) for (; i + 1 < cap; i++) { uint16_t ch = ld_u16(a + 2 * (uint32_t)i); if (!ch) break; out[i] = ch; }
    out[i] = 0;
}
static void osk_load_async(ctx_t *c) {   /* (container, CellOskDialogParam*, CellOskDialogInputFieldInfo* {u16 *message, u16 *init_text, u32 limit_length}) */
    if (InterlockedExchange(&osk.open, 1)) { RET(0x8002b502); return; }   /* CELL_OSKDIALOG_ERROR_IME_ALREADY_IN_USE */
    uint32_t f = ARG32(2);
    be_wstr_in(f ? ld_u32(f) : 0, osk.title, 256);
    be_wstr_in(f ? ld_u32(f + 4) : 0, osk.text, 512);
    osk.limit = f ? ld_u32(f + 8) : 0; if (osk.limit > 511) osk.limit = 511;
    osk.result = 1; osk_typing = 1;
    sysutil_post(OSK_LOADED, 0);
    CreateThread(NULL, 0, osk_thread, NULL, 0, NULL);
    RET(0);
}
static void osk_unload_async(ctx_t *c) {   /* (CellOskDialogCallbackReturnParam* {s32 result, s32 numCharsResultString, u16 *pResultString}) */
    uint32_t o = ARG32(0);
    if (o) {
        uint32_t s = ld_u32(o + 8); size_t n = wcslen(osk.text);
        if (osk.limit && n > osk.limit) n = osk.limit;
        st_u32(o, (uint32_t)(osk.result ? 1 : n ? 0 : 3));   /* OK / CANCELED / NO_INPUT_TEXT */
        st_u32(o + 4, osk.result ? 0 : (uint32_t)n);
        if (s) { for (size_t i = 0; i < n && !osk.result; i++) st_u16(s + 2 * (uint32_t)i, osk.text[i]); st_u16(s + 2 * (uint32_t)(osk.result ? 0 : n), 0); }
    }
    osk.open = 0;
    sysutil_post(OSK_UNLOADED, 0);
    RET(0);
}
static void osk_abort(ctx_t *c) { osk_finish(1); RET(0); }

#define SD_PARAM_SIZE 1552   /* CellSaveDataSystemFileParam */
static int savedata_log = -1;
static void save_data_auto(ctx_t *c, int save, uint32_t dir_name, uint32_t setbuf, uint32_t fstat, uint32_t ffile, uint32_t userdata) {
    if (savedata_log < 0) savedata_log = getenv("MNR_SAVEDATA_LOG") != NULL;
    char gdir[128], hdir[MAX_PATH], hp[MAX_PATH];
    snprintf(gdir, sizeof gdir, "/dev_hdd0/home/00000001/savedata/%s", (const char *)gp(dir_name));
    if (!fs_host_path(gdir, hdir, sizeof hdir)) { RET(0x8002b402); return; }
    {   /* MNR_SAVEDATA_DIR=<host folder>: keep saves there instead (test runs leave the player's save alone) */
        const char *sd = getenv("MNR_SAVEDATA_DIR");
        if (sd) { CreateDirectoryA(sd, NULL); snprintf(hdir, sizeof hdir, "%s\\%s", sd, (const char *)gp(dir_name)); }
    }
    DWORD at = GetFileAttributesA(hdir);
    int exists = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
    if (savedata_log) printf("[savedata] auto%s %s (%s)\n", save ? "save" : "load", gdir, exists ? "exists" : "new");
    if (!save && !exists) { RET(0x8002b40b); return; }   /* CELL_SAVEDATA_ERROR_NODATA */
    static uint32_t blk;   /* result 32 | statGet 1704 | statSet 16 | fileGet 68 | fileSet 48 | file names */
    if (!blk) blk = gmalloc(0x1000);
    uint32_t res = blk, sget = blk + 0x40, sset = blk + 0x700, fget = blk + 0x740, fset = blk + 0x7C0;
    mem_zero(blk, 0x1000);
    st_u32(res + 16, userdata);
    /* stat: free space, new-data flag, directory name, stored parameters, file list */
    st_u32(sget, 10 * 1024 * 1024);
    st_u32(sget + 4, !exists);
    memcpy(gp(sget + 8 + 24), gp(dir_name), strnlen((const char *)gp(dir_name), 31));
    snprintf(hp, sizeof hp, "%s\\PARAM.BIN", hdir);
    FILE *pf = fopen(hp, "rb");
    if (pf) { fread(gp(sget + 64), 1, SD_PARAM_SIZE, pf); fclose(pf); }
    uint32_t list_max = setbuf ? ld_u32(setbuf + 4) : 0, list = setbuf ? ld_u32(setbuf + 36) : 0, nfiles = 0, nlist = 0;
    uint64_t total = 0;
    if (exists) {
        WIN32_FIND_DATAA fd; snprintf(hp, sizeof hp, "%s\\*", hdir);
        HANDLE h = FindFirstFileA(hp, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !_stricmp(fd.cFileName, "PARAM.BIN")) continue;
                nfiles++; total += fd.nFileSizeLow;
                if (list && nlist < list_max && strlen(fd.cFileName) < 13) {   /* CellSaveDataFileStat {type, pad, u64 size, 3 times, name[13]} = 56 bytes */
                    uint32_t e = list + 56 * nlist++;
                    mem_zero(e, 56);
                    st_u64(e + 8, fd.nFileSizeLow);
                    memcpy(gp(e + 40), fd.cFileName, strlen(fd.cFileName));
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    st_u32(sget + 1620, (uint32_t)((total + 1023) / 1024));
    st_u32(sget + 1628, nfiles); st_u32(sget + 1632, nlist); st_u32(sget + 1636, list);
    call_opd(c, fstat, res, sget, sset);
    int32_t r = (int32_t)ld_u32(res);
    if (savedata_log) printf("[savedata]   stat callback -> %d\n", r);
    if (r < 0) { RET(0x8002b401); return; }   /* CELL_SAVEDATA_ERROR_CBRESULT */
    if (save) {
        CreateDirectoryA(hdir, NULL);   /* parent folders: created by fs_host_path users below */
        if (GetFileAttributesA(hdir) == INVALID_FILE_ATTRIBUTES) { fs_mkdirs(hdir); CreateDirectoryA(hdir, NULL); }
        uint32_t param = ld_u32(sset);
        if (param) { snprintf(hp, sizeof hp, "%s\\PARAM.BIN", hdir); FILE *f = fopen(hp, "wb"); if (f) { fwrite(gp(param), 1, SD_PARAM_SIZE, f); fclose(f); } }
    }
    if (r == 1 || r == 2) { RET(0); return; }   /* OK_LAST(_NOCONFIRM): no file operations */
    mem_zero(fget, 0x80);   /* fileGet.excSize reports the previous operation to the next callback: don't clear it per call */
    for (int it = 0; it < 4096; it++) {
        mem_zero(fset, 48);
        call_opd(c, ffile, res, fget, fset);
        r = (int32_t)ld_u32(res);
        if (r < 0) { if (savedata_log) printf("[savedata]   file callback -> %d\n", r); RET(0x8002b401); return; }
        if (r == 1 || r == 2) break;
        uint32_t op = ld_u32(fset), type = ld_u32(fset + 8), nm = ld_u32(fset + 28), off = ld_u32(fset + 32), size = ld_u32(fset + 36);
        uint32_t bufsize = ld_u32(fset + 40), buf = ld_u32(fset + 44);
        static const char *fixed[] = {NULL, NULL, "ICON0.PNG", "ICON1.PAM", "PIC1.PNG", "SND0.AT3"};
        /* content files (ICON0, ICON1, PIC1, SND0) have fixed names whatever the game passes: MNR names its icon
         * "ICONS/ICON0.PNG", and refusing that ended every save before UP-OPS/UP-CREA ("corrupt save" on the next load) */
        const char *name = type >= 2 && type < 6 ? fixed[type] : nm ? (const char *)gp(nm) : NULL;
        if (!name || strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) { RET(0x8002b402); return; }
        snprintf(hp, sizeof hp, "%s\\%s", hdir, name);
        uint32_t done = 0;
        if (op == 0) {   /* READ */
            FILE *f = fopen(hp, "rb");
            if (!f) {   /* as the firmware does (and RPCS3): a missing file ends the operation with FAILURE, no further callbacks */
                if (savedata_log) printf("[savedata]   read %s: no such file -> CELL_SAVEDATA_ERROR_FAILURE\n", name);
                RET(0x8002b407); return;
            }
            fseek(f, off, SEEK_SET); done = (uint32_t)fread(gp(buf), 1, size < bufsize ? size : bufsize, f); fclose(f); ww_touch(buf, done);
        } else if (op == 1 || op == 3) {   /* WRITE (truncating), WRITE_NOTRUNC */
            if (!save) { RET(0x8002b402); return; }
            FILE *f = fopen(hp, op == 1 ? "wb" : "r+b");
            if (!f && op == 3) f = fopen(hp, "wb");
            if (f) { fseek(f, off, SEEK_SET); done = (uint32_t)fwrite(gp(buf), 1, size, f); fclose(f); }
        } else if (op == 2) {   /* DELETE */
            if (save) DeleteFileA(hp);
        }
        st_u32(fget, done);
        if (savedata_log) printf("[savedata]   %s %s: %u bytes\n", op == 0 ? "read" : op == 2 ? "delete" : "write", name, done);
    }
    RET(0);
}
/* (version, userId, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata on the stack) */
static void save_data_user_auto_save(ctx_t *c) { save_data_auto(c, 1, ARG32(2), ARG32(4), ARG32(5), ARG32(6), (uint32_t)ld_u64((uint32_t)c->r[1] + 112)); }
static void save_data_user_auto_load(ctx_t *c) { save_data_auto(c, 0, ARG32(2), ARG32(4), ARG32(5), ARG32(6), (uint32_t)ld_u64((uint32_t)c->r[1] + 112)); }
/* (version, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata) */
static void save_data_auto_save2(ctx_t *c) { save_data_auto(c, 1, ARG32(1), ARG32(3), ARG32(4), ARG32(5), ARG32(7)); }
static void save_data_auto_load2(ctx_t *c) { save_data_auto(c, 0, ARG32(1), ARG32(3), ARG32(4), ARG32(5), ARG32(7)); }

/* ------------------------------------------------------------------ PSN (NP) sign-in
 * Offline unless MNR_NET=1 and MNR_NP_ID (the online name) are set. Online, the game asks for an NP ticket (the
 * signed proof of identity it hands to its servers); tickets come from the launcher, which signs in to RPCN with
 * the player's own account (MNR_NP_HELPER=host:port answers "GET /ticket?service=<id>&cookie=<hex>" with the
 * ticket bytes), or from a file (MNR_NP_TICKET). Results reach the game through its manager callback, run from
 * cellSysutilCheckCallback as on the console. */
static int np_online(void) { static int id = -1; if (id < 0) { const char *e = getenv("MNR_NP_ID"); id = e && *e; } return net_enabled && id; }
static uint32_t np_cb, np_cb_arg;
static struct { int32_t event, result; } np_q[16];
static int np_qn;
static uint8_t *np_ticket; static int np_ticket_len;
static void np_post(int32_t event, int32_t result) {
    AcquireSRWLockExclusive(&sysutil_qlock);
    if (np_qn < 16) { np_q[np_qn].event = event; np_q[np_qn].result = result; np_qn++; }
    ReleaseSRWLockExclusive(&sysutil_qlock);
}
/* sceNpBasic's handler and its queued calls (the friends block below) */
static struct { int32_t event, ret; uint32_t req; } npb_calls[64]; static int npb_ncalls;
static uint32_t npb_handler, npb_arg;
static int npb_log(void) { static int on = -1; if (on < 0) on = getenv("MNR_NP_LOG") != NULL || getenv("MNR_NET_LOG") != NULL; return on; }
static void np_deliver(ctx_t *c) {   /* (from cellSysutilCheckCallback) */
    for (;;) {
        int32_t ev, re;
        AcquireSRWLockExclusive(&sysutil_qlock);
        if (!np_qn) { ReleaseSRWLockExclusive(&sysutil_qlock); break; }
        ev = np_q[0].event; re = np_q[0].result;
        memmove(np_q, np_q + 1, --np_qn * sizeof np_q[0]);
        ReleaseSRWLockExclusive(&sysutil_qlock);
        printf("[np] manager event %d result 0x%x\n", ev, re);
        if (np_cb) call_opd(c, np_cb, (uint64_t)(int64_t)ev, (uint64_t)(int64_t)re, np_cb_arg);
    }
    for (;;) {   /* sceNpBasic events: handler(s32 event, s32 retCode, u32 reqId, void *arg) */
        int32_t ev, re; uint32_t rq;
        AcquireSRWLockExclusive(&sysutil_qlock);
        if (!npb_ncalls) { ReleaseSRWLockExclusive(&sysutil_qlock); break; }
        ev = npb_calls[0].event; re = npb_calls[0].ret; rq = npb_calls[0].req;
        memmove(npb_calls, npb_calls + 1, --npb_ncalls * sizeof npb_calls[0]);
        ReleaseSRWLockExclusive(&sysutil_qlock);
        if (npb_log()) printf("[np] basic event %d result %d\n", ev, re);
        if (npb_handler) {
            ctx_t save = *c;
            c->r[3] = (uint64_t)(int64_t)ev; c->r[4] = (uint64_t)(int64_t)re; c->r[5] = rq; c->r[6] = npb_arg;
            c->r[2] = ld_u32(npb_handler + 4); c->lr = 0; c->r[1] -= 256;
            dispatch(c, ld_u32(npb_handler), 0);
            *c = save;
        }
    }
}
static void np_manager_register_callback(ctx_t *c) {
    np_cb = ARG32(0); np_cb_arg = ARG32(1);
    if (np_online()) np_post(3, 0);   /* SCE_NP_MANAGER_STATUS_ONLINE */
    RET(0);
}
static void np_manager_unregister_callback(ctx_t *c) { np_cb = 0; RET(0); }
static void np_manager_get_status(ctx_t *c) { st_u32(ARG32(0), np_online() ? 3 : (uint32_t)-1); RET(0); }   /* ONLINE / OFFLINE */
#define NP_ERR_OFFLINE 0x80550006   /* SCE_NP_ERROR_INVALID_STATE */
static void np_fill_online_id(uint32_t a) {   /* SceNpOnlineId {char data[16]; char term; char dummy[3]} */
    mem_zero(a, 20);
    const char *id = getenv("MNR_NP_ID");
    if (id) { size_t n = strlen(id); if (n > 16) n = 16; memcpy(gp(a), id, n); }
}
static void np_manager_get_np_id(ctx_t *c) {   /* SceNpId {online id (20), u8 opt[8], u8 reserved[8]} */
    if (!np_online()) { RET(NP_ERR_OFFLINE); return; }
    mem_zero(ARG32(0), 36); np_fill_online_id(ARG32(0));
    memcpy(gp(ARG32(0) + 20), "un\0\0\0\0\0\0", 8);   /* as RPCN signs in ("un" + platform bytes left 0) */
    RET(0);
}
static void np_manager_get_online_id(ctx_t *c) { if (!np_online()) { RET(NP_ERR_OFFLINE); return; } np_fill_online_id(ARG32(0)); RET(0); }
static void np_manager_get_account_region(ctx_t *c) {   /* (SceNpCountryCode {char data[2]; char term; char pad}*, s32 *language) */
    if (!np_online()) { RET(NP_ERR_OFFLINE); return; }
    if (ARG32(0)) { mem_zero(ARG32(0), 4); memcpy(gp(ARG32(0)), "us", 2); }
    if (ARG32(1)) st_u32(ARG32(1), 1);   /* English (US) */
    RET(0);
}
static void np_manager_get_chat_restriction(ctx_t *c) { if (ARG32(0)) st_u32(ARG32(0), 0); RET(np_online() ? 0 : NP_ERR_OFFLINE); }
static void np_manager_get_content_rating(ctx_t *c) {   /* (s32 *isRestricted, s32 *age) */
    if (ARG32(0)) st_u32(ARG32(0), 0);
    if (ARG32(1)) st_u32(ARG32(1), 18);
    RET(np_online() ? 0 : NP_ERR_OFFLINE);
}
static void np_manager_get_avatar_url(ctx_t *c) {   /* SceNpAvatarUrl {char data[128]; char term; char pad[3]} */
    if (ARG32(0)) { mem_zero(ARG32(0), 132); strcpy((char *)gp(ARG32(0)), "http://static-resource.np.community.playstation.net/avatar_m/default/DefaultAvatar_m.png"); }
    RET(np_online() ? 0 : NP_ERR_OFFLINE);
}
/* sceNpSignaling: connections between players, kept by p2p.c on the shared P2P port (RPCS3-compatible). The game
 * only creates a context and checks or ends connections other players open to it. */
#include "p2p.h"
static uint32_t np_sig_handler, np_sig_arg;
static void np_signaling_create_ctx(ctx_t *c) {   /* (const SceNpId *, handler, arg, u32 *ctx_id) */
    if (!ARG32(0) || !ARG32(3)) { RET(0x8002a815); return; }   /* SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT */
    np_sig_handler = ARG32(1); np_sig_arg = ARG32(2);
    st_u32(ARG32(3), 1);
    RET(0);
}
static void np_signaling_destroy_ctx(ctx_t *c) { np_sig_handler = 0; RET(0); }
static void np_signaling_get_connection_status(ctx_t *c) {   /* (ctx, conn_id, s32 *status, in_addr *peer, u16 *port) */
    uint32_t addr = 0; uint16_t port = 0;
    int st = p2p_conn_status(ARG32(1), &addr, &port);
    if (!ARG32(2)) { RET(0x8002a815); return; }
    st_u32(ARG32(2), st < 0 ? 0 : (uint32_t)st);
    if (st < 0) { RET(0x8002a80e); return; }   /* SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND */
    if (ARG32(3)) memcpy(gp(ARG32(3)), &addr, 4);   /* network order, as the console stores it */
    if (ARG32(4)) st_u16(ARG32(4), port);
    RET(0);
}
static void np_signaling_terminate_connection(ctx_t *c) { p2p_terminate(ARG32(1)); RET(0); }

/* the ticket from the launcher (MNR_NP_HELPER) or a file (MNR_NP_TICKET) */
static int np_fetch_ticket(const char *service, const uint8_t *cookie, int cookie_len) {
    const char *file = getenv("MNR_NP_TICKET"), *helper = getenv("MNR_NP_HELPER");
    free(np_ticket); np_ticket = NULL; np_ticket_len = 0;
    if (helper && *helper) {
        char path[2600]; int n = snprintf(path, sizeof path, "/ticket?service=%s&cookie=", service);
        for (int i = 0; i < cookie_len && n < (int)sizeof path - 3; i++) n += snprintf(path + n, sizeof path - n, "%02x", cookie[i]);
        np_ticket_len = net_http_get(helper, path, &np_ticket);
        if (np_ticket_len <= 0) { printf("[np] the launcher's ticket service at %s gave no ticket\n", helper); free(np_ticket); np_ticket = NULL; np_ticket_len = 0; }
    }
    if (!np_ticket && file && *file) {
        FILE *f = fopen(file, "rb");
        if (f) { fseek(f, 0, SEEK_END); np_ticket_len = (int)ftell(f); fseek(f, 0, SEEK_SET); np_ticket = malloc(np_ticket_len ? np_ticket_len : 1); np_ticket_len = (int)fread(np_ticket, 1, np_ticket_len, f); fclose(f); }
    }
    printf("[np] ticket for service %s: %d bytes\n", service, np_ticket_len);
    return np_ticket_len > 0;
}
static struct { char service[256]; uint8_t cookie[1024]; int cookie_len; } np_req;
static DWORD WINAPI np_ticket_thread(void *p) {
    (void)p;
    np_post(255, np_fetch_ticket(np_req.service, np_req.cookie, np_req.cookie_len) ? np_ticket_len : (int32_t)0x8002a1a0);   /* SCE_NP_MANAGER_EVENT_GOT_TICKET (size, or an auth error) */
    return 0;
}
static void np_request_ticket_common(ctx_t *c, uint32_t service, uint32_t cookie, uint32_t cookie_size) {
    if (!np_online()) { RET(NP_ERR_OFFLINE); return; }
    snprintf(np_req.service, sizeof np_req.service, "%s", service ? (const char *)gp(service) : "");
    np_req.cookie_len = cookie && cookie_size <= sizeof np_req.cookie ? (int)cookie_size : 0;
    if (np_req.cookie_len) memcpy(np_req.cookie, gp(cookie), np_req.cookie_len);
    printf("[np] ticket requested: service %s, cookie %d bytes\n", np_req.service, np_req.cookie_len);
    CloseHandle(CreateThread(NULL, 0, np_ticket_thread, NULL, 0, NULL));
    RET(0);
}
/* (const SceNpId *, const char *serviceId, const void *cookie, size_t cookieSize, const char *entitlementId, u32 consumedCount) */
static void np_manager_request_ticket(ctx_t *c) { np_request_ticket_common(c, ARG32(1), ARG32(2), ARG32(3)); }
/* (const SceNpId *, const SceNpTicketVersion *, serviceId, cookie, cookieSize, entitlementId, consumedCount) */
static void np_manager_request_ticket2(ctx_t *c) { np_request_ticket_common(c, ARG32(2), ARG32(3), ARG32(4)); }
static void np_manager_get_ticket(ctx_t *c) {   /* (void *buffer, size_t *bufferSize) */
    if (!np_ticket) { RET(0x80550008); return; }   /* SCE_NP_ERROR_ABORTED-ish: no ticket yet */
    uint32_t sz = ld_u32(ARG32(1));
    if (!ARG32(0)) { st_u32(ARG32(1), np_ticket_len); RET(0); return; }
    if (sz < (uint32_t)np_ticket_len) { RET(0x80550005); return; }   /* SCE_NP_ERROR_INVALID_ARGUMENT */
    memcpy(gp(ARG32(0)), np_ticket, np_ticket_len); st_u32(ARG32(1), np_ticket_len);
    RET(0);
}
/* cellSslCertificateLoader(flag, buffer, size, size_t *required): the game loads the console's root certificates
 * before starting HTTP(S). Its servers are plain HTTP, so an empty PEM-sized block will do. */
static void ssl_certificate_loader(ctx_t *c) {
    if (ARG32(3)) st_u32(ARG32(3), 4096);
    if (ARG32(1) && ARG32(2)) mem_zero(ARG32(1), ARG32(2));
    RET(0);
}
/* sceNpLookup: the game looks up other players' NP ids by online name (online rooms, before their pictures and
 * profiles). Answered locally and at once: an NP id is the online name plus fixed option bytes. */
static volatile long np_lookup_next;
static void np_lookup_create_title_ctx(ctx_t *c) { RET(1); }
static void np_lookup_create_transaction_ctx(ctx_t *c) { RET(1 + (InterlockedIncrement(&np_lookup_next) & 0x3FF)); }
static void np_lookup_np_id_async(ctx_t *c) {   /* (transId, const SceNpOnlineId *, SceNpId *out, option, ...) */
    uint32_t id = ARG32(1), out = ARG32(2);
    if (out) {
        mem_zero(out, 36);
        if (id) memcpy(gp(out), gp(id), 16);
        memcpy(gp(out + 20), "un", 2);
    }
    RET(0);
}
static void np_lookup_poll_async(ctx_t *c) { if (ARG32(1)) st_u32(ARG32(1), 0); RET(0); }   /* (transId, s32 *result): finished, OK */
static void np_basic_count0_arg0(ctx_t *c) { if (ARG32(0)) st_u32(ARG32(0), 0); RET(0); }   /* friend / block list counts: none */
/* sceNpBasicAddPlayersHistoryAsync(const SceNpId *, u32 count, u64 options, u32 *reqId): the console's "players met"
 * list; there is none here, so it is accepted (the game warned "AddRecentPlayer failed!" after every online race) */
static void np_basic_add_players_history(ctx_t *c) { static uint32_t req; if (ARG32(3)) st_u32(ARG32(3), ++req); RET(0); }
static void np_basic_count0_arg1(ctx_t *c) { if (ARG32(1)) st_u32(ARG32(1), 0); RET(0); }   /* players history (options, u32 *count) */

/* ------------------------------------------------------------------ sceNpBasic: friends, presence, messages
 * As RPCS3 answers them from its RPCN client. The RPCN session is the launcher's (one sign-in per account), so the
 * friend list, presence and messages come through MNR_NP_HELPER: GET /rpcn/friends (the list), /rpcn/friendevents
 * (polled every second: status, presence, new/lost friends, messages), /rpcn/friendpresence (ours), /rpcn/friendadd
 * (a friend request) and /rpcn/friendmsg (invites). Events reach the game's basic handler from
 * cellSysutilCheckCallback; it then fetches each with sceNpBasicGetEvent. */
/* Friend notices for the player (the console's own pop-ups, RPCS3's overlay messages): drawn for 6 s in the corner box
 * with the volume and microphone (rsx_d3d.c, draw_hud). */
static struct { DWORD at; char text[96]; } npb_notes[8]; static int npb_note_next;
static SRWLOCK npb_note_lock = SRWLOCK_INIT;
static void np_notice(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    AcquireSRWLockExclusive(&npb_note_lock);
    vsnprintf(npb_notes[npb_note_next].text, sizeof npb_notes[0].text, fmt, ap);
    npb_notes[npb_note_next].at = GetTickCount(); npb_note_next = (npb_note_next + 1) % 8;
    ReleaseSRWLockExclusive(&npb_note_lock);
    va_end(ap);
}
int np_notices(wchar_t out[][96], int max) {   /* the notices still shown, oldest first */
    int n = 0; DWORD now = GetTickCount();
    AcquireSRWLockShared(&npb_note_lock);
    for (int k = 0; k < 8 && n < max; k++) {
        int i = (npb_note_next + k) % 8;
        if (!npb_notes[i].at || now - npb_notes[i].at > 6000) continue;
        MultiByteToWideChar(CP_UTF8, 0, npb_notes[i].text, -1, out[n], 96); n++;
    }
    ReleaseSRWLockShared(&npb_note_lock);
    return n;
}
static void np_util_cmp_np_id(ctx_t *c) {   /* (const SceNpId *, const SceNpId *): the same online id? */
    if (!ARG32(0) || !ARG32(1)) { RET(0x8002ab01); return; }   /* SCE_NP_UTIL_ERROR_INVALID_ARGUMENT */
    RET(strncmp((const char *)gp(ARG32(0)), (const char *)gp(ARG32(1)), 16) ? 0x8002ab0f : 0);   /* NOT_MATCH */
}
#define NPB_MAXF 100
typedef struct { char name[17]; int online; char com[13]; char title[128], status[64], comment[64]; uint8_t data[128]; uint32_t size; } npb_friend_t;
typedef struct { int32_t event; char from[17]; uint8_t data[1024]; uint32_t size; } npb_event_t;
static npb_friend_t npb_f[NPB_MAXF]; static int npb_nf;
static npb_event_t npb_ev[32]; static int npb_nev;
static SRWLOCK npb_lock = SRWLOCK_INIT;
static int npb_ctx_sensitive; static char npb_com[13];
static const char *npb_helper(void) { const char *h = getenv("MNR_NP_HELPER"); return np_online() && h && *h ? h : NULL; }
static int npb_unhex(const char *s, uint8_t *out, int max) {   /* "-" = empty */
    int n = 0;
    if (!s || !strcmp(s, "-")) return 0;
    for (; s[0] && s[1] && n < max; s += 2) { unsigned v; if (sscanf(s, "%2x", &v) != 1) break; out[n++] = (uint8_t)v; }
    return n;
}
static void npb_hex(char *out, const uint8_t *b, int n) { if (!n) { strcpy(out, "-"); return; } for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02X", b[i]); }
static void npb_call(int32_t event, int32_t ret) {   /* the game's basic handler, from cellSysutilCheckCallback */
    AcquireSRWLockExclusive(&sysutil_qlock);
    if (npb_ncalls < 64) { npb_calls[npb_ncalls].event = event; npb_calls[npb_ncalls].ret = ret; npb_calls[npb_ncalls].req = 0; npb_ncalls++; }
    ReleaseSRWLockExclusive(&sysutil_qlock);
}
static void npb_event(int32_t event, const char *from, const uint8_t *data, uint32_t size) {   /* (npb_lock held) for sceNpBasicGetEvent */
    if (npb_nev == 32) { memmove(npb_ev, npb_ev + 1, 31 * sizeof npb_ev[0]); npb_nev--; }
    npb_event_t *e = &npb_ev[npb_nev++];
    e->event = event; strncpy(e->from, from, 16); e->from[16] = 0;
    e->size = size > sizeof e->data ? sizeof e->data : size; if (e->size) memcpy(e->data, data, e->size);
    npb_call(event, 0);
}
static void npb_presence_event(const npb_friend_t *f) {   /* (npb_lock held) as RPCS3: PRESENCE in our game, else OUT_OF_CONTEXT / OFFLINE */
    if (f->online && !strcmp(f->com, npb_com)) npb_event(1, f->name, f->data, f->size);
    else npb_event(f->online && npb_ctx_sensitive ? 9 : 0, f->name, NULL, 0);
}
static npb_friend_t *npb_find(const char *name, int add) {   /* (npb_lock held) */
    for (int i = 0; i < npb_nf; i++) if (!strcmp(npb_f[i].name, name)) return &npb_f[i];
    if (!add || npb_nf == NPB_MAXF) return NULL;
    npb_friend_t *f = &npb_f[npb_nf++]; memset(f, 0, sizeof *f); strncpy(f->name, name, 16); return f;
}
static void npb_set_presence(npb_friend_t *f, char **tok) {   /* tok: online com title status comment data */
    f->online = atoi(tok[0]);
    snprintf(f->com, sizeof f->com, "%s", strcmp(tok[1], "-") ? tok[1] : "");
    uint8_t b[256]; int n;
    n = npb_unhex(tok[2], b, sizeof f->title - 1); memcpy(f->title, b, n); f->title[n] = 0;
    n = npb_unhex(tok[3], b, sizeof f->status - 1); memcpy(f->status, b, n); f->status[n] = 0;
    n = npb_unhex(tok[4], b, sizeof f->comment - 1); memcpy(f->comment, b, n); f->comment[n] = 0;
    f->size = (uint32_t)npb_unhex(tok[5], f->data, sizeof f->data);
}
static int npb_split(char *line, char **tok, int max) { int n = 0; for (char *t = strtok(line, " "); t && n < max; t = strtok(NULL, " ")) tok[n++] = t; return n; }
static void npb_load_friends(void) {   /* the list at registration, then an initial presence event per friend (as RPCS3) */
    const char *h = npb_helper(); if (!h) return;
    uint8_t *b = NULL; int n = net_http_get(h, "/rpcn/friends", &b);
    if (n < 0) { free(b); return; }
    char *s = malloc(n + 1); memcpy(s, b, n); s[n] = 0; free(b);
    AcquireSRWLockExclusive(&npb_lock);
    npb_nf = 0;
    char *save = NULL;
    for (char *line = strtok_s(s, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
        char *tok[8]; int k = npb_split(line, tok, 8);
        if (k == 8 && !strcmp(tok[0], "friend")) { npb_friend_t *f = npb_find(tok[1], 1); if (f) npb_set_presence(f, tok + 2); }
    }
    for (int i = 0; i < npb_nf; i++) npb_presence_event(&npb_f[i]);
    printf("[np] %d friends\n", npb_nf);
    ReleaseSRWLockExclusive(&npb_lock);
    npb_call(6, 0);   /* SCE_NP_BASIC_EVENT_END_OF_INITIAL_PRESENCE */
    free(s);
}
static DWORD WINAPI npb_poll(void *p) {
    (void)p;
    for (;;) {
        Sleep(1000);
        const char *h = npb_helper(); if (!h || !npb_handler) continue;
        uint8_t *b = NULL; int n = net_http_get(h, "/rpcn/friendevents", &b);
        if (n <= 0) { free(b); continue; }
        char *s = malloc(n + 1); memcpy(s, b, n); s[n] = 0; free(b);
        AcquireSRWLockExclusive(&npb_lock);
        char *save = NULL;
        for (char *line = strtok_s(s, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
            if (npb_log()) printf("[np] friend event: %.120s\n", line);
            char *tok[8]; int k = npb_split(line, tok, 8);
            npb_friend_t *f;
            if (k == 3 && !strcmp(tok[0], "status") && (f = npb_find(tok[1], 0))) {   /* on- or offline: presence cleared */
                char *t[6] = {tok[2], "-", "-", "-", "-", "-"}; npb_set_presence(f, t); npb_presence_event(f);
                np_notice(f->online ? "%s is online" : "%s went offline", f->name);
            } else if (k == 8 && !strcmp(tok[0], "presence") && (f = npb_find(tok[1], 0))) { npb_set_presence(f, tok + 2); npb_presence_event(f); }
            else if (k == 3 && !strcmp(tok[0], "new") && (f = npb_find(tok[1], 1))) {
                char *t[6] = {tok[2], "-", "-", "-", "-", "-"}; npb_set_presence(f, t); npb_presence_event(f);
                printf("[np] new friend %s\n", f->name); np_notice("You're now friends with %s", f->name);
            }
            else if (k == 2 && !strcmp(tok[0], "lost")) {
                for (int i = 0; i < npb_nf; i++) if (!strcmp(npb_f[i].name, tok[1])) { npb_f[i] = npb_f[--npb_nf]; break; }
                printf("[np] %s is no longer a friend\n", tok[1]); np_notice("%s is no longer your friend", tok[1]);
            } else if (k == 2 && !strcmp(tok[0], "query")) { printf("[np] friend request from %s\n", tok[1]); np_notice("%s sent you a friend request", tok[1]); }
            else if (k == 7 && !strcmp(tok[0], "msg") && !strcmp(tok[2], npb_com)) {   /* msg from com main sub features data */
                unsigned main_type = (unsigned)atoi(tok[3]), feat = (unsigned)atoi(tok[5]); int ev;
                switch (main_type) {
                case 0: case 5: ev = 4; break;                       /* attachment: INCOMING_ATTACHMENT */
                case 3: ev = feat & 2 ? 23 : 5; break;               /* invite: INCOMING_(BOOTABLE_)INVITATION */
                case 4: ev = feat & 2 ? 25 : 21; break;              /* custom data */
                case 1: ev = 2; break;                               /* general: MESSAGE */
                default: ev = -1; break;
                }
                if (ev >= 0) { static uint8_t d[1024]; int dn = npb_unhex(tok[6], d, sizeof d); npb_event(ev, tok[1], d, (uint32_t)dn); printf("[np] message (type %u) from %s\n", main_type, tok[1]);
                             np_notice(main_type == 3 ? "%s sent you an invite" : "Message from %s", tok[1]); }
            }
        }
        ReleaseSRWLockExclusive(&npb_lock);
        free(s);
    }
    return 0;
}
static void npb_register(ctx_t *c, int sensitive) {   /* (const SceNpCommunicationId *, handler, arg) */
    uint32_t ctx = ARG32(0);
    if (ctx) snprintf(npb_com, sizeof npb_com, "%.9s_%02u", (const char *)gp(ctx), (unsigned)ld_u8(ctx + 10));
    npb_handler = ARG32(1); npb_arg = ARG32(2); npb_ctx_sensitive = sensitive;
    printf("[np] basic handler registered (%s)\n", npb_com);
    if (npb_helper()) {
        static int started; if (!started++) CreateThread(NULL, 1 << 16, npb_poll, NULL, 0, NULL);
        npb_load_friends();
    }
    RET(0);
}
static void np_basic_register_handler(ctx_t *c) { npb_register(c, 0); }
static void np_basic_register_cs_handler(ctx_t *c) { npb_register(c, 1); }
static void np_basic_unregister_handler(ctx_t *c) { npb_handler = 0; RET(0); }
static void np_basic_friend_count(ctx_t *c) {   /* (u32 *count) */
    if (!ARG32(0)) { RET(0x8002a665); return; }
    AcquireSRWLockShared(&npb_lock); st_u32(ARG32(0), np_online() ? (uint32_t)npb_nf : 0); ReleaseSRWLockShared(&npb_lock);
    RET(0);
}
static void np_basic_friend_presence(ctx_t *c) {   /* (u32 index, SceNpUserInfo *user, SceNpBasicPresenceDetails *pres, u32 options) */
    uint32_t i = ARG32(0), user = ARG32(1), pres = ARG32(2);
    if (!user || !pres) { RET(0x8002a665); return; }
    AcquireSRWLockShared(&npb_lock);
    if (i >= (uint32_t)npb_nf) { ReleaseSRWLockShared(&npb_lock); RET(0x8002a665); return; }
    const npb_friend_t *f = &npb_f[i];
    mem_zero(user, 216);   /* {SceNpId (36), SceNpOnlineName (52), SceNpAvatarUrl (128)} */
    memcpy(gp(user), f->name, strlen(f->name)); memcpy(gp(user + 36), f->name, strlen(f->name));
    mem_zero(pres, 392);   /* {title[128], status[64], comment[64], data[128], u32 size, s32 state} */
    memcpy(gp(pres), f->title, strlen(f->title)); memcpy(gp(pres + 128), f->status, strlen(f->status)); memcpy(gp(pres + 192), f->comment, strlen(f->comment));
    memcpy(gp(pres + 256), f->data, f->size); st_u32(pres + 384, f->size);
    st_u32(pres + 388, f->online ? (!strcmp(f->com, npb_com) ? 2 : 1) : 0);   /* IN_CONTEXT / OUT_OF_CONTEXT / OFFLINE */
    ReleaseSRWLockShared(&npb_lock);
    RET(0);
}
static void np_basic_get_event(ctx_t *c) {   /* (s32 *event, SceNpUserInfo *from, u8 *data, u32 *size) */
    uint32_t pe = ARG32(0), from = ARG32(1), data = ARG32(2), psize = ARG32(3);
    if (!pe || !from || !data || !psize) { RET(0x8002a665); return; }
    AcquireSRWLockExclusive(&npb_lock);
    if (!npb_nev) { ReleaseSRWLockExclusive(&npb_lock); RET(0x8002a66a); return; }   /* SCE_NP_BASIC_ERROR_NO_EVENT */
    npb_event_t e = npb_ev[0]; memmove(npb_ev, npb_ev + 1, --npb_nev * sizeof npb_ev[0]);
    ReleaseSRWLockExclusive(&npb_lock);
    st_u32(pe, (uint32_t)e.event);
    mem_zero(from, 216); memcpy(gp(from), e.from, strlen(e.from)); memcpy(gp(from + 36), e.from, strlen(e.from));
    uint32_t r = 0;
    if (e.event != 0) {
        uint32_t avail = ld_u32(psize), n = e.size < avail ? e.size : avail;
        memcpy(gp(data), e.data, n); st_u32(psize, n);
        if (n < e.size) r = 0x8002a66e;   /* DATA_LOST */
    }
    if (npb_log()) printf("[np] event %d from %s (%u bytes)\n", e.event, e.from, e.size);
    RET(r);
}
static void npb_send_presence(const char *status, const uint8_t *data, int size) {   /* only when it changes (as RPCS3) */
    static char last_status[64]; static uint8_t last_data[128]; static int last_size = -1, has_status;
    if (status) { if (has_status && !strcmp(status, last_status)) status = NULL; else { snprintf(last_status, sizeof last_status, "%s", status); has_status = 1; } }
    if (data) { if (size == last_size && !memcmp(data, last_data, size)) data = NULL; else { memcpy(last_data, data, size); last_size = size; } }
    if (!status && !data && last_size >= 0) return;
    const char *h = npb_helper(); if (!h) return;
    char path[1024], st[160], dt[300], ti[64];
    npb_hex(st, (const uint8_t *)last_status, (int)strlen(last_status));
    npb_hex(dt, last_data, last_size > 0 ? last_size : 0);
    npb_hex(ti, (const uint8_t *)"ModNation Racers", 16);
    snprintf(path, sizeof path, "/rpcn/friendpresence?com=%s&title=%s&status=%s&data=%s", npb_com, ti, st, dt);
    uint8_t *b = NULL; net_http_get(h, path, &b); free(b);
}
static void np_basic_set_presence(ctx_t *c) {   /* (const u8 *data, u32 size) */
    uint32_t n = ARG32(1); if (n > 128) { RET(0x8002a66b); return; }
    if (!npb_handler) { RET(0x8002a66d); return; }
    npb_send_presence(NULL, n ? gp(ARG32(0)) : (const uint8_t *)"", (int)n);
    RET(0);
}
static void np_basic_set_presence_details(ctx_t *c) {   /* (const SceNpBasicPresenceDetails *, u32 options: 1 data, 2 status) */
    uint32_t p = ARG32(0), opt = ARG32(1);
    if (!npb_handler) { RET(0x8002a66d); return; }
    if (!p || opt < 1 || opt > 3) { RET(0x8002a665); return; }
    uint32_t size = ld_u32(p + 384); if (size > 128) { RET(0x8002a66b); return; }
    char status[64]; memcpy(status, gp(p + 128), 63); status[63] = 0;
    npb_send_presence(opt & 2 ? status : NULL, opt & 1 ? gp(p + 256) : NULL, (int)size);
    RET(0);
}
static void npb_message(const char *to, unsigned main_type, unsigned sub, unsigned feat, const char *subject, const char *body, const uint8_t *data, uint32_t size) {
    const char *h = npb_helper(); if (!h) return;
    size_t cap = 512 + 2 * size + 2 * (subject ? strlen(subject) : 0) + 2 * (body ? strlen(body) : 0);
    char *path = malloc(cap), *dt = malloc(2 * size + 2), *sj = malloc(2 * (subject ? strlen(subject) : 0) + 2), *bd = malloc(2 * (body ? strlen(body) : 0) + 2);
    npb_hex(dt, data, (int)size); npb_hex(sj, (const uint8_t *)(subject ? subject : ""), subject ? (int)strlen(subject) : 0); npb_hex(bd, (const uint8_t *)(body ? body : ""), body ? (int)strlen(body) : 0);
    snprintf(path, cap, "/rpcn/friendmsg?com=%s&main=%u&sub=%u&feat=%u&to=%s&subject=%s&body=%s&data=%s", npb_com, main_type, sub, feat, to, sj, bd, dt);
    uint8_t *b = NULL; net_http_get(h, path, &b); free(b);
    printf("[np] message (type %u, %u bytes) to %s\n", main_type, size, to);
    free(path); free(dt); free(sj); free(bd);
}
static void np_basic_send_message(ctx_t *c) {   /* (const SceNpId *to, const void *data, u32 size): a general message */
    uint32_t to = ARG32(0), data = ARG32(1), size = ARG32(2);
    if (!npb_handler) { RET(0x8002a66d); return; }
    if (!to || !ld_u8(to) || !data || !size) { RET(0x8002a665); return; }
    if (size > 512) { RET(0x8002a66b); return; }
    char name[17]; memcpy(name, gp(to), 16); name[16] = 0;
    if (!npb_helper()) { RET(0x8002a678); return; }   /* NOT_CONNECTED */
    npb_message(name, 1, 0, 0, NULL, NULL, gp(data), size);
    RET(0);
}
static int npb_add_friend(const char *name) {   /* 0 = sent / accepted */
    const char *h = npb_helper(); if (!h) return -1;
    char path[128]; snprintf(path, sizeof path, "/rpcn/friendadd?npid=%s", name);
    uint8_t *b = NULL; int n = net_http_get(h, path, &b);
    int ok = n >= 2 && !memcmp(b, "ok", 2);
    printf("[np] friend request to %s: %.*s\n", name, n > 0 ? (n < 40 ? n : 40) : 6, n > 0 ? (const char *)b : "failed");
    free(b);
    return ok ? 0 : -1;
}
/* sceNpBasicSendMessageGui: the console shows a box to confirm (and pick recipients). Here the game's own recipients
 * are used without a box (the game's lists choose them); with none, it is cancelled. */
static void np_basic_send_message_gui(ctx_t *c) {   /* (const SceNpBasicMessageDetails *, container) */
    uint32_t m = ARG32(0);
    if (!npb_handler) { RET(0x8002a66d); return; }
    if (!m) { RET(0x8002a665); return; }
    unsigned main_type = ld_u16(m + 8), sub = ld_u16(m + 10), feat = ld_u32(m + 12);
    uint32_t npids = ld_u32(m + 16), count = ld_u32(m + 20), subj = ld_u32(m + 24), body = ld_u32(m + 28), data = ld_u32(m + 32), size = ld_u32(m + 36);
    if (size > 1024) { RET(0x8002a66b); return; }
    char to[17 * 16 + 1] = ""; int ok = npids && count && count <= 16;
    for (uint32_t i = 0; ok && i < count; i++) {
        char n[17]; memcpy(n, gp(npids + 36 * i), 16); n[16] = 0;
        if (!n[0]) { ok = 0; break; }
        if (i) strcat(to, ","); strcat(to, n);
    }
    int ret = -1;
    if (ok && main_type == 2) ret = npb_add_friend(to);   /* ADD_FRIEND (one recipient) */
    else if (ok) { npb_message(to, main_type, sub, feat, subj ? (const char *)gp(subj) : NULL, body ? (const char *)gp(body) : NULL, data ? gp(data) : NULL, data ? size : 0); ret = 0; }
    static const int result_event[6] = {7, 12, 3, 13, 19, 22};   /* SEND_ATTACHMENT / SEND_MESSAGE / ADD_FRIEND / SEND_INVITATION / SEND_CUSTOM_DATA / SEND_URL_ATTACHMENT _RESULT */
    if (main_type < 6) npb_call(result_event[main_type], ret);
    RET(0);
}
static void np_basic_add_friend(ctx_t *c) {   /* (const SceNpId *contact, const char *body, container): a friend request */
    uint32_t to = ARG32(0);
    if (!npb_handler) { RET(0x8002a66d); return; }
    if (!to || !ld_u8(to)) { RET(0x8002a665); return; }
    char name[17]; memcpy(name, gp(to), 16); name[16] = 0;
    npb_call(3, npb_add_friend(name));   /* ADD_FRIEND_RESULT */
    RET(0);
}
/* sceNpBasicRecvMessageCustom opens the console's list of received invites; the game shows its own (from the
 * INCOMING_INVITATION events), so the box is cancelled, as when the player backs out of it. */
static void np_basic_recv_message_custom(ctx_t *c) { RET(0x8002a671); }   /* SCE_NP_BASIC_ERROR_CANCEL */
/* The "connecting to the network" dialog: already connected, so it loads and finishes at once
 * (CELL_SYSUTIL_NET_CTL_NETSTART_LOADED / FINISHED / UNLOADED); the result {u32 size; s32 result} is success. */
static void net_ctl_netstart_load(ctx_t *c) {
    sysutil_post(0x0801, 0);
    sysutil_post(0x0802, 0);
    RET(0);
}
static void net_ctl_netstart_unload(ctx_t *c) {
    if (ARG32(0)) st_u32(ARG32(0) + 4, net_enabled ? 0 : 0x80130106);
    sysutil_post(0x0803, 0);
    RET(0);
}
/* cellNetCtl: connected (IP obtained) when online */
static void net_ctl_get_info(ctx_t *c) {   /* (s32 code, CellNetCtlInfo *info) */
    if (!net_enabled) { RET(0x80130106); return; }   /* CELL_NET_CTL_ERROR_NOT_CONNECTED */
    uint32_t code = ARG32(0), p = ARG32(1);
    mem_zero(p, 256);
    switch (code) {
    case 1: st_u32(p, 0); break;            /* device: wired */
    case 2: {                               /* ether address: one per account (a shared one made every PC the same console online) */
        const char *id = getenv("MNR_NP_ID");
        uint32_t h = 2166136261u;
        for (const char *s = id && *id ? id : "mnr"; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
        uint8_t *m = gp(p);
        m[0] = 0x00; m[1] = 0x1f; m[2] = 0xa7; m[3] = (uint8_t)(h >> 16); m[4] = (uint8_t)(h >> 8); m[5] = (uint8_t)h;
        static int said; if (!said++) printf("[net] ether address %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
        break;
    }
    case 3: st_u32(p, 1500); break;         /* MTU */
    case 4: st_u32(p, 1); break;            /* link: connected */
    case 5: st_u32(p, 2); break;            /* link type: 100BASE full duplex */
    case 13: st_u32(p, 0); break;           /* IP config: DHCP */
    case 16: strcpy((char *)gp(p), net_local_ip()); break;
    case 17: strcpy((char *)gp(p), "255.255.255.0"); break;
    case 18: case 19: { char r[32]; snprintf(r, sizeof r, "%s", net_local_ip()); char *d = strrchr(r, '.'); if (d) strcpy(d, ".1"); strcpy((char *)gp(p), r); break; }
    case 20: strcpy((char *)gp(p), "8.8.8.8"); break;
    default: break;                         /* proxies off, UPnP off, ... */
    }
    RET(0);
}

/* ------------------------------------------------------------------ guest function hooks (recomp/hooks.txt) */
/* qTaskFunctionManager::OnSpuElfLoaded(decl list, SPU ELF descriptor {+8 name, +0x10 ufg_qtask records,
 * +0x14 their size}): for each record (4 magic words, +0x14 SPU entry, +0x1C uid) bind the PPU declaration
 * with the same uid to the ELF (decl +0x10 = descriptor, +0x14 = SPU entry). Tasks of bound functions are
 * then launched through the SPU launcher (hook_qtask_spu_launch, which queues them for the PPU workers).
 * Every ELF is bound, as on a PS3 (MNR_QTASK_BIND=<names> binds only those, =none none): the bound
 * functions' tasks then run their SPU versions (qtask_run_spu). */
static void qtask_elf_note(uint32_t rec, const char *name);
void hook_qtask_on_spu_elf_loaded(ctx_t *c) {
    uint32_t list = ARG32(0), desc = ARG32(1);
    const char *name = ld_u32(desc + 8) ? (const char *)gp(ld_u32(desc + 8)) : "";
    const char *e = getenv("MNR_QTASK_BIND");
    int bind = !e || !strcmp(e, "all") || (strcmp(e, "none") && strstr(e, name) != NULL);
    if (!bind) return;
    if (ld_u32(desc + 0x14)) qtask_elf_note(ld_u32(desc + 0x10), name);
    int n = 0;
    for (uint32_t rec = ld_u32(desc + 0x10), end = rec + ld_u32(desc + 0x14); rec < end; rec += 0x10) {
        if (ld_u32(rec) != 0x12345678 || ld_u32(rec + 4) != 0x12345678 || ld_u32(rec + 8) != 0x12345678 ||
            ld_u32(rec + 12) != 0x12345678 || !ld_u32(rec + 0x1C)) continue;
        for (uint32_t node = ld_u32(list); node; node = ld_u32(node)) {
            uint32_t decl = ld_u32(node + 4);
            if (ld_u32(decl + 0x1C) != ld_u32(rec + 0x1C)) continue;
            st_u32(rec + 0x10, desc); st_u32(rec + 0x18, ld_u32(decl + 0x18));
            st_u32(decl + 0x14, ld_u32(rec + 0x14)); st_u32(decl + 0x10, desc);
            n++;
            break;
        }
    }
    printf("[hook] qTask SPU ELF '%s': %d functions bound\n", name, n);
}

/* Lua debugging pre-hooks: remember recent table lookups made through the C API */
static __thread struct { uint32_t key, caller; int32_t idx; } lua_lookups[16];
static __thread unsigned lua_lookup_pos;
void prehook_lua_getfield(ctx_t *c) {   /* lua_getfield(L, idx, const char *k) */
    unsigned i = lua_lookup_pos++ & 15;
    lua_lookups[i].key = ARG32(2); lua_lookups[i].idx = (int32_t)ARG32(1); lua_lookups[i].caller = (uint32_t)c->lr;
}
void prehook_lua_gettable(ctx_t *c) {   /* lua_gettable(L, idx): key is on the stack */
    unsigned i = lua_lookup_pos++ & 15;
    lua_lookups[i].key = 0; lua_lookups[i].idx = (int32_t)ARG32(1); lua_lookups[i].caller = (uint32_t)c->lr;
}
/* qTasks with an SPU implementation run it on the SPU core, as on a PS3. A task of a function whose ELF
 * hook_qtask_on_spu_elf_loaded bound carries the SPU ELF image at +0x14 and the function's LS entry at
 * +0x18; the SPU qTask kernel calls entry(ctx, scratch, params at task+0x20..0x2C). The PPU versions are
 * development fallbacks: SceneModel_DrawModels / DrawModelTable (0x20C9D8) are empty stubs, and the
 * culling, track-scene and scene-model tasks keep their data in layouts the SPU versions don't share.
 * With the PPU versions, the per-view model groups in the SceneModel layers (0xFACF20, 0xFAD020) were
 * never built, so no scene model drew: no Track Studio road, props, buildings or ModSpot scenery.
 * The PPU worker still schedules the task and completes it: after the SPU function ran, the task's PPU
 * function is pointed at an empty one (0xEA0A70's) for the rest of 0x7B4318.
 * MNR_QTASK_SPU=0 runs the PPU versions instead; MNR_QTASK_SPU_ELFS=all|<names> picks the programs
 * (RenderQueueTask stays on the PPU by default: its PPU version is complete and the SPU one asserts on
 * DMA alignment here). */
static const char qtask_spu_default[] = "SceneModelTask,SPUOcclusionCullTask,KartTrackSceneTask,TrackViewTask,"
                                        "GroundCoverUpdateTask,FlowfieldTask,ParticleRenderUpdateTask,DecalCreateTask,LightningTask";
int spu_run_func(uint32_t elf, uint32_t init, uint32_t entry, const uint32_t regs[6], uint32_t scratch, uint64_t max_steps);
#define QTASK_EMPTY_OPD G_QTASK_EMPTY_OPD
static struct { uint32_t image, init; char name[32]; } qtask_elfs[32];
static int qtask_nelfs;
static void qtask_elf_note(uint32_t rec, const char *name) {   /* the ELF image holding a ufg_qtask record */
    uint32_t a = rec & ~0x7Fu;
    for (int i = 0; i < 0x2000 && ld_u32(a) != 0x7F454C46; i++) a -= 0x80;
    if (ld_u32(a) != 0x7F454C46 || qtask_nelfs >= 32) return;
    for (int i = 0; i < qtask_nelfs; i++) if (qtask_elfs[i].image == a) return;
    uint32_t init = 0, shoff = ld_u32(a + 0x20), shnum = ld_u16(a + 0x30), shstr = ld_u16(a + 0x32);
    uint32_t strtab = a + ld_u32(a + shoff + 40 * shstr + 16);
    for (uint32_t i = 0; i < shnum; i++)
        if (!strcmp((const char *)gp(strtab + ld_u32(a + shoff + 40 * i)), ".init")) init = ld_u32(a + shoff + 40 * i + 12);
    qtask_elfs[qtask_nelfs].image = a; qtask_elfs[qtask_nelfs].init = init;
    snprintf(qtask_elfs[qtask_nelfs].name, sizeof qtask_elfs[0].name, "%s", name);
    qtask_nelfs++;
}
static void qtask_run_spu(ctx_t *c, uint32_t task) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("MNR_QTASK_SPU"); mode = !(e && *e == '0'); }
    uint32_t fd = ld_u32(task + 0x1C), elf = ld_u32(task + 0x14), entry = ld_u32(task + 0x18);
    if (!mode || !elf || !entry || fd == 0xFFFFFFFF || fd == 0xFFFFFFFE) return;
    int k = 0;
    while (k < qtask_nelfs && qtask_elfs[k].image != elf) k++;
    if (k == qtask_nelfs) return;
    {   /* which SPU programs run (MNR_QTASK_SPU_ELFS=all or a list of names): the rest use their PPU versions */
        static const char *want = (const char *)-1;
        if (want == (const char *)-1) { want = getenv("MNR_QTASK_SPU_ELFS"); if (!want) want = qtask_spu_default; }
        if (strcmp(want, "all") && !strstr(want, qtask_elfs[k].name)) return;
    }
    uint32_t regs[6] = {1, 0x3FF00, ld_u32(task + 0x20), ld_u32(task + 0x24), ld_u32(task + 0x28), ld_u32(task + 0x2C)};
    int r = spu_run_func(elf, qtask_elfs[k].init, entry, regs, 0x3FF00, 60000000);
    st_u32(task + 0x1C, QTASK_EMPTY_OPD);
    static volatile long fails; static volatile long seen[32];
    if (r && InterlockedIncrement(&fails) <= 10) printf("[qtask] SPU function 0x%x of %s (task 0x%08x) failed: %d\n", entry, qtask_elfs[k].name, task, r);
    if (InterlockedIncrement(&seen[k]) == 1) printf("[qtask] running %s functions on the SPU core\n", qtask_elfs[k].name);
}
/* qTask runner (0x7B4318: manager, isMain, task): SPU versions (above); MNR_QTASK_STATS=1 prints how many
 * tasks ran, per PPU function, every 2 seconds (to tell long task chains from stalls). */
void prehook_qtask_run(ctx_t *c) {
    if (ARG32(2)) qtask_run_spu(c, ARG32(2));
    static int on = -1;
    if (on < 0) { const char *e = getenv("MNR_QTASK_STATS"); on = e && *e == '1'; }
    if (!on) return;
    static volatile long total;
    static struct { uint32_t fn; long n; } per[64];
    static SRWLOCK l = SRWLOCK_INIT;
    static DWORD last;
    uint32_t task = ARG32(2), fd = task ? ld_u32(task + 0x1C) : 0, fn = (fd && fd != 0xFFFFFFFF && fd != 0xFFFFFFFE) ? ld_u32(fd) : fd;
    if (fn == G_FN_QTASK_STAT && (total % 997) == 0)
        printf("[qtask] 22f1c0 task 0x%08x args %08x %08x %08x %08x state %016llx\n", task, ld_u32(task + 0x20), ld_u32(task + 0x24),
               ld_u32(task + 0x28), ld_u32(task + 0x2C), (unsigned long long)ld_u64(task + 0x38));
    AcquireSRWLockExclusive(&l);
    total++;
    int i = 0;
    while (i < 64 && per[i].n && per[i].fn != fn) i++;
    if (i < 64) { per[i].fn = fn; per[i].n++; }
    DWORD now = GetTickCount();
    if (now - last > 2000) {
        last = now;
        printf("[qtask] %ld tasks run:", total);
        for (int k = 0; k < 64 && per[k].n; k++) printf(" %08x x%ld", per[k].fn, per[k].n);
        printf("\n");
    }
    ReleaseSRWLockExclusive(&l);
}

/* Havok hkSpuJobThreadPool::waitForCompletion replacement (see hooks.txt) */
void hook_havok_spu_wait(ctx_t *c) {
    static long n;
    uint32_t pool = ARG32(0);
    if (ld_u8(pool + 0x14)) {
        if (n++ < 3) printf("[havok] waitForCompletion on %d SPU workers skipped (jobs ran on the PPU)\n", (int)ld_u32(pool + 0x10));
        st_u8(pool + 0x14, 0);
    }
}

void prehook_async_physics_off(ctx_t *c) {
    static int said;
    if (ld_u8(G_HAVOK_ASYNC_FLAG)) { st_u8(G_HAVOK_ASYNC_FLAG, 0); if (!said++) printf("[havok] async physics job disabled (no SPU thread pool)\n"); }
}

/* MNR_UNLOCK_TRACKS=1 (launcher: "Unlock all career tracks"): every career track counts as unlocked, so a new player
 * can race any track an online room picks. The career's UnlockManager keeps its unlocks in a hash table of 128
 * buckets (entry: +0 hash, +32 unlocked, +33 new, +36 type, +48 next; manager +512 = number unlocked). Right after
 * LoadUnlocks has read a save (prehook_game_printf sees its closing log call) the tracks (type 2, eUAT_TRACK) are
 * marked unlocked. Like a real unlock, the next save keeps them. */
void career_unlock_tracks(uint32_t mgr) {
    static int on = -1;
    if (on < 0) on = getenv("MNR_UNLOCK_TRACKS") && atoi(getenv("MNR_UNLOCK_TRACKS"));
    if (!on || mgr < 0x10000) return;
    int n = 0, tracks = 0;
    for (uint32_t b = 0; b < 128; b++)
        for (uint32_t e = ld_u32(mgr + 4 * b), guard = 0; e && guard < 4096; e = ld_u32(e + 48), guard++)
            if (ld_u32(e + 36) == 2) { tracks++; if (!ld_u8(e + 32)) { st_u8(e + 32, 1); n++; } }
    if (n) st_u32(mgr + 512, ld_u32(mgr + 512) + n);
    printf("[career] unlock table 0x%08x: %d career tracks, %d newly unlocked (MNR_UNLOCK_TRACKS)\n", mgr, tracks, n);
}

void prehook_lua_panic(ctx_t *c) {
    fprintf(stderr, "\n[lua] panic; recent C-API table lookups on this thread (oldest first):\n");
    for (unsigned n = 0; n < 16; n++) {
        unsigned i = (lua_lookup_pos + n) & 15;
        if (!lua_lookups[i].caller) continue;
        fprintf(stderr, "   %s(idx %d, \"%s\") from 0x%08x\n", lua_lookups[i].key ? "getfield" : "gettable", lua_lookups[i].idx,
                lua_lookups[i].key ? (const char *)gp(lua_lookups[i].key) : "<stack key>", lua_lookups[i].caller);
    }
    fflush(stderr);
}

/* ------------------------------------------------------------------ misc system */
static void sys_ppu_thread_once(ctx_t *c) {   /* (once_ctrl *, init opd) */
    static SRWLOCK once_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&once_lock);
    int run = ld_u32(ARG32(0)) == 0;
    if (run) st_u32(ARG32(0), 1);
    ReleaseSRWLockExclusive(&once_lock);
    if (run) {
        uint32_t opd = ARG32(1);
        uint64_t save_r2 = c->r[2], save_lr = c->lr;
        c->r[2] = ld_u32(opd + 4);
        dispatch(c, ld_u32(opd), 0);
        c->r[2] = save_r2; c->lr = save_lr;
    }
    RET(0);
}
static void sys_cache_mount(ctx_t *c) {   /* CellSysCacheParam {char cacheId[32]; char getCachePath[1024]; ...} */
    const char *p = "/dev_hdd1";
    memcpy(gp(ARG32(0) + 32), p, strlen(p) + 1);
    RET(0);   /* CELL_SYSCACHE_RET_OK_CLEARED */
}
/* SPURS event ports: cellSpursAttachLv2EventQueue hands out an SPU-side port number; SPU programs
 * (tasks, jobs) send to it with sys_spu_thread_send_event, which lands in the attached queue. */
/* Ports are freed by cellSpursDetachLv2EventQueue and handed out round-robin over the free ones. They used to be
 * counted up and never freed: by the second race the count wrapped past 63 and re-assigned port 58, the one Wwise's
 * SPU mixing jobs report to, so the audio thread waited forever, its message queue filled and the game froze
 * (2026-10-26, online, both players at the second race's intro). */
static kqueue_t *spurs_port_q[64];   /* where an SPU event on the port goes; kept after a detach for events still in flight */
static uint8_t spurs_port_used[64];   /* attached (a detach frees the number for the next attach) */
static SRWLOCK spurs_port_lock = SRWLOCK_INIT;
static int jobchain_log = -1;
static void spurs_attach_queue(ctx_t *c) {   /* (spurs, queue, u8 *port, isDynamic) */
    kqueue_t *q = kobj_get(ARG32(1));
    if (!q) { RET(0x80410711); return; }   /* CELL_SPURS_CORE_ERROR_INVAL */
    AcquireSRWLockExclusive(&spurs_port_lock);
    int port = -1;
    if (ARG32(3) || !ARG32(2)) {   /* dynamic: the next free one after the last handed out (a freed port is reused only after a full cycle) */
        static int next = 16;
        for (int k = 0; k < 48 && port < 0; k++) { int p = 16 + (next - 16 + k) % 48; if (!spurs_port_used[p]) port = p; }
        if (port >= 0) next = port + 1 > 63 ? 16 : port + 1;
    }
    else { port = ld_u8(ARG32(2)) & 63; if (spurs_port_used[port] && spurs_port_q[port] != q) port = -2; }       /* the port asked for */
    if (port >= 0) { spurs_port_q[port] = q; spurs_port_used[port] = 1; }
    ReleaseSRWLockExclusive(&spurs_port_lock);
    if (port < 0) {
        printf("[spurs] attach queue 0x%08x: %s\n", ARG32(1), port == -1 ? "no free SPU event port" : "port in use");
        RET(port == -1 ? 0x80410710 : 0x80410708); return;   /* CELL_SPURS_CORE_ERROR_AGAIN / _BUSY */
    }
    if (ARG32(2)) st_u8(ARG32(2), (uint8_t)port);
    if (jobchain_log < 0) jobchain_log = getenv("MNR_JOBCHAIN_LOG") ? atoi(getenv("MNR_JOBCHAIN_LOG")) : 0;
    if (jobchain_log > 0) printf("[spurs] SPU event port %d -> queue 0x%08x (%s)\n", port, ARG32(1), ARG32(3) || !ARG32(2) ? "dynamic" : "asked for");
    RET(0);
}
static void spurs_detach_queue(ctx_t *c) {   /* (spurs, port) */
    uint32_t port = ARG32(1) & 63;
    AcquireSRWLockExclusive(&spurs_port_lock);
    spurs_port_used[port] = 0;
    ReleaseSRWLockExclusive(&spurs_port_lock);
    if (jobchain_log > 0) printf("[spurs] SPU event port %u detached\n", port);
    RET(0);
}
/* sys_spu_thread_send_event from SPU code: event {SYS_SPU_THREAD_EVENT_USER_KEY, spu, port << 32 | data0, data1} */
int spurs_send_event(uint32_t port, uint32_t data0, uint32_t data1) {
    kqueue_t *q = spurs_port_q[port & 63];
    if (jobchain_log > 1) printf("[jobchain] event: port %u data0 0x%x data1 0x%x\n", port, data0, data1);
    if (!q) return -1;
    kevent_t e = {0xFFFFFFFF53505501ull, 0x02000000, (uint64_t)(port & 0xFF) << 32 | (data0 & 0xFFFFFF), data1};
    if (kqueue_push(q, e)) return 0;
    {   /* an event lost because the queue is full: whoever waits for it may wait forever (a freeze), so say so */
        static volatile long n; if (InterlockedIncrement(&n) <= 20) printf("[spurs] SPU event on port %u DROPPED: queue full (data0 0x%x data1 0x%x)\n", port, data0, data1);
    }
    return -2;
}

/* ------------------------------------------------------------------ SPURS job chains
 * A job chain is a list of 64-bit commands in guest memory (JOB descriptors, NEXT/CALL/RET jumps,
 * GUARDs the PPU opens with cellSpursJobGuardNotify, SYNC barriers, END). One host thread per running
 * chain walks the list; consecutive jobs run on the SPU worker pool when the chain allows more than one
 * SPU (jobchain_job), and every other command first waits for them. Our own attribute layout (the game
 * never reads it back): +0 command list, +4 job descriptor size, +8 max descriptor size, +12 max contention. */
typedef struct { uint32_t chain, pc, desc_size, max_desc, contention; uint32_t stack[16]; int sp; HANDLE wake; volatile int running;
                 volatile long launched, done; } jobchain_t;
static jobchain_t jobchains[16];
static int njobchains;
static jobchain_t *jobchain_find(uint32_t chain) {
    for (int i = 0; i < njobchains; i++) if (jobchains[i].chain == chain) return &jobchains[i];
    return NULL;
}
static void spurs_jc_attr_init(ctx_t *c) {   /* (jmVer, sdkVer, attr, commandList, sizeJobDesc, maxGrabbed, prio[8], maxContention, autoReq, tag1, tag2, fixedMem, maxSizeJobDesc, initReq) */
    uint32_t attr = ARG32(2);
    mem_zero(attr, 512);
    st_u32(attr, ARG32(3)); st_u32(attr + 4, ARG32(4));
    st_u32(attr + 8, (uint32_t)ld_u64((uint32_t)c->r[1] + 112 + 8 * 4));   /* 13th argument: maxSizeJobDescriptor (stack parameter area) */
    st_u32(attr + 12, ARG32(7));   /* maxContention: SPUs that may run the chain's jobs at once */
    RET(0);
}
static void spurs_jc_create(ctx_t *c) {   /* (spurs, chain, attr) */
    if (jobchain_log < 0) jobchain_log = getenv("MNR_JOBCHAIN_LOG") ? atoi(getenv("MNR_JOBCHAIN_LOG")) : 0;
    if (njobchains >= 16) { RET(0x80410911); return; }
    jobchain_t *j = &jobchains[njobchains++];
    memset(j, 0, sizeof *j);
    j->chain = ARG32(1); j->pc = ld_u32(ARG32(2)); j->desc_size = ld_u32(ARG32(2) + 4); j->max_desc = ld_u32(ARG32(2) + 8);
    j->contention = ld_u32(ARG32(2) + 12);
    j->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (jobchain_log) printf("[jobchain] create 0x%08x: commands at 0x%08x, descriptors %u bytes (max %u), max contention %u\n", j->chain, j->pc, j->desc_size, j->max_desc, j->contention);
    RET(0);
}
/* guard (128 bytes, ours): +0 remaining notifications, +4 notify count, +8 auto reset, +12 chain */
static void spurs_jc_guard_init(ctx_t *c) {   /* (chain, guard, notifyCount, requestSpuCount, autoReset) */
    uint32_t g = ARG32(1);
    mem_zero(g, 128);
    st_u32(g, ARG32(2)); st_u32(g + 4, ARG32(2)); st_u32(g + 8, ARG32(4)); st_u32(g + 12, ARG32(0));
    RET(0);
}
static void spurs_jc_guard_notify(ctx_t *c) {   /* (guard) */
    uint32_t g = ARG32(0);
    volatile LONG *n = (volatile LONG *)gp(g);   /* big-endian in guest memory: update under the chain's view */
    for (;;) {
        LONG old = *n; uint32_t v = __builtin_bswap32((uint32_t)old);
        if (!v) break;
        if (InterlockedCompareExchange(n, (LONG)__builtin_bswap32(v - 1), old) == old) break;
    }
    jobchain_t *j = jobchain_find(ld_u32(g + 12));
    if (j) SetEvent(j->wake);
    RET(0);
}
static void spurs_jc_guard_reset(ctx_t *c) { uint32_t g = ARG32(0); st_u32(g, ld_u32(g + 4)); RET(0); }

int spu_run_job(uint32_t job, uint32_t desc_size);   /* spu.c */
/* Jobs between two other commands may run at once (several SPUs take jobs from a chain until a SYNC):
 * they go to the SPU worker pool, and the chain waits for them before any command that is not a job. */
static void jobchain_job(jobchain_t *j, uint32_t job, uint32_t size) {
    spu_async_start();
    if (async_workers && j->contention > 1) { j->launched++; spu_async_push(0, NULL, job, size, &j->done); return; }
    int r = spu_run_job(job, size);
    if (r && jobchain_log) printf("[jobchain] job 0x%08x failed (%d)\n", job, r);
}
static void jobchain_drain(jobchain_t *j) {   /* spin a little, then sleep until a worker finishes one (spinning took a core) */
    for (int spins = 0; j->done < j->launched; spins++) {
        if (spins < 64) { SwitchToThread(); continue; }
        long cur = j->done;
        if (cur < j->launched) WaitOnAddress((volatile void *)&j->done, &cur, sizeof cur, 2);
    }
}
static DWORD WINAPI jobchain_thread(void *p) {
    jobchain_t *j = p;
    static ctx_t jc_ctx; thread_register(&jc_ctx, "SPURS job chain");
    while (j->running) {
        uint64_t cmd = ld_u64(j->pc);
        uint32_t op = (uint32_t)cmd & 7, ea = (uint32_t)cmd & ~0xFu;
        if (op != 6 && !(op == 0 && cmd)) jobchain_drain(j);
        if (jobchain_log > 1) printf("[jobchain] 0x%08x: %016llx%s %08x\n", j->pc, (unsigned long long)cmd, (cmd & 7) == 0 && cmd ? " job, binary" : "",
                                     (cmd & 7) == 0 && cmd ? (uint32_t)ld_u64((uint32_t)cmd & ~0xFu) : 0);
        switch (op) {
        case 0:
            if (cmd) jobchain_job(j, (uint32_t)cmd & ~0xFu, j->desc_size);   /* JOB */
            j->pc += 8; break;
        case 1: j->pc = (uint32_t)cmd & ~7u; break;                 /* RESET_PC */
        case 2: case 5: j->pc += 8; break;                           /* SYNC / LWSYNC / labels, FLUSH: jobs run in order */
        case 3: j->pc = (uint32_t)cmd & ~7u; break;                  /* NEXT */
        case 4: if (j->sp < 16) j->stack[j->sp++] = j->pc + 8; j->pc = (uint32_t)cmd & ~7u; break;   /* CALL */
        case 6: {   /* JOBLIST: {u32 numJobs, u32 sizeOfJob, u64 eaJobList} */
            uint32_t l = (uint32_t)cmd & ~7u, n = ld_u32(l), sz = ld_u32(l + 4), base = (uint32_t)ld_u64(l + 8);
            for (uint32_t i = 0; i < n; i++) jobchain_job(j, base + i * sz, sz);
            j->pc += 8; break;
        }
        case 7:
            switch (((uint32_t)cmd >> 3) & 15) {
            case 1: {   /* GUARD: wait until the PPU has notified it often enough */
                uint32_t g = (uint32_t)cmd & ~0x7Fu;
                while (j->running && ld_u32(g)) WaitForSingleObject(j->wake, 5);
                if (ld_u32(g + 8)) st_u32(g, ld_u32(g + 4));
                j->pc += 8; break;
            }
            case 14: j->pc = j->sp ? j->stack[--j->sp] : j->pc + 8; break;   /* RET */
            case 15: case 0:                                              /* END, ABORT */
                if (jobchain_log) printf("[jobchain] 0x%08x ended at 0x%08x\n", j->chain, j->pc);
                j->running = 0; break;
            default: j->pc += 8; break;                                   /* SET_LABEL, ... */
            }
            break;
        }
    }
    return 0;
}
static void spurs_jc_run(ctx_t *c) {   /* (chain) */
    jobchain_t *j = jobchain_find(ARG32(0));
    if (!j) { RET(0x80410902); return; }   /* CELL_SPURS_JOB_ERROR_INVAL */
    if (!j->running) { j->running = 1; CreateThread(NULL, 1 << 20, jobchain_thread, j, 0, NULL); }
    RET(0);
}
static void audio_out_get_state(ctx_t *c) {   /* (audioOut, deviceIndex, CellAudioOutState *) */
    uint32_t s = ARG32(2);   /* {u8 state, encoder, reserved[6]; u32 downMixer; CellAudioOutSoundMode {u8 type, channel, fs, reserved; u32 layout}} */
    mem_zero(s, 0x14);
    st_u8(s, 0);             /* CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED (the game polls for it after configuring) */
    st_u8(s + 12, 0);        /* CELL_AUDIO_OUT_CODING_TYPE_LPCM */
    st_u8(s + 13, 2);        /* 2 channels */
    st_u8(s + 14, 4);        /* CELL_AUDIO_OUT_FS_48KHZ */
    st_u32(s + 16, 1);       /* CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH */
    RET(0);
}
/* cellAudio: see audio.c (ports, read indices, waveOut output) */
#include "audio.h"
#include "input.h"
static uint32_t audio_alloc_cb(uint32_t size, uint32_t align) { return user_alloc(size, align); }
static void hle_audio_init(ctx_t *c) { audio_guest_alloc = audio_alloc_cb; RET(audio_init()); }
static void hle_audio_port_open(ctx_t *c) {   /* (CellAudioPortParam {u64 nChannel, nBlock, attr; float level}*, u32 *portNum) */
    uint32_t prm = ARG32(0), port;
    float level = (ld_u64(prm + 16) & 0x1000) ? ld_f32(prm + 24) : 1.0f;   /* CELL_AUDIO_PORTATTR_INITLEVEL */
    if (audio_port_open((uint32_t)ld_u64(prm), (uint32_t)ld_u64(prm + 8), level, &port)) { RET(0x80310705); return; }   /* PORT_FULL */
    st_u32(ARG32(1), port);
    RET(0);
}
static void hle_audio_get_port_config(ctx_t *c) { RET(audio_port_config(ARG32(0), ARG32(1)) ? 0x80310704 : 0); }
static void hle_audio_port_start(ctx_t *c) { RET(audio_port_start(ARG32(0)) ? 0x80310704 : 0); }
static void hle_audio_port_stop(ctx_t *c) { RET(audio_port_stop(ARG32(0)) ? 0x80310704 : 0); }
static void hle_audio_port_close(ctx_t *c) { RET(audio_port_close(ARG32(0)) ? 0x80310704 : 0); }
static void hle_audio_set_level(ctx_t *c) { RET(audio_port_level(ARG32(0), (float)F(1)) ? 0x80310704 : 0); }
static void net_ctl_get_state(ctx_t *c) { st_u32(ARG32(0), net_enabled ? 3 : 0); RET(0); }   /* IPObtained / Disconnected */
/* cellPad: port 0 is a DualShock 3 driven by the keyboard (and the first XInput controller), read only while the console
 * window has focus; port 1, for split screen, is player 2's XInput controller (see xi_slot), connected only while one is plugged in.
 * Arrows = d-pad and left stick, Enter = START, Backspace = SELECT, Z/Space = Cross, X = Circle,
 * A = Square, S = Triangle, Q/W = L1/R1, 1/2 = L2/R2. MNR_AUTOPRESS=1 taps Start, then Cross, every
 * 3 seconds (for headless runs). */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xi_pad_t;
static int xinput_read(DWORD slot, xi_pad_t *out);
/* Which XInput slot a port reads: normally slot 0 is player 1 and slot 1 player 2; with a controller from the
 * launcher's Controls page connected (input.c), that one is player 1 and XInput slot 0 is player 2 */
static int xi_slot(unsigned port) {
    int bound = input_bound_present(); static int was = -1;
    if (bound != was) { was = bound; printf("[pad] %s\n", bound ? "controller from the Controls page is player 1, the Xbox controller player 2" : "Xbox controllers: first is player 1, second player 2"); }
    return port == 0 ? (bound ? -1 : 0) : (bound ? 0 : 1); }
static int xinput_port(unsigned port, xi_pad_t *out) { int s = xi_slot(port); return s >= 0 && xinput_read((DWORD)s, out); }
static int pad2_fake(void) { static int on = -1; if (on < 0) { const char *e = getenv("MNR_PAD2"); on = e && *e == '1'; } return on; }   /* tests */
static int pad_script(unsigned port, unsigned *d1, unsigned *d2);
static void pad_get_info2(ctx_t *c) {   /* CellPadInfo2 {max_connect, now_connect, system_info, port_status[7], port_setting[7], device_capability[7], device_type[7]} */
    uint32_t p = ARG32(0);
    mem_zero(p, 0x100);
    st_u32(p, 7);
    st_u32(p + 12, 1);              /* port 0: CELL_PAD_STATUS_CONNECTED */
    st_u32(p + 12 + 56, 1 | 2);     /* PS3 conformity, pressure-sensitive */
    xi_pad_t xp; int two = xinput_port(1, &xp) || pad2_fake();   /* port 1 (split screen): player 2's XInput controller */
    static int had = -1;
    if (two) { st_u32(p + 16, 1 | (had != 1 ? 2 : 0)); st_u32(p + 16 + 56, 1 | 2); }   /* (2: ASSIGN_CHANGES, just plugged in) */
    else if (had == 1) st_u32(p + 16, 2);   /* just unplugged */
    if (two != had) printf("[pad] second controller %s (port 1)\n", two ? "connected" : had < 0 ? "not connected" : "disconnected");
    had = two;
    st_u32(p + 4, 1 + two);
    RET(0);
}
static int key(int vk);
int mnr_key_down(int vk) { return key(vk); }   /* voice.c: push to talk */
static int key(int vk) {
    static HWND con; static int con_known;   /* (asked once: without a console it returns NULL, and at 15 us a call the
                                              * 20 key checks per pad read cost the main thread 0.3 ms a frame) */
    if (!con_known) { con = GetConsoleWindow(); con_known = 1; }
    HWND fg = GetForegroundWindow();
    if (osk_typing) return 0;   /* the keyboard is typing text */
    return fg && (fg == con || fg == rsx_window) && (GetAsyncKeyState(vk) & 0x8000);
}
/* XInput (Xbox-layout controllers: slot 0 is player 1, slot 1 player 2 for split screen), loaded at runtime: buttons
 * map by position (A = Cross, B = Circle, X = Square, Y = Triangle), LB/RB = L1/R1, triggers = L2/R2 (analog), Back = Select. */
typedef struct { DWORD packet; xi_pad_t pad; } xi_state_t;
static int xinput_read(DWORD slot, xi_pad_t *out) {
    static DWORD (WINAPI *get)(DWORD, xi_state_t *); static int tried; static DWORD next_try[2];
    if (!tried) {
        tried = 1;
        if (getenv("MNR_NO_XINPUT")) return 0;   /* test runs: a controller in use elsewhere must not drive them */
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (DWORD (WINAPI *)(DWORD, xi_state_t *))GetProcAddress(m, "XInputGetState");
    }
    if (!get || slot > 1 || GetTickCount() < next_try[slot]) return 0;
    xi_state_t st;
    if (get(slot, &st) != 0) { next_try[slot] = GetTickCount() + 1000; return 0; }   /* not connected: poll again in a second */
    *out = st.pad;
    return 1;
}
static unsigned stick8(int v, int invert) {   /* XInput -32768..32767 -> DualShock 0..255 (y down = 255), with a dead zone */
    if (v > -7849 && v < 7849) return 0x80;
    int u = (v + 32768) >> 8;
    return (unsigned)(invert ? 255 - u : u);
}
/* MNR_PAD_SCRIPT="<s>:<buttons>;<s>:<buttons>;..." (tests): at <s> seconds after the clock starts (MNR_PAD_SCRIPT_AFTER,
 * or the first pad read), press <buttons> for 150 ms: '+'-joined names up down left right start select x o sq tri l1 r1
 * l2 r2 l3 r3, "hold<ms>" to make that press longer, "shot" for a screenshot (shot_<flip>.bmp), "p2" for player 2's pad
 * (split screen; MNR_PAD2=1 plugs it in). While a script runs, MNR_AUTOPRESS stays quiet. */
volatile ULONGLONG pad_script_t0;
volatile int pad_script_shot;
static int pad_script(unsigned port, unsigned *d1, unsigned *d2) {   /* 1 while the script is active */
    static int n = -1; static struct { unsigned at, ms, b1, b2, shot, port; } ev[256]; static int fired[256];
    if (n < 0) {
        n = 0;
        const char *e = getenv("MNR_PAD_SCRIPT");
        for (const char *p = e; p && *p && n < 256; ) {
            char item[128]; const char *q = strchr(p, ';'); size_t l = q ? (size_t)(q - p) : strlen(p);
            if (l >= sizeof item) l = sizeof item - 1;
            memcpy(item, p, l); item[l] = 0; p = q ? q + 1 : p + l;
            char *colon = strchr(item, ':'); if (!colon) continue;
            *colon = 0; ev[n].at = (unsigned)(atof(item) * 1000); ev[n].ms = 150; ev[n].b1 = ev[n].b2 = ev[n].shot = ev[n].port = 0;
            for (char *t = strtok(colon + 1, "+"); t; t = strtok(NULL, "+")) {
                static const struct { const char *nm; unsigned b1, b2; } names[] = {
                    {"left", 0x80, 0}, {"down", 0x40, 0}, {"right", 0x20, 0}, {"up", 0x10, 0}, {"start", 0x08, 0}, {"select", 0x01, 0}, {"l3", 0x02, 0}, {"r3", 0x04, 0},
                    {"sq", 0, 0x80}, {"x", 0, 0x40}, {"o", 0, 0x20}, {"tri", 0, 0x10}, {"r1", 0, 0x08}, {"l1", 0, 0x04}, {"r2", 0, 0x02}, {"l2", 0, 0x01}};
                if (!strcmp(t, "shot")) { ev[n].shot = 1; continue; }
                if (!strcmp(t, "p2")) { ev[n].port = 1; continue; }
                if (!strncmp(t, "hold", 4)) { ev[n].ms = (unsigned)atoi(t + 4); continue; }
                for (unsigned k = 0; k < sizeof names / sizeof names[0]; k++) if (!strcmp(t, names[k].nm)) { ev[n].b1 |= names[k].b1; ev[n].b2 |= names[k].b2; }
            }
            n++;
        }
        if (n) printf("[padscript] %d steps\n", n);
    }
    if (!n) return 0;
    if (!getenv("MNR_PAD_SCRIPT_AFTER") && !pad_script_t0) pad_script_t0 = GetTickCount64();
    if (!pad_script_t0) return 0;   /* waiting for MNR_PAD_SCRIPT_AFTER: the usual automatic presses (title screen) */
    unsigned now = (unsigned)(GetTickCount64() - pad_script_t0);
    for (int i = 0; i < n; i++) {
        if (ev[i].port != port) continue;
        if (now >= ev[i].at && now < ev[i].at + ev[i].ms) { *d1 |= ev[i].b1; *d2 |= ev[i].b2; }
        if (now >= ev[i].at && !fired[i]) { fired[i] = 1; if (ev[i].shot) pad_script_shot = 1; printf("[padscript] step %d at %.1f s\n", i, now / 1000.0); }
    }
    return 1;
}
static void pad_write(uint32_t d, unsigned d1, unsigned d2, const xi_pad_t *xpp, const input_state_t *jsp);
static void pad_get_data(ctx_t *c) {   /* (port, CellPadData {s32 len; u16 button[64]}) */
    uint32_t d = ARG32(1);
    mem_zero(d, 0x84);
    if (ARG32(0) == 1) {   /* player 2: the second controller only (MNR_PAD2=1, tests: a pad driven by the script's "p2" steps) */
        xi_pad_t xp; unsigned s1 = 0, s2 = 0;
        static int said; if (!said++) printf("[pad] the game reads player 2's pad (port 1)\n");
        if (xinput_port(1, &xp)) pad_write(d, 0, 0, &xp, NULL);
        else if (pad2_fake()) { pad_script(1, &s1, &s2); pad_write(d, s1, s2, NULL, NULL); }
        RET(0); return;
    }
    if (ARG32(0) != 0) { RET(0); return; }
    static int autopress = -1;
    if (autopress < 0) { const char *e = getenv("MNR_AUTOPRESS"); autopress = e && *e == '1'; }
    unsigned d1 = 0, d2 = 0;
    if (key(VK_LEFT)) d1 |= 0x80;  if (key(VK_DOWN)) d1 |= 0x40;
    if (key(VK_RIGHT)) d1 |= 0x20; if (key(VK_UP)) d1 |= 0x10;
    if (key(VK_RETURN)) d1 |= 0x08; if (key(VK_BACK)) d1 |= 0x01;
    if (key('A')) d2 |= 0x80; if (key('Z') || key(VK_SPACE)) d2 |= 0x40;
    if (key('X')) d2 |= 0x20; if (key('S')) d2 |= 0x10;
    if (key('W')) d2 |= 0x08; if (key('Q')) d2 |= 0x04;
    if (key('2')) d2 |= 0x02; if (key('1')) d2 |= 0x01;
    static long autodrive = -2;
    if (autodrive == -2) { const char *e = getenv("MNR_AUTODRIVE"); autodrive = e ? atol(e) : -1; }
    static int noskip = -1; if (noskip < 0) noskip = getenv("MNR_AUTOPRESS_NOSKIP") != NULL;
    LARGE_INTEGER hq, hf; QueryPerformanceCounter(&hq); QueryPerformanceFrequency(&hf);
    int hold = noskip && ((fs_movies_open > 0 && hq.QuadPart - fs_movie_read_qpc < 2 * hf.QuadPart) || GetTickCount64() < nis_hold_until);   /* let videos and cutscenes play out */
#ifdef G_AI_CONTROLS_HUMANS
    {   /* MNR_AI_PLAYER=1 (tests): the game's own debug option "AIControlsHumans", so the AI drives the player's kart;
         * =2: only once a race track (.TRK) has been opened (in ModSpot the AI drove off and menu presses did nothing);
         * =3: only once the race itself has begun (split-screen setup menus come after ModSpot's track is open) */
        static int ai = -1; if (ai < 0) { const char *e = getenv("MNR_AI_PLAYER"); ai = e ? atoi(e) : 0; }
        if (ai == 1 || (ai == 2 && race_track_opened) || (ai == 3 && race_began)) st_u8(G_AI_CONTROLS_HUMANS, 1);
    }
#endif
    if (pad_script(0, &d1, &d2)) ;   /* MNR_PAD_SCRIPT: scripted presses only */
    else if (hold) ;
    else if (autodrive_file_tick && GetTickCount64() < autodrive_file_tick) ;   /* MNR_AUTODRIVE_AFTER: waiting, no presses */
    else if ((autodrive >= 0 && rsx_flips >= autodrive) || autodrive_file_tick) {   /* MNR_AUTODRIVE=<flip>: from then on hold R2 (accelerate) and tap Cross (dismisses tutorial panels), no Start */
        d2 |= 0x02;
        if (GetTickCount() % 3000 >= 1500 && GetTickCount() % 3000 < 1700) d2 |= 0x40;
        static long drift = -2; if (drift == -2) { const char *e = getenv("MNR_AUTODRIFT"); drift = e ? atol(e) : -1; }
        if (drift >= 0 && rsx_flips >= drift) {   /* MNR_AUTODRIFT=<flip>: from then on also drift (R1 + steer) left then right, to fill the boost gauge (testing) */
            DWORD t = GetTickCount() % 4000;
            if (t < 1500) d2 |= 0x08, d1 |= 0x80; else if (t >= 2000 && t < 3500) d2 |= 0x08, d1 |= 0x20;
        }
    }
    else if (autopress) {   /* 3 s cycle: Start held 0.0-0.2 s, Cross held 1.5-1.7 s */
        DWORD t = GetTickCount() % 3000;
        if (t < 200) d1 |= 0x08;
        else if (t >= 1500 && t < 1700) d2 |= 0x40;
    }
    xi_pad_t xp; int xi = xinput_port(0, &xp);
    input_state_t js; int jy = input_read(&js);   /* wheels, pedals, other controllers (MNR_INPUT_CONFIG) */
    pad_write(d, d1, d2, xi ? &xp : NULL, jy ? &js : NULL);
    RET(0);
}
/* CellPadData for one pad: buttons already pressed (keyboard, scripts), an XInput controller and a wheel, if any. */
static void pad_write(uint32_t d, unsigned d1, unsigned d2, const xi_pad_t *xpp, const input_state_t *jsp) {
    int xi = xpp != NULL, jy = jsp != NULL;
    xi_pad_t xp = xi ? *xpp : (xi_pad_t){0};
    input_state_t js = jy ? *jsp : (input_state_t){0};
    unsigned lt = 0, rt = 0;
    if (xi) {
        WORD b = xp.buttons;
        if (b & 0x0004) d1 |= 0x80; if (b & 0x0002) d1 |= 0x40; if (b & 0x0008) d1 |= 0x20; if (b & 0x0001) d1 |= 0x10;
        if (b & 0x0010) d1 |= 0x08; if (b & 0x0080) d1 |= 0x04; if (b & 0x0040) d1 |= 0x02; if (b & 0x0020) d1 |= 0x01;
        if (b & 0x4000) d2 |= 0x80; if (b & 0x1000) d2 |= 0x40; if (b & 0x2000) d2 |= 0x20; if (b & 0x8000) d2 |= 0x10;
        if (b & 0x0200) d2 |= 0x08; if (b & 0x0100) d2 |= 0x04;
        lt = xp.lt; rt = xp.rt;
        if (rt > 30) d2 |= 0x02; if (lt > 30) d2 |= 0x01;
    }
    if (jy) { d1 |= js.d1; d2 |= js.d2; }
    st_u32(d, 24);                       /* len: digital + analog + pressure fields */
    st_u16(d + 4 + 2 * 2, d1);           /* button[2] digital 1 */
    st_u16(d + 4 + 3 * 2, d2);           /* button[3] digital 2 */
    unsigned rx = xi ? stick8(xp.rx, 0) : 0x80, ry = xi ? stick8(xp.ry, 1) : 0x80;
    unsigned lx = xi ? stick8(xp.lx, 0) : 0x80, ly = xi ? stick8(xp.ly, 1) : 0x80;
    if (lx == 0x80) lx = d1 & 0x80 ? 0x00 : d1 & 0x20 ? 0xFF : 0x80;   /* keyboard arrows / d-pad drive the left stick too */
    if (ly == 0x80) ly = d1 & 0x10 ? 0x00 : d1 & 0x40 ? 0xFF : 0x80;
    if (jy) { if (js.lx >= 0) lx = (unsigned)js.lx; if (js.ly >= 0) ly = (unsigned)js.ly; if (js.rx >= 0) rx = (unsigned)js.rx; if (js.ry >= 0) ry = (unsigned)js.ry; }
    st_u16(d + 4 + 4 * 2, rx);           /* right stick x, y */
    st_u16(d + 4 + 5 * 2, ry);
    st_u16(d + 4 + 6 * 2, lx);           /* left stick x, y */
    st_u16(d + 4 + 7 * 2, ly);
    static const unsigned pmap[12][2] = {   /* pressure: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 */
        {1, 0x20}, {1, 0x80}, {1, 0x10}, {1, 0x40}, {2, 0x10}, {2, 0x20}, {2, 0x40}, {2, 0x80}, {2, 0x04}, {2, 0x08}, {2, 0x01}, {2, 0x02}};
    for (int i = 0; i < 12; i++)
        st_u16(d + 4 + (8 + i) * 2, ((pmap[i][0] == 1 ? d1 : d2) & pmap[i][1]) ? 0xFF : 0);
    if (lt > 30) st_u16(d + 4 + 18 * 2, lt);   /* analog L2/R2 pressure from the triggers */
    if (rt > 30) st_u16(d + 4 + 19 * 2, rt);
    if (jy && js.l2 > 0) st_u16(d + 4 + 18 * 2, (unsigned)js.l2);   /* pedals: their own pressure, light presses too */
    if (jy && js.r2 > 0) st_u16(d + 4 + 19 * 2, (unsigned)js.r2);
}

/* ------------------------------------------------------------------ cellFs over the extracted disc
 * /dev_bdvd and /app_home -> <MNR>/extracted, /dev_hdd0 -> <MNR>/hdd0, /dev_hdd1 -> <MNR>/hdd1. */
#define CELL_FS_ENOENT 0x80010006u
#define CELL_FS_EBADF  0x8001002Au
#define CELL_FS_EEXIST 0x80010014u
#define CELL_FS_EMFILE 0x80010025u
static char mnr_root[MAX_PATH];
/* Creations' paint jobs are composed on the GPU once and cached (DATA\COMPOSITES\LOCAL_2\COMPRESSED, keyed by the
 * creation's data). Ones composed before the shader fix of 2026-11-01 (source precision modifiers, rsx_hlsl.c) can have
 * pattern stamps spread over whole panels, so that cache is emptied once (the game composes them again as needed). */
static void composites_reset_once(void) {
    char dir[MAX_PATH], mark[MAX_PATH], pat[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\hdd0\\game\\BCUS98167\\USRDIR\\DATA\\COMPOSITES\\LOCAL_2\\COMPRESSED", mnr_root);
    snprintf(mark, sizeof mark, "%s\\..\\mnr_composed_v2", dir);
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES) return;
    snprintf(pat, sizeof pat, "%s\\*.BIN", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do { char f[MAX_PATH]; snprintf(f, sizeof f, "%s\\%s", dir, fd.cFileName); if (DeleteFileA(f)) n++; } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    FILE *m = fopen(mark, "w"); if (m) { fputs("paint jobs composed with the 2026-11-01 shader fix\n", m); fclose(m); }
    printf("[composites] %d cached paint jobs cleared (composed again with the shader fix)\n", n);
}
typedef struct { FILE *f; HANDLE dir; WIN32_FIND_DATAA fd; int dir_first; char path[MAX_PATH];  int movie; } fsh_t;
static fsh_t *fs_handles[256];
static SRWLOCK fs_lock = SRWLOCK_INIT;

static int fs_host_path(const char *g, char *out, size_t cap) {
    static const struct { const char *prefix, *dir; } map[] = {
        {"/dev_bdvd", "extracted"}, {"/app_home", "extracted/PS3_GAME/USRDIR"},
        {"/dev_hdd0", "hdd0"}, {"/dev_hdd1", "hdd1"}, {"/host_root", "extracted"},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t n = strlen(map[i].prefix);
        if (!_strnicmp(g, map[i].prefix, n) && (g[n] == '/' || g[n] == 0)) {
            snprintf(out, cap, "%s/%s%s", mnr_root, map[i].dir, g + n);
            for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
            return 1;
        }
    }
    return 0;
}
/* Mods (MNR_MODS=<dir>;<dir>..., set by the launcher; the first mod that has a file wins): <mod>/files/ mirrors the
 * game's USRDIR, e.g. <mod>/files/DATA/WORLD/THEMEDEFS.XML replaces USRDIR/DATA/WORLD/THEMEDEFS.XML whether the game
 * reads it from the disc or the installed copy. Only reads are redirected. */
static int fs_mod_path(const char *g, char *out, size_t cap) {
    static char *mods[32]; static int nmods = -1;
    if (nmods < 0) {
        nmods = 0;
        const char *m = getenv("MNR_MODS");
        while (m && *m && nmods < 32) {
            const char *end = strchr(m, ';'); size_t n = end ? (size_t)(end - m) : strlen(m);
            if (n) { mods[nmods] = malloc(n + 1); memcpy(mods[nmods], m, n); mods[nmods][n] = 0; printf("[mods] %s\n", mods[nmods]); nmods++; }
            m = end ? end + 1 : NULL;
        }
    }
    if (!nmods) return 0;
    static const char *roots[] = {"/dev_bdvd/PS3_GAME/USRDIR/", "/dev_hdd0/game/BCUS98167/USRDIR/", "/app_home/"};
    const char *rest = NULL;
    for (int i = 0; i < 3 && !rest; i++) { size_t n = strlen(roots[i]); if (!_strnicmp(g, roots[i], n)) rest = g + n; }
    if (!rest || !*rest) return 0;
    while (*rest == '/') rest++;
    for (int i = 0; i < nmods; i++) {
        snprintf(out, cap, "%s\\files\\%s", mods[i], rest);
        for (char *q = out; *q; q++) if (*q == '/') *q = '\\';
        DWORD at = GetFileAttributesA(out);
        if (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY)) {
            static int nlog; if (nlog++ < 200) printf("[mods] %s -> %s\n", g, out);
            return 1;
        }
    }
    return 0;
}
/* Creations under USRDIR\1\DATA are named <8 hex id>_<name>; the game asks for them by the name kept in its save
 * (000013f6_GirDogSuit.sav) while the launcher installs downloads under their display name (000013F6_Gir Dog Suit.SAV),
 * which left downloaded characters unloaded (the default look; black outside the Creation Station). A missing file
 * there is looked up by its id among files whose names match ignoring case, spaces and punctuation. */
static void fs_name_key(const char *s, char *k, size_t cap) {
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) if (isalnum((unsigned char)*s)) k[n++] = (char)toupper((unsigned char)*s);
    k[n] = 0;
}
static int fs_creation_fallback(char *out, size_t cap) {
    char *file = strrchr(out, '\\');
    char up[MAX_PATH]; snprintf(up, sizeof up, "%s", out); _strupr(up);
    if (!file || !strstr(up, "\\USRDIR\\1\\DATA\\") || GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 0;
    file++;
    if (strlen(file) < 10 || file[8] != '_') return 0;
    for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)file[i])) return 0;
    char want[MAX_PATH], pat[MAX_PATH], key[MAX_PATH];
    fs_name_key(file + 9, want, sizeof want);
    snprintf(pat, sizeof pat, "%.*s%.8s_*", (int)(file - out), out, file);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    /* else the one file with this id and the same kind (extension; _SMALL picture or not): the launcher cut names longer
     * than 24 characters short ("Sonic Kart - Transformed" for the game's "...Transformed2"), and a missing kart there
     * crashed the game on leaving the studio. Ids are unique in a folder (the launcher won't install a second). */
    const char *ext = strrchr(file, '.'); if (!ext) ext = file + strlen(file);
    int small = ext - file >= 6 && !_strnicmp(ext - 6, "_SMALL", 6), nkind = 0;
    char kind[MAX_PATH] = "";
    do {
        fs_name_key(fd.cFileName + 9, key, sizeof key);
        if (!strcmp(key, want)) { snprintf(file, cap - (size_t)(file - out), "%s", fd.cFileName); found = 1; break; }
        const char *e2 = strrchr(fd.cFileName, '.'); if (!e2) e2 = fd.cFileName + strlen(fd.cFileName);
        int s2 = e2 - fd.cFileName >= 6 && !_strnicmp(e2 - 6, "_SMALL", 6);
        if (!_stricmp(e2, ext) && s2 == small) { nkind++; snprintf(kind, sizeof kind, "%s", fd.cFileName); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!found && nkind == 1) { snprintf(file, cap - (size_t)(file - out), "%s", kind); found = 1; }
    if (found) { static int nlog; if (nlog++ < 50) printf("[fs] creation file found as %s\n", out); }
    return found;
}
static int fs_host_path_read(const char *g, char *out, size_t cap) {
    if (fs_mod_path(g, out, cap)) return 1;
    if (!fs_host_path(g, out, cap)) return 0;
    fs_creation_fallback(out, cap);
    return 1;
}
static void fs_mkdirs(const char *path) {   /* create parent directories of a host path */
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++) if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
}
volatile int race_track_opened;   /* a .TRK was opened (MNR_AI_PLAYER=2) */
volatile int race_began;          /* the game logged GameStateKart_Race OnEnter (MNR_AI_PLAYER=3) */
static int fs_alloc(fsh_t *h) {
    AcquireSRWLockExclusive(&fs_lock);
    for (int i = 3; i < 256; i++) if (!fs_handles[i]) { fs_handles[i] = h; ReleaseSRWLockExclusive(&fs_lock); return i; }
    ReleaseSRWLockExclusive(&fs_lock);
    return -1;
}
static fsh_t *fs_get(uint32_t fd) { return fd < 256 ? fs_handles[fd] : NULL; }
static int fs_log;

static void cell_fs_open(ctx_t *c) {   /* (path, flags, *fd, arg, size) */
    const char *g = (const char *)gp(ARG32(0));
    uint32_t flags = ARG32(1);
    char hp[MAX_PATH];
    int reading = !(flags & 3) && !(flags & 0x240);
    if (!(reading ? fs_host_path_read : fs_host_path)(g, hp, sizeof hp)) { if (fs_log) printf("[fs] open %s -> unmapped\n", g); RET(CELL_FS_ENOENT); return; }
    const char *mode = "rb";
    uint32_t acc = flags & 3;
    if (((flags & 3) || (flags & 0x240)) && !fs_writable(hp)) {   /* the disc is read-only */
        if (fs_log) printf("[fs] open %s (flags 0x%x) -> read-only\n", g, flags);
        RET(CELL_FS_ENOENT); return;
    }
    if (flags & 0x40) fs_mkdirs(hp);   /* CREAT */
    if (acc == 0) mode = "rb";
    else if (flags & 0x400) mode = acc == 1 ? "ab" : "a+b";                               /* APPEND */
    else if ((flags & 0x200) || ((flags & 0x40) && GetFileAttributesA(hp) == INVALID_FILE_ATTRIBUTES)) mode = acc == 1 ? "wb" : "w+b";
    else mode = "r+b";
    FILE *f = fopen(hp, mode);
    /* another program holding the file (a picture viewer, Explorer's preview, antivirus): the game reports a failed
     * save as "not enough space in the HDD", so wait for it a little (up to 2 s) before giving up */
    for (int tries = 0; !f && acc != 0 && tries < 40 && GetLastError() == ERROR_SHARING_VIOLATION; tries++) { Sleep(50); f = fopen(hp, mode); }
    if (fs_log) { static LARGE_INTEGER q0, qf; if (!q0.QuadPart) { QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0); } LARGE_INTEGER q; QueryPerformanceCounter(&q);
                  printf("[fs] open %s (flags 0x%x) -> %s at %.2f s\n", g, flags, f ? "ok" : "ENOENT", (double)(q.QuadPart - q0.QuadPart) / qf.QuadPart); }
    if (!f) { RET(CELL_FS_ENOENT); return; }
    fsh_t *h = calloc(1, sizeof *h); h->f = f; snprintf(h->path, sizeof h->path, "%s", hp);
    { size_t L = strlen(g); if (L > 4 && !_stricmp(g + L - 4, ".BIK")) { h->movie = 1; InterlockedIncrement(&fs_movies_open); } }
    {   /* MNR_AUTODRIVE_AFTER=<file>:<seconds> (benchmarks): no more Start presses once <file> is opened, driving <seconds> later */
        static char want[64]; static int secs = -1;
        if (secs < 0) { const char *e = getenv("MNR_AUTODRIVE_AFTER"); secs = 0; if (e) { const char *c2 = strchr(e, ':'); size_t n = c2 ? (size_t)(c2 - e) : strlen(e); if (n < sizeof want) { memcpy(want, e, n); want[n] = 0; } secs = c2 ? atoi(c2 + 1) : 0; } }
        if (want[0] && !autodrive_file_tick) {
            char up[512]; size_t i = 0; for (; g[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)g[i]); up[i] = 0;
            char w2[64]; for (i = 0; want[i] && i < sizeof w2 - 1; i++) w2[i] = (char)toupper((unsigned char)want[i]); w2[i] = 0;
            if (strstr(up, w2)) { autodrive_file_tick = GetTickCount64() + 1000ull * secs; printf("[autodrive] %s opened: driving in %d s\n", want, secs); }
        }
    }
    {   /* MNR_PAD_SCRIPT_AFTER=<file>: the pad script's clock starts when that file is first opened */
        const char *want = getenv("MNR_PAD_SCRIPT_AFTER");
        if (want && *want && !pad_script_t0) {
            char up[512], w2[64]; size_t i = 0;
            for (; g[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)g[i]); up[i] = 0;
            for (i = 0; want[i] && i < sizeof w2 - 1; i++) w2[i] = (char)toupper((unsigned char)want[i]); w2[i] = 0;
            if (strstr(up, w2)) { pad_script_t0 = GetTickCount64(); printf("[padscript] %s opened: script clock started\n", want); }
        }
    }
    {   size_t L = strlen(g); const char *base = strrchr(g, '/'); base = base ? base + 1 : g;
        if (L > 4 && !_stricmp(g + L - 4, ".ACT") && !_strnicmp(base, "NIS_SCENE", 9)) {
            const char *e = getenv("MNR_AUTOPRESS_NIS_HOLD"); nis_hold_until = GetTickCount64() + 1000ull * (e ? atoi(e) : 90);
            if (getenv("MNR_AUTOPRESS_NOSKIP")) printf("[autopress] cutscene %s: presses held\n", base);
        }
    }
    {   size_t L = strlen(g); if (L > 4 && !_stricmp(g + L - 4, ".TRK")) race_track_opened = 1; }   /* MNR_AI_PLAYER=2 */
    int fd = fs_alloc(h);
    if (fd < 0) { fclose(f); free(h); RET(CELL_FS_EMFILE); return; }
    st_u32(ARG32(2), (uint32_t)fd);
    RET(0);
}
/* Large freads go straight from the kernel (ReadFile) into guest memory, and the kernel's writes are not counted by
 * GetWriteWatch: the renderer then kept a texture's old contents when a file (thumbnails, ...) was read over it.
 * One user-mode write per page marks them written. */
static void ww_touch(uint32_t addr, size_t n) {
    if (!n) return;
    for (uint64_t a = addr & ~0xFFFu; a < (uint64_t)addr + n; a += 0x1000) {
        uint32_t p = a < addr ? addr : (uint32_t)a;
        volatile uint8_t *b = gp(p); *b = *b;
    }
}
static void cell_fs_read(ctx_t *c) {   /* (fd, buf, nbytes, *nread) */
    fsh_t *h = fs_get(ARG32(0));
    if (!h || !h->f) { RET(CELL_FS_EBADF); return; }
    size_t n = fread(gp(ARG32(1)), 1, (size_t)ARG(2), h->f);
    ww_touch(ARG32(1), n);
    if (ARG32(3)) st_u64(ARG32(3), n);
    if (h->movie && n) { LARGE_INTEGER q; QueryPerformanceCounter(&q); fs_movie_read_qpc = q.QuadPart; }
    RET(0);
}
static void cell_fs_write(ctx_t *c) {  /* (fd, buf, nbytes, *nwrite) */
    fsh_t *h = fs_get(ARG32(0));
    if (!h || !h->f) { RET(CELL_FS_EBADF); return; }
    size_t n = fwrite(gp(ARG32(1)), 1, (size_t)ARG(2), h->f);
    if (ARG32(3)) st_u64(ARG32(3), n);
    RET(0);
}
static void cell_fs_close(ctx_t *c) {
    fsh_t *h = fs_get(ARG32(0));
    if (!h) { RET(CELL_FS_EBADF); return; }
    if (h->f) fclose(h->f);
    if (h->movie) InterlockedDecrement(&fs_movies_open);
    if (h->dir && h->dir != INVALID_HANDLE_VALUE) FindClose(h->dir);
    AcquireSRWLockExclusive(&fs_lock); fs_handles[ARG32(0)] = NULL; ReleaseSRWLockExclusive(&fs_lock);
    free(h);
    RET(0);
}
static void cell_fs_lseek(ctx_t *c) {   /* (fd, s64 offset, whence, u64 *pos) */
    fsh_t *h = fs_get(ARG32(0));
    if (!h || !h->f) { RET(CELL_FS_EBADF); return; }
    _fseeki64(h->f, (int64_t)ARG(1), (int)ARG32(2));   /* SEEK_SET/CUR/END = 0/1/2 on both sides */
    if (ARG32(3)) st_u64(ARG32(3), (uint64_t)_ftelli64(h->f));
    RET(0);
}
static void fs_fill_stat(uint32_t st, const WIN32_FILE_ATTRIBUTE_DATA *a) {   /* CellFsStat, packed, 52 bytes */
    int dir = a->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    uint64_t size = (uint64_t)a->nFileSizeHigh << 32 | a->nFileSizeLow;
    uint64_t t = (((uint64_t)a->ftLastWriteTime.dwHighDateTime << 32 | a->ftLastWriteTime.dwLowDateTime) - 116444736000000000ULL) / 10000000ULL;
    mem_zero(st, 52);
    st_u32(st + 0, dir ? 0040777 : 0100666);
    st_u64(st + 12, t); st_u64(st + 20, t); st_u64(st + 28, t);
    st_u64(st + 36, dir ? 0 : size); st_u64(st + 44, 512);
}
static void cell_fs_stat(ctx_t *c) {   /* (path, *stat) */
    const char *g = (const char *)gp(ARG32(0));
    char hp[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!fs_host_path_read(g, hp, sizeof hp) || !GetFileAttributesExA(hp, GetFileExInfoStandard, &a)) {
        if (fs_log) printf("[fs] stat %s -> ENOENT\n", g);
        RET(CELL_FS_ENOENT); return;
    }
    if (fs_log) printf("[fs] stat %s -> ok\n", g);
    fs_fill_stat(ARG32(1), &a);
    RET(0);
}
static void cell_fs_fstat(ctx_t *c) {
    fsh_t *h = fs_get(ARG32(0));
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!h || !GetFileAttributesExA(h->path, GetFileExInfoStandard, &a)) { RET(CELL_FS_EBADF); return; }
    fs_fill_stat(ARG32(1), &a);
    RET(0);
}
static void cell_fs_opendir(ctx_t *c) {   /* (path, *fd) */
    const char *g = (const char *)gp(ARG32(0));
    char hp[MAX_PATH], pat[MAX_PATH];
    if (!fs_host_path(g, hp, sizeof hp)) { RET(CELL_FS_ENOENT); return; }
    snprintf(pat, sizeof pat, "%s\\*", hp);
    fsh_t *h = calloc(1, sizeof *h);
    h->dir = FindFirstFileA(pat, &h->fd);
    if (fs_log) printf("[fs] opendir %s -> %s\n", g, h->dir == INVALID_HANDLE_VALUE ? "ENOENT" : "ok");
    if (h->dir == INVALID_HANDLE_VALUE) { free(h); RET(CELL_FS_ENOENT); return; }
    h->dir_first = 1; snprintf(h->path, sizeof h->path, "%s", hp);
    int fd = fs_alloc(h);
    if (fd < 0) { FindClose(h->dir); free(h); RET(CELL_FS_EMFILE); return; }
    st_u32(ARG32(1), (uint32_t)fd);
    RET(0);
}
static void cell_fs_readdir(ctx_t *c) {   /* (fd, CellFsDirent *, u64 *nread) */
    fsh_t *h = fs_get(ARG32(0));
    if (!h || !h->dir) { RET(CELL_FS_EBADF); return; }
    int have = h->dir_first ? 1 : FindNextFileA(h->dir, &h->fd);
    h->dir_first = 0;
    if (!have) { if (ARG32(2)) st_u64(ARG32(2), 0); RET(0); return; }
    uint32_t d = ARG32(1);
    size_t n = strlen(h->fd.cFileName);
    st_u8(d, (h->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 2);
    st_u8(d + 1, (uint8_t)n);
    memcpy(gp(d + 2), h->fd.cFileName, n + 1);
    if (ARG32(2)) st_u64(ARG32(2), 258);
    RET(0);
}
/* lv2 fs syscalls (801..818) used directly by the game's libc. Destructive ones only act under hdd0/hdd1. */
static int fs_writable(const char *host) {
    char a[MAX_PATH], b[MAX_PATH];
    snprintf(a, sizeof a, "%s\\hdd0\\", mnr_root); snprintf(b, sizeof b, "%s\\hdd1\\", mnr_root);
    for (char *q = a; *q; q++) if (*q == '/') *q = '\\';   /* MNR_ROOT may use forward slashes */
    for (char *q = b; *q; q++) if (*q == '/') *q = '\\';
    return !_strnicmp(host, a, strlen(a)) || !_strnicmp(host, b, strlen(b));
}
static void ksys_fs_path_op(ctx_t *c, int op) {   /* 811 mkdir, 813 rmdir, 814 unlink, 816 access */
    char hp[MAX_PATH];
    if (!fs_host_path((const char *)gp(ARG32(0)), hp, sizeof hp)) { RET(CELL_FS_ENOENT); return; }
    DWORD attr = GetFileAttributesA(hp);
    switch (op) {
    case 811: if (!fs_writable(hp)) { RET(CELL_FS_ENOENT); return; } fs_mkdirs(hp); RET(CreateDirectoryA(hp, NULL) ? 0 : CELL_FS_EEXIST); return;
    case 813: RET(fs_writable(hp) && RemoveDirectoryA(hp) ? 0 : CELL_FS_ENOENT); return;
    case 814: RET(attr != INVALID_FILE_ATTRIBUTES && fs_writable(hp) && DeleteFileA(hp) ? 0 : CELL_FS_ENOENT); return;
    case 816: RET(attr != INVALID_FILE_ATTRIBUTES ? 0 : CELL_FS_ENOENT); return;
    }
}
static void ksys_fs_rename(ctx_t *c) {   /* 812 (from, to) */
    char a[MAX_PATH], b[MAX_PATH];
    if (!fs_host_path((const char *)gp(ARG32(0)), a, sizeof a) || !fs_host_path((const char *)gp(ARG32(1)), b, sizeof b)
        || !fs_writable(a) || !fs_writable(b)) { RET(CELL_FS_ENOENT); return; }
    RET(MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : CELL_FS_ENOENT);
}

static void cell_fs_mkdir(ctx_t *c) {
    char hp[MAX_PATH];
    if (!fs_host_path((const char *)gp(ARG32(0)), hp, sizeof hp)) { RET(CELL_FS_ENOENT); return; }
    fs_mkdirs(hp);
    RET(CreateDirectoryA(hp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? (GetLastError() == ERROR_ALREADY_EXISTS ? CELL_FS_EEXIST : 0) : CELL_FS_ENOENT);
}

static const struct { const char *name; hle_fn fn; } hle_table[] = {
    {"cellSysutil::cellVideoOutGetState", video_out_get_state},
    {"cellSysutil::cellVideoOutGetResolution", video_out_get_resolution},
    {"cellSysutil::cellVideoOutGetNumberOfDevice", video_out_one},
    {"cellSysutil::cellVideoOutGetResolutionAvailability", video_out_one},
    {"cellSysutil::cellVideoOutConfigure", hle_ok},
    {"cellGcmSys::_cellGcmInitBody", gcm_init_body},
    {"cellGcmSys::cellGcmGetConfiguration", gcm_get_configuration},
    {"cellGcmSys::cellGcmMapMainMemory", gcm_map_main_memory},
    {"cellGcmSys::cellGcmMapEaIoAddress", gcm_map_ea_io},
    {"cellGcmSys::cellGcmUnmapEaIoAddress", hle_ok},
    {"cellGcmSys::cellGcmAddressToOffset", gcm_address_to_offset},
    {"cellGcmSys::cellGcmGetControlRegister", gcm_get_control_register},
    {"cellGcmSys::cellGcmGetLabelAddress", gcm_get_label_address},
    {"cellGcmSys::cellGcmGetReportDataAddressLocation", gcm_get_report_address},
    {"cellGcmSys::cellGcmGetTiledPitchSize", gcm_get_tiled_pitch},
    {"cellGcmSys::cellGcmSetDisplayBuffer", gcm_set_display_buffer},
    {"cellGcmSys::cellGcmUnbindTile", hle_ok},
    {"cellGcmSys::cellGcmUnbindZcull", hle_ok},
    {"cellGcmSys::_cellGcmSetFlipCommand", gcm_set_flip_command},
    {"cellGcmSys::cellGcmSetTileInfo", hle_ok},
    {"cellGcmSys::cellGcmBindTile", hle_ok},
    {"cellGcmSys::cellGcmSetFlipMode", gcm_set_flip_mode},
    {"cellGcmSys::cellGcmSetUserHandler", hle_ok},
    {"cellGcmSys::cellGcmSetFlipHandler", gcm_set_flip_handler},
    {"cellGcmSys::cellGcmSetVBlankHandler", gcm_set_vblank_handler},
    {"cellSpurs::_cellSpursLFQueueInitialize", spurs_lfq_init},
    {"cellSpurs::_cellSpursLFQueuePushBody", spurs_lfq_push},
    {"cellSpurs::_cellSpursLFQueuePopBody", spurs_lfq_pop},
    {"cellSpurs::cellSpursLFQueueAttachLv2EventQueue", hle_ok},
    {"cellSpurs::cellSpursAttachLv2EventQueue", spurs_attach_queue},
    {"cellSpurs::_cellSpursJobChainAttributeInitialize", spurs_jc_attr_init},
    {"cellSpurs::cellSpursCreateJobChainWithAttribute", spurs_jc_create},
    {"cellSpurs::cellSpursJobGuardInitialize", spurs_jc_guard_init},
    {"cellSpurs::cellSpursJobGuardNotify", spurs_jc_guard_notify},
    {"cellSpurs::cellSpursJobGuardReset", spurs_jc_guard_reset},
    {"cellSpurs::cellSpursRunJobChain", spurs_jc_run},
    {"cellSpurs::cellSpursDetachLv2EventQueue", spurs_detach_queue},
    {"cellSysutil::cellAudioOutGetState", audio_out_get_state},
    {"cellSysutil::cellAudioOutConfigure", hle_ok},
    {"cellPngEnc::cellPngEncQueryAttr", enc_query_attr},
    {"cellPngEnc::cellPngEncOpenEx", enc_open},
    {"cellPngEnc::cellPngEncOpen", enc_open},
    {"cellPngEnc::cellPngEncWaitForInput", enc_wait_input},
    {"cellPngEnc::cellPngEncEncodePicture", pngenc_encode},
    {"cellPngEnc::cellPngEncWaitForOutput", enc_wait_output},
    {"cellPngEnc::cellPngEncGetStreamInfo", pngenc_stream_info},
    {"cellPngEnc::cellPngEncClose", enc_close},
    {"cellJpgEnc::cellJpgEncQueryAttr", enc_query_attr},
    {"cellJpgEnc::cellJpgEncOpen", enc_open},
    {"cellJpgEnc::cellJpgEncOpenEx", enc_open},
    {"cellJpgEnc::cellJpgEncWaitForInput", enc_wait_input},
    {"cellJpgEnc::cellJpgEncEncodePicture2", jpgenc_encode},
    {"cellJpgEnc::cellJpgEncWaitForOutput", enc_wait_output},
    {"cellJpgEnc::cellJpgEncGetStreamInfo", jpgenc_stream_info},
    {"cellJpgEnc::cellJpgEncClose", enc_close},
    {"cellPngDec::cellPngDecCreate", png_create},
    {"cellPngDec::cellPngDecOpen", png_open},
    {"cellPngDec::cellPngDecReadHeader", png_read_header},
    {"cellPngDec::cellPngDecSetParameter", png_set_parameter},
    {"cellPngDec::cellPngDecDecodeData", png_decode_data},
    {"cellPngDec::cellPngDecClose", png_close},
    {"cellPngDec::cellPngDecDestroy", hle_ok},
    {"cellAudio::cellAudioPortClose", hle_audio_port_close},
    {"cellSysutil::cellSysutilGetBgmPlaybackStatus2", sysutil_bgm_status},
    {"cellVoice::cellVoiceInit", hle_voice_init},
    {"cellVoice::cellVoiceInitEx", hle_voice_init},
    {"cellVoice::cellVoiceEnd", hle_voice_end},
    {"cellVoice::cellVoiceStart", hle_voice_start},
    {"cellVoice::cellVoiceStartEx", hle_voice_start},
    {"cellVoice::cellVoiceStop", hle_voice_stop},
    {"cellVoice::cellVoiceCreateNotifyEventQueue", hle_voice_create_queue},
    {"cellVoice::cellVoiceSetNotifyEventQueue", hle_voice_set_queue},
    {"cellVoice::cellVoiceRemoveNotifyEventQueue", hle_voice_remove_queue},
    {"cellVoice::cellVoiceCreatePort", hle_voice_create_port},
    {"cellVoice::cellVoiceDeletePort", hle_voice_delete_port},
    {"cellVoice::cellVoiceConnectIPortToOPort", hle_voice_connect},
    {"cellVoice::cellVoiceDisconnectIPortFromOPort", hle_voice_disconnect},
    {"cellVoice::cellVoiceGetPortInfo", hle_voice_port_info},
    {"cellVoice::cellVoiceWriteToIPort", hle_voice_write},
    {"cellVoice::cellVoiceWriteToIPortEx", hle_voice_write},
    {"cellVoice::cellVoiceWriteToIPortEx2", hle_voice_write},
    {"cellVoice::cellVoiceReadFromOPort", hle_voice_read},
    {"cellVoice::cellVoiceSetVolume", hle_voice_set_volume},
    {"cellVoice::cellVoiceGetVolume", hle_voice_get_volume},
    {"cellVoice::cellVoiceSetMuteFlag", hle_voice_set_mute},
    {"cellVoice::cellVoiceGetMuteFlag", hle_voice_get_mute},
    {"cellVoice::cellVoiceSetMuteFlagAll", hle_voice_mute_all},
    {"cellVoice::cellVoiceSetBitRate", hle_voice_set_bitrate},
    {"cellVoice::cellVoiceGetBitRate", hle_voice_get_bitrate},
    {"cellVoice::cellVoicePausePort", hle_voice_pause},
    {"cellVoice::cellVoiceResumePort", hle_voice_resume},
    {"cellVoice::cellVoicePausePortAll", hle_voice_pause_all},
    {"cellVoice::cellVoiceResumePortAll", hle_voice_resume_all},
    {"cellVoice::cellVoiceResetPort", hle_voice_reset_port},
    {"cellVoice::cellVoiceSetPortAttr", hle_voice_set_attr},
    {"cellVoice::cellVoiceGetPortAttr", hle_voice_get_attr},
    {"cellVoice::cellVoiceGetSignalState", hle_voice_signal_state},
    {"cellSysutil::cellSysutilGetBgmPlaybackStatus", sysutil_bgm_status},
    {"cellAudio::cellAudioInit", hle_audio_init},
    {"cellAudio::cellAudioPortOpen", hle_audio_port_open},
    {"cellAudio::cellAudioGetPortConfig", hle_audio_get_port_config},
    {"cellAudio::cellAudioPortStart", hle_audio_port_start},
    {"cellAudio::cellAudioPortStop", hle_audio_port_stop},
    {"cellAudio::cellAudioSetPortLevel", hle_audio_set_level},
    {"cellAudio::cellAudioQuit", hle_ok},
    {"cellSysutil::cellSysutilEnableBgmPlaybackEx", hle_ok},
    {"cellSysutil::cellSysutilDisableBgmPlayback", hle_ok},
    {"cellSpurs::_cellSpursAttributeInitialize", hle_ok},
    {"cellSpurs::cellSpursAttributeSetNamePrefix", hle_ok},
    {"cellSpurs::cellSpursAttributeEnableSpuPrintfIfAvailable", hle_ok},
    {"cellSpurs::cellSpursAttributeSetSpuThreadGroupType", hle_ok},
    {"cellSpurs::cellSpursInitializeWithAttribute", hle_ok},
    {"cellSpurs::_cellSpursTasksetAttributeInitialize", hle_ok},
    {"cellSpurs::cellSpursTasksetAttributeSetName", hle_ok},
    {"cellSpurs::cellSpursTasksetAttributeSetTasksetSize", hle_ok},
    {"cellSpurs::cellSpursCreateTasksetWithAttribute", hle_ok},
    {"sysPrxForUser::sys_ppu_thread_once", sys_ppu_thread_once},
    {"sysPrxForUser::sys_ppu_thread_exit", ppu_thread_exit},
    {"cellSysutil::cellSysutilGetSystemParamInt", sysutil_get_param_int},
    {"cellRtc::cellRtcGetCurrentTick", rtc_get_current_tick},
    {"cellGame::cellGameCreateGameData", game_create_game_data},
    {"cellGame::cellGameSetParamString", hle_ok},
    {"cellSysutil::cellMsgDialogOpen2", msg_dialog_open},
    {"cellSysutil::cellMsgDialogClose", hle_ok},
    {"cellSysutil::cellMsgDialogProgressBarReset", hle_ok},
    {"cellSysutil::cellMsgDialogProgressBarInc", hle_ok},
    {"cellSysutil::cellSaveDataUserAutoLoad", save_data_user_auto_load},
    {"cellSysutil::cellSaveDataUserAutoSave", save_data_user_auto_save},
    {"cellSysutil::cellSaveDataAutoLoad2", save_data_auto_load2},
    {"cellSysutil::cellSaveDataAutoSave2", save_data_auto_save2},
    {"sceNp::sceNpInit", hle_ok},
    {"sceNp::sceNpManagerGetStatus", np_manager_get_status},
    {"sceNp::sceNpManagerRegisterCallback", np_manager_register_callback},
    {"sceNp::sceNpManagerUnregisterCallback", np_manager_unregister_callback},
    {"sceNp::sceNpManagerGetOnlineId", np_manager_get_online_id},
    {"sceNp::sceNpManagerGetAccountRegion", np_manager_get_account_region},
    {"sceNp::sceNpManagerGetChatRestrictionFlag", np_manager_get_chat_restriction},
    {"sceNp::sceNpManagerGetContentRatingFlag", np_manager_get_content_rating},
    {"sceNp::sceNpManagerGetAvatarUrl", np_manager_get_avatar_url},
    {"sceNp::sceNpManagerRequestTicket", np_manager_request_ticket},
    {"sceNp::sceNpManagerRequestTicket2", np_manager_request_ticket2},
    {"sceNp::sceNpManagerGetTicket", np_manager_get_ticket},
    {"sceNp::sceNpBasicRegisterContextSensitiveHandler", np_basic_register_cs_handler},
    {"sceNp::sceNpBasicRegisterHandler", np_basic_register_handler},
    {"sceNp::sceNpBasicUnregisterHandler", np_basic_unregister_handler},
    {"sceNp::sceNpBasicGetFriendPresenceByIndex", np_basic_friend_presence},
    {"sceNp::sceNpBasicGetEvent", np_basic_get_event},
    {"sceNp::sceNpBasicSetPresence", np_basic_set_presence},
    {"sceNp::sceNpBasicSendMessage", np_basic_send_message},
    {"sceNp::sceNpBasicSendMessageGui", np_basic_send_message_gui},
    {"sceNp::sceNpBasicAddFriend", np_basic_add_friend},
    {"sceNp::sceNpBasicRecvMessageCustom", np_basic_recv_message_custom},
    {"sceNp::sceNpBasicAbortGui", hle_ok},
    {"sceNp::sceNpUtilCmpNpId", np_util_cmp_np_id},
    {"cellNetCtl::cellNetCtlGetInfo", net_ctl_get_info},
    {"cellNetCtl::cellNetCtlAddHandler", hle_ok},
    {"sys_net::inet_aton", net_inet_aton},
    {"sceNp::sceNpLookupInit", hle_ok},
    {"sceNp::sceNpLookupTerm", hle_ok},
    {"sceNp::sceNpLookupCreateTitleCtx", np_lookup_create_title_ctx},
    {"sceNp::sceNpLookupDestroyTitleCtx", hle_ok},
    {"sceNp::sceNpLookupCreateTransactionCtx", np_lookup_create_transaction_ctx},
    {"sceNp::sceNpLookupDestroyTransactionCtx", hle_ok},
    {"sceNp::sceNpLookupNpIdAsync", np_lookup_np_id_async},
    {"sceNp::sceNpLookupPollAsync", np_lookup_poll_async},
    {"sceNp::sceNpBasicGetFriendListEntryCount", np_basic_friend_count},
    {"sceNp::sceNpBasicGetBlockListEntryCount", np_basic_count0_arg0},
    {"sceNp::sceNpBasicGetPlayersHistoryEntryCount", np_basic_count0_arg1},
    {"sceNp::sceNpBasicAddPlayersHistoryAsync", np_basic_add_players_history},
    {"sceNp::sceNpSignalingCreateCtx", np_signaling_create_ctx},
    {"sceNp::sceNpSignalingDestroyCtx", np_signaling_destroy_ctx},
    {"sceNp::sceNpSignalingGetConnectionStatus", np_signaling_get_connection_status},
    {"sceNp::sceNpSignalingTerminateConnection", np_signaling_terminate_connection},
    {"cellSsl::cellSslCertificateLoader", ssl_certificate_loader},
    {"cellNetCtl::cellNetCtlNetStartDialogLoadAsync", net_ctl_netstart_load},
    {"cellNetCtl::cellNetCtlNetStartDialogUnloadAsync", net_ctl_netstart_unload},
    {"cellHttp::cellHttpInit", hle_ok},
    {"cellHttp::cellHttpInitCookie", hle_ok},
    {"cellHttp::cellHttpsInit", hle_ok},
    {"cellHttp::cellHttpEnd", hle_ok},
    {"cellHttp::cellHttpEndCookie", hle_ok},
    {"cellHttp::cellHttpsEnd", hle_ok},
    {"cellSsl::cellSslInit", hle_ok},
    {"cellSsl::cellSslEnd", hle_ok},
    {"sys_net::socket", net_socket},             /* (no sockets unless MNR_NET=1) */
    {"sys_net::socketclose", net_socketclose},
    {"sys_net::shutdown", net_shutdown},
    {"sys_net::inet_addr", net_inet_addr},
    {"sys_net::inet_ntoa", net_inet_ntoa},
    {"sys_net::gethostbyname", net_gethostbyname},
    {"sys_net::_sys_net_h_errno_loc", net_h_errno_loc},
    {"sys_net::bind", net_bind},
    {"sys_net::listen", net_listen},
    {"sys_net::accept", net_accept},
    {"sys_net::connect", net_connect},
    {"sys_net::getpeername", net_getpeername},
    {"sys_net::send", net_send},
    {"sys_net::sendto", net_sendto},
    {"sys_net::recv", net_recv},
    {"sys_net::recvfrom", net_recvfrom},
    {"sys_net::socketpoll", net_socketpoll},
    {"sys_net::socketselect", net_socketselect},
    {"cellSpurs::cellSpursShutdownTaskset", hle_ok},   /* tasksets are nominal here (tasks run on the caller) */
    {"cellSpurs::cellSpursJoinTaskset", hle_ok},
    {"cellSpurs::cellSpursFinalize", hle_ok},
    {"sceNp::sceNpBasicSetPresenceDetails", np_basic_set_presence_details},
    {"sceNp::sceNpManagerGetNpId", np_manager_get_np_id},
    {"sys_net::setsockopt", net_setsockopt},
    {"sys_net::_sys_net_errno_loc", net_errno_loc},
    {"cellGame::cellGameBootCheck", cell_game_boot_check},
    {"cellGame::cellGameDataCheck", cell_game_data_check},
    {"cellGame::cellGameContentPermit", cell_game_content_permit},
    {"cellGame::cellGameGetSizeKB", game_get_size_kb},
    {"cellGame::cellGameGetParamInt", cell_game_get_param_int},
    {"cellGame::cellGameGetParamString", cell_game_get_param_string},
    {"cellUserInfo::cellUserInfoGetList", cell_user_info_get_list},
    {"cellUserInfo::cellUserInfoGetStat", cell_user_info_get_stat},
    {"sceNpTrophy::sceNpTrophyInit", hle_ok},
    {"sceNpTrophy::sceNpTrophyCreateContext", np_trophy_create},
    {"sceNpTrophy::sceNpTrophyCreateHandle", np_trophy_create},
    {"sceNpTrophy::sceNpTrophyRegisterContext", hle_ok},
    {"sceNpTrophy::sceNpTrophyGetRequiredDiskSpace", np_trophy_space},
    {"sceNpTrophy::sceNpTrophyUnlockTrophy", hle_ok},
    {"cellGcmSys::cellGcmBindZcull", hle_ok},
    {"sysPrxForUser::_sys_spu_printf_initialize", hle_ok},
    {"sysPrxForUser::_sys_spu_printf_finalize", hle_ok},
    {"sysPrxForUser::_sys_spu_printf_attach_group", hle_ok},
    {"sysPrxForUser::_sys_spu_printf_detach_group", hle_ok},
    {"cellSysmodule::cellSysmoduleLoadModule", hle_ok},
    {"cellSysmodule::cellSysmoduleUnloadModule", hle_ok},
    {"cellSysmodule::cellSysmoduleIsLoaded", hle_ok},
    {"cellSysutil::cellSysCacheMount", sys_cache_mount},
    {"sys_net::sys_net_initialize_network_ex", hle_ok},
    {"cellNetCtl::cellNetCtlInit", hle_ok},
    {"cellNetCtl::cellNetCtlGetState", net_ctl_get_state},
    {"sys_io::cellPadInit", hle_ok},
    {"sys_io::cellPadSetPortSetting", hle_ok},
    {"sys_io::cellPadGetInfo2", pad_get_info2},
    {"sys_io::cellPadGetData", pad_get_data},
    {"sys_io::cellPadClearBuf", hle_ok},
    {"sys_fs::cellFsOpen", cell_fs_open},
    {"sys_fs::cellFsRead", cell_fs_read},
    {"sys_fs::cellFsWrite", cell_fs_write},
    {"sys_fs::cellFsClose", cell_fs_close},
    {"sys_fs::cellFsLseek", cell_fs_lseek},
    {"sys_fs::cellFsStat", cell_fs_stat},
    {"sys_fs::cellFsFstat", cell_fs_fstat},
    {"sys_fs::cellFsOpendir", cell_fs_opendir},
    {"sys_fs::cellFsReaddir", cell_fs_readdir},
    {"sys_fs::cellFsClosedir", cell_fs_close},
    {"sys_fs::cellFsMkdir", cell_fs_mkdir},
    {"sys_fs::cellFsUtime", hle_ok},
    {"sys_fs::cellFsAioInit", hle_ok},
    {"sys_fs::cellFsAioFinish", hle_ok},
    {"sysPrxForUser::sys_initialize_tls", sys_initialize_tls},
    {"sysPrxForUser::sys_prx_register_library", hle_ok},
    {"sysPrxForUser::_sys_process_atexitspawn", hle_ok},
    {"sysPrxForUser::_sys_process_at_Exitspawn", hle_ok},
    {"sysPrxForUser::sys_time_get_system_time", sys_time_get_system_time},
    {"sysPrxForUser::sys_ppu_thread_get_id", sys_ppu_thread_get_id_r},
    {"sysPrxForUser::sys_ppu_thread_create", sys_ppu_thread_create},
    {"sysPrxForUser::sys_lwmutex_create", sys_lwmutex_create},
    {"sysPrxForUser::sys_lwmutex_destroy", sys_lw_destroy},
    {"sysPrxForUser::sys_lwmutex_lock", sys_lwmutex_lock},
    {"sysPrxForUser::sys_lwmutex_unlock", sys_lwmutex_unlock},
    {"sysPrxForUser::sys_lwmutex_trylock", sys_lwmutex_trylock},
    {"sysPrxForUser::sys_lwcond_create", sys_lwcond_create},
    {"sysPrxForUser::sys_lwcond_destroy", sys_lw_destroy},
    {"sysPrxForUser::sys_lwcond_wait", sys_lwcond_wait},
    {"sysPrxForUser::sys_lwcond_signal", sys_lwcond_signal},
    {"sysPrxForUser::sys_lwcond_signal_all", sys_lwcond_signal_all},
    {"cellSysutil::cellSysutilRegisterCallback", cell_sysutil_register_callback},
    {"cellSysutil::cellSysutilUnregisterCallback", hle_ok},
    {"cellSysutil::cellSysutilCheckCallback", cell_sysutil_check_callback},
    {"cellSysutil::cellOskDialogLoadAsync", osk_load_async},
    {"cellSysutil::cellOskDialogUnloadAsync", osk_unload_async},
    {"cellSysutil::cellOskDialogAbort", osk_abort},
    {"cellSysutil::cellOskDialogSetLayoutMode", hle_ok},
    {"cellSysutil::cellOskDialogSetKeyLayoutOption", hle_ok},
    {"cellSysutil::cellOskDialogSetInitialKeyLayout", hle_ok},
    {"cellSysutil::cellOskDialogSetDeviceMask", hle_ok},
    {"cellSysutil::cellOskDialogAddSupportLanguage", hle_ok},
    {"cellOskExtUtility::cellOskDialogExtRegisterConfirmWordFilterCallback", hle_ok},
    {"cellSync::cellSyncMutexInitialize", cell_sync_mutex_init},
    {"cellSync::cellSyncMutexTryLock", cell_sync_mutex_trylock},
    {"cellSync::cellSyncMutexUnlock", cell_sync_mutex_unlock},
};

static void hle_call_inner(ctx_t *c, const char *name, uint32_t stub);
void hle_call(ctx_t *c, const char *name, uint32_t stub) {   /* MNR_HLE_TRACE=a,b: log calls whose names contain a or b */
    static const char *trace = (const char *)1;
    if (trace == (const char *)1) trace = getenv("MNR_HLE_TRACE");
    int hit = 0;
    if (trace) { char t[256]; snprintf(t, sizeof t, "%s", trace); char *ctx_s = NULL;
        for (char *k = strtok_s(t, ",", &ctx_s); k; k = strtok_s(NULL, ",", &ctx_s)) if (strstr(name, k)) { hit = 1; break; } }
    if (!hit) {
        if (tl_state == 1) { LARGE_INTEGER a, b; uint32_t lr = (uint32_t)c->lr, a0 = (uint32_t)c->r[3]; QueryPerformanceCounter(&a); hle_call_inner(c, name, stub); QueryPerformanceCounter(&b); tl_rec(name, a.QuadPart, b.QuadPart, lr, a0); return; }
        hle_call_inner(c, name, stub); return; }
    uint64_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3), lr = c->lr;
    hle_call_inner(c, name, stub);
    printf("[hle] %s(%llx, %llx, %llx, %llx) = 0x%llx (lr 0x%llx)\n", name, (unsigned long long)a0, (unsigned long long)a1,
           (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)c->r[3], (unsigned long long)lr);
}
static void hle_call_inner(ctx_t *c, const char *name, uint32_t stub) {
    /* cache by stub address: each stub always names the same import (no strcmp scan per call) */
    static struct { uint32_t stub; void (*fn)(ctx_t *); } cache[4096];
    unsigned h = (stub >> 2) & 4095;
    treg_t *tr = my_treg;
    if (tr) { tr->hle_lr = (uint32_t)c->lr; tr->hle = name; }
    if (cache[h].stub == stub && cache[h].fn) { cache[h].fn(c); if (tr) tr->hle = NULL; return; }
    for (unsigned i = 0; i < sizeof hle_table / sizeof hle_table[0]; i++)
        if (!strcmp(name, hle_table[i].name)) { cache[h].fn = hle_table[i].fn; cache[h].stub = stub; hle_table[i].fn(c); if (tr) tr->hle = NULL; return; }
    fprintf(stderr, "HLE not implemented: %s (stub 0x%08x, lr 0x%08llx, args %llx %llx %llx %llx)\n", name, stub,
            (unsigned long long)c->lr, (unsigned long long)ARG(0), (unsigned long long)ARG(1),
            (unsigned long long)ARG(2), (unsigned long long)ARG(3));
    if (!hle_lenient) exit(5);
    RET(0);
}

static int syscall_log;
static void hle_syscall_inner(ctx_t *c);
void hle_syscall(ctx_t *c) {
    static char names[1024][16];
    unsigned n = (unsigned)c->r[11] & 1023;
    treg_t *tr = my_treg;
    if (tr) { if (!names[n][0]) snprintf(names[n], 16, "syscall %u", n); tr->hle_lr = (uint32_t)c->lr; tr->hle = names[n]; }
    if (tl_state == 1) { if (!names[n][0]) snprintf(names[n], 16, "syscall %u", n);
        LARGE_INTEGER a, b; uint32_t lr = (uint32_t)c->lr, a0 = (uint32_t)c->r[3]; QueryPerformanceCounter(&a); hle_syscall_inner(c); QueryPerformanceCounter(&b);
        tl_rec(names[n], a.QuadPart, b.QuadPart, lr, a0); if (tr) tr->hle = NULL; return; }
    hle_syscall_inner(c);
    if (tr) tr->hle = NULL;
}
static void hle_syscall_inner(ctx_t *c) {
    uint64_t n = c->r[11];
    if (syscall_log && n != 141 && n != 130)   /* skip the noisiest (sleep, event receive) */
        printf("[sc] %llu(%llx, %llx, %llx, %llx) lr 0x%08llx thread 0x%llx\n", (unsigned long long)n,
               (unsigned long long)ARG(0), (unsigned long long)ARG(1), (unsigned long long)ARG(2),
               (unsigned long long)ARG(3), (unsigned long long)c->lr, (unsigned long long)c->thread_id);
    switch (n) {
    case 402: if (ARG32(3)) st_u32(ARG32(3), 0); RET(0); return;   /* sys_tty_read: no console input */
    case 403: {  /* sys_tty_write(ch, buf, len, *written) */
        uint32_t len = ARG32(2);
        fwrite(gp(ARG32(1)), 1, len, stdout);
        if (ARG32(3)) st_u32(ARG32(3), len);
        RET(0);
        return;
    }
    case 348: {  /* sys_memory_allocate(size, flags, *alloc_addr) */
        uint32_t align = (ARG(1) & 0x400) ? 0x100000 : 0x10000;   /* 1M vs 64K pages */
        uint32_t a = user_alloc(ARG32(0), align);
        printf("[mem] sys_memory_allocate(0x%x, flags 0x%llx) -> 0x%08x (lr 0x%08llx)\n", ARG32(0),
               (unsigned long long)ARG(1), a, (unsigned long long)c->lr);
        if (!a) { RET(0x80010004); return; }   /* CELL_ENOMEM */
        st_u32(ARG32(2), a);
        RET(0);
        return;
    }
    case 349: RET(0); return;   /* sys_memory_free: bump allocator, ignore */
    case 352: {  /* sys_memory_get_user_memory_size(*mem_info {total, available}) */
        /* Report a retail console's 213 MB. The game sizes its Main Pool from 'available' and
         * takes a devkit branch (subtracting 196 MB) above ~195 MB, which starved the pool. */
        const uint32_t total = 0x0D500000;
        uint32_t used = user_top - USER_LO;
        st_u32(ARG32(0), total);
        st_u32(ARG32(0) + 4, used < total ? total - used : 0);
        RET(0);
        return;
    }
    case 1: RET(0x01000500 >> 8); return;   /* sys_process_getpid */
    case 100: ksys_mutex_create(c); return;       /* sys_mutex_create(*id, attr) */
    case 101: RET(0); return;                      /* sys_mutex_destroy */
    case 102: ksys_mutex_lock(c); return;          /* sys_mutex_lock(id, timeout) */
    case 103: ksys_mutex_trylock(c); return;
    case 104: ksys_mutex_unlock(c); return;
    case 105: ksys_cond_create(c); return;        /* sys_cond_create(*id, mutex_id, attr) */
    case 106: RET(0); return;
    case 107: ksys_cond_wait(c); return;          /* sys_cond_wait(id, timeout) */
    case 108: { kcond_t *k = kobj_get(ARG32(0)); if (k) WakeConditionVariable(&k->cv); RET(0); return; }
    case 109: { kcond_t *k = kobj_get(ARG32(0)); if (k) WakeAllConditionVariable(&k->cv); RET(0); return; }
    case 110: { kcond_t *k = kobj_get(ARG32(0)); if (k) WakeAllConditionVariable(&k->cv); RET(0); return; }  /* sys_cond_signal_to(id, thread): waking all is safe, waiters recheck */
    case 90: ksys_sema_create(c); return;         /* sys_semaphore_create(*id, attr, initial, max) */
    case 91: RET(0); return;
    case 92: ksys_sema_wait(c); return;           /* sys_semaphore_wait(id, timeout) */
    case 93: { ksema_t *s = kobj_get(ARG32(0)); RET(s && WaitForSingleObject(s->h, 0) == WAIT_OBJECT_0 ? 0 : CELL_EBUSY); return; }
    case 94: { ksema_t *s = kobj_get(ARG32(0)); if (s) ReleaseSemaphore(s->h, (LONG)ARG32(1), NULL); RET(0); return; }
    case 41: ppu_thread_exit(c); return;          /* sys_ppu_thread_exit(value) */
    case 43: SwitchToThread(); RET(0); return;    /* sys_ppu_thread_yield */
    case 82: ksys_eflag_create(c); return;        /* sys_event_flag_create(*id, attr, init) */
    case 83: RET(0); return;                       /* sys_event_flag_destroy */
    case 85: ksys_eflag_wait(c, 0); return;       /* sys_event_flag_wait(id, pat, mode, *result, timeout) */
    case 86: ksys_eflag_wait(c, 1); return;       /* sys_event_flag_trywait */
    case 87: ksys_eflag_set(c); return;           /* sys_event_flag_set(id, pat) */
    case 118: ksys_eflag_clear(c); return;        /* sys_event_flag_clear(id, pat) */
    case 169: RET(0); return;                      /* sys_spu_initialize */
    case 128: ksys_queue_create(c); return;       /* sys_event_queue_create(*id, attr, key, size) */
    case 129: RET(0); return;                      /* sys_event_queue_destroy */
    case 130: ksys_queue_receive(c, 0); return;   /* sys_event_queue_receive(id, *ev, timeout) */
    case 131: ksys_queue_receive(c, 1); return;   /* sys_event_queue_tryreceive */
    case 134: ksys_port_create(c); return;        /* sys_event_port_create(*id, type, name) */
    case 135: RET(0); return;                      /* sys_event_port_destroy */
    case 136: ksys_port_connect(c); return;       /* sys_event_port_connect_local(port, queue) */
    case 137: RET(0); return;                      /* sys_event_port_disconnect */
    case 138: ksys_port_send(c); return;          /* sys_event_port_send(port, d1, d2, d3) */
    case 44: {                                     /* sys_ppu_thread_join(id, *value) */
        pthr_t *t = kobj_get(ARG32(0));
        if (!t) { RET(CELL_ESRCH); return; }
        WaitForSingleObject(t->h, INFINITE);
        if (ARG32(1)) st_u64(ARG32(1), t->exit_value);
        RET(0);
        return;
    }
    case 45: case 47: case 48: RET(0); return;    /* detach / set_priority / get_priority (stubbed) */
    case 141: {                                    /* sys_timer_usleep(usec) */
        uint64_t us = ARG(0);
        {   /* MNR_USLEEP_LOG=1 (debug): the GCM labels polled with usleep(300) (r31 holds the label's address), per 5 s */
            static int on = -1; if (on < 0) on = getenv("MNR_USLEEP_LOG") != NULL;
            if (on && us == 300) {
                static long cnt[256]; static ULONGLONG t0; uint32_t a = (uint32_t)c->r[31];
                if (a >= GCM_LABELS && a < GCM_LABELS + 0x1000) cnt[(a - GCM_LABELS) / 16]++;
                ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
                if (t - t0 >= 5000) { t0 = t; printf("[usleep] label polls (5 s):"); for (int i = 0; i < 256; i++) if (cnt[i]) printf(" %d(0x%x)=%ld", i, i * 16, cnt[i]); printf(" flips %ld\n", (long)gcm_flips); memset(cnt, 0, sizeof cnt); }
            }
        }
        if (us >= 1000) Sleep((DWORD)(us / 1000));
        else if (us == 300 && (uint32_t)c->r[31] >= GCM_LABELS && (uint32_t)c->r[31] < GCM_LABELS + 0x1000 && !(c->r[31] & 3) && ld_u32((uint32_t)c->r[31]) == 0) {
            /* the render thread's fence wait (FUN_007cad24: while (label == 0) usleep(300), r31 = the label): sleep on the
             * label itself, woken the moment the command processor writes it (gcm_method wakes every released label) */
            uint32_t zero = 0; WaitOnAddress(gp((uint32_t)c->r[31]), &zero, 4, 1);
        } else {   /* a real short sleep: the game polls labels with usleep(300), and yielding made that loop spin a whole core */
            static __thread HANDLE tm; static __thread int tried;
            if (!tried++) tm = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(us ? us : 1) * 10;
            if (tm && SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(tm, INFINITE); else SwitchToThread();
        }
        RET(0);
        return;
    }
    case 142: Sleep((DWORD)(ARG(0) * 1000)); RET(0); return;   /* sys_timer_sleep(sec) */
    case 145: {                        /* sys_time_get_current_time(*sec, *nsec) */
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        uint64_t t100 = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL;
        st_u64(ARG32(0), t100 / 10000000ULL);
        st_u64(ARG32(1), (t100 % 10000000ULL) * 100);
        RET(0);
        return;
    }
    case 144:                          /* sys_time_get_timezone(*timezone minutes, *summertime) */
        if (ARG32(0)) st_u32(ARG32(0), 0);
        if (ARG32(1)) st_u32(ARG32(1), 0);
        RET(0);
        return;
    case 872: {                        /* sys_ss_get_open_psid(CellSsOpenPSID *{u64 high, low}): one per account. The game
                                          * hashes it into its online player ids, so a shared one made every PC the same player */
        const char *id = getenv("MNR_NP_ID");
        uint64_t h = 14695981039346656037ull, l = 0x9e3779b97f4a7c15ull;
        for (const char *t = id && *id ? id : "mnr"; *t; t++) { h = (h ^ (uint8_t)*t) * 1099511628211ull; l = (l ^ (uint8_t)*t) * 0x100000001b3ull + 0x632be59bd9b4e019ull; }
        st_u64(ARG32(0), h); st_u64(ARG32(0) + 8, l);
        printf("[ss] open psid %016llx%016llx\n", (unsigned long long)h, (unsigned long long)l);
        RET(0); return;
    }
    case 147: RET(79800000); return;   /* sys_time_get_timebase_frequency */
    case 808: { ctx_t tmp = *c; tmp.r[3] = ARG(0); tmp.r[4] = ARG(1); cell_fs_stat(&tmp); c->r[3] = tmp.r[3]; return; }  /* sys_fs_stat */
    /* the other lv2 fs syscalls take the same arguments as their cellFs wrappers */
    case 801: cell_fs_open(c); return;       /* sys_fs_open(path, flags, *fd, mode, arg, size) */
    case 802: cell_fs_read(c); return;       /* sys_fs_read(fd, buf, n, *nread) */
    case 803: cell_fs_write(c); return;      /* sys_fs_write(fd, buf, n, *nwrite) */
    case 804: cell_fs_close(c); return;      /* sys_fs_close(fd) */
    case 805: cell_fs_opendir(c); return;    /* sys_fs_opendir(path, *fd) */
    case 806: cell_fs_readdir(c); return;    /* sys_fs_readdir(fd, *dirent, *nread) */
    case 807: cell_fs_close(c); return;      /* sys_fs_closedir(fd) */
    case 809: cell_fs_fstat(c); return;      /* sys_fs_fstat(fd, *stat) */
    case 818: cell_fs_lseek(c); return;      /* sys_fs_lseek(fd, offset, whence, *pos) */
    case 811: case 813: case 814: case 816: ksys_fs_path_op(c, (int)n); return;
    case 812: ksys_fs_rename(c); return;
    case 49: {                         /* sys_ppu_thread_get_stack_information(*info {u32 addr, u32 size}) */
        uint32_t top = (uint32_t)(c->r[1] + 0xFFFF) & ~0xFFFFu;
        st_u32(ARG32(0), top - 0x10000); st_u32(ARG32(0) + 4, 0x10000);
        RET(0);
        return;
    }
    }
    fprintf(stderr, "syscall %llu not implemented (lr 0x%08llx, args %llx %llx %llx %llx)", (unsigned long long)n,
            (unsigned long long)c->lr, (unsigned long long)ARG(0), (unsigned long long)ARG(1),
            (unsigned long long)ARG(2), (unsigned long long)ARG(3));
    for (int i = 0; i < 2; i++) {   /* show string arguments, if any, to help identify the call */
        uint32_t a = ARG32(i);
        if (a >= 0x10000 && a < COMMIT && gp(a)[0] >= 0x20 && gp(a)[0] < 0x7f) fprintf(stderr, " arg%d=\"%.80s\"", i, (const char *)gp(a));
    }
    fprintf(stderr, "\n");
    if (!hle_lenient) exit(6);
    RET(0);
}

_Noreturn void unimpl(ctx_t *c, uint32_t addr, uint32_t insn) {
    fprintf(stderr, "untranslated instruction %08x at 0x%08x\n", insn, addr);
    exit(7);
}
void trap_always(ctx_t *c, uint32_t addr) {
    fprintf(stderr, "trap at 0x%08x\n", addr);
    exit(8);
}
uint64_t host_timebase(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return (uint64_t)((double)t.QuadPart / (double)f.QuadPart * 79800000.0);
}

/* ------------------------------------------------------------------ loading */

static void load_elf(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }
    fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *d = malloc(size);
    if (fread(d, 1, size, fp) != (size_t)size) { perror("read"); exit(1); }
    fclose(fp);
#define BE64(p) __builtin_bswap64(*(uint64_t *)(p))
#define BE32(p) __builtin_bswap32(*(uint32_t *)(p))
#define BE16(p) __builtin_bswap16(*(uint16_t *)(p))
    uint64_t phoff = BE64(d + 0x20);
    unsigned phnum = BE16(d + 0x38);
    for (unsigned i = 0; i < phnum; i++) {
        uint8_t *ph = d + phoff + i * 56;
        uint64_t off = BE64(ph + 8), va = BE64(ph + 16), fsz = BE64(ph + 32), msz = BE64(ph + 40);
        if (BE32(ph) == 7) { tls_va = (uint32_t)va; tls_filesz = (uint32_t)fsz; tls_memsz = (uint32_t)msz; continue; }
        if (BE32(ph) != 1) continue;
        if (!msz) continue;
        memcpy(gp((uint32_t)va), d + off, fsz);
        memset(gp((uint32_t)va + (uint32_t)fsz), 0, msz - fsz);
    }
    free(d);
}

/* ------------------------------------------------------------------ Lua helpers */
static uint32_t opd_alloc, opd_panic;

static uint32_t lua_open_state(ctx_t *c) {
    c->r[3] = opd_alloc; c->r[4] = 0;
    call_guest(c, LUA_NEWSTATE);
    uint32_t L = (uint32_t)c->r[3];
    if (!L) { fprintf(stderr, "lua_newstate returned NULL\n"); exit(9); }
    c->r[3] = L; c->r[4] = opd_panic; call_guest(c, LUA_ATPANIC);
    uint32_t libs[] = {LUAOPEN_BASE, LUAOPEN_STRING, LUAOPEN_TABLE};
    for (unsigned i = 0; i < 3; i++) {
        c->r[3] = L; call_guest(c, libs[i]);
        c->r[3] = L; c->r[4] = 0; call_guest(c, LUA_SETTOP);
    }
    return L;
}

static int lua_run(ctx_t *c, uint32_t L, const char *buf, size_t len, const char *name) {
    uint32_t gbuf = gstrdup(buf, len), gname = gstrdup(name, strlen(name));
    c->r[3] = L; c->r[4] = gbuf; c->r[5] = len; c->r[6] = gname;
    call_guest(c, LUAL_LOADBUFFER);
    int st = (int)(int32_t)c->r[3];
    if (st == 0) {
        c->r[3] = L; c->r[4] = 0; c->r[5] = 0; c->r[6] = 0;
        call_guest(c, LUA_PCALL);
        st = (int)(int32_t)c->r[3];
    }
    if (st != 0) {
        c->r[3] = L; c->r[4] = (uint64_t)(int64_t)-1; c->r[5] = 0;
        call_guest(c, LUA_TOLSTRING);
        uint32_t s = (uint32_t)c->r[3];
        printf("  [%s] error %d: %s\n", name, st, s ? (char *)gp(s) : "(non-string error)");
        c->r[3] = L; c->r[4] = 0; call_guest(c, LUA_SETTOP);
    }
    return st;
}

static int lua_global_type(ctx_t *c, uint32_t L, const char *g) {
    uint32_t gname = gstrdup(g, strlen(g));
    c->r[3] = L; c->r[4] = (uint64_t)(int64_t)LUA_GLOBALSINDEX; c->r[5] = gname; call_guest(c, LUA_GETFIELD);
    c->r[3] = L; c->r[4] = (uint64_t)(int64_t)-1; call_guest(c, LUA_TYPE);
    int t = (int)(int32_t)c->r[3];
    return t;
}
static double lua_global_number(ctx_t *c, uint32_t L, const char *g) {
    lua_global_type(c, L, g);
    c->r[3] = L; c->r[4] = (uint64_t)(int64_t)-1; call_guest(c, LUA_TONUMBER);
    double v = c->f[1].d;
    c->r[3] = L; c->r[4] = 0; call_guest(c, LUA_SETTOP);
    return v;
}
static const char *lua_global_string(ctx_t *c, uint32_t L, const char *g, char *out, size_t cap) {
    lua_global_type(c, L, g);
    c->r[3] = L; c->r[4] = (uint64_t)(int64_t)-1; c->r[5] = 0; call_guest(c, LUA_TOLSTRING);
    uint32_t s = (uint32_t)c->r[3];
    snprintf(out, cap, "%s", s ? (char *)gp(s) : "(nil)");
    c->r[3] = L; c->r[4] = 0; call_guest(c, LUA_SETTOP);
    return out;
}

static void log_drain(void);
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    log_drain();
    fprintf(stderr, "\n*** host exception 0x%08lx at host pc %p (exe offset 0x%llx; addr2line -e mnr.exe 0x%llx)\n",
            er->ExceptionCode, er->ExceptionAddress,
            (unsigned long long)((uintptr_t)er->ExceptionAddress - (uintptr_t)GetModuleHandleA(NULL)),
            (unsigned long long)((uintptr_t)er->ExceptionAddress - (uintptr_t)GetModuleHandleA(NULL) + 0x140000000ULL));
    fprintf(stderr, "    thread '%s' (guest thread 0x%08llx, lr %08llx, r1 %08llx, r3 %08llx)\n", my_treg ? my_treg->name : "?",
            crash_ctx ? (unsigned long long)crash_ctx->thread_id : 0ULL, crash_ctx ? (unsigned long long)crash_ctx->lr : 0ULL,
            crash_ctx ? (unsigned long long)crash_ctx->r[1] : 0ULL, crash_ctx ? (unsigned long long)crash_ctx->r[3] : 0ULL);
    fflush(stderr);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
        if (a >= (uintptr_t)guest_mem && a < (uintptr_t)guest_mem + 0x100000000ULL)
            fprintf(stderr, "    %s of guest address 0x%08llx\n", er->ExceptionInformation[0] ? "write" : "read",
                    (unsigned long long)(a - (uintptr_t)guest_mem));
        else
            fprintf(stderr, "    %s of host address %p (outside guest memory)\n", er->ExceptionInformation[0] ? "write" : "read", (void *)a);
    }
    fprintf(stderr, "    last functions entered on this thread (oldest first, function<-caller):\n     ");
    for (unsigned i = 0; i < 32; i++) {
        uint64_t e = trace_ring[(trace_pos + i) & 31];
        fprintf(stderr, " %08x<-%08x%s", (uint32_t)(e >> 32), (uint32_t)e, (i % 6 == 5) ? "\n     " : "");
    }
    if (crash_ctx) {
        fprintf(stderr, "\n    thread 0x%08llx", (unsigned long long)crash_ctx->thread_id);
        fprintf(stderr, "\n    r1=%08llx r2=%08llx r3=%08llx r4=%08llx r5=%08llx lr=%08llx ctr=%08llx",
                (unsigned long long)crash_ctx->r[1], (unsigned long long)crash_ctx->r[2], (unsigned long long)crash_ctx->r[3],
                (unsigned long long)crash_ctx->r[4], (unsigned long long)crash_ctx->r[5],
                (unsigned long long)crash_ctx->lr, (unsigned long long)crash_ctx->ctr);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* MNR_WATCH=<hex addr>[,<hex len>]: debug write watchpoint on guest memory. The page is made read-only;
 * a write into the range logs the writing thread's recent guest functions, single-steps the store with
 * the page writable, logs the new value and protects the page again. */
static uint32_t watch_lo, watch_hi;
static __thread uint32_t watch_hit;
static LONG WINAPI watch_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD old;
    uint8_t *page = guest_mem + (watch_lo & ~0xFFFu);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 && er->ExceptionInformation[0] == 1) {
        uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
        if (a < (uintptr_t)page || a >= (uintptr_t)page + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
        uint32_t g = (uint32_t)(a - (uintptr_t)guest_mem);
        watch_hit = g >= watch_lo && g < watch_hi ? g : 0;
        if (watch_hit) {
            printf("[watch] write to 0x%08x (old %08x); recent functions:", g, ld_u32(g & ~3u));
            for (unsigned i = 24; i < 32; i++) { uint64_t e = trace_ring[(trace_pos + i) & 31]; printf(" %08x<-%08x", (uint32_t)(e >> 32), (uint32_t)e); }
            printf("\n");
        }
        VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
        ep->ContextRecord->EFlags |= 0x100;   /* trap after the store */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && watch_lo) {
        if (watch_hit) printf("[watch]   0x%08x now %08x\n", watch_hit, ld_u32(watch_hit & ~3u));
        watch_hit = 0;
        VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void watch_init(void) {
    const char *e = getenv("MNR_WATCH");
    if (!e) return;
    { static int armed; const char *at = getenv("MNR_WATCH_AT");   /* arm at that game flip (heap pages exist by then) */
      if (at && !armed && gcm_flips < atol(at)) return;
      if (armed++) return; }
    char *end; watch_lo = (uint32_t)strtoul(e, &end, 16); watch_hi = watch_lo + (*end == ',' ? (uint32_t)strtoul(end + 1, NULL, 16) : 4);
    DWORD old;
    AddVectoredExceptionHandler(1, watch_handler);
    VirtualProtect(guest_mem + (watch_lo & ~0xFFFu), 0x1000, PAGE_READONLY, &old);
    printf("[watch] watching 0x%08x..0x%08x\n", watch_lo, watch_hi);
}

static char *read_file(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); *len = ftell(fp); fseek(fp, 0, SEEK_SET);
    char *b = malloc(*len);
    if (fread(b, 1, *len, fp) != *len) { fclose(fp); free(b); return NULL; }
    fclose(fp);
    return b;
}

/* The log reaches the launcher (a pipe) or a console window, and both can be slow readers: written a line at a time,
 * a detailed log (every network packet) stalled the game's network threads until the servers dropped it from online
 * ModSpot. So stdout goes into a 16 MB pipe of our own, and this thread copies it out at whatever pace the reader
 * manages; a write only waits if 16 MB are backed up. */
#include <io.h>
#include <fcntl.h>
static HANDLE log_sink, log_rd, log_tee;   /* log_tee: MNR_LOG_FILE, a copy of the output while it goes to a console */
static HANDLE log_run;                     /* logs\run_<date>_<time>.txt: a copy of every run's output (run_log_open) */
/* The game's output never waits for its reader: one thread takes it from our pipe into memory at once, another
 * writes it on. When the launcher stopped reading (a busy .NET thread pool), the pipe filled, a printf blocked, and
 * the game froze on the loading screen. Beyond 64 MB waiting, output is dropped (and counted). */
typedef struct lchunk { struct lchunk *next; DWORD n; char d[]; } lchunk_t;
static lchunk_t *lq_head, *lq_tail; static size_t lq_bytes; static long lq_dropped; static volatile long lq_busy;
static SRWLOCK lq_lock = SRWLOCK_INIT; static CONDITION_VARIABLE lq_cv = CONDITION_VARIABLE_INIT;
static DWORD log_out_type, log_err_type; static int log_async_on; static HANDLE log_pump_h, log_writer_h;
static volatile long long log_pumped, log_written;
static void log_drain(void) {   /* at exit and on a crash: the last lines must reach the reader */
    fflush(stdout); fflush(stderr);
    DWORD left = 1;
    for (int i = 0; i < 150 && log_rd && PeekNamedPipe(log_rd, NULL, 0, NULL, &left, NULL) && left; i++) Sleep(10);
    for (int i = 0; i < 150 && log_rd && (lq_head || lq_busy); i++) Sleep(10);
    Sleep(log_rd ? 20 : 0);
}
static DWORD WINAPI log_pump(void *p) {   /* pipe -> memory */
    HANDLE rd = p; static char buf[1 << 16]; DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        AcquireSRWLockExclusive(&lq_lock);
        if (lq_bytes + n > (64u << 20)) lq_dropped += n;
        else {
            lchunk_t *c = malloc(sizeof *c + n);
            if (c) { c->next = NULL; c->n = n; memcpy(c->d, buf, n); if (lq_tail) lq_tail->next = c; else lq_head = c; lq_tail = c; lq_bytes += n; }
        }
        ReleaseSRWLockExclusive(&lq_lock); WakeConditionVariable(&lq_cv);
        log_pumped += n;
    }
    return 0;
}
void log_state(FILE *f) {   /* for the F9 dump */
    DWORD pend = 0; if (log_rd) PeekNamedPipe(log_rd, NULL, 0, NULL, &pend, NULL);
    fprintf(f, "\n===== log =====\nasync %d, stdout type %lu, stderr type %lu, pumped %lld, written %lld, queued %zu, in pipe %lu, dropped %ld, writer busy %ld\n",
            log_async_on, log_out_type, log_err_type, log_pumped, log_written, lq_bytes, pend, lq_dropped, lq_busy);
    HANDLE th[2] = {log_pump_h, log_writer_h}; const char *nm[2] = {"pump", "writer"};
    for (int i = 0; i < 2; i++) {
        if (!th[i] || SuspendThread(th[i]) == (DWORD)-1) continue;
        CONTEXT cx; cx.ContextFlags = CONTEXT_FULL; fprintf(f, "-- log %s:", nm[i]);
        if (GetThreadContext(th[i], &cx))
            for (int d = 0; d < 16 && cx.Rip; d++) {
                HMODULE m = NULL; char mn[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)cx.Rip, &m) && m) {
                    GetModuleFileNameA(m, mn, sizeof mn); const char *b = strrchr(mn, '\\'); if (b) memmove(mn, b + 1, strlen(b)); }
                fprintf(f, " %s+0x%llx", mn, (unsigned long long)(cx.Rip - (DWORD64)m));
                DWORD64 ib = 0; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(cx.Rip, &ib, NULL);
                if (fe) { void *hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, cx.Rip, fe, &cx, &hd, &est, NULL); } else { cx.Rip = *(DWORD64 *)cx.Rsp; cx.Rsp += 8; }
            }
        fprintf(f, "\n"); ResumeThread(th[i]);
    }
}
static DWORD WINAPI log_writer(void *p) {   /* memory -> the launcher's pipe / the console (may block: only this thread waits) */
    (void)p; DWORD w;
    for (;;) {
        AcquireSRWLockExclusive(&lq_lock);
        while (!lq_head) SleepConditionVariableSRW(&lq_cv, &lq_lock, INFINITE, 0);
        lchunk_t *c = lq_head; lq_head = c->next; if (!lq_head) lq_tail = NULL; lq_bytes -= c->n;
        long dropped = lq_dropped; lq_dropped = 0; lq_busy = 1;
        ReleaseSRWLockExclusive(&lq_lock);
        if (dropped) { char m[96]; int k = snprintf(m, sizeof m, "\n[log] %ld bytes of output dropped (the reader fell behind)\n", dropped); WriteFile(log_sink, m, (DWORD)k, &w, NULL); }
        WriteFile(log_sink, c->d, c->n, &w, NULL); if (log_tee) WriteFile(log_tee, c->d, c->n, &w, NULL);
        if (log_run) WriteFile(log_run, c->d, c->n, &w, NULL);
        log_written += c->n; free(c); lq_busy = 0;
    }
    return 0;
}
/* Every run's output also goes to MNR\logs\run_<date>_<time>.txt (the launcher's logs\last_run.txt is overwritten each
 * start), so several test runs can be kept and sent together; the newest 100 are kept. MNR_RUN_LOG=0 turns it off. */
static void run_log_open(void) {
    const char *off = getenv("MNR_RUN_LOG");
    if (off && *off == '0') return;
    char root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    if (getenv("MNR_ROOT")) snprintf(root, sizeof root, "%s", getenv("MNR_ROOT"));
    else { GetModuleFileNameA(NULL, root, sizeof root); for (int up = 0; up < 4; up++) { char *s = strrchr(root, '\\'); if (s) *s = 0; } }   /* as main does */
    snprintf(dir, sizeof dir, "%s\\logs", root);
    CreateDirectoryA(dir, NULL);
    {   /* keep the newest 99 + this one (names sort by date) */
        static char names[1024][40]; int n = 0; WIN32_FIND_DATAA fd; char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\run_*.txt", dir);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) { do { if (n < 1024 && strlen(fd.cFileName) < 40) strcpy(names[n++], fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
        qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))strcmp);
        for (int i = 0; i < n - 99; i++) { snprintf(path, sizeof path, "%s\\%s", dir, names[i]); DeleteFileA(path); }
    }
    SYSTEMTIME t; GetLocalTime(&t);
    for (int k = 1; k < 100 && !log_run; k++) {   /* two starts in the same second get _2, _3 ... */
        if (k == 1) snprintf(path, sizeof path, "%s\\run_%04u-%02u-%02u_%02u-%02u-%02u.txt", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        else snprintf(path, sizeof path, "%s\\run_%04u-%02u-%02u_%02u-%02u-%02u_%d.txt", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, k);
        log_run = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (log_run == INVALID_HANDLE_VALUE) { log_run = NULL; if (GetLastError() != ERROR_FILE_EXISTS) return; }
    }
}
static void log_async_setup(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = out && out != INVALID_HANDLE_VALUE ? GetFileType(out) : FILE_TYPE_UNKNOWN;
    log_out_type = type;
    { HANDLE e = GetStdHandle(STD_ERROR_HANDLE); log_err_type = e && e != INVALID_HANDLE_VALUE ? GetFileType(e) : 0; }
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_CHAR) { setvbuf(stdout, NULL, _IONBF, 0); return; }   /* a file: fast as it is */
    HANDLE rd, wr, sink;
    /* our own copy of the real output: _dup2 below closes the handle stdout had */
    if (!DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &sink, 0, FALSE, DUPLICATE_SAME_ACCESS)) { setvbuf(stdout, NULL, _IONBF, 0); return; }
    if (!CreatePipe(&rd, &wr, NULL, 16u << 20)) { CloseHandle(sink); setvbuf(stdout, NULL, _IONBF, 0); return; }
    out = sink;
    int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
    if (fd < 0 || _dup2(fd, _fileno(stdout)) != 0) { setvbuf(stdout, NULL, _IONBF, 0); return; }
    log_sink = out; log_rd = rd;
    { const char *lf = getenv("MNR_LOG_FILE");
      if (lf && *lf && type == FILE_TYPE_CHAR) { log_tee = CreateFileA(lf, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (log_tee == INVALID_HANDLE_VALUE) log_tee = NULL; } }
    run_log_open();
    SetStdHandle(STD_OUTPUT_HANDLE, wr);
    {   /* stderr too (it went to the launcher on a pipe of its own, and could block the game the same way) */
        HANDLE e = GetStdHandle(STD_ERROR_HANDLE); DWORD et = e && e != INVALID_HANDLE_VALUE ? GetFileType(e) : FILE_TYPE_UNKNOWN;
        if (et == FILE_TYPE_PIPE || et == FILE_TYPE_CHAR) { fflush(stderr); if (_dup2(fd, _fileno(stderr)) == 0) SetStdHandle(STD_ERROR_HANDLE, wr); }
    }
    atexit(log_drain);
    log_pump_h = CreateThread(NULL, 1 << 16, log_pump, rd, 0, NULL);
    log_writer_h = CreateThread(NULL, 1 << 16, log_writer, NULL, 0, NULL);
    log_async_on = 1;
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);   /* flushed every second by f9_thread, and at exit */
}

#ifdef MNR_PGO_GEN   /* build.py --pgo-gen, hidden profiling runs: MNR_PGO_EXIT_AFTER=<s> writes re\mnr.profraw and exits */
static DWORD WINAPI pgo_exit_thread(void *p) {
    extern void __llvm_profile_set_filename(const char *); extern int __llvm_profile_write_file(void);
    Sleep((DWORD)(uintptr_t)p * 1000);
    __llvm_profile_set_filename("mnr.profraw"); int r = __llvm_profile_write_file();
    printf("[pgo] profile written (%d), exiting\n", r); fflush(stdout);
    ExitProcess(0);
}
#endif
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s EBOOT.elf [file.LBIN ...]\n", argv[0]); return 1; }
    log_async_setup();
    timeBeginPeriod(1);   /* 1 ms timer resolution: guest sleeps and timed waits are short */
    SetUnhandledExceptionFilter(crash_handler);
    guest_mem = VirtualAlloc(NULL, 0x100000000ULL, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (!guest_mem || !VirtualAlloc(guest_mem, COMMIT, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "cannot reserve guest memory\n"); return 1;
    }
    load_elf(argv[1]);
    opd_alloc = host_register(host_lua_alloc, "lua_alloc");
    opd_panic = host_register(host_lua_panic, "lua_panic");

    static ctx_t ctx;
    ctx_t *c = &ctx;
    crash_ctx = c;
    {   /* which machine, for the [perf] lines */
        char cpu[128] = "?"; DWORD sz = sizeof cpu; MEMORYSTATUSEX ms = {sizeof ms}; SYSTEM_INFO si;
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &sz);
        GlobalMemoryStatusEx(&ms); GetSystemInfo(&si);
        printf("[sys] CPU %s, %lu logical cores, %.0f GB RAM\n", cpu, (unsigned long)si.dwNumberOfProcessors, ms.ullTotalPhys / 1073741824.0);
    }
    atexit(perf_exit);   /* after log_async_setup: atexit runs it before the log drains */
    thread_register(c, "main");
    c->r[1] = STACK_TOP - 0x100;
    /* Thread-local storage: PowerPC64 ABI as used on the PS3, r13 = TLS block + 0x7030
     * (code reaches TLS variables at negative offsets from r13). Block = PT_TLS template. */
    uint32_t tls = gmalloc(0x8000 + tls_memsz);
    memcpy(gp(tls), gp(tls_va), tls_filesz);
    memset(gp(tls + tls_filesz), 0, tls_memsz - tls_filesz);
    c->r[13] = tls + 0x7030;
    net_setup(gmalloc);
    hle_lenient = getenv("MNR_HLE_STRICT") == NULL;   /* unimplemented imports return 0 and are logged; MNR_HLE_STRICT=1 stops at the first */
    fs_log = getenv("MNR_FS_LOG") != NULL;
    syscall_log = getenv("MNR_SYSCALL_LOG") != NULL;
    if (getenv("MNR_TIMELINE")) CreateThread(NULL, 0, tl_thread, (void *)(uintptr_t)atoi(getenv("MNR_TIMELINE")), 0, NULL);
    if (getenv("MNR_PROFILE")) CreateThread(NULL, 0, profiler_thread, (void *)(uintptr_t)atoi(getenv("MNR_PROFILE")), 0, NULL);
#ifdef MNR_PGO_GEN
    if (getenv("MNR_PGO_EXIT_AFTER")) CreateThread(NULL, 0, pgo_exit_thread, (void *)(uintptr_t)atoi(getenv("MNR_PGO_EXIT_AFTER")), 0, NULL);
#endif
    CreateThread(NULL, 0, f9_thread, NULL, 0, NULL);
    if (getenv("MNR_DUMP_AFTER")) CreateThread(NULL, 0, watchdog_thread, (void *)(uintptr_t)atoi(getenv("MNR_DUMP_AFTER")), 0, NULL);
    {   /* file-system root: the MNR folder, three levels above recomp\out\mnr.exe */
        GetModuleFileNameA(NULL, mnr_root, sizeof mnr_root);
        for (int up = 0; up < 4; up++) { char *s = strrchr(mnr_root, '\\'); if (s) *s = 0; }
        if (getenv("MNR_ROOT")) snprintf(mnr_root, sizeof mnr_root, "%s", getenv("MNR_ROOT"));
    }
    composites_reset_once();
    sfo_load();

    if (argc >= 3 && !strcmp(argv[2], "--boot")) {
        /* lv2-style process start: entry OPD from the ELF header, arguments as RPCS3's loader
         * passes them (r3 argc, r4 argv, r5 envp, r7 main thread id, r8-r10 TLS template,
         * r12 malloc page size). The game calls sys_initialize_tls itself. */
        const char *path = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
        uint32_t gpath = gstrdup(path, strlen(path));
        /* lv2 passes argv/envp as arrays of 64-bit pointers; the CRT narrows them for main */
        uint32_t argvp = gmalloc(16), envp = gmalloc(8);
        st_u64(argvp, gpath); st_u64(argvp + 8, 0); st_u64(envp, 0);
        c->r[1] = STACK_TOP - 0x70;
        c->r[3] = 1; c->r[4] = argvp; c->r[5] = envp; c->r[6] = 0;
        c->r[7] = 0x01000500; c->r[8] = tls_va; c->r[9] = tls_filesz; c->r[10] = tls_memsz;
        c->r[11] = 0; c->r[12] = 0x100000;
        c->r[13] = 0;
        uint32_t entry = ld_u32(G_ENTRY_OPD);
        /* data patches: switches that select SPU jobs where the game has a PPU implementation.
         * 0xED1D95 (read by 0x2ABD00, 0x2AC318, 0x2AC36C, 0x2AE488): 1 = SPU job (completion flag at
         * object+0x2E0 set by the SPU), 0 = run the PPU path. */
        if (!getenv("MNR_SPU_2AB")) st_u8(G_SPU2AB_FLAG, 0);   /* MNR_SPU_2AB=1: keep the SPU path (test) */
        apply_patches();
        printf("booting EBOOT %s: entry 0x%08x, TOC 0x%08x\n", G_NAME, entry, ld_u32(G_ENTRY_OPD + 4));
        watch_init();
        call_guest(c, entry);
        printf("entry returned, r3 = 0x%llx\n", (unsigned long long)c->r[3]);
        return 0;
    }

    uint32_t L = lua_open_state(c);
    printf("lua_newstate -> L = 0x%08x (%llu allocator calls so far)\n", L, alloc_calls);

    /* 1. VM self-test in Lua source, compiled by the game's own parser */
    const char *test =
        "local s = 0 for i = 1, 100 do s = s + i end\n"
        "local t = {} for i = 1, 10 do t[i] = (i * 7) % 11 end table.sort(t)\n"
        "local function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end\n"
        "local parts = {} for w in string.gmatch('alpha beta gamma', '%a+') do parts[#parts+1] = string.upper(w) end\n"
        "local mt = setmetatable({}, {__index = function(_, k) return k * 2 end})\n"
        "sum100 = s\n"
        "sorted = table.concat(t, ',')\n"
        "fib20 = fib(20)\n"
        "words = table.concat(parts, '-')\n"
        "meta = mt[21]\n"
        "fmt = string.format('%d|%5.2f|%s|%x', 42, 3.14159, 'ok', 255)\n"
        "float = 1/3 + 2^10\n"
        "rep = string.rep('ab', 3) .. #('hello')\n";
    int st = lua_run(c, L, test, strlen(test), "=selftest");
    char buf[256];
    printf("self-test status %d\n", st);
    printf("  sum100 = %.17g (expect 5050)\n", lua_global_number(c, L, "sum100"));
    printf("  sorted = %s (expect 1,2,3,4,5,6,7,8,9,10)\n", lua_global_string(c, L, "sorted", buf, sizeof buf));
    printf("  fib20  = %.17g (expect 6765)\n", lua_global_number(c, L, "fib20"));
    printf("  words  = %s (expect ALPHA-BETA-GAMMA)\n", lua_global_string(c, L, "words", buf, sizeof buf));
    printf("  meta   = %.17g (expect 42)\n", lua_global_number(c, L, "meta"));
    printf("  fmt    = %s (expect 42| 3.14|ok|ff)\n", lua_global_string(c, L, "fmt", buf, sizeof buf));
    printf("  float  = %.17g (expect %.17g)\n", lua_global_number(c, L, "float"), 1.0 / 3.0 + 1024.0);
    printf("  rep    = %s (expect ababab5)\n", lua_global_string(c, L, "rep", buf, sizeof buf));

    /* 2. Game scripts (.LBIN bytecode), each in a fresh state */
    int ok = 0, total = 0;
    for (int i = 2; i < argc; i++) {
        size_t len;
        char *b = read_file(argv[i], &len);
        if (!b) { printf("cannot read %s\n", argv[i]); continue; }
        const char *base = strrchr(argv[i], '\\') ? strrchr(argv[i], '\\') + 1 : argv[i];
        uint32_t Ls = lua_open_state(c);
        int r = lua_run(c, Ls, b, len, base);
        total++; ok += r == 0;
        printf("script %s: status %d\n", base, r);
        if (!strcmp(base, "BOOSTPAD.LBIN"))
            printf("  BoostPadTrigger is a %s (expect function)\n",
                   lua_global_type(c, Ls, "BoostPadTrigger") == 6 ? "function" : "NOT a function");
        free(b);
    }
    if (total) printf("game scripts: %d of %d ran without error\n", ok, total);
    printf("allocator: %llu calls, %llu bytes; guest heap used %u KB\n",
           alloc_calls, alloc_bytes, (heap_top - HEAP_LO) / 1024);
    return 0;
}

/* Game patches: <MNR>/patches/<G_PATCH_FILE> (or MNR_PATCHES=<file>), applied once the executable is
 * loaded, before it starts (like RPCS3's patch.yml, whose data patches carry over as they are). One patch
 * per line: <byte|be16|be32|bef32> <hex address> <value>; '#' starts a comment, so a patch is turned off by
 * commenting it out. Patches to code (below G_TEXT_END) can't apply at run time: the code is recompiled
 * ahead of time. */
static void apply_patch_file(const char *path);
static void apply_patches(void) {
    char path[MAX_PATH];
    const char *e = getenv("MNR_PATCHES");
    if (e) snprintf(path, sizeof path, "%s", e); else snprintf(path, sizeof path, "%s/patches/%s", mnr_root, G_PATCH_FILE);
    apply_patch_file(path);
    const char *m = getenv("MNR_MOD_PATCHES");   /* enabled mods' patches.txt (the launcher sets this), in load order */
    while (m && *m) {
        const char *end = strchr(m, ';'); size_t n = end ? (size_t)(end - m) : strlen(m);
        if (n && n < sizeof path) { memcpy(path, m, n); path[n] = 0; apply_patch_file(path); }
        m = end ? end + 1 : NULL;
    }
}
static void apply_patch_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("[patch] can't open %s\n", path); return; }
    char line[512]; int n = 0, skipped = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char type[16], val[64]; unsigned long addr;
        if (sscanf(line, "%15s %lx %63s", type, &addr, val) != 3) continue;
        uint32_t a = (uint32_t)addr;
        if (a < G_TEXT_END) { if (!skipped++) printf("[patch] %s: patches to code (0x%08x...) are not supported (recompiled ahead of time)\n", path, a); continue; }
        if (!strcmp(type, "byte")) st_u8(a, (uint8_t)strtoul(val, NULL, 0));
        else if (!strcmp(type, "be16")) st_u16(a, (uint16_t)strtoul(val, NULL, 0));
        else if (!strcmp(type, "be32")) st_u32(a, (uint32_t)strtoul(val, NULL, 0));
        else if (!strcmp(type, "bef32")) { float fv = strtof(val, NULL); uint32_t u; memcpy(&u, &fv, 4); st_u32(a, u); }
        else { printf("[patch] unknown type '%s'\n", type); continue; }
        n++;
    }
    fclose(f);
    printf("[patch] %d patch%s from %s\n", n, n == 1 ? "" : "es", path);
}
/* the game's debug printf (0x986314, an empty varargs function in retail): MNR_GAME_LOG=1 formats and
 * prints it. PPC64 varargs: argument k is in r3+k for k < 8, else in the parameter save area at r1+48+8k
 * (doubles are passed there too, as their bit pattern). */
static uint64_t game_arg(ctx_t *c, int k) { return k < 8 ? c->r[3 + k] : ld_u64((uint32_t)c->r[1] + 48 + 8 * k); }
void prehook_game_printf(ctx_t *c) {
    {   /* the career's LoadUnlocks (0x332DA4) ends with this log call, its unlock table in r29: tracks unlocked here */
        if ((uint32_t)c->lr == 0x333034) career_unlock_tracks((uint32_t)c->r[29]);
    }
    static int on = -1;
    if (on < 0) { const char *e = getenv("MNR_GAME_LOG"); on = e && *e == '1'; }
    if (!on || !ARG32(0)) return;
    const char *f = (const char *)gp(ARG32(0));
    char out[1024]; size_t o = 0; int k = 1;
    for (int i = 0; f[i] && o < sizeof out - 64 && i < 900; i++) {
        if (f[i] != '%') { out[o++] = f[i]; continue; }
        char spec[16]; int s = 0; spec[s++] = '%'; i++;
        while (f[i] && strchr("-+ #0123456789.lhzjt", f[i]) && s < 12) { if (!strchr("lhzjt", f[i])) spec[s++] = f[i]; i++; }
        char cv = f[i]; if (!cv) break;
        if (cv == '%') { out[o++] = '%'; continue; }
        uint64_t v = game_arg(c, k++);
        if (cv == 's') { spec[s++] = 's'; spec[s] = 0; const char *str = (uint32_t)v ? (const char *)gp((uint32_t)v) : "(null)"; o += snprintf(out + o, sizeof out - o, spec, str); }
        else if (strchr("fFeEgG", cv)) { double d; memcpy(&d, &v, 8); spec[s++] = cv; spec[s] = 0; o += snprintf(out + o, sizeof out - o, spec, d); }
        else if (cv == 'c') { out[o++] = (char)v; }
        else if (cv == 'p') { o += snprintf(out + o, sizeof out - o, "0x%08x", (uint32_t)v); }
        else { spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = cv; spec[s] = 0; o += snprintf(out + o, sizeof out - o, spec, strchr("di", cv) ? (long long)(int32_t)v : (long long)(uint32_t)v); }
        if (o >= sizeof out) o = sizeof out - 1;
    }
    out[o] = 0;
    { if (!race_began && strstr(out, "GameStateKart_Race OnEnter")) race_began = 1; }
    static int stamp = -1; if (stamp < 0) stamp = getenv("MNR_GAME_LOG_TIME") != NULL;   /* tests: seconds since boot */
    if (stamp) { printf("[game] @%.3f %s%s", GetTickCount64() / 1000.0, out, o && out[o - 1] == '\n' ? "" : "\n"); return; }
    printf("[game] %s%s", out, o && out[o - 1] == '\n' ? "" : "\n");
}

/* Voice chat (debug, MNR_VOICE_LOG=1): each step of the game's NetCodeVoipPlayer setup that ends in cellVoiceInit */
static void voice_trace(ctx_t *c, const char *what) {
    static int on = -1; if (on < 0) on = getenv("MNR_VOICE_LOG") != NULL;
    static int n; if (!on || n++ > 200) return;
    printf("[voice] %s (lr 0x%08x, r3 0x%08x r4 0x%08x)\n", what, (uint32_t)c->lr, (uint32_t)R(3), (uint32_t)R(4));
}
void prehook_voice_a(ctx_t *c) { voice_trace(c, "8eb484 (vtable slot 22)"); }
void prehook_voice_b(ctx_t *c) { voice_trace(c, "91c168"); }
void prehook_voice_c(ctx_t *c) { voice_trace(c, "91b684"); }
void prehook_voice_d(ctx_t *c) { voice_trace(c, "91aacc"); }
void prehook_voice_e(ctx_t *c) { voice_trace(c, "91a7f0 (calls cellVoiceInit)"); }
void prehook_voice_f(ctx_t *c) { voice_trace(c, "8ef6d0 (voip ctor A)"); }
void prehook_voice_g(ctx_t *c) { voice_trace(c, "8efc78 (voip ctor B)"); }
void prehook_voice_h(ctx_t *c) { voice_trace(c, "8f01fc (voip ctor C)"); }
void prehook_voice_i(ctx_t *c) { voice_trace(c, "8d7c84 (calls ctor C)"); }

/* voice object methods (debug, MNR_VOICE_LOG=1): the first calls of each */
static void voice_m(ctx_t *c, int slot, int *n) { if (*n < 3) { (*n)++; char w[48]; snprintf(w, sizeof w, "method slot %d", slot); voice_trace(c, w); } }
void prehook_vm_2(ctx_t *c) { static int n; voice_m(c, 2, &n); }
void prehook_vm_3(ctx_t *c) { static int n; voice_m(c, 3, &n); }
void prehook_vm_4(ctx_t *c) { static int n; voice_m(c, 4, &n); }
void prehook_vm_5(ctx_t *c) { static int n; voice_m(c, 5, &n); }
void prehook_vm_6(ctx_t *c) { static int n; voice_m(c, 6, &n); }
void prehook_vm_8(ctx_t *c) { static int n; voice_m(c, 8, &n); }
void prehook_vm_9(ctx_t *c) { static int n; voice_m(c, 9, &n); }
void prehook_vm_10(ctx_t *c) { static int n; voice_m(c, 10, &n); }
void prehook_vm_11(ctx_t *c) { static int n; voice_m(c, 11, &n); }
void prehook_vm_12(ctx_t *c) { static int n; voice_m(c, 12, &n); }
void prehook_vm_13(ctx_t *c) { static int n; voice_m(c, 13, &n); }
void prehook_vm_14(ctx_t *c) { static int n; voice_m(c, 14, &n); }
void prehook_vm_15(ctx_t *c) { static int n; voice_m(c, 15, &n); }
void prehook_vm_18(ctx_t *c) { static int n; voice_m(c, 18, &n); }
void prehook_vm_19(ctx_t *c) { static int n; voice_m(c, 19, &n); }
void prehook_vm_20(ctx_t *c) { static int n; voice_m(c, 20, &n); }
void prehook_vm_21(ctx_t *c) { static int n; voice_m(c, 21, &n); }
void prehook_vm_23(ctx_t *c) { static int n; voice_m(c, 23, &n); }
void prehook_vm_24(ctx_t *c) { static int n; voice_m(c, 24, &n); }
void prehook_vm_25(ctx_t *c) { static int n; voice_m(c, 25, &n); }
void prehook_vm_26(ctx_t *c) { static int n; voice_m(c, 26, &n); }
void prehook_vm_113(ctx_t *c) { static int n; voice_m(c, 113, &n); }
void prehook_vm_114(ctx_t *c) { static int n; voice_m(c, 114, &n); }
void prehook_vm_1113(ctx_t *c) { static int n; voice_m(c, 1113, &n); }
void prehook_vm_1114(ctx_t *c) { static int n; voice_m(c, 1114, &n); }
void prehook_vm_1115(ctx_t *c) { static int n; voice_m(c, 1115, &n); }
