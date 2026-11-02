/* Minimal PNG decoder (png.c) used by the cellPngDec HLE. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t width, height; int bit_depth, color_type, interlace; } png_info_t;
int png_header(const uint8_t *data, size_t n, png_info_t *info);   /* 0 on success */
uint8_t *png_decode_rgba(const uint8_t *data, size_t n, png_info_t *info);   /* malloc'd RGBA8, or NULL */
uint8_t *png_zlib_inflate(const uint8_t *src, size_t len, size_t *outlen);   /* zlib stream -> malloc'd bytes (png.c) */
