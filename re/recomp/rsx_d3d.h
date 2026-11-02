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
extern int rsx_use_d3d;
