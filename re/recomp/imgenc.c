/* Minimal PNG (fixed-Huffman deflate with LZ77, adaptive row filters) and baseline JPEG (4:4:4, Annex K tables)
 * encoders for the cellPngEnc / cellJpgEnc HLE: the game encodes creation thumbnails and photos with them. */
#include "imgenc.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { uint8_t *p; size_t n, cap; uint32_t bits; int nb; int fail; } ob_t;
static void ob_byte(ob_t *o, uint8_t b) {
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 65536; uint8_t *q = realloc(o->p, nc);
        if (!q) { o->fail = 1; return; }
        o->p = q; o->cap = nc;
    }
    o->p[o->n++] = b;
}
static void ob_bytes(ob_t *o, const void *d, size_t n) { for (size_t i = 0; i < n; i++) ob_byte(o, ((const uint8_t *)d)[i]); }
static void ob_be32(ob_t *o, uint32_t v) { ob_byte(o, v >> 24); ob_byte(o, v >> 16); ob_byte(o, v >> 8); ob_byte(o, v); }
static void ob_be16(ob_t *o, uint32_t v) { ob_byte(o, v >> 8); ob_byte(o, v); }

/* ------------------------------------------------------------------ PNG */
static void dbits(ob_t *o, uint32_t v, int len) {   /* deflate: LSB first */
    o->bits |= v << o->nb; o->nb += len;
    while (o->nb >= 8) { ob_byte(o, (uint8_t)o->bits); o->bits >>= 8; o->nb -= 8; }
}
static uint32_t rev(uint32_t c, int len) { uint32_t r = 0; while (len--) { r = (r << 1) | (c & 1); c >>= 1; } return r; }
static void dlit(ob_t *o, unsigned v) {   /* fixed Huffman literal/length code */
    if (v < 144) dbits(o, rev(0x30 + v, 8), 8);
    else if (v < 256) dbits(o, rev(0x190 + v - 144, 9), 9);
    else if (v < 280) dbits(o, rev(v - 256, 7), 7);
    else dbits(o, rev(0xC0 + v - 280, 8), 8);
}
static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static void dmatch(ob_t *o, unsigned len, unsigned dist) {
    int i = 28; while (lbase[i] > len) i--;
    dlit(o, 257 + i); dbits(o, len - lbase[i], lext[i]);
    int j = 29; while (dbase[j] > dist) j--;
    dbits(o, rev(j, 5), 5); dbits(o, dist - dbase[j], dext[j]);
}
static void zlib_compress(ob_t *o, const uint8_t *d, size_t n) {
    ob_byte(o, 0x78); ob_byte(o, 0x01);
    dbits(o, 1, 1); dbits(o, 1, 2);   /* one final block, fixed Huffman */
    enum { HB = 15, WIN = 32768, CHAIN = 48 };
    int32_t *head = malloc(sizeof(int32_t) << HB), *prev = malloc(sizeof(int32_t) * (n ? n : 1));
    if (!head || !prev) { o->fail = 1; free(head); free(prev); return; }
    memset(head, 0xff, sizeof(int32_t) << HB);
#define HASH(i) ((((uint32_t)d[i] << 10) ^ ((uint32_t)d[(i) + 1] << 5) ^ d[(i) + 2]) & ((1u << HB) - 1))
    size_t i = 0;
    while (i < n) {
        unsigned best = 0, bdist = 0;
        if (i + 2 < n) {
            uint32_t h = HASH(i);
            size_t maxl = n - i < 258 ? n - i : 258;
            int chain = CHAIN;
            for (int32_t j = head[h]; j >= 0 && i - (size_t)j <= WIN && chain--; j = prev[j]) {
                unsigned l = 0; while (l < maxl && d[j + l] == d[i + l]) l++;
                if (l > best) { best = l; bdist = (unsigned)(i - j); if (l == maxl) break; }
            }
            prev[i] = head[h]; head[h] = (int32_t)i;
        }
        if (best >= 3) {
            dmatch(o, best, bdist);
            for (size_t k = i + 1; k < i + best; k++) if (k + 2 < n) { uint32_t h = HASH(k); prev[k] = head[h]; head[h] = (int32_t)k; }
            i += best;
        } else { dlit(o, d[i]); i++; }
    }
#undef HASH
    dlit(o, 256);
    if (o->nb) dbits(o, 0, 8 - o->nb);
    free(head); free(prev);
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < n; k++) { a = (a + d[k]) % 65521; b = (b + a) % 65521; }
    ob_be32(o, (b << 16) | a);
}
static uint32_t crc_tab[256];
static uint32_t crc32_of(const uint8_t *d, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void png_chunk(ob_t *o, const char *type, const uint8_t *d, size_t n) {
    ob_be32(o, (uint32_t)n);
    size_t at = o->n;
    ob_bytes(o, type, 4); ob_bytes(o, d, n);
    if (!o->fail) ob_be32(o, crc32_of(o->p + at, n + 4));
}
static int paeth(int a, int b, int c) { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }

uint8_t *png_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int ch, size_t *n) {
    static const int ctype[5] = {0, 0, 4, 2, 6};
    static const int pick[5][4] = {{0}, {0}, {0, 3}, {0, 1, 2}, {0, 1, 2, 3}};   /* which RGBA bytes each channel count keeps */
    if (ch < 1 || ch > 4 || !w || !h) return NULL;
    size_t stride = (size_t)w * ch, rawn = (stride + 1) * h;
    uint8_t *px = malloc(stride * h), *raw = malloc(rawn), *cand = malloc(stride);
    if (!px || !raw || !cand) { free(px); free(raw); free(cand); return NULL; }
    for (size_t i = 0; i < (size_t)w * h; i++)
        for (int k = 0; k < ch; k++) px[i * ch + k] = rgba[i * 4 + pick[ch][k]];
    for (uint32_t y = 0; y < h; y++) {   /* adaptive filter: smallest sum of |signed residual| */
        const uint8_t *cur = px + y * stride, *up = y ? cur - stride : NULL;
        long bestsum = -1; uint8_t *out = raw + y * (stride + 1);
        for (int f = 0; f < 5; f++) {
            long sum = 0;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)ch ? cur[x - ch] : 0, b = up ? up[x] : 0, c = up && x >= (size_t)ch ? up[x - ch] : 0;
                int pr = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : paeth(a, b, c);
                uint8_t v = (uint8_t)(cur[x] - pr); cand[x] = v; sum += v < 128 ? v : 256 - v;
            }
            if (bestsum < 0 || sum < bestsum) { bestsum = sum; out[0] = (uint8_t)f; memcpy(out + 1, cand, stride); }
        }
    }
    free(px); free(cand);
    ob_t o = {0}, z = {0};
    zlib_compress(&z, raw, rawn);
    free(raw);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    ob_bytes(&o, sig, 8);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, (uint8_t)ctype[ch], 0, 0, 0};
    png_chunk(&o, "IHDR", ihdr, 13);
    png_chunk(&o, "IDAT", z.p, z.n);
    png_chunk(&o, "IEND", NULL, 0);
    free(z.p);
    if (o.fail || z.fail) { free(o.p); return NULL; }
    *n = o.n; return o.p;
}

/* ------------------------------------------------------------------ JPEG */
static const uint8_t nat[64] = {0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
                                35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
static const uint8_t qlum[64] = {16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55, 14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
                                 18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92, 49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99};
static const uint8_t qchr[64] = {17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99, 24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
                                 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99};
static const uint8_t dcl_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0}, dcc_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t dc_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t acl_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d}, acc_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t acl_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};
static const uint8_t acc_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};
typedef struct { uint16_t code; uint8_t len; } hc_t;
static void build_huff(const uint8_t *bits, const uint8_t *vals, hc_t *t) {
    unsigned code = 0, k = 0;
    for (int l = 1; l <= 16; l++) { for (int i = 0; i < bits[l - 1]; i++) { t[vals[k]].code = (uint16_t)code++; t[vals[k]].len = (uint8_t)l; k++; } code <<= 1; }
}
static void jbits(ob_t *o, uint32_t v, int len) {   /* MSB first, 0xFF stuffed */
    o->bits = (o->bits << len) | (v & ((1u << len) - 1)); o->nb += len;
    while (o->nb >= 8) { uint8_t b = (uint8_t)(o->bits >> (o->nb - 8)); ob_byte(o, b); if (b == 0xFF) ob_byte(o, 0); o->nb -= 8; }
    o->bits &= (1u << o->nb) - 1;
}
static void jval(ob_t *o, const hc_t *t, int sym, int v) {   /* Huffman symbol (run<<4 | size), then the magnitude bits */
    int a = v < 0 ? -v : v, sz = 0; while (a >> sz) sz++;
    jbits(o, t[sym | sz].code, t[sym | sz].len);
    if (sz) jbits(o, (uint32_t)(v < 0 ? v - 1 : v), sz);
}
static double dctc[8][8];
static void jblock(ob_t *o, const float *f, const uint8_t *q, int *dcp, const hc_t *dc, const hc_t *ac) {
    float tmp[64]; int z[64];
    for (int y = 0; y < 8; y++) for (int u = 0; u < 8; u++) { double s = 0; for (int x = 0; x < 8; x++) s += dctc[u][x] * f[y * 8 + x]; tmp[y * 8 + u] = (float)s; }
    for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
        double s = 0; for (int y = 0; y < 8; y++) s += dctc[v][y] * tmp[y * 8 + u];
        z[v * 8 + u] = (int)lrint(s / q[v * 8 + u]);
    }
    int diff = z[0] - *dcp; *dcp = z[0];
    jval(o, dc, 0, diff);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = z[nat[k]];
        if (!v) { run++; continue; }
        while (run > 15) { jbits(o, ac[0xF0].code, ac[0xF0].len); run -= 16; }
        jval(o, ac, run << 4, v); run = 0;
    }
    if (run) jbits(o, ac[0].code, ac[0].len);
}
uint8_t *jpg_encode(const uint8_t *rgba, uint32_t w, uint32_t h, int quality, size_t *n) {
    if (!w || !h || w > 65535 || h > 65535) return NULL;
    if (quality < 1 || quality > 100) quality = 85;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t ql[64], qc[64];
    for (int i = 0; i < 64; i++) {
        int a = (qlum[i] * scale + 50) / 100, b = (qchr[i] * scale + 50) / 100;
        ql[i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a); qc[i] = (uint8_t)(b < 1 ? 1 : b > 255 ? 255 : b);
    }
    for (int u = 0; u < 8; u++) for (int x = 0; x < 8; x++) dctc[u][x] = (u ? 0.5 : 0.5 / sqrt(2.0)) * cos((2 * x + 1) * u * 3.14159265358979323846 / 16);
    hc_t hdl[256] = {{0}}, hdc[256] = {{0}}, hal[256] = {{0}}, hac[256] = {{0}};
    build_huff(dcl_bits, dc_vals, hdl); build_huff(dcc_bits, dc_vals, hdc); build_huff(acl_bits, acl_vals, hal); build_huff(acc_bits, acc_vals, hac);
    ob_t o = {0};
    static const uint8_t app0[18] = {0xFF,0xD8, 0xFF,0xE0, 0,16, 'J','F','I','F',0, 1,1, 0, 0,1, 0,1};
    ob_bytes(&o, app0, 18); ob_byte(&o, 0); ob_byte(&o, 0);
    ob_be16(&o, 0xFFDB); ob_be16(&o, 132);
    ob_byte(&o, 0); for (int k = 0; k < 64; k++) ob_byte(&o, ql[nat[k]]);
    ob_byte(&o, 1); for (int k = 0; k < 64; k++) ob_byte(&o, qc[nat[k]]);
    ob_be16(&o, 0xFFC0); ob_be16(&o, 17); ob_byte(&o, 8); ob_be16(&o, h); ob_be16(&o, w); ob_byte(&o, 3);
    for (int i = 1; i <= 3; i++) { ob_byte(&o, (uint8_t)i); ob_byte(&o, 0x11); ob_byte(&o, i == 1 ? 0 : 1); }
    ob_be16(&o, 0xFFC4); ob_be16(&o, 418);
    ob_byte(&o, 0x00); ob_bytes(&o, dcl_bits, 16); ob_bytes(&o, dc_vals, 12);
    ob_byte(&o, 0x10); ob_bytes(&o, acl_bits, 16); ob_bytes(&o, acl_vals, 162);
    ob_byte(&o, 0x01); ob_bytes(&o, dcc_bits, 16); ob_bytes(&o, dc_vals, 12);
    ob_byte(&o, 0x11); ob_bytes(&o, acc_bits, 16); ob_bytes(&o, acc_vals, 162);
    static const uint8_t sos[14] = {0xFF,0xDA, 0,12, 3, 1,0x00, 2,0x11, 3,0x11, 0,63, 0};
    ob_bytes(&o, sos, 14);
    int dy = 0, dcb = 0, dcr = 0;
    for (uint32_t by = 0; by < h; by += 8) for (uint32_t bx = 0; bx < w; bx += 8) {
        float Y[64], Cb[64], Cr[64];
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            uint32_t sx = bx + x < w ? bx + x : w - 1, sy = by + y < h ? by + y : h - 1;   /* edge blocks repeat the last pixel */
            const uint8_t *p = rgba + ((size_t)sy * w + sx) * 4;
            float r = p[0], g = p[1], b = p[2];
            Y[y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128;
            Cb[y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
            Cr[y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
        }
        jblock(&o, Y, ql, &dy, hdl, hal); jblock(&o, Cb, qc, &dcb, hdc, hac); jblock(&o, Cr, qc, &dcr, hdc, hac);
    }
    if (o.nb) jbits(&o, 0x7F, 8 - o.nb);   /* pad with 1 bits */
    ob_be16(&o, 0xFFD9);
    if (o.fail) { free(o.p); return NULL; }
    *n = o.n; return o.p;
}
