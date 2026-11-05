/* Direct3D 11 RSX backend (rsx_d3d.c), selected with MNR_RSX=d3d. */
#pragma once
#include <stdint.h>
#include <windows.h>
int rsx_d3d_init(HWND window);
int rsx_d3d_draw(unsigned prim, const uint32_t *elems, uint32_t nelems);   /* 1 = handled */
int rsx_d3d_clear(uint32_t mask);
void rsx_d3d_zpass_enable(int on);   /* occlusion counting (SET_ZPASS_PIXEL_COUNT_ENABLE, CLEAR_REPORT_VALUE, GET_REPORT) */
void rsx_d3d_zpass_clear(void);
uint32_t rsx_d3d_zpass_read(void);
uint32_t rsx_d3d_zpass_read_at(uint32_t addr);   /* value to write now; the real count lands at addr later */
void rsx_d3d_zpass_poll(void);
void rsx_d3d_present(uint32_t buffer);
void rsx_d3d_sync(void);                                     /* write back CPU-visible surfaces */
void rsx_d3d_flush_range(uint32_t addr, uint32_t size);      /* write back surfaces overlapping a range */
int rsx_d3d_read_display(uint32_t buffer, uint32_t *bgrx, uint32_t w, uint32_t h);
int rsx_d3d_blit(uint32_t src, uint32_t spitch, uint32_t sfmt, uint32_t sw, uint32_t sh, float u0, float v0, float dsdx, float dtdy,
                 uint32_t dst, uint32_t dpitch, uint32_t dfmt, int ox, int oy, int ow, int oh, int cx, int cy, int cw, int ch, uint32_t interp);
void rsx_d3d_reload_range(uint32_t addr, uint32_t size);   /* the CPU wrote guest memory: reload overlapping surfaces */
int rsx_d3d_res_scaled(void);   /* MNR_RESOLUTION: fragment programs scale wpos */

/* stats and debug counters (rsx_d3d.c, read by host.c and rsx.c) */
extern double rsx_d3d_gpu_ms, rsx_d3d_present_ms;
extern long rsx_d3d_gpu_frames;
extern volatile long rsx_d3d_sh_compiles, rsx_d3d_sh_compile_us, rsx_d3d_sh_skipped, rsx_d3d_sh_waited;
extern uint32_t rsx_d3d_flush_from;
extern volatile long rsx_lazy_req;
void rsx_d3d_lazy_service(void);
extern uint64_t rsx_d3d_wb_count, rsx_d3d_wb_pixels;
extern uint64_t rsx_d3d_wb_why[10], rsx_d3d_wb_why_px[10];
extern double rsx_d3d_wb_s;
extern uint32_t rsx_d3d_last_flush[5];
extern uint64_t rsx_d3d_ld_count[3], rsx_d3d_ld_pixels;
extern double rsx_d3d_compile_s, rsx_d3d_upload_s;
extern long rsx_d3d_compiles, rsx_d3d_uploads, rsx_d3d_skip_vs, rsx_d3d_skip_ps;
extern long long rsx_d3d_vbytes, rsx_d3d_vbig;
extern long rsx_d3d_zq_draws, rsx_d3d_zq_enables, rsx_d3d_zq_clears;

extern int rsx_use_d3d;
