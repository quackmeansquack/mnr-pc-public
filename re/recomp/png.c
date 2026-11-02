/* Minimal PNG decoder for cellPngDec: zlib inflate (stored, fixed and dynamic Huffman blocks), PNG
 * chunk parsing, the five scanline filters, colour types 0/2/3/4/6 at bit depths 1..16, no interlace.
 * Output is always 8-bit RGBA; the HLE layer converts to the colour space the game asks for. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "png.h"

/* ------------------------------------------------------------------ inflate */
typedef struct { const uint8_t *src; size_t len, pos; uint32_t bitbuf; int bitcnt; uint8_t *out; size_t outlen, outcap; int err; } inf_t;

static int getbit(inf_t *s) {
    if (!s->bitcnt) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        s->bitbuf = s->src[s->pos++]; s->bitcnt = 8;
    }
    int b = s->bitbuf & 1; s->bitbuf >>= 1; s->bitcnt--;
    return b;
}
static uint32_t getbits(inf_t *s, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= (uint32_t)getbit(s) << i; return v; }

typedef struct { uint16_t count[16], symbol[320]; } huff_t;
static void huff_build(huff_t *h, const uint8_t *lengths, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
}
static int decode_sym(inf_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbit(s);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1;
    return -1;
}
static void out_byte(inf_t *s, uint8_t b) {
    if (s->outlen == s->outcap) { s->outcap = s->outcap ? s->outcap * 2 : 65536; s->out = realloc(s->out, s->outcap); }
    s->out[s->outlen++] = b;
}
static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void inflate_codes(inf_t *s, const huff_t *lit, const huff_t *dist) {
    for (;;) {
        int sym = decode_sym(s, lit);
        if (sym < 0 || s->err) return;
        if (sym < 256) out_byte(s, (uint8_t)sym);
        else if (sym == 256) return;
        else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; return; }
            size_t len = len_base[sym] + getbits(s, len_extra[sym]);
            int ds = decode_sym(s, dist);
            if (ds < 0 || ds >= 30) { s->err = 1; return; }
            size_t d = dist_base[ds] + getbits(s, dist_extra[ds]);
            if (d > s->outlen) { s->err = 1; return; }
            for (size_t i = 0; i < len; i++) out_byte(s, s->out[s->outlen - d]);
        }
    }
}
static uint8_t *zlib_inflate(const uint8_t *src, size_t len, size_t *outlen) {
    inf_t s = {0};
    if (len < 2) return NULL;
    s.src = src + 2; s.len = len - 2;   /* skip the zlib header */
    int last;
    do {
        last = getbit(&s);
        int type = (int)getbits(&s, 2);
        if (type == 0) {
            s.bitcnt = 0;
            if (s.pos + 4 > s.len) { s.err = 1; break; }
            uint32_t n = s.src[s.pos] | s.src[s.pos + 1] << 8;
            s.pos += 4;
            if (s.pos + n > s.len) { s.err = 1; break; }
            for (uint32_t i = 0; i < n; i++) out_byte(&s, s.src[s.pos + i]);
            s.pos += n;
        } else if (type == 1) {
            static huff_t fl, fd; static int init;
            if (!init) {
                uint8_t l[288];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                huff_build(&fl, l, 288);
                for (int i = 0; i < 30; i++) l[i] = 5;
                huff_build(&fd, l, 30);
                init = 1;
            }
            inflate_codes(&s, &fl, &fd);
        } else if (type == 2) {
            int hlit = (int)getbits(&s, 5) + 257, hdist = (int)getbits(&s, 5) + 1, hclen = (int)getbits(&s, 4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {0}, lens[320] = {0};
            for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)getbits(&s, 3);
            huff_t ch; huff_build(&ch, cl, 19);
            int n = 0;
            while (n < hlit + hdist && !s.err) {
                int sym = decode_sym(&s, &ch);
                if (sym < 16) lens[n++] = (uint8_t)sym;
                else if (sym == 16) { if (!n) { s.err = 1; break; } int r = 3 + (int)getbits(&s, 2); while (r-- && n < 320) { lens[n] = lens[n - 1]; n++; } }
                else if (sym == 17) { int r = 3 + (int)getbits(&s, 3); while (r-- && n < 320) lens[n++] = 0; }
                else if (sym == 18) { int r = 11 + (int)getbits(&s, 7); while (r-- && n < 320) lens[n++] = 0; }
                else s.err = 1;
            }
            huff_t lit, dist;
            huff_build(&lit, lens, hlit); huff_build(&dist, lens + hlit, hdist);
            inflate_codes(&s, &lit, &dist);
        } else s.err = 1;
    } while (!last && !s.err);
    if (s.err && !s.outlen) { free(s.out); return NULL; }
    *outlen = s.outlen;
    return s.out;
}

uint8_t *png_zlib_inflate(const uint8_t *src, size_t len, size_t *outlen) { return zlib_inflate(src, len, outlen); }   /* fwextract.c */

/* ------------------------------------------------------------------ PNG */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int png_header(const uint8_t *d, size_t n, png_info_t *info) {
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) || memcmp(d + 12, "IHDR", 4)) return -1;
    info->width = be32(d + 16); info->height = be32(d + 20);
    info->bit_depth = d[24]; info->color_type = d[25]; info->interlace = d[28];
    return 0;
}

static inline int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

uint8_t *png_decode_rgba(const uint8_t *d, size_t n, png_info_t *info) {
    if (png_header(d, n, info) || info->interlace) return NULL;
    uint8_t pal[256][4];
    for (int i = 0; i < 256; i++) { pal[i][0] = pal[i][1] = pal[i][2] = 0; pal[i][3] = 255; }
    uint8_t *idat = NULL; size_t idat_len = 0;
    int trns_gray = -1, trns_r = -1, trns_g = -1, trns_b = -1;
    for (size_t p = 8; p + 12 <= n;) {
        uint32_t len = be32(d + p);
        const uint8_t *type = d + p + 4, *body = d + p + 8;
        if (p + 12 + len > n) break;
        if (!memcmp(type, "PLTE", 4)) for (uint32_t i = 0; i < len / 3 && i < 256; i++) { pal[i][0] = body[3 * i]; pal[i][1] = body[3 * i + 1]; pal[i][2] = body[3 * i + 2]; }
        else if (!memcmp(type, "tRNS", 4)) {
            if (info->color_type == 3) for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = body[i];
            else if (info->color_type == 0 && len >= 2) trns_gray = body[0] << 8 | body[1];
            else if (info->color_type == 2 && len >= 6) { trns_r = body[0] << 8 | body[1]; trns_g = body[2] << 8 | body[3]; trns_b = body[4] << 8 | body[5]; }
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = realloc(idat, idat_len + len); memcpy(idat + idat_len, body, len); idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) break;
        p += 12 + len;
    }
    if (!idat) return NULL;
    size_t raw_len = 0;
    uint8_t *raw = zlib_inflate(idat, idat_len, &raw_len);
    free(idat);
    if (!raw) return NULL;
    static const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
    int ch = info->color_type <= 6 ? chans[info->color_type] : 0, bd = info->bit_depth;
    if (!ch) { free(raw); return NULL; }
    uint32_t w = info->width, h = info->height;
    size_t bpp_bits = (size_t)ch * bd, stride = (w * bpp_bits + 7) / 8, bpp = (bpp_bits + 7) / 8;
    if (raw_len < (stride + 1) * h) { free(raw); return NULL; }
    uint8_t *prev = calloc(stride, 1), *cur = malloc(stride);
    uint8_t *out = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *line = raw + y * (stride + 1);
        int f = line[0];
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = line[1 + i];
            switch (f) {
            case 1: x += a; break; case 2: x += b; break; case 3: x += (a + b) / 2; break; case 4: x += paeth(a, b, c); break;
            }
            cur[i] = (uint8_t)x;
        }
        for (uint32_t xi = 0; xi < w; xi++) {
            uint8_t *o = out + ((size_t)y * w + xi) * 4;
            int v[4];
            for (int k = 0; k < ch; k++) {
                if (bd == 8) v[k] = cur[xi * ch + k];
                else if (bd == 16) v[k] = cur[(xi * ch + k) * 2] << 8 | cur[(xi * ch + k) * 2 + 1];
                else { size_t bit = (size_t)(xi * ch + k) * bd; v[k] = (cur[bit / 8] >> (8 - bd - bit % 8)) & ((1 << bd) - 1); }
            }
            int maxv = (1 << bd) - 1;
            #define S8(x) (bd == 16 ? (x) >> 8 : bd == 8 ? (x) : (x) * 255 / maxv)
            switch (info->color_type) {
            case 0: o[0] = o[1] = o[2] = (uint8_t)S8(v[0]); o[3] = v[0] == trns_gray ? 0 : 255; break;
            case 2: o[0] = (uint8_t)S8(v[0]); o[1] = (uint8_t)S8(v[1]); o[2] = (uint8_t)S8(v[2]);
                    o[3] = (v[0] == trns_r && v[1] == trns_g && v[2] == trns_b) ? 0 : 255; break;
            case 3: memcpy(o, pal[v[0] & 255], 4); break;
            case 4: o[0] = o[1] = o[2] = (uint8_t)S8(v[0]); o[3] = (uint8_t)S8(v[1]); break;
            case 6: o[0] = (uint8_t)S8(v[0]); o[1] = (uint8_t)S8(v[1]); o[2] = (uint8_t)S8(v[2]); o[3] = (uint8_t)S8(v[3]); break;
            }
            #undef S8
        }
        uint8_t *t = prev; prev = cur; cur = t;
    }
    free(prev); free(cur); free(raw);
    return out;
}
