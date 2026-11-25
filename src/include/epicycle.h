/* SPDX-License-Identifier: MIT */
#ifndef EPICYCLE_H
#define EPICYCLE_H

#include <stddef.h>
#include <stdint.h>

#define EPICYCLE_VERSION "0.0.1-scaffold"
#define EPICYCLE_FB_W 320
#define EPICYCLE_FB_H 240
#define EPICYCLE_AUDIO_FRAMES 128
#define EPICYCLE_PAD_COUNT 16

typedef struct EpicyclePaths {
    char app_dir[512];   /* folder that holds the binary / payload */
    char card_root[512]; /* microSD mount point */
    char data_dir[512];  /* <card>/Epicycle */
    char log_path[640];  /* <card>/Epicycle/logs/last-session.log */
} EpicyclePaths;

typedef struct EpicyclePad {
    float start;     /* 0..1 sample fraction */
    float end;       /* 0..1 */
    float loop_start;
    float loop_end;
    int freeform;    /* 1 = freeform brace, 0 = time-division snap */
    float level;
} EpicyclePad;

typedef struct EpicycleEngine {
    EpicyclePad pads[EPICYCLE_PAD_COUNT];
    float tempo_bpm;
    int running;
} EpicycleEngine;

/* platform */
int epicycle_paths_resolve(EpicyclePaths *out, const char *argv0, const char *env_app_dir);
void epicycle_log(const EpicyclePaths *paths, const char *fmt, ...);

/* display stub: software 320x240 BGRX framebuffer → /dev/fb0 when present */
int display_open(void);
void display_clear(uint32_t bgrx);
void display_present(void);
void display_close(void);
uint32_t *display_pixels(void);

/* audio stub: ALSA I2S stereo S32 @ 44.1 kHz */
int audio_open(void);
void audio_close(void);
int audio_running(void);

/* surface stub: ALSA raw MIDI "Surface" / Control Surface */
int surface_open(void);
void surface_close(void);
int surface_poll_quit(void); /* non-zero when wheel push / quit gesture seen (stub) */

/* loop engine stubs */
void engine_init(EpicycleEngine *eng);
void engine_render(EpicycleEngine *eng, int32_t *out_l, int32_t *out_r, size_t frames);

/* minimal UI */
void ui_draw_splash(const char *status_line);

#endif /* EPICYCLE_H */
