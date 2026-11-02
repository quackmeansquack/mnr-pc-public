/* Texture unit description and decoding shared by the software renderer and the D3D11 backend. */
#pragma once
#include <stdint.h>
typedef struct {
    uint32_t addr, fmt, w, h, pitch, linear, unnorm, dims, remap, wrap_s, wrap_t, wrap_r, mag_linear, min_linear, bytes, is_float, levels, min_filter, depth;
} rsx_texinfo_t;
int rsx_tex_info(int unit, rsx_texinfo_t *out);   /* 0 if the unit is disabled */
void rsx_tex_decode(int unit, void *dst);          /* level 0 as raw A,R,G,B: 4 bytes or 4 floats per texel */
uint64_t rsx_range_gen(uint32_t addr, uint32_t size);      /* newest write generation of the guest pages (cached per frame) */
uint64_t rsx_range_gen_now(uint32_t addr, uint32_t size);  /* same, uncached */
int rsx_range_pages_after(uint32_t addr, uint32_t size, uint64_t since, uint8_t *out);   /* per page: written after <since> */
long rsx_ww_scans(void);                                  /* full write-watch scans started so far */
uint64_t rsx_ww_gen_after(long k);                         /* generation of full scan k+1 (UINT64_MAX until it has run) */
int rsx_range_dirty_span(uint32_t addr, uint32_t size, uint64_t since, uint32_t *lo, uint32_t *hi);   /* pages written after <since> */
void rsx_tex_decode_level(int unit, uint32_t level, void *dst);   /* raw A,R,G,B of one mip level */
void rsx_tex_decode_3d(int unit, uint32_t depth, void *dst);   /* linear 3D texture, slices stacked */
