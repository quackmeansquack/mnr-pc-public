/* RSX interface between the GCM command processor (host.c) and the RSX model (rsx.c). */
#pragma once
#include <stdint.h>

#define RSX_LOCAL_BASE 0xC0000000u   /* guest address of RSX local memory */

typedef struct { uint32_t offset, pitch, width, height; } rsx_display_t;
extern rsx_display_t rsx_display[8];   /* cellGcmSetDisplayBuffer */
extern volatile long rsx_flips;

void rsx_method(unsigned subchannel, uint32_t method, uint32_t arg);
void rsx_flip(uint32_t buffer);
void rsx_present(uint32_t buffer);
uint32_t rsx_reg(unsigned subchannel, uint32_t method);
uint32_t gcm_io_to_ea_pub(uint32_t io_offset);   /* host.c: IO offset -> guest address (0 if unmapped) */

/* rsx_render.c */
void rsx_draw_begin_end(uint32_t prim);
void rsx_draw_arrays(uint32_t arg);
void rsx_draw_index_array(uint32_t arg);
void rsx_clear(uint32_t mask);
void rsx_vp_dirty(void);
