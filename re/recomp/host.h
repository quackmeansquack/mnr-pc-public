/* Host services shared by the runtime modules (host.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <windows.h>

const char *mnr_root_dir(void);
const char *mnr_settings_path(void);
int mnr_key_down(int vk);
void mnr_request_dump(void);
int osk_overlay(WCHAR *title, size_t tcap, WCHAR *text, size_t xcap);
int osk_key(unsigned msg, unsigned wp);
int np_notices(wchar_t out[][96], int max);
int gcm_io_regions(uint32_t *ea, uint32_t *size, int max);
void career_unlock_tracks(uint32_t mgr);
void gcm_flip_presented(void);
void perf_frame(void);
void log_state(FILE *f);
void mnr_print_thread_states(FILE *o);   /* one line per guest thread: the call it waits in, or running */
void tl_rec(const char *what, LONGLONG t0, LONGLONG t1, uint32_t lr, uint32_t arg);   /* MNR_TIMELINE (debug) */

extern volatile int gcm_frame_ticked;
extern volatile int race_began, race_track_opened;
extern volatile long fs_movies_open;
extern volatile LONGLONG fs_movie_read_qpc;
extern volatile ULONGLONG pad_script_t0;
extern volatile int pad_script_shot;
extern volatile long spu_async_launched, spu_async_done;
