/* SPDX-License-Identifier: MIT */
#ifndef EPICYCLE_H
#define EPICYCLE_H

#include <stddef.h>
#include <stdint.h>

#define EPICYCLE_VERSION "0.3.2-tombola-polish"
#define EPICYCLE_FB_W 320
#define EPICYCLE_FB_H 240
#define EPICYCLE_AUDIO_FRAMES 128
#define EPICYCLE_PAD_COUNT 16
#define EPICYCLE_VOICE_COUNT 16
#define EPICYCLE_LFO_PER_PAD 2
#define EPICYCLE_MOD_SLOTS 4
#define EPICYCLE_HITMOD_SLOTS 8 /* matrix rows; UI shows ~6, encoder scrolls */
#define EPICYCLE_HITMOD_VISIBLE 6
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
 * Pages: Tombola is primary home; Sample-Mode-style edit pages follow.
 * HITMOD = physics impact → param matrix (stub UI; full matrix = phase 1.5).
 */
typedef enum EpicycleModePage {
    EPICYCLE_PAGE_TOMBOLA = 0,
    EPICYCLE_PAGE_TRIM,
    EPICYCLE_PAGE_MIX,
    EPICYCLE_PAGE_AMP,
    EPICYCLE_PAGE_TUNE,
    EPICYCLE_PAGE_FILTER,
    EPICYCLE_PAGE_FILT_ENV,
    EPICYCLE_PAGE_LFO,
    EPICYCLE_PAGE_HITMOD,
    EPICYCLE_PAGE_COUNT
} EpicycleModePage;

/* Physics mod sources (phase 1.5; IMPACT wired, others stub-selectable). */
typedef enum EpicycleHitModSrc {
    EPICYCLE_HITMOD_SRC_IMPACT = 0, /* wall impact speed */
    EPICYCLE_HITMOD_SRC_ANGLE,      /* stub: wall angle */
    EPICYCLE_HITMOD_SRC_HARDNESS,   /* stub: shape angularity */
    EPICYCLE_HITMOD_SRC_COUNT
} EpicycleHitModSrc;

/* Physics impact destinations (not MIDI velocity). Sends are stubbed. */
typedef enum EpicycleHitModDest {
    EPICYCLE_HITMOD_NONE = 0,
    EPICYCLE_HITMOD_LEVEL,   /* volume */
    EPICYCLE_HITMOD_PITCH,
    EPICYCLE_HITMOD_FILTER,  /* cutoff */
    EPICYCLE_HITMOD_SEND_A,  /* stub reverb send */
    EPICYCLE_HITMOD_SEND_B,  /* stub delay send */
    EPICYCLE_HITMOD_DEST_COUNT
} EpicycleHitModDest;

typedef struct EpicycleHitModRoute {
    EpicycleHitModSrc src;
    EpicycleHitModDest dest;
    float amount;    /* 0..1 depth at max impact */
    float threshold; /* impact 0..1 below = no mod */
    float range;     /* destination units at amount=1 (semitones / cutoff / level) */
    int enabled;
} EpicycleHitModRoute;

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
 * Division mode: equal-slice grid — sample split into N equal slices
 * (N = division_beats, typically 1/2/4/8/16/32); brace length = 1/N,
 * start snapped to a slice boundary. (Not tempo-mapped beat length.)
 */
typedef struct EpicyclePad {
    EpicycleSample *sample; /* borrowed; may be NULL */
    float window_start;     /* 0..1 */
    float window_length;    /* 0..1, >0 */
    int freeform;           /* 1 = free length; 0 = equal-slice grid */
    int division_beats;     /* slice count N = 1..32 when !freeform */
    float level;            /* 0..1 pad volume (Mix) */
    float pan;              /* 0 = L, 0.5 = C, 1 = R */
    int octave;             /* -4..+4 */
    int semitone;           /* -24..+24 (Tune page; engine clamps pitch) */
    float fine;             /* -100..+100 cents */
    float crossfade;        /* 0..1 of window length used for Xfade */
    int loop_enable;        /* 1 = pad press toggles latch; 0 = one-shot on press */
    int ping_pong;          /* 1 = reverse at brace ends; 0 = forward only */
    int allow_scrub;        /* 1 = may use LFO brace scan (budgeted) */
    /* Amp / filter page params — Chamberlin SVF in the voice path. */
    float amp_attack;  /* 0..1 → time */
    float amp_decay;   /* 0..1 → time */
    float amp_sustain; /* 0..1 level */
    float amp_release; /* 0..1 → time */
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
    float env;          /* ADSR amp level 0..1 */
    int env_stage;      /* 0=A 1=D 2=S 3=R */
    int gate;           /* 1 = held (sustain); 0 = releasing */
    int in_crossfade;
    double xfade_pos;   /* 0..1 through crossfade */
    int direction;      /* +1 forward, -1 reverse (ping-pong) */
    /* Per-voice stereo Chamberlin SVF state */
    float filt_z1_l, filt_z2_l;
    float filt_z1_r, filt_z2_r;
    float filt_env; /* 0..1 filt-env level after note-on */
    /* Physics hit-mod (tombola wall impact); 1 / 0 = none */
    float impact_pitch; /* pitch ratio multiplier */
    float impact_filt;  /* added to filter cutoff 0..1 */
    float impact_level; /* level multiplier */
    /* 1 = bounce one-shot when LOOP off; 0 = bounce may loop when LOOP on */
    int force_oneshot;
} EpicycleVoice;

typedef struct EpicycleTombolaBall {
    float x, y;
    float vx, vy;
    int active;
    float hit_flash; /* 0..1 visual flash after wall hit */
} EpicycleTombolaBall;

typedef struct EpicycleTombola {
    EpicycleTombolaBall balls[EPICYCLE_PAD_COUNT];
    float morph;      /* 0..1 fader: circle → more faces */
    float spin_rad;   /* container rotation */
    float spin_speed; /* rad/s from K2 (signed: +CW / −CCW) */
    float radius;
    int face_count; /* 0 = circle, else 3..16 */
    int ball_target; /* last MUTE compose count 0..16 */
    /* Hit queue drained by main each tick */
    int hit_pad[EPICYCLE_PAD_COUNT];
    float hit_vel[EPICYCLE_PAD_COUNT];
    int hit_n;
    /* Global physics → param matrix (multi-slot UI) */
    EpicycleHitModRoute hitmods[EPICYCLE_HITMOD_SLOTS];
    int hitmod_edit; /* which slot Hit Mod page edits 0..SLOTS-1 */
    /* Stub send accumulators (no FX yet — UI + eval wire them) */
    float send_a;
    float send_b;
} EpicycleTombola;

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
    int mute_held;                       /* MUTE (ch1 note 43) — +pad toggles ball */
    int loop_btn;                        /* LOOP (ch1 note 74) edge — toggle latch */
    int hitmod_btn;                      /* FULL LEVEL (ch1 note 7) edge — Hit Mod page */
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
/* Selected pad = solid pad RGB; wall-hit flash = brighter same colour. */
void surface_set_selected_pad(int pad /* 0..15 */);
/* Brief pad LED flash on tombola wall hit (pad colour; connection feedback). */
void surface_flash_pad(int pad /* 0..15 */);

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
void engine_set_division(EpicycleEngine *eng, int pad, int slices); /* 0 = freeform; else N slices */
void engine_set_window(EpicycleEngine *eng, int pad, float start, float length);
void engine_set_pitch(EpicycleEngine *eng, int pad, int octave, int semi, float fine);
void engine_note_on(EpicycleEngine *eng, int pad, float velocity);
/* Tombola wall-hit: velocity + optional physics hit-mod (pitch/filt/level). */
void engine_note_on_impact(EpicycleEngine *eng, int pad, float velocity, float pitch_mul,
                           float filt_add, float level_mul);
void engine_note_off(EpicycleEngine *eng, int pad); /* start Release stage (ADSR fade) */
void engine_stop_pad(EpicycleEngine *eng, int pad); /* hard stop (MUTE ball / emergency) */
int engine_pad_voice_active(const EpicycleEngine *eng, int pad); /* 1 if any voice for pad */
/* Division helpers: equal-slice length; snap start to grid; map CC → N. */
float engine_division_slice_frac(int slices);
int engine_division_quantize_slices(int slices);
int engine_division_from_cc(uint8_t cc); /* discrete musical N from K2 */
int engine_division_from_cc_sticky(uint8_t cc, int current_n); /* hysteresis vs flicker */
uint8_t engine_division_cc_center(int slices); /* pot target for pickup at N */
int engine_division_step(int slices, int delta); /* encoder: step 1/2/4/8/16/32 */
void engine_division_snap_brace(EpicycleEngine *eng, int pad); /* start→slice, len=1/N */
void engine_division_set_slice_index(EpicycleEngine *eng, int pad, int slice_index);
int engine_division_slice_index(const EpicyclePad *pad);
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
const char *epicycle_hitmod_dest_name(EpicycleHitModDest dest);
const char *epicycle_hitmod_src_name(EpicycleHitModSrc src);
/* Keep selected-pad LOOP button LED in sync (surface). */
void surface_set_loop_led(int on);
void surface_set_mute_led(int on);
void surface_set_hitmod_led(int on);
/* Host/CI: feed synthetic MIDI streams; 0 = ok. */
int surface_midi_selftest(void);

/* Tombola physics (2D projected drum). */
void tombola_init(EpicycleTombola *tb);
EpicycleTombola *tombola_state(void);
void tombola_set_morph(EpicycleTombola *tb, float morph01);
/* K2: bipolar spin — 0 = max CCW, 64 ≈ stop, 127 = max CW. */
void tombola_set_spin_cc(EpicycleTombola *tb, uint8_t k2);
/* Host/preview helper: activate first N balls. Device compose = MUTE+pad only. */
void tombola_set_ball_count(EpicycleTombola *tb, int count);
int tombola_active_count(const EpicycleTombola *tb);
void tombola_toggle_ball(EpicycleTombola *tb, int pad);
int tombola_ball_active(const EpicycleTombola *tb, int pad);
int tombola_face_count(float morph01);
uint32_t tombola_ball_color(int pad);
void tombola_step(EpicycleTombola *tb, float dt);
int tombola_drain_hits(EpicycleTombola *tb, int *pads_out, float *vels_out, int max_n);
void tombola_world_to_screen(const EpicycleTombola *tb, float x, float y, int cx, int cy,
                             int scale, int *sx, int *sy);
void tombola_edge_point(const EpicycleTombola *tb, int edge, float t, float *ox, float *oy);
/* Map impact + shape angularity → pitch/filt/level/sends via hitmod routes. */
void tombola_eval_hitmod(EpicycleTombola *tb, float impact, float *pitch_mul, float *filt_add,
                         float *level_mul);
int tombola_selftest(void);

/* PNG (stb_image) — RGB buffer is 0x00RRGGBB, alpha already on black */
int epicycle_png_load_rgb(const char *path, uint32_t **out_pixels, int *out_w, int *out_h);
void epicycle_png_free(uint32_t *pixels);

/* on-device UI */
int ui_load_splash_logo(const char *path); /* assets/boot.png; 0 = ok */
void ui_draw_splash(const char *status_line); /* logo letterboxed on black; status optional */
/* Bottom strip prompt only (does not cover logo body). */
void ui_draw_splash_prompt(const char *prompt_line);
void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf, const EpicycleTombola *tb);

#endif /* EPICYCLE_H */
