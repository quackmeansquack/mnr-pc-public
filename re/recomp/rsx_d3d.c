/* Direct3D 11 backend for the RSX (MNR_RSX=d3d).
 *
 * The front end is shared with the software renderer: rsx.c tracks method state and rsx_render.c
 * collects each draw's element list, then hands it here. Programs are translated to HLSL
 * (rsx_hlsl.c) and compiled once; vertex shaders fetch raw big-endian vertex data from a
 * ByteAddressBuffer, so no input layouts are needed.
 *
 * Surfaces: every RSX colour/depth surface (keyed by guest address) is a GPU texture sized in samples
 * (multisampled RSX surfaces are 2x wide / 2x2). A new surface is initialised from guest memory, and
 * reloaded when the CPU writes its pages (write-watch generations). Small surfaces are written back to
 * guest memory at semaphore releases and flips, because the game reads some render results on the CPU
 * (e.g. character composites before the DXT job). Textures whose address is a surface sample that
 * surface directly; others are decoded (raw A,R,G,B) and uploaded once per write generation. */
#define COBJMACROS
#define INITGUID
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include "runtime.h"
#include "rsx.h"
#include "rsx_shader.h"
#include "rsx_hlsl.h"
#include "rsx_tex.h"
#include "rsx_d3d.h"
/* a debug switch read once (getenv is far too slow for per-draw checks) */
#define ENVFLAG(name) ({ static int v_ = -1; if (v_ < 0) v_ = getenv(name) != NULL; v_; })
#define ENVSTR(name) ({ static const char *v_ = (const char *)-1; if (v_ == (const char *)-1) v_ = getenv(name); v_; })   /* (the same, for a value) */

extern uint32_t rsx_vp[512][4];
extern uint32_t rsx_vpc[468][4];
uint32_t rsx_fp_addr(void);
#define REG(m) rsx_reg(0, (m))
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11DeviceContext1 *ctx1;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *back_rtv;
static HWND hwnd;
static int failed;

/* ------------------------------------------------------------------ shader compilation */
double rsx_d3d_compile_s, rsx_d3d_upload_s; long rsx_d3d_compiles, rsx_d3d_uploads, rsx_d3d_skip_vs, rsx_d3d_skip_ps;
/* for the [perf] lines (host.c): real compiles (not disk-cache hits), their time, draws skipped while one ran, and draws
 * that waited for one (off-screen targets); totals since start, read as differences */
volatile long rsx_d3d_sh_compiles, rsx_d3d_sh_compile_us, rsx_d3d_sh_skipped, rsx_d3d_sh_waited;
static double now_s(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / f.QuadPart; }
static uint64_t fnv(const void *p, size_t n, uint64_t h);
static void *compile_uncached(const char *src, const char *profile, size_t *len);
static long stepdump_force = -1;   /* MNR_D3D_WATCHSTEP: the flip MNR_D3D_STEPDUMP should dump (debug) */
static long long fpdump_want = -2;   /* MNR_D3D_FPDUMP / MNR_D3D_FPDUMP_TEX (debug) */
/* compiled bytecode is cached on disk (MNR\re\shadercache\<hash>.cso), keyed by source and profile */
static void cache_path(const char *src, const char *profile, char path[MAX_PATH]) {
    uint64_t h = fnv(profile, strlen(profile), fnv(src, strlen(src), 1469598103934665603ull));
    CreateDirectoryA("shadercache", NULL);
    snprintf(path, MAX_PATH, "shadercache\\%016llx.cso", (unsigned long long)h);
}
static void *cache_load(const char *path, size_t *len) {   /* the disk cache's bytecode, or NULL */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) == (size_t)n) { fclose(f); *len = (size_t)n; return p; }
    free(p); fclose(f); return NULL;
}
static void *compile_store(const char *src, const char *profile, const char *path, size_t *len) {   /* compile, add to the disk cache */
    void *code = compile_uncached(src, profile, len);
    FILE *f;
    if (code && (f = fopen(path, "wb"))) { fwrite(code, 1, *len, f); fclose(f); }
    return code;
}
static void *compile(const char *src, const char *profile, size_t *len) {
    char path[MAX_PATH]; cache_path(src, profile, path);
    void *code = cache_load(path, len);
    return code ? code : compile_store(src, profile, path, len);
}
static void *compile_uncached(const char *src, const char *profile, size_t *len) {
    double t0 = now_s();
    void *r = NULL;
    InterlockedIncrement((volatile LONG *)&rsx_d3d_compiles); InterlockedIncrement((volatile LONG *)&rsx_d3d_sh_compiles);
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "rsx", NULL, NULL, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        static int nfail;
        char name[64]; snprintf(name, sizeof name, "d3d_fail_%d.txt", nfail++);
        FILE *f = fopen(name, "w");
        if (f) { fputs(src, f); if (err) fprintf(f, "\n/*\n%s\n*/\n", (const char *)ID3D10Blob_GetBufferPointer(err)); fclose(f); }
        if (err) ID3D10Blob_Release(err);
        InterlockedAdd((volatile LONG *)&rsx_d3d_sh_compile_us, (LONG)((now_s() - t0) * 1e6));
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    *len = ID3D10Blob_GetBufferSize(code);
    r = malloc(*len); memcpy(r, ID3D10Blob_GetBufferPointer(code), *len);
    ID3D10Blob_Release(code);
    InterlockedAdd((volatile LONG *)&rsx_d3d_sh_compile_us, (LONG)((now_s() - t0) * 1e6));
    return r;
}
static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
/* the same idea 8 bytes at a time (vertex program keys: the byte-wise one showed in race profiles) */
static uint64_t hash64(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void *state_cache_get(uint64_t k); static void state_cache_put(uint64_t k, void *o);
typedef struct { uint64_t key; ID3D11VertexShader *vs; ID3D11PixelShader *ps; int nconst; volatile int bad; volatile LONG pending; } shader_t;
#define NSHADER 8192
static shader_t shaders[NSHADER];
static shader_t *shader_slot(uint64_t key) {
    uint32_t i = (uint32_t)(key % NSHADER);
    for (int k = 0; k < NSHADER; k++, i = (i + 1) % NSHADER) {
        if (shaders[i].key == key) return &shaders[i];
        if (!shaders[i].key) { shaders[i].key = key; return &shaders[i]; }
    }
    return NULL;
}
static char *src_buf;
static int sh_skip;   /* set by get_vs/get_ps: the draw is skipped because its shader is still compiling */

/* Background compiles. A shader not in the disk cache is compiled (D3DCompile, tens of ms) on a worker thread instead of
 * inside the draw, which made the first race on a track hitch. Meanwhile draws into the main scene are skipped (the
 * object shows a frame or two late, once); draws into other targets wait for it, since some run only once (textures the
 * game renders at load time, avatar pictures). MNR_D3D_SYNC_SHADERS=1 compiles in the draw as before. */
typedef struct sh_job { struct sh_job *next; shader_t *s; char *src; int ps; } sh_job_t;
static sh_job_t *sh_head, *sh_tail; static SRWLOCK sh_lock = SRWLOCK_INIT; static CONDITION_VARIABLE sh_cv = CONDITION_VARIABLE_INIT;
static int sh_async = -1;
static void sh_list_load(void);   /* (the shader list, below) */
static DWORD WINAPI sh_worker(void *arg) {
    (void)arg;
    for (;;) {
        AcquireSRWLockExclusive(&sh_lock);
        while (!sh_head) SleepConditionVariableSRW(&sh_cv, &sh_lock, INFINITE, 0);
        sh_job_t *j = sh_head; sh_head = j->next; if (!sh_head) sh_tail = NULL;
        ReleaseSRWLockExclusive(&sh_lock);
        char path[MAX_PATH]; size_t len = 0; const char *prof = j->ps ? "ps_5_0" : "vs_5_0";
        cache_path(j->src, prof, path);
        void *code = cache_load(path, &len);
        if (!code) code = compile_store(j->src, prof, path, &len);
        int bad = 1;
        if (code) {   /* (the device is free-threaded: creating shaders off the render thread is allowed) */
            if (j->ps) { ID3D11PixelShader *o = NULL; if (SUCCEEDED(ID3D11Device_CreatePixelShader(dev, code, len, NULL, &o))) { j->s->ps = o; bad = 0; } }
            else { ID3D11VertexShader *o = NULL; if (SUCCEEDED(ID3D11Device_CreateVertexShader(dev, code, len, NULL, &o))) { j->s->vs = o; bad = 0; } }
            free(code);
        }
        if (bad) j->s->bad = 1;
        MemoryBarrier();
        InterlockedExchange(&j->s->pending, 0);
        free(j->src); free(j);
    }
    return 0;
}
/* 1: the shader (HLSL in src_buf) is being compiled in the background; 0: compile it here (async off, or in the disk cache) */
static int sh_submit(shader_t *s, int ps) {
    if (sh_async < 0) {
        sh_async = getenv("MNR_D3D_SYNC_SHADERS") == NULL;
        if (sh_async) { sh_list_load(); SYSTEM_INFO si; GetSystemInfo(&si); int n = si.dwNumberOfProcessors >= 8 ? 3 : 2;
            for (int i = 0; i < n; i++) { HANDLE h = CreateThread(NULL, 0, sh_worker, NULL, 0, NULL); if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); } } }
    }
    if (!sh_async) return 0;
    char path[MAX_PATH]; cache_path(src_buf, ps ? "ps_5_0" : "vs_5_0", path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 0;   /* in the disk cache: quick, load it here */
    sh_job_t *j = malloc(sizeof *j); if (!j) return 0;
    j->src = _strdup(src_buf); if (!j->src) { free(j); return 0; }
    j->s = s; j->ps = ps; j->next = NULL;
    s->pending = 1;
    AcquireSRWLockExclusive(&sh_lock);
    if (sh_tail) sh_tail->next = j; else sh_head = j;
    sh_tail = j;
    ReleaseSRWLockExclusive(&sh_lock);
    WakeConditionVariable(&sh_cv);
    return 1;
}
/* Shader list (re\shadercache\programs.bin): every program the game drew with, as the RSX program and the state the
 * translation depends on - not compiled code. At startup a background thread translates them all with the current
 * translator and compiles what isn't in the disk cache yet, so after an update that changed the translation the
 * shaders are rebuilt at the title screen instead of mid-race. Records: {u8 kind (1 VP, 2 FP), u8 pad[3], u32 size}
 * then VP {u32 start, u32 nwords, words[4*n] as in rsx_vp} / FP {u32 ctl, u8 dims[16], bytes}. */
#define SH_LIST "shadercache\\programs.bin"
static uint64_t sh_seen[16384]; static int sh_seen_n; static SRWLOCK sh_list_lock = SRWLOCK_INIT;
static int sh_seen_add(uint64_t k) {   /* 1 if new (open addressing on the key) */
    uint32_t i = (uint32_t)(k % 16384);
    for (int t = 0; t < 16384; t++, i = (i + 1) % 16384) { if (sh_seen[i] == k) return 0; if (!sh_seen[i]) { if (sh_seen_n > 15000) return 0; sh_seen[i] = k; sh_seen_n++; return 1; } }
    return 0;
}
static void sh_list_add(int kind, uint64_t key, const void *head, uint32_t hn, const void *body, uint32_t bn) {
    static int off = -1; if (off < 0) off = getenv("MNR_D3D_SYNC_SHADERS") != NULL;
    if (off || !key) return;
    AcquireSRWLockExclusive(&sh_list_lock);
    if (sh_seen_add(key)) {
        CreateDirectoryA("shadercache", NULL);
        FILE *f = fopen(SH_LIST, "ab");
        if (f) { uint8_t h[8] = {(uint8_t)kind}; uint32_t sz = hn + bn; memcpy(h + 4, &sz, 4); fwrite(h, 8, 1, f); fwrite(head, hn, 1, f); fwrite(body, bn, 1, f); fclose(f); }
    }
    ReleaseSRWLockExclusive(&sh_list_lock);
}
static DWORD WINAPI sh_precompile(void *arg) {   /* the startup pass over the list */
    (void)arg;
    FILE *f = fopen(SH_LIST, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return 0; }
    fclose(f);
    char *src = malloc(1 << 20); fp_insn_t *prog = malloc(sizeof(fp_insn_t) * FP_MAX_INSNS); uint32_t (*vp)[4] = calloc(512, 16);
    long done = 0, built = 0; double t0 = now_s();
    for (long o = 0; src && prog && vp && o + 8 <= n; ) {
        int kind = d[o]; uint32_t sz; memcpy(&sz, d + o + 4, 4); const uint8_t *p = d + o + 8;
        if (o + 8 + (long)sz > n) break;
        o += 8 + sz;
        const char *prof = NULL; int ok = 0;
        if (kind == 1 && sz >= 8) {
            uint32_t start, nw; memcpy(&start, p, 4); memcpy(&nw, p + 4, 4);
            if (start < 512 && nw && start + nw <= 512 && sz == 8 + nw * 16) {
                memset(vp, 0, 512 * 16); memcpy(vp[start], p + 8, nw * 16);
                ok = !hlsl_vp((const uint32_t (*)[4])vp, start, src, 1 << 20, NULL); prof = "vs_5_0";
            }
        } else if (kind == 2 && sz > 20) {
            uint32_t ctl; uint8_t dims[16]; memcpy(&ctl, p, 4); memcpy(dims, p + 4, 16);
            int ni = fp_decode_buf(p + 20, sz - 20, prog, FP_MAX_INSNS), nc;
            ok = ni > 0 && !hlsl_fp(prog, ni, ctl, dims, src, 1 << 20, &nc); prof = "ps_5_0";
        }
        if (!ok) continue;
        char path[MAX_PATH]; cache_path(src, prof, path); done++;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) continue;
        size_t len; void *code = compile_store(src, prof, path, &len); if (code) { built++; free(code); }
    }
    printf("[d3d] shader list: %ld programs, %ld compiled ahead (%.1f s)\n", done, built, now_s() - t0);
    free(src); free(prog); free(vp); free(d);
    return 0;
}
static void sh_list_load(void) {   /* known keys, so the list isn't appended twice; then the precompile thread */
    FILE *f = fopen(SH_LIST, "rb");
    if (f) {
        uint8_t h[8];
        while (fread(h, 8, 1, f) == 1) {
            uint32_t sz; memcpy(&sz, h + 4, 4); if (sz > (1u << 20)) break;
            uint8_t *b = malloc(sz); if (!b || fread(b, 1, sz, f) != sz) { free(b); break; }
            uint64_t k = 0;
            if (h[0] == 1 && sz >= 8) { uint32_t start; memcpy(&start, b, 4); k = hash64(b + 8, sz - 8, fnv(&start, 4, 1469598103934665603ull)) | 1; }
            else if (h[0] == 2) k = fnv(b, sz, 0x5053) | 2;
            if (k) sh_seen_add(k);
            free(b);
        }
        fclose(f);
        HANDLE t = CreateThread(NULL, 0, sh_precompile, NULL, 0, NULL);
        if (t) { SetThreadPriority(t, THREAD_PRIORITY_LOWEST); CloseHandle(t); }
    }
}
/* a shader still compiling: 0 = skip the draw (main scene), or wait for it (other targets: sh_wait_pending) */
static int sh_wait_pending;   /* set per draw by rsx_d3d_draw */
static int sh_ready(shader_t *s) {
    if (!s->pending) return 1;
    if (!sh_wait_pending) return 0;
    InterlockedIncrement(&rsx_d3d_sh_waited);
    while (s->pending) Sleep(0);
    return 1;
}

extern uint32_t rsx_vp_gen_pub;
static ID3D11VertexShader *get_vs(void) {
    static uint32_t last_gen = 0xFFFFFFFF, last_start; static ID3D11VertexShader *last_vs;
    uint32_t start = REG(0x1EA0) & 511, end = start;
    if (rsx_vp_gen_pub == last_gen && start == last_start && last_vs) return last_vs;
    last_gen = rsx_vp_gen_pub; last_start = start; last_vs = NULL;
    while (end < 511 && !(rsx_vp[end][3] & 1)) end++;
    uint64_t key = hash64(&rsx_vp[start], (end - start + 1) * 16, fnv(&start, 4, 1469598103934665603ull)) | 1;
    shader_t *s = shader_slot(key);
    if (!s) return NULL;
    if (!sh_ready(s)) { last_gen = 0xFFFFFFFF; sh_skip = 1; return NULL; }
    if (s->bad) return NULL;
    if (s->vs) return last_vs = s->vs;
    if (getenv("MNR_D3D_TXLLOG")) {   /* debug: vertex programs that fetch vertex textures */
        for (uint32_t i = start; i <= end; i++) { vp_insn_t in; vp_decode(rsx_vp[i], &in);
            if (in.vec_op == 24) { printf("[d3d] VP %u uses TXL unit %u", start, in.tex_num);
                for (int t = 0; t < 4; t++) printf(" | vt%d off %08x fmt %08x ctl %08x rect %08x", t, REG(0x900 + 0x20 * t), REG(0x904 + 0x20 * t), REG(0x90C + 0x20 * t), REG(0x918 + 0x20 * t));
                printf("\n"); break; } }
    }
    if (hlsl_vp((const uint32_t (*)[4])rsx_vp, start, src_buf, 1 << 20, NULL)) {
        s->bad = 1;
        if (getenv("MNR_D3D_FPFAIL")) {   /* debug: disassembly of vertex programs the translator rejects */
            char nm[64]; snprintf(nm, sizeof nm, "vp_fail_%u_%08x.txt", start, (uint32_t)key); FILE *f = fopen(nm, "w");
            if (f) { char b[512]; for (uint32_t i = start; i <= end; i++) { vp_insn_t in; vp_decode(rsx_vp[i], &in); vp_disasm(&in, b, sizeof b); fprintf(f, "%3u: vec %2u sca %2u  %s\n", i, in.vec_op, in.sca_op, b); } fclose(f); }
        }
        return NULL;
    }
    {   uint32_t hd[2] = {start, end - start + 1};   /* the shader list: this program */
        sh_list_add(1, hash64(&rsx_vp[start], (end - start + 1) * 16, fnv(&start, 4, 1469598103934665603ull)) | 1, hd, 8, &rsx_vp[start], (end - start + 1) * 16); }
    if (sh_submit(s, 0)) { last_gen = 0xFFFFFFFF; if (!sh_ready(s)) { sh_skip = 1; return NULL; } return s->bad ? NULL : (last_vs = s->vs); }
    size_t len; void *code = compile(src_buf, "vs_5_0", &len);
    if (!code) { s->bad = 1; return NULL; }
    if (FAILED(ID3D11Device_CreateVertexShader(dev, code, len, NULL, &s->vs))) s->bad = 1;
    free(code);
    return last_vs = s->vs;
}
static ID3D11PixelShader *get_ps(uint32_t fp_addr, const fp_insn_t *prog, int n, uint32_t ctl, const uint8_t dims[16], int *nconst) {
    uint64_t key = 1469598103934665603ull ^ 0x5053;
    uint32_t a = fp_addr;
    for (int i = 0; i < n; i++) {   /* (a word-wise mix: the byte-wise FNV of every draw's program showed in race profiles) */
        uint64_t w[2]; memcpy(w, gp(a), 16);
        key = (key ^ w[0]) * 0x9E3779B97F4A7C15ull; key ^= key >> 29; key = (key ^ w[1]) * 0x9E3779B97F4A7C15ull; key ^= key >> 29;
        a += prog[i].has_const ? 32 : 16;
    }
    key = fnv(&ctl, 4, fnv(dims, 16, key)) & ~1ull;
    shader_t *s = shader_slot(key ? key : 2);
    if (!s) return NULL;
    if (!sh_ready(s)) { sh_skip = 1; return NULL; }
    if (s->bad) return NULL;
    if (s->ps) { *nconst = s->nconst; return s->ps; }
    if (hlsl_fp(prog, n, ctl, dims, src_buf, 1 << 20, &s->nconst)) {
        s->bad = 1;
        if (getenv("MNR_D3D_FPFAIL")) {   /* debug: disassembly of fragment programs the translator rejects */
            char nm[64]; snprintf(nm, sizeof nm, "fp_fail_%08x.txt", fp_addr); FILE *f = fopen(nm, "w");
            if (f) { char b[512]; for (int i = 0; i < n; i++) { fp_disasm(&prog[i], b, sizeof b); fprintf(f, "%3d: op %02x else %u end %u rep %u/%u/%u  %s\n", i, prog[i].op, prog[i].else_off, prog[i].end_off, prog[i].rep_init, prog[i].rep_inc, prog[i].rep_count, b); } fclose(f); }
        }
        return NULL;
    }
    {   /* MNR_D3D_FPDUMP=<hex FP register value, as MNR_D3D_SAVETARGET prints it>: disassembly and HLSL of that program (debug) */
        if (fpdump_want == -2) { const char *e = getenv("MNR_D3D_FPDUMP"); fpdump_want = !e ? -1 : !strcmp(e, "all") ? -3 : (long long)strtoull(e, NULL, 16); }   /* "all": every program */
        long long want = fpdump_want;
        if (want == -3 || (want >= 0 && REG(0x08E4) == (uint32_t)want)) {
            char nm[64]; snprintf(nm, sizeof nm, "fp_%08x.txt", REG(0x08E4)); FILE *f = fopen(nm, "w");
            if (f) { char b[512]; for (int i = 0; i < n; i++) { fp_disasm(&prog[i], b, sizeof b); fprintf(f, "%3d: %s\n", i, b); }
                     fprintf(f, "\nctl 0x%08x dims", ctl); for (int i = 0; i < 16; i++) fprintf(f, " %u", dims[i]); fprintf(f, "\n\n%s\n", src_buf); fclose(f); }
        }
    }
    {   uint32_t nb = fp_bytes(prog, n); const uint8_t *g = gp(fp_addr);   /* the shader list: this program and its state */
        uint8_t hd[20]; memcpy(hd, &ctl, 4); memcpy(hd + 4, dims, 16);
        uint8_t *rec = malloc(20 + nb);
        if (rec) { memcpy(rec, hd, 20); memcpy(rec + 20, g, nb); sh_list_add(2, fnv(rec, 20 + nb, 0x5053) | 2, rec, 20 + nb, NULL, 0); free(rec); } }
    if (sh_submit(s, 1)) { if (!sh_ready(s)) { sh_skip = 1; return NULL; } *nconst = s->nconst; return s->bad ? NULL : s->ps; }
    size_t len; void *code = compile(src_buf, "ps_5_0", &len);
    if (!code) { s->bad = 1; return NULL; }
    if (FAILED(ID3D11Device_CreatePixelShader(dev, code, len, NULL, &s->ps))) s->bad = 1;
    free(code);
    *nconst = s->nconst;
    return s->ps;
}

/* ------------------------------------------------------------------ buffers */
static ID3D11Buffer *cb_vpc, *cb_draw, *cb_fpc, *cb_fpd;
static ID3D11Buffer *vbuf, *ebuf; static uint32_t vbuf_size, ebuf_size, vbuf_pos, ebuf_pos;
static ID3D11ShaderResourceView *vbuf_srv, *ebuf_srv;

static ID3D11Buffer *make_cb(uint32_t size) {
    D3D11_BUFFER_DESC d = {(size + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *b = NULL; ID3D11Device_CreateBuffer(dev, &d, NULL, &b); return b;
}
static void upload(ID3D11Buffer *b, const void *data, size_t n) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, n);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)b, 0);
    }
}
/* ring append: NO_OVERWRITE while there is room, DISCARD when wrapping */
static uint32_t ring_put(ID3D11Buffer *b, uint32_t *pos, uint32_t size, const void *data, uint32_t n, uint32_t align) {
    uint32_t off = (*pos + align - 1) / align * align;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (off + n > size) { off = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b, 0, mode, 0, &m))) return 0;
    memcpy((uint8_t *)m.pData + off, data, n);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)b, 0);
    *pos = off + n;
    return off;
}
/* the same, for a caller that fills the space itself: returns where to write (unmap with ring_unmap), NULL on failure */
static uint32_t vbuf_lap;   /* +1 whenever the vertex ring's contents are discarded (vertex cache entries die) */
static uint8_t *ring_map(ID3D11Buffer *b, uint32_t *pos, uint32_t size, uint32_t n, uint32_t align, uint32_t *off_out) {
    uint32_t off = (*pos + align - 1) / align * align;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (off + n > size) { off = 0; mode = D3D11_MAP_WRITE_DISCARD; vbuf_lap++; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b, 0, mode, 0, &m))) return NULL;
    *pos = off + n; *off_out = off;
    return (uint8_t *)m.pData + off;
}
static void ring_unmap(ID3D11Buffer *b) { ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)b, 0); }
typedef struct { uint32_t addr, size, off, lap; uint64_t gen; } vc_t;
static vc_t vcache[16384];
static unsigned vc_slot(uint32_t addr, uint32_t size) { return ((addr >> 4) ^ (addr >> 17) ^ size * 2654435761u) & 16383; }
static void ensure_vbuf(uint32_t size) {
    if (size <= vbuf_size / 2) return;
    if (vbuf) { ID3D11ShaderResourceView_Release(vbuf_srv); ID3D11Buffer_Release(vbuf); }
    /* 256 MB: the vertex cache keeps unchanged vertex data in the ring until it wraps (a race draws ~35 MB a frame,
     * ~30 MB of it unchanged), so a bigger ring means fewer re-uploads; 64 MB if the driver won't give that much */
    vbuf_size = size * 2 < (256u << 20) ? (256u << 20) : (size * 2 + 0xFFFFF) & ~0xFFFFFu; vbuf_pos = 0; vbuf_lap++;
    D3D11_BUFFER_DESC d = {vbuf_size, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    if (FAILED(ID3D11Device_CreateBuffer(dev, &d, NULL, &vbuf)) && size * 2 < (64u << 20)) { vbuf_size = d.ByteWidth = 64u << 20; ID3D11Device_CreateBuffer(dev, &d, NULL, &vbuf); }
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {0};
    v.Format = DXGI_FORMAT_R32_TYPELESS; v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    v.BufferEx.FirstElement = 0; v.BufferEx.NumElements = vbuf_size / 4; v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)vbuf, &v, &vbuf_srv);
}
static void ensure_ebuf(uint32_t count) {
    if (count * 4 <= ebuf_size / 2) return;
    if (ebuf) { ID3D11ShaderResourceView_Release(ebuf_srv); ID3D11Buffer_Release(ebuf); }
    ebuf_size = count * 8 < (32u << 20) ? (32u << 20) : (count * 8 + 0xFFFFF) & ~0xFFFFFu; ebuf_pos = 0;
    D3D11_BUFFER_DESC d = {ebuf_size, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
    ID3D11Device_CreateBuffer(dev, &d, NULL, &ebuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {0};
    v.Format = DXGI_FORMAT_UNKNOWN; v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    v.Buffer.FirstElement = 0; v.Buffer.NumElements = ebuf_size / 4;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)ebuf, &v, &ebuf_srv);
}

/* ------------------------------------------------------------------ surfaces */
typedef struct {
    uint32_t addr, rsx_fmt, depth, w, h, pitch, aa_x, aa_y, swizzled;   /* w, h in samples */
    DXGI_FORMAT dxfmt;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11DepthStencilView *dsv_ro;   /* read-only view: lets a draw that writes no depth or stencil sample this buffer */
    ID3D11ShaderResourceView *srv;
    uint64_t guest_gen;   /* guest page generation already reflected in the texture */
    int gpu_dirty;        /* rendered since the last write-back */
    uint32_t dirty_h;     /* rows (samples) drawn since the last write-back: a target re-used at a smaller height must not
                           * write back, or claim, the rows beyond (the ModSpot's paint-job pass draws 288 rows of a 512-row
                           * FP16 target whose memory continues with a 2048x2048 texture) */
    int prot;             /* its guest pages are no-access until something touches them (lazy write-back) */
    int lazy_hits;        /* consecutive syncs after which the CPU touched it: read every frame, write it back eagerly */
    uint64_t last_use;
    uint32_t draw_seq;    /* bumped for every draw into it: the GPU 3D-texture copy (tex3d_srv) reruns only after new drawing */
    uint64_t drawn_at;    /* draw_clock when last rendered to: overlapping surfaces are written back oldest first (wb_sort) */
    long drawn_scan;      /* write-watch scans started by then (rsx_ww_scans): orders CPU writes against the last draw */
    uint64_t dirty_gen;   /* its memory's generation read at its first draw each frame (surf_drawn): CPU writes newer than
                           * that came after the drawing (a statue's paint loaded from the cache into memory a paint job
                           * had just been drawn in showed the other paint, 2026-11-01) */
    long drawn_flip;
    ID3D11Texture2D *fb_tex; ID3D11ShaderResourceView *fb_srv;   /* snapshot for draws that sample their own target (tex_srv) */
    uint32_t pw, ph;      /* the D3D texture's size: w, h, or larger for the main scene at a higher resolution (res_scale) */
} surf_t;
/* MNR_RESOLUTION=1080 (1440, 2160, ...; or MNR_RES_SCALE=<factor>): the main scene's targets (1280x720 and its 2560x720
 * 2x-wide copy, colour and depth) are drawn with that many times the pixels. Everything else keeps the game's sizes and
 * reads them by normalised coordinates; viewports, scissors and clears scale with the target, CPU copies in and out
 * resample (nearest), and fragment programs see screen positions in the game's pixels (wps). */
static float res_scale = -1;
static float res_scale_get(void) {
    if (res_scale < 0) {
        const char *e = getenv("MNR_RES_SCALE"), *r = getenv("MNR_RESOLUTION");
        res_scale = e ? (float)atof(e) : r ? (float)atoi(r) / 720.0f : 1.0f;
        if (!(res_scale >= 0.5f && res_scale <= 4.0f)) res_scale = 1.0f;
        if (res_scale != 1.0f) printf("[d3d] main scene at %.0fx%.0f (x%.2f)\n", 1280 * res_scale, 720 * res_scale, res_scale);
    }
    return res_scale;
}
int rsx_d3d_res_scaled(void) { return res_scale_get() != 1.0f; }   /* rsx_hlsl.c: fragment programs scale wpos */
#define NSURF 128
static surf_t surfs[NSURF];
static uint64_t use_clock, draw_clock;
/* Surfaces that overlap in memory, written back oldest-drawn first so the newest rendering ends up in guest memory, as
 * on the console. In array order a stale 128x128 avatar target inside a newer 256x256 one was written back last and
 * corrupted the uploaded avatar (2026-10-26). */
static void save_surface_bmp(surf_t *s, const char *name);
static int wb_sort(surf_t **v, int n) {
    for (int i = 1; i < n; i++) { surf_t *x = v[i]; int j = i - 1; while (j >= 0 && v[j]->drawn_at > x->drawn_at) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    return n;
}

static DXGI_FORMAT color_dxgi(uint32_t f) {
    switch (f) {
    case 9: return DXGI_FORMAT_R8_UNORM;
    case 0xA: return DXGI_FORMAT_R8G8_UNORM;
    case 0xB: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 0xC: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 0xD: return DXGI_FORMAT_R32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;   /* A8R8G8B8, X8R8G8B8, R5G6B5 (widened), X8B8G8R8... */
    }
}
static uint32_t color_bytes(uint32_t f) { return f == 3 || f == 1 || f == 2 || f == 0xA ? 2 : f == 9 ? 1 : f == 0xB ? 8 : f == 0xC ? 16 : 4; }
static uint32_t surf_bytes(const surf_t *s) {
    uint32_t bpp = s->depth ? (s->rsx_fmt == 1 ? 2 : 4) : color_bytes(s->rsx_fmt);
    return s->swizzled ? s->w * s->h * bpp : s->pitch * s->h;
}
/* Limiting write-backs to the drawn rows is switched off (whole surfaces, as before) unless MNR_D3D_DIRTYROWS=1: it
 * didn't cure the post-race ModSpot load (the terrain target really is drawn in full) and that path builds track terrain
 * (positions read back as vertices), so a mistake here leaves tracks empty. dirty_h is still tracked for the experiment. */
static uint32_t surf_dirty_rows(const surf_t *s) {
    static int on = -1; if (on < 0) on = getenv("MNR_D3D_DIRTYROWS") != NULL;
    return !on || s->swizzled || !s->dirty_h || s->dirty_h > s->h ? s->h : s->dirty_h;
}
static uint32_t surf_dirty_bytes(const surf_t *s) { return s->swizzled ? surf_bytes(s) : s->pitch * surf_dirty_rows(s); }

static uint32_t swz_off(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {
    uint32_t off = 0, bit = 0, lmin = lw < lh ? lw : lh;
    for (uint32_t i = 0; i < lmin; i++) { off |= ((x >> i) & 1) << bit++; off |= ((y >> i) & 1) << bit++; }
    if (lw > lh) for (uint32_t i = lmin; i < lw; i++) off |= ((x >> i) & 1) << bit++;
    else for (uint32_t i = lmin; i < lh; i++) off |= ((y >> i) & 1) << bit++;
    return off;
}
static uint32_t ilog2u(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }
/* Byte offsets of each column and row: the swizzle interleaves x and y bits, so it separates into
   an x part and a y part that are simply added. */
static int f16rev(void) { static int v = -1; if (v < 0) v = getenv("MNR_D3D_F16REV") != NULL; return v; }   /* float RGBA surfaces stored W,Z,Y,X */
static void pix_tables(const surf_t *s, uint32_t bpp, uint32_t *xo, uint32_t *yo) {
    if (s->swizzled) {
        uint32_t lw = ilog2u(s->w), lh = ilog2u(s->h);
        for (uint32_t x = 0; x < s->w; x++) xo[x] = swz_off(x, 0, lw, lh) * bpp;
        for (uint32_t y = 0; y < s->h; y++) yo[y] = swz_off(0, y, lw, lh) * bpp;
    } else {
        for (uint32_t x = 0; x < s->w; x++) xo[x] = x * bpp;
        for (uint32_t y = 0; y < s->h; y++) yo[y] = y * s->pitch;
    }
}

/* guest memory <-> texture contents */
uint64_t rsx_d3d_ld_count[3], rsx_d3d_ld_pixels;   /* surface uploads from guest memory: new, CPU-modified, reload_range */
static int surf_load_why;
/* rows y0..y1 of the surface from guest memory (the CPU-modified path reloads only the rows it touched) */
static void surf_unprotect(surf_t *s);
static int surf_upscale_gpu(surf_t *s, uint32_t y0, uint32_t y1, const void *buf, uint32_t bpp);
/* rows y0..y1 of the game-size image in buf into the texture (stretched when the texture is larger: smoothly on the GPU
 * for colour, nearest for depth and anything the GPU path can't take) */
static void surf_update(surf_t *s, uint32_t y0, uint32_t y1, const void *buf, uint32_t bpp) {
    if (s->pw == s->w && s->ph == s->h) {
        D3D11_BOX box = {0, y0, 0, s->w, y1, 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)s->tex, 0, y1 - y0 == s->h ? NULL : &box, buf, s->w * bpp, 0);
        return;
    }
    if (!s->depth && surf_upscale_gpu(s, y0, y1, buf, bpp)) return;
    uint32_t py0 = (uint32_t)((uint64_t)y0 * s->ph / s->h), py1 = (uint32_t)(((uint64_t)y1 * s->ph + s->h - 1) / s->h);
    if (py1 > s->ph) py1 = s->ph;
    if (py1 <= py0) return;
    uint8_t *o = malloc((size_t)s->pw * (py1 - py0) * bpp); uint32_t *xs = malloc(4 * s->pw);
    if (!o || !xs) { free(o); free(xs); return; }
    for (uint32_t x = 0; x < s->pw; x++) xs[x] = (uint32_t)((uint64_t)x * s->w / s->pw) * bpp;
    for (uint32_t py = py0; py < py1; py++) {
        uint32_t y = (uint32_t)((uint64_t)py * s->h / s->ph); if (y < y0) y = y0; if (y >= y1) y = y1 - 1;
        const uint8_t *r = (const uint8_t *)buf + (size_t)(y - y0) * s->w * bpp; uint8_t *p = o + (size_t)(py - py0) * s->pw * bpp;
        if (bpp == 4) for (uint32_t x = 0; x < s->pw; x++) memcpy(p + 4 * x, r + xs[x], 4);
        else for (uint32_t x = 0; x < s->pw; x++) memcpy(p + bpp * x, r + xs[x], bpp);
    }
    D3D11_BOX box = {0, py0, 0, s->pw, py1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)s->tex, 0, &box, o, s->pw * bpp, 0);
    free(o); free(xs);
}
static void surf_load_rows(surf_t *s, uint32_t y0, uint32_t y1) {
    surf_unprotect(s);
    uint32_t rows = y1 - y0, n = s->w * rows, f = s->rsx_fmt;
    rsx_d3d_ld_count[surf_load_why]++; rsx_d3d_ld_pixels += n;
    if (ENVFLAG("MNR_D3D_WBLOG")) { extern volatile long rsx_flips; printf("[d3d] flip %ld load(%s) 0x%08x %ux%u fmt %u%s\n", rsx_flips, surf_load_why == 0 ? "new" : surf_load_why == 1 ? "cpu" : "range", s->addr, s->w, s->h, f, s->depth ? " depth" : ""); }
    uint32_t bpp = s->depth ? (f == 1 ? 2 : 4) : color_bytes(f);
    uint32_t *xo = malloc(4 * (s->w + s->h)), *yo = xo + s->w;
    pix_tables(s, bpp, xo, yo);
    const uint8_t *g = gp(s->addr);
    if (s->depth) {   /* depth: Z24S8 (D24_UNORM_S8_UINT: 24 bit depth low, stencil high) or Z16 */
        if (f == 1) {
            uint16_t *buf = malloc(n * 2);
            for (uint32_t y = y0; y < y1; y++) { const uint8_t *r = g + yo[y]; uint16_t *o = buf + (y - y0) * s->w;
                for (uint32_t x = 0; x < s->w; x++) { uint16_t v; memcpy(&v, r + xo[x], 2); o[x] = __builtin_bswap16(v); } }
            surf_update(s, y0, y1, buf, 2);
            free(buf);
        } else {
            uint32_t *buf = malloc(n * 4);
            for (uint32_t y = y0; y < y1; y++) { const uint8_t *r = g + yo[y]; uint32_t *o = buf + (y - y0) * s->w;
                for (uint32_t x = 0; x < s->w; x++) { uint32_t z; memcpy(&z, r + xo[x], 4); z = __builtin_bswap32(z); o[x] = (z >> 8) | (z & 0xFF) << 24; } }
            surf_update(s, y0, y1, buf, 4);
            free(buf);
        }
        free(xo);
        return;
    }
    if (s->dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
        uint8_t *buf = malloc(n * 4);
        for (uint32_t y = y0; y < y1; y++) {
            const uint8_t *r = g + yo[y]; uint8_t *o = buf + 4 * (y - y0) * s->w;
            if (f == 3) for (uint32_t x = 0; x < s->w; x++, o += 4) { const uint8_t *a = r + xo[x]; uint16_t c = (uint16_t)(a[0] << 8 | a[1]);
                o[0] = (uint8_t)((c >> 11) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); o[3] = 255; }
            else if (f >= 0xE) for (uint32_t x = 0; x < s->w; x++, o += 4) { uint32_t v; memcpy(&v, r + xo[x], 4); memcpy(o, &v, 4); o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v; }
            else for (uint32_t x = 0; x < s->w; x++, o += 4) { uint32_t v; memcpy(&v, r + xo[x], 4); uint32_t rgba = v >> 8 | v << 24; memcpy(o, &rgba, 4); }
        }
        surf_update(s, y0, y1, buf, 4);
        free(buf);
    } else {   /* other formats: byte-swap element-wise into place */
        uint32_t ebytes = f == 0xB ? 2 : f == 0xC || f == 0xD ? 4 : 1;
        uint8_t *buf = malloc(n * bpp);
        for (uint32_t y = y0; y < y1; y++) for (uint32_t x = 0; x < s->w; x++) {
            const uint8_t *a = g + yo[y] + xo[x]; uint8_t *o = buf + bpp * ((y - y0) * s->w + x);
            for (uint32_t e = 0; e < bpp; e += ebytes) { uint32_t me = f16rev() && ebytes < bpp ? bpp - ebytes - e : e;
                for (uint32_t k = 0; k < ebytes; k++) o[e + k] = a[me + ebytes - 1 - k]; }
        }
        surf_update(s, y0, y1, buf, bpp);
        free(buf);
    }
    free(xo);
}
static void surf_load(surf_t *s) { surf_load_rows(s, 0, s->h); }
/* Lazy write-back. At a semaphore the CPU may read what the GPU rendered, but it almost never does for
 * most small targets (shadow maps, reflections, the minimap), and reading each one back every frame stalls
 * the GPU. Instead their guest pages are made no-access; the first access by any thread faults, and the
 * surface is written back then (by the RSX thread, which owns the D3D context) before the access resumes. */
static uint8_t *prot_page;                 /* per guest page: covered by a protected surface */
static DWORD rsx_tid;                      /* the thread that owns the D3D context */
volatile long rsx_lazy_req;                /* a page another thread faulted on, + 1 (0: none) */
static volatile uint32_t lazy_flush_addr, lazy_flush_size;   /* or a range to flush (rsx_d3d_flush_range from another thread) */
static HANDLE lazy_done; static CRITICAL_SECTION lazy_cs;
uint64_t rsx_d3d_wb_count, rsx_d3d_wb_pixels;
/* write-backs by cause (MNR_RSX_LOG): 1 page unprotect, 2 lazy CPU fault, 3 surface re-created, 4 cache eviction,
 * 5 frame sync, 6 range flush (CPU/SPU access), 7 read as a texture, 8 read as vertices, 9 screenshot */
uint64_t rsx_d3d_wb_why[10], rsx_d3d_wb_why_px[10]; double rsx_d3d_wb_s;
uint32_t rsx_d3d_last_flush[5], rsx_d3d_flush_from;   /* the last range-flushed surface (addr, w, h, fmt, who asked), for MNR_RSX_LOG */
static int wb_why;
static void surf_writeback(surf_t *s);
static void surf_pages(const surf_t *s, uint32_t *p0, uint32_t *p1) { *p0 = s->addr >> 12; *p1 = (uint32_t)(((uint64_t)s->addr + surf_bytes(s) + 4095) >> 12); }
static void surf_protect(surf_t *s) {
    if (s->prot || !prot_page) return;
    uint32_t p0, p1; surf_pages(s, &p0, &p1);
    for (uint32_t p = p0; p < p1; p++) prot_page[p] = 1;   /* before the pages fault, so the handler knows them */
    s->prot = 1;
    DWORD old; VirtualProtect(gp(p0 << 12), (SIZE_T)(p1 - p0) << 12, PAGE_NOACCESS, &old);
}
static void surf_unprotect(surf_t *s) {
    if (!s->prot) return;
    s->prot = 0;
    uint32_t p0, p1; surf_pages(s, &p0, &p1);
    {   /* protected surfaces sharing these pages: bring them up to date first */
        surf_t *v[NSURF]; int n = 0;
        for (int i = 0; i < NSURF; i++) {
            surf_t *o = &surfs[i]; uint32_t q0, q1;
            if (!o->tex || !o->prot) continue;
            surf_pages(o, &q0, &q1);
            if (q0 < p1 && p0 < q1) v[n++] = o;
        }
        for (int i = 0, k = wb_sort(v, n); i < k; i++) { wb_why = 1; surf_writeback(v[i]); }
    }
    DWORD old; VirtualProtect(gp(p0 << 12), (SIZE_T)(p1 - p0) << 12, PAGE_READWRITE, &old);
    for (uint32_t p = p0; p < p1; p++) prot_page[p] = 0;
}
static void lazy_service_page(uint32_t page) {
    surf_t *v[NSURF]; int n = 0;
    for (int i = 0; i < NSURF; i++) {
        surf_t *o = &surfs[i]; uint32_t q0, q1;
        if (!o->tex || !o->prot) continue;
        surf_pages(o, &q0, &q1);
        if (q0 <= page && page < q1) v[n++] = o;
    }
    for (int i = 0, k = wb_sort(v, n); i < k; i++) { v[i]->lazy_hits++; wb_why = 2; surf_writeback(v[i]); }
    if (prot_page[page]) { DWORD old; VirtualProtect(gp(page << 12), 4096, PAGE_READWRITE, &old); prot_page[page] = 0; }
}
void rsx_d3d_flush_range(uint32_t addr, uint32_t size);
void rsx_d3d_lazy_service(void) {   /* on the RSX thread, between commands and while it waits */
    long r = rsx_lazy_req;
    if (!r) return;
    if (r > 0) lazy_service_page((uint32_t)(r - 1));
    else rsx_d3d_flush_range(lazy_flush_addr, lazy_flush_size);
    rsx_lazy_req = 0;
    SetEvent(lazy_done);
}
static void lazy_request(long r) {   /* from another thread: have the RSX thread do it and wait */
    EnterCriticalSection(&lazy_cs);
    ResetEvent(lazy_done);
    rsx_lazy_req = r;
    if (WaitForSingleObject(lazy_done, 3000) != WAIT_OBJECT_0) {
        static int warned; if (!warned++) fprintf(stderr, "[d3d] lazy write-back request timed out\n");
        if (r > 0 && prot_page[r - 1]) { DWORD old; VirtualProtect(gp((uint32_t)(r - 1) << 12), 4096, PAGE_READWRITE, &old); prot_page[r - 1] = 0; }
        rsx_lazy_req = 0;
    }
    LeaveCriticalSection(&lazy_cs);
}
static LONG CALLBACK lazy_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)e->ExceptionRecord->ExceptionInformation[1];
    if (a < guest_mem || a >= guest_mem + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t page = (uint32_t)((a - guest_mem) >> 12);
    if (!prot_page[page]) {   /* unprotected meanwhile (the flag is cleared after the pages) */
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(a, &mi, sizeof mi) && (mi.Protect & (PAGE_READWRITE | PAGE_READONLY))) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    {   static int lg = -1; static volatile long n; if (lg < 0) lg = getenv("MNR_LAZY_LOG") != NULL;   /* debug: who reads written-back surfaces */
        if (lg && InterlockedIncrement(&n) <= 60) { extern volatile long rsx_flips;
            printf("[lazy] flip %ld page 0x%08x read by thread %lu at rip %p%s\n", rsx_flips, page << 12, GetCurrentThreadId(), (void *)(e->ContextRecord->Rip - (DWORD64)GetModuleHandleA(NULL) + 0x140000000ull), GetCurrentThreadId() == rsx_tid ? " (RSX thread)" : ""); } }
    if (GetCurrentThreadId() == rsx_tid) lazy_service_page(page);   /* the RSX thread itself (our own code reading guest memory) */
    else lazy_request((long)page + 1);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void lazy_init(void) {
    prot_page = calloc(1u << 20, 1);
    rsx_tid = GetCurrentThreadId();
    lazy_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&lazy_cs);
    AddVectoredExceptionHandler(1, lazy_fault);
}
static int lazy_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("MNR_D3D_LAZY"); on = !(e && *e == '0'); } return on; }

static void surf_writeback_body(surf_t *s);
static void surf_writeback(surf_t *s) {
    int why = wb_why; wb_why = 0;
    surf_unprotect(s);   /* we are about to write its pages (and the CPU will read them) */
    if (!s->gpu_dirty) return;
    rsx_d3d_wb_count++; rsx_d3d_wb_pixels += (uint64_t)s->w * s->h;
    rsx_d3d_wb_why[why]++; rsx_d3d_wb_why_px[why] += (uint64_t)s->w * s->h;
    if (why == 6) { rsx_d3d_last_flush[0] = s->addr; rsx_d3d_last_flush[1] = s->w; rsx_d3d_last_flush[2] = s->h; rsx_d3d_last_flush[3] = s->rsx_fmt; rsx_d3d_last_flush[4] = rsx_d3d_flush_from; }
    double t0 = now_s();
    /* Memory the CPU (or a newer write-back) rewrote after this surface's last draw is newer than the surface: keep it.
     * A stale 1280x720 target written back over a podium kart's paint, normal and shine maps, just read from the
     * composite cache, made the kart grainy (2026-10-31). Not for surfaces drawn since the last full scan began. */
    uint64_t after = s->dirty_gen, thr = after > s->guest_gen ? after : s->guest_gen;
    uint32_t bytes = surf_bytes(s), p0 = s->addr >> 12, np = (uint32_t)((((uint64_t)s->addr + bytes + 4095) >> 12) - p0);
    uint8_t *pmask = after != UINT64_MAX ? malloc(np) : NULL, *keep = NULL;
    if (pmask && rsx_range_pages_after(s->addr, bytes, thr, pmask) && (keep = malloc(bytes))) memcpy(keep, gp(s->addr), bytes);
    surf_writeback_body(s);
    if (keep) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < np; i++) {
            if (!pmask[i]) continue;
            uint32_t lo = (p0 + i) << 12, hi = lo + 4096;
            if (lo < s->addr) lo = s->addr;
            if (hi > s->addr + bytes) hi = s->addr + bytes;
            memcpy(gp(lo), keep + (lo - s->addr), hi - lo); kept++;
        }
        static int nl; if (nl++ < 20) printf("[d3d] surface 0x%08x %ux%u written back, keeping %u pages the CPU rewrote after its last draw\n", s->addr, s->w, s->h, kept);
        free(keep);
    }
    free(pmask);
    {   /* Newer surfaces over the bytes just written: their pictures were overwritten with older ones, so write them
         * again, oldest first, and memory holds the latest rendering, as on the console. The avatar's kart shot is
         * drawn at a 512x512 target 192 rows into an older one; the older one written back later put its rows over
         * it, the downscale then read memory, and the uploaded avatar came out cut (2026-10-26). */
        uint32_t a0 = s->addr, a1 = s->addr + surf_bytes(s);
        surf_t *v[NSURF]; int n = 0;
        for (int i = 0; i < NSURF; i++) {
            surf_t *o = &surfs[i];
            if (o == s || !o->tex || o->depth != s->depth || o->drawn_at <= s->drawn_at) continue;
            if (o->addr < a1 && a0 < o->addr + surf_bytes(o)) v[n++] = o;
        }
        for (int i = 0, k = wb_sort(v, n); i < k; i++) {
            surf_t *o = v[i];
            if (!o->gpu_dirty) {   /* only down to the rows the older write-back reached */
                uint32_t rows = o->swizzled || !o->pitch ? o->h : (a1 - o->addr + o->pitch - 1) / o->pitch;
                o->gpu_dirty = 1; o->dirty_h = rows < o->h ? rows : o->h;
            }
            surf_unprotect(o);
            surf_writeback_body(o);
        }
    }
    rsx_d3d_wb_s += now_s() - t0;
}
static void surf_writeback_body(surf_t *s) {
    uint32_t rows = surf_dirty_rows(s);
    /* Rows below those drawn since the last sync that the game has written meanwhile: its data is newer than ours, so
     * they are not written back. The UFG creation thumbnails (DXT, compressed into memory overlapping the bottom of a
     * 1280x720 target the menus only draw the top of) were overwritten with our pixels and showed as noise. */
    if (!s->swizzled && s->dirty_h && s->dirty_h < s->h && rows > s->dirty_h &&
        rsx_range_gen_now(s->addr + s->pitch * s->dirty_h, s->pitch * (s->h - s->dirty_h)) > s->guest_gen) rows = s->dirty_h;
    {   static int tl = -1; static int n; if (tl < 0) tl = getenv("MNR_THUMB_LOG") != NULL;   /* debug: GPU results written back to memory */
        extern volatile long rsx_flips; extern int wb_why;
        if (tl && n++ < 3000) printf("[thumb] writeback 0x%08x %ux%u fmt %u pitch %u rows %u why %d flip %ld\n", s->addr, s->w, s->h, s->rsx_fmt, s->pitch, rows, wb_why, rsx_flips); }
    s->gpu_dirty = 0; s->dirty_h = 0;
    /* surfaces sharing a 4 KB page with this one (but not its bytes) would see our write as a CPU
     * modification and reload: remember which were current so they stay current */
    uint32_t a0 = s->addr, a1 = s->addr + (s->swizzled ? surf_bytes(s) : s->pitch * rows), pg0 = a0 & ~4095u, pg1 = (a1 + 4095) & ~4095u;
    surf_t *keep[NSURF]; int nkeep = 0;
    for (int i = 0; i < NSURF; i++) {
        surf_t *o = &surfs[i];
        if (o == s || !o->tex) continue;
        uint32_t b0 = o->addr, b1 = o->addr + surf_bytes(o);
        if (b0 < pg1 && pg0 < b1 && !(b0 < a1 && a0 < b1) && rsx_range_gen_now(b0, b1 - b0) <= o->guest_gen) keep[nkeep++] = o;
    }
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(s->tex, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        uint32_t f = s->rsx_fmt, bpp = s->depth ? (f == 1 ? 2 : 4) : color_bytes(f);
        uint32_t *xo = malloc(4 * (s->w + s->h)), *yo = xo + s->w;
        pix_tables(s, bpp, xo, yo);
        uint8_t *g = gp(s->addr);
        uint32_t ebytes = f == 0xB ? 2 : f == 0xC || f == 0xD ? 4 : 1;
        uint8_t *srow = s->pw != s->w ? malloc((size_t)s->w * 16) : NULL;   /* a larger texture: its row at the game's positions */
        for (uint32_t y = 0; y < (s->swizzled ? s->h : rows); y++) {
            const uint8_t *row = (const uint8_t *)m.pData + (size_t)(s->ph == s->h ? y : (uint32_t)((uint64_t)y * s->ph / s->h)) * m.RowPitch;
            if (srow) { uint32_t pb = s->depth ? (f == 1 ? 2 : 4) : (s->dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : bpp);
                        for (uint32_t x = 0; x < s->w; x++) memcpy(srow + (size_t)pb * x, row + (size_t)pb * (uint32_t)((uint64_t)x * s->pw / s->w), pb);
                        row = srow; }
            uint8_t *r = g + yo[y];
            if (s->depth) {
                if (f == 1) for (uint32_t x = 0; x < s->w; x++) { uint16_t v = __builtin_bswap16(((const uint16_t *)row)[x]); memcpy(r + xo[x], &v, 2); }
                else for (uint32_t x = 0; x < s->w; x++) { uint32_t v = ((const uint32_t *)row)[x]; v = __builtin_bswap32((v & 0xFFFFFF) << 8 | v >> 24); memcpy(r + xo[x], &v, 4); }
                continue;
            }
            if (s->dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                for (uint32_t x = 0; x < s->w; x++) {
                    const uint8_t *p = row + 4 * x; uint8_t *a = r + xo[x];
                    if (f == 3) { uint16_t c = (uint16_t)((uint32_t)(p[0] >> 3) << 11 | (uint32_t)(p[1] >> 2) << 5 | (p[2] >> 3)); a[0] = (uint8_t)(c >> 8); a[1] = (uint8_t)c; }
                    else if (f >= 0xE) { a[0] = p[3]; a[1] = p[2]; a[2] = p[1]; a[3] = p[0]; }
                    else { a[0] = f == 4 || f == 5 ? 0xFF : p[3]; a[1] = p[0]; a[2] = p[1]; a[3] = p[2]; }
                }
            } else {
                for (uint32_t x = 0; x < s->w; x++) {
                    const uint8_t *p = row + bpp * x; uint8_t *a = r + xo[x];
                    for (uint32_t e = 0; e < bpp; e += ebytes) { uint32_t me = f16rev() && ebytes < bpp ? bpp - ebytes - e : e;
                        for (uint32_t k = 0; k < ebytes; k++) a[me + k] = p[e + ebytes - 1 - k]; }
                }
            }
        }
        free(xo); free(srow);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    s->guest_gen = rsx_range_gen_now(s->addr, surf_bytes(s));   /* our own writes are not CPU modifications */
    for (int i = 0; i < nkeep; i++) keep[i]->guest_gen = rsx_range_gen_now(keep[i]->addr, surf_bytes(keep[i]));
}
/* the bound render targets, viewport and scissor, so unchanged ones aren't re-sent to the driver on every draw */
static ID3D11RenderTargetView *bound_rtv[4]; static ID3D11DepthStencilView *bound_dsv; static int bound_valid;
static D3D11_VIEWPORT bound_vp; static D3D11_RECT bound_sc;
static int ds_valid;   /* the draw path's cached pipeline bindings (shaders, buffers, textures) are what the context has */
static void dstate_invalidate(void) { ds_valid = 0; }
static void targets_invalidate(void) { bound_valid = 0; ds_valid = 0; }
static void surf_free(surf_t *s) {
    targets_invalidate();
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->dsv_ro) ID3D11DepthStencilView_Release(s->dsv_ro);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    if (s->fb_srv) ID3D11ShaderResourceView_Release(s->fb_srv);
    if (s->fb_tex) ID3D11Texture2D_Release(s->fb_tex);
    memset(s, 0, sizeof *s);
}
static surf_t *surf_find(uint32_t addr, int depth) {   /* (looked up for every texture of every draw: remember the last hit per address) */
    static uint8_t last[512];
    unsigned h = ((addr >> 8) ^ (addr >> 17) ^ (unsigned)depth) & 511;
    surf_t *c = &surfs[last[h]];
    if (c->tex && c->addr == addr && c->depth == (uint32_t)depth) return c;
    for (int i = 0; i < NSURF; i++) if (surfs[i].tex && surfs[i].addr == addr && surfs[i].depth == (uint32_t)depth) { last[h] = (uint8_t)i; return &surfs[i]; }
    return NULL;
}
static surf_t *surf_get(uint32_t addr, int depth, uint32_t rsx_fmt, uint32_t w, uint32_t h, uint32_t pitch, uint32_t aax, uint32_t aay, int swizzled) {
    surf_t *s = surf_find(addr, depth);
    if (s && (s->rsx_fmt != rsx_fmt || s->w < w || s->h < h || s->pitch != pitch || s->swizzled != swizzled)) {
        wb_why = 3; surf_writeback(s); surf_free(s); s = NULL;
    }
    if (!s) {
        surf_t *victim = &surfs[0];
        for (int i = 0; i < NSURF; i++) { if (!surfs[i].tex) { victim = &surfs[i]; break; } if (surfs[i].last_use < victim->last_use) victim = &surfs[i]; }
        if (victim->tex) { wb_why = 4; surf_writeback(victim); surf_free(victim); }
        s = victim;
        s->addr = addr; s->depth = depth; s->rsx_fmt = rsx_fmt; s->w = w; s->h = h; s->pitch = pitch; s->aa_x = aax; s->aa_y = aay; s->swizzled = swizzled;
        {   float S = res_scale_get(); int big = S != 1.0f && !swizzled && h == 720 && (w == 1280 || w == 2560);
            s->pw = big ? (uint32_t)(w * S + 0.5f) : w; s->ph = big ? (uint32_t)(h * S + 0.5f) : h; }
        D3D11_TEXTURE2D_DESC d = {s->pw, s->ph, 1, 1, 0, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (depth) {
            d.Format = rsx_fmt == 1 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
            d.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
        } else {
            d.Format = color_dxgi(rsx_fmt);
            d.BindFlags |= D3D11_BIND_RENDER_TARGET;
        }
        s->dxfmt = d.Format;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &s->tex))) { memset(s, 0, sizeof *s); return NULL; }
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dv = {rsx_fmt == 1 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0};
            ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)s->tex, &dv, &s->dsv);
            dv.Flags = D3D11_DSV_READ_ONLY_DEPTH | (rsx_fmt == 1 ? 0 : D3D11_DSV_READ_ONLY_STENCIL);
            if (FAILED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)s->tex, &dv, &s->dsv_ro))) s->dsv_ro = NULL;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {0};
            sv.Format = rsx_fmt == 1 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->tex, &sv, &s->srv);
        } else {
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)s->tex, NULL, &s->rtv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
        }
        surf_load_why = 0; surf_load(s);
        s->guest_gen = rsx_range_gen_now(addr, surf_bytes(s));
    } else {
        uint64_t g = rsx_range_gen(addr, surf_bytes(s));
        if (g > s->guest_gen) {   /* the CPU (or a copy) wrote this memory: take its contents */
            if (s->gpu_dirty && ENVFLAG("MNR_D3D_LOG")) printf("[d3d] surface 0x%08x overwritten by the CPU while GPU-dirty\n", addr);
            if (ENVFLAG("MNR_D3D_WBLOG")) { void rsx_range_report(uint32_t, uint32_t, uint64_t); rsx_range_report(addr, surf_bytes(s), s->guest_gen); }
            uint32_t lo, hi, bytes = surf_bytes(s);
            surf_load_why = 1;
            if (!s->swizzled && rsx_range_dirty_span(addr, bytes, s->guest_gen, &lo, &hi) && hi - lo < bytes / 4) {
                uint32_t y0 = (lo > addr ? lo - addr : 0) / s->pitch, y1 = (hi - addr + s->pitch - 1) / s->pitch;
                if (y1 > s->h) y1 = s->h;
                if (y0 < y1) surf_load_rows(s, y0, y1);
            } else surf_load(s);
            s->guest_gen = g; s->gpu_dirty = 0; s->dirty_h = 0;
        }
    }
    s->last_use = ++use_clock;
    return s;
}

/* write back surfaces the CPU may read: everything but big targets (the display and the main 3D
 * buffers are only read by the GPU here) */
void rsx_d3d_sync(void) {
    if (!dev) return;
    static long long max_px = -1;
    if (max_px < 0) { const char *e = getenv("MNR_D3D_SYNC_MAX"); max_px = e ? atoll(e) : 512 * 512; }
    { static long off = -2; if (off == -2) { const char *e = getenv("MNR_D3D_SYNC_OFF_AT"); off = e ? atol(e) : -1; }   /* experiment: no writebacks from <flip> on */
      if (off >= 0 && rsx_flips >= off) return; }
    for (int i = 0; i < NSURF; i++) {
        surf_t *s = &surfs[i];
        if (s->tex && s->gpu_dirty && (long long)s->w * s->h <= max_px) {
            /* lazily, unless the CPU has read it after each of the last few syncs (then the fault and the
             * round trip to this thread cost the reader more than an eager write-back costs here); every
             * 256 flips such surfaces are tried lazily again */
            if (lazy_on() && (s->lazy_hits < 3 || (rsx_flips & 255) == 0)) { if (s->lazy_hits >= 3) s->lazy_hits = 0; surf_protect(s); continue; }
            if (ENVFLAG("MNR_D3D_WBLOG")) printf("[d3d] writeback 0x%08x %ux%u fmt %u%s\n", s->addr, s->w, s->h, s->rsx_fmt, s->depth ? " depth" : "");
            wb_why = 5; surf_writeback(s);
        }
    }
}
static int flush_range_count(uint32_t addr, uint32_t size);
void rsx_d3d_flush_range(uint32_t addr, uint32_t size) {   /* before the RSX copies from guest memory */
    if (!dev) return;
    if (rsx_tid && GetCurrentThreadId() != rsx_tid) {   /* another thread (e.g. an SPU job hook): the RSX thread owns the context */
        EnterCriticalSection(&lazy_cs);   /* (re-entrant: lazy_request takes it again) */
        lazy_flush_addr = addr; lazy_flush_size = size; lazy_request(-1);
        LeaveCriticalSection(&lazy_cs);
        return;
    }
    flush_range_count(addr, size);
}
static int flush_range_count(uint32_t addr, uint32_t size) {   /* (on the RSX thread) write back GPU surfaces over the range; how many */
    surf_t *v[NSURF]; int n = 0;
    if (rsx_d3d_flush_from == 5 && getenv("MNR_IMGENC_DUMP")) {   /* debug: every surface over an encoder's source, as the GPU has it */
        static int k;
        for (int i = 0; i < NSURF; i++) {
            surf_t *s = &surfs[i];
            if (!s->tex || s->depth || !(s->addr < addr + size && addr < s->addr + surf_bytes(s))) continue;
            char nm[96]; snprintf(nm, sizeof nm, "enc_%02d_surf_%08x_%ux%u.bmp", k, s->addr, s->w, s->h);
            printf("[encdump] %d: surface 0x%08x %ux%u fmt %u pitch %u dirty %d rows %u drawn_at %llu prot %d\n", k, s->addr, s->w, s->h, s->rsx_fmt, s->pitch, s->gpu_dirty, s->dirty_h, (unsigned long long)s->drawn_at, s->prot);
            save_surface_bmp(s, nm);
        }
        k++;
    }
    for (int i = 0; i < NSURF; i++) {
        surf_t *s = &surfs[i];
        if (s->tex && s->gpu_dirty && addr < s->addr + surf_dirty_bytes(s) && s->addr < addr + size) v[n++] = s;
    }
    for (int i = 0, k = wb_sort(v, n); i < k; i++) { wb_why = 6; surf_writeback(v[i]); }
    return n;
}

/* ------------------------------------------------------------------ textures */
typedef struct {
    uint32_t addr, fmt, w, h, pitch, linear, isf, levels;
    uint64_t gen, last_use;
    ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv;
    uint64_t hash;   /* debug (MNR_TEX_RELOAD): FNV of the guest bytes when last loaded */
    long sample_flip;   /* the frame the safety net last sampled it in */
} dtex_t;
#define NDTEX 4096
static dtex_t dtex[NDTEX];
/* 3D textures (e.g. the colour-grading LUT the game renders into a 32x1024 surface and samples as
 * 32x32x32): write back any GPU surface over that memory, decode the slices and upload a Texture3D */
static struct { uint32_t addr, w, h, d; uint64_t gen; ID3D11Texture3D *tex; ID3D11ShaderResourceView *srv; uint32_t seq; int from_gpu, uav_ok; } t3[16];
/* The colour-grading table is drawn on the GPU every frame into a 32x1024 target and sampled as a 32x32x32 3D texture.
 * Instead of writing it back to guest memory and re-uploading it (a GPU stall, ~5 ms a frame), a compute shader copies
 * it into the 3D texture: texel (x, y, z) = target (x, z * h + y), channels as the CPU decode produces them (A, R, G, B
 * bytes in R, G, B, A; A8R8G8B8 targets keep alpha in .a). MNR_D3D_LUT_CPU=1 turns it off. */
static ID3D11ComputeShader *lut_cs; static ID3D11Buffer *lut_cb;
static const char *lut_cs_src =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture3D<unorm float4> dst : register(u0);\n"
    "cbuffer c : register(b0) { uint w, h, d, opaque; }\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= w || id.y >= h || id.z >= d) return;\n"
    "    float4 p = src.Load(int3(id.x, id.z * h + id.y, 0));\n"
    "    dst[id] = float4(opaque ? 1.0 : p.a, p.r, p.g, p.b);\n"
    "}\n";
static int lut_gpu_copy(ID3D11Texture3D *dst3, const rsx_texinfo_t *ti, surf_t *o) {
    if (!lut_cs) {
        size_t len; void *code = compile(lut_cs_src, "cs_5_0", &len);
        if (!code || FAILED(ID3D11Device_CreateComputeShader(dev, code, len, NULL, &lut_cs))) { free(code); return 0; }
        free(code);
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &lut_cb);
    }
    ID3D11UnorderedAccessView *uav = NULL;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)dst3, NULL, &uav))) return 0;
    uint32_t cb[4] = {ti->w, ti->h, ti->depth, o->rsx_fmt == 4 || o->rsx_fmt == 5};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)lut_cb, 0, NULL, cb, 0, 0);
    ID3D11RenderTargetView *none_rtv[4] = {0};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, none_rtv, NULL);   /* the target can't be read while bound for output */
    ID3D11DeviceContext_CSSetShader(ctx, lut_cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &o->srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &lut_cb);
    ID3D11DeviceContext_Dispatch(ctx, (ti->w + 7) / 8, (ti->h + 7) / 8, ti->depth);
    ID3D11ShaderResourceView *no_srv = NULL; ID3D11UnorderedAccessView *no_uav = NULL;
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &no_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &no_uav, NULL);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, bound_rtv, bound_dsv);   /* back to the draw's targets */
    dstate_invalidate();   /* (binding targets unbinds any of them still bound as textures) */
    ID3D11UnorderedAccessView_Release(uav);
    return 1;
}
static ID3D11ShaderResourceView *tex3d_srv(int u, const rsx_texinfo_t *ti) {
    {   /* GPU path: the table is a plain 32-bit colour target exactly covering the texture, not bound for this draw */
        static int cpu = -1; if (cpu < 0) cpu = getenv("MNR_D3D_LUT_CPU") != NULL;
        surf_t *o = cpu ? NULL : surf_find(ti->addr, 0);
        int bound = 0;
        if (o) for (int i = 0; i < 4; i++) if (bound_rtv[i] && bound_rtv[i] == o->rtv) bound = 1;
        if (o && o->gpu_dirty && !bound && !o->swizzled && o->aa_x == 1 && o->aa_y == 1 && o->dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM &&
            (o->rsx_fmt == 8 || o->rsx_fmt == 4 || o->rsx_fmt == 5) && (ti->fmt == 0x85 || ti->fmt == 0x9E) &&
            o->w == ti->w && o->h >= ti->h * ti->depth && o->pitch == ti->w * 4) {
            int slot = -1;
            for (int i = 0; i < 16; i++) if (t3[i].tex && t3[i].addr == ti->addr && t3[i].w == ti->w && t3[i].h == ti->h && t3[i].d == ti->depth && t3[i].uav_ok) { slot = i; break; }
            if (slot >= 0 && t3[slot].seq == o->draw_seq && t3[slot].from_gpu) return t3[slot].srv;   /* not drawn since the copy */
            if (slot < 0) {
                for (int i = 0; i < 16; i++) if (t3[i].tex && t3[i].addr == ti->addr) { ID3D11ShaderResourceView_Release(t3[i].srv); ID3D11Texture3D_Release(t3[i].tex); t3[i].tex = NULL; }
                for (int i = 0; i < 16; i++) if (!t3[i].tex) { slot = i; break; }
                if (slot < 0) { slot = 0; ID3D11ShaderResourceView_Release(t3[0].srv); ID3D11Texture3D_Release(t3[0].tex); t3[0].tex = NULL; }
                D3D11_TEXTURE3D_DESC d = {ti->w, ti->h, ti->depth, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
                if (FAILED(ID3D11Device_CreateTexture3D(dev, &d, NULL, &t3[slot].tex))) { t3[slot].tex = NULL; goto cpu_path; }
                ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t3[slot].tex, NULL, &t3[slot].srv);
                t3[slot].addr = ti->addr; t3[slot].w = ti->w; t3[slot].h = ti->h; t3[slot].d = ti->depth; t3[slot].uav_ok = 1;
            }
            if (lut_gpu_copy(t3[slot].tex, ti, o)) {
                t3[slot].seq = o->draw_seq; t3[slot].from_gpu = 1; t3[slot].gen = 0;   /* a later CPU write makes the CPU path take over */
                return t3[slot].srv;
            }
        }
    }
cpu_path:;
    /* sampled by nearly every draw: the exact (system call) check only after a write-back of ours, else the
     * once-per-frame one (an uncached check per draw was a quarter of the RSX thread's time) */
    uint64_t g = flush_range_count(ti->addr, ti->bytes) ? rsx_range_gen_now(ti->addr, ti->bytes) : rsx_range_gen(ti->addr, ti->bytes);
    int slot = -1;
    for (int i = 0; i < 16; i++) if (t3[i].tex && t3[i].addr == ti->addr && t3[i].w == ti->w && t3[i].h == ti->h && t3[i].d == ti->depth) { slot = i; break; }
    if (slot >= 0 && t3[slot].gen >= g && !t3[slot].from_gpu) return t3[slot].srv;
    if (slot < 0) {
        for (int i = 0; i < 16; i++) if (!t3[i].tex) { slot = i; break; }
        if (slot < 0) { slot = 0; ID3D11ShaderResourceView_Release(t3[0].srv); ID3D11Texture3D_Release(t3[0].tex); t3[0].tex = NULL; }
        D3D11_TEXTURE3D_DESC d = {ti->w, ti->h, ti->depth, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture3D(dev, &d, NULL, &t3[slot].tex))) { t3[slot].tex = NULL; return NULL; }
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t3[slot].tex, NULL, &t3[slot].srv);
        t3[slot].addr = ti->addr; t3[slot].w = ti->w; t3[slot].h = ti->h; t3[slot].d = ti->depth;
    }
    uint8_t *buf = malloc((size_t)ti->w * ti->h * ti->depth * 4);
    rsx_tex_decode_3d(u, ti->depth, buf);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t3[slot].tex, 0, NULL, buf, ti->w * 4, ti->w * ti->h * 4);
    free(buf);
    t3[slot].gen = g; t3[slot].from_gpu = 0; t3[slot].uav_ok = 1;
    return t3[slot].srv;
}
/* cube maps (sky, environment lighting): six faces after each other, each face's mip chain padded to 128 bytes */
void rsx_tex_decode_level_off(int u, uint32_t level, uint32_t off, void *dst);
static struct { uint32_t addr, w, fmt, levels; uint64_t gen; ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; } tcube[32];
static ID3D11ShaderResourceView *cube_srv(int u, const rsx_texinfo_t *ti) {
    uint32_t face = (ti->bytes + 127) & ~127u, levels = ti->levels ? ti->levels : 1;
    rsx_d3d_flush_range(ti->addr, face * 6);
    uint64_t g = rsx_range_gen(ti->addr, face * 6);
    int slot = -1;
    for (int i = 0; i < 32; i++) if (tcube[i].tex && tcube[i].addr == ti->addr && tcube[i].w == ti->w && tcube[i].fmt == ti->fmt && tcube[i].levels == levels) { slot = i; break; }
    if (slot >= 0 && tcube[slot].gen >= g) return tcube[slot].srv;
    if (slot < 0) {
        for (int i = 0; i < 32; i++) if (!tcube[i].tex) { slot = i; break; }
        if (slot < 0) { slot = (int)(g % 32); ID3D11ShaderResourceView_Release(tcube[slot].srv); ID3D11Texture2D_Release(tcube[slot].tex); tcube[slot].tex = NULL; }
        D3D11_TEXTURE2D_DESC d = {ti->w, ti->w, levels, 6, ti->is_float ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                  D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE};
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &tcube[slot].tex))) { tcube[slot].tex = NULL; return NULL; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {0};
        sv.Format = d.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; sv.TextureCube.MipLevels = levels;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tcube[slot].tex, &sv, &tcube[slot].srv);
        tcube[slot].addr = ti->addr; tcube[slot].w = ti->w; tcube[slot].fmt = ti->fmt; tcube[slot].levels = levels;
    }
    size_t px = ti->is_float ? 16 : 4;
    void *buf = malloc((size_t)ti->w * ti->w * px);
    for (uint32_t f = 0; f < 6; f++)
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t lw = ti->w >> l ? ti->w >> l : 1;
            rsx_tex_decode_level_off(u, l, f * face, buf);
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tcube[slot].tex, f * levels + l, NULL, buf, lw * (UINT)px, 0);
        }
    free(buf);
    tcube[slot].gen = g; rsx_d3d_uploads++;
    return tcube[slot].srv;
}
/* debug: MNR_TEX_LOG=1 - what the renderer does with each 128x128 texture (thumbnails), once a second per texture */
static void texlog(const rsx_texinfo_t *ti, const char *what, uint64_t g, uint64_t have) {
    static int tl = -1; if (tl < 0) tl = getenv("MNR_TEX_LOG") != NULL;
    if (!tl || ti->w != 128 || ti->h != 128) return;
    static struct { uint32_t addr; const char *what; ULONGLONG t; } seen[512]; static volatile long n;
    unsigned k = ((ti->addr >> 8) ^ (uint32_t)(uintptr_t)what) & 511; ULONGLONG now = GetTickCount64();
    if (seen[k].addr == ti->addr && seen[k].what == what && now - seen[k].t < 1000) return;
    seen[k].addr = ti->addr; seen[k].what = what; seen[k].t = now;
    if (InterlockedIncrement(&n) > 6000) return;
    extern volatile long rsx_flips; const uint8_t *b = gp(ti->addr);
    printf("[tex128] flip %ld %s 0x%08x fmt 0x%02x linear %u gen %llu cached %llu first %02x%02x%02x%02x%02x%02x%02x%02x\n", rsx_flips, what, ti->addr, ti->fmt, ti->linear,
           (unsigned long long)g, (unsigned long long)have, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}
/* The CPU rewrote [addr, addr+bytes) of a GPU-dirty surface after the surface's last draw, so memory is newer there than
 * the surface. Targets over 512x512 are never written back at the flip (rsx_d3d_sync), and a creation's paint job read
 * from the composite cache into memory such a target had held was drawn with the target's old picture: a podium kart
 * in another creation's paint (2026-10-31). */
/* A draw (or blit) into the surface: at its first one each frame, or the first since it was in step with memory, its
 * memory's generation is read exactly, so a later CPU write can be told from one made before the drawing. */
static void surf_drawn(surf_t *s) {
    extern volatile long rsx_flips;
    if (!s->gpu_dirty || s->drawn_flip != rsx_flips) { s->dirty_gen = rsx_range_gen_now(s->addr, surf_bytes(s)); s->drawn_flip = rsx_flips; }
    s->gpu_dirty = 1; s->drawn_at = ++draw_clock; s->drawn_scan = rsx_ww_scans();
}
static int surf_cpu_newer(const surf_t *s, uint32_t addr, uint32_t bytes) {
    if (!s->gpu_dirty) return 0;
    uint32_t lo = addr > s->addr ? addr : s->addr, e = s->addr + surf_dirty_bytes(s), hi = addr + bytes < e ? addr + bytes : e;
    if (hi <= lo) return 0;
    uint64_t g = rsx_range_gen(lo, hi - lo);
    return g > s->guest_gen && g > s->dirty_gen;
}
static ID3D11ShaderResourceView *tex_srv(int u, const rsx_texinfo_t *ti, uint32_t *is_surface, uint32_t *is_depth) {
    *is_surface = *is_depth = 0;
    if (ti->dims == 3) return tex3d_srv(u, ti);   /* before the surface lookup: a 2D surface can't fill a 3D slot */
    if (ti->dims == 6) return cube_srv(u, ti);
    { static long long wa = -1; static int n; if (wa < 0) { const char *e = getenv("MNR_D3D_TEXDUMP"); wa = e ? (long long)strtoul(e, NULL, 16) : 0; }
      if (wa && ti->addr == (uint32_t)wa && n++ < 5) printf("[d3d] tex_srv 0x%08x %ux%u fmt %02x dims %u levels %u bytes %u linear %u surf %p\n", ti->addr, ti->w, ti->h, ti->fmt, ti->dims, ti->levels, ti->bytes, ti->linear, (void *)surf_find(ti->addr, 0)); }
    /* A compressed (DXT) texture can't be what a render target holds: memory once rendered to and now holding a
     * picture (the creation thumbnails) showed the stale target's noise until the slot was reloaded. */
    int dxt = ti->fmt == 0x86 || ti->fmt == 0x87 || ti->fmt == 0x88;
    if (dxt) {   /* debug: MNR_THUMB_LOG=1 - a DXT texture whose memory holds raw A8R8G8B8 pixels (alpha ff every 4th byte) */
        static int tl = -1; if (tl < 0) tl = getenv("MNR_THUMB_LOG") != NULL;
        const uint8_t *b = gp(ti->addr);
        if (tl && b[0] == 0xff && b[4] == 0xff && b[8] == 0xff && b[12] == 0xff) {
            static uint32_t said[256]; unsigned k = (ti->addr >> 12) & 255;
            if (said[k] != ti->addr) { said[k] = ti->addr; extern volatile long rsx_flips;
                printf("[thumb] DXT texture 0x%08x %ux%u fmt 0x%02x holds raw pixels (flip %ld)\n", ti->addr, ti->w, ti->h, ti->fmt, rsx_flips); }
        }
    }
    surf_t *s = dxt ? NULL : surf_find(ti->addr, 0);
    if (!s && !dxt) { s = surf_find(ti->addr, 1); if (s) *is_depth = 1; }
    int cpu_newer = s && surf_cpu_newer(s, ti->addr, ti->bytes);
    if (cpu_newer) { wb_why = 7; surf_writeback(s); }   /* (keeps what the CPU rewrote) */
    if (s && !cpu_newer && (s->gpu_dirty || rsx_range_gen(s->addr, surf_bytes(s)) <= s->guest_gen)) {
        static int nlog, tl = -1; if (tl < 0) tl = getenv("MNR_D3D_TEXLOG") != NULL;
        if (tl && nlog < 200 && (s->w != ti->w * s->aa_x || s->h != ti->h * s->aa_y)) { nlog++;
            printf("[d3d] texture 0x%08x %ux%u fmt %02x read from surface %ux%u fmt %u%s\n", ti->addr, ti->w, ti->h, ti->fmt, s->w, s->h, s->rsx_fmt, s->depth ? " depth" : ""); }
        *is_surface = 1; s->last_use = ++use_clock; texlog(ti, "surface", s->guest_gen, s->gpu_dirty);
        /* A draw that samples the target it renders to (the terrain's tunnel/hole pass reads and writes its height
         * map): the RSX reads the memory as it was before the draw, but Direct3D can't bind one texture as both,
         * and silently gives the shader zeros. Read a copy taken now instead. */
        int bound = 0;
        for (int i = 0; i < 4; i++) if (s->rtv && bound_rtv[i] == s->rtv) bound = 1;
        int dbound = s->depth && s->dsv && bound_dsv == s->dsv;   /* (a depth buffer sampled while bound: left as is; copying it made shadows noisy) */
        if (bound || dbound) { static int nl; extern volatile long rsx_flips;
            if (ENVSTR("MNR_D3D_FEEDBACK_LOG") && rsx_flips >= atol(ENVSTR("MNR_D3D_FEEDBACK_LOG")) && nl++ < 20000) printf("[d3d] flip %ld: draw samples its bound %s target 0x%08x %ux%u fmt %u\n", rsx_flips, s->depth ? "depth" : "colour", s->addr, s->w, s->h, s->rsx_fmt); }
        if (dbound) {   /* test: MNR_D3D_DEPTH_FB=<hex addr>,...: copy these depth buffers too when read while bound */
            const char *e = ENVSTR("MNR_D3D_DEPTH_FB"); char want[16]; if (e) snprintf(want, sizeof want, "%08x", s->addr);
            if (e && strstr(e, want)) bound = 1;
        }
        if (bound) {
            static int off = -1; if (off < 0) off = getenv("MNR_D3D_NO_FEEDBACK_COPY") != NULL;
            if (!off) {
                if (!s->fb_tex) {
                    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(s->tex, &d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
                    D3D11_SHADER_RESOURCE_VIEW_DESC vd; ID3D11ShaderResourceView_GetDesc(s->srv, &vd);
                    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &s->fb_tex))) s->fb_tex = NULL;
                    else if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->fb_tex, &vd, &s->fb_srv))) { ID3D11Texture2D_Release(s->fb_tex); s->fb_tex = NULL; }
                }
                if (s->fb_tex) {
                    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s->fb_tex, (ID3D11Resource *)s->tex);
                    static int nl; if (nl++ < 5) printf("[d3d] draw samples its own target 0x%08x %ux%u fmt %u: reading a copy\n", s->addr, s->w, s->h, s->rsx_fmt);
                    return s->fb_srv;
                }
            }
        }
        return s->srv;
    }
    /* "write color buffers": a render target read back as a texture in another layout (format, size, offset)
     * must be in guest memory first. The terrain's 2048x2048 material mask is drawn on the GPU at load time. */
    int wrote = cpu_newer;
    /* (not for a DXT texture: rendering can't produce one, and writing an overlapping target back put its pixels over
     * the creation thumbnails, compressed into memory under the bottom rows of a 1280x720 target) */
    surf_t *wv[NSURF]; int wn = 0;
    for (int i = 0; i < NSURF && !dxt; i++) {
        surf_t *o = &surfs[i];
        if (o->tex && o->gpu_dirty && o->addr < ti->addr + ti->bytes && ti->addr < o->addr + surf_dirty_bytes(o)) {
            wv[wn++] = o;
        }
    }
    for (int i = 0, k = wb_sort(wv, wn); i < k; i++) {
        surf_t *o = wv[i];
        {
            { static int nl;   /* MNR_D3D_WBLOG: occurrences 200-239 (rare in normal frames, thousands while the ModSpot builds paint jobs) */
              if (ENVFLAG("MNR_D3D_WBLOG") && ++nl >= 200 && nl < 240) {
                printf("[d3d] texture 0x%08x %ux%u fmt %02x linear %u pitch %u bytes %u levels %u <- surface 0x%08x %ux%u fmt %u pitch %u swz %u aa %ux%u drawn rows %u%s%s\n",
                       ti->addr, ti->w, ti->h, ti->fmt, ti->linear, ti->pitch, ti->bytes, ti->levels, o->addr, o->w, o->h, o->rsx_fmt, o->pitch, o->swizzled,
                       o->aa_x, o->aa_y, o->dirty_h, o->depth ? " depth" : "", s == o ? " (same address, stale guest copy)" : ""); } }
            wb_why = 7; surf_writeback(o); wrote = 1;
        }
    }
    uint64_t g = wrote ? rsx_range_gen_now(ti->addr, ti->bytes) : rsx_range_gen(ti->addr, ti->bytes);
    dtex_t *hit = NULL, *victim = &dtex[0];
    static uint16_t dlast[4096];   /* last entry per (address, format, size): most lookups hit it */
    unsigned dh = ((ti->addr >> 7) ^ (ti->addr >> 19) ^ ti->fmt * 31u ^ ti->w * 7u ^ ti->h) & 4095;
#define DTEX_MATCH(t) ((t)->tex && (t)->addr == ti->addr && (t)->fmt == ti->fmt && (t)->w == ti->w && (t)->h == ti->h && (t)->pitch == ti->pitch && (t)->linear == ti->linear && (t)->levels == ti->levels)
    if (DTEX_MATCH(&dtex[dlast[dh]])) hit = &dtex[dlast[dh]];
    else for (int i = 0; i < NDTEX; i++) {
        dtex_t *t = &dtex[i];
        if (DTEX_MATCH(t)) { hit = t; dlast[dh] = (uint16_t)i; break; }
        if (!t->tex) { if (victim->tex) victim = t; }
        else if (victim->tex && t->last_use < victim->last_use) victim = t;
    }
#undef DTEX_MATCH
    /* Safety net: a texture can be uploaded before the game finished writing it, and that later write was sometimes
     * not seen (garbage thumbnails, noise on loading screens). Small textures (up to 512x512) are sampled at every use
     * (512 bytes) and reloaded when the sample changed. MNR_TEX_CHECK=1: every texture (4096 samples), and reports. */
    static int check = -1; if (check < 0) check = getenv("MNR_TEX_CHECK") != NULL;
    extern volatile long rsx_flips;
    /* (once a frame: the 512 scattered reads at every use were a third of the RSX thread's time in races) */
    /* (each texture every 4th frame, staggered by address: 512 scattered reads are 512 cache misses, and a frame
     * samples hundreds of textures; a write seen by write-watch still reloads at once, a missed one within 4 frames) */
    int sampled = (check || (ti->w <= 512 && ti->h <= 512)) &&
                  (!hit || g > hit->gen || (hit->sample_flip != rsx_flips && (check || ((rsx_flips + (ti->addr >> 12)) & 3) == 0)));
    /* (not over a page a render target protects: the read faulted, wrote the target back and, after three, made it
     * written back at every frame sync - a 256x256 target and its depth buffer cost the RSX thread a quarter of a race) */
    if (sampled && prot_page && ti->bytes)
        for (uint32_t p = ti->addr >> 12, pe = (ti->addr + ti->bytes - 1) >> 12; p <= pe; p++) if (prot_page[p]) { sampled = 0; break; }
    uint64_t sample = 0;
    if (sampled) {
        uint64_t h = 1469598103934665603ull; const uint8_t *b = gp(ti->addr);
        /* (512 single bytes: 128 eight-byte reads from the same span missed partial updates the 512 caught, 65 in a race) */
        uint32_t want = check ? 4096 : 512, step = ti->bytes > want ? ti->bytes / want : 1;
        /* (the same bytes in four independent chains: one chain of 512 dependent multiplies was ~8% of the RSX thread) */
        uint64_t h1 = h ^ 1, h2 = h ^ 2, h3 = h ^ 3; uint32_t i = 0;
        for (; i + 3 * step < ti->bytes; i += 4 * step) {
            h = (h ^ b[i]) * 1099511628211ull; h1 = (h1 ^ b[i + step]) * 1099511628211ull;
            h2 = (h2 ^ b[i + 2 * step]) * 1099511628211ull; h3 = (h3 ^ b[i + 3 * step]) * 1099511628211ull;
        }
        for (; i < ti->bytes; i += step) h = (h ^ b[i]) * 1099511628211ull;
        h ^= h1 * 3 ^ h2 * 5 ^ h3 * 7;
        sample = h | 1;   /* never 0: 0 = not sampled */
    }
    {
        if (sampled && hit) {
            uint64_t h = sample;
            if (hit->hash && h != hit->hash && g <= hit->gen) {
                if (check) {
                static volatile long n; if (InterlockedIncrement(&n) <= 40) {
                    extern int gcm_io_regions(uint32_t *ea, uint32_t *size, int max);
                    uint32_t e[64], sz[64]; int nr = gcm_io_regions(e, sz, 64), in = ti->addr >= 0xC0000000u && ti->addr < 0xD0000000u;
                    for (int i = 0; i < nr && !in; i++) in = ti->addr >= e[i] && ti->addr < e[i] + sz[i];
                    printf("[texstale] 0x%08x %ux%u fmt 0x%02x linear %u: bytes changed, no write seen (gen %llu, page gen %llu, in scanned memory: %s, %d io regions)\n",
                           ti->addr, ti->w, ti->h, ti->fmt, ti->linear, (unsigned long long)hit->gen, (unsigned long long)rsx_range_gen_now(ti->addr, ti->bytes), in ? "yes" : "NO", nr);
                }
                }
                g = hit->gen + 1;   /* reload it */
            }
            hit->hash = h; hit->sample_flip = rsx_flips;
        }
    }
    if (hit && g <= hit->gen) { hit->last_use = ++use_clock; texlog(ti, "cached", g, hit->gen); return hit->srv; }
    texlog(ti, hit ? "reload" : "new", g, hit ? hit->gen : 0);
    if (!hit) {
        hit = victim; dlast[dh] = (uint16_t)(victim - dtex);
        if (hit->tex) { ID3D11ShaderResourceView_Release(hit->srv); ID3D11Texture2D_Release(hit->tex); }
        memset(hit, 0, sizeof *hit);
        hit->addr = ti->addr; hit->fmt = ti->fmt; hit->w = ti->w; hit->h = ti->h; hit->pitch = ti->pitch; hit->linear = ti->linear; hit->isf = ti->is_float;
        hit->levels = ti->levels ? ti->levels : 1;
        D3D11_TEXTURE2D_DESC d = {ti->w, ti->h, hit->levels, 1, ti->is_float ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                  D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &hit->tex))) { memset(hit, 0, sizeof *hit); return NULL; }
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)hit->tex, NULL, &hit->srv);
    }
    { static long long dumpa = -1; if (dumpa < 0) { const char *e = getenv("MNR_D3D_TEXDUMP"); dumpa = e ? (long long)strtoul(e, NULL, 16) : 0; }   /* debug: raw bytes of one texture */
      if (dumpa && ti->addr == (uint32_t)dumpa) { char nm[64]; snprintf(nm, sizeof nm, "tex_%08x_%ux%u_f%02x.raw", ti->addr, ti->w, ti->h, ti->fmt); FILE *f = fopen(nm, "wb"); if (f) { fwrite(gp(ti->addr), 1, ti->bytes, f); fclose(f); } } }
    double t0 = now_s(); rsx_d3d_uploads++;
    size_t px = (size_t)ti->w * ti->h * (ti->is_float ? 16 : 4);
    void *buf = malloc(px);
    for (uint32_t l = 0; l < hit->levels; l++) {
        uint32_t lw = ti->w >> l ? ti->w >> l : 1;
        rsx_tex_decode_level(u, l, buf);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)hit->tex, l, NULL, buf, lw * (ti->is_float ? 16 : 4), 0);
    }
    free(buf);
    hit->gen = g; hit->last_use = ++use_clock; hit->hash = sample; hit->sample_flip = rsx_flips;   /* the sample taken with this upload (0: not sampled) */
    {   /* debug: MNR_TEX_LOG=1 logs the uploads of thumbnail-sized textures (64..256 texels a side) */
        static int tl = -1; if (tl < 0) tl = getenv("MNR_TEX_LOG") != NULL;
        static volatile long n;
        if (tl && ti->w >= 64 && ti->w <= 256 && ti->h >= 64 && ti->h <= 256 && InterlockedIncrement(&n) <= 600) {
            extern volatile long rsx_flips; const uint8_t *b = gp(ti->addr);
            printf("[texup] flip %ld 0x%08x %ux%u fmt 0x%02x linear %u pitch %u levels %u gen %llu sample %016llx first %02x%02x%02x%02x%02x%02x%02x%02x\n",
                   rsx_flips, ti->addr, ti->w, ti->h, ti->fmt, ti->linear, ti->pitch, ti->levels, (unsigned long long)g,
                   (unsigned long long)sample, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        }
    }
    rsx_d3d_upload_s += now_s() - t0;
    return hit->srv;
}
static D3D11_TEXTURE_ADDRESS_MODE wrap_mode(uint32_t m) {
    switch (m) { case 1: return D3D11_TEXTURE_ADDRESS_WRAP; case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER; case 6: case 7: case 8: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE; default: return D3D11_TEXTURE_ADDRESS_CLAMP; }
}
static ID3D11SamplerState *sampler(const rsx_texinfo_t *ti) {
    D3D11_SAMPLER_DESC d = {0};
    /* RSX min filter: 1 nearest, 2 linear, 3 nearest/mip nearest, 4 linear/mip nearest, 5 nearest/mip linear, 6 linear/mip linear */
    uint32_t mf = ti->min_filter;
    int min_lin = mf == 2 || mf == 4 || mf == 6 || mf == 7, mip_lin = mf == 5 || mf == 6 || mf == 7, mag_lin = ti->mag_linear;
    d.Filter = (D3D11_FILTER)((min_lin ? 0x10 : 0) | (mag_lin ? 0x04 : 0) | (mip_lin ? 0x01 : 0));
    if (mf <= 2) d.MaxLOD = 0;   /* no mip filtering: level 0 only */
    d.AddressU = wrap_mode(ti->wrap_s); d.AddressV = wrap_mode(ti->wrap_t); d.AddressW = wrap_mode(ti->wrap_r);
    d.MaxAnisotropy = 1; d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (mf > 2) d.MaxLOD = D3D11_FLOAT32_MAX;
    {   /* MNR_ANISO=2..16: anisotropic filtering for linearly filtered, mipmapped textures (sharper at grazing angles) */
        static int an = -1; if (an < 0) { const char *e = getenv("MNR_ANISO"); an = e ? atoi(e) : 0; if (an > 16) an = 16; if (an > 1) printf("[d3d] anisotropic filtering x%d\n", an); }
        if (an > 1 && min_lin && mf > 2) { d.Filter = D3D11_FILTER_ANISOTROPIC; d.MaxAnisotropy = (UINT)an; }
    }
    { static int l0 = -1; if (l0 < 0) l0 = getenv("MNR_D3D_LOD0") != NULL; if (l0) d.MaxLOD = 0; }   /* debug: base level only */
    uint64_t k = hash64(&d, sizeof d, 17);   /* (state keys: in memory only, hashed 8 bytes at a time) */
    ID3D11SamplerState *s = state_cache_get(k);
    if (!s) { ID3D11Device_CreateSamplerState(dev, &d, &s); state_cache_put(k, s); }
    return s;
}

static ID3D11SamplerState *shadow_sampler(const rsx_texinfo_t *ti, uint32_t zfunc) {   /* comparison sampler for depth textures */
    D3D11_SAMPLER_DESC d = {0};
    d.Filter = ti->mag_linear ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    d.AddressU = wrap_mode(ti->wrap_s); d.AddressV = wrap_mode(ti->wrap_t); d.AddressW = wrap_mode(ti->wrap_r);
    /* the RSX compares the stored depth against the reference (texel OP ref); D3D compares ref OP texel:
     * swap the operand order (LESS <-> GREATER, LEQUAL <-> GEQUAL). Unswapped, every surface the shadow
     * map covered was in shadow except under the objects casting it. MNR_D3D_SHADOW_NOSWAP=1: old behaviour */
    static int noswap = -1; if (noswap < 0) noswap = getenv("MNR_D3D_SHADOW_NOSWAP") != NULL;
    if (!noswap) { static const uint8_t sw[8] = {0, 4, 2, 6, 1, 5, 3, 7}; zfunc = sw[zfunc & 7]; }
    d.MaxAnisotropy = 1; d.ComparisonFunc = (D3D11_COMPARISON_FUNC)((zfunc & 7) + 1);   /* RSX NEVER..ALWAYS = 0..7, D3D 1..8 */
    d.MaxLOD = 0;
    uint64_t k = hash64(&d, sizeof d, 19);
    ID3D11SamplerState *s = state_cache_get(k);
    if (!s) { ID3D11Device_CreateSamplerState(dev, &d, &s); state_cache_put(k, s); }
    return s;
}

/* ------------------------------------------------------------------ state */
/* created state objects by descriptor hash (kept for the whole run) */
static struct { uint64_t key; void *obj; } state_cache[4096];
static void *state_cache_get(uint64_t k) {
    for (unsigned i = (unsigned)(k & 4095), n = 0; n < 4096; n++, i = (i + 1) & 4095) {
        if (state_cache[i].key == k) return state_cache[i].obj;
        if (!state_cache[i].key) return NULL;
    }
    return NULL;
}
static void state_cache_put(uint64_t k, void *o) {
    for (unsigned i = (unsigned)(k & 4095), n = 0; n < 4096; n++, i = (i + 1) & 4095)
        if (!state_cache[i].key) { state_cache[i].key = k; state_cache[i].obj = o; return; }
}
static D3D11_BLEND blend_factor(uint32_t f, int alpha) {
    switch (f) {
    case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA; case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA; case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}
static D3D11_BLEND_OP blend_op(uint32_t e) {
    switch (e) { case 0x800A: return D3D11_BLEND_OP_SUBTRACT; case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN; case 0x8008: return D3D11_BLEND_OP_MAX; default: return D3D11_BLEND_OP_ADD; }
}
static D3D11_STENCIL_OP stencil_op(uint32_t o) {
    switch (o) { case 0: return D3D11_STENCIL_OP_ZERO; case 0x1E01: return D3D11_STENCIL_OP_REPLACE; case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT; case 0x150A: return D3D11_STENCIL_OP_INVERT; case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR; default: return D3D11_STENCIL_OP_KEEP; }
}
static D3D11_COMPARISON_FUNC cmp(uint32_t f) { return (D3D11_COMPARISON_FUNC)((f & 0xF) + 1); }

static uint64_t last_blend, last_depth, last_raster;   /* reset when presentation changes state */
static void apply_states(int nrt, uint32_t fmt0) {
    D3D11_BLEND_DESC bd = {0};
    bd.IndependentBlendEnable = FALSE;
    uint32_t cm = REG(0x0324);
    D3D11_RENDER_TARGET_BLEND_DESC *r = &bd.RenderTarget[0];
    r->BlendEnable = REG(0x0310) & 1;
    uint32_t sf = REG(0x0314), df = REG(0x0318), eq = REG(0x0320);
    r->SrcBlend = blend_factor(sf & 0xFFFF, 0); r->DestBlend = blend_factor(df & 0xFFFF, 0); r->BlendOp = blend_op(eq & 0xFFFF);
    r->SrcBlendAlpha = blend_factor(sf >> 16, 1); r->DestBlendAlpha = blend_factor(df >> 16, 1); r->BlendOpAlpha = blend_op(eq >> 16);
    r->RenderTargetWriteMask = (UINT8)((cm >> 16 & 1) | (cm >> 8 & 1) << 1 | (cm & 1) << 2 | (cm >> 24 & 1) << 3);
    {   /* test: float RGBA targets (W16Z16Y16X16, W32Z32Y32X32) with the R and B mask bits exchanged */
        static int sw = -1; if (sw < 0) sw = getenv("MNR_D3D_F16MASKSWAP") != NULL;
        if (sw && (fmt0 == 0xB || fmt0 == 0xC)) { UINT8 m = r->RenderTargetWriteMask; r->RenderTargetWriteMask = (UINT8)((m & 0xA) | (m & 1) << 2 | (m >> 2 & 1)); }
    }
    uint32_t bc = REG(0x031C);
    uint64_t kb = hash64(&bd, sizeof bd, fnv(&bc, 4, 7));
    if (kb != last_blend) {
        ID3D11BlendState *bs = state_cache_get(kb);
        if (!bs) { ID3D11Device_CreateBlendState(dev, &bd, &bs); state_cache_put(kb, bs); }
        float bf[4] = {((bc >> 16) & 255) / 255.f, ((bc >> 8) & 255) / 255.f, (bc & 255) / 255.f, (bc >> 24) / 255.f};
        ID3D11DeviceContext_OMSetBlendState(ctx, bs, bf, 0xFFFFFFFF);
        last_blend = kb;
    }
    (void)nrt;

    D3D11_DEPTH_STENCIL_DESC dd = {0};
    dd.DepthEnable = REG(0x0A74) & 1;
    { static int nd = -1; if (nd < 0) nd = getenv("MNR_D3D_NODEPTH") != NULL; if (nd) dd.DepthEnable = 0; }   /* debug */
    dd.DepthWriteMask = (REG(0x0A70) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = cmp(REG(0x0A6C));
    if (!dd.DepthEnable && dd.DepthWriteMask) { dd.DepthEnable = TRUE; dd.DepthFunc = D3D11_COMPARISON_ALWAYS; }   /* RSX writes depth even when the test is off */
    /* stencil: front at 0x328 (enable, write mask, func, ref, func mask, fail/zfail/zpass ops), 0x348 the
     * two-sided enable with the back face's set after it (D3D11 has one read/write mask for both) */
    dd.StencilEnable = REG(0x0328) & 1;
    dd.StencilReadMask = (UINT8)REG(0x0338); dd.StencilWriteMask = (UINT8)REG(0x032C);
    dd.FrontFace.StencilFunc = cmp(REG(0x0330)); dd.FrontFace.StencilFailOp = stencil_op(REG(0x033C));
    dd.FrontFace.StencilDepthFailOp = stencil_op(REG(0x0340)); dd.FrontFace.StencilPassOp = stencil_op(REG(0x0344));
    if (REG(0x0348) & 1) {
        dd.BackFace.StencilFunc = cmp(REG(0x0350)); dd.BackFace.StencilFailOp = stencil_op(REG(0x035C));
        dd.BackFace.StencilDepthFailOp = stencil_op(REG(0x0360)); dd.BackFace.StencilPassOp = stencil_op(REG(0x0364));
    } else dd.BackFace = dd.FrontFace;
    uint32_t sref = REG(0x0334) & 0xFF;
    uint64_t kd = hash64(&dd, sizeof dd, fnv(&sref, 4, 11));
    if (kd != last_depth) {
        ID3D11DepthStencilState *ds = state_cache_get(kd);
        if (!ds) { ID3D11Device_CreateDepthStencilState(dev, &dd, &ds); state_cache_put(kd, ds); }
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds, sref);
        last_depth = kd;
    }

    D3D11_RASTERIZER_DESC rd = {0};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (REG(0x183C) & 1) {
        uint32_t cf = REG(0x1830);
        rd.CullMode = cf == 0x0404 ? D3D11_CULL_FRONT : cf == 0x0405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    }
    int origin_bottom = (REG(0x1D88) >> 12) & 1, ccw = REG(0x1834) == 0x0901;
    rd.FrontCounterClockwise = ccw != origin_bottom;   /* verified on the race scene: the opposite culls every visible face */
    { static int flip = -1; if (flip < 0) { const char *e = getenv("MNR_D3D_FLIPCULL"); flip = e && *e == '1'; } if (flip) rd.FrontCounterClockwise = !rd.FrontCounterClockwise; }
    { static int nc = -1; if (nc < 0) nc = getenv("MNR_D3D_NOCULL") != NULL; if (nc) rd.CullMode = D3D11_CULL_NONE; }   /* debug */
    rd.DepthClipEnable = FALSE;
    if (REG(0x0A68) & 1) {   /* POLYGON_OFFSET_FILL: factor (0x0A78) and bias (0x0A7C) in depth-buffer units, as in GL */
        float fac, bias; uint32_t a = REG(0x0A78), b = REG(0x0A7C); memcpy(&fac, &a, 4); memcpy(&bias, &b, 4);
        static int n; if (n < 12 && ENVFLAG("MNR_D3D_POFSLOG")) { n++; printf("[d3d] polygon offset factor %g bias %g (0x%08x 0x%08x)\n", fac, bias, a, b); }
        rd.SlopeScaledDepthBias = fac; rd.DepthBias = (INT)bias;
    }
    rd.ScissorEnable = TRUE;
    uint64_t kr = hash64(&rd, sizeof rd, 13);
    if (kr != last_raster) {
        ID3D11RasterizerState *rs = state_cache_get(kr);
        if (!rs) { ID3D11Device_CreateRasterizerState(dev, &rd, &rs); state_cache_put(kr, rs); }
        ID3D11DeviceContext_RSSetState(ctx, rs);
        last_raster = kr;
    }
}

/* ------------------------------------------------------------------ render target binding */
typedef struct { surf_t *color[4]; int ncolor; surf_t *depth; uint32_t w, h, aax, aay; int clip_x0, clip_y0, clip_x1, clip_y1; float sx, sy; } rt_t;   /* sx, sy: texture / game pixels */
/* MNR_D3D_TGTLOG (debug): log the draws into the colour target at <hex address>, or into any <w>x<h> one */
static int tgtlog_match(const rt_t *rt) {
    static int mode = -1; static uint32_t a, w, h;
    if (mode < 0) {
        const char *e = getenv("MNR_D3D_TGTLOG"); mode = 0;
        if (e && strchr(e, 'x') && sscanf(e, "%ux%u", &w, &h) == 2) mode = 2;
        else if (e) { a = (uint32_t)strtoul(e, NULL, 16); mode = 1; }
    }
    if (!mode || !rt->ncolor) return 0;
    return mode == 1 ? rt->color[0]->addr == a : rt->color[0]->w == w && rt->color[0]->h == h;
}
/* need_depth: 0 none, 1 read and write, 2 read only (depth/stencil tested, nothing written): the read-only view, so the
 * draw can also sample the buffer (the water reads the scene depth for its thickness; with the buffer bound for
 * writing Direct3D hands the shader zeros, which turned the sea white). A run of such draws (the transparent pass)
 * keeps one binding. */
static int bind_targets(rt_t *rt, int need_depth) {
    memset(rt, 0, sizeof *rt);
    uint32_t fmt = REG(0x0208), cf = fmt & 31, zf = (fmt >> 5) & 7, aa = (fmt >> 12) & 15, sw = ((fmt >> 8) & 15) == 2;
    rt->aax = aa >= 3 ? 2 : 1; rt->aay = aa >= 4 ? 2 : 1;
    uint32_t ch = REG(0x0200), cv = REG(0x0204);
    uint32_t w = (ch & 0xFFFF) + (ch >> 16), h = (cv & 0xFFFF) + (cv >> 16);
    if (sw) { w = 1u << ((fmt >> 16) & 0xFF); h = 1u << ((fmt >> 24) & 0xFF); }
    if (!w || !h || w > 4096 || h > 4096) return 0;
    rt->w = w * rt->aax; rt->h = h * rt->aay;
    static const uint32_t offs[4] = {0x0210, 0x0218, 0x0288, 0x028C}, pitches[4] = {0x020C, 0x021C, 0x0280, 0x0284}, dmas[4] = {0x0194, 0x018C, 0x01B4, 0x01B8};
    uint32_t target = REG(0x0220), idx[4], n = 0;
    switch (target) {
    case 0x01: idx[n++] = 0; break;
    case 0x02: idx[n++] = 1; break;
    case 0x13: idx[n++] = 0; idx[n++] = 1; break;
    case 0x17: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; break;
    case 0x1F: idx[n++] = 0; idx[n++] = 1; idx[n++] = 2; idx[n++] = 3; break;
    }
    ID3D11RenderTargetView *rtv[4] = {0};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = idx[i], off = REG(offs[k]);
        uint32_t addr = REG(dmas[k]) == 0xFEED0001 ? gcm_io_to_ea_pub(off) : RSX_LOCAL_BASE + off;
        surf_t *s = surf_get(addr, 0, cf, rt->w, rt->h, REG(pitches[k]), rt->aax, rt->aay, (int)sw);
        {   static long long ta = -2; static int tn; if (ta == -2) { const char *e = getenv("MNR_D3D_TRACE_ADDR"); ta = e ? (long long)strtoull(e, NULL, 16) : -1; }
            if (ta >= 0 && addr <= (uint32_t)ta && (uint32_t)ta < addr + REG(pitches[k]) * rt->h && tn++ < 400) {
                printf("[trace] flip %ld target 0x%08x (mrt %u/%u target 0x%x) %ux%u fmt %u pitch %u\n", rsx_flips, addr, i, n, target, rt->w, rt->h, cf, REG(pitches[k])); } }
        if (!s) continue;
        rt->color[rt->ncolor++] = s; rtv[i] = s->rtv; surf_drawn(s);
        {   /* MNR_D3D_TARGETS_AT=<flip>[,<flip>...]: the sequence of colour targets drawn into during those frames (debug) */
            static const char *e = (const char *)1; if (e == (const char *)1) e = getenv("MNR_D3D_TARGETS_AT");
            static uint32_t last; static long lastf = -1;
            if (e) { char b[16]; snprintf(b, sizeof b, "%ld", rsx_flips);
                const char *p = strstr(e, b);
                if (p && (p == e || p[-1] == ',') && (p[strlen(b)] == 0 || p[strlen(b)] == ',') && (s->addr != last || lastf != rsx_flips)) {
                    printf("[targets] flip %ld target 0x%08x %ux%u fmt %u FP 0x%x\n", rsx_flips, s->addr, s->w, s->h, s->rsx_fmt, REG(0x08E4));
                    last = s->addr; lastf = rsx_flips; } }
        }
    }
    ID3D11DepthStencilView *dsv = NULL;
    if (zf && need_depth) {   /* a depth buffer only when depth or stencil is used: the zeta registers are often stale otherwise */
        uint32_t off = REG(0x0214), addr = REG(0x0198) == 0xFEED0001 ? gcm_io_to_ea_pub(off) : RSX_LOCAL_BASE + off;
        surf_t *s = surf_get(addr, 1, zf, rt->w, rt->h, REG(0x022C), rt->aax, rt->aay, (int)sw);
        if (s && rt->ncolor && (s->pw * rt->color[0]->w != rt->color[0]->pw * s->w || s->ph * rt->color[0]->h != rt->color[0]->ph * s->h)) s = NULL;   /* (another scale: not usable together) */
        if (s) { rt->depth = s; dsv = need_depth == 2 && s->dsv_ro ? s->dsv_ro : s->dsv; surf_drawn(s); }
    }
    {   surf_t *f0 = rt->ncolor ? rt->color[0] : rt->depth;
        rt->sx = f0 ? (float)f0->pw / f0->w : 1.0f; rt->sy = f0 ? (float)f0->ph / f0->h : 1.0f; }
    D3D11_VIEWPORT vp = {0, 0, rt->w * rt->sx, rt->h * rt->sy, 0, 1};
    if (!bound_valid || memcmp(rtv, bound_rtv, sizeof rtv) || dsv != bound_dsv) {
        /* a resource can't be bound as a target and a texture at once: unbind textures first */
        ID3D11ShaderResourceView *none[16] = {0};
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 16, none); dstate_invalidate();
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, rtv, dsv);
        memcpy(bound_rtv, rtv, sizeof rtv); bound_dsv = dsv;
        if (!bound_valid) memset(&bound_sc, 0xFF, sizeof bound_sc);
        bound_valid = 1; bound_vp.Width = -1;
    }
    if (memcmp(&vp, &bound_vp, sizeof vp)) { ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); bound_vp = vp; }
    int x0 = ch & 0xFFFF, x1 = x0 + (int)(ch >> 16), y0 = cv & 0xFFFF, y1 = y0 + (int)(cv >> 16);
    uint32_t sx = REG(0x08C0), sy = REG(0x08C4);
    if (sx >> 16) { int a = sx & 0xFFFF, b = a + (int)(sx >> 16); if (a > x0) x0 = a; if (b < x1) x1 = b; }
    if (sy >> 16) { int a = sy & 0xFFFF, b = a + (int)(sy >> 16); if (a > y0) y0 = a; if (b < y1) y1 = b; }
    if (sw) { x0 = 0; y0 = 0; x1 = (int)w; y1 = (int)h; }
    rt->clip_x0 = x0 * (int)rt->aax; rt->clip_x1 = x1 * (int)rt->aax; rt->clip_y0 = y0 * (int)rt->aay; rt->clip_y1 = y1 * (int)rt->aay;
    D3D11_RECT sc = {(LONG)(rt->clip_x0 * rt->sx), (LONG)(rt->clip_y0 * rt->sy), (LONG)ceilf(rt->clip_x1 * rt->sx), (LONG)ceilf(rt->clip_y1 * rt->sy)};
    if (memcmp(&sc, &bound_sc, sizeof sc)) { ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc); bound_sc = sc; }
    {   /* rows this draw can touch (target clip and scissor; not the viewport register, which the renderer does not apply): see surf_t.dirty_h */
        surf_t *all[5] = {rt->color[0], rt->color[1], rt->color[2], rt->color[3], rt->depth};
        for (int i = 0; i < 5; i++) if (all[i]) {
            all[i]->draw_seq++; all[i]->drawn_at = ++draw_clock;
            uint32_t yb = rt->clip_y1 > 0 ? (uint32_t)rt->clip_y1 : all[i]->h;
            if (yb > all[i]->h) yb = all[i]->h;
            if (yb > all[i]->dirty_h) all[i]->dirty_h = yb;
        }
    }
    return rt->ncolor || rt->depth;
}

/* ------------------------------------------------------------------ draw */
static uint32_t *tri_idx; static uint32_t tri_cap;
static const fp_insn_t *fp_prog;   /* this draw's decoded fragment program (fp_cached) */
/* Decoded fragment programs by content: decoding every draw's program field by field showed in race profiles (13% of the
 * RSX thread). The game copies each program into a ring before drawing (a new address nearly every draw) and rewrites
 * its inline constants, so the cache is keyed by the instruction words alone: a quick scan reads only what decides the
 * length (the end bit, constant sources), hashes the instructions, and on a hit just the constants are reloaded from
 * this copy. MNR_FPC_STATS=1 prints hits/misses. */
#define FPC_SLOTS 1024
static struct { uint64_t hash; int n; uint32_t nins; uint8_t *ins; fp_insn_t *prog; } fpc[FPC_SLOTS];
static long fpc_hit, fpc_miss;
static int fp_cached(uint32_t addr, const fp_insn_t **out) {
    static uint8_t ins[FP_MAX_INSNS * 16]; static uint32_t coff[FP_MAX_INSNS]; static int cidx[FP_MAX_INSNS];
    uint32_t a = addr, ni = 0; int n = 0, nc = 0; uint64_t h = 1469598103934665603ull;
    while (n < FP_MAX_INSNS) {
        const uint8_t *p = gp(a);
        uint32_t d = ld_u32(a), s0 = ld_u32(a + 4), s1 = ld_u32(a + 8), s2 = ld_u32(a + 12);
        d = d >> 16 | d << 16; s0 = s0 >> 16 | s0 << 16; s1 = s1 >> 16 | s1 << 16; s2 = s2 >> 16 | s2 << 16;   /* as fp_word */
        memcpy(ins + ni, p, 16); ni += 16;
        uint64_t w[2]; memcpy(w, p, 16);
        h = (h ^ w[0]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; h = (h ^ w[1]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29;
        int op = (int)((d >> 24 & 63) | (s1 >> 31) << 6);
        a += 16;
        if (!(op & 0x40) && ((s0 & 3) == 2 || (s1 & 3) == 2 || (s2 & 3) == 2)) { coff[nc] = a; cidx[nc++] = n; a += 16; }
        n++;
        if (d & 1) break;
    }
    unsigned slot = (unsigned)(h >> 54) & (FPC_SLOTS - 1);
    if (ENVFLAG("MNR_FPC_STATS") && (fpc_hit + fpc_miss) % 100000 == 99999) printf("[fpc] hit %ld, decoded %ld\n", fpc_hit, fpc_miss);
    if (fpc[slot].prog && fpc[slot].hash == h && fpc[slot].nins == ni && !memcmp(fpc[slot].ins, ins, ni)) {
        fp_insn_t *prog = fpc[slot].prog;
        for (int k = 0; k < nc; k++)   /* this copy's constants, halfword-swapped as fp_decode reads them */
            for (int j = 0; j < 4; j++) { uint32_t v = ld_u32(coff[k] + 4 * j); v = v >> 16 | v << 16; memcpy(&prog[cidx[k]].constant[j], &v, 4); }
        fpc_hit++; *out = prog; return fpc[slot].n;
    }
    fpc_miss++;
    if (!fpc[slot].prog) fpc[slot].prog = malloc(sizeof(fp_insn_t) * FP_MAX_INSNS);
    int m = fp_decode(addr, fpc[slot].prog, FP_MAX_INSNS);
    free(fpc[slot].ins); fpc[slot].ins = malloc(ni ? ni : 1); memcpy(fpc[slot].ins, ins, ni);
    fpc[slot].hash = h; fpc[slot].nins = ni; fpc[slot].n = m;
    if (m != n) fpc[slot].prog = fpc[slot].prog, fpc[slot].nins = 0;   /* scan and decode disagree: never reuse this slot's entry */
    *out = fpc[slot].prog; return m;
}

static uint32_t to_list(unsigned prim, const uint32_t *e, uint32_t n, D3D11_PRIMITIVE_TOPOLOGY *topo) {
    uint32_t need = n * 3 + 6;
    if (need > tri_cap) { tri_cap = need * 2; tri_idx = realloc(tri_idx, tri_cap * 4); }
    uint32_t m = 0;
    *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (prim) {
    case 1: *topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; memcpy(tri_idx, e, n * 4); return n;
    case 2: *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; memcpy(tri_idx, e, (n & ~1u) * 4); return n & ~1u;
    case 3: *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (uint32_t i = 0; i < n; i++) { tri_idx[m++] = e[i]; tri_idx[m++] = e[(i + 1) % n]; } return n > 1 ? m : 0;
    case 4: *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (uint32_t i = 0; i + 1 < n; i++) { tri_idx[m++] = e[i]; tri_idx[m++] = e[i + 1]; } return m;
    case 5: memcpy(tri_idx, e, (n - n % 3) * 4); return n - n % 3;
    case 6: for (uint32_t i = 0; i + 2 < n; i++) {
        if (i & 1) { tri_idx[m++] = e[i + 1]; tri_idx[m++] = e[i]; } else { tri_idx[m++] = e[i]; tri_idx[m++] = e[i + 1]; }
        tri_idx[m++] = e[i + 2];
    } return m;
    case 7: case 10: for (uint32_t i = 1; i + 1 < n; i++) { tri_idx[m++] = e[0]; tri_idx[m++] = e[i]; tri_idx[m++] = e[i + 1]; } return m;
    case 8: for (uint32_t i = 0; i + 3 < n; i += 4) {
        tri_idx[m++] = e[i]; tri_idx[m++] = e[i + 1]; tri_idx[m++] = e[i + 2];
        tri_idx[m++] = e[i]; tri_idx[m++] = e[i + 2]; tri_idx[m++] = e[i + 3];
    } return m;
    case 9: for (uint32_t i = 0; i + 3 < n; i += 2) {
        tri_idx[m++] = e[i]; tri_idx[m++] = e[i + 1]; tri_idx[m++] = e[i + 3];
        tri_idx[m++] = e[i]; tri_idx[m++] = e[i + 3]; tri_idx[m++] = e[i + 2];
    } return m;
    }
    return 0;
}

typedef struct { uint32_t afmt[16][4]; int32_t abase[16][4]; float aconst[16][4]; float vscale[4], voff[4], surf[4]; uint32_t ebase, pad[3]; } draw_cb_t;
typedef struct { uint32_t remap[16][4]; float alpha_ref[4]; uint32_t alpha_func[4]; float tex_size[16][4]; float wps[4]; } fpd_cb_t;

static void save_surface_bmp(surf_t *s, const char *name);
static int zq_running; extern long rsx_d3d_zq_draws;   /* (occlusion counting, below) */
/* MNR_D3D_DROPLOG=<from flip>: draws the backend drops before drawing, with the reason (debug) */
static void droplog(const char *why, unsigned prim, uint32_t nelems, uint32_t extra) {
    static long from = -2; static int n; if (from == -2) { const char *e = getenv("MNR_D3D_DROPLOG"); from = e ? atol(e) : -1; }
    if (from < 0 || rsx_flips < from || n++ > 3000) return;
    printf("[drop] flip %ld %s: prim %u n %u FP 0x%x target 0x%x colormask 0x%x extra 0x%x\n", rsx_flips, why, prim, nelems, REG(0x08E4), REG(0x0210), REG(0x0324), extra);
}
int rsx_d3d_draw(unsigned prim, const uint32_t *elems, uint32_t nelems) {
    if (!dev || failed) return 0;
    { static long skip = -1; if (skip < 0) { const char *e = getenv("MNR_D3D_SKIP_TARGET"); skip = e ? (long)strtoul(e, NULL, 16) : 0; }   /* debug: drop draws into one colour target (offset) */
      if (skip && REG(0x0210) == (uint32_t)skip) return 1; }
    { static int nz = -1; if (nz < 0) nz = getenv("MNR_D3D_SKIP_ZPREPASS") != NULL; if (nz && REG(0x0324) == 0 && (REG(0x0A74) & 1)) return 1; }   /* debug */
    { static int op = -1; if (op < 0) op = getenv("MNR_D3D_ONLYPROPS") != NULL;   /* debug: only draws with attribute 14 as UB256 (the prop batches) */
      if (op && (REG(0x0210) & 0x0FFFFFFF) == 0x0e2d0000 && !(((REG(0x1740 + 4 * 14) >> 4) & 15) && (REG(0x1740 + 4 * 14) & 15) == 7 && (REG(0x1FF0) >> 14 & 1))) return 1; }
    if (zq_running) rsx_d3d_zq_draws++;
    static long zqfrom = -2; if (zqfrom == -2) { const char *e = getenv("MNR_D3D_ZQLOG"); zqfrom = e ? atol(e) : -1; }
    int zql = zqfrom >= 0 && rsx_flips >= zqfrom;
    if (zql && zq_running) { static int n; if (n++ < 40) printf("[zq] draw prim %u n %u target 0x%x colormask 0x%08x ztest %u zfunc 0x%x zwrite %u cull %u/0x%x vscale (%g %g %g) voff (%g %g %g) clip h 0x%x v 0x%x\n",
        prim, nelems, REG(0x0210), REG(0x0324), REG(0x0A74) & 1, REG(0x0A6C), REG(0x0A70) & 1, REG(0x183C) & 1, REG(0x1830),
        u2f(REG(0x0A30)), u2f(REG(0x0A34)), u2f(REG(0x0A38)), u2f(REG(0x0A20)), u2f(REG(0x0A24)), u2f(REG(0x0A28)), REG(0x0394), REG(0x0398)); }
    rt_t rt;
    int zuse = (REG(0x0A74) & 1) || (REG(0x0328) & 1);
    int zwrite = ((REG(0x0A74) & 1) && (REG(0x0A70) & 1)) || ((REG(0x0328) & 1) && ((REG(0x0348) & 1) ||   /* two-sided stencil: taken as writing */
                 ((REG(0x032C) & 0xFF) && (REG(0x033C) != 0x1E00 || REG(0x0340) != 0x1E00 || REG(0x0344) != 0x1E00))));
    { static int off = -1; if (off < 0) off = getenv("MNR_D3D_NO_RO_DEPTH") != NULL; if (off) zwrite = 1; }
    if (!bind_targets(&rt, zuse ? (zwrite ? 1 : 2) : 0)) { if (zql && zq_running) printf("[zq]   -> no targets\n"); droplog("no targets", prim, nelems, REG(0x0208)); return 1; }
    /* shaders still compiling in the background: the main scene skips this draw, other targets wait (see sh_submit) */
    sh_skip = 0; sh_wait_pending = !(rt.ncolor && rt.color[0]->w >= 1280 && rt.color[0]->h >= 720);
    ID3D11VertexShader *vs = get_vs();
    uint32_t ctl = REG(0x1D60);
    uint32_t fp_addr = rsx_fp_addr();
    {   /* MNR_D3D_DRAWLOG=<from flip>: every draw into a 2560-wide target (the main scene): shader programs and key state (debug) */
        static long from = -2; if (from == -2) { const char *e = getenv("MNR_D3D_DRAWLOG"); from = e ? atol(e) : -1; }
        if (from >= 0 && rsx_flips >= from && rsx_flips < from + 400)
            printf("[draw] flip %ld tgt 0x%x fp 0x%x vp %u n %u blend %u cm 0x%x zt %u tex0 0x%x eye %.1f %.1f %.1f\n", rsx_flips, rt.ncolor ? rt.color[0]->addr : 0, REG(0x08E4), REG(0x1EA0) & 511, nelems, REG(0x0310) & 1, REG(0x0324), REG(0x0A74) & 1,
                   (REG(0x1A0C) >> 31) ? REG(0x1A00) : 0, u2f(rsx_vpc[293][0]), u2f(rsx_vpc[293][1]), u2f(rsx_vpc[293][2]));
    }
    int nfp = fp_cached(fp_addr, &fp_prog), nconst = 0;
    {   /* MNR_FPC_VERIFY=1 (debug): compare the cached program with a fresh decode */
        static int v = -1; static fp_insn_t *fresh; static long draws, bad;
        if (v < 0) { v = getenv("MNR_FPC_VERIFY") != NULL; if (v) fresh = malloc(sizeof(fp_insn_t) * FP_MAX_INSNS); }
        if (v) { int n2 = fp_decode(fp_addr, fresh, FP_MAX_INSNS); draws++;
                 if (n2 != nfp || memcmp(fresh, fp_prog, sizeof(fp_insn_t) * nfp)) { if (bad++ < 5) printf("[fpc] MISMATCH at fp 0x%08x (n %d vs %d)\n", fp_addr, nfp, n2); }
                 if (draws % 200000 == 0) printf("[fpc] %ld draws checked, %ld mismatches\n", draws, bad); }
    }
    uint8_t dims[16];
    rsx_texinfo_t ti[16]; int ten[16];
    for (int u = 0; u < 16; u++) {
        ten[u] = rsx_tex_info(u, &ti[u]); dims[u] = ten[u] ? (uint8_t)ti[u].dims : 2;
        /* depth formats (DEPTH24_D8/DEPTH16, float or not) on a depth surface: the RSX compares the coordinate's
         * r with the stored depth (shadow maps) and returns 0/1, with the function in TEXTURE_ADDRESS bits 28..31 */
        if (ten[u] && ti[u].dims == 2 && ti[u].fmt >= 0x90 && ti[u].fmt <= 0x93 && surf_find(ti[u].addr, 1)) dims[u] |= 0x80;
    }
    {   /* MNR_D3D_FPDUMP_TEX=<hex texture 0 address>: dump the fragment program of draws with that texture (debug) */
        static long long dt = -2; if (dt == -2) { const char *e = getenv("MNR_D3D_FPDUMP_TEX"); dt = e ? (long long)strtoull(e, NULL, 16) : -1; }
        if (dt >= 0 && ten[0] && ti[0].addr == (uint32_t)dt) fpdump_want = REG(0x08E4);
    }
    int watch_hit = 0;
    {   /* MNR_D3D_WATCHDRAW=<w>,<h>,<fmt>[,<count>]: at the first blended draw whose texture 0 has that size and format
         * (and that many elements), its fragment program, HLSL and every texture unit to watch_draw.txt (debug) */
        static int ww = -2, wh, wf, wc, wb = 1, done; static uint32_t dumped[4]; if (ww == -2) { const char *e = getenv("MNR_D3D_WATCHDRAW"); ww = -1; if (e) { wc = 0; sscanf(e, "%i,%i,%i,%i,%i", &ww, &wh, &wf, &wc, &wb); } }   /* <blend>: 1 blended (default), 0 not, -1 either */
        if (ww > 0 && ten[0] && (int)ti[0].w == ww && (int)ti[0].h == wh && (int)ti[0].fmt == wf && (wb < 0 || (int)(REG(0x0310) & 1) == wb) && (!wc || (int)nelems == wc)) {
            static long from = -2; if (from == -2) { const char *e = getenv("MNR_D3D_WATCH_FROM"); from = e ? atol(e) : 0; }   /* from that flip on */
            static int wn = -2; if (wn == -2) { const char *e = getenv("MNR_D3D_WATCH_NFP"); wn = e ? atoi(e) : 0; }   /* only programs that long */
            watch_hit = rsx_flips >= from && (!wn || nfp == wn);
        }
        if (watch_hit && done < 4 && REG(0x08E4) != dumped[0] && REG(0x08E4) != dumped[1] && REG(0x08E4) != dumped[2]) {   /* the first 4 programs */
            dumped[done] = REG(0x08E4); char wn[32]; snprintf(wn, sizeof wn, "watch_draw_%d.txt", done++); FILE *f = fopen(wn, "w"); char *src = malloc(1 << 20); int nc = 0;
            if (f && src) {
                fprintf(f, "flip %ld FP 0x%x ctl 0x%08x n %u target 0x%x prim %u colormask 0x%08x depth %u/%u func 0x%x\n", rsx_flips, REG(0x08E4), ctl, nelems, rt.ncolor ? rt.color[0]->addr : 0,
                        prim, REG(0x0324), REG(0x0A74) & 1, REG(0x0A70) & 1, REG(0x0A6C));
                for (int a = 0; a < 16; a++) { uint32_t fa = REG(0x1740 + 4 * a); if (!((fa >> 4) & 15) || !(REG(0x1FF0) >> a & 1)) continue;
                    uint32_t off = REG(0x1680 + 4 * a); fprintf(f, "attr %d type %u size %u stride %u offset 0x%08x (%s)", a, fa & 15, (fa >> 4) & 15, (fa >> 8) & 255, off & 0x7FFFFFFF, off >> 31 ? "main" : "local");
                    uint32_t ea = (off >> 31) ? gcm_io_to_ea_pub(off & 0x7FFFFFFF) : RSX_LOCAL_BASE + (off & 0x7FFFFFFF);
                    const uint8_t *p = gp(ea); fprintf(f, " first bytes"); for (int k = 0; k < 16; k++) fprintf(f, " %02x", p[k]); fprintf(f, "\n"); }
                for (int u = 0; u < 16; u++) if (ten[u]) { surf_t *cs = surf_find(ti[u].addr, 0), *ds = surf_find(ti[u].addr, 1);
                    fprintf(f, "tex%d addr 0x%08x %ux%u fmt 0x%x dims %u remap 0x%x wrap %u/%u unnorm %u levels %u%s%s\n", u, ti[u].addr, ti[u].w, ti[u].h, ti[u].fmt, dims[u], ti[u].remap, ti[u].wrap_s, ti[u].wrap_t, ti[u].unnorm, ti[u].levels,
                            cs ? " [colour surface]" : "", ds ? " [depth surface]" : ""); }
                char b2[512]; for (int i = 0; i < nfp; i++) { fp_disasm(&fp_prog[i], b2, sizeof b2); fprintf(f, "%3d: %s\n", i, b2); }
                {   uint32_t st = REG(0x1EA0) & 511; fprintf(f, "\nVP start %u in 0x%x out 0x%x\n", st, REG(0x1FF0), REG(0x1FF4));
                    for (uint32_t i = st; i < 512; i++) { vp_insn_t in; vp_decode(rsx_vp[i], &in); vp_disasm(&in, b2, sizeof b2); fprintf(f, "%3u: %s\n", i, b2); if (rsx_vp[i][3] & 1) break; } }
                if (!hlsl_fp(fp_prog, nfp, ctl, dims, src, 1 << 20, &nc)) fprintf(f, "\n%s\n", src);
                fclose(f);
            }
            free(src);
        }
    }
    ID3D11PixelShader *ps = get_ps(fp_addr, fp_prog, nfp, ctl, dims, &nconst);
    if (watch_hit && ps && getenv("MNR_D3D_WATCHOUT")) {   /* debug: the watched draw outputs float3(<MNR_D3D_WATCHOUT>), opaque */
        static struct { uint64_t fp; ID3D11PixelShader *ps; } wv[4096]; static int nwv;   /* keyed by the program without its constants (programs move and addresses get reused) */
        uint64_t wkey = (uint64_t)nfp; for (int i = 0; i < nfp; i++) wkey = fnv(&fp_prog[i], offsetof(fp_insn_t, constant), wkey);
        ID3D11PixelShader *v = NULL;
        for (int i = 0; i < nwv; i++) if (wv[i].fp == wkey) v = wv[i].ps;
        if (!v && nwv < 4096) {
            char *src = malloc(1 << 20); int nc = 0; char *tail = src ? NULL : NULL;
            if (src && !hlsl_fp(fp_prog, nfp, ctl, dims, src, (1 << 20) - 4096, &nc)) {
                const char *ret = "  return finish(R[0], R[1], R[2], R[3], R[4], H[0], H[4], H[6], H[8]);\n}\n";
                size_t rl = strlen(ret), sl = strlen(src);
                if (sl > rl && !strcmp(src + sl - rl, ret)) {
                    tail = src + sl - rl;
                    const char *at = getenv("MNR_D3D_WATCHOUT_AT");   /* output at instruction <n> (before it runs) instead of at the end */
                    if (at) { char *q = src; for (int k = 0, want = atoi(at); q && k <= want; k++) {   /* instruction starts, not their saturate/clamp lines */
                                  do q = strstr(q + 1, "\n  t = "); while (q && (!strncmp(q + 7, "saturate(t)", 11) || !strncmp(q + 7, "clamp(t,", 8))); }
                              if (q) tail = q + 1; }
                    sprintf(tail, "  PO o; o.c0 = float4(%s, 1); o.c1 = o.c2 = o.c3 = 0; return o;\n}\n", getenv("MNR_D3D_WATCHOUT"));
                    size_t len; void *code = compile(src, "ps_5_0", &len);
                    if (code) { ID3D11Device_CreatePixelShader(dev, code, len, NULL, &v); free(code); }
                    printf("[watch] variant for FP 0x%x: %s\n", REG(0x08E4), v ? "ok" : "compile failed");
                }
            }
            free(src);
            wv[nwv].fp = wkey; wv[nwv].ps = v; nwv++;
        }
        if (v) ps = v;
    }
    if (!vs || !ps) { if (sh_skip) { InterlockedIncrement(&rsx_d3d_sh_skipped); return 1; }
        if (!vs) rsx_d3d_skip_vs++; else rsx_d3d_skip_ps++; droplog(!vs ? "no VS" : "no PS", prim, nelems, 0); return 1; }

    /* elements -> list; index range for vertex upload */
    D3D11_PRIMITIVE_TOPOLOGY topo;
    uint32_t count = to_list(prim, elems, nelems, &topo);
    if (!count) { droplog("no elements", prim, nelems, 0); return 1; }
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (uint32_t i = 0; i < count; i++) { if (tri_idx[i] < lo) lo = tri_idx[i]; if (tri_idx[i] > hi) hi = tri_idx[i]; }
    if (hi - lo > 4u << 20) { droplog("index range", prim, nelems, hi - lo); return 1; }

    draw_cb_t dc; memset(&dc, 0, sizeof dc);
    uint32_t total = 0, inmask = REG(0x1FF0);
    struct { uint32_t addr, size, off; } span[16];
    for (int a = 0; a < 16; a++) {
        uint32_t f = REG(0x1740 + 4 * a), size = (f >> 4) & 15, type = f & 15, stride = (f >> 8) & 255;
        span[a].size = 0;
        for (int k = 0; k < 4; k++) dc.aconst[a][k] = u2f(REG(0x1C00 + 16 * a + 4 * k));
        if (!size || !(inmask >> a & 1)) continue;
        uint32_t off = REG(0x1680 + 4 * a);
        uint32_t base = (off >> 31) ? gcm_io_to_ea_pub((off & 0x7FFFFFFF) + REG(0x1738)) : RSX_LOCAL_BASE + (off & 0x7FFFFFFF) + REG(0x1738);
        if (!base) continue;
        static const uint8_t tsize[8] = {0, 2, 4, 2, 1, 2, 4, 1};
        uint32_t esz = type == 6 ? 4 : tsize[type & 7] * size;
        /* frequency divider (instancing): format bits 16..31; FREQUENCY_DIVIDER_OPERATION bit a: 1 = index % freq, 0 = index / freq */
        uint32_t freq = f >> 16, modulo = (REG(0x1FC0) >> a) & 1, alo = lo, ahi = hi;
        { static int n; if (ENVFLAG("MNR_D3D_FREQLOG") && n < 60 && (f >> 16) != 0) { n++; printf("[d3d] attr %d fmt 0x%08x freq %u op %u lo %u hi %u\n", a, f, f >> 16, modulo, lo, hi); } }
        if (freq > 1) {
            if (modulo) { if (hi - lo + 1 >= freq || lo % freq > hi % freq) { alo = 0; ahi = freq - 1; } else { alo = lo % freq; ahi = hi % freq; } }
            else { alo = lo / freq; ahi = hi / freq; }
            dc.abase[a][1] = (int32_t)freq; dc.abase[a][2] = (int32_t)modulo;
        }
        span[a].addr = base + stride * alo; span[a].size = stride * (ahi - alo) + esz; span[a].off = total;
        dc.afmt[a][0] = type; dc.afmt[a][1] = size; dc.afmt[a][2] = stride; dc.afmt[a][3] = 1;
        span[a].off = 0; dc.abase[a][0] = -(int32_t)(stride * alo);   /* + the span's place in the staging buffer, below */
    }
    struct { uint32_t addr, size, off; } grp[16]; int ngrp = 0, gidx[16] = {0};
    {   /* attributes interleaved in one vertex buffer overlap: stage each overlapping run of memory once (copying every
         * attribute's whole range on its own moved tens of MB a frame) */
        int ord[16], n = 0;
        for (int a = 0; a < 16; a++) if (span[a].size) {
            int k = n++; while (k > 0 && span[ord[k - 1]].addr > span[a].addr) { ord[k] = ord[k - 1]; k--; } ord[k] = a;
        }
        uint32_t gaddr = 0, gend = 0, goff = 0; int open = 0;
        for (int k = 0; k < n; k++) {
            int a = ord[k];
            if (open && span[a].addr <= gend) { if (span[a].addr + span[a].size > gend) gend = span[a].addr + span[a].size; }
            else {
                if (open) { grp[ngrp].addr = gaddr; grp[ngrp].size = gend - gaddr; grp[ngrp++].off = goff; total += (gend - gaddr + 3) & ~3u; }
                gaddr = span[a].addr; gend = gaddr + span[a].size; goff = total; open = 1;
            }
            span[a].off = goff + (span[a].addr - gaddr); gidx[a] = ngrp;
        }
        if (open) { grp[ngrp].addr = gaddr; grp[ngrp].size = gend - gaddr; grp[ngrp++].off = goff; total += (gend - gaddr + 3) & ~3u; }
        for (int a = 0; a < 16; a++) if (span[a].size) dc.abase[a][0] += (int32_t)span[a].off;
    }
    /* vertex data rendered on the GPU (the terrain's positions are drawn into a half-float target and then used as
     * a vertex buffer): bring such render targets back to guest memory before reading them */
    int wrote_back = 0;
    {   surf_t *dl[NSURF]; int nd = 0;   /* (only the GPU-dirty surfaces, not all of them per attribute) */
        for (int i = 0; i < NSURF; i++) if (surfs[i].tex && surfs[i].gpu_dirty) dl[nd++] = &surfs[i];
        for (int a = 0; nd && a < 16; a++) if (span[a].size)
            for (int i = 0; i < nd; i++) {
                surf_t *o = dl[i];
                if (o->gpu_dirty && o->addr < span[a].addr + span[a].size && span[a].addr < o->addr + surf_dirty_bytes(o)) { wb_why = 8; surf_writeback(o); wrote_back = 1; }
            }
    }
    { extern long long rsx_d3d_vbytes, rsx_d3d_vbig; rsx_d3d_vbytes += total; for (int k = 0; k < ngrp; k++) if (grp[k].size >= 16384) rsx_d3d_vbig += grp[k].size; }
    { static int n; if (ENVFLAG("MNR_D3D_VTXLOG") && n < 6) for (int a = 0; a < 16; a++)
        if (span[a].size && span[a].addr >= 0xc0000000u && span[a].addr < 0xc0100000u) { n++;
            printf("[d3d] attr %d from 0x%08x (%u bytes):", a, span[a].addr, span[a].size);
            uint32_t nz = 0; for (uint32_t k = 0; k < span[a].size; k++) nz += *gp(span[a].addr + k) != 0;
            printf(" nonzero %u:", nz); for (int k = 0; k < 16; k++) printf(" %04x", ld_u16(span[a].addr + 2 * k));
            printf("\n"); break; } }
    if (zql && zq_running) { static int n; if (n++ < 20) {   /* the counted draw's positions (attribute 0) */
        uint32_t f = REG(0x1740), stride = f >> 8 & 0xFF;
        printf("[zq]   attr0 fmt 0x%08x at 0x%08x:", f, span[0].addr);
        for (uint32_t v = lo; v <= hi && v < lo + 4; v++) { printf(" ("); for (int k = 0; k < 4; k++) printf("%g ", u2f(ld_u32(span[0].addr + (v - lo) * stride + 4 * k))); printf(")"); }
        printf("  VP c[0..3] row0 (%g %g %g %g)\n", u2f(rsx_vpc[0][0]), u2f(rsx_vpc[0][1]), u2f(rsx_vpc[0][2]), u2f(rsx_vpc[0][3])); } }
    ensure_vbuf(total + 16);
    {   /* MNR_VSTAT=1 (debug): per 300 flips, vertex bytes copied: all, from RSX local memory, unchanged for 2+ scans */
        static int on = -1; if (on < 0) on = getenv("MNR_VSTAT") != NULL;
        if (on) {
            static double all, loc, stable, stable_loc; static long f0 = -1, draws;
            extern long rsx_ww_scans(void); extern uint64_t rsx_ww_gen_after(long k), rsx_range_gen(uint32_t a, uint32_t s);
            uint64_t old = rsx_ww_gen_after(rsx_ww_scans() - 4); draws++;
            for (int k = 0; k < ngrp; k++) { int l = grp[k].addr >= 0xC0000000u, st = old != UINT64_MAX && rsx_range_gen(grp[k].addr, grp[k].size) < old;
                all += grp[k].size; if (l) loc += grp[k].size; if (st) stable += grp[k].size; if (st && l) stable_loc += grp[k].size; }
            if (f0 < 0) f0 = rsx_flips;
            if (rsx_flips - f0 >= 300) { double n = (double)(rsx_flips - f0);
                printf("[vstat] per frame: %ld draws, %.1f MB vertex data, %.1f MB from local memory, %.1f MB unchanged 2+ scans (%.1f MB of it local)\n",
                       (long)(draws / n), all / n / 1e6, loc / n / 1e6, stable / n / 1e6, stable_loc / n / 1e6);
                all = loc = stable = stable_loc = 0; draws = 0; f0 = rsx_flips; }
        }
    }
    /* Vertex cache: a run of guest memory already in the ring from an earlier draw is used from there while write-watch
     * shows no write to it since that upload and none in the last scans before it (a race copied ~35 MB a frame, ~30 MB
     * of it unchanged track and scenery). Entries die when the ring wraps (its contents are discarded) and are not used
     * by a draw that just wrote a GPU surface back into its memory. The rest is copied straight from guest memory into
     * the ring (a staging copy first doubled the copying). MNR_NO_VCACHE=1: copy everything (A/B tests). */
    uint32_t gabs[16], gnew[16], newtotal = 0; uint64_t gg[16]; int need[16];
    {   static int off = -1; if (off < 0) off = getenv("MNR_NO_VCACHE") != NULL;
        extern long rsx_ww_scans(void); extern uint64_t rsx_ww_gen_after(long k), rsx_range_gen(uint32_t a, uint32_t s);
        uint64_t old = off || wrote_back ? UINT64_MAX : rsx_ww_gen_after(rsx_ww_scans() - 3);
        for (int k = 0; k < ngrp; k++) {
            need[k] = 1; gg[k] = UINT64_MAX;
            if (old != UINT64_MAX && grp[k].addr >= 0xC0000000u) {   /* (RSX local memory: its write-watch answer is a table lookup) */
                uint64_t g = rsx_range_gen(grp[k].addr, grp[k].size); vc_t *e = &vcache[vc_slot(grp[k].addr, grp[k].size)];
                if (e->addr == grp[k].addr && e->size == grp[k].size && e->lap == vbuf_lap && e->gen == g && g < old) { gabs[k] = e->off; need[k] = 0; }
                gg[k] = g;
            }
            if (need[k]) { gnew[k] = newtotal; newtotal += (grp[k].size + 3) & ~3u; }
        }
        uint32_t at = (vbuf_pos + 15) & ~15u;
        if (newtotal && at + newtotal + 4 > vbuf_size) {   /* this upload wraps the ring: what it reuses goes too */
            newtotal = 0; for (int k = 0; k < ngrp; k++) { need[k] = 1; gnew[k] = newtotal; newtotal += (grp[k].size + 3) & ~3u; }
        }
        if (newtotal) {
            uint32_t voff = 0; uint8_t *dst = ring_map(vbuf, &vbuf_pos, vbuf_size, newtotal + 4, 16, &voff);
            if (dst) {
                for (int k = 0; k < ngrp; k++) if (need[k]) {
                    memcpy(dst + gnew[k], gp(grp[k].addr), grp[k].size); gabs[k] = voff + gnew[k];
                    if (gg[k] != UINT64_MAX) { vc_t *e = &vcache[vc_slot(grp[k].addr, grp[k].size)]; e->addr = grp[k].addr; e->size = grp[k].size; e->lap = vbuf_lap; e->gen = gg[k]; e->off = gabs[k]; }
                }
                ring_unmap(vbuf);
            } else for (int k = 0; k < ngrp; k++) if (need[k]) gabs[k] = 0;
        }
    }
    for (int a = 0; a < 16; a++) if (dc.afmt[a][3]) dc.abase[a][0] += (int32_t)(gabs[gidx[a]] - grp[gidx[a]].off);
    ensure_ebuf(count);
    uint32_t eoff = ring_put(ebuf, &ebuf_pos, ebuf_size, tri_idx, count * 4, 4);
    dc.ebase = eoff / 4;

    for (int k = 0; k < 4; k++) {
        dc.vscale[k] = u2f(REG(0x0A30 + 4 * k)) * (k == 0 ? rt.aax : k == 1 ? rt.aay : 1);
        dc.voff[k] = u2f(REG(0x0A20 + 4 * k)) * (k == 0 ? rt.aax : k == 1 ? rt.aay : 1);
    }
    dc.surf[0] = (float)rt.w; dc.surf[1] = (float)rt.h;
    {   if (tgtlog_match(&rt))
            printf("[tgtlog] flip %ld draw into 0x%08x (%ux%u, rt %ux%u): %u verts, FP 0x%x, viewport scale (%g %g) offset (%g %g), clip x %d-%d y %d-%d, window 0x%x\n",
                   rsx_flips, rt.color[0]->addr, rt.color[0]->w, rt.color[0]->h, rt.w, rt.h, count, REG(0x08E4), dc.vscale[0], dc.vscale[1], dc.voff[0], dc.voff[1],
                   rt.clip_x0, rt.clip_x1, rt.clip_y0, rt.clip_y1, REG(0x02B8));
    }
    upload(cb_draw, &dc, sizeof dc);
    static uint32_t last_vpc_gen = 0xFFFFFFFF;
    extern uint32_t rsx_vpc_gen;
    if (rsx_vpc_gen != last_vpc_gen) { upload(cb_vpc, rsx_vpc, sizeof(uint32_t) * 468 * 4); last_vpc_gen = rsx_vpc_gen; }

    /* fragment constants and textures */
    static float fc[FP_MAX_INSNS][4];
    int ci = 0;
    for (int i = 0; i < nfp; i++) if (fp_prog[i].has_const) memcpy(fc[ci++], fp_prog[i].constant, 16);
    upload(cb_fpc, fc, 16 * (ci ? ci : 1));
    fpd_cb_t fd; memset(&fd, 0, sizeof fd);
    ID3D11ShaderResourceView *srv[16] = {0}; ID3D11SamplerState *smp[16] = {0};
    /* Only the units the fragment program samples (as hlsl_fp counts them): a unit left enabled but unused is never read
     * by the RSX, but setting it up can cost a GPU->CPU write-back and a re-upload. Returning to the ModSpot, the paint
     * jobs left a 2048x2048 B8 texture enabled over the 512x512 FP16 target they draw into: every draw wrote the target
     * back and re-uploaded 16 MB, about 13 ms a draw, and the load took 100-150 s. */
    uint32_t fp_tex_used = 0;
    for (int i = 0; i < nfp; i++) { uint8_t op = fp_prog[i].op; if (op == 0x17 || op == 0x18 || op == 0x19 || op == 0x2F || op == 0x31) fp_tex_used |= 1u << fp_prog[i].tex; }
    { static int all = -1; if (all < 0) all = getenv("MNR_D3D_ALL_TEX_UNITS") != NULL; if (all) fp_tex_used = 0xFFFF; }   /* A/B test: the old behaviour */
    for (int u = 0; u < 16; u++) {
        if (!ten[u] || !(fp_tex_used >> u & 1)) continue;
        uint32_t is_surf, is_depth;
        srv[u] = tex_srv(u, &ti[u], &is_surf, &is_depth);
        {   /* MNR_D3D_TGTLOG=<hex addr> (debug): the textures each draw into that colour target samples */
            if (tgtlog_match(&rt)) {
                surf_t *ss = is_surf ? surf_find(ti[u].addr, is_depth) : NULL;
                printf("[tgtlog] flip %ld unit %d tex 0x%08x %ux%u fmt 0x%02x linear %u pitch %u%s", rsx_flips, u, ti[u].addr, ti[u].w, ti[u].h, ti[u].fmt, ti[u].linear, ti[u].pitch, is_surf ? " SURFACE" : " (from memory)");
                if (ss) printf(" (surface %ux%u pitch %u drawn_at %llu)", ss->w, ss->h, ss->pitch, (unsigned long long)ss->drawn_at);
                printf("\n");
            }
        }
        {   /* MNR_D3D_TEXSET=<from flip>: per frame, the distinct textures (address, size, format, surface?) sampled by draws into
             * the main scene target (debug: what lighting inputs a frame uses) */
            static long from = -2; if (from == -2) { const char *e = getenv("MNR_D3D_TEXSET"); from = e ? atol(e) : -1; }
            if (from >= 0 && rsx_flips >= from && rt.ncolor && rt.color[0]->w == 2560) {
                static uint32_t seen[256]; static int nseen; static long sf = -1;
                if (sf != rsx_flips) { sf = rsx_flips; nseen = 0; }
                int dup = 0; for (int k = 0; k < nseen; k++) if (seen[k] == ti[u].addr) dup = 1;
                if (!dup && nseen < 256) { seen[nseen++] = ti[u].addr;
                    printf("[texset] flip %ld unit %d tex 0x%08x %ux%u fmt 0x%02x%s FP 0x%x depth 0x%x\n", rsx_flips, u, ti[u].addr, ti[u].w, ti[u].h, ti[u].fmt, is_surf ? " (surface)" : "",
                           REG(0x08E4), rt.depth ? rt.depth->addr : 0); }
            }
        }
        smp[u] = (dims[u] & 0x80) ? (ID3D11SamplerState *)shadow_sampler(&ti[u], REG(0x1A08 + 0x20 * u) >> 28) : sampler(&ti[u]);
        fd.remap[u][0] = ti[u].remap & 0xFF; fd.remap[u][1] = (ti[u].remap >> 8) & 0xFF;
        fd.remap[u][2] = is_surf && !is_depth;
        if (fd.remap[u][2]) { surf_t *cs = surf_find(ti[u].addr, 0); if (cs && cs->rsx_fmt == 9) fd.remap[u][2] = 2; }   /* B8 target: broadcast */
        fd.remap[u][3] = is_depth ? (ti[u].fmt == 0x85 ? 2 : 1) : 0;   /* 2: depth bytes as ARGB8 */
        /* coordinate scale: unnormalized (texel) coordinates, and surfaces whose D3D texture differs in size */
        float sx = ti[u].unnorm ? 1.0f / ti[u].w : 1.0f, sy = ti[u].unnorm ? 1.0f / ti[u].h : 1.0f;
        if (is_surf) {
            surf_t *s = surf_find(ti[u].addr, is_depth);
            if (s) {
                uint32_t ew = ti[u].w * s->aa_x <= s->w ? ti[u].w * s->aa_x : ti[u].w, eh = ti[u].h * s->aa_y <= s->h ? ti[u].h * s->aa_y : ti[u].h;
                sx *= (float)ew / s->w; sy *= (float)eh / s->h;
            }
        }
        fd.tex_size[u][0] = sx; fd.tex_size[u][1] = sy;
    }
    fd.wps[0] = 1.0f / rt.sx; fd.wps[1] = 1.0f / rt.sy;   /* (used only by programs built with res_scale) */
    fd.alpha_ref[0] = (REG(0x030C) & 0xFF) / 255.0f;
    fd.alpha_func[0] = REG(0x0308) & 0xF; fd.alpha_func[1] = REG(0x0304) & 1;
    { static int flat = -1; if (flat < 0) flat = getenv("MNR_D3D_FLAT") != NULL; fd.alpha_func[2] = flat && (REG(0x0A74) & 1); static uint32_t dn; fd.alpha_func[3] = ++dn % 997; }   /* debug: a colour per 3D draw */
    upload(cb_fpd, &fd, sizeof fd);

    apply_states(rt.ncolor, rt.ncolor ? rt.color[0]->rsx_fmt : 0);
    {   /* only what changed since the last draw (thousands of draws a frame; the driver checks every call) */
        static D3D11_PRIMITIVE_TOPOLOGY c_topo; static ID3D11VertexShader *c_vs; static ID3D11PixelShader *c_ps;
        static ID3D11ShaderResourceView *c_vsr[2], *c_srv[16]; static ID3D11SamplerState *c_smp[16];
        static int off = -1; if (off < 0) off = getenv("MNR_D3D_NO_STATE_CACHE") != NULL;
        if (!ds_valid || off) {
            ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
            ID3D11Buffer *vcb[2] = {cb_vpc, cb_draw}, *pcb[2] = {cb_fpc, cb_fpd};
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 2, vcb);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 2, pcb);
            c_topo = (D3D11_PRIMITIVE_TOPOLOGY)-1; c_vs = NULL; c_ps = NULL;
            memset(c_vsr, 0xFF, sizeof c_vsr); memset(c_srv, 0xFF, sizeof c_srv); memset(c_smp, 0xFF, sizeof c_smp);
            ds_valid = 1;
        }
        if (topo != c_topo) { ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo); c_topo = topo; }
        if (vs != c_vs) { ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0); c_vs = vs; }
        if (ps != c_ps) { ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0); c_ps = ps; }
        ID3D11ShaderResourceView *vsr[2] = {vbuf_srv, ebuf_srv};
        if (memcmp(vsr, c_vsr, sizeof vsr)) { ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 2, vsr); memcpy(c_vsr, vsr, sizeof vsr); }
        int lo_s = 16, hi_s = -1, lo_m = 16, hi_m = -1;
        for (int u = 0; u < 16; u++) {
            if (srv[u] != c_srv[u]) { if (u < lo_s) lo_s = u; hi_s = u; }
            if (smp[u] != c_smp[u]) { if (u < lo_m) lo_m = u; hi_m = u; }
        }
        if (hi_s >= 0) { ID3D11DeviceContext_PSSetShaderResources(ctx, lo_s, hi_s - lo_s + 1, srv + lo_s); memcpy(c_srv + lo_s, srv + lo_s, (hi_s - lo_s + 1) * sizeof *srv); }
        if (hi_m >= 0) { ID3D11DeviceContext_PSSetSamplers(ctx, lo_m, hi_m - lo_m + 1, smp + lo_m); memcpy(c_smp + lo_m, smp + lo_m, (hi_m - lo_m + 1) * sizeof *smp); }
        if (off) ds_valid = 0;
    }
    if (zql && zq_running) { static int n; if (n++ < 40) printf("[zq]   -> drawn: %u vertices, alpha test %u (func 0x%x ref 0x%x), FP 0x%x\n", count, REG(0x0304) & 1, REG(0x0308), REG(0x030C), REG(0x08E4)); }
    ID3D11DeviceContext_Draw(ctx, count, 0);
    if (watch_hit && getenv("MNR_D3D_WATCHSAVE") && rt.ncolor) {   /* debug: the target after the watched draw, every 30th flip */
        static long last = -100; static int ns;
        if (rsx_flips - last >= 30 && ns < 30) { char nm[64]; snprintf(nm, sizeof nm, "watch_%05ld.bmp", rsx_flips); save_surface_bmp(rt.color[0], nm); last = rsx_flips; ns++;
            const char *e = getenv("MNR_D3D_WATCHSTEP"); if (e && ns == atoi(e)) stepdump_force = rsx_flips + 1; }   /* step-dump the frame after the <n>th save */
    }
    {   /* MNR_D3D_STEPDUMP=<flip>: the main scene buffer after every 40th draw into it during that frame */
        {   /* MNR_D3D_CONSTLOG=<from flip>,<tex0 hex>: all vertex constants at the first main-pass draw with that texture, each flip (debug) */
            static long from = -2; static uint32_t tex; static long last = -1;
            if (from == -2) { const char *e = getenv("MNR_D3D_CONSTLOG"); from = e ? atol(e) : -1; tex = e && strchr(e, ',') ? (uint32_t)strtoul(strchr(e, ',') + 1, NULL, 16) : 0; }
            if (from >= 0 && rsx_flips >= from && rsx_flips < from + 400 && last != rsx_flips && (!tex || (ten[0] && ti[0].addr == tex)) && rt.ncolor && rt.color[0]->w >= 2560) {
                last = rsx_flips; printf("[const] flip %ld", rsx_flips);
                for (int ci = 0; ci < 468; ci++) { float f4[4]; memcpy(f4, rsx_vpc[ci], 16); printf(" %g,%g,%g,%g", f4[0], f4[1], f4[2], f4[3]); }
                printf("\n");
            }
        }
        static long sf = -2; static int n;
        if (sf == -2) { const char *e = getenv("MNR_D3D_STEPDUMP"); sf = e ? atol(e) : -1; }
        if (stepdump_force >= 0) { sf = stepdump_force; stepdump_force = -1; n = 0; }
        static int every = -1; if (every < 0) { const char *e = getenv("MNR_D3D_STEPEVERY"); every = e ? atoi(e) : 40; if (every < 1) every = 1; }
        static long long stt = -2; if (stt == -2) { const char *e = getenv("MNR_D3D_STEPTARGET"); stt = e ? (long long)strtoull(e, NULL, 16) : -1; }   /* only draws into that colour offset */
        if (stt >= 0 && REG(0x0210) == (uint32_t)stt && sf >= 0 && rsx_flips >= sf - 2 && rsx_flips <= sf) { static long lastp = -1; if (lastp != rsx_flips) { lastp = rsx_flips; printf("[step] flip %ld draws into target (%u x %u, ncolor %d)\n", rsx_flips, rt.color[0] ? rt.color[0]->w : 0, rt.color[0] ? rt.color[0]->h : 0, rt.ncolor); } }
        if (stt >= 0 && sf >= 0 && rsx_flips > sf && REG(0x0210) == (uint32_t)stt && !n) sf = rsx_flips;   /* the first frame from <flip> on that draws into it */
        if (sf >= 0 && rsx_flips == sf && rt.ncolor && rt.color[0]->w >= 1024 && rt.color[0]->h >= 600 && (stt < 0 || REG(0x0210) == (uint32_t)stt)) {
            static int logall = -1; if (logall < 0) logall = getenv("MNR_D3D_STEPLOGALL") != NULL;   /* log every draw, save every <every>th */
            if (logall) {   /* and each program once: disassembly and texture units to stepfp_<FP>.txt */
                static uint32_t seen[4096]; static int nseen; int known = 0;
                for (int i = 0; i < nseen; i++) if (seen[i] == REG(0x08E4)) known = 1;
                if (!known && nseen < 4096) {
                    seen[nseen++] = REG(0x08E4); char nm[64]; snprintf(nm, sizeof nm, "stepfp_%x.txt", REG(0x08E4)); FILE *f = fopen(nm, "w");
                    if (f) { for (int u = 0; u < 16; u++) if (ten[u]) fprintf(f, "tex%d addr 0x%08x %ux%u fmt 0x%x dims %u remap 0x%x%s%s\n", u, ti[u].addr, ti[u].w, ti[u].h, ti[u].fmt, dims[u], ti[u].remap,
                                                                         surf_find(ti[u].addr, 0) ? " [colour surface]" : "", surf_find(ti[u].addr, 1) ? " [depth surface]" : "");
                             char b2[512]; for (int i = 0; i < nfp; i++) { fp_disasm(&fp_prog[i], b2, sizeof b2); fprintf(f, "%3d: %s\n", i, b2); } fclose(f); }
                }
            }
            int white = -1;
            if (logall && getenv("MNR_D3D_STEPWHITE") && rt.color[0]->dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM) {   /* near-white pixels in the upper middle band after each draw */
                static ID3D11Texture2D *stg; static UINT sw, sh;
                D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(rt.color[0]->tex, &d);
                if (!stg || sw != d.Width || sh != d.Height) { if (stg) ID3D11Texture2D_Release(stg); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
                    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &stg))) stg = NULL; sw = d.Width; sh = d.Height; }
                D3D11_MAPPED_SUBRESOURCE m;
                if (stg) { ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)rt.color[0]->tex);
                    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) { white = 0;
                        for (UINT y = sh / 4; y < sh * 55 / 100; y += 2) { const uint8_t *row = (const uint8_t *)m.pData + y * m.RowPitch;
                            for (UINT x = 0; x < sw; x += 2) if (row[4 * x] > 225 && row[4 * x + 1] > 225 && row[4 * x + 2] > 225) white++; }
                        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0); } }
            }
            if (logall && (n % every || white >= 0)) printf("[step] %04d FP 0x%x tex0 0x%08x %ux%u fmt 0x%x | blend %u 0x%08x 0x%08x | count %u | depth %u write %u func 0x%x | cm 0x%08x | st %u f 0x%x ref 0x%x m 0x%x wm 0x%x | nfp %d white %d\n", n, REG(0x08E4), ten[0] ? ti[0].addr : 0, ten[0] ? ti[0].w : 0, ten[0] ? ti[0].h : 0, ten[0] ? ti[0].fmt : 0,
                                            REG(0x0310) & 1, REG(0x0314), REG(0x0318), count, REG(0x0A74) & 1, REG(0x0A70) & 1, REG(0x0A6C), REG(0x0324),
                                            REG(0x0328) & 1, REG(0x0330), REG(0x0334), REG(0x0338), REG(0x032C), nfp, white);
            if (n % every == 0) { char nm[64]; snprintf(nm, sizeof nm, "step_%04d.bmp", n); save_surface_bmp(rt.color[0], nm);
                printf("[step] %04d FP 0x%x tex0 0x%08x %ux%u fmt 0x%x | blend %u 0x%08x 0x%08x eq 0x%08x | colormask 0x%08x | alphatest %u | count %u | depth %u func 0x%x write %u | stencil %u func 0x%x ref 0x%x mask 0x%x wmask 0x%x ops 0x%x/0x%x/0x%x | Z 0x%x zfmt %u\n", n, REG(0x08E4),
                       ten[0] ? ti[0].addr : 0, ten[0] ? ti[0].w : 0, ten[0] ? ti[0].h : 0, ten[0] ? ti[0].fmt : 0, REG(0x0310) & 1, REG(0x0314), REG(0x0318), REG(0x0320), REG(0x0324), REG(0x0304) & 1, count,
                       REG(0x0A74) & 1, REG(0x0A6C), REG(0x0A70) & 1, REG(0x0328) & 1, REG(0x0330), REG(0x0334), REG(0x0338), REG(0x032C), REG(0x033C), REG(0x0340), REG(0x0344),
                       REG(0x0214), (REG(0x0208) >> 5) & 7);
                for (int ci = 256; ci < 268; ci++) { float f4[4]; memcpy(f4, rsx_vpc[ci], 16); printf("   c%d=(%g %g %g %g)", ci, f4[0], f4[1], f4[2], f4[3]); }
                printf("\n"); }
            n++;
        }
    }
    {   /* MNR_D3D_SAVETARGET=<hex colour offset>: after each draw into that target, save it and the surface
         * bound as texture 0 (if it is a render target) as BMPs (debug) */
        static long st = -2; static int n;
        if (st == -2) { const char *e = getenv("MNR_D3D_SAVETARGET"); st = e ? (long)strtoul(e, NULL, 16) : -1; }
        static int sw_ = -2, sh_; if (sw_ == -2) { const char *e = getenv("MNR_D3D_SAVE_WH"); sw_ = -1; if (e) sscanf(e, "%dx%d", &sw_, &sh_); }   /* or every target of that size */
        int skipfp = 0;   /* MNR_D3D_SAVE_SKIPFP=<hex>,<hex>: not draws with these FP register values (e.g. hundreds of brush stamps) */
        { const char *e = ENVSTR("MNR_D3D_SAVE_SKIPFP"); if (e) { char want[16]; snprintf(want, sizeof want, "%x", REG(0x08E4)); if (strstr(e, want)) skipfp = 1; } }
        static int sfmt = -2; if (sfmt == -2) { const char *e = getenv("MNR_D3D_SAVE_FMT"); sfmt = e ? atoi(e) : -1; }   /* or every target of that RSX colour format */
        static int smax = -1; if (smax < 0) { const char *e = getenv("MNR_D3D_SAVE_MAX"); smax = e ? atoi(e) : 200; }
        static long sfrom = -1; if (sfrom < 0) { const char *e = getenv("MNR_D3D_SAVE_FROM"); sfrom = e ? atol(e) : 0; }   /* from that flip on */
        if (rsx_flips < sfrom) skipfp = 1;
        { const char *e = ENVSTR("MNR_D3D_SAVE_SKIPADDR"); char want[16]; if (e && rt.ncolor) { snprintf(want, sizeof want, "%08x", rt.color[0]->addr); if (strstr(e, want)) skipfp = 1; } }
        if (!skipfp && ((st >= 0 && REG(0x0210) == (uint32_t)st) || (sw_ > 0 && rt.ncolor && (int)rt.color[0]->w == sw_ && (int)rt.color[0]->h == sh_)
                        || (sfmt >= 0 && rt.ncolor && (int)rt.color[0]->rsx_fmt == sfmt)) && rt.ncolor && n < smax) {
            char nm[64]; snprintf(nm, sizeof nm, "tgt_%03d_f%ld_%08x.bmp", n, rsx_flips, rt.color[0]->addr); save_surface_bmp(rt.color[0], nm);
            printf("[savetarget] %03d flip %ld target 0x%08x fmt %u FP 0x%x tex0 0x%08x fmt 0x%x tex1 0x%08x blend %u colormask 0x%08x\n", n, rsx_flips, rt.color[0]->addr, rt.color[0]->rsx_fmt, REG(0x08E4),
                   ten[0] ? ti[0].addr : 0, ten[0] ? ti[0].fmt : 0, ten[1] ? ti[1].addr : 0, REG(0x0310) & 1, REG(0x0324));
            if (getenv("MNR_D3D_SAVE_CONSTS")) {   /* the program's inline constants, as this draw used them (debug) */
                printf("[savetarget] %03d blend sf 0x%08x df 0x%08x eq 0x%08x color 0x%08x consts", n, REG(0x0314), REG(0x0318), REG(0x0320), REG(0x031C));
                for (int i = 0; i < nfp; i++) if (fp_prog[i].has_const) printf(" {%g,%g,%g,%g}", fp_prog[i].constant[0], fp_prog[i].constant[1], fp_prog[i].constant[2], fp_prog[i].constant[3]);
                printf("\n");
                char want[16]; snprintf(want, sizeof want, ",%d,", n); const char *e = ENVSTR("MNR_D3D_SAVE_TEX");   /* ",89,183,": these steps' textures (debug) */
                if (e && strstr(e, want)) for (int u = 0; u < 4; u++) if (ten[u] && ti[u].w <= 1024 && ti[u].h <= 1024 && !ti[u].is_float) {
                    uint8_t *px = malloc((size_t)ti[u].w * ti[u].h * 4); if (!px) continue;
                    rsx_tex_decode(u, px);   /* A,R,G,B bytes */
                    for (int half = 0; half < 2; half++) {   /* colour, then alpha as grey */
                        char tn[64]; snprintf(tn, sizeof tn, "tex_%03d_u%d_%s_%08x.bmp", n, u, half ? "alpha" : "rgb", ti[u].addr);
                        FILE *f = fopen(tn, "wb"); if (!f) continue;
                        uint32_t w = ti[u].w, h = ti[u].h, sz = 54 + w * h * 3;
                        uint8_t hd[54] = {'B', 'M'}; memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40; memcpy(hd + 18, &w, 4); int32_t nh = -(int32_t)h; memcpy(hd + 22, &nh, 4); hd[26] = 1; hd[28] = 24;
                        fwrite(hd, 1, 54, f);
                        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { const uint8_t *p = px + 4 * ((size_t)y * w + x);
                            uint8_t bgr[3] = {half ? p[0] : p[3], half ? p[0] : p[2], half ? p[0] : p[1]}; fwrite(bgr, 1, 3, f); }
                        fclose(f);
                    }
                    printf("[savetarget] %03d tex%d 0x%08x %ux%u fmt 0x%x remap 0x%x levels %u\n", n, u, ti[u].addr, ti[u].w, ti[u].h, ti[u].fmt, ti[u].remap, ti[u].levels);
                    free(px);
                }
            }
            if (ten[0]) { surf_t *src = surf_find(ti[0].addr, 0); if (src) { snprintf(nm, sizeof nm, "tgt_%03d_src_%08x.bmp", n, ti[0].addr); save_surface_bmp(src, nm); }
                          else printf("[d3d] savetarget %d: texture 0 at 0x%08x is not a render target\n", n, ti[0].addr); }
            n++;
        }
    }
    return 1;
}

static void clear_channels(surf_t *s, const float col[4], UINT8 wmask, const D3D11_RECT *r);
/* ------------------------------------------------------------------ occlusion (ZPASS pixel count) reports
 * SET_ZPASS_PIXEL_COUNT_ENABLE turns counting on and off, CLEAR_REPORT_VALUE (type 1) zeroes the count and GET_REPORT
 * (type 1) reads it: the samples that passed the depth test meanwhile. Counted with Direct3D occlusion queries
 * (one per run of draws between those commands; reading waits for the GPU). The game sizes its sun glare from it: the
 * constant "visible" answer this replaced made the sun hundreds of times too bright, and the exposure turned the scene
 * black for a frame (MNR_D3D_ZPASS_FIXED=1 brings that back, for comparison). */
/* Reading a query waits for the GPU to finish everything before it, which stalled this thread about twice a frame
 * (~15% of its time in races). So a report is answered with the last value for that report slot and the real count
 * is written into the report memory when the queries finish (polled without waiting): the glare is a frame late. */
#define ZQ_POOL 64
static ID3D11Query *zq_pool[ZQ_POOL]; static uint8_t zq_own[ZQ_POOL], zq_ref[ZQ_POOL];   /* own: running or counted since the last clear; ref: pending reports */
static int zq_cur = -1, zq_enabled, zq_running;
static int zq_acc[32], zq_nacc; static uint64_t zq_acc_done;   /* runs since the last clear: their queries, plus counts read synchronously */
static struct { uint32_t addr; int q[32]; int n; uint64_t sum; } zq_job[16]; static int zq_njob;
static uint32_t zq_last[64]; static uint8_t zq_known[64];
long rsx_d3d_zq_draws, rsx_d3d_zq_enables, rsx_d3d_zq_clears;
long long rsx_d3d_vbytes, rsx_d3d_vbig;   /* vertex bytes staged and uploaded (and those in spans of 16 KB or more), per frame-log window */
static uint64_t zq_wait(int i) { UINT64 n = 0; while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)zq_pool[i], &n, sizeof n, 0) == S_FALSE) ; return n; }
static int zq_job_poll(int k, int block) {   /* 1: done (value written to the report) */
    for (int m = 0; m < zq_job[k].n; m++) {
        int i = zq_job[k].q[m]; if (i < 0) continue;
        UINT64 n = 0;
        if (block) n = zq_wait(i);
        else if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)zq_pool[i], &n, sizeof n, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        zq_job[k].sum += n; zq_job[k].q[m] = -1; zq_ref[i]--;
    }
    for (int m = 0; m < zq_job[k].n; m++) if (zq_job[k].q[m] >= 0) return 0;
    uint32_t v = zq_job[k].sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)zq_job[k].sum, slot = (zq_job[k].addr >> 4) & 63;
    st_u32(zq_job[k].addr, v); zq_last[slot] = v; zq_known[slot] = 1;
    zq_job[k] = zq_job[--zq_njob];
    return 1;
}
void rsx_d3d_zpass_poll(void) { if (!dev) return; for (int k = 0; k < zq_njob; ) if (!zq_job_poll(k, 0)) k++; }
static void zq_stop(void) {
    if (zq_cur < 0) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)zq_pool[zq_cur]);
    if (zq_nacc < 32) zq_acc[zq_nacc++] = zq_cur;
    else { zq_acc_done += zq_wait(zq_cur); zq_own[zq_cur] = 0; }
    zq_cur = -1; zq_running = 0;
}
static void zq_start(void) {
    if (zq_cur >= 0 || !dev) return;
    int i = -1;
    for (int tries = 0; i < 0 && tries < 2; tries++) {
        for (int k = 0; k < ZQ_POOL; k++) if (!zq_own[k] && !zq_ref[k]) { i = k; break; }
        if (i < 0 && zq_njob) zq_job_poll(0, 1);   /* all in use: finish the oldest report */
    }
    if (i < 0) return;
    if (!zq_pool[i]) { D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION, 0}; if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &zq_pool[i]))) return; }
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)zq_pool[i]);
    zq_own[i] = 1; zq_cur = i; zq_running = 1;
}
void rsx_d3d_zpass_enable(int on) {
    if (!dev) return;
    if (on) rsx_d3d_zq_enables++;
    zq_enabled = on;
    if (on) zq_start(); else zq_stop();
}
void rsx_d3d_zpass_clear(void) {
    if (!dev) return;
    rsx_d3d_zq_clears++;
    zq_stop();
    for (int m = 0; m < zq_nacc; m++) zq_own[zq_acc[m]] = 0;
    zq_nacc = 0; zq_acc_done = 0;
    if (zq_enabled) zq_start();
}
/* GET_REPORT (type 1) for the report value at guest address <addr>: returns what to write there now */
uint32_t rsx_d3d_zpass_read_at(uint32_t addr) {
    if (!dev) return 0x10000;
    zq_stop();
    static int sync = -1; if (sync < 0) sync = getenv("MNR_D3D_ZPASS_SYNC") != NULL;   /* the old waiting read (A/B) */
    if (zq_njob == 16) zq_job_poll(0, 1);
    int k = zq_njob++;
    zq_job[k].addr = addr; zq_job[k].n = zq_nacc; zq_job[k].sum = zq_acc_done;
    for (int m = 0; m < zq_nacc; m++) { zq_job[k].q[m] = zq_acc[m]; zq_ref[zq_acc[m]]++; }
    uint32_t slot = (addr >> 4) & 63, v;
    int done = zq_job_poll(k, sync || !zq_known[slot]);
    v = zq_last[slot];
    if (!done) v = zq_last[slot];
    if (zq_enabled) zq_start();
    return v;
}
uint32_t rsx_d3d_zpass_read(void) { return 0x10000; }   /* (unused: see rsx_d3d_zpass_read_at) */

int rsx_d3d_clear(uint32_t mask) {
    if (!dev || failed) return 0;
    rt_t rt;
    if (!bind_targets(&rt, (mask & 3) ? 1 : 0)) return 1;   /* a clear writes */
    D3D11_RECT r = {rt.clip_x0, rt.clip_y0, rt.clip_x1, rt.clip_y1};
    int full = rt.clip_x0 <= 0 && rt.clip_y0 <= 0 && (uint32_t)rt.clip_x1 >= rt.w && (uint32_t)rt.clip_y1 >= rt.h;
    if (mask & 0xF0) {
        uint32_t c = REG(0x1D90);
        float col[4] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f};
        /* CLEAR_SURFACE has a bit per channel (0x10 R, 0x20 G, 0x40 B, 0x80 A): the character composite
         * clears only alpha (to make the texture opaque) before copying it out */
        UINT8 wm = (UINT8)((mask & 0x10 ? D3D11_COLOR_WRITE_ENABLE_RED : 0) | (mask & 0x20 ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                           (mask & 0x40 ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | (mask & 0x80 ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
        for (int i = 0; i < rt.ncolor; i++) {
            if (wm != D3D11_COLOR_WRITE_ENABLE_ALL) clear_channels(rt.color[i], col, wm, &r);
            else if (full || !ctx1) ID3D11DeviceContext_ClearRenderTargetView(ctx, rt.color[i]->rtv, col);
            else { D3D11_RECT pr = {(LONG)(r.left * rt.sx), (LONG)(r.top * rt.sy), (LONG)ceilf(r.right * rt.sx), (LONG)ceilf(r.bottom * rt.sy)};
                   ID3D11DeviceContext1_ClearView(ctx1, (ID3D11View *)rt.color[i]->rtv, col, &pr, 1); }
        }
    }
    if ((mask & 3) && rt.depth) {
        uint32_t z = REG(0x1D8C);
        float depth = rt.depth->rsx_fmt == 1 ? (z & 0xFFFF) / 65535.0f : (z >> 8) / 16777215.0f;
        UINT flags = (mask & 1 ? D3D11_CLEAR_DEPTH : 0) | (mask & 2 ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, rt.depth->dsv, flags, depth, (UINT8)(z & 0xFF));
    }
    return 1;
}

/* ------------------------------------------------------------------ presentation */
static ID3D11VertexShader *blit_vs; static ID3D11PixelShader *blit_ps; static ID3D11SamplerState *blit_smp;
static const char blit_vs_src[] =
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO main(uint id : SV_VertexID) { VO o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
static const char blit_ps_src[] =
    "Texture2D t : register(t0); SamplerState s : register(s0); cbuffer C : register(b0) { float4 sc; };\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { float4 c = t.Sample(s, uv * sc.xy); return float4(c.rgb, 1); }\n";
static ID3D11Buffer *blit_cb;
/* MNR_FXAA=1: the picture is smoothed on its way to the window (FXAA: edges found by brightness contrast, blended along
 * them; about 0.2 ms at 1080p). sc.zw: the source's texel size. */
static ID3D11PixelShader *fxaa_ps;
static const char fxaa_ps_src[] =
    "Texture2D t : register(t0); SamplerState s : register(s0); cbuffer C : register(b0) { float4 sc; };\n"
    "float3 S(float2 uv) { return t.SampleLevel(s, uv, 0).rgb; }\n"
    "float L(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float4 main(float4 pos : SV_Position, float2 uv0 : TEXCOORD0) : SV_Target {\n"
    "  float2 uv = uv0 * sc.xy, px = sc.zw;\n"
    "  float3 m = S(uv);\n"
    "  float lnw = L(S(uv - px)), lne = L(S(uv + float2(px.x, -px.y))), lsw = L(S(uv + float2(-px.x, px.y))), lse = L(S(uv + px)), lm = L(m);\n"
    "  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
    "  if (lmax - lmin < max(0.0312, lmax * 0.125)) return float4(m, 1);\n"
    "  float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
    "  float red = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + red), -8, 8) * px;\n"
    "  float3 a = 0.5 * (S(uv + dir * (1.0 / 3.0 - 0.5)) + S(uv + dir * (2.0 / 3.0 - 0.5)));\n"
    "  float3 b = a * 0.5 + 0.25 * (S(uv - dir * 0.5) + S(uv + dir * 0.5));\n"
    "  float lb = L(b);\n"
    "  return float4(lb < lmin || lb > lmax ? a : b, 1);\n"
    "}\n";
/* CPU-drawn images (the loading screens are written straight into the display buffers) into a larger texture: the rows
 * go into a game-size texture, which is drawn stretched with bilinear filtering (nearest made 1.5x uneven and blocky).
 * r.xy keeps the filter inside the uploaded rows (the rest of the game-size texture is stale). */
static ID3D11PixelShader *upsc_ps;
static const char upsc_ps_src[] =
    "Texture2D t : register(t0); SamplerState s : register(s0); cbuffer C : register(b0) { float4 r; };\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.SampleLevel(s, float2(uv.x, clamp(uv.y, r.x, r.y)), 0); }\n";
static int surf_upscale_gpu(surf_t *s, uint32_t y0, uint32_t y1, const void *buf, uint32_t bpp) {
    static struct { ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; uint32_t w, h; DXGI_FORMAT f; } tmp[4]; static int next;
    if (!s->rtv || !blit_vs || !blit_cb || !blit_smp || y1 <= y0) return 0;
    if (!upsc_ps) { size_t len; void *code = compile(upsc_ps_src, "ps_5_0", &len);
        if (!code || FAILED(ID3D11Device_CreatePixelShader(dev, code, len, NULL, &upsc_ps))) { free(code); upsc_ps = NULL; return 0; } free(code); }
    int k = 0; while (k < 4 && !(tmp[k].tex && tmp[k].w == s->w && tmp[k].h == s->h && tmp[k].f == s->dxfmt)) k++;
    if (k == 4) {
        k = next++ & 3;
        if (tmp[k].srv) ID3D11ShaderResourceView_Release(tmp[k].srv);
        if (tmp[k].tex) ID3D11Texture2D_Release(tmp[k].tex);
        tmp[k].tex = NULL; tmp[k].srv = NULL;
        D3D11_TEXTURE2D_DESC d = {s->w, s->h, 1, 1, s->dxfmt, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &tmp[k].tex))) { tmp[k].tex = NULL; return 0; }
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tmp[k].tex, NULL, &tmp[k].srv))) {
            ID3D11Texture2D_Release(tmp[k].tex); tmp[k].tex = NULL; tmp[k].srv = NULL; return 0; }
        tmp[k].w = s->w; tmp[k].h = s->h; tmp[k].f = s->dxfmt;
    }
    D3D11_BOX box = {0, y0, 0, s->w, y1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tmp[k].tex, 0, &box, buf, s->w * bpp, 0);
    ID3D11ShaderResourceView *none[16] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 16, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s->rtv, NULL); targets_invalidate();
    D3D11_VIEWPORT vp = {0, 0, (float)s->pw, (float)s->ph, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11RasterizerState *rs = state_cache_get(0xB117);
    if (!rs) { D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, FALSE, TRUE, FALSE, FALSE};
               ID3D11Device_CreateRasterizerState(dev, &rd, &rs); state_cache_put(0xB117, rs); }
    ID3D11DeviceContext_RSSetState(ctx, rs);
    uint32_t py0 = (uint32_t)((uint64_t)y0 * s->ph / s->h), py1 = y1 == s->h ? s->ph : (uint32_t)((uint64_t)y1 * s->ph / s->h);
    D3D11_RECT sc = {0, (LONG)py0, (LONG)s->pw, (LONG)py1};
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    last_blend = last_depth = last_raster = 0;
    float r[4] = {(y0 + 0.5f) / s->h, (y1 - 0.5f) / s->h, 0, 0};
    upload(blit_cb, r, 16);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, upsc_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &tmp[k].srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &blit_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &blit_cb);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 16, none);
    memset(&bound_sc, 0xFF, sizeof bound_sc);   /* the draw path sets its own scissor again */
    return 1;
}
static int fxaa_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("MNR_FXAA"); on = e && atoi(e) > 0; if (on) printf("[d3d] FXAA on\n"); } return on; }
static ID3D11Texture2D *upl_tex; static ID3D11ShaderResourceView *upl_srv; static uint32_t upl_w, upl_h;

/* ------------------------------------------------------------------ 2D engine (NV3089 scaled image) */
static ID3D11VertexShader *sblit_vs; static ID3D11PixelShader *sblit_ps; static ID3D11Buffer *sblit_cb;
static const char sblit_vs_src[] =
    "cbuffer C : register(b0) { float4 rect; float4 uvr; };   /* rect: x0 y0 x1 y1 in NDC, uvr: u0 v0 u1 v1 */\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO main(uint id : SV_VertexID) { VO o; float2 t = float2(id & 1, id >> 1);\n"
    "  o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1); o.uv = lerp(uvr.xy, uvr.zw, t); return o; }\n";
static const char sblit_ps_src[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }\n";


int rsx_d3d_blit(uint32_t src, uint32_t spitch, uint32_t sfmt, uint32_t sw, uint32_t sh, float u0, float v0, float dsdx, float dtdy,
                 uint32_t dst, uint32_t dpitch, uint32_t dfmt, int ox, int oy, int ow, int oh, int cx, int cy, int cw, int ch, uint32_t interp) {
    if (!dev || failed) return 0;
    surf_t *s = surf_find(src, 0);
    {   static long long ta = -2; static int tn; if (ta == -2) { const char *e = getenv("MNR_D3D_TRACE_ADDR"); ta = e ? (long long)strtoull(e, NULL, 16) : -1; }
        if (ta >= 0 && dst <= (uint32_t)ta && (uint32_t)ta < dst + dpitch * (uint32_t)(oy + oh) && tn < 400) {
            uint64_t sum[4] = {0}; for (uint32_t i = 0; i < 4096; i++) for (int k = 0; k < 4; k++) sum[k] += ld_u8(src + i * 256 + (uint32_t)k);
            printf("[trace]   source memory mean %llu %llu %llu %llu, gen %llu surface gen %llu dirty %d\n", sum[0] / 4096, sum[1] / 4096, sum[2] / 4096, sum[3] / 4096,
                   (unsigned long long)rsx_range_gen_now(src, spitch * sh), s ? (unsigned long long)s->guest_gen : 0ull, s ? s->gpu_dirty : -1);
            for (int i = 0; i < NSURF; i++) { surf_t *o = &surfs[i];
                if (o->tex && src < o->addr + surf_bytes(o) && o->addr < src + spitch * sh)
                    printf("[trace]   overlapping surface %d: 0x%08x %ux%u fmt %u pitch %u depth %d dirty %d%s\n", i, o->addr, o->w, o->h, o->rsx_fmt, o->pitch, o->depth, o->gpu_dirty, o == s ? " (source)" : ""); } }
        if (ta >= 0 && ((dst <= (uint32_t)ta && (uint32_t)ta < dst + dpitch * (uint32_t)(oy + oh)) || (src <= (uint32_t)ta && (uint32_t)ta < src + spitch * sh)) && tn++ < 400)
            printf("[trace] flip %ld blit 0x%08x (%s %ux%u fmt %u) %ux%u pitch %u sfmt %u -> 0x%08x pitch %u dfmt %u out %d,%d %dx%d in %.1f,%.1f ds %.3f dt %.3f\n",
                   rsx_flips, src, s ? "surface" : "memory", s ? s->w : 0, s ? s->h : 0, s ? s->rsx_fmt : 0, sw, sh, spitch, sfmt, dst, dpitch, dfmt, ox, oy, ow, oh, u0, v0, dsdx, dtdy); }
    if (!s || s->depth || sfmt == 7 || dfmt == 4) return 0;   /* only 32-bit colour surfaces on the GPU path */
    { static int cpu = -1; if (cpu < 0) cpu = getenv("MNR_D3D_CPUBLIT") != NULL; if (cpu) return 0; }
    {   /* the CPU or an SPU may have written the source since the GPU last had it (the character
         * composite is built in local memory that way, then blitted): take its contents, as binding does */
        uint64_t g = rsx_range_gen_now(s->addr, surf_bytes(s));
        if (g > s->guest_gen) { surf_load_why = 1; surf_load(s); s->guest_gen = g; s->gpu_dirty = 0; s->dirty_h = 0; }
    }
    (void)spitch; (void)sw; (void)sh;
    if (!sblit_vs) {
        size_t len; void *code;
        code = compile(sblit_vs_src, "vs_5_0", &len); ID3D11Device_CreateVertexShader(dev, code, len, NULL, &sblit_vs); free(code);
        code = compile(sblit_ps_src, "ps_5_0", &len); ID3D11Device_CreatePixelShader(dev, code, len, NULL, &sblit_ps); free(code);
        sblit_cb = make_cb(32);
    }
    uint32_t dw = (uint32_t)(ox + ow > cx + cw ? ox + ow : cx + cw), dh = (uint32_t)(oy + oh > cy + ch ? oy + oh : cy + ch);
    surf_t *d = surf_find(dst, 0);
    if (!d || d->w < dw || d->h < dh) d = surf_get(dst, 0, 8, d && d->w > dw ? d->w : dw, d && d->h > dh ? d->h : dh, dpitch, 1, 1, 0);
    if (!d || d == s) return 0;
    ID3D11ShaderResourceView *none[16] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 16, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &d->rtv, NULL); targets_invalidate();
    D3D11_VIEWPORT vp = {0, 0, (float)d->pw, (float)d->ph, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    float bsx = (float)d->pw / d->w, bsy = (float)d->ph / d->h;
    D3D11_RECT sc = {(LONG)((cx > ox ? cx : ox) * bsx), (LONG)((cy > oy ? cy : oy) * bsy),
                     (LONG)ceilf((cx + cw < ox + ow ? cx + cw : ox + ow) * bsx), (LONG)ceilf((cy + ch < oy + oh ? cy + ch : oy + oh) * bsy)};
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, FALSE, TRUE, FALSE, FALSE};
    ID3D11RasterizerState *rs = state_cache_get(0xB117);
    if (!rs) { ID3D11Device_CreateRasterizerState(dev, &rd, &rs); state_cache_put(0xB117, rs); }
    ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    last_blend = last_depth = last_raster = 0;
    float cbd[8] = {ox / (float)d->w * 2 - 1, 1 - oy / (float)d->h * 2, (ox + ow) / (float)d->w * 2 - 1, 1 - (oy + oh) / (float)d->h * 2,
                    u0 / s->w, v0 / s->h, (u0 + ow * dsdx) / s->w, (v0 + oh * dtdy) / s->h};
    upload(sblit_cb, cbd, sizeof cbd);
    D3D11_SAMPLER_DESC smd = {interp ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, 0};
    uint64_t k = hash64(&smd, sizeof smd, 19);
    ID3D11SamplerState *smp = state_cache_get(k);
    if (!smp) { ID3D11Device_CreateSamplerState(dev, &smd, &smp); state_cache_put(k, smp); }
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, sblit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, sblit_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &sblit_cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
    {   static int on = -1, n; if (on < 0) on = getenv("MNR_D3D_BLITSAVE") != NULL;   /* debug: source and result of each blit */
        if (on && n < 30) { char nm[64]; snprintf(nm, sizeof nm, "blit_%02d_src_%08x.bmp", n, src); save_surface_bmp(s, nm);
                            snprintf(nm, sizeof nm, "blit_%02d_dst_%08x.bmp", n, dst); save_surface_bmp(d, nm); n++; } }
    {   /* rows the blit can reach (output and clip rectangles), not the whole target: see surf_writeback_body */
        int yb = oy + oh < cy + ch ? oy + oh : cy + ch; if (yb < 0) yb = 0; if ((uint32_t)yb > d->h) yb = (int)d->h;
        if ((uint32_t)yb > d->dirty_h) d->dirty_h = (uint32_t)yb;
    }
    surf_drawn(d); d->last_use = ++use_clock;
    return 1;
}
/* a clear of some colour channels only: a quad of the clear colour with those channels writable */
static ID3D11PixelShader *cclr_ps; static ID3D11Buffer *cclr_cb;
static void clear_channels(surf_t *s, const float col[4], UINT8 wmask, const D3D11_RECT *r) {
    if (!wmask) return;
    if (!sblit_vs) {   /* the 2D engine's shaders (same set up as rsx_d3d_blit) */
        size_t len; void *code;
        code = compile(sblit_vs_src, "vs_5_0", &len); ID3D11Device_CreateVertexShader(dev, code, len, NULL, &sblit_vs); free(code);
        code = compile(sblit_ps_src, "ps_5_0", &len); ID3D11Device_CreatePixelShader(dev, code, len, NULL, &sblit_ps); free(code);
        sblit_cb = make_cb(32);
    }
    if (!cclr_ps) {
        static const char src[] = "cbuffer C : register(b0) { float4 col; };\nfloat4 main(float4 pos : SV_Position) : SV_Target { return col; }\n";
        size_t len; void *code = compile(src, "ps_5_0", &len);
        ID3D11Device_CreatePixelShader(dev, code, len, NULL, &cclr_ps); free(code);
        cclr_cb = make_cb(16);
    }
    ID3D11ShaderResourceView *none[16] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 16, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s->rtv, NULL); targets_invalidate();
    D3D11_VIEWPORT vp = {0, 0, (float)s->pw, (float)s->ph, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11RasterizerState *rs = state_cache_get(0xB117);
    if (!rs) { D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, FALSE, TRUE, FALSE, FALSE};
               ID3D11Device_CreateRasterizerState(dev, &rd, &rs); state_cache_put(0xB117, rs); }
    ID3D11DeviceContext_RSSetState(ctx, rs);
    float csx = (float)s->pw / s->w, csy = (float)s->ph / s->h;
    D3D11_RECT sc = {r->left > 0 ? (LONG)(r->left * csx) : 0, r->top > 0 ? (LONG)(r->top * csy) : 0,
                     r->right < (LONG)s->w ? (LONG)ceilf(r->right * csx) : (LONG)s->pw, r->bottom < (LONG)s->h ? (LONG)ceilf(r->bottom * csy) : (LONG)s->ph};
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    ID3D11BlendState *bs = state_cache_get(0xC1EA0000ull | wmask);
    if (!bs) { D3D11_BLEND_DESC bd = {0}; bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
               bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
               bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask = wmask;
               ID3D11Device_CreateBlendState(dev, &bd, &bs); state_cache_put(0xC1EA0000ull | wmask, bs); }
    ID3D11DeviceContext_OMSetBlendState(ctx, bs, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    last_blend = last_depth = last_raster = 0;
    float cbd[8] = {-1, 1, 1, -1, 0, 0, 1, 1};
    upload(sblit_cb, cbd, sizeof cbd); upload(cclr_cb, col, 16);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, sblit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, cclr_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &sblit_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cclr_cb);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
}
void rsx_d3d_reload_range(uint32_t addr, uint32_t size) {
    if (!dev) return;
    for (int i = 0; i < NSURF; i++) {
        surf_t *s = &surfs[i];
        if (s->tex && addr < s->addr + surf_bytes(s) && s->addr < addr + size) {
            surf_load_why = 2; surf_load(s); s->gpu_dirty = 0; s->dirty_h = 0; s->guest_gen = rsx_range_gen_now(s->addr, surf_bytes(s));
        }
    }
}

/* MNR_D3D_SURFSHOT=<flip>: save every live colour surface (RGBA8 ones) as surf_<addr>_<w>x<h>.bmp */
static void save_surface_bmp(surf_t *s, const char *name) {   /* debug: one colour surface to a BMP */
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(s->tex, &d);
    if (d.Format == DXGI_FORMAT_R8_UNORM) {   /* single-channel targets: a grayscale PGM */
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ID3D11Texture2D *st = NULL;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) return;
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            char nm[96]; snprintf(nm, sizeof nm, "%s.pgm", name); FILE *f = fopen(nm, "wb");
            if (f) { fprintf(f, "P5 %u %u 255\n", d.Width, d.Height);
                     for (uint32_t y = 0; y < d.Height; y++) fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 1, d.Width, f); fclose(f); }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
        return;
    }
    if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {   /* half-float targets: raw texels to <name>.f16 (u32 w, u32 h, then w*h*4 halfs) */
        DXGI_FORMAT fmt = d.Format;
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ID3D11Texture2D *st = NULL; (void)fmt;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) return;
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            char nm[96]; snprintf(nm, sizeof nm, "%s.f16", name); FILE *f = fopen(nm, "wb");
            if (f) { uint32_t hdr[2] = {d.Width, d.Height}; fwrite(hdr, 4, 2, f);
                     for (uint32_t y = 0; y < d.Height; y++) fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 8, d.Width, f); fclose(f); }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
        return;
    }
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = fopen(name, "wb");
        if (f) {
            uint32_t w = s->w / 2, h = s->h, sz = w * h * 4;   /* every 2nd column (2x AA storage) */
            BITMAPFILEHEADER fh = {0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + sz, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
            BITMAPINFOHEADER ih = {sizeof ih, (LONG)w, -(LONG)h, 1, 32, BI_RGB};
            fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
            uint8_t *row = malloc(w * 4);
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t *p = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                for (uint32_t x = 0; x < w; x++) { row[4 * x] = p[8 * x + 2]; row[4 * x + 1] = p[8 * x + 1]; row[4 * x + 2] = p[8 * x]; row[4 * x + 3] = 255; }
                fwrite(row, 1, w * 4, f);
            }
            free(row); fclose(f);
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}
static void dump_surfaces(void) {
    for (int i = 0; i < NSURF; i++) {
        surf_t *s = &surfs[i];
        int dz = s->depth && s->rsx_fmt == 2;   /* Z24S8 depth: 24-bit depth as gray, stretched over its range */
        if (!s->tex || (s->depth && !dz) || (!dz && s->dxfmt != DXGI_FORMAT_R8G8B8A8_UNORM && s->dxfmt != DXGI_FORMAT_R8_UNORM && s->dxfmt != DXGI_FORMAT_R16G16B16A16_FLOAT)) continue;
        int r8 = s->dxfmt == DXGI_FORMAT_R8_UNORM, f16 = s->dxfmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(s->tex, &d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ID3D11Texture2D *st = NULL;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) continue;
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            char name[96]; snprintf(name, sizeof name, "surf_%05ld_%08x_%ux%u_fmt%u.bmp", rsx_flips, s->addr, s->w, s->h, s->rsx_fmt);
            FILE *f = fopen(name, "wb");
            if (f) {
                uint32_t sz = s->w * s->h * 4;
                BITMAPFILEHEADER fh = {0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + sz, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
                BITMAPINFOHEADER ih = {sizeof ih, (LONG)s->w, -(LONG)s->h, 1, 32, BI_RGB};
                fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                uint8_t *row = malloc(s->w * 4);
                uint32_t zlo = 0xFFFFFF, zhi = 0;
                if (dz) for (uint32_t y = 0; y < s->h; y++) { const uint32_t *p = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
                    for (uint32_t x = 0; x < s->w; x++) { uint32_t z = p[x] & 0xFFFFFF; if (z < zlo) zlo = z; if (z > zhi) zhi = z; } }
                if (dz) printf("[dump] depth 0x%08x %ux%u: range %06x..%06x\n", s->addr, s->w, s->h, zlo, zhi);
                for (uint32_t y = 0; y < s->h; y++) {
                    const uint8_t *p = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                    for (uint32_t x = 0; x < s->w; x++) {
                        if (dz) { uint32_t z = ((const uint32_t *)p)[x] & 0xFFFFFF; uint8_t g = zhi > zlo ? (uint8_t)((uint64_t)(z - zlo) * 255 / (zhi - zlo)) : 0;
                                  row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = g; row[4 * x + 3] = 255; continue; }
                        if (r8) { row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = p[x]; row[4 * x + 3] = 255; continue; }
                        if (f16) {   /* half floats: R,G,B scaled by MNR_D3D_F16SCALE (default 1/64) into 8 bits */
                            static float sc = 0; if (!sc) { const char *e = getenv("MNR_D3D_F16SCALE"); sc = e ? (float)atof(e) : 1.0f / 64; }
                            for (int k = 0; k < 3; k++) { uint16_t h; memcpy(&h, p + 8 * x + 2 * k, 2); uint32_t e = (h >> 10) & 31, m = h & 1023; float v = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24); if (h & 0x8000) v = -v;
                                float o = v * sc * 255.0f; row[4 * x + 2 - k] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o); }
                            row[4 * x + 3] = 255; continue; }
                        row[4 * x] = p[4 * x + 2]; row[4 * x + 1] = p[4 * x + 1]; row[4 * x + 2] = p[4 * x]; row[4 * x + 3] = 255; }
                    fwrite(row, 1, s->w * 4, f);
                }
                free(row); fclose(f);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
}
/* The on-screen keyboard's text entry (host.c, osk_overlay), drawn over the picture: a panel with the prompt and the
 * text typed so far, rendered with GDI into a texture each frame it is open (a separate window stayed hidden behind
 * the game in fullscreen). */
static ID3D11Texture2D *osk_tex; static ID3D11ShaderResourceView *osk_srv; static UINT osk_tw, osk_th;
static void draw_osk(D3D11_VIEWPORT pic) {
    extern int osk_overlay(WCHAR *title, size_t tcap, WCHAR *text, size_t xcap);
    WCHAR title[256], text[512];
    if (!osk_overlay(title, 256, text, 512)) return;
    UINT w = (UINT)(pic.Width * 0.62f), h = (UINT)(pic.Height * 0.17f);
    if (w < 320) w = 320; if (h < 90) h = 90;
    if (!osk_tex || osk_tw != w || osk_th != h) {
        if (osk_srv) ID3D11ShaderResourceView_Release(osk_srv); if (osk_tex) ID3D11Texture2D_Release(osk_tex);
        osk_srv = NULL; osk_tex = NULL;
        D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &osk_tex))) { osk_tex = NULL; return; }
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)osk_tex, NULL, &osk_srv);
        osk_tw = w; osk_th = h;
    }
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB}};
    void *bits = NULL; HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) { DeleteDC(dc); return; }
    HGDIOBJ oldbm = SelectObject(dc, bm);
    RECT r = {0, 0, (LONG)w, (LONG)h};
    HBRUSH edge = CreateSolidBrush(RGB(214, 40, 40)), fill = CreateSolidBrush(RGB(28, 31, 38)), field = CreateSolidBrush(RGB(12, 13, 16));
    FillRect(dc, &r, edge);
    int bdr = (int)h / 30 + 2; RECT in = {bdr, bdr, (LONG)w - bdr, (LONG)h - bdr}; FillRect(dc, &in, fill);
    int th = (int)h / 6, xh = (int)h / 4, pad = (int)h / 8;
    HFONT ft = CreateFontW(-th, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fx = CreateFontW(-xh, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldf = SelectObject(dc, ft); SetTextColor(dc, RGB(200, 205, 215));
    RECT tr = {pad, pad, (LONG)w - pad, pad + th + 4}; DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT fr = {pad, tr.bottom + pad / 2, (LONG)w - pad, (LONG)h - pad - th}; FillRect(dc, &fr, field);
    SelectObject(dc, fx); SetTextColor(dc, RGB(255, 255, 255));
    WCHAR shown[520]; swprintf(shown, 520, L"%ls%ls", text, (GetTickCount() / 500) & 1 ? L"" : L"|");   /* blinking caret */
    RECT xr = {fr.left + pad / 2, fr.top, fr.right - pad / 2, fr.bottom};
    SIZE sz; GetTextExtentPoint32W(dc, shown, (int)wcslen(shown), &sz);
    DrawTextW(dc, shown, -1, &xr, (sz.cx > xr.right - xr.left ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);   /* long text: its end */
    SelectObject(dc, ft); SetTextColor(dc, RGB(150, 155, 165));
    RECT hr = {pad, (LONG)h - pad - th, (LONG)w - pad, (LONG)h - pad / 2};
    DrawTextW(dc, L"Enter: OK     Esc: cancel     Ctrl+V: paste", -1, &hr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    GdiFlush();
    {   /* debug: MNR_OSK_DUMP=1 - the panel as osk_panel.bmp, every 2 s while open (screenshots show only the game's picture) */
        static DWORD last; DWORD now = GetTickCount();
        if (getenv("MNR_OSK_DUMP") && now - last > 2000) { last = now;
            FILE *f = fopen("osk_panel.bmp", "wb");
            if (f) { BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + w * h * 4), 0, 0, (DWORD)(sizeof fh + sizeof bi.bmiHeader)};
                     fwrite(&fh, sizeof fh, 1, f); fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f); fwrite(bits, 4, (size_t)w * h, f); fclose(f); } } }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)osk_tex, 0, NULL, bits, w * 4, 0);
    SelectObject(dc, oldf); SelectObject(dc, oldbm);
    DeleteObject(ft); DeleteObject(fx); DeleteObject(edge); DeleteObject(fill); DeleteObject(field); DeleteObject(bm); DeleteDC(dc);
    D3D11_VIEWPORT vp = {pic.TopLeftX + (pic.Width - (float)w) / 2, pic.TopLeftY + pic.Height * 0.38f, (float)w, (float)h, 0, 1};
    float sc[4] = {1, 1, 0, 0};
    upload(blit_cb, sc, 16);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &osk_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &blit_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &blit_cb);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}


/* The PC build's own indicator, top right over the picture: the voice chat's push-to-talk key (Talking / no
 * microphone / voice chat not running) while it is held, and the game's volume for a moment after - or = changes it.
 * Drawn with GDI into a texture, like the text entry above. */
static ID3D11Texture2D *hud_tex; static ID3D11ShaderResourceView *hud_srv; static UINT hud_tw, hud_th;
static void draw_hud(D3D11_VIEWPORT pic) {
    extern volatile long voice_talking, voice_mic_state, voice_active, voice_send_state;
    extern int voice_ptt_key(void), voice_ptt_mode(void), audio_volume_pct(void), mnr_key_down(int vk);
    extern volatile unsigned long audio_volume_changed;
    DWORD now = GetTickCount(), vc = audio_volume_changed;
    int show_vol = vc && now - vc < 1500;
    extern volatile DWORD voice_mic_changed; extern volatile float voice_mic_level; extern const char *voice_mic_name(void);
    DWORD mc = voice_mic_changed; int show_mic = mc && now - mc < 2500;
    int held = voice_ptt_mode() ? mnr_key_down(voice_ptt_key()) : (int)voice_talking;
    extern int np_notices(wchar_t out[][96], int max);   /* friend notices (host.c): requests, new friends, online, invites */
    WCHAR notes[3][96]; int nnotes = np_notices(notes, 3);
    if (!show_vol && !held && !show_mic && !nnotes) return;
    WCHAR talk[64] = L""; COLORREF dot = RGB(230, 50, 50);
    if (held) {
        if (!voice_active) { wcscpy(talk, L"Voice chat not running"); dot = RGB(120, 120, 130); }
        else if (voice_mic_state < 0) { wcscpy(talk, L"No microphone found"); dot = RGB(230, 160, 40); }
        else if (voice_send_state < 0) { extern volatile long voice_codec_missing;
            wcscpy(talk, voice_codec_missing ? L"Needs PS3UPDAT.PUP in MNR folder" : L"Talking (voice can't be sent)"); dot = RGB(230, 160, 40); }
        else wcscpy(talk, L"Talking");
    }
    WCHAR micl[96]; swprintf(micl, 96, L"Mic: %hs", voice_mic_name());
    int show_micl = show_mic || (held && voice_active && voice_mic_state > 0);
    int lines = (held ? 1 : 0) + (show_micl ? 1 : 0) + (show_vol ? 1 : 0) + nnotes;
    UINT lh = (UINT)(pic.Height * 0.045f); if (lh < 22) lh = 22;
    UINT w = lh * (nnotes ? 14 : 9), h = lh * lines + lh / 2;
    if (!hud_tex || hud_tw != w || hud_th != h) {
        if (hud_srv) ID3D11ShaderResourceView_Release(hud_srv); if (hud_tex) ID3D11Texture2D_Release(hud_tex);
        hud_srv = NULL; hud_tex = NULL;
        D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &hud_tex))) { hud_tex = NULL; return; }
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)hud_tex, NULL, &hud_srv);
        hud_tw = w; hud_th = h;
    }
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB}};
    void *bits = NULL; HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) { DeleteDC(dc); return; }
    HGDIOBJ oldbm = SelectObject(dc, bm);
    RECT r = {0, 0, (LONG)w, (LONG)h};
    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28)); FillRect(dc, &r, bg); DeleteObject(bg);
    HFONT ft = CreateFontW(-(int)(lh * 0.62f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, ft); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(235, 238, 245));
    int y = (int)lh / 4, pad = (int)lh / 3;
    if (held) {
        int d = (int)lh / 2; HBRUSH b = CreateSolidBrush(dot); HGDIOBJ ob = SelectObject(dc, b); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, pad, y + ((int)lh - d) / 2, pad + d, y + ((int)lh - d) / 2 + d);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(b);
        RECT tr = {pad * 2 + d, y, (LONG)(w * 0.6f), y + (LONG)lh}; DrawTextW(dc, talk, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (voice_active && voice_mic_state > 0) {   /* the microphone's level: silent here = wrong device (F8) */
            int x0 = (int)(w * 0.62f), x1 = (int)w - pad, bh = (int)lh / 3, by = y + ((int)lh - bh) / 2;
            float lv = voice_mic_level; int fill = (int)((x1 - x0) * (lv > 0.0001f ? (1.0f + log10f(lv) / 3.0f) : 0));   /* -60..0 dB */
            if (fill < 0) fill = 0; if (fill > x1 - x0) fill = x1 - x0;
            HBRUSH bgb = CreateSolidBrush(RGB(60, 64, 74)), fb = CreateSolidBrush(lv > 0.9f ? RGB(230, 80, 60) : RGB(90, 200, 120));
            RECT br = {x0, by, x1, by + bh}; FillRect(dc, &br, bgb); RECT fr = {x0, by, x0 + fill, by + bh}; FillRect(dc, &fr, fb);
            DeleteObject(bgb); DeleteObject(fb);
        }
        y += (int)lh;
    }
    if (show_micl) {
        SetTextColor(dc, RGB(170, 175, 190));
        RECT tr = {pad, y, (LONG)w - pad, y + (LONG)lh}; DrawTextW(dc, micl, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, RGB(235, 238, 245));
        y += (int)lh;
    }
    if (show_vol) {
        int v = audio_volume_pct(); WCHAR t[32]; swprintf(t, 32, L"Volume %d%%", v);
        RECT tr = {pad, y, (LONG)(w * 0.48f), y + (LONG)lh}; DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        int x0 = (int)(w * 0.5f), x1 = (int)w - pad, seg = (x1 - x0) / 10, bh = (int)lh / 3, by = y + ((int)lh - bh) / 2;
        HBRUSH on = CreateSolidBrush(RGB(90, 200, 120)), off = CreateSolidBrush(RGB(60, 64, 74));
        for (int i = 0; i < 10; i++) { RECT sr = {x0 + i * seg, by, x0 + i * seg + seg - 2, by + bh}; FillRect(dc, &sr, i < v / 10 ? on : off); }
        DeleteObject(on); DeleteObject(off);
        y += (int)lh;
    }
    for (int i = 0; i < nnotes; i++) {   /* friend notices: a blue dot and the text */
        int d = (int)lh / 3; HBRUSH b = CreateSolidBrush(RGB(70, 150, 240)); HGDIOBJ ob = SelectObject(dc, b); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, pad, y + ((int)lh - d) / 2, pad + d, y + ((int)lh - d) / 2 + d);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(b);
        RECT tr = {pad * 2 + d, y, (LONG)w - pad, y + (LONG)lh}; DrawTextW(dc, notes[i], -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += (int)lh;
    }
    GdiFlush();
    for (UINT i = 0; i < w * h; i++) ((uint8_t *)bits)[4 * i + 3] = 255;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)hud_tex, 0, NULL, bits, w * 4, 0);
    SelectObject(dc, oldf); SelectObject(dc, oldbm); DeleteObject(ft); DeleteObject(bm); DeleteDC(dc);
    D3D11_VIEWPORT vp = {pic.TopLeftX + pic.Width - (float)w - pic.Height * 0.03f, pic.TopLeftY + pic.Height * 0.03f, (float)w, (float)h, 0, 1};
    float sc[4] = {1, 1, 0, 0};
    upload(blit_cb, sc, 16);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &hud_srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &blit_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &blit_cb);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}

/* GPU time per frame: a disjoint query + a timestamp at each flip, read back 4 frames later */
double rsx_d3d_gpu_ms, rsx_d3d_present_ms; long rsx_d3d_gpu_frames;
static void gpu_timing_flip(void) {
    enum { N = 6 };
    static ID3D11Query *dj[N], *ts[N]; static int open = -1, have[N]; static long frame;
    if (!ts[0]) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt = {D3D11_QUERY_TIMESTAMP, 0};
        for (int i = 0; i < N; i++) { ID3D11Device_CreateQuery(dev, &qd, &dj[i]); ID3D11Device_CreateQuery(dev, &qt, &ts[i]); }
    }
    int cur = (int)(frame % N);
    if (open >= 0) ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)dj[open]);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)dj[cur]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)ts[cur]);
    have[cur] = 1; open = cur;
    /* the frame between timestamp k and k+1 (both under disjoint k for the frequency) */
    int k = (int)((frame + 2) % N), k1 = (k + 1) % N;
    if (frame >= N && have[k] && have[k1]) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d; UINT64 t0, t1;
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)dj[k], &d, sizeof d, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)ts[k], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)ts[k1], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !d.Disjoint && t1 > t0) {
            rsx_d3d_gpu_ms += (double)(t1 - t0) * 1000.0 / (double)d.Frequency; rsx_d3d_gpu_frames++;
        }
    }
    frame++;
}
/* MNR_BBSHOT=<flip>[,<flip>...]: the window's picture as presented (bb_<flip>.bmp; debug, shows resolution and FXAA) */
static void bb_shot(ID3D11Texture2D *surf) {   /* MNR_BBSHOT_SURF=1: the displayed surface at its own size instead */
    const char *e = getenv("MNR_BBSHOT"); int want = 0;
    for (; e && *e; ) { char *end; long v = strtol(e, &end, 10); if (end == e) break; if (v == rsx_flips) want = 1; e = *end == ',' ? end + 1 : end; }
    if (!want) return;
    ID3D11Texture2D *bb = NULL;
    if (surf && getenv("MNR_BBSHOT_SURF")) { bb = surf; ID3D11Texture2D_AddRef(bb); } else IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    if (!bb) return;
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(bb, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            char nm[64]; snprintf(nm, sizeof nm, "bb_%ld.bmp", rsx_flips); FILE *f = fopen(nm, "wb");
            if (f) {
                uint32_t sz = d.Width * d.Height * 4;
                BITMAPFILEHEADER fh = {0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + sz, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
                BITMAPINFOHEADER ih = {sizeof ih, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB};
                fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                uint8_t *row = malloc(d.Width * 4);
                for (uint32_t y = 0; y < d.Height; y++) { const uint8_t *q = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                    for (uint32_t x = 0; x < d.Width; x++) { row[4 * x] = q[4 * x + 2]; row[4 * x + 1] = q[4 * x + 1]; row[4 * x + 2] = q[4 * x]; row[4 * x + 3] = 255; }
                    fwrite(row, 1, d.Width * 4, f); }
                free(row); fclose(f);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Texture2D_Release(bb);
}
void rsx_d3d_present(uint32_t buffer) {
    rsx_d3d_zpass_poll();   /* finished occlusion counts into their reports */
    if (!dev || failed) return;
    { const char *e = ENVSTR("MNR_D3D_SURFSHOT"); if (e && atol(e) && rsx_flips % atol(e) == 0 && rsx_flips > 2600) dump_surfaces(); }   /* every <n> flips once in the race (debug) */
    rsx_d3d_sync();
    rsx_display_t d = rsx_display[buffer & 7];
    uint32_t addr = RSX_LOCAL_BASE + d.offset;
    surf_t *s = surf_find(addr, 0);
    ID3D11ShaderResourceView *srv = NULL;
    float sc[4] = {1, 1, 0, 0};
    if (s) { srv = s->srv; sc[0] = (float)d.width * s->aa_x / s->w; sc[1] = (float)d.height * s->aa_y / s->h; sc[2] = 1.0f / s->pw; sc[3] = 1.0f / s->ph; }
    if (s) {   /* a GPU surface: shown as is (below); only a display buffer the CPU wrote gets uploaded */
      if (s->dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM && ENVSTR("MNR_D3D_DARKDUMP")) {   /* debug: a frame much darker than the last one dumps every surface (and the next frame's) */
        static ID3D11Texture2D *row; extern void spu_occ_dump(long); static float prev = 255; static int events, dump_next;
        if (!row) { D3D11_TEXTURE2D_DESC td = {s->w, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0}; ID3D11Device_CreateTexture2D(dev, &td, NULL, &row); }
        D3D11_BOX b = {0, s->h * 2 / 3, 0, s->w, s->h * 2 / 3 + 1, 1};
        if (row) {
            ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)row, 0, 0, 0, 0, (ID3D11Resource *)s->tex, 0, &b);
            D3D11_MAPPED_SUBRESOURCE m; float mean = 0;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)row, 0, D3D11_MAP_READ, 0, &m))) {
                const uint8_t *p = m.pData; uint64_t sum = 0; for (uint32_t x = 0; x < s->w; x++) sum += p[4 * x] + p[4 * x + 1] + p[4 * x + 2];
                mean = sum / (3.0f * s->w); ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)row, 0);
            }
            if (dump_next) { printf("[darkdump] flip %ld: next frame (mean %.1f)\n", rsx_flips, mean); dump_surfaces(); dump_next = 0; }
            else if (mean < prev * 0.25f && prev > 30 && ENVSTR("MNR_D3D_DARKLOG")) printf("[darkdump] flip %ld: dark frame (mean %.1f, previous %.1f), not dumped\n", rsx_flips, mean, prev), spu_occ_dump(rsx_flips);
            if (0) ;
            else if (mean < prev * 0.25f && prev > 30 && events < 3 && rsx_flips >= atol(getenv("MNR_D3D_DARKDUMP"))) { events++; printf("[darkdump] flip %ld: dark frame (mean %.1f, previous %.1f)\n", rsx_flips, mean, prev); dump_surfaces(); dump_next = 1; }
            prev = mean;
        }
      }
    }
    else if (d.width && d.height) {   /* CPU-written display buffer: upload */
        if (upl_w != d.width || upl_h != d.height) {
            if (upl_tex) { ID3D11ShaderResourceView_Release(upl_srv); ID3D11Texture2D_Release(upl_tex); }
            D3D11_TEXTURE2D_DESC td = {d.width, d.height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
            ID3D11Device_CreateTexture2D(dev, &td, NULL, &upl_tex);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)upl_tex, NULL, &upl_srv);
            upl_w = d.width; upl_h = d.height;
        }
        uint8_t *buf = malloc((size_t)d.width * d.height * 4);
        for (uint32_t y = 0; y < d.height; y++) for (uint32_t x = 0; x < d.width; x++) {
            uint32_t a = addr + y * d.pitch + x * 4; uint8_t *o = buf + 4 * (y * d.width + x);
            o[0] = ld_u8(a + 1); o[1] = ld_u8(a + 2); o[2] = ld_u8(a + 3); o[3] = 255;
        }
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)upl_tex, 0, NULL, buf, d.width * 4, 0);
        free(buf);
        srv = upl_srv; sc[2] = 1.0f / upl_w; sc[3] = 1.0f / upl_h;
    }
    RECT cr; GetClientRect(hwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1280, ch = cr.bottom > 0 ? (UINT)cr.bottom : 720;
    {   /* the swap chain's buffers follow the window (fullscreen, resizing): a 1280x720 buffer drawn at the window's
         * size showed only its top-left corner */
        static UINT bw = 1280, bh = 720;
        if ((cw != bw || ch != bh) && !IsIconic(hwnd)) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
            if (back_rtv) { ID3D11RenderTargetView_Release(back_rtv); back_rtv = NULL; }
            if (SUCCEEDED(IDXGISwapChain_ResizeBuffers(swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) { bw = cw; bh = ch; }
            ID3D11Texture2D *bb = NULL;
            IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &back_rtv);
            ID3D11Texture2D_Release(bb);
        }
        cw = bw; ch = bh;
    }
    /* the 16:9 picture as large as fits, centred (black bars on other shapes of screen) */
    float vw = (float)cw, vh = (float)ch;
    if (vw * 9 > vh * 16) vw = vh * 16 / 9; else vh = vw * 9 / 16;
    D3D11_VIEWPORT vp = {((float)cw - vw) / 2, ((float)ch - vh) / 2, vw, vh, 0, 1};
    ID3D11RenderTargetView *rtv = back_rtv;
    ID3D11ShaderResourceView *none[16] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 16, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL); targets_invalidate();
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    last_blend = last_depth = last_raster = 0;
    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, back_rtv, black);
    if (srv) {
        upload(blit_cb, sc, 16);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
        if (fxaa_on() && !fxaa_ps) { size_t len; void *code = compile(fxaa_ps_src, "ps_5_0", &len);
            if (!code || FAILED(ID3D11Device_CreatePixelShader(dev, code, len, NULL, &fxaa_ps))) fxaa_ps = NULL; free(code); }
        ID3D11DeviceContext_PSSetShader(ctx, fxaa_on() && fxaa_ps ? fxaa_ps : blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &blit_smp);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &blit_cb);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
    }
    draw_osk(vp);
    draw_hud(vp);
    bb_shot(s ? s->tex : NULL);
    {   LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        IDXGISwapChain_Present(swap, 0, 0);
        QueryPerformanceCounter(&b); rsx_d3d_present_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; }
    gpu_timing_flip();
}

/* screenshot of the presented frame (for MNR_SHOT in D3D mode): reads the display surface back */
int rsx_d3d_read_display(uint32_t buffer, uint32_t *bgrx, uint32_t w, uint32_t h) {
    if (!dev) return 0;
    rsx_display_t d = rsx_display[buffer & 7];
    surf_t *s = surf_find(RSX_LOCAL_BASE + d.offset, 0);
    if (!s) return 0;
    s->gpu_dirty = 1; s->dirty_h = s->h; wb_why = 9; surf_writeback(s);   /* bring guest memory up to date, then the caller reads it */
    (void)bgrx; (void)w; (void)h;
    return 1;
}

/* ------------------------------------------------------------------ setup */
int rsx_d3d_init(HWND window) {
    hwnd = window;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferDesc.Width = 1280; sd.BufferDesc.Height = 720; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.OutputWindow = window; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
    if (FAILED(hr)) {   /* no usable GPU: say so instead of running on without a picture (the launcher explains exit code 3) */
        printf("[d3d] device creation failed (0x%08lx)\n", (unsigned long)hr); fflush(stdout);
        char msg[256];
        snprintf(msg, sizeof msg, "Direct3D 11 couldn't start (error 0x%08lX).\n\nThe game needs a graphics card and driver with Direct3D 11 (feature level 11.0). Updating the graphics driver may help.", (unsigned long)hr);
        MessageBoxA(NULL, msg, "ModNation Racers", MB_OK | MB_ICONERROR);
        ExitProcess(3);
    }
    {   /* DXGI's Alt+Enter went to exclusive fullscreen at the monitor's mode with our 1280x720 buffers: only a corner
         * showed. The window procedure (present.c) does borderless fullscreen instead. */
        IDXGIFactory *f = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(swap, &IID_IDXGIFactory, (void **)&f))) {
            IDXGIFactory_MakeWindowAssociation(f, window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            IDXGIFactory_Release(f);
        }
    }
    ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&ctx1);
    lazy_init();
    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &back_rtv);
    ID3D11Texture2D_Release(bb);
    cb_vpc = make_cb(468 * 16); cb_draw = make_cb(sizeof(draw_cb_t)); cb_fpc = make_cb(FP_MAX_INSNS * 16); cb_fpd = make_cb(sizeof(fpd_cb_t)); blit_cb = make_cb(16);
    src_buf = malloc(1 << 20);
    size_t len; void *code;
    code = compile(blit_vs_src, "vs_5_0", &len); ID3D11Device_CreateVertexShader(dev, code, len, NULL, &blit_vs); free(code);
    code = compile(blit_ps_src, "ps_5_0", &len); ID3D11Device_CreatePixelShader(dev, code, len, NULL, &blit_ps); free(code);
    D3D11_SAMPLER_DESC smd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
    ID3D11Device_CreateSamplerState(dev, &smd, &blit_smp);
    printf("[d3d] Direct3D 11 backend ready (feature level 0x%x)\n", (unsigned)got);
    {   /* which GPU, for the [perf] lines */
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC ds;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &ds)))
            printf("[sys] GPU %ls, %u MB video memory\n", ds.Description, (unsigned)(ds.DedicatedVideoMemory >> 20));
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
    }
    return 0;
}
