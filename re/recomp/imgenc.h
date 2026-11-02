/* Minimal PNG and baseline-JPEG encoders (imgenc.c) used by the cellPngEnc / cellJpgEnc HLE. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* rgba: width*height*4 bytes, R,G,B,A per pixel. channels: 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA.
 * Both return a malloc'd stream and its size in *n, or NULL. */
uint8_t *png_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int channels, size_t *n);
uint8_t *jpg_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int quality, size_t *n);
