/* cellVoice: the PS3 voice service, as sys_audio's voice process runs it. Every 20 ms (320 samples at 16 kHz) each
 * input port gets its samples (IN_MIC: the PC's default recording device; IN_VOICE: CELP frames the game wrote,
 * decoded; IN_PCMAUDIO: PCM the game wrote), and each output port the mix of the inputs connected to it (OUT_VOICE:
 * CELP frames for the game to send; OUT_PCMAUDIO: PCM for the game; OUT_SECONDARY: the headset, here mixed into the
 * game's sound in audio.c). Voice ports carry whole CELP frames back to back, as the PS3 firmware's codecs make them
 * (voice_celp.c), so PS3 and RPCS3 players understand ours and we theirs.
 * The microphone only sends while the push-to-talk key is held (pc_settings.ini [voice]: push_to_talk=0 sends whenever
 * it hears speech, ptt_key = a letter or a virtual-key number; default T). Modelled on RPCS3's cellVoice (PR 19984). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <windows.h>
#include <mmsystem.h>
#include "runtime.h"
#include "audio.h"
#include "voice.h"

#define RATE 16000
#define TICK 320                 /* samples per 20 ms period */
#define MAX_VPORTS 32
enum { IN_MIC, IN_PCMAUDIO, IN_VOICE, OUT_PCMAUDIO, OUT_VOICE, OUT_SECONDARY };
enum { EV_DATA_ERROR = 1, EV_PORT_ATTACHED = 2, EV_PORT_DETACHED = 4, EV_SERVICE_ATTACHED = 8, EV_SERVICE_DETACHED = 16 };
#define MAX_IN_FRAMES 60         /* IN_VOICE: newest frames kept (latency; MNR sends 400 ms, 40 frames, at a time) */
#define PREBUFFER_TICKS 3        /* IN_VOICE: wait after silence for the rest of the talker's burst */
#define MAX_IN_BYTES 8192        /* IN_VOICE: what a write may queue */
#define MAX_OUT_FRAMES 50        /* OUT_VOICE: what it keeps for the game at most */
#define HANGOVER_TICKS 15        /* OUT_VOICE: still sent after the voice drops (300 ms) */

typedef struct { uint8_t *p; uint32_t n, cap; } bytes_t;
typedef struct { float *p; uint32_t n, cap; } floats_t;
static void b_push(bytes_t *b, const uint8_t *s, uint32_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n;
}
static void b_drop(bytes_t *b, uint32_t n) { if (n > b->n) n = b->n; memmove(b->p, b->p + n, b->n - n); b->n -= n; }
static void f_push(floats_t *b, const float *s, uint32_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap * sizeof(float)); }
    memcpy(b->p + b->n, s, n * sizeof(float)); b->n += n;
}
static void f_drop(floats_t *b, uint32_t n) { if (n > b->n) n = b->n; memmove(b->p, b->p + n, (b->n - n) * sizeof(float)); b->n -= n; }

typedef struct {
    int used; uint16_t id;
    uint32_t type, bitrate, bufsize, dtype; uint8_t nch; float volume; int mute, paused;
    bytes_t bytes;               /* IN_*: written by the game; OUT_VOICE / OUT_PCMAUDIO: for it to read */
    floats_t pcm;                /* IN_VOICE: decoded, not played yet; OUT_VOICE: not encoded yet */
    float signal[TICK];          /* an input's samples this period (16-bit scale) */
    void *codec; uint32_t codec_rate; int codec_failed;
    int playing, waited, hangover, attached, energy_on, logged, read_logged;
    float noise_db, mic_noise_db, energy;
    uint32_t vad_threshold, vad_activity;
} vport_t;

static SRWLOCK lock = SRWLOCK_INIT;
static vport_t ports[MAX_VPORTS];
static struct { uint16_t in, out; } edges[128]; static int nedges;
static int inited, started, mute_all;
static uint8_t id_ctr;
static HANDLE thread;
/* activity since the last "[voice] last 10 s" log line */
static float mic_boost = 1.5f;      /* pc_settings.ini [voice] mic_boost (percent, default 150): PC mics record quieter than a PS3 headset */
static struct { double mic_peak_db; long mic_peak_n; long ticks_talk, enc_frames, enc_bytes, read_bytes, write_bytes, dec_frames, info_out, info_in, codec_err; } st;
static void stats_log(void) {
    if (!st.ticks_talk && !st.enc_frames && !st.read_bytes && !st.write_bytes && !st.dec_frames && !st.codec_err) { memset(&st, 0, sizeof st); return; }
    if (st.mic_peak_n) printf("[voice] last 10 s: your voice peaks at %.0f dBFS on average after the x%.1f boost (-12 to -3 is good; pc_settings.ini [voice] mic_boost)\n", st.mic_peak_db / st.mic_peak_n, mic_boost);
    printf("[voice] last 10 s: key held %ld ms, encoded %ld frames (%ld bytes), game read %ld bytes, game wrote %ld bytes (other players), decoded %ld frames, polls out %ld / in %ld, codec errors %ld\n",
           st.ticks_talk * 20, st.enc_frames, st.enc_bytes, st.read_bytes, st.write_bytes, st.dec_frames, st.info_out, st.info_in, st.codec_err);
    memset(&st, 0, sizeof st);
}
void (*voice_notify)(uint64_t event, uint64_t data2);

/* ---- settings and keys (shared with the overlay) */
static int ptt_mode = 1, ptt_vk = 'T';
static char mic_want[64];          /* pc_settings.ini [voice] mic: part of the recording device's name; empty = Windows' default */
static char mic_name_cur[64];      /* the device in use (overlay) */
volatile float voice_mic_level;    /* 0..1, the microphone's level this tick while sending (overlay meter) */
volatile DWORD voice_mic_changed;  /* GetTickCount of the last F8 (overlay shows the device a moment) */
static volatile LONG mic_reopen;   /* F8: close the device so the next tick opens the chosen one */
volatile long voice_talking, voice_mic_state;   /* talking: key held (or voice heard, open mic); mic: 0 none needed, 1 ok, -1 missing */
volatile long voice_active;                     /* the game's voice chat is running */
volatile long voice_send_state;                 /* the encoder: 1 runs, -1 could not start */
static void settings_load(void) {
    char ini[MAX_PATH]; extern const char *mnr_settings_path(void);
    snprintf(ini, sizeof ini, "%s", mnr_settings_path());
    ptt_mode = GetPrivateProfileIntA("voice", "push_to_talk", 1, ini) != 0;
    char k[32]; GetPrivateProfileStringA("voice", "ptt_key", "T", k, sizeof k, ini);
    if (strlen(k) == 1) ptt_vk = toupper((unsigned char)k[0]);
    else if (k[0]) ptt_vk = (int)strtoul(k, NULL, 0);
    if (ptt_vk <= 0 || ptt_vk > 255) ptt_vk = 'T';
    GetPrivateProfileStringA("voice", "mic", "", mic_want, sizeof mic_want, ini);
    int b = GetPrivateProfileIntA("voice", "mic_boost", 150, ini); mic_boost = (b < 25 ? 25 : b > 2000 ? 2000 : b) / 100.0f;
}
const char *voice_mic_name(void) { return mic_name_cur[0] ? mic_name_cur : "default microphone"; }
/* F8: the next recording device (Windows' default, then each one by number), saved by name */
void voice_mic_cycle(void) {
    UINT n = waveInGetNumDevs(); int cur = -1;
    for (UINT i = 0; i < n && mic_want[0]; i++) { WAVEINCAPSA c; if (waveInGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR && strstr(c.szPname, mic_want)) { cur = (int)i; break; } }
    int next = cur + 1 >= (int)n ? -1 : cur + 1;
    if (next < 0) mic_want[0] = 0;
    else { WAVEINCAPSA c; if (waveInGetDevCapsA((UINT)next, &c, sizeof c) == MMSYSERR_NOERROR) snprintf(mic_want, sizeof mic_want, "%s", c.szPname); }
    extern const char *mnr_settings_path(void);
    WritePrivateProfileStringA("voice", "mic", mic_want, mnr_settings_path());
    snprintf(mic_name_cur, sizeof mic_name_cur, "%s", mic_want[0] ? mic_want : "Windows default");
    mic_reopen = 1; voice_mic_changed = GetTickCount() | 1;
}
int voice_ptt_key(void) { return ptt_vk; }
int voice_ptt_mode(void) { return ptt_mode; }

/* ---- the codec (voice_celp.c): frames of `samples` samples at `rate` Hz become `bytes` bytes */
typedef struct { uint32_t bitrate, samples, bytes, rate; } vcfg_t;
static const vcfg_t cfgs[] = {
    {3850, 320, 20, 8000}, {4650, 320, 24, 8000}, {5700, 160, 15, 8000}, {7300, 160, 19, 8000},
    {14400, 240, 27, 16000}, {16000, 160, 20, 16000}, {22533, 240, 43, 16000},
};
static const vcfg_t *cfg_of(uint32_t bitrate) { for (unsigned i = 0; i < sizeof cfgs / sizeof *cfgs; i++) if (cfgs[i].bitrate == bitrate) return &cfgs[i]; return NULL; }

/* 2:1 between 16 and 8 kHz (the 8 kHz rates): 31-tap windowed sinc at 3.8 kHz */
static float hb[31];
static void hb_init(void) {
    double fc = 3800.0 / RATE, sum = 0;
    for (int i = 0; i < 31; i++) {
        double n = i - 15.0, s = i == 15 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / 30) + 0.08 * cos(4 * M_PI * i / 30);
        hb[i] = (float)(s * w); sum += hb[i];
    }
    for (int i = 0; i < 31; i++) hb[i] = (float)(hb[i] / sum);
}
typedef struct { float x[31]; } hbf_t;
static float hb_push(hbf_t *f, float v) { memmove(f->x + 1, f->x, 30 * sizeof(float)); f->x[0] = v; float s = 0; for (int i = 0; i < 31; i++) s += hb[i] * f->x[i]; return s; }
static hbf_t port_filter[MAX_VPORTS];

/* ---- the microphone: the default recording device through the wave mapper, 16 kHz mono 16-bit */
#define MIC_BUFS 8
static HWAVEIN wi; static WAVEHDR mic_hdr[MIC_BUFS]; static int16_t mic_pcm[MIC_BUFS][TICK];
static floats_t mic_fifo; static int mic_failed, mic_primed;
/* MNR_VOICE_TEST_WAV=<16 kHz mono 16-bit WAV> (testing): the file, looped, is the microphone and is always sent */
#define TEST_MIC ((HWAVEIN)(intptr_t)-1)
static int16_t *test_pcm; static uint32_t test_n, test_pos; static int test_mode = -1;
static int voice_test(void) {
    if (test_mode < 0) {
        test_mode = 0;
        const char *e = getenv("MNR_VOICE_TEST_WAV"); FILE *f = e ? fopen(e, "rb") : NULL;
        if (f) { fseek(f, 0, SEEK_END); long n = ftell(f) - 44; fseek(f, 44, SEEK_SET);
                 if (n > 0) { test_pcm = malloc(n); test_n = (uint32_t)(fread(test_pcm, 2, n / 2, f)); test_mode = test_n > 0; } fclose(f); }
        if (test_mode) printf("[voice] test: microphone = %s (%u samples), always sending\n", e, test_n);
    }
    return test_mode;
}
static void mic_open(void) {
    if (wi || mic_failed) return;
    if (voice_test()) {   /* one tick ahead, as a device delivers it */
        wi = TEST_MIC; mic_fifo.n = 0; mic_primed = 0;
        float s[TICK]; for (int k = 0; k < TICK; k++) { s[k] = test_pcm[test_pos]; test_pos = (test_pos + 1) % test_n; }
        f_push(&mic_fifo, s, TICK);
        return;
    }
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0};
    UINT dev = WAVE_MAPPER;
    for (UINT i = 0, n = waveInGetNumDevs(); i < n && mic_want[0]; i++) { WAVEINCAPSA c; if (waveInGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR && strstr(c.szPname, mic_want)) { dev = i; break; } }
    MMRESULT r = waveInOpen(&wi, dev, &f, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) { wi = NULL; mic_failed = 1; printf("[voice] no microphone (waveInOpen %u)\n", r); return; }
    for (int i = 0; i < MIC_BUFS; i++) {
        mic_hdr[i].lpData = (LPSTR)mic_pcm[i]; mic_hdr[i].dwBufferLength = sizeof mic_pcm[i]; mic_hdr[i].dwFlags = 0;
        waveInPrepareHeader(wi, &mic_hdr[i], sizeof mic_hdr[i]); waveInAddBuffer(wi, &mic_hdr[i], sizeof mic_hdr[i]);
    }
    waveInStart(wi);
    mic_fifo.n = 0; mic_primed = 0;
    WAVEINCAPSA caps;
    if (dev == WAVE_MAPPER) snprintf(mic_name_cur, sizeof mic_name_cur, "Windows default");
    else if (waveInGetDevCapsA(dev, &caps, sizeof caps) == MMSYSERR_NOERROR) snprintf(mic_name_cur, sizeof mic_name_cur, "%s", caps.szPname);
    printf("[voice] microphone: %s (F8 picks another)\n", mic_name_cur);
    for (UINT i = 0, n = waveInGetNumDevs(); i < n; i++) if (waveInGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR) printf("[voice]   recording device %u: %s\n", i, caps.szPname);
}
static void mic_close(void) {
    if (!wi) return;
    if (wi == TEST_MIC) { wi = NULL; mic_fifo.n = 0; mic_primed = 0; return; }
    waveInReset(wi);
    for (int i = 0; i < MIC_BUFS; i++) waveInUnprepareHeader(wi, &mic_hdr[i], sizeof mic_hdr[i]);
    waveInClose(wi); wi = NULL; mic_fifo.n = 0; mic_primed = 0;
}
static void mic_pull(void) {
    if (!wi) return;
    if (wi == TEST_MIC) {
        float s[TICK]; for (int k = 0; k < TICK; k++) { s[k] = test_pcm[test_pos]; test_pos = (test_pos + 1) % test_n; }
        f_push(&mic_fifo, s, TICK);
        return;
    }
    for (int i = 0; i < MIC_BUFS; i++) {
        WAVEHDR *h = &mic_hdr[i];
        if (!(h->dwFlags & WHDR_DONE)) continue;
        float s[TICK]; uint32_t n = h->dwBytesRecorded / 2;
        for (uint32_t k = 0; k < n && k < TICK; k++) s[k] = mic_pcm[i][k];
        f_push(&mic_fifo, s, n < TICK ? n : TICK);
        h->dwFlags &= ~WHDR_DONE; h->dwBytesRecorded = 0;
        waveInAddBuffer(wi, h, sizeof *h);
    }
    if (mic_fifo.n > TICK * 6) f_drop(&mic_fifo, mic_fifo.n - TICK * 3);   /* the device runs fast: keep the latency down */
}
static void mic_take(float *out) {
    memset(out, 0, TICK * sizeof(float));
    if (!mic_primed) { if (mic_fifo.n < TICK * 2) return; mic_primed = 1; }
    uint32_t n = mic_fifo.n < TICK ? mic_fifo.n : TICK;
    memcpy(out, mic_fifo.p, n * sizeof(float)); f_drop(&mic_fifo, n);
    if (n < TICK) mic_primed = 0;
}

/* ---- ports */
static vport_t *port_get(uint32_t id) {
    for (int i = 0; i < MAX_VPORTS; i++) if (ports[i].used && ports[i].id == (uint16_t)id) return &ports[i];
    return NULL;
}
static int is_input(uint32_t t) { return t <= IN_VOICE; }
static int is_output(uint32_t t) { return t >= OUT_PCMAUDIO && t <= OUT_SECONDARY; }
static uint32_t pcm_size(uint32_t t) { return t == 2 || t == 3 ? 2 : 4; }
static float pcm_read(const uint8_t *p, uint32_t t) {
    switch (t) {
    case 0: { uint32_t v = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; float f; memcpy(&f, &v, 4); return f * 32768.0f; }
    case 1: { float f; memcpy(&f, p, 4); return f * 32768.0f; }
    case 2: return (int16_t)(p[0] << 8 | p[1]);
    case 3: return (int16_t)(p[0] | p[1] << 8);
    case 4: return (int32_t)((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) / 65536.0f;
    default: { int32_t v; memcpy(&v, p, 4); return v / 65536.0f; }
    }
}
static void pcm_write(uint8_t *p, uint32_t t, float v) {
    v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    switch (t) {
    case 0: { float f = v / 32768.0f; uint32_t u; memcpy(&u, &f, 4); p[0] = u >> 24; p[1] = u >> 16; p[2] = u >> 8; p[3] = u; break; }
    case 1: { float f = v / 32768.0f; memcpy(p, &f, 4); break; }
    case 2: { int16_t s = (int16_t)lrintf(v); p[0] = s >> 8; p[1] = s; break; }
    case 3: { int16_t s = (int16_t)lrintf(v); p[0] = s; p[1] = s >> 8; break; }
    case 4: { int32_t s = (int32_t)lrintf(v) * 65536; p[0] = s >> 24; p[1] = s >> 16; p[2] = s >> 8; p[3] = s; break; }
    default: { int32_t s = (int32_t)lrintf(v) * 65536; memcpy(p, &s, 4); break; }
    }
}
uint32_t voice_frame_bytes(uint32_t bitrate) { const vcfg_t *c = cfg_of(bitrate); return c ? c->bytes : 1; }
static void codec_drop(vport_t *p) { if (p->codec) voice_celp_close(p->codec); p->codec = NULL; p->codec_failed = 0; }
static int codec_ready(vport_t *p, int encode) {
    if (p->codec) return 1;
    if (p->codec_failed) return 0;
    p->codec = voice_celp_open(p->bitrate, encode);
    if (encode) voice_send_state = p->codec ? 1 : -1;
    if (!p->codec) { p->codec_failed = 1; printf("[voice] port 0x%x: no %s for %u bps: voice %s\n", p->id, encode ? "encoder" : "decoder", p->bitrate, encode ? "is not sent" : "is not heard"); return 0; }
    return 1;
}
static void measure(vport_t *p, const float *s, int mic) {
    if (!p->energy_on && !mic) return;
    double sum = 0; for (int i = 0; i < TICK; i++) sum += (double)s[i] * s[i];
    if (p->energy_on) { double rms = sqrt(sum / TICK) / 32768.0; double db = 90.0 + 10.0 * log10(rms > 0.00001 ? rms : 0.00001); p->energy = (float)(db < 40 ? 40 : db > 90 ? 90 : db); }
    if (mic) {
        if (p->vad_threshold < 1 || p->vad_threshold > 10) p->vad_threshold = 8;
        float level = (float)(10.0 * log10(sum / TICK + 1.0));
        int active = level > fmaxf(p->mic_noise_db + 9.0f, 40.0f);
        p->vad_activity = active ? 9 : 0;
        if (!active) p->mic_noise_db = fmaxf(level < p->mic_noise_db ? level : p->mic_noise_db + 0.05f, 20.0f);
    }
}
static void decode_voice(vport_t *p) {
    const vcfg_t *c = cfg_of(p->bitrate);
    if (!c || !codec_ready(p, 0)) { p->bytes.n = 0; return; }
    if (p->bytes.n > MAX_IN_FRAMES * c->bytes) b_drop(&p->bytes, (p->bytes.n - MAX_IN_FRAMES * c->bytes + c->bytes - 1) / c->bytes * c->bytes);
    if (!p->playing) {
        if (p->bytes.n < c->bytes) { p->waited = 0; return; }
        if (++p->waited < PREBUFFER_TICKS) return;
        p->playing = 1;
    }
    while (p->pcm.n < TICK && p->bytes.n >= c->bytes) {
        float out[320], up[640]; int n = voice_celp_decode(p->codec, p->bytes.p, out);
        b_drop(&p->bytes, c->bytes);
        if (n > 0) st.dec_frames++; else st.codec_err++;
        if (n <= 0) { if (n < 0) { p->codec_failed = 1; if (voice_notify) voice_notify(EV_DATA_ERROR, p->id); } break; }
        if (c->rate == RATE) f_push(&p->pcm, out, (uint32_t)n);
        else { hbf_t *f = &port_filter[p - ports]; for (int i = 0; i < n; i++) { up[2 * i] = 2 * hb_push(f, out[i]); up[2 * i + 1] = 2 * hb_push(f, 0); } f_push(&p->pcm, up, 2 * (uint32_t)n); }
        if (!p->logged) { printf("[voice] IN_VOICE port 0x%x: hearing voice (%u bps)\n", p->id, p->bitrate); p->logged = 1; }
    }
    uint32_t n = p->pcm.n < TICK ? p->pcm.n : TICK;
    memcpy(p->signal, p->pcm.p, n * sizeof(float)); f_drop(&p->pcm, n);
    if (!p->pcm.n && p->bytes.n < c->bytes) { p->playing = 0; p->waited = 0; }
}
static int ptt_sending;   /* the push-to-talk key is held: send everything, no voice-activity gate */
static void encode_voice(vport_t *p, const float *mix) {
    if (p->mute || p->paused || mute_all) { p->pcm.n = 0; p->hangover = 0; return; }
    const vcfg_t *c = cfg_of(p->bitrate);
    if (!c || !codec_ready(p, 1)) return;
    double e = 0; for (int i = 0; i < TICK; i++) e += (double)mix[i] * mix[i];
    float level = (float)(10.0 * log10(e / TICK + 1.0));
    int active = ptt_sending || level > fmaxf(p->noise_db + 9.0f, 40.0f);
    if (!active) p->noise_db = fmaxf(level < p->noise_db ? level : p->noise_db + 0.05f, 20.0f);
    if (active) p->hangover = HANGOVER_TICKS;
    else if (p->hangover) p->hangover--;
    else { p->pcm.n = 0; return; }
    if (c->rate == RATE) f_push(&p->pcm, mix, TICK);
    else { hbf_t *f = &port_filter[p - ports]; float d[TICK / 2]; for (int i = 0; i < TICK; i++) { float y = hb_push(f, mix[i]); if (i & 1) d[i / 2] = y; } f_push(&p->pcm, d, TICK / 2); }
    while (p->pcm.n >= c->samples) {
        uint8_t pkt[64]; int n = voice_celp_encode(p->codec, p->pcm.p, pkt);
        f_drop(&p->pcm, c->samples);
        if (n >= 0) { st.enc_frames++; st.enc_bytes += n; } else st.codec_err++;
        if (n < 0) { p->codec_failed = 1; codec_drop(p); p->codec_failed = 1; if (voice_notify) voice_notify(EV_DATA_ERROR, p->id); return; }
        if (n > 0) b_push(&p->bytes, pkt, (uint32_t)n);
    }
    if (p->bytes.n > MAX_OUT_FRAMES * c->bytes) b_drop(&p->bytes, p->bytes.n - MAX_OUT_FRAMES * c->bytes);
}
static void read_pcm(vport_t *p) {
    uint32_t ch = p->nch ? p->nch : 1, ss = pcm_size(p->dtype), fr = p->bytes.n / (ss * ch); if (fr > TICK) fr = TICK;
    for (uint32_t i = 0; i < fr; i++) { float s = 0; for (uint32_t k = 0; k < ch; k++) s += pcm_read(p->bytes.p + (i * ch + k) * ss, p->dtype); p->signal[i] = s / ch; }
    b_drop(&p->bytes, fr * ss * ch);
}
static void write_pcm(vport_t *p, const float *mix) {
    uint32_t ch = p->nch ? p->nch : 1, ss = pcm_size(p->dtype), fs = ss * ch;
    uint8_t buf[TICK * 8 * 2];
    if (fs * TICK > sizeof buf) return;
    for (int i = 0; i < TICK; i++) for (uint32_t k = 0; k < ch; k++) pcm_write(buf + i * fs + k * ss, p->dtype, mix[i]);
    b_push(&p->bytes, buf, fs * TICK);
    uint32_t cap = p->bufsize / fs * fs; if (cap < TICK * fs) cap = TICK * fs;
    if (p->bytes.n > cap) b_drop(&p->bytes, p->bytes.n - cap);
}
static void discard_mic_outputs(void) {   /* the key let go (or pressed): nothing captured before goes out */
    for (int e = 0; e < nedges; e++) {
        vport_t *in = port_get(edges[e].in), *out = port_get(edges[e].out);
        if (!in || !out || in->type != IN_MIC) continue;
        out->bytes.n = 0; out->pcm.n = 0; out->hangover = 0; out->noise_db = 30.0f;
        if (out->type == OUT_VOICE) codec_drop(out);
    }
    mic_fifo.n = 0; mic_primed = 0;
}

static void tick(void) {
    int want_mic = 0, want_headset = 0;
    for (int i = 0; i < MAX_VPORTS; i++) {
        vport_t *p = &ports[i]; if (!p->used) continue;
        if (p->type == IN_MIC) want_mic = 1;
        if (p->type == OUT_SECONDARY) for (int e = 0; e < nedges; e++) if (edges[e].out == p->id) want_headset = 1;
    }
    if (mic_reopen) { mic_reopen = 0; mic_close(); mic_failed = 0; }
    if (want_mic) mic_open(); else mic_close();
    voice_mic_state = want_mic ? (wi ? 1 : -1) : 0;
    extern int mnr_key_down(int vk);
    int key = want_mic && mnr_key_down(ptt_vk);
    static int was_sending;
    if (wi == TEST_MIC) key = 1;
    int sending = wi && (ptt_mode ? key : 1);
    if (sending != was_sending) { discard_mic_outputs(); was_sending = sending; }
    mic_pull();
    if (!sending) { mic_fifo.n = 0; mic_primed = 0; }
    float mic[TICK]; mic_take(mic);
    if (mic_boost != 1.0f) for (int k = 0; k < TICK; k++) mic[k] = 32767.0f * tanhf(mic[k] * mic_boost / 32767.0f);   /* boost, softly limited */
    { float pk = 0; for (int k = 0; k < TICK; k++) pk = fmaxf(pk, fabsf(mic[k])); voice_mic_level = sending ? pk / 32768.0f : 0;
      if (sending && pk > 3000.0f) { st.mic_peak_db += 20.0 * log10(pk / 32768.0); st.mic_peak_n++; } }   /* speech, not room noise */
    ptt_sending = ptt_mode && key && sending;
    for (int i = 0; i < MAX_VPORTS; i++) {   /* the game learns of the microphone from PORT_ATTACHED on its IN_MIC port */
        vport_t *p = &ports[i];
        if (!p->used || p->type != IN_MIC || p->attached == (wi != NULL)) continue;
        p->attached = wi != NULL;
        printf("[voice] IN_MIC port 0x%x %s\n", p->id, p->attached ? "attached" : "detached");
        if (voice_notify) voice_notify(p->attached ? EV_PORT_ATTACHED : EV_PORT_DETACHED, p->id);
    }
    int heard = 0;
    for (int i = 0; i < MAX_VPORTS; i++) {
        vport_t *p = &ports[i]; if (!p->used || !is_input(p->type)) continue;
        memset(p->signal, 0, sizeof p->signal);
        if (p->type == IN_MIC) memcpy(p->signal, mic, sizeof mic);
        else if (p->type == IN_PCMAUDIO) read_pcm(p);
        else decode_voice(p);
        measure(p, p->signal, p->type == IN_MIC);
        if (p->type == IN_MIC) { if (p->vad_activity) heard = 1; if (!ptt_mode && p->vad_activity <= p->vad_threshold) memset(p->signal, 0, sizeof p->signal); }
        float g = p->mute || mute_all || p->paused ? 0.0f : p->volume;
        if (g != 1.0f) for (int k = 0; k < TICK; k++) p->signal[k] *= g;
    }
    voice_talking = ptt_mode ? key : (sending && heard);
    if (sending && (ptt_mode ? key : heard)) st.ticks_talk++;
    float headset[TICK] = {0};
    for (int i = 0; i < MAX_VPORTS; i++) {
        vport_t *p = &ports[i]; if (!p->used || !is_output(p->type)) continue;
        float mix[TICK] = {0};
        for (int e = 0; e < nedges; e++) {
            if (edges[e].out != p->id) continue;
            vport_t *in = port_get(edges[e].in);
            if (in) for (int k = 0; k < TICK; k++) mix[k] += in->signal[k];
        }
        float g = p->mute || p->paused ? 0.0f : p->volume;
        if (g != 1.0f) for (int k = 0; k < TICK; k++) mix[k] *= g;
        measure(p, mix, 0);
        if (p->type == OUT_PCMAUDIO) write_pcm(p, mix);
        else if (p->type == OUT_VOICE) encode_voice(p, mix);
        else for (int k = 0; k < TICK; k++) headset[k] += mix[k];
    }
    if (want_headset) audio_voice_feed(headset, TICK);
}
static DWORD WINAPI voice_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    uint64_t done = 0;
    for (;;) {
        Sleep(5);
        QueryPerformanceCounter(&t);
        uint64_t due = (uint64_t)(t.QuadPart - t0.QuadPart) * 50 / f.QuadPart;
        if (due > done + 5) done = due - 1;   /* after a stall, don't catch up */
        while (done < due) {
            done++;
            AcquireSRWLockExclusive(&lock);
            if (inited && started) tick();
            else if (wi) { mic_close(); voice_mic_state = 0; voice_talking = 0; }
            if (done % 500 == 0) stats_log();
            ReleaseSRWLockExclusive(&lock);
        }
    }
    return 0;
}

/* ---- the library (host.c's HLE wrappers): 0 or a CELL_VOICE_ERROR code */
#define E_NOT_INIT 0x80310801u
#define E_INITIALIZED 0x80310802u
#define E_PORT_INVALID 0x80310804u
#define E_ARGUMENT 0x80310805u
#define E_RESOURCE 0x80310808u
#define E_TOPOLOGY 0x80310807u
#define E_SERVICE_DETACHED 0x8031080bu
uint32_t voice_init(void) {
    AcquireSRWLockExclusive(&lock);
    uint32_t r = inited ? E_INITIALIZED : 0;
    if (!inited) {
        inited = 1;
        if (!thread) { settings_load(); hb_init(); thread = CreateThread(NULL, 1 << 17, voice_thread, NULL, 0, NULL); }
        printf("[voice] voice chat on (push to talk: %s)\n", ptt_mode ? "on" : "off, open microphone");
    }
    voice_active = 1;
    ReleaseSRWLockExclusive(&lock);
    return r;
}
static void reset_all(void) {
    for (int i = 0; i < MAX_VPORTS; i++) if (ports[i].used) { codec_drop(&ports[i]); free(ports[i].bytes.p); free(ports[i].pcm.p); }
    memset(ports, 0, sizeof ports); nedges = 0; mute_all = 0; id_ctr = 0;
    mic_close();
}
uint32_t voice_start(void) {
    AcquireSRWLockExclusive(&lock);
    uint32_t r = inited ? 0 : E_NOT_INIT; int was = started;
    if (inited) started = 1;
    ReleaseSRWLockExclusive(&lock);
    if (!r && !was && voice_notify) voice_notify(EV_SERVICE_ATTACHED, 0);
    return r;
}
uint32_t voice_stop(int ending) {
    AcquireSRWLockExclusive(&lock);
    if (!inited) { ReleaseSRWLockExclusive(&lock); return E_NOT_INIT; }
    int was = started; started = 0;
    for (int i = 0; i < MAX_VPORTS; i++) if (ports[i].used) { ports[i].bytes.n = ports[i].pcm.n = 0; ports[i].playing = ports[i].hangover = 0; codec_drop(&ports[i]); if (ports[i].attached && voice_notify) voice_notify(EV_PORT_DETACHED, ports[i].id); ports[i].attached = 0; }
    mic_close(); voice_mic_state = 0; voice_talking = 0;
    if (ending) { reset_all(); inited = 0; voice_active = 0; }
    ReleaseSRWLockExclusive(&lock);
    if (was && voice_notify) voice_notify(EV_SERVICE_DETACHED, 0);
    return 0;
}
uint32_t voice_create_port(uint32_t param, uint32_t *id_out) {   /* CellVoicePortParam {type, u16 threshold, u16 bMute, f32 volume, union} */
    uint32_t type = ld_u32(param);
    if (type == IN_VOICE || type == OUT_VOICE) { if (!cfg_of(ld_u32(param + 12))) return E_ARGUMENT; }
    else if (type == IN_PCMAUDIO || type == OUT_PCMAUDIO) { if (ld_u32(param + 20) > 5) return E_ARGUMENT; }
    else if (type != IN_MIC && type != OUT_SECONDARY) return E_ARGUMENT;
    AcquireSRWLockExclusive(&lock);
    if (!inited) { ReleaseSRWLockExclusive(&lock); return E_NOT_INIT; }
    int slot = -1; for (int i = 0; i < MAX_VPORTS; i++) if (!ports[i].used) { slot = i; break; }
    if (slot < 0) { ReleaseSRWLockExclusive(&lock); return E_RESOURCE; }
    id_ctr = (uint8_t)((id_ctr + 1) % 0xa0);
    uint16_t id = 0;   /* bits 8..15: nonzero, unique; bits 0..7: a creation counter (as the PS3 hands them out) */
    for (uint32_t hi = 1; hi < 256 && !id; hi++) if (!port_get((hi << 8) | id_ctr)) id = (uint16_t)((hi << 8) | id_ctr);
    vport_t *p = &ports[slot]; memset(p, 0, sizeof *p); memset(&port_filter[slot], 0, sizeof port_filter[slot]);
    p->used = 1; p->id = id; p->type = type;
    p->mute = ld_u16(param + 6) != 0; p->volume = ld_f32(param + 8);
    if (type == IN_VOICE || type == OUT_VOICE) p->bitrate = ld_u32(param + 12);
    if (type == IN_PCMAUDIO || type == OUT_PCMAUDIO) { p->bufsize = ld_u32(param + 12); p->nch = ld_u8(param + 16); p->dtype = ld_u32(param + 20); }
    p->noise_db = p->mic_noise_db = 30.0f;
    ReleaseSRWLockExclusive(&lock);
    static const char *names[] = {"IN_MIC", "IN_PCMAUDIO", "IN_VOICE", "OUT_PCMAUDIO", "OUT_VOICE", "OUT_SECONDARY"};
    printf("[voice] port 0x%x: %s, volume %.2f%s", id, names[type], p->volume, p->mute ? ", muted" : "");
    if (p->bitrate) printf(", %u bps", p->bitrate);
    printf("\n");
    *id_out = id;
    return 0;
}
uint32_t voice_delete_port(uint32_t id) {
    AcquireSRWLockExclusive(&lock);
    vport_t *p = inited ? port_get(id) : NULL; uint32_t r = !inited ? E_NOT_INIT : p ? 0 : E_TOPOLOGY;
    if (p) {
        codec_drop(p); free(p->bytes.p); free(p->pcm.p); memset(p, 0, sizeof *p);
        for (int e = 0; e < nedges; ) if (edges[e].in == (uint16_t)id || edges[e].out == (uint16_t)id) edges[e] = edges[--nedges]; else e++;
    }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
uint32_t voice_connect(uint32_t in, uint32_t out, int connect) {
    AcquireSRWLockExclusive(&lock);
    uint32_t r = 0;
    vport_t *a = port_get(in), *b = port_get(out);
    if (!inited) r = E_NOT_INIT;
    else if (!a || !is_input(a->type) || !b || !is_output(b->type)) r = E_TOPOLOGY;
    else {
        int k = -1; for (int e = 0; e < nedges; e++) if (edges[e].in == (uint16_t)in && edges[e].out == (uint16_t)out) k = e;
        if (connect && k < 0 && nedges < 128) { edges[nedges].in = (uint16_t)in; edges[nedges].out = (uint16_t)out; nedges++; }
        if (!connect && k >= 0) edges[k] = edges[--nedges];
    }
    ReleaseSRWLockExclusive(&lock);
    if (!r) printf("[voice] %s 0x%x -> 0x%x\n", connect ? "connect" : "disconnect", in, out);
    return r;
}
uint32_t voice_port_info(uint32_t id, uint32_t info) {   /* CellVoiceBasePortInfo {type, state, u16 numEdge, pEdge, numByte, frameSize} */
    AcquireSRWLockShared(&lock);
    vport_t *p = port_get(id); uint32_t r = !inited ? E_NOT_INIT : !p ? E_TOPOLOGY : !started ? E_SERVICE_DETACHED : 0;
    if (!r) {
        uint32_t avail = p->bytes.n;
        if (p->type == IN_VOICE || p->type == IN_PCMAUDIO) {   /* inputs: free space (games poll it before writing) */
            uint32_t cap = p->type == IN_VOICE ? MAX_IN_BYTES : (p->bufsize > TICK * 8 ? p->bufsize : TICK * 8);
            avail = cap - (p->bytes.n < cap ? p->bytes.n : cap);
        }
        uint32_t fs = 0;
        if (p->type == IN_VOICE || p->type == OUT_VOICE) fs = voice_frame_bytes(p->bitrate);
        else if (p->type == IN_PCMAUDIO || p->type == OUT_PCMAUDIO) fs = pcm_size(p->dtype) * (p->nch ? p->nch : 1);
        uint16_t ne = 0; for (int e = 0; e < nedges; e++) if (edges[e].in == p->id || edges[e].out == p->id) ne++;
        if (p->type == OUT_VOICE) st.info_out++; else if (p->type == IN_VOICE) st.info_in++;
        st_u32(info, p->type); st_u32(info + 4, 3 /* RUNNING */); st_u16(info + 8, ne);
        st_u32(info + 16, avail); st_u32(info + 20, fs);
    }
    ReleaseSRWLockShared(&lock);
    return r;
}
uint32_t voice_write(uint32_t id, uint32_t src, uint32_t *size) {
    AcquireSRWLockExclusive(&lock);
    vport_t *p = port_get(id); uint32_t r = !inited ? E_NOT_INIT : !p || !is_input(p->type) ? E_TOPOLOGY : 0;
    if (!r) {
        uint32_t take = 0;
        if (p->type == IN_VOICE || p->type == IN_PCMAUDIO) {
            uint32_t cap = p->type == IN_VOICE ? MAX_IN_BYTES : (p->bufsize > TICK * 8 ? p->bufsize : TICK * 8);
            take = cap > p->bytes.n ? cap - p->bytes.n : 0; if (take > *size) take = *size;
            if (take) b_push(&p->bytes, gp(src), take);
            st.write_bytes += take;
        }
        *size = take;
    }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
uint32_t voice_read(uint32_t id, uint32_t dst, uint32_t *size) {
    AcquireSRWLockExclusive(&lock);
    vport_t *p = port_get(id); uint32_t r = !inited ? E_NOT_INIT : !p || !is_output(p->type) ? E_TOPOLOGY : 0;
    if (!r) {
        uint32_t n = 0;
        if (p->type == OUT_VOICE) { uint32_t fb = voice_frame_bytes(p->bitrate); n = (*size / fb < p->bytes.n / fb ? *size / fb : p->bytes.n / fb) * fb; }
        else if (p->type == OUT_PCMAUDIO) { uint32_t fs = pcm_size(p->dtype) * (p->nch ? p->nch : 1); n = (*size < p->bytes.n ? *size : p->bytes.n) / fs * fs; }
        if (n) { st.read_bytes += n; memcpy(gp(dst), p->bytes.p, n); b_drop(&p->bytes, n); if (p->type == OUT_VOICE && !p->read_logged) { printf("[voice] OUT_VOICE port 0x%x: the game sends voice (%u bytes)\n", p->id, n); p->read_logged = 1; } }
        *size = n;
    }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
static void voice_dbg(const char *fmt, uint32_t id, uint32_t a, double b) {
    static volatile long n; if (InterlockedIncrement(&n) > 60) return;
    printf("[voice] game: "); printf(fmt, id, a, b); printf("\n");
}
uint32_t voice_set(uint32_t id, int what, uint32_t value, float fvalue) {   /* what: 0 volume, 1 mute, 2 pause, 3 bitrate, 4 reset */
    voice_dbg("port 0x%x op %u (0 volume, 1 mute, 2 pause, 3 bitrate, 4 reset) value %g", id, (uint32_t)what, what == 0 ? fvalue : (double)value);
    AcquireSRWLockExclusive(&lock);
    vport_t *p = port_get(id); uint32_t r = !inited ? E_NOT_INIT : !p ? E_TOPOLOGY : 0;
    if (!r) switch (what) {
        case 0: p->volume = fvalue; break;
        case 1: p->mute = value != 0; break;
        case 2: p->paused = value != 0; break;
        case 3: if (!(p->type == IN_VOICE || p->type == OUT_VOICE) || !cfg_of(value)) r = E_ARGUMENT; else if (p->bitrate != value) { p->bitrate = value; p->bytes.n = p->pcm.n = 0; codec_drop(p); } break;
        case 4: p->bytes.n = p->pcm.n = 0; p->playing = p->hangover = 0; break;
    }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
uint32_t voice_get(uint32_t id, int what, uint32_t *value, float *fvalue) {   /* what: 0 volume, 1 mute, 3 bitrate */
    AcquireSRWLockShared(&lock);
    vport_t *p = port_get(id); uint32_t r = !inited ? E_NOT_INIT : !p ? E_TOPOLOGY : 0;
    if (!r) { if (what == 0) *fvalue = p->volume; else if (what == 1) *value = (uint32_t)p->mute; else if (p->type == IN_VOICE || p->type == OUT_VOICE) *value = p->bitrate; else r = E_TOPOLOGY; }
    ReleaseSRWLockShared(&lock);
    return r;
}
uint32_t voice_all(int what, uint32_t value) {   /* what: 1 mute all, 2 pause all */
    voice_dbg("all ports %u: what %u (%.0f)", (uint32_t)what, value, 0);
    AcquireSRWLockExclusive(&lock);
    uint32_t r = inited ? 0 : E_NOT_INIT;
    if (!r) { if (what == 1) mute_all = value != 0; else for (int i = 0; i < MAX_VPORTS; i++) if (ports[i].used) ports[i].paused = value != 0; }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
uint32_t voice_attr(uint32_t id, uint32_t attr, uint32_t *value, int set) {   /* 1000 ENERGY_LEVEL, 1001 VAD */
    if (set) voice_dbg("port 0x%x set attr %u = %.0f", id, attr, *value);
    else { static volatile long g; if (InterlockedIncrement(&g) <= 3) voice_dbg("port 0x%x get attr %u (%.0f)", id, attr, 0); }
    AcquireSRWLockExclusive(&lock);
    vport_t *p = port_get(id); uint32_t r = !inited ? E_NOT_INIT : !p ? E_PORT_INVALID : 0;
    if (!r) {
        if (attr == 1000) { if (set) p->energy_on = *value != 0; else memcpy(value, &p->energy, 4); }
        else if (attr == 1001 && p->type == IN_MIC) { if (set) p->vad_threshold = *value; else *value = p->vad_activity; }
        else r = p->type == IN_MIC ? 0x80310812u : E_ARGUMENT;
    }
    ReleaseSRWLockExclusive(&lock);
    return r;
}
