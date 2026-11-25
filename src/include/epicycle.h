/* SPDX-License-Identifier: MIT */
#ifndef EPICYCLE_H
#define EPICYCLE_H

#include <stddef.h>
#include <stdint.h>

#define EPICYCLE_VERSION "0.2.9-trim-vol-filt"
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

/*
 * Sample-Mode-style pages (stock B1–B3 cycle mapping, EPICYCLE identity).
 * Trim keeps the large waveform home; LFO is EPICYCLE-only.
 */
typedef enum EpicycleModePage {
    EPICYCLE_PAGE_TRIM = 0,
    EPICYCLE_PAGE_MIX,
    EPICYCLE_PAGE_AMP,
    EPICYCLE_PAGE_TUNE,
    EPICYCLE_PAGE_FILTER,
    EPICYCLE_PAGE_FILT_ENV,
    EPICYCLE_PAGE_LFO,
    EPICYCLE_PAGE_COUNT
} EpicycleModePage;

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
    float level;            /* 0..1 pad volume (Mix) */
    float pan;              /* 0 = L, 0.5 = C, 1 = R */
    int octave;             /* -4..+4 */
    int semitone;           /* -24..+24 (Tune page; engine clamps pitch) */
    float fine;             /* -100..+100 cents */
    float crossfade;        /* 0..1 of window length used for Xfade */
    int loop_enable;        /* 1 = crossfade loop; 0 = one-shot through window */
    int ping_pong;          /* 1 = reverse at brace ends; 0 = forward only */
    int allow_scrub;        /* 1 = may use LFO brace scan (budgeted) */
    /* Amp / filter page params — Chamberlin SVF in the voice path. */
    float amp_attack;  /* 0..1 */
    float amp_decay;   /* 0..1 */
    float vel_sens;    /* 0..1 */
    float filter_cutoff; /* 0..1 (open = full); mapped exponentially in DSP */
    float filter_reso;   /* 0..1 */
    int filter_type; /* 0=OFF 1=LPF 2=HPF 3=BPF 4=NOTCH */
    float filt_attack;
    float filt_decay;
    float filt_depth; /* 0..1 cutoff env depth */
    EpicycleLfo lfos[EPICYCLE_LFO_PER_PAD];
    EpicycleModRoute mods[EPICYCLE_MOD_SLOTS];
    /* Runtime modulated (engine fills each block; UI may read) */
    float mod_window_start;
    float mod_window_length;
    float mod_pitch_ratio;
    float mod_level;
    float mod_filter_cutoff; /* after LFO / filt-env */
} EpicyclePad;

typedef struct EpicycleVoice {
    int active;
    int pad_index;
    double phase;       /* sample-frame position within absolute sample */
    float velocity;
    float env;          /* simple amp for one-shots */
    int in_crossfade;
    double xfade_pos;   /* 0..1 through crossfade */
    int direction;      /* +1 forward, -1 reverse (ping-pong) */
    /* Per-voice stereo Chamberlin SVF state */
    float filt_z1_l, filt_z2_l;
    float filt_z1_r, filt_z2_r;
    float filt_env; /* 0..1 filt-env level after note-on */
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
    EpicycleModePage mode_page; /* Sample-Mode-style page */
    uint64_t transport_frames; /* free-run clock for sync LFOs / divisions */
    int scrub_active; /* how many pads currently LFO-scanning brace */
    float main_sq;        /* smoothed CC19 squared taper 0..1 (render) */
    float main_sq_target; /* raw target from CC19 — gain only, no voice touch */
    float fader_gain;     /* CC20 linear 0..1; 0 = true silence */
    float master_gain;    /* main_sq * fader_gain (cached for render) */
    float kit_level;      /* Mix SHIFT-K2 analogue: global pad trim 0..1 */
} EpicycleEngine;

/* Live surface snapshot for the UI (pads / knobs / quit). */
typedef struct EpicycleSurfaceState {
    uint8_t pad_vel[EPICYCLE_PAD_COUNT]; /* 0 = up, else last strike velocity */
    uint8_t pad_down[EPICYCLE_PAD_COUNT]; /* edge-detect helper cleared by app */
    uint8_t pad_up[EPICYCLE_PAD_COUNT];   /* edge on release; app clears */
    uint8_t k1, k2, k3;                  /* CC 16 / 17 / 18 */
    uint8_t main_vol;                    /* CC 19 */
    uint8_t fader;                        /* CC 20 raw position */
    int fader_seen;                      /* 1 after first CC20 (0x42 dump) */
    int fader_engaged;                   /* 1 after user moves fader from dump */
    uint8_t fader_dump;                   /* first CC20 value (ignored until move) */
    int main_seen;                       /* 1 after first CC19 */
    int main_engaged;                    /* 1 after user moves main from dump */
    uint8_t main_dump;                   /* first CC19 (0x42); ignored until move */
    uint8_t main_stable;                 /* last accepted main CC (debounce) */
    int wheel_accum;                     /* relative wheel steps since open */
    int wheel_press;                     /* encoder push edge — never quits */
    int quit_requested;                  /* soft-right quit only (not wheel) */
    int soft_left;                       /* note 12 edge — prev page */
    int soft_mid;                        /* note 13 edge — next page */
    int soft_right;                      /* note 14 edge — quit (after splash) */
    int sample_held;                     /* SAMPLE mode button (ch1 note 6) */
    int loop_btn;                        /* LOOP (ch1 note 74) edge — toggle latch */
    int shift_held;                      /* SHIFT (ch1 note 49) */
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

/* display: software 320×240 → DRM /dev/dri/card0 (preferred) or /dev/fb0 */
int display_open(void);
void display_clear(uint32_t rgb);
void display_present(void);
void display_close(void);
uint32_t *display_pixels(void); /* 0x00RRGGBB pixels */
int display_ok(void);
const char *display_info(void);
int display_log_count(void);          /* verbose probe/present lines for session log */
const char *display_log_line(int i);

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
/* Keep selected-pad LED amber when not struck (pad LEDs = active focus). */
void surface_set_selected_pad(int pad /* 0..15 */);

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
void engine_note_off(EpicycleEngine *eng, int pad); /* stop that pad's voice(s) */
/* Playhead 0..1 in sample, or -1 if pad has no active voice. */
float engine_pad_playhead(const EpicycleEngine *eng, int pad);
/* Page-aware K1–K3 mapping. mask bits: 1=K1, 2=K2, 4=K3 (only touched knobs). */
void engine_apply_page_knobs(EpicycleEngine *eng, int pad, EpicycleModePage page, uint8_t k1,
                             uint8_t k2, uint8_t k3);
void engine_apply_page_knobs_mask(EpicycleEngine *eng, int pad, EpicycleModePage page, uint8_t k1,
                                  uint8_t k2, uint8_t k3, unsigned mask);
void engine_apply_knobs(EpicycleEngine *eng, int pad, uint8_t k1, uint8_t k2, uint8_t k3);
void engine_set_master_gain_cc(EpicycleEngine *eng, uint8_t cc19); /* squared taper */
void engine_set_fader_gain_cc(EpicycleEngine *eng, uint8_t cc20);   /* linear; 0 = silence */
void engine_lfo_set_enabled(EpicycleEngine *eng, int pad, int on);
void engine_lfo_toggle_sync(EpicycleEngine *eng, int pad); /* free ↔ 1-beat sync */
void engine_toggle_loop(EpicycleEngine *eng, int pad);     /* loop latch ↔ one-shot */
void engine_set_ping_pong(EpicycleEngine *eng, int pad, int on);
void engine_sync_mod_window(EpicycleEngine *eng, int pad); /* copy window → mod (no LFO) */
/* Trim: adjust left/right edges (length) or slide position; keep other edge fixed. */
void engine_trim_set_start(EpicycleEngine *eng, int pad, float start01);
void engine_trim_set_end(EpicycleEngine *eng, int pad, float end01);
void engine_trim_set_position(EpicycleEngine *eng, int pad, float pos01);
void engine_render(EpicycleEngine *eng, int32_t *out_l, int32_t *out_r, size_t frames);
void engine_host_demo_setup(EpicycleEngine *eng); /* load synth tones for host test */

const char *epicycle_page_name(EpicycleModePage page);
const char *epicycle_filter_type_name(int type); /* OFF/LPF/HPF/BPF/NOTCH */
/* Keep selected-pad LOOP button LED in sync (surface). */
void surface_set_loop_led(int on);
/* Host/CI: feed synthetic MIDI streams; 0 = ok. */
int surface_midi_selftest(void);

/* PNG (stb_image) — RGB buffer is 0x00RRGGBB, alpha already on black */
int epicycle_png_load_rgb(const char *path, uint32_t **out_pixels, int *out_w, int *out_h);
void epicycle_png_free(uint32_t *pixels);

/* on-device UI */
int ui_load_splash_logo(const char *path); /* assets/boot.png; 0 = ok */
void ui_draw_splash(const char *status_line); /* logo letterboxed on black; status optional */
/* Bottom strip prompt only (does not cover logo body). */
void ui_draw_splash_prompt(const char *prompt_line);
void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf);

#endif /* EPICYCLE_H */
