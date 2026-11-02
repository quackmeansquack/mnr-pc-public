/* cellAudio: up to 8 ports, each a ring of nBlock blocks of 256 samples x nChannel big-endian floats in
 * guest memory, with a 64-bit read index the game polls. A host thread advances every started port's
 * read index at the console's rate (48 kHz, 256-sample blocks = 5.33 ms), mixes the block it just
 * "played" from every started port down to stereo, and streams the mix to the Windows waveOut API.
 * MNR_MUTE=1 keeps the timing but plays nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <math.h>
#include <mmsystem.h>
#include "runtime.h"
#include "audio.h"

#define MAX_PORTS 8
#define BLOCK 256
typedef struct { int open, started; uint32_t addr, nch, nblk, size, index_addr; uint64_t blk; float level; int filling; float last_l, last_r; } port_t;
static port_t ports[MAX_PORTS];
static uint32_t index_area;   /* guest: 8 x u64 read indices */
static SRWLOCK lock = SRWLOCK_INIT;

/* ---- waveOut stream: 16 buffers of 4 blocks (1024 stereo frames, ~21 ms) */
#define NBUF 16
#define BUF_FRAMES (4 * BLOCK)
static HWAVEOUT wo;
static WAVEHDR hdr[NBUF];
static int16_t pcm[NBUF][BUF_FRAMES * 2];
static int cur_buf, cur_frames, muted = -1;
/* When the device runs dry (nothing queued), the next buffer goes out behind two buffers of silence so
 * scheduling jitter doesn't starve it again at once: without a cushion every hiccup was a click, and under
 * load (movies) the clicks ran together into warble. MNR_AUDIO_SILENT=1 drives the device with silent samples
 * and reports how often it ran dry (testing without sound). */
static WAVEHDR sil_hdr[2];
static int16_t sil_pcm[BUF_FRAMES * 2];
static volatile long starved;
static int silent_test;

/* The game's overall volume (keys - and = in the game window, saved in pc_settings.ini [audio] volume=0..100) and the
 * voice chat's headset output (voice.c: 16 kHz, made 48 kHz here) */
static volatile LONG master_pct = -1;
static float master(void) { return master_pct < 0 ? 1.0f : master_pct / 100.0f; }
volatile DWORD audio_volume_changed;   /* GetTickCount of the last change (the overlay shows the level a moment) */
int audio_volume_pct(void) { return master_pct < 0 ? 100 : (int)master_pct; }
void audio_volume_step(int dir) {
    extern const char *mnr_settings_path(void);
    LONG v = audio_volume_pct() + 10 * dir; v = v < 0 ? 0 : v > 100 ? 100 : v;
    master_pct = v; audio_volume_changed = GetTickCount() | 1;
    char b[16]; snprintf(b, sizeof b, "%ld", v);
    WritePrivateProfileStringA("audio", "volume", b, mnr_settings_path());
}
#define VRING (48000 / 2)
static float vring[VRING]; static volatile LONG vr_head, vr_count; static float v_last; static SRWLOCK vlock = SRWLOCK_INIT;
void audio_voice_feed(const float *pcm, int n) {   /* 16 kHz samples (16-bit scale) */
    AcquireSRWLockExclusive(&vlock);
    for (int i = 0; i < n; i++) for (int k = 1; k <= 3; k++) {
        if (vr_count == VRING) { vr_head = (vr_head + 1) % VRING; vr_count--; }   /* full: drop the oldest */
        vring[(vr_head + vr_count++) % VRING] = (v_last + (pcm[i] - v_last) * k / 3.0f) / 32768.0f;
    }
    if (n) v_last = pcm[n - 1];
    ReleaseSRWLockExclusive(&vlock);
}
static void voice_mix(float *lr, int n) {
    AcquireSRWLockExclusive(&vlock);
    /* x2.5 (a headset on the PS3 has voice on its own, here it competes with the game), softly limited */
    for (int i = 0; i < n && vr_count; i++) { float v = vring[vr_head] * 2.5f; vr_head = (vr_head + 1) % VRING; vr_count--; v = v / (1.0f + fabsf(v) * 0.5f) * 1.5f; lr[2 * i] += v; lr[2 * i + 1] += v; }
    ReleaseSRWLockExclusive(&vlock);
}

static void out_open(void) {
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = NULL; return; }
    for (int i = 0; i < NBUF; i++) { hdr[i].lpData = (LPSTR)pcm[i]; hdr[i].dwBufferLength = sizeof pcm[i]; hdr[i].dwFlags = WHDR_DONE; }
    for (int i = 0; i < 2; i++) { sil_hdr[i].lpData = (LPSTR)sil_pcm; sil_hdr[i].dwBufferLength = sizeof sil_pcm; sil_hdr[i].dwFlags = WHDR_DONE; }
    /* full app volume: Windows keeps an app's volume between runs, and earlier silent test runs set it to 0 */
    waveOutSetVolume(wo, 0xFFFFFFFF);
}
static void out_prime_if_dry(void) {
    for (int i = 0; i < NBUF; i++) if (!(hdr[i].dwFlags & WHDR_DONE)) return;   /* something still queued */
    for (int i = 0; i < 2; i++) if (!(sil_hdr[i].dwFlags & WHDR_DONE)) return;
    starved++;
    for (int i = 0; i < 2; i++) {
        WAVEHDR *h = &sil_hdr[i];
        if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
        h->dwFlags = 0; waveOutPrepareHeader(wo, h, sizeof *h); waveOutWrite(wo, h, sizeof *h);
    }
}
/* Races mix many loud sounds at once and the surround downmix adds channels together, so peaks go past full
 * scale: hard clipping crackles. Above 0.9 the level bends smoothly towards 1.0 instead. */
static volatile long st_clipped, st_late, st_missed;
static float soft_limit(float v) {
    float a = fabsf(v);
    if (a <= 0.9f) return v;
    if (a > 1.0f) st_clipped++;
    a = 0.9f + 0.1f * tanhf((a - 0.9f) / 0.1f);
    return v < 0 ? -a : a;
}
static void out_frames(const float *lr, int n) {   /* n stereo frames */
    if (!wo) return;
    for (int i = 0; i < n; i++) {
        WAVEHDR *h = &hdr[cur_buf];
        if (cur_frames == 0) {
            if (!(h->dwFlags & WHDR_DONE)) return;   /* device behind: drop rather than stall the game */
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
        }
        for (int k = 0; k < 2; k++) {
            float v = soft_limit(lr[2 * i + k] * master());
            v = silent_test ? 0.0f : v * 32767.0f;   /* silent test: real timing, no sound */
            pcm[cur_buf][2 * cur_frames + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        if (++cur_frames == BUF_FRAMES) {
            out_prime_if_dry();
            h->dwFlags = 0; h->dwBufferLength = sizeof pcm[cur_buf];
            waveOutPrepareHeader(wo, h, sizeof *h);
            waveOutWrite(wo, h, sizeof *h);
            cur_buf = (cur_buf + 1) % NBUF; cur_frames = 0;
        }
    }
}

/* Like RPCS3, a played block is cleared and tagged (a NaN in its first and last float the game never writes),
 * so a block the game hasn't refilled in time shows: it plays as silence, not as the stale audio from a ring
 * ago, which repeated a few ms of sound out of place (crackling when the game fell behind in races). */
#define TAG 0x7FC0A0D1u
static uint32_t blk_base(port_t *p, uint64_t b) { return p->addr + (uint32_t)(b % p->nblk) * p->nch * BLOCK * 4; }
static uint32_t blk_last(port_t *p, uint64_t b) { return blk_base(p, b) + (BLOCK - 1) * p->nch * 4; }   /* channel 0 (some games fill only the front) */
static int blk_state(port_t *p, uint64_t b) {   /* 0 refilled, 1 untouched, 2 being written */
    int first = ld_u32(blk_base(p, b)) == TAG, last = ld_u32(blk_last(p, b)) == TAG;
    return first && last ? 1 : last ? 2 : 0;
}
static void blk_clear(port_t *p, uint64_t b) {
    mem_zero(blk_base(p, b), p->nch * BLOCK * 4);
    st_u32(blk_base(p, b), TAG); st_u32(blk_last(p, b), TAG);
}
static void mix_block(port_t *p, uint64_t b, float *lr) {   /* add block b of port p into lr[256 * 2] */
    uint32_t base = blk_base(p, b);
    int st = blk_state(p, b);
    if (st == 1) {   /* nothing new: fade out from where the sound stopped instead of cutting to silence */
        for (int s = 0; s < 64; s++) { float g = 1.0f - s / 64.0f; lr[2 * s] += p->last_l * g; lr[2 * s + 1] += p->last_r * g; }
        p->last_l = p->last_r = 0;
        return;
    }
    if (ld_u32(base) == TAG) st_u32(base, 0);
    if (ld_u32(blk_last(p, b)) == TAG) st_u32(blk_last(p, b), 0);
    for (int s = 0; s < BLOCK; s++) {
        float ch[8] = {0};
        for (uint32_t c = 0; c < p->nch && c < 8; c++) ch[c] = ld_f32(base + (s * p->nch + c) * 4);
        float l, r;
        if (p->nch >= 6) {   /* L R C LFE Ls Rs [Lb Rb] */
            l = ch[0] + 0.707f * ch[2] + 0.707f * ch[4] + 0.5f * ch[6];
            r = ch[1] + 0.707f * ch[2] + 0.707f * ch[5] + 0.5f * ch[7];
        } else if (p->nch == 1) l = r = ch[0];
        else { l = ch[0]; r = ch[1]; }
        lr[2 * s] += l * p->level; lr[2 * s + 1] += r * p->level;
        p->last_l = l * p->level; p->last_r = r * p->level;
    }
}

/* MNR_AUDIO_WAV=<file>: also write the mix to a 16-bit stereo WAV (checking sound without speakers) */
static void wav_frames(const float *lr, int n) {
    static FILE *f; static int tried; static uint32_t bytes, since;
    if (!tried) {
        tried = 1;
        const char *e = getenv("MNR_AUDIO_WAV");
        if (e && (f = fopen(e, "wb"))) { static const uint8_t z[44] = {0}; fwrite(z, 1, 44, f); }
    }
    if (!f) return;
    int16_t b[512 * 2];
    for (int i = 0; i < 2 * n && i < 1024; i++) { float v = lr[i] * 32767.0f; b[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    fwrite(b, 2, 2 * n, f); bytes += 4 * n; since += n;
    if (since >= 48000) {   /* refresh the header about once a second */
        since = 0;
        uint8_t h[44] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0,0xEE,2,0, 4,0, 16,0, 'd','a','t','a'};
        uint32_t riff = 36 + bytes; memcpy(h + 4, &riff, 4); memcpy(h + 40, &bytes, 4);
        long pos = ftell(f); fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fseek(f, pos, SEEK_SET); fflush(f);
    }
}

static int portlog;
static DWORD WINAPI audio_thread(void *arg) {
    portlog = getenv("MNR_AUDIO_PORTLOG") != NULL;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    uint64_t done = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);   /* the game threads must not delay it */
    if (!muted) out_open();
    DWORD last_report = GetTickCount(), last_stats = last_report; long last_starved = 0;
    LONGLONG wait_from = 0;
    for (;;) {
        Sleep(1);
        if (GetTickCount() - last_stats >= 30000) {   /* how the game kept up, every 30 s when anything happened */
            last_stats = GetTickCount();
            if (st_late || st_missed || st_clipped)
                printf("[audio] last 30 s: %ld blocks late, %ld missed, %ld samples over full scale\n", st_late, st_missed, st_clipped);
            st_late = st_missed = st_clipped = 0;
        }
        if (silent_test && GetTickCount() - last_report >= 5000) {
            last_report = GetTickCount();
            if (starved != last_starved) printf("[audio] device ran dry %ld times so far\n", starved);
            last_starved = starved;
        }
        QueryPerformanceCounter(&t);
        uint64_t due = (uint64_t)(t.QuadPart - t0.QuadPart) * 48000 / BLOCK / f.QuadPart;
        if (due > done + 32) done = due - 32;   /* after a long stall don't try to catch up forever */
        while (done < due) {
            /* RPCS3's rule: a port the game is filling gets up to two periods' grace for a block it hasn't
             * refilled yet (it falls behind for a moment), then the block plays as it is */
            int late = 0;
            AcquireSRWLockShared(&lock);
            for (int i = 0; i < MAX_PORTS; i++) {
                port_t *p = &ports[i];
                if (!p->open || !p->started) continue;
                int st = blk_state(p, p->blk);
                if (st == 2 || (st == 1 && p->filling)) late = 1;
            }
            ReleaseSRWLockShared(&lock);
            if (late) {
                QueryPerformanceCounter(&t);
                if (!wait_from) wait_from = t.QuadPart;
                if ((uint64_t)(t.QuadPart - wait_from) * 48000 / f.QuadPart < 2 * BLOCK) break;   /* look again next ms */
                st_missed++;
            } else if (wait_from) st_late++;
            wait_from = 0;
            done++;
            float lr[BLOCK * 2] = {0};
            int any = 0;
            AcquireSRWLockShared(&lock);
            for (int i = 0; i < MAX_PORTS; i++) {
                port_t *p = &ports[i];
                if (!p->open || !p->started) continue;
                if (portlog) {   /* MNR_AUDIO_PORTLOG=1: raw level per port, about once a second */
                    uint32_t base = p->addr + (uint32_t)(p->blk % p->nblk) * p->nch * BLOCK * 4; double sq = 0; float mx = 0;
                    for (uint32_t k = 0; k < p->nch * BLOCK; k++) { float v = ld_f32(base + 4 * k); sq += (double)v * v; if (fabsf(v) > mx) mx = fabsf(v); }
                    if (done % 188 == 0) printf("[audio] port %d (%u ch, level %.2f): rms %.4f max %.4f\n", i, p->nch, p->level, sqrt(sq / (p->nch * BLOCK)), mx);
                }
                int st = blk_state(p, p->blk);
                if (st != 1) p->filling = 1; else if (p->filling) p->filling = 0;
                mix_block(p, p->blk, lr);
                blk_clear(p, p->blk);
                p->blk++;
                st_u64(p->index_addr, p->blk % p->nblk);
                any = 1;
            }
            ReleaseSRWLockShared(&lock);
            if (any) voice_mix(lr, BLOCK);
            if (any && !muted) out_frames(lr, BLOCK);
            if (any) wav_frames(lr, BLOCK);
        }
    }
    return 0;
}

uint32_t (*audio_guest_alloc)(uint32_t size, uint32_t align);

int audio_init(void) {
    if (index_area) return 0;
    if (muted < 0) { const char *e = getenv("MNR_MUTE"); muted = e && *e == '1'; }
    if (getenv("MNR_AUDIO_SILENT")) { silent_test = 1; muted = 0; }
    { extern const char *mnr_settings_path(void); int v = GetPrivateProfileIntA("audio", "volume", 100, mnr_settings_path()); master_pct = v < 0 ? 0 : v > 100 ? 100 : v; }
    index_area = audio_guest_alloc(MAX_PORTS * 8, 0x80);
    mem_zero(index_area, MAX_PORTS * 8);
    CreateThread(NULL, 1 << 16, audio_thread, NULL, 0, NULL);
    return 0;
}
int audio_port_open(uint32_t nch, uint32_t nblk, float level, uint32_t *port) {
    AcquireSRWLockExclusive(&lock);
    int i = 0;
    while (i < MAX_PORTS && ports[i].open) i++;
    if (i == MAX_PORTS) { ReleaseSRWLockExclusive(&lock); return -1; }
    port_t *p = &ports[i];
    memset(p, 0, sizeof *p);
    p->nch = nch ? nch : 2; p->nblk = nblk ? nblk : 8; p->level = level > 0 ? level : 1.0f;
    p->size = p->nch * p->nblk * BLOCK * 4;
    p->addr = audio_guest_alloc(p->size, 0x10000);
    mem_zero(p->addr, p->size);
    p->index_addr = index_area + 8 * i;
    st_u64(p->index_addr, 0);
    p->open = 1;
    ReleaseSRWLockExclusive(&lock);
    *port = (uint32_t)i;
    printf("[audio] port %d open: %u channels, %u blocks at 0x%08x\n", i, p->nch, p->nblk, p->addr);
    return 0;
}
int audio_port_config(uint32_t port, uint32_t cfg) {   /* CellAudioPortConfig {u32 readIndexAddr, status; u64 nChannel, nBlock; u32 portSize, portAddr} */
    if (port >= MAX_PORTS || !ports[port].open) return -1;
    port_t *p = &ports[port];
    st_u32(cfg, p->index_addr); st_u32(cfg + 4, p->started ? 2 : 1);
    st_u64(cfg + 8, p->nch); st_u64(cfg + 16, p->nblk);
    st_u32(cfg + 24, p->size); st_u32(cfg + 28, p->addr);
    return 0;
}
int audio_port_start(uint32_t port) { if (port >= MAX_PORTS || !ports[port].open) return -1; ports[port].started = 1; return 0; }
int audio_port_stop(uint32_t port) { if (port >= MAX_PORTS || !ports[port].open) return -1; ports[port].started = 0; return 0; }
int audio_port_close(uint32_t port) {
    if (port >= MAX_PORTS) return -1;
    AcquireSRWLockExclusive(&lock);
    ports[port].open = ports[port].started = 0;
    ReleaseSRWLockExclusive(&lock);
    return 0;
}
int audio_port_level(uint32_t port, float level) { if (port >= MAX_PORTS) return -1; ports[port].level = level; return 0; }
