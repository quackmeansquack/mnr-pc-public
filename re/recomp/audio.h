/* cellAudio ports and waveOut output (audio.c). */
#pragma once
#include <stdint.h>

extern uint32_t (*audio_guest_alloc)(uint32_t size, uint32_t align);   /* set by host.c */
int audio_init(void);
int audio_port_open(uint32_t nch, uint32_t nblk, float level, uint32_t *port);
int audio_port_config(uint32_t port, uint32_t cfg_guest_addr);
int audio_port_start(uint32_t port);
int audio_port_stop(uint32_t port);
int audio_port_close(uint32_t port);
int audio_port_level(uint32_t port, float level);
int audio_volume_pct(void);
void audio_volume_step(int dir);                 /* +1 / -1: 10% up or down, saved */
extern volatile unsigned long audio_volume_changed;
void audio_voice_feed(const float *pcm, int n);  /* voice chat headset: 16 kHz, 16-bit scale */
