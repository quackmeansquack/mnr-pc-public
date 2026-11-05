/* RSX (NV47-class "Curie" 3D engine, class NV4097) as seen through the GCM command stream.
 *
 * The command processor in host.c hands every method here. Method arguments are kept in a register
 * file per subchannel; triggers (draws, clears, program/constant uploads, blits, flips) act on that
 * state. This is the base for a software renderer: the frame log below shows what the game issues.
 *
 * MNR_RSX_LOG=1 prints a one-line summary for every 60th frame. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "runtime.h"
#include "host.h"
#include "rsx.h"
#include "rsx_shader.h"
#include "rsx_d3d.h"
#include "spu.h"
/* a debug switch read once (getenv is far too slow for per-draw checks) */
#define ENVFLAG(name) ({ static int v_ = -1; if (v_ < 0) v_ = getenv(name) != NULL; v_; })
uint32_t rsx_vpc_gen;

#define NREG (0x10000 / 4)
static uint32_t reg[8][NREG];   /* last argument written to each method, per subchannel */
#define R3D(m) (reg[0][(m) >> 2])

/* vertex program memory: 512 instructions x 4 words; 468 constants x 4 floats (as raw words) */
uint32_t rsx_vp[512][4];
uint32_t rsx_vpc[468][4];
static void cam_track(void);
static uint32_t vp_load, vp_word, vpc_load, vpc_word;

rsx_display_t rsx_display[8];
volatile long rsx_flips;

static struct {
    unsigned draws, indexed, verts, clears, fp_changes, vp_uploads, blits, inline_draws;
    uint32_t last_fp;
    uint32_t last_log_flip;
} fs;
static int log_on = -1;
uint32_t rsx_sc_hist[8][0x800];

static const char *prim_name(unsigned p) {
    static const char *n[] = {"?", "points", "lines", "lineloop", "linestrip", "tris", "tristrip", "trifan", "quads", "quadstrip", "polygon"};
    return p < 11 ? n[p] : "?";
}

static void frame_log(uint32_t buffer) {
    if (log_on < 0) { const char *e = getenv("MNR_RSX_LOG"); log_on = e && *e == '1'; }
    if (!log_on) return;
    static LARGE_INTEGER last; LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    if (rsx_flips % 60 != 1 && !(last.QuadPart && now.QuadPart - last.QuadPart > 5 * f.QuadPart)) return;   /* every 60 frames, or 5 s when slow */
    long frames = rsx_flips - (long)fs.last_log_flip; fs.last_log_flip = (uint32_t)rsx_flips;
    double dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / f.QuadPart : 0;
    if (dt > 0) printf("[rsx] %.2f fps over the last %ld frames, %.0f%% of the time in draws, %llu writebacks (%llu kpx)\n", frames / dt, frames, 100 * rsx_draw_seconds / dt,
                       (unsigned long long)rsx_d3d_wb_count, (unsigned long long)rsx_d3d_wb_pixels / 1000);
    rsx_d3d_wb_count = rsx_d3d_wb_pixels = 0;
    {
      static const char *why[10] = {"other", "unprotect", "lazy fault", "re-created", "evicted", "frame sync", "range flush", "as texture", "as vertices", "screenshot"};
      { if (rsx_d3d_wb_why[6]) printf("[rsx]   last range flush: surface 0x%08x %ux%u fmt %u (from %u)\n", rsx_d3d_last_flush[0], rsx_d3d_last_flush[1], rsx_d3d_last_flush[2], rsx_d3d_last_flush[3], rsx_d3d_last_flush[4]); }
      if (rsx_d3d_wb_s > 0) { printf("[rsx]   writebacks took %.2f s:", rsx_d3d_wb_s);
          for (int k = 0; k < 10; k++) if (rsx_d3d_wb_why[k]) printf(" %s %llu (%llu kpx)", why[k], (unsigned long long)rsx_d3d_wb_why[k], (unsigned long long)rsx_d3d_wb_why_px[k] / 1000);
          printf("\n"); }
      memset(rsx_d3d_wb_why, 0, sizeof rsx_d3d_wb_why); memset(rsx_d3d_wb_why_px, 0, sizeof rsx_d3d_wb_why_px); rsx_d3d_wb_s = 0; }
    {
      printf("[rsx]   surface loads: %llu new, %llu CPU-modified, %llu copy targets (%llu kpx)\n", (unsigned long long)rsx_d3d_ld_count[0],
             (unsigned long long)rsx_d3d_ld_count[1], (unsigned long long)rsx_d3d_ld_count[2], (unsigned long long)rsx_d3d_ld_pixels / 1000);
      memset(rsx_d3d_ld_count, 0, sizeof rsx_d3d_ld_count); rsx_d3d_ld_pixels = 0; }
    {
      if (rsx_d3d_gpu_frames) printf("[rsx]   GPU: %.1f ms per frame (flip to flip, %ld frames measured)\n", rsx_d3d_gpu_ms / rsx_d3d_gpu_frames, rsx_d3d_gpu_frames);
      rsx_d3d_gpu_ms = 0; rsx_d3d_gpu_frames = 0; }
    {
      printf("[rsx]   page tracking: %ld direct range checks (%.2f s), frame scans %.2f s\n", rsx_rg_now_calls, rsx_rg_now_s, rsx_ww_scan_s);
      {
        for (int i = 0; i < 16 && rsx_rg_callers[i]; i++) if (rsx_rg_caller_n[i] > 100) printf("[rsx]     from 0x%llx: %ld\n", (unsigned long long)((char *)rsx_rg_callers[i] - (char *)GetModuleHandleA(NULL)) + 0x140000000ull, rsx_rg_caller_n[i]);
        memset(rsx_rg_caller_n, 0, sizeof rsx_rg_caller_n); }
      rsx_rg_now_calls = 0; rsx_rg_now_s = 0; rsx_ww_scan_s = 0; }
    {
      if (rsx_d3d_skip_vs || rsx_d3d_skip_ps) printf("[rsx]   d3d: skipped draws: %ld (vertex program), %ld (fragment program)\n", rsx_d3d_skip_vs, rsx_d3d_skip_ps);
      rsx_d3d_skip_vs = rsx_d3d_skip_ps = 0;
      if (rsx_d3d_compiles || rsx_d3d_uploads)
          printf("[rsx]   d3d: %ld shader compiles (%.1f s), %ld texture uploads (%.1f s)\n", rsx_d3d_compiles, rsx_d3d_compile_s, rsx_d3d_uploads, rsx_d3d_upload_s);
      rsx_d3d_compiles = rsx_d3d_uploads = 0; rsx_d3d_compile_s = rsx_d3d_upload_s = 0; }
    {
        for (int sc = 1; sc < 8; sc++) for (int m = 0; m < 0x800; m++)
            if (rsx_sc_hist[sc][m]) { printf("[rsx]   subchannel %d method 0x%04x x%u (last arg 0x%08x)\n", sc, m * 4, rsx_sc_hist[sc][m], reg[sc][m]); rsx_sc_hist[sc][m] = 0; }
    }
    { if (rsx_d3d_vbytes) printf("[rsx]   vertex data: %.1f MB staged, %.1f MB of it in spans of 16 KB+\n", rsx_d3d_vbytes / 1048576.0, rsx_d3d_vbig / 1048576.0); rsx_d3d_vbytes = rsx_d3d_vbig = 0; }
    {
      if (spu_steps) printf("[rsx]   spu: %.0f%% of the time, %.1fM instructions\n", 100 * spu_seconds / dt, spu_steps / 1e6);
      spu_seconds = 0; spu_steps = 0; }
    rsx_draw_seconds = 0; last = now;
    uint32_t fmt = R3D(0x0208), target = R3D(0x0220);
    printf("[rsx] frame %ld -> buffer %u: %u draws (%u indexed, %u inline), %u verts, %u clears, %u FP changes, "
           "%u VP uploads, %u blits; surface fmt 0x%x target 0x%x color A 0x%08x pitch %u, %ux%u\n",
           rsx_flips, buffer, fs.draws, fs.indexed, fs.inline_draws, fs.verts, fs.clears, fs.fp_changes,
           fs.vp_uploads, fs.blits, fmt, target, R3D(0x0210), R3D(0x020C) & 0xFFFF,
           R3D(0x0200) >> 16, R3D(0x0204) >> 16);
    float sc[4], of[4];
    for (int k = 0; k < 4; k++) { uint32_t a = R3D(0x0A30 + 4 * k), b = R3D(0x0A20 + 4 * k); memcpy(&sc[k], &a, 4); memcpy(&of[k], &b, 4); }
    printf("[rsx]   shader window 0x%x, viewport scale (%g %g %g) offset (%g %g %g), viewport h 0x%x v 0x%x\n",
           R3D(0x1D88), sc[0], sc[1], sc[2], of[0], of[1], of[2], R3D(0x0A00), R3D(0x0A04));
}

/* NV0039 memory-to-memory copy (subchannel bound to class 0x3062/0x0039): offsets are in the DMA
 * spaces chosen by SET_CONTEXT_DMA_BUFFER_IN/OUT; local memory or main memory (IO). */
static uint32_t rsx_addr(uint32_t offset, uint32_t dma) {
    /* CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER = 0xFEED0000 (local), ..._HOST_BUFFER = 0xFEED0001 (main) */
    if ((dma & 0xF) == 0 || dma == 0xFEED0000) return RSX_LOCAL_BASE + offset;
    return gcm_io_to_ea_pub(offset);
}

/* NV3089 scaled image (subchannel 6) writing into the NV3062 2D surface (subchannel 3), as bound by
 * libgcm. Used for the AA resolve: a 2560x720 2-sample surface scaled by 2 into 1280x720.
 *   NV3062: 0x184/0x188 DMA src/dst, 0x300 format (4 R5G6B5, 0xA A8R8G8B8, 0xB Y32), 0x304 pitch dst<<16|src,
 *           0x30C destination offset
 *   NV3089: 0x184 source DMA, 0x300 source format (3/4 A8R8G8B8/X8R8G8B8, 7 R5G6B5), 0x308/0x30C clip point/size,
 *           0x310/0x314 out point/size, 0x318/0x31C ds/dx dt/dy (12.20), 0x400 in size, 0x404 in format
 *           (pitch | origin << 16 | interpolator << 24), 0x408 in offset, 0x40C in point (12.4) = trigger */
static void nv3089_blit(void) {
    uint32_t *s = reg[6], *d = reg[3];
    uint32_t in_size = s[0x400 >> 2], in_fmt = s[0x404 >> 2], in_pt = s[0x40C >> 2];
    uint32_t sw = in_size & 0xFFFF, sh = in_size >> 16, spitch = in_fmt & 0xFFFF, interp = (in_fmt >> 24) & 0xFF;
    uint32_t src = rsx_addr(s[0x408 >> 2], s[0x184 >> 2]);
    uint32_t dst = rsx_addr(d[0x30C >> 2], d[0x188 >> 2]), dpitch = d[0x304 >> 2] >> 16, dfmt = d[0x300 >> 2];
    uint32_t sfmt = s[0x300 >> 2];
    int ox = (int16_t)(s[0x310 >> 2] & 0xFFFF), oy = (int16_t)(s[0x310 >> 2] >> 16);
    int ow = (int)(s[0x314 >> 2] & 0xFFFF), oh = (int)(s[0x314 >> 2] >> 16);
    int cx = (int16_t)(s[0x308 >> 2] & 0xFFFF), cy = (int16_t)(s[0x308 >> 2] >> 16);
    int cw = (int)(s[0x30C >> 2] & 0xFFFF), chh = (int)(s[0x30C >> 2] >> 16);
    float dsdx = (int32_t)s[0x318 >> 2] / 1048576.0f, dtdy = (int32_t)s[0x31C >> 2] / 1048576.0f;
    float u0 = (in_pt & 0xFFFF) / 16.0f, v0 = (in_pt >> 16) / 16.0f;
    {   static int n; if (ENVFLAG("MNR_BLIT_LOG") && n++ < 12)
            printf("[blit] src 0x%08x %ux%u pitch %u fmt %u -> dst 0x%08x pitch %u fmt %u out %d,%d %dx%d clip %d,%d %dx%d ds %.3f dt %.3f in %.1f,%.1f interp %u\n",
                   src, sw, sh, spitch, sfmt, dst, dpitch, dfmt, ox, oy, ow, oh, cx, cy, cw, chh, dsdx, dtdy, u0, v0, interp); }
    if (!src || !dst || !sw || !sh || ow <= 0 || oh <= 0) return;
    {   static int tl = -1; static int n; if (tl < 0) tl = getenv("MNR_THUMB_LOG") != NULL;   /* debug: blits into GPU memory */
        if (tl && dst >= 0xC0000000u && n++ < 3000) printf("[thumb] blit 0x%08x %ux%u fmt %u -> 0x%08x pitch %u fmt %u %dx%d flip %ld\n", src, sw, sh, sfmt, dst, dpitch, dfmt, ow, oh, rsx_flips); }
    fs.blits++;
    if (rsx_use_d3d == 1 && rsx_d3d_blit(src, spitch, sfmt, sw, sh, u0, v0, dsdx, dtdy, dst, dpitch, dfmt, ox, oy, ow, oh, cx, cy, cw, chh, interp)) return;
    if (rsx_use_d3d == 1) { rsx_d3d_flush_from = 3; rsx_d3d_flush_range(src, spitch * sh); rsx_d3d_flush_from = 0; }
    int s32 = sfmt != 7, d32 = dfmt != 4;
    for (int y = oy; y < oy + oh; y++) {
        if (y < cy || y >= cy + chh) continue;
        for (int x = ox; x < ox + ow; x++) {
            if (x < cx || x >= cx + cw) continue;
            float fu = u0 + (x - ox + 0.5f) * dsdx - 0.5f, fv = v0 + (y - oy + 0.5f) * dtdy - 0.5f;
            float acc[4] = {0}, wsum = 0;
            int taps = interp ? 2 : 1;
            for (int ty = 0; ty < taps; ty++) for (int tx = 0; tx < taps; tx++) {
                int ix = interp ? (int)floorf(fu) + tx : (int)(fu + 0.5f), iy = interp ? (int)floorf(fv) + ty : (int)(fv + 0.5f);
                float wx = interp ? (tx ? fu - floorf(fu) : 1 - (fu - floorf(fu))) : 1, wy = interp ? (ty ? fv - floorf(fv) : 1 - (fv - floorf(fv))) : 1;
                if (ix < 0) ix = 0; if (iy < 0) iy = 0; if (ix >= (int)sw) ix = (int)sw - 1; if (iy >= (int)sh) iy = (int)sh - 1;
                float c[4];
                if (s32) { uint32_t a = src + (uint32_t)iy * spitch + (uint32_t)ix * 4; for (int k = 0; k < 4; k++) c[k] = ld_u8(a + (uint32_t)k); }
                else { uint16_t v = ld_u16(src + (uint32_t)iy * spitch + (uint32_t)ix * 2); c[0] = 255; c[1] = (float)((v >> 11) * 255 / 31); c[2] = (float)(((v >> 5) & 63) * 255 / 63); c[3] = (float)((v & 31) * 255 / 31); }
                for (int k = 0; k < 4; k++) acc[k] += c[k] * wx * wy;
                wsum += wx * wy;
            }
            for (int k = 0; k < 4; k++) acc[k] /= wsum;
            if (d32) { uint32_t a = dst + (uint32_t)y * dpitch + (uint32_t)x * 4; for (int k = 0; k < 4; k++) st_u8(a + (uint32_t)k, (uint8_t)(acc[k] + 0.5f)); }
            else st_u16(dst + (uint32_t)y * dpitch + (uint32_t)x * 2, ((uint32_t)acc[1] >> 3) << 11 | ((uint32_t)acc[2] >> 2) << 5 | ((uint32_t)acc[3] >> 3));
        }
    }
    if (rsx_use_d3d == 1) rsx_d3d_reload_range(dst, dpitch * (uint32_t)(oy + oh));
}

static void nv0039_copy(int sc) {
    uint32_t *r = reg[sc];
    uint32_t in = rsx_addr(r[0x30C >> 2], r[0x184 >> 2]), out = rsx_addr(r[0x310 >> 2], r[0x188 >> 2]);
    int32_t pin = (int32_t)r[0x314 >> 2], pout = (int32_t)r[0x318 >> 2];
    uint32_t len = r[0x31C >> 2], lines = r[0x320 >> 2];
    if (!in || !out || !len) return;
    uint32_t out_bytes = (uint32_t)(pout < 0 ? -pout : pout) * lines + len;
    {   static int tl = -1; static int n; if (tl < 0) tl = getenv("MNR_THUMB_LOG") != NULL;   /* debug: copies into GPU memory */
        if (tl && out >= 0xC0000000u && n++ < 3000) printf("[thumb] copy 0x%08x -> 0x%08x %u x %u flip %ld\n", in, out, len, lines, rsx_flips); }
    {   static int n, on = -1; if (on < 0) on = ENVFLAG("MNR_BLIT_LOG");
        if (on && n++ < 40) printf("[copy] 0x%08x -> 0x%08x %u x %u lines (pitch %d -> %d)\n", in, out, len, lines, pin, pout); }
    /* a GPU surface over the destination must not later write its stale contents over the copy:
     * bring its memory up to date first (for partial copies), then reload it from the result */
    if (rsx_use_d3d == 1) { rsx_d3d_flush_from = 1; rsx_d3d_flush_range(in, (uint32_t)(pin < 0 ? -pin : pin) * lines + len); rsx_d3d_flush_from = 2; rsx_d3d_flush_range(out, out_bytes); rsx_d3d_flush_from = 0; }
    for (uint32_t y = 0; y < lines; y++)
        memmove(gp(out + (uint32_t)(pout * (int32_t)y)), gp(in + (uint32_t)(pin * (int32_t)y)), len);
    if (rsx_use_d3d == 1) rsx_d3d_reload_range(out, out_bytes);
    fs.blits++;
}

/* ---- MNR_RSX_DUMP=<flip number>: print every draw of that frame with its programs and inputs */
static long dump_frame = -2, dump_heavy_at = -1;   /* MNR_RSX_DUMP_HEAVY=<k>: k frames after the first frame with >= 4500 draws */
static int dumping(void) {
    if (dump_frame == -2) { const char *e = getenv("MNR_RSX_DUMP"); dump_frame = e ? atol(e) : -1; }
    {   /* MNR_RSX_DUMP_TARGET=<hex colour offset>: every draw into that render target (debug) */
        static long tgt = -2, tgt2 = -1; static int n;
        if (tgt == -2) { const char *e = getenv("MNR_RSX_DUMP_TARGET"); char *end = NULL; tgt = e ? (long)strtoul(e, &end, 16) : -1;
                         if (end && *end == ',') tgt2 = (long)strtoul(end + 1, NULL, 16); }
        if (tgt >= 0 && (R3D(0x0210) == (uint32_t)tgt || R3D(0x0210) == (uint32_t)tgt2) && n++ < 3000) return 1;
    }
    if (dump_heavy_at > 0 && rsx_flips + 1 == dump_heavy_at) return 1;
    return dump_frame >= 0 && rsx_flips + 1 == dump_frame;
}
uint32_t rsx_fp_addr(void) {   /* SHADER_PROGRAM: offset | location (1 local, 2 main) */
    uint32_t v = R3D(0x08E4);
    return (v & 3) == 2 ? gcm_io_to_ea_pub(v & ~3u) : RSX_LOCAL_BASE + (v & ~3u);
}
__attribute__((noinline)) static void dump_draw(unsigned prim, uint32_t arg, int indexed) {
    static uint32_t seen_fp[4096], nseen_fp; static uint32_t seen_vp[512], nseen_vp;
    char b[512];
    printf("[draw] %s %s first %u count %u", indexed ? "indexed" : "arrays", prim_name(prim), arg & 0xFFFFFF, (arg >> 24) + 1);
    if (indexed) printf(" (index dma 0x%x offset 0x%x)", R3D(0x1820), R3D(0x181C));
    printf(" VP start %u in 0x%x out 0x%x FP 0x%x ctl 0x%x\n", R3D(0x1EA0), R3D(0x1FF0), R3D(0x1FF4), R3D(0x08E4), R3D(0x1D60));
    printf("   surface fmt 0x%08x target 0x%x A 0x%x pitch %u Z 0x%x zpitch %u clip %ux%u+%u+%u viewport scale %08x %08x\n",
           R3D(0x0208), R3D(0x0220), R3D(0x0210), R3D(0x020C), R3D(0x0214), R3D(0x022C), R3D(0x0200) >> 16, R3D(0x0204) >> 16,
           R3D(0x0200) & 0xFFFF, R3D(0x0204) & 0xFFFF, R3D(0x0A30), R3D(0x0A34));
    { static const int ci[] = {264, 265, 266, 267, 288, 298, 299, 466, 467};
      printf("   vpc:"); for (unsigned i = 0; i < sizeof ci / sizeof ci[0]; i++) { float f[4]; memcpy(f, rsx_vpc[ci[i]], 16); printf(" c%d=(%g %g %g %g)", ci[i], f[0], f[1], f[2], f[3]); }
      printf(" | base off 0x%x idx 0x%x\n", R3D(0x1738), R3D(0x173C)); }
    printf("   state: blend %u sfactor 0x%08x dfactor 0x%08x eq 0x%08x | alphatest %u func 0x%x ref 0x%x | depth test %u func 0x%x write %u | colormask 0x%08x | cull %u\n",
           R3D(0x0310) & 1, R3D(0x0314), R3D(0x0318), R3D(0x0320), R3D(0x0304) & 1, R3D(0x0308), R3D(0x030C), R3D(0x0A74) & 1, R3D(0x0A6C), R3D(0x0A70) & 1, R3D(0x0324), R3D(0x1830) & 1);
    printf("   scissor x %u+%u y %u+%u | stencil %u func 0x%x ref 0x%x mask 0x%x wmask 0x%x ops 0x%x/0x%x/0x%x | two-sided %u\n",
           R3D(0x08C0) & 0xFFFF, R3D(0x08C0) >> 16, R3D(0x08C4) & 0xFFFF, R3D(0x08C4) >> 16, R3D(0x0328) & 1, R3D(0x0330), R3D(0x0334), R3D(0x0338), R3D(0x032C),
           R3D(0x033C), R3D(0x0340), R3D(0x0344), R3D(0x0348) & 1);
    for (int a = 0; a < 16; a++) {
        uint32_t f = R3D(0x1740 + 4 * a);
        if ((f >> 4) & 15) printf("   attr %2d: type %u size %u stride %u offset 0x%08x\n", a, f & 15, (f >> 4) & 15, (f >> 8) & 255, R3D(0x1680 + 4 * a));
        if (((f >> 4) & 15) && a == 0 && (f & 15) == 5) {   /* S32K positions: the first 16 vertices */
            uint32_t off = R3D(0x1680), base = (off >> 31) ? gcm_io_to_ea_pub((off & 0x7FFFFFFF) + R3D(0x1738)) : RSX_LOCAL_BASE + (off & 0x7FFFFFFF) + R3D(0x1738);
            printf("   pos:"); for (uint32_t i = 0; i < 16 && base; i++) printf(" (%d,%d)", (int16_t)ld_u16(base + i * ((f >> 8) & 255)), (int16_t)ld_u16(base + i * ((f >> 8) & 255) + 2)); printf("\n");
        }
    }
    for (int t = 0; t < 16; t++) {
        uint32_t ctl0 = R3D(0x1A0C + 0x20 * t);
        if (ctl0 >> 31) {
            uint32_t fmt = R3D(0x1A04 + 0x20 * t), rect = R3D(0x1A18 + 0x20 * t);
            printf("   tex %2d: offset 0x%08x fmt 0x%08x (%02x) %ux%u pitch %u remap 0x%x filter 0x%x addr 0x%x\n", t, R3D(0x1A00 + 0x20 * t), fmt,
                   (fmt >> 8) & 0xFF, rect >> 16, rect & 0xFFFF, R3D(0x1840 + 4 * t) & 0xFFFFF, R3D(0x1A10 + 0x20 * t), R3D(0x1A14 + 0x20 * t), R3D(0x1A08 + 0x20 * t));
        }
    }
    uint32_t vs = R3D(0x1EA0); int new_vp = 1;
    uint32_t vh = 2166136261u;   /* key: the program's words, not just its start slot */
    for (uint32_t i = vs; i < 512; i++) { for (int k = 0; k < 4; k++) vh = (vh ^ rsx_vp[i][k]) * 16777619u; if (rsx_vp[i][3] & 1) break; }
    for (uint32_t i = 0; i < nseen_vp; i++) if (seen_vp[i] == vh) new_vp = 0;
    if (new_vp && nseen_vp < 512) {
        seen_vp[nseen_vp++] = vh;
        printf("   vp program 0x%08x:\n", vh);
        for (uint32_t i = vs; i < 512; i++) {
            vp_insn_t in; vp_decode(rsx_vp[i], &in); vp_disasm(&in, b, sizeof b);
            printf("   vp %3u: %08x %08x %08x %08x  %s\n", i, rsx_vp[i][0], rsx_vp[i][1], rsx_vp[i][2], rsx_vp[i][3], b);
            if (in.end) break;
        }
    }
    uint32_t fa = rsx_fp_addr(); int new_fp = 1;
    for (uint32_t i = 0; i < nseen_fp; i++) if (seen_fp[i] == fa) new_fp = 0;
    if (new_fp && nseen_fp < 4096) {
        seen_fp[nseen_fp++] = fa;
        static fp_insn_t prog[512];
        int n = fp_decode(fa, prog, 512);
        for (int i = 0; i < n; i++) { fp_disasm(&prog[i], b, sizeof b); printf("   fp %3d: %s\n", i, b); }
    }
}

/* Rarely used paths, out of line: rsx_method runs for every command word (millions a second in races), and with
 * these inlined each call saved and restored ten SSE registers and a 0x218-byte frame */
__attribute__((noinline)) static void method_debug(unsigned sc, uint32_t m, uint32_t arg) {
    { static int cmds = -1; if (cmds < 0) cmds = getenv("MNR_RSX_DUMPCMDS") != NULL;   /* with MNR_RSX_DUMP: every method of that frame */
      if (cmds && dumping()) printf("[m] sc%u 0x%04x = 0x%08x\n", sc, m, arg); }
    {   /* MNR_RSX_CMDTRIG=<hex value of 0x08E4>: when that fragment program is selected (after flip 3000), print the
         * 200 methods before it and the 200 after (debug) */
        static long trig = -2; static uint32_t ring[600][3]; static unsigned rp, after;
        if (trig == -2) { const char *e = getenv("MNR_RSX_CMDTRIG"); trig = e ? (long)strtoul(e, NULL, 16) : -1; }
        if (trig >= 0 || after) {
            if (after) { printf("[m+] sc%u 0x%04x = 0x%08x\n", sc, m, arg); after--; }
            else {
                ring[rp % 600][0] = sc; ring[rp % 600][1] = m; ring[rp % 600][2] = arg; rp++;
                if (m == 0x08E4 && arg == (uint32_t)trig && rsx_flips > 3000) {
                    for (unsigned i = rp > 200 ? rp - 200 : 0; i < rp; i++) printf("[m-] sc%u 0x%04x = 0x%08x\n", ring[i % 200][0], ring[i % 200][1], ring[i % 200][2]);
                    after = 400; trig = -1;
                }
            }
        }
    }
    {   /* MNR_RSX_TICKLOG=1 (debug): every 5 s of wall time, what the command processor did, flips or not (long loads) */
        static int on = -1; static unsigned long long methods, draws, blits, copies, t0; static unsigned n;
        if (on < 0) on = getenv("MNR_RSX_TICKLOG") != NULL;
        if (on) {
            methods++;
            if (sc == 0 && (m == 0x1814 || m == 0x1824)) draws++;
            else if (sc == 6 && m == 0x40C) blits++;
            else if (sc != 0 && sc != 3 && sc != 6 && m == 0x328) copies++;
            if (!(++n & 1023)) {
                ULONGLONG t = GetTickCount64();
                if (!t0) t0 = t;
                else if (t - t0 >= 5000) {
                    static long up0; static double ups0, wbs0; static uint64_t wb0, why0[10];
                    printf("[rsxtick] %.1f s: %llu methods, %llu draws, %llu blits, %llu copies | %ld uploads (%.2f s) | %llu writebacks (%.2f s) by cause:",
                           (t - t0) / 1000.0, methods, draws, blits, copies, rsx_d3d_uploads - up0, rsx_d3d_upload_s - ups0,
                           (unsigned long long)(rsx_d3d_wb_count - wb0), rsx_d3d_wb_s - wbs0);
                    for (int k = 0; k < 10; k++) { if (rsx_d3d_wb_why[k] != why0[k]) printf(" %d:%llu", k, (unsigned long long)(rsx_d3d_wb_why[k] - why0[k])); why0[k] = rsx_d3d_wb_why[k]; }
                    printf(" | flips %ld\n", rsx_flips);
                    methods = draws = blits = copies = 0; t0 = t; up0 = rsx_d3d_uploads; ups0 = rsx_d3d_upload_s; wb0 = rsx_d3d_wb_count; wbs0 = rsx_d3d_wb_s;
                }
            }
        }
    }
}
__attribute__((noinline)) static void method_report(uint32_t arg) {   /* GET_REPORT: (type << 24) | offset into the report area (cellGcmGetReportDataAddressLocation)
                      * writes {u64 timestamp, u32 value, u32 pad}. ZPASS pixel counts (type 1) report "visible": the game uses
                      * them for occlusion culling, and 0 hid every tested object. Other types (ZCULL statistics) report 0. */
        uint32_t a = RSX_LOCAL_BASE + 0x0FE00000u + (arg & 0xFFFFFF), type = arg >> 24;   /* the report area (GCM_REPORTS in host.c), outside the game's 249 MB */
        { static int n; if (ENVFLAG("MNR_RSX_REPORTLOG") && n < 40) { n++; printf("[rsx] GET_REPORT type %u offset 0x%x (flip %ld)\n", type, arg & 0xFFFFFF, rsx_flips); } }
        LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
        uint64_t ns = (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
        st_u32(a, (uint32_t)(ns >> 32)); st_u32(a + 4, (uint32_t)ns);
        { static int fixed = -1; if (fixed < 0) fixed = getenv("MNR_D3D_ZPASS_FIXED") != NULL;
          uint32_t v = type == 1 ? (rsx_use_d3d == 1 && !fixed ? rsx_d3d_zpass_read_at(a + 8) : 0x10000u) : 0;
          st_u32(a + 8, v);
          { static long from = -2; if (from == -2) { const char *e = getenv("MNR_RSX_REPORTVAL"); from = e ? atol(e) : -1; }
            if (from >= 0 && rsx_flips >= from) {
              printf("[report] flip %ld offset 0x%x value %u (enables %ld clears %ld counted draws %ld)\n", rsx_flips, arg & 0xFFFFFF, v, rsx_d3d_zq_enables, rsx_d3d_zq_clears, rsx_d3d_zq_draws); } } }
        st_u32(a + 12, 0);
}
__attribute__((noinline)) static void method_subchannel(unsigned sc, uint32_t m, uint32_t arg) {
        /* MNR_RSX_SCLOG=1: histogram of methods on the 2D subchannels, printed with the frame log */
        static int on = -1; if (on < 0) { const char *e = getenv("MNR_RSX_SCLOG"); on = e && *e == '1'; }
        if (on) { rsx_sc_hist[sc & 7][(m >> 2) & 0x7FF]++; }
        /* other subchannels: NV0039 copies are triggered by writing the line count + format */
        if (sc == 6 && m == 0x40C) nv3089_blit();   /* NV3089_IMAGE_IN: scaled image trigger */
        else if (m == 0x328 && sc != 3 && sc != 6) nv0039_copy((int)sc);   /* NV0039_BUFFER_NOTIFY */
}
static int method_debug_on = -1;
void rsx_method(unsigned sc, uint32_t m, uint32_t arg) {
    if (m >= 0x10000) return;
    if (method_debug_on) {
        if (method_debug_on < 0) method_debug_on = getenv("MNR_RSX_DUMP") || getenv("MNR_RSX_DUMPCMDS") || getenv("MNR_RSX_CMDTRIG") || getenv("MNR_RSX_TICKLOG");
        if (method_debug_on) method_debug(sc, m, arg);
    }
    reg[sc][m >> 2] = arg;
    if (sc != 0) { method_subchannel(sc, m, arg); return; }
    switch (m) {
    case 0x1808:                        /* BEGIN_END: arg = primitive (0 = end) */
        rsx_draw_begin_end(arg);
        break;
    case 0x1814: {                      /* DRAW_ARRAYS: (count - 1) << 24 | first */
        if (dumping()) dump_draw(R3D(0x1808), arg, 0);
        fs.draws++; fs.verts += (arg >> 24) + 1;
        rsx_draw_arrays(arg);
        break;
    }
    case 0x1824:                        /* DRAW_INDEX_ARRAY */
        cam_track();
        if (dumping()) dump_draw(R3D(0x1808), arg, 1);
        fs.draws++; fs.indexed++; fs.verts += (arg >> 24) + 1;
        rsx_draw_index_array(arg);
        break;
    case 0x1818:                        /* INLINE_ARRAY */
        fs.inline_draws++;
        break;
    case 0x1D94: fs.clears++; rsx_clear(arg); break;    /* CLEAR_SURFACE */
    case 0x1D84: if (rsx_use_d3d == 1) rsx_d3d_zpass_enable(arg & 1); break;   /* SET_ZPASS_PIXEL_COUNT_ENABLE */
    case 0x17C8: if (rsx_use_d3d == 1 && arg == 1) rsx_d3d_zpass_clear(); break;   /* CLEAR_REPORT_VALUE (1: ZPASS) */
    case 0x1800: method_report(arg); break;   /* GET_REPORT */
    case 0x08E4:                        /* SHADER_PROGRAM: fragment program offset | location */
        if (arg != fs.last_fp) { fs.fp_changes++; fs.last_fp = arg; }
        break;
    case 0x1E9C: vp_load = arg; vp_word = 0; break;    /* TRANSFORM_PROGRAM_LOAD */
    case 0x1EFC: vpc_load = arg; vpc_word = 0; break;  /* TRANSFORM_CONSTANT_LOAD */
    case 0xFEAC: break;                                /* GCM flip (handled by rsx_flip) */
    case 0x006C: case 0x1D70: case 0x1D74: case 0x0050:   /* semaphore releases and SET_REFERENCE: the CPU may look at results now */
        if (rsx_use_d3d == 1) rsx_d3d_sync();
        break;
    default:
        if (m >= 0x0B80 && m < 0x0C00) {               /* TRANSFORM_PROGRAM[32]: streamed words */
            uint32_t i = vp_load + vp_word / 4;
            if (i < 512) rsx_vp[i][vp_word & 3] = arg;
            rsx_vp_dirty();
            if (++vp_word % 4 == 0 && vp_word == 4) fs.vp_uploads++;
        } else if (m >= 0x1F00 && m < 0x1F80) {        /* TRANSFORM_CONSTANT[32] */
            uint32_t i = vpc_load + vpc_word / 4;
            if (i < 468) rsx_vpc[i][vpc_word & 3] = arg;
            rsx_vpc_gen++;
            vpc_word++;
        }
    }
}

/* the processor reached a flip: the displayed buffer changes now */
/* MNR_RSX_DRAW_HEAVY=<n>: drawing stays off until a frame issues at least n draws (then stays on) */
volatile int rsx_heavy_started = -1;
long rsx_heavy_flip;
/* vsync: at most one flip per 1/MNR_FPS_CAP s (default 30, as on the console), so the game is paced like the console when it keeps up
 * (MNR_NO_VSYNC=1 disables). Flips are not snapped to a fixed vblank grid: when a frame takes longer
 * than a period, waiting for the next grid line only added up to 16.7 ms to every slow frame. */
static void wait_vblank(void) {
    static int off = -1;
    static LARGE_INTEGER freq, next;
    if (off < 0) { const char *e = getenv("MNR_NO_VSYNC"); off = e && *e == '1'; QueryPerformanceFrequency(&freq);
                   /* with the game's vblank handler running, its semaphore paces the flips (flip mode 2 = immediate):
                    * an extra cap here only delays each flip past the next 30 Hz slot */
                   if (getenv("MNR_VBLANK_HANDLER") && !getenv("MNR_FPS_CAP")) off = 1; }
    if (off) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    /* the console shows a new frame at most every second vblank for this game (30 fps); MNR_FPS_CAP=<fps> */
    static LONGLONG cap = -1; if (cap < 0) { const char *e = getenv("MNR_FPS_CAP"); cap = e && atoi(e) > 0 ? atoi(e) : 60; }
    /* everything flips at most at 30 Hz like the console (loading screens included), except Bink movies: our
     * player needs more presents per movie frame than the console's SPU decoder (see host.c, label wait) */
    int movie = fs_movies_open > 0 && now.QuadPart - fs_movie_read_qpc < 2 * freq.QuadPart;
    LONGLONG period = freq.QuadPart / (getenv("MNR_FPS_CAP") || movie ? cap : 30);
    /* in gameplay the game's own frame lock (host.c, the label 68 tick on the same grid) already held this frame back:
     * waiting again here on a second clock pushed frames that made the first deadline past the next one */
    { if (gcm_frame_ticked && !movie) { gcm_frame_ticked = 0; next.QuadPart = now.QuadPart + period; return; } }
    if (!next.QuadPart || now.QuadPart >= next.QuadPart) { next.QuadPart = now.QuadPart + period; return; }   /* slow frame: flip now */
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = next.QuadPart - now.QuadPart;
        if (left <= 0) break;
        DWORD ms = (DWORD)(left * 1000 / freq.QuadPart);
        { if (rsx_lazy_req) rsx_d3d_lazy_service(); }
        if (ms > 1) Sleep(1); else SwitchToThread();   /* (1 ms steps: lazy write-back requests are served while waiting) */
    }
    next.QuadPart += period;
}

/* MNR_CAMLOG=1 (debug): per flip, the largest translation magnitude of a perspective transform (c267 of
 * indexed draws with c267.w > 0.5, the MVP's last row) and the time since the previous flip: shows the
 * camera running away */
static float cam_max; static int cam_n, cam_big; static uint32_t cam_bad[4][4];
static uint32_t cam_eye[3];
__attribute__((noinline)) static void cam_track(void) {   /* debug (MNR_CAMLOG / MNR_CAMFIND) */
    static int on = -1; if (on < 0) on = getenv("MNR_CAMLOG") || getenv("MNR_CAMFIND");
    if (!on) return;
    { float e[4]; memcpy(e, rsx_vpc[293], 16);   /* eye position as the main-pass shaders get it */
      if (e[3] == 1.0f && fabsf(e[0]) + fabsf(e[1]) + fabsf(e[2]) > 5.0f && !(e[0] == 1.0f && e[1] == 1.0f)) memcpy(cam_eye, rsx_vpc[293], 12); }
    float f[4]; memcpy(f, rsx_vpc[267], 16);
    if (fabsf(f[2] - f[3]) < 0.01f * fabsf(f[3]) + 1.0f && fabsf(f[3]) > 1.0f) {   /* perspective: z row ~ w row */
        float m = fabsf(f[0]) + fabsf(f[1]) + fabsf(f[3]); if (m > cam_max || m != m) cam_max = m;
        cam_n++;
        if (m > 1e6f && !cam_big++) memcpy(cam_bad, rsx_vpc + 264, sizeof cam_bad);
    }
}
static void vpc_dump(void) {   /* MNR_VPCDUMP=<flip>: every vertex constant at the end of that frame (debug) */
    static long at = -2; if (at == -2) { const char *e = getenv("MNR_VPCDUMP"); at = e ? atol(e) : -1; }
    if (at < 0 || rsx_flips != at) return;
    for (int i = 0; i < 468; i++) { float f[4]; memcpy(f, rsx_vpc[i], 16); printf("[vpc] c%d %g %g %g %g\n", i, f[0], f[1], f[2], f[3]); }
}
static void cam_find(void) {   /* MNR_CAMFIND=<flip>: guest addresses holding the eye position (c293.xyz) at that flip (debug) */
    static long at = -2; if (at == -2) { const char *e = getenv("MNR_CAMFIND"); at = e ? atol(e) : -1; }
    if (at < 0 || rsx_flips != at) return;
    uint32_t w[3]; for (int k = 0; k < 3; k++) w[k] = cam_eye[k];
    printf("[camfind] eye %08x %08x %08x\n", w[0], w[1], w[2]);
    static const uint32_t ranges[][2] = {{0x00E80000, 0x01170000}, {0x30000000, 0x3D000000}};
    for (int r = 0; r < 2; r++)
        for (uint32_t a = ranges[r][0]; a + 12 <= ranges[r][1]; a += 4)
            if (ld_u32(a) == w[0] && ld_u32(a + 4) == w[1] && ld_u32(a + 8) == w[2]) printf("[camfind] at 0x%08x\n", a);
}
static void cam_log(void) {
    vpc_dump(); cam_find();
    static int on = -1; if (on < 0) { const char *e = getenv("MNR_CAMLOG"); on = e && *e == '1'; }
    if (on) {
        static LARGE_INTEGER last; LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
        double ms = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / f.QuadPart : 0; last = now;
        if (rsx_flips > 2700) { float e[3]; memcpy(e, cam_eye, 12); printf("[eye] flip %ld %g %g %g\n", rsx_flips, e[0], e[1], e[2]); }
        if (rsx_flips > 2700) printf("[cam] flip %ld %.0f ms max %g (%d of %d perspective draws far)\n", rsx_flips, ms, cam_max, cam_big, cam_n);
        if (cam_big) {
            float m[16]; memcpy(m, cam_bad, 64);
            printf("[cam]   far MVP rows: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                   m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
    cam_max = 0; cam_n = cam_big = 0;
}
void rsx_flip(uint32_t buffer) {
    cam_log();
    rsx_cp_state = "flip: waiting for vblank";
    { static int ms = -1; if (ms < 0) { const char *e = getenv("MNR_RSX_FLIP_SLEEP"); ms = e ? atoi(e) : 0; } if (ms) Sleep(ms); }   /* debug: a slower GPU */
    wait_vblank();
    rsx_cp_state = "flip: present";
    if (dump_heavy_at < 0) { const char *e = getenv("MNR_RSX_DUMP_HEAVY"); dump_heavy_at = e ? 0 : -2; }
    if (dump_heavy_at == 0 && fs.draws >= 4500) { dump_heavy_at = rsx_flips + atol(getenv("MNR_RSX_DUMP_HEAVY")); printf("[rsx] heavy frame at flip %ld: dumping flip %ld\n", rsx_flips, dump_heavy_at); }
    InterlockedIncrement(&rsx_flips);
    { rsx_ww_kick(rsx_flips); }   /* this frame's write-watch scan, on its own thread */
    { gcm_flip_presented(); }   /* the game's flip handler runs on its own thread */
    if (rsx_heavy_started < 0) { const char *e = getenv("MNR_RSX_DRAW_HEAVY"); rsx_heavy_started = e ? 0 : 1; }
    if (!rsx_heavy_started) {
        const char *e = getenv("MNR_RSX_DRAW_HEAVY");
        if (fs.draws >= (unsigned)atoi(e)) { rsx_heavy_started = 1; rsx_heavy_flip = rsx_flips; printf("[rsx] heavy frame (%u draws) at flip %ld: drawing on\n", fs.draws, rsx_flips); }
    }
    frame_log(buffer);
    memset(&fs, 0, sizeof fs - sizeof fs.last_fp);
    rsx_present(buffer);
}

uint32_t rsx_reg(unsigned sc, uint32_t m) { return reg[sc & 7][(m & 0xFFFC) >> 2]; }
