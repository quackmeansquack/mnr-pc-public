/* MNR_HLSL_TEST=1: while the software renderer draws, translate every distinct vertex/fragment program
 * to HLSL and compile it with D3DCompile (d3dcompiler_47.dll, loaded at runtime). Failures are counted
 * and their source is written to hlsl_fail_<n>.txt; a summary line is printed every 200 new programs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "rsx_hlsl.h"

typedef HRESULT (WINAPI *d3dcompile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                       UINT, UINT, void **, void **);
typedef struct { void **vtbl; } blob_t;   /* ID3DBlob: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
static d3dcompile_t compile_fn;
static uint64_t seen[65536]; static int nseen;
static int ok_vs, ok_ps, fail_vs, fail_ps, unsup_vs, unsup_ps, nfail_files;

static uint64_t hash64(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static int already(uint64_t h) {
    for (int i = 0; i < nseen; i++) if (seen[i] == h) return 1;
    if (nseen < 65536) seen[nseen++] = h;
    return 0;
}
static int try_compile(const char *src, const char *profile) {
    if (!compile_fn) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (m) compile_fn = (d3dcompile_t)GetProcAddress(m, "D3DCompile");
        if (!compile_fn) return -1;
    }
    blob_t *code = NULL, *err = NULL;
    HRESULT hr = compile_fn(src, strlen(src), "rsx", NULL, NULL, "main", profile, 1 << 11 /* OPTIMIZATION_LEVEL1 */, 0, (void **)&code, (void **)&err);
    if (FAILED(hr)) {
        char name[64]; snprintf(name, sizeof name, "hlsl_fail_%d.txt", nfail_files++);
        FILE *f = fopen(name, "w");
        if (f) {
            fputs(src, f);
            if (err) { const char *msg = ((const char *(WINAPI *)(void *))err->vtbl[3])(err); fprintf(f, "\n/* ---- errors ----\n%s\n*/\n", msg); }
            fclose(f);
        }
    }
    if (code) ((ULONG (WINAPI *)(void *))code->vtbl[2])(code);
    if (err) ((ULONG (WINAPI *)(void *))err->vtbl[2])(err);
    return FAILED(hr) ? 1 : 0;
}

void rsx_hlsl_test(const uint32_t (*vp)[4], uint32_t vp_start, const fp_insn_t *fp, int nfp, uint32_t ctl, const uint8_t dims[16]) {
    static char *buf;
    if (!buf) buf = malloc(1 << 20);
    uint64_t hv = hash64(&vp_start, 4, hash64(vp[vp_start], 4 * 4 * (512 - vp_start), 1469598103934665603ull) ^ 1);
    if (!already(hv)) {
        uint32_t used;
        int r = hlsl_vp(vp, vp_start, buf, 1 << 20, &used);
        if (r == -1) unsup_vs++;
        else if (r == 0) { if (try_compile(buf, "vs_5_0")) fail_vs++; else ok_vs++; }
    }
    uint64_t hf = 1469598103934665603ull ^ 2;
    for (int i = 0; i < nfp; i++) { fp_insn_t t = fp[i]; memset(t.constant, 0, sizeof t.constant); hf = hash64(&t, sizeof t, hf); }
    hf = hash64(&ctl, 4, hash64(dims, 16, hf));
    if (!already(hf)) {
        int nc;
        int r = hlsl_fp(fp, nfp, ctl, dims, buf, 1 << 20, &nc);
        if (r == -1) unsup_ps++;
        else if (r == 0) { if (try_compile(buf, "ps_5_0")) fail_ps++; else ok_ps++; }
    }
    static int last;
    int total = ok_vs + ok_ps + fail_vs + fail_ps + unsup_vs + unsup_ps;
    if (total / 50 != last / 50) {
        last = total;
        printf("[hlsl] VS ok %d fail %d unsupported %d | PS ok %d fail %d unsupported %d\n", ok_vs, fail_vs, unsup_vs, ok_ps, fail_ps, unsup_ps);
    }
}
