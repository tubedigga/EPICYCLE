/* SPDX-License-Identifier: MIT */
#ifndef EPICYCLE_H
#define EPICYCLE_H

#include <stddef.h>
#include <stdint.h>

#define EPICYCLE_VERSION "0.2.0-loop"
#define EPICYCLE_FB_W 320
#define EPICYCLE_FB_H 240
#define EPICYCLE_AUDIO_FRAMES 128
#define EPICYCLE_PAD_COUNT 16
#define EPICYCLE_VOICE_COUNT 16
#define EPICYCLE_LFO_PER_PAD 2
#define EPICYCLE_MOD_SLOTS 4
#define EPICYCLE_SCRUB_BUDGET 8 /* aggressive LFO window-scan limit */
#define EPICYCLE_MAX_DIVISION 32
#define EPICYCLE_XFADE_MAX 2048

typedef struct EpicyclePaths {
    char app_dir[512];   /* folder that holds the binary / payload */
    char card_root[512]; /* microSD mount point */
    char data_dir[512];  /* <card>/Epicycle */
    char log_path[640];  /* <card>/Epicycle/logs/last-session.log */
} EpicyclePaths;

/* Loaded sample (RAM-resident for scrub/loop). Mono interleaved as L=R if mono. */
typedef struct EpicycleSample {
    float *data; /* interleaved stereo float -1..1 */
    size_t frames;
    unsigned rate;
    int channels; /* 1 or 2 stored as stereo */
    char name[64];
} EpicycleSample;

typedef enum EpicycleLfoShape {
    EPICYCLE_LFO_SINE = 0,
    EPICYCLE_LFO_TRI,
    EPICYCLE_LFO_SAW,
    EPICYCLE_LFO_SQUARE,
    EPICYCLE_LFO_SHAPE_COUNT
} EpicycleLfoShape;

/* Modulation destinations for the per-pad matrix (hooks for later UI). */
typedef enum EpicycleModDest {
    EPICYCLE_MOD_NONE = 0,
    EPICYCLE_MOD_BRACE_START, /* scroll window across sample */
    EPICYCLE_MOD_BRACE_LEN,
    EPICYCLE_MOD_PITCH,
    EPICYCLE_MOD_LEVEL,
    EPICYCLE_MOD_FILTER, /* stub — reserved */
    EPICYCLE_MOD_DEST_COUNT
} EpicycleModDest;

typedef struct EpicycleLfo {
    EpicycleLfoShape shape;
    float rate_hz; /* free-run Hz; ignored when sync_beats > 0 */
    float sync_beats; /* 0 = free Hz; else cycle length in beats */
    float depth;   /* 0..1 amount into destination units */
    float phase;   /* 0..1 */
    int enabled;
} EpicycleLfo;

typedef struct EpicycleModRoute {
    int lfo_index; /* 0 .. EPICYCLE_LFO_PER_PAD-1, or -1 unused */
    EpicycleModDest dest;
    float amount; /* bipolar scale, typically -1..1 */
} EpicycleModRoute;

/*
 * Loop brace = a window on the sample.
 *   window_start  — left edge (0..1 of sample)
 *   window_length — width (0..1); end = start + length (clamped)
 * Knobs / LFOs slide the window (start) or resize it (length).
 * Time-locked mode snaps length to N beats at project tempo vs sample rate
 * (no warp — length in samples = beats * 60/tempo * sample_rate).
 */
typedef struct EpicyclePad {
    EpicycleSample *sample; /* borrowed; may be NULL */
    float window_start;     /* 0..1 */
    float window_length;    /* 0..1, >0 */
    int freeform;           /* 1 = free length; 0 = division snap */
    int division_beats;     /* 1..32 when !freeform */
    float level;            /* 0..1 */
    int octave;             /* -4..+4 */
    int semitone;           /* -12..+12 */
    float fine;             /* -100..+100 cents */
    float crossfade;        /* 0..1 of window length used for Xfade */
    int loop_enable;        /* 1 = crossfade loop; 0 = one-shot through window */
    int allow_scrub;        /* 1 = may use LFO brace scan (budgeted) */
    EpicycleLfo lfos[EPICYCLE_LFO_PER_PAD];
    EpicycleModRoute mods[EPICYCLE_MOD_SLOTS];
    /* Runtime modulated (engine fills each block; UI may read) */
    float mod_window_start;
    float mod_window_length;
    float mod_pitch_ratio;
    float mod_level;
} EpicyclePad;

typedef struct EpicycleVoice {
    int active;
    int pad_index;
    double phase;       /* sample-frame position within absolute sample */
    float velocity;
    float env;          /* simple amp for one-shots */
    int in_crossfade;
    double xfade_pos;   /* 0..1 through crossfade */
} EpicycleVoice;

typedef struct EpicycleEngine {
    EpicyclePad pads[EPICYCLE_PAD_COUNT];
    EpicycleVoice voices[EPICYCLE_VOICE_COUNT];
    EpicycleSample *bank; /* owned sample slots */
    int bank_count;
    float tempo_bpm;
    unsigned sample_rate;
    int running;
    int selected_pad; /* UI focus 0..15 */
    uint64_t transport_frames; /* free-run clock for sync LFOs / divisions */
    int scrub_active; /* how many pads currently LFO-scanning brace */
} EpicycleEngine;

/* Live surface snapshot for the UI (pads / knobs / quit). */
typedef struct EpicycleSurfaceState {
    uint8_t pad_vel[EPICYCLE_PAD_COUNT]; /* 0 = up, else last strike velocity */
    uint8_t pad_down[EPICYCLE_PAD_COUNT]; /* edge-detect helper cleared by app */
    uint8_t k1, k2, k3;                  /* CC 16 / 17 / 18 */
    uint8_t main_vol;                    /* CC 19 */
    uint8_t fader;                        /* CC 20 */
    int wheel_accum;                     /* relative wheel steps since open */
    int quit_requested;                  /* wheel push / soft-key quit */
    int soft_left;                       /* note 12 edge */
    int soft_mid;                        /* note 13 edge */
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

/* audio: ALSA I2S stereo S32 @ 44.1 kHz; engine callback via audio_set_engine */
int audio_open(void);
void audio_close(void);
int audio_running(void);
void audio_set_engine(EpicycleEngine *eng); /* realtime thread calls engine_render */
unsigned audio_rate(void);
const char *audio_info(void);

/* surface: ALSA raw MIDI "Surface" / Control Surface */
int surface_open(void);
void surface_close(void);
void surface_poll(void); /* non-blocking read + parse */
int surface_poll_quit(void);
const EpicycleSurfaceState *surface_state(void);
EpicycleSurfaceState *surface_state_mut(void);
const char *surface_info(void);

/* samples */
void sample_free(EpicycleSample *s);
int sample_load_wav(EpicycleSample *out, const char *path);
int sample_synth_demo(EpicycleSample *out, unsigned rate, float freq_hz, float seconds);

/* loop engine */
void engine_init(EpicycleEngine *eng, unsigned sample_rate);
void engine_shutdown(EpicycleEngine *eng);
int engine_load_bank_wav(EpicycleEngine *eng, int slot, const char *path);
int engine_assign_sample(EpicycleEngine *eng, int pad, int bank_slot);
void engine_set_tempo(EpicycleEngine *eng, float bpm);
void engine_set_division(EpicycleEngine *eng, int pad, int beats); /* 0 = freeform */
void engine_set_window(EpicycleEngine *eng, int pad, float start, float length);
void engine_set_pitch(EpicycleEngine *eng, int pad, int octave, int semi, float fine);
void engine_note_on(EpicycleEngine *eng, int pad, float velocity);
void engine_note_off(EpicycleEngine *eng, int pad);
void engine_apply_knobs(EpicycleEngine *eng, int pad, uint8_t k1, uint8_t k2, uint8_t k3);
void engine_render(EpicycleEngine *eng, int32_t *out_l, int32_t *out_r, size_t frames);
void engine_host_demo_setup(EpicycleEngine *eng); /* load synth tones for host test */

/* on-device UI */
void ui_draw_splash(const char *status_line);
void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf);

#endif /* EPICYCLE_H */
