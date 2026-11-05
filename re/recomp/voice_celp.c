/* The PS3 firmware's MPEG-4 CELP voice codecs (wideband RPE, 16 kHz), run on our SPU interpreter so the voice we send is
 * what PS3 and RPCS3 players' firmware decodes, and theirs decodes here.
 *
 * The codecs are SPURS tasks embedded in libcelpenc.sprx / libcelpdec.sprx. Their PPU libraries talk to the task through
 * two SPURS queues of 128-byte entries; here the task's own queue calls are intercepted instead (pop: the command is put
 * in local store; push: the result is taken), so no SPURS is needed:
 * - encoder: one task run per command (as libcelpenc does: it creates the task for each). Command {u64 pcm ea, u64 au ea,
 *   u32 +0x10 pcm bytes, u32 +0x14 op, u64 +0x20 state ea, u32 +0x28 state size 0x4c80}; op 0x1c00 | configuration << 4 |
 *   0x200 (float samples) starts, 0x2000 encodes a frame. The state is DMAed in and out each run. Result: +0x18 status
 *   (0x1000 ok), +0x1c bytes in the AU (0 for the first two frames: look-ahead).
 * - decoder: one task that stays alive, popping commands {+0 channel, +4 op (2 start, 4 decode), +0x18 AU bytes,
 *   +0x20 pcm ea, +0x24 pcm bytes, +0x28 excitation 1 (RPE), sample rate 2 (16 kHz), configuration, word size 1 (float),
 *   AU bytes inline at +0x40}. Result: +8 status (0 ok). The channel selects one of its 64 decoder states (one per talker).
 * Samples are big-endian floats in -1..1.
 *
 * The libraries: <MNR>\firmware\libcelpenc.prx and libcelpdec.prx, extracted on first use from the system update that
 * comes with the game (extracted\PS3_UPDATE\PS3UPDAT.PUP, fwextract.c), or RPCS3's `--decrypt` of a newer firmware's
 * dev_flash\sys\external\*.sprx put there by hand. Without them voice is neither sent nor heard.
 * Where the queue calls are differs between firmware versions, so they are found in the program (find_queue_calls). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "spu_core.h"
#include "voice.h"
#include "host.h"
#include "fwextract.h"

uint32_t (*voice_guest_alloc)(uint32_t size, uint32_t align);   /* set by host.c */
volatile long voice_codec_missing;   /* no firmware library found (overlay tells the player what to do) */

typedef struct {   /* one firmware SPU program and where its queue calls are */
    const char *file; uint8_t *elf; size_t size;
    uint32_t pop, popend, push, pushend;
    int tried, ok;
} fw_t;
static fw_t fw_enc = {"libcelpenc.prx"};   /* 4.93: pop 0x10028 / 0x104b8, push 0xf810 / 0xfca0; 3.21: 0xff18 / 0x103a8, 0xf770 / 0xfc00 */
static fw_t fw_dec = {"libcelpdec.prx"};   /* 4.93: 0x7660 / 0x7af0, 0x6e48 / 0x72d8; 3.21: 0x7638 / 0x7ac8, 0x6e90 / 0x7320 */
static SRWLOCK fw_lock = SRWLOCK_INIT;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void load_ls(spu_t *s, const fw_t *f) {
    memset(s->ls, 0, LS_SIZE); memset(s->r, 0, sizeof s->r);
    const uint8_t *e = f->elf;
    uint32_t phoff = be32(e + 0x1C), phnum = (uint32_t)e[0x2C] << 8 | e[0x2D];
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + 32 * i;
        if (be32(ph) != 1) continue;
        uint32_t off = be32(ph + 4), va = be32(ph + 8), fsz = be32(ph + 16);
        if (va + fsz <= LS_SIZE && off + fsz <= f->size) memcpy(s->ls + va, e + off, fsz);
    }
    s->stopped = 0; s->steps = 0; s->rsv_ea = 0xFFFFFFFF; s->atomic_stat = 0;
    s->r[1].w[0] = 0x3FFF0; s->pc = be32(e + 0x18);
}
/* The task's SPURS queue calls, found by what they do: the begin calls are the functions that check the queue's direction
 * (lqa from LS 0x90, then ceqi 1 = SPU pops (PPU to SPU), 2 = SPU pushes); of these, the one the task calls with its DMA
 * tag in r5 (il r5, tag) is used, and its end call is the next call (brsl, or a tail `br`) after it that passes the same
 * tag in r4. */
static int find_queue_calls(const spu_t *s, uint32_t end, fw_t *f) {
    enum { MAXC = 2048 };
    static uint32_t site[MAXC], tgt[MAXC]; int nc = 0;
    static uint32_t tsite[MAXC], ttgt[MAXC]; int nt = 0;
    for (uint32_t pc = 0x3000; pc + 4 <= end; pc += 4) {
        uint32_t w = rd32(s, pc), t = (pc + (uint32_t)(sext((w >> 7) & 0xFFFF, 16) << 2)) & LS_MASK;
        if (w >> 23 == 0x66 && nc < MAXC) { site[nc] = pc; tgt[nc++] = t; }
        if (w >> 23 == 0x64 && (t > pc + 0x400 || t + 0x400 < pc) && nt < MAXC) { tsite[nt] = pc; ttgt[nt++] = t; }
    }
    uint32_t found[3][2] = {{0}};   /* [direction][begin, end] */
    for (uint32_t pc = 0x3000; pc + 16 <= end; pc += 4) {
        uint32_t w = rd32(s, pc);
        if (!(w >> 23 == 0x61 && ((w >> 7) & 0xFFFF) == 0x90 >> 2)) continue;   /* lqa rX, 0x90 */
        for (int k = 1; k < 4; k++) {
            uint32_t w2 = rd32(s, pc + 4 * k); int dir = sext((w2 >> 14) & 0x3FF, 10);
            if (w2 >> 24 != 0x7C || (dir != 1 && dir != 2) || found[dir][0]) continue;   /* ceqi */
            uint32_t fn = 0; for (int i = 0; i < nc; i++) if (tgt[i] <= pc && tgt[i] > fn) fn = tgt[i];   /* the function it is in */
            for (int i = 0; i < nc && fn && !found[dir][0]; i++) {
                if (tgt[i] != fn) continue;
                int tag = -1;
                for (int b = 1; b < 8 && tag < 0; b++) { uint32_t x = rd32(s, site[i] - 4 * b); if (x >> 23 == 0x81 && (x & 0x7F) == 5) tag = sext((x >> 7) & 0xFFFF, 16); }
                if (tag < 0) continue;
                uint32_t best = 0, best_site = 0xFFFFFFFF;   /* the nearest later call with il r4, tag before it */
                for (int pass = 0; pass < 2; pass++)
                    for (int j = 0; j < (pass ? nt : nc); j++) {
                        uint32_t s2 = pass ? tsite[j] : site[j], t2 = pass ? ttgt[j] : tgt[j];
                        if (s2 <= site[i] || s2 > site[i] + 0x100 || s2 >= best_site) continue;
                        for (int b = 1; b < 12; b++) { uint32_t x = rd32(s, s2 - 4 * b); if (x >> 23 == 0x81 && (x & 0x7F) == 4 && sext((x >> 7) & 0xFFFF, 16) == tag) { best = t2; best_site = s2; break; } }
                    }
                if (best) { found[dir][0] = fn; found[dir][1] = best; }
            }
        }
    }
    f->pop = found[1][0]; f->popend = found[1][1]; f->push = found[2][0]; f->pushend = found[2][1];
    return f->pop && f->popend && f->push && f->pushend;
}
static const struct { const char *fmt; int home; } pup_sources[] = {
    { "%s\\extracted\\PS3_UPDATE\\PS3UPDAT.PUP", 0 },
    { "%s\\PS3UPDAT.PUP", 0 },
    { "%s\\firmware\\PS3UPDAT.PUP", 0 },
    { "%s\\Downloads\\PS3UPDAT.PUP", 1 },
};
static FILE *open_firmware_from_pup(const char *path, const char *dir) {
    const char *home = getenv("USERPROFILE");
    char pup[MAX_PATH];
    for (size_t i = 0; i < sizeof pup_sources / sizeof pup_sources[0]; i++) {
        const char *base = pup_sources[i].home ? home : mnr_root_dir();
        if (!base) continue;
        snprintf(pup, sizeof pup, pup_sources[i].fmt, base);
        if (GetFileAttributesA(pup) == INVALID_FILE_ATTRIBUTES) continue;
        fw_extract_voice(pup, dir);
        FILE *fp = fopen(path, "rb");
        if (fp) return fp;
    }
    return NULL;
}
static int fw_load(fw_t *f) {
    AcquireSRWLockExclusive(&fw_lock);
    if (!f->tried) {
        f->tried = 1;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\firmware\\%s", mnr_root_dir(), f->file);
        FILE *fp = fopen(path, "rb");
        if (!fp) {   /* first use: from a PS3 system update: the disc's, or one the player downloaded (playstation.com) */
            char dir[MAX_PATH];
            snprintf(dir, sizeof dir, "%s\\firmware", mnr_root_dir());
            fp = open_firmware_from_pup(path, dir);
        }
        uint8_t *d = NULL; long n = 0;
        if (fp) { fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET); d = malloc(n); if (fread(d, 1, n, fp) != (size_t)n) n = 0; fclose(fp); }
        /* the SPU program: the ELF (machine 23) inside the PRX */
        for (long o = 1; d && o + 0x34 < n; o++)
            if (!memcmp(d + o, "\x7F" "ELF", 4) && d[o + 18] == 0 && d[o + 19] == 23) { f->elf = d + o; f->size = (size_t)(n - o); break; }
        if (f->elf) {
            static spu_t probe; load_ls(&probe, f);
            uint32_t end = 0, phoff = be32(f->elf + 0x1C), phnum = (uint32_t)f->elf[0x2C] << 8 | f->elf[0x2D];
            for (uint32_t i = 0; i < phnum; i++) { const uint8_t *ph = f->elf + phoff + 32 * i; if (be32(ph) == 1 && be32(ph + 8) + be32(ph + 16) > end) end = be32(ph + 8) + be32(ph + 16); }
            f->ok = find_queue_calls(&probe, end > LS_SIZE ? LS_SIZE : end, f);
        }
        if (f->ok) printf("[voice] %s: ready (queue calls %05x %05x / %05x %05x)\n", f->file, f->pop, f->popend, f->push, f->pushend);
        else printf("[voice] %s: %s\n", f->file, !d ? "not found: voice chat needs a PS3 system update file. Download PS3UPDAT.PUP from playstation.com and put it in the MNR folder (or Downloads)" : !f->elf ? "no SPU program in it" : "unknown firmware version");
    }
    int ok = f->ok;
    ReleaseSRWLockExclusive(&fw_lock);
    return ok;
}

/* Run the task until it has pushed a result. persistent: stop when it asks for the next command (and resume there next
 * time); otherwise run it until it exits. 1: a result in res, 0: none, -1: it went wrong. */
static int run_(spu_t *s, const fw_t *f, const uint8_t cmd[0x80], uint8_t res[0x80], int persistent);
/* the task may only write its own scratch memory: anything else is dropped and reported (spu_core.h) */
static int run(spu_t *s, const fw_t *f, const uint8_t cmd[0x80], uint8_t res[0x80], int persistent, uint32_t mem) {
    spu_guard_lo = mem; spu_guard_hi = mem + 0x5200;
    int r = run_(s, f, cmd, res, persistent);
    spu_guard_lo = spu_guard_hi = 0;
    return r;
}
static int run_(spu_t *s, const fw_t *f, const uint8_t cmd[0x80], uint8_t res[0x80], int persistent) {
    int fed = 0, pushed = 0; uint64_t steps = 0;
    while (!s->stopped && steps < 50000000ull) {
        uint32_t pc = s->pc;
        if (pc == f->pop) {
            if (fed) return pushed;
            memcpy(s->ls + (s->r[4].w[0] & LS_MASK & ~15u), cmd, 0x80); fed = 1;
            s->r[3] = splat(0); s->pc = s->r[0].w[0] & LS_MASK; continue;
        }
        if (pc == f->popend || pc == f->pushend) { s->r[3] = splat(0); s->pc = s->r[0].w[0] & LS_MASK; continue; }
        if (pc == f->push) { memcpy(res, s->ls + (s->r[4].w[0] & LS_MASK & ~15u), 0x80); pushed = 1; s->r[3] = splat(0); s->pc = s->r[0].w[0] & LS_MASK; continue; }
        if (pc < 0x3000) return persistent ? -1 : pushed;   /* the task exited (into the SPURS kernel) */
        step(s); steps++;
    }
    return -1;
}
static void put32(uint8_t *p, int o, uint32_t v) { p[o] = (uint8_t)(v >> 24); p[o + 1] = (uint8_t)(v >> 16); p[o + 2] = (uint8_t)(v >> 8); p[o + 3] = (uint8_t)v; }

typedef struct { int encode; uint32_t bitrate, cfg, samples, bytes, mem; spu_t *s; } celp_t;
enum { M_PCM = 0, M_AU = 0x400, M_STATE = 0x500, M_SIZE = 0x5200 };   /* guest scratch per codec */

void *voice_celp_open(uint32_t bitrate, int encode) {
    if (bitrate != 16000 || !voice_guest_alloc) return NULL;   /* the 8 kHz (CELP8) and other RPE rates: not set up */
    if (!fw_load(encode ? &fw_enc : &fw_dec)) { if (!(encode ? fw_enc : fw_dec).elf) voice_codec_missing = 1; return NULL; }
    celp_t *c = calloc(1, sizeof *c);
    c->encode = encode; c->bitrate = bitrate; c->cfg = 1; c->samples = 160; c->bytes = 20;
    c->mem = voice_guest_alloc(M_SIZE, 128); mem_zero(c->mem, M_SIZE);
    c->s = malloc(sizeof *c->s);
    uint8_t cmd[0x80] = {0}, res[0x80];
    int r;
    if (encode) {
        put32(cmd, 4, c->mem + M_PCM); put32(cmd, 12, c->mem + M_AU);
        put32(cmd, 0x14, 0x1c00 | c->cfg << 4 | 0x200); put32(cmd, 0x24, c->mem + M_STATE); put32(cmd, 0x28, 0x4c80);
        load_ls(c->s, &fw_enc);
        r = run(c->s, &fw_enc, cmd, res, 0, c->mem);
        if (r != 1 || be32(res + 0x18) != 0x1000) r = -1;
    } else {
        put32(cmd, 4, 2); put32(cmd, 0x28, 1); put32(cmd, 0x2c, 2); put32(cmd, 0x30, c->cfg); put32(cmd, 0x34, 1);
        load_ls(c->s, &fw_dec);
        r = run(c->s, &fw_dec, cmd, res, 1, c->mem);
        if (r != 1 || be32(res + 8) != 0) r = -1;
    }
    if (r < 0) { printf("[voice] the firmware %s did not start\n", encode ? "encoder" : "decoder"); voice_celp_close(c); return NULL; }
    return c;
}
void voice_celp_close(void *codec) {
    celp_t *c = codec;
    if (!c) return;
    free(c->s); free(c);   /* (the guest scratch stays: a few KB per port the game ever opened) */
}
int voice_celp_encode(void *codec, const float *pcm, uint8_t *out) {
    celp_t *c = codec;
    for (uint32_t i = 0; i < c->samples; i++) { float v = pcm[i] / 32768.0f; st_f32(c->mem + M_PCM + 4 * i, v > 1 ? 1 : v < -1 ? -1 : v); }
    uint8_t cmd[0x80] = {0}, res[0x80];
    put32(cmd, 4, c->mem + M_PCM); put32(cmd, 12, c->mem + M_AU); put32(cmd, 0x10, c->samples * 4);
    put32(cmd, 0x14, 0x2000); put32(cmd, 0x24, c->mem + M_STATE); put32(cmd, 0x28, 0x4c80);
    load_ls(c->s, &fw_enc);
    if (run(c->s, &fw_enc, cmd, res, 0, c->mem) != 1 || be32(res + 0x18) != 0x1000) return -1;
    uint32_t n = be32(res + 0x1c);
    if (n != 0 && n != c->bytes) return -1;
    memcpy(out, gp(c->mem + M_AU), n);
    return (int)n;
}
int voice_celp_decode(void *codec, const uint8_t *in, float *out) {
    celp_t *c = codec;
    { static int skip = -1; if (skip < 0) skip = getenv("MNR_VOICE_NODECODE") != NULL; if (skip) { memset(out, 0, c->samples * sizeof(float)); return (int)c->samples; } }   /* (debug) */
    uint8_t cmd[0x80] = {0}, res[0x80];
    put32(cmd, 0, 0); put32(cmd, 4, 4); put32(cmd, 0x18, c->bytes); put32(cmd, 0x20, c->mem + M_PCM); put32(cmd, 0x24, c->samples * 4);
    put32(cmd, 0x28, 1); put32(cmd, 0x2c, 2); put32(cmd, 0x30, c->cfg); put32(cmd, 0x34, 1);
    memcpy(cmd + 0x40, in, c->bytes);
    if (run(c->s, &fw_dec, cmd, res, 1, c->mem) != 1 || (int32_t)be32(res + 8) != 0) return -1;
    for (uint32_t i = 0; i < c->samples; i++) out[i] = ld_f32(c->mem + M_PCM + 4 * i) * 32768.0f;
    return (int)c->samples;
}
