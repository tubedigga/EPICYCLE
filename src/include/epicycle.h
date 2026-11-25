/* SPDX-License-Identifier: MIT */
#ifndef EPICYCLE_H
#define EPICYCLE_H

#include <stddef.h>
#include <stdint.h>

#define EPICYCLE_VERSION "0.1.0-bringup"
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
    float start; /* 0..1 sample fraction */
    float end;   /* 0..1 */
    float loop_start;
    float loop_end;
    int freeform; /* 1 = freeform brace, 0 = time-division snap */
    float level;
} EpicyclePad;

typedef struct EpicycleEngine {
    EpicyclePad pads[EPICYCLE_PAD_COUNT];
    float tempo_bpm;
    int running;
} EpicycleEngine;

/* Live surface snapshot for the bring-up UI (pads / knobs / quit). */
typedef struct EpicycleSurfaceState {
    uint8_t pad_vel[EPICYCLE_PAD_COUNT]; /* 0 = up, else last strike velocity */
    uint8_t k1, k2, k3;                  /* CC 16 / 17 / 18 */
    uint8_t main_vol;                    /* CC 19 */
    uint8_t fader;                        /* CC 20 */
    int wheel_accum;                     /* relative wheel steps since open */
    int quit_requested;                  /* wheel push / soft-key quit */
    int open;                            /* 1 if MIDI port bound */
} EpicycleSurfaceState;

typedef struct EpicycleHwStatus {
    int display_ok;
    int audio_ok;
    int surface_ok;
    unsigned audio_rate;
    char display_info[64];
    char audio_info[64];
    char surface_info[64];
} EpicycleHwStatus;

/* platform */
int epicycle_paths_resolve(EpicyclePaths *out, const char *argv0, const char *env_app_dir);
void epicycle_log(const EpicyclePaths *paths, const char *fmt, ...);
int epicycle_find_alsa_card(const char *want_id);

/* display: software 320×240 → /dev/fb0 (DRM/fb via tinydrm), stride/bpp aware */
int display_open(void);
void display_clear(uint32_t rgb);
void display_present(void);
void display_close(void);
uint32_t *display_pixels(void); /* 0x00RRGGBB pixels */
int display_ok(void);
const char *display_info(void);

/* audio: ALSA I2S stereo S32 @ 44.1 kHz via kernel ioctls (no alsa-lib) */
int audio_open(void);
void audio_close(void);
int audio_running(void);
int audio_write_silence(size_t frames);
unsigned audio_rate(void);
const char *audio_info(void);

/* surface: ALSA raw MIDI "Surface" / Control Surface */
int surface_open(void);
void surface_close(void);
void surface_poll(void); /* non-blocking read + parse */
int surface_poll_quit(void);
const EpicycleSurfaceState *surface_state(void);
const char *surface_info(void);

/* loop engine stubs (still silent — Phase 2+) */
void engine_init(EpicycleEngine *eng);
void engine_render(EpicycleEngine *eng, int32_t *out_l, int32_t *out_r, size_t frames);

/* on-device UI */
void ui_draw_splash(const char *status_line);
void ui_draw_home(const EpicycleHwStatus *hw, const EpicycleSurfaceState *surf);

#endif /* EPICYCLE_H */
