/* cellVoice service (voice.c) and the CELP codec it uses (voice_celp.c). */
#pragma once
#include <stdint.h>

extern void (*voice_notify)(uint64_t event, uint64_t data2);   /* set by host.c: to the queues the game registered */
extern volatile long voice_talking, voice_mic_state, voice_active;
extern volatile unsigned long voice_mic_changed;   /* GetTickCount of the last microphone change */
extern volatile float voice_mic_level;             /* 0..1, the microphone's level this tick */
extern volatile long voice_codec_missing;          /* no firmware library found (voice_celp.c) */
const char *voice_mic_name(void);
void voice_mic_cycle(void);
int voice_ptt_key(void);
int voice_ptt_mode(void);
uint32_t voice_init(void);
uint32_t voice_start(void);
uint32_t voice_stop(int ending);
uint32_t voice_create_port(uint32_t param, uint32_t *id);
uint32_t voice_delete_port(uint32_t id);
uint32_t voice_connect(uint32_t in, uint32_t out, int connect);
uint32_t voice_port_info(uint32_t id, uint32_t info);
uint32_t voice_write(uint32_t id, uint32_t src, uint32_t *size);
uint32_t voice_read(uint32_t id, uint32_t dst, uint32_t *size);
uint32_t voice_set(uint32_t id, int what, uint32_t value, float fvalue);
uint32_t voice_get(uint32_t id, int what, uint32_t *value, float *fvalue);
uint32_t voice_all(int what, uint32_t value);
uint32_t voice_attr(uint32_t id, uint32_t attr, uint32_t *value, int set);
uint32_t voice_frame_bytes(uint32_t bitrate);

/* CELP: one frame per call. encode: pcm (16-bit scale, the codec's rate) -> bytes written (0: none yet, <0: error);
 * decode: one frame of bytes -> samples written (<0: error). open returns NULL when the codec can't run. */
void *voice_celp_open(uint32_t bitrate, int encode);
void voice_celp_close(void *codec);
int voice_celp_encode(void *codec, const float *pcm, uint8_t *out);
int voice_celp_decode(void *codec, const uint8_t *in, float *out);
extern volatile long voice_send_state;
extern uint32_t (*voice_guest_alloc)(uint32_t size, uint32_t align);   /* set by host.c (voice_celp.c scratch) */
