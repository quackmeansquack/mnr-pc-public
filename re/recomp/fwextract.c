/* The voice codec libraries from the PS3 system update that ships with every copy of the game
 * (extracted\PS3_UPDATE\PS3UPDAT.PUP), so voice chat works without RPCS3 or anything else installed.
 *
 * PUP ("SCEUF"): a table of files; entry 0x300 is update_files.tar. Its dev_flash_*.tar.aa.* members are SCE
 * containers ("SCE\0"): metadata info at 0x20 + se_meta, AES-256-CBC with the SCE package key; it holds the AES-128
 * key/iv for the metadata headers (CTR), which list the sections and their CTR keys; section data may be zlib
 * compressed. The last section is a tar of dev_flash files. dev_flash/sys/external/libcelpenc.sprx and libcelpdec.sprx
 * are SELFs with the same layout, their metadata key found by trying the known SELF keys (the right one leaves the
 * key/iv padding zero). The decrypted, decompressed segments are written to <MNR>\firmware\libcelp{enc,dec}.prx: not a
 * runnable PRX, but voice_celp.c only takes the SPU program embedded in it.
 * The keys are the public ones RPCS3 uses (fwkeys.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <bcrypt.h>
#include "png.h"
#include "fwkeys.h"

static uint64_t be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* ---- AES through Windows CNG */
static BCRYPT_KEY_HANDLE aes_key(const uint8_t *key, ULONG bits, const wchar_t *mode) {
    static BCRYPT_ALG_HANDLE cbc, ecb;
    BCRYPT_ALG_HANDLE *a = wcscmp(mode, BCRYPT_CHAIN_MODE_CBC) ? &ecb : &cbc;
    if (!*a) {
        if (BCryptOpenAlgorithmProvider(a, BCRYPT_AES_ALGORITHM, NULL, 0)) return NULL;
        BCryptSetProperty(*a, BCRYPT_CHAINING_MODE, (PUCHAR)mode, (ULONG)(wcslen(mode) + 1) * 2, 0);
    }
    BCRYPT_KEY_HANDLE k = NULL;
    if (BCryptGenerateSymmetricKey(*a, &k, NULL, 0, (PUCHAR)key, bits / 8, 0)) return NULL;
    return k;
}
static int aes_cbc_dec(const uint8_t key[32], const uint8_t iv_in[16], uint8_t *d, ULONG n) {   /* AES-256 */
    BCRYPT_KEY_HANDLE k = aes_key(key, 256, BCRYPT_CHAIN_MODE_CBC); if (!k) return -1;
    uint8_t iv[16]; memcpy(iv, iv_in, 16); ULONG got;
    NTSTATUS r = BCryptDecrypt(k, d, n, NULL, iv, 16, d, n, &got, 0);
    BCryptDestroyKey(k); return r ? -1 : 0;
}
static int aes_ctr(const uint8_t key[16], const uint8_t iv_in[16], uint8_t *d, size_t n) {   /* AES-128, counter = iv */
    BCRYPT_KEY_HANDLE k = aes_key(key, 128, BCRYPT_CHAIN_MODE_ECB); if (!k) return -1;
    uint8_t ctr[16]; memcpy(ctr, iv_in, 16);
    enum { CH = 4096 };   /* counter blocks encrypted a page at a time */
    uint8_t *blk = malloc(CH * 16); ULONG got;
    for (size_t off = 0; off < n; off += CH * 16) {
        size_t nb = (n - off + 15) / 16; if (nb > CH) nb = CH;
        for (size_t b = 0; b < nb; b++) { memcpy(blk + 16 * b, ctr, 16); for (int i = 15; i >= 0 && !++ctr[i]; i--) {} }
        if (BCryptEncrypt(k, blk, (ULONG)(nb * 16), NULL, NULL, 0, blk, (ULONG)(nb * 16), &got, 0)) { free(blk); BCryptDestroyKey(k); return -1; }
        for (size_t i = 0; i < nb * 16 && off + i < n; i++) d[off + i] ^= blk[i];
    }
    free(blk); BCryptDestroyKey(k); return 0;
}
static void hex(const char *s, uint8_t *out, int n) { for (int i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); out[i] = (uint8_t)v; } }

/* ---- SCE container: decrypted (and decompressed) sections, concatenated; sec_off/sec_len index them */
typedef struct { uint8_t *data; size_t len; size_t off[64], size[64]; int n; } sce_out_t;
static int sce_decrypt(const uint8_t *f, size_t flen, int is_self, sce_out_t *o) {
    memset(o, 0, sizeof *o);
    if (flen < 0x20 || be32(f) != 0x53434500) return -1;
    uint16_t flags = (uint16_t)(f[8] << 8 | f[9]);
    uint32_t meta = be32(f + 0x0C); uint64_t hsize = be64(f + 0x10);
    size_t mi = 0x20 + (size_t)meta;   /* metadata info (0x40), then the metadata headers up to hsize */
    if (flags & 0x8000 || mi + 0x40 + 0x20 > hsize || hsize > flen) return -1;   /* debug (unencrypted) ones: not expected here */
    uint8_t info[0x40]; int ok = 0;
    int nkeys = is_self ? (int)(sizeof self_keys / sizeof *self_keys) : 1;
    for (int k = 0; k < nkeys && !ok; k++) {
        uint8_t erk[32], riv[16];
        if (is_self) { hex(self_keys[k].erk, erk, 32); hex(self_keys[k].riv, riv, 16); } else { memcpy(erk, SCEPKG_ERK, 32); memcpy(riv, SCEPKG_RIV, 16); }
        memcpy(info, f + mi, 0x40);
        if (aes_cbc_dec(erk, riv, info, 0x40)) return -1;
        ok = 1; for (int i = 0; i < 16; i++) if (info[0x10 + i] || info[0x30 + i]) ok = 0;   /* key / iv padding */
    }
    if (!ok) return -2;
    size_t hl = (size_t)hsize - (mi + 0x40);
    uint8_t *h = malloc(hl); memcpy(h, f + mi + 0x40, hl);
    aes_ctr(info, info + 0x20, h, hl);
    uint32_t nsec = be32(h + 0x0C), nk = be32(h + 0x10);
    if (nsec > 64 || 0x20 + (size_t)nsec * 0x30 + (size_t)nk * 16 > hl) { free(h); return -3; }
    const uint8_t *keys = h + 0x20 + nsec * 0x30;
    for (uint32_t s = 0; s < nsec; s++) {
        const uint8_t *sh = h + 0x20 + s * 0x30;
        uint64_t off = be64(sh), sz = be64(sh + 8);
        uint32_t enc = be32(sh + 0x20), ki = be32(sh + 0x24), ii = be32(sh + 0x28), comp = be32(sh + 0x2C);
        if (off + sz > flen) { free(h); return -4; }
        uint8_t *b = malloc((size_t)sz ? (size_t)sz : 1); memcpy(b, f + off, (size_t)sz);
        if (enc == 3 && ki < nk && ii < nk) aes_ctr(keys + 16 * ki, keys + 16 * ii, b, (size_t)sz);
        size_t outn = (size_t)sz;
        if (comp == 2) { uint8_t *u = png_zlib_inflate(b, (size_t)sz, &outn); free(b); if (!u) { free(h); return -5; } b = u; }
        o->data = realloc(o->data, o->len + outn + 1); memcpy(o->data + o->len, b, outn); free(b);
        o->off[o->n] = o->len; o->size[o->n] = outn; o->n++; o->len += outn;
    }
    free(h);
    return 0;
}

/* ---- tar: the member called `name` (ustar, 512-byte blocks) */
static const uint8_t *tar_find(const uint8_t *t, size_t n, const char *name, size_t *len, int prefix) {
    for (size_t p = 0; p + 512 <= n; ) {
        const uint8_t *hd = t + p;
        if (!hd[0]) break;
        char nm[256] = {0}; memcpy(nm, hd, 100);
        if (!memcmp(hd + 257, "ustar", 5) && hd[345]) { char full[256]; snprintf(full, sizeof full, "%.155s/%s", (const char *)hd + 345, nm); strcpy(nm, full); }
        size_t sz = (size_t)strtoull((const char *)hd + 124, NULL, 8);
        if (prefix ? !strncmp(nm, name, strlen(name)) : !strcmp(nm, name)) { *len = sz; return hd + 512; }
        p += 512 + (sz + 511) / 512 * 512;
    }
    return NULL;
}

/* Extract both libraries into out_dir. 0 when both were written. */
int fw_extract_voice(const char *pup_path, const char *out_dir) {
    FILE *fp = fopen(pup_path, "rb");
    if (!fp) { printf("[voice] no system update file at %s\n", pup_path); return -1; }
    uint8_t hd[0x30];
    if (fread(hd, 1, 0x30, fp) != 0x30 || memcmp(hd, "SCEUF", 5)) { fclose(fp); return -1; }
    uint64_t nfiles = be64(hd + 0x18), tar_off = 0, tar_len = 0;
    for (uint64_t i = 0; i < nfiles && i < 64; i++) {
        uint8_t e[0x20]; if (fread(e, 1, 0x20, fp) != 0x20) break;
        if (be64(e) == 0x300) { tar_off = be64(e + 8); tar_len = be64(e + 16); }
    }
    if (!tar_len) { fclose(fp); return -1; }
    uint8_t *tar = malloc((size_t)tar_len);
    _fseeki64(fp, (long long)tar_off, SEEK_SET);
    size_t got = fread(tar, 1, (size_t)tar_len, fp); fclose(fp);
    if (got != tar_len) { free(tar); return -1; }
    static const char *want[2] = {"dev_flash/sys/external/libcelpenc.sprx", "dev_flash/sys/external/libcelpdec.sprx"};
    static const char *outn[2] = {"libcelpenc.prx", "libcelpdec.prx"};
    int done = 0;
    /* each dev_flash package is an SCE container whose last section is a tar */
    for (size_t p = 0; p + 512 <= tar_len && done != 3; ) {
        const uint8_t *th = tar + p;
        if (!th[0]) break;
        size_t sz = (size_t)strtoull((const char *)th + 124, NULL, 8);
        if (!strncmp((const char *)th, "dev_flash", 9)) {
            sce_out_t pk;
            if (!sce_decrypt(th + 512, sz, 0, &pk) && pk.n) {
                const uint8_t *inner = pk.data + pk.off[pk.n - 1]; size_t il = pk.size[pk.n - 1];
                for (int w = 0; w < 2; w++) {
                    size_t sl; const uint8_t *self = (done & (1 << w)) ? NULL : tar_find(inner, il, want[w], &sl, 0);
                    if (!self) continue;
                    sce_out_t el; int r = sce_decrypt(self, sl, 1, &el);
                    if (r) { printf("[voice] %s: could not decrypt (%d)\n", want[w], r); continue; }
                    char path[MAX_PATH], tmp[MAX_PATH + 32]; snprintf(path, sizeof path, "%s\\%s", out_dir, outn[w]);
                    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, GetCurrentProcessId());   /* two games extracting at once */
                    CreateDirectoryA(out_dir, NULL);
                    FILE *o = fopen(tmp, "wb");
                    if (o) {
                        int wr = fwrite(el.data, 1, el.len, o) == el.len; fclose(o);
                        if (wr && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { done |= 1 << w; printf("[voice] %s extracted from the system update (%zu bytes)\n", outn[w], el.len); }
                        else DeleteFileA(tmp);
                    }
                    free(el.data);
                }
            }
            free(pk.data);
        }
        p += 512 + (sz + 511) / 512 * 512;
    }
    free(tar);
    return done == 3 ? 0 : -1;
}

#ifdef FWEXTRACT_MAIN
int main(int argc, char **argv) { return fw_extract_voice(argv[1], argv[2]) ? 1 : 0; }
#endif
