/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE tombola — primary 2D projected drum; wall hits trigger pads.
 * SAMPLE+pad select; MUTE+pad toggle ball; SEQ → Tombola; PAD FX → Hit Mod;
 * B1/B2 cycle Tombola / Trim / Mix / Delay / Reverb / …; B3 quit.
 * 0.3.10: SAMPLE browse/preview crash harden (cap, UAF retire, soft fail).
 * 0.3.11: MAIN absolute latch + hard mute; LFO POS-only default; shrink + AA gfx.
 * 0.3.12: LFO depth → audible POS scrub; kill phase-follow Doppler pitch.
 * 0.3.13: relative-phase POS scrub (no trill); pads boot empty (no pad01/kit).
 * 0.3.14: LFO depth = matrix source only; POS via slot dest; graphics polish.
 * 0.3.15: Always-visible LFO matrix; SHIFT chrome clear of title; FREE grain hold;
 *         SAMPLE+pad stays on edit page; Mix independent DEL/REV sends.
 * 0.3.16: Dual-reader POS (no Doppler); one DEPTH; 8 slots; audible reverb; gfx.
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static EpicycleSampleBrowser g_browser;
static const EpicyclePaths *g_browser_paths;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "EPICYCLE %s — tombola for MPC Sample\n"
            "usage: %s [--help] [--version] [--demo] [--midi-selftest] "
            "[--tombola-selftest] [--sample-preview-selftest] [--lfo-selftest] "
            "[--wav PATH]...\n"
            "  --demo             synth demo tones onto pads (host / no card samples)\n"
            "  --midi-selftest    host MIDI parser regression checks\n"
            "  --tombola-selftest physics containment + hit + mute ball checks\n"
            "  --sample-preview-selftest  SAMPLE browse/preview fuzz (host)\n"
            "  --lfo-selftest     LFO POS scrub (no pitch) regression checks\n"
            "  --wav PATH         load WAV into next bank slot (up to 16)\n"
            "  MPC_APP_DIR  set by the SD launcher to the payload folder\n"
            "  On device: FADER morph, MAIN vol, K1 spin; SAMPLE page=browse/preview/assign;\n"
            "             MUTE+pad balls; ENC freeze; FULL=HitMod; B3 quit.\n",
            EPICYCLE_VERSION, argv0);
}

static void sleep_ms(int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/*
 * Ash 0.3.13: pads boot empty. No pad01.wav / 1.wav auto-map, no kit preload,
 * no silent synth fallback. User assigns via SAMPLE browse. Host --demo / --wav
 * / --preview still seed banks explicitly.
 */
static int try_load_card_samples(EpicycleEngine *eng, const EpicyclePaths *paths)
{
    (void)eng;
    (void)paths;
    return 0;
}

static void page_prev(EpicycleEngine *eng)
{
    int p = (int)eng->mode_page - 1;
    if (p < 0)
        p = (int)EPICYCLE_PAGE_COUNT - 1;
    eng->mode_page = (EpicycleModePage)p;
}

static void page_next(EpicycleEngine *eng)
{
    int p = (int)eng->mode_page + 1;
    if (p >= (int)EPICYCLE_PAGE_COUNT)
        p = 0;
    eng->mode_page = (EpicycleModePage)p;
}

/* After pad hits: briefly ignore B3 quit only (pages stay responsive). */
static int g_quit_guard_ticks;
static const EpicyclePaths *g_log_paths;

static void handle_soft_keys(EpicycleEngine *eng, EpicycleSurfaceState *st)
{
    if (!eng || !st)
        return;
    /* Surface already requires clean note-off + hold. App only guards quit
     * after pads so paging is not laggy/stuck (0.2.7 over-debounce). */
    if (st->soft_left) {
        st->soft_left = 0;
        page_prev(eng);
        if (g_log_paths)
            epicycle_log(g_log_paths, "page -> %s", epicycle_page_name(eng->mode_page));
    }
    if (st->soft_mid) {
        st->soft_mid = 0;
        page_next(eng);
        if (g_log_paths)
            epicycle_log(g_log_paths, "page -> %s", epicycle_page_name(eng->mode_page));
    }
    if (st->soft_right) {
        st->soft_right = 0;
        if (g_quit_guard_ticks > 0) {
            if (g_log_paths)
                epicycle_log(g_log_paths, "soft-B3 ignored (quit guard)");
            return;
        }
        st->quit_requested = 1;
        if (g_log_paths)
            epicycle_log(g_log_paths, "quit: soft-B3 release");
    }
}

/*
 * True pickup: knobs do not jump params until the physical pot crosses the
 * current parameter value, then they track. Trim width lock: Start/End set
 * length; POS (and wheel) slide that locked width only. LFO→POS scrub is
 * separate (engine mod path) and must stay untouched.
 */
static float g_trim_lock_len = -1.0f;
static uint8_t g_pick_target[3];
static uint8_t g_pick_prev[3];
static uint8_t g_pick_on[3];
static int g_trim_pos_idle = 1000; /* ticks since POS; exclusive while < 6 */
static int g_trim_rel[3];          /* 1 = Start/End tracking in frame steps */
static uint8_t g_trim_edge_cc[3];  /* last CC while relative */
static int g_trim_last_edge;       /* 0=Start 1=End 2=POS — encoder fine */

static int clampf_u8(int v)
{
    if (v < 0)
        return 0;
    if (v > 127)
        return 127;
    return v;
}

static int u8_near(int a, int b, int eps)
{
    int d = a - b;
    if (d < 0)
        d = -d;
    return d <= eps;
}

static int pickup_crossed(int prev, int cur, int target)
{
    /* Slightly wider catch so FREE Start/End feel less sticky (Ash). */
    if (u8_near(cur, target, 4))
        return 1;
    /* Crossed or landed on target between prev and cur. */
    if ((prev < target && cur >= target) || (prev > target && cur <= target))
        return 1;
    return 0;
}

/* 1-frame fraction for FREE Trim fine moves. */
static float trim_frame_frac(const EpicycleEngine *eng, int pad)
{
    const EpicyclePad *p = &eng->pads[pad];
    if (p->sample && p->sample->frames > 2)
        return 1.0f / (float)p->sample->frames;
    return 1e-6f;
}

/* Short audible slice — absolute 0..127 pots are too coarse here (Ash 0.3.15). */
static int trim_is_short_slice(const EpicycleEngine *eng, int pad)
{
    float w = eng->pads[pad].window_length;
    float frame = trim_frame_frac(eng, pad);
    return w <= 0.02f || w <= frame * 256.0f;
}

/*
 * Nudge Start/End by CC delta — width only (POS slides separately).
 * Close AND reopen must always work (Ash: End locked after 0.23–0.25).
 * Short-slice open uses frame steps so a tiny grain stays holdable (Ash 0.3.15).
 */
static void trim_nudge_edge(EpicycleEngine *eng, int pad, int end_edge, int dcc)
{
    if (!dcc)
        return;
    /* Cap MIDI jumps — tighter while short so a nudge cannot explode width. */
    int cap = trim_is_short_slice(eng, pad) ? 2 : 3;
    if (dcc > cap)
        dcc = cap;
    if (dcc < -cap)
        dcc = -cap;
    EpicyclePad *p = &eng->pads[pad];
    float frame = trim_frame_frac(eng, pad);
    float start = p->window_start;
    float width = p->window_length;
    if (width < frame)
        width = frame;
    int closing = (end_edge && dcc < 0) || (!end_edge && dcc > 0);

    float step;
    if (closing) {
        if (width <= frame * 32.0f) {
            /* Near grain: step by frames so we can land on 1-frame. */
            step = frame * (float)dcc * (width <= frame * 8.0f ? 1.0f : 2.0f);
        } else if (width <= 0.08f) {
            float per = width * 0.22f;
            if (per < frame * 8.0f)
                per = frame * 8.0f;
            step = per * (float)dcc;
        } else {
            float per = width * 0.16f; /* close faster to granular (Ash 0.3.8) */
            if (per < frame * 8.0f)
                per = frame * 8.0f;
            step = per * (float)dcc;
        }
    } else {
        /* Opening: fine while short; proportional once past grain territory. */
        float headroom = end_edge ? (1.0f - start - width) : start;
        if (headroom < 0.0f)
            headroom = 0.0f;
        float per;
        if (width <= frame * 64.0f) {
            /* Hold tiny audible grains — open a few frames per CC tick. */
            per = frame * (width <= frame * 8.0f ? 1.0f : 4.0f);
        } else if (width <= 0.02f) {
            per = width * 0.12f;
            if (per < frame * 4.0f)
                per = frame * 4.0f;
        } else if (width <= 0.08f) {
            per = width * 0.10f;
            if (per < frame * 8.0f)
                per = frame * 8.0f;
        } else {
            per = headroom * 0.10f;
            if (per < 0.008f)
                per = 0.008f;
            if (per < frame * 16.0f)
                per = frame * 16.0f;
        }
        if (per > headroom && headroom > frame)
            per = headroom;
        step = per * (float)dcc;
    }
    if (end_edge) {
        float end = start + width + step;
        engine_trim_set_end(eng, pad, end);
    } else {
        engine_trim_set_start(eng, pad, start + step);
    }
}

/* Quantize 0..1 to nearest sample frame for predictable Start/End landing. */
static float trim_quantize_01(const EpicycleEngine *eng, int pad, float v)
{
    const EpicyclePad *p = &eng->pads[pad];
    if (!p->sample || p->sample->frames < 2)
        return v;
    size_t fr = p->sample->frames;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    size_t i = (size_t)(v * (float)(fr - 1) + 0.5f);
    if (i >= fr)
        i = fr - 1;
    return (float)i / (float)(fr - 1);
}

/* Absolute pot → frame-quantized edge; grain snap at the other edge. */
static void trim_set_edge_abs(EpicycleEngine *eng, int pad, int end_edge, uint8_t cc)
{
    EpicyclePad *p = &eng->pads[pad];
    float abs01 = trim_quantize_01(eng, pad, (float)cc / 127.0f);
    float start = p->window_start;
    float end = start + p->window_length;
    if (end > 1.0f)
        end = 1.0f;
    float frame = trim_frame_frac(eng, pad);

    if (end_edge) {
        /* Pot at/under Start → snap to 1-frame grain. */
        if (abs01 <= start + frame * 2.0f)
            engine_trim_set_end(eng, pad, trim_quantize_01(eng, pad, start + frame));
        else
            engine_trim_set_end(eng, pad, abs01);
    } else {
        /* Pot at/over End → snap to 1-frame grain. */
        if (abs01 >= end - frame * 2.0f)
            engine_trim_set_start(eng, pad, trim_quantize_01(eng, pad, end - frame));
        else
            engine_trim_set_start(eng, pad, abs01);
    }
}

static void trim_apply_pos(EpicycleEngine *eng, int pad, float pos01)
{
    /* Preserve locked width exactly — including 1-frame grains (no 2% inflate). */
    float lock = g_trim_lock_len;
    if (lock < 0.0f)
        lock = eng->pads[pad].window_length;
    /* Keep even sub-0.01 locks; only reject NaN/neg. */
    if (!(lock >= 0.0f))
        lock = eng->pads[pad].window_length;
    eng->pads[pad].window_length = lock;
    engine_trim_set_position(eng, pad, pos01);
    /* engine_trim_set_position may floor to 1-frame min; re-read actual. */
    lock = eng->pads[pad].window_length;
    if (eng->pads[pad].window_start + lock > 1.0f)
        eng->pads[pad].window_start = 1.0f - lock;
    /* Sync base window; LFO scrub (if on) still modulates mod_window_* each block. */
    engine_sync_mod_window(eng, pad);
    g_trim_lock_len = lock;
}

static void pickup_targets_from_pad(EpicycleEngine *eng, EpicycleModePage page, int shift)
{
    EpicyclePad *p = &eng->pads[eng->selected_pad];
    uint8_t t0 = 0, t1 = 0, t2 = 0;
    switch (page) {
    case EPICYCLE_PAGE_TRIM: {
        float start = p->window_start;
        float end = start + p->window_length;
        if (end > 1.0f)
            end = 1.0f;
        if (shift) {
            t0 = (uint8_t)clampf_u8((int)((p->crossfade - 0.01f) / 0.49f * 127.0f + 0.5f));
            t1 = p->ping_pong ? 96 : 32;
            t2 = (uint8_t)clampf_u8((int)(start * 127.0f + 0.5f));
        } else if (p->freeform) {
            t0 = (uint8_t)clampf_u8((int)(start * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(end * 127.0f + 0.5f));
            /* POS target = brace start — same numeric range as Start, but a
             * separate pot; pickup arms independently so Start ≠ POS jump. */
            t2 = t0;
        } else {
            /* DIV: K1/K3 = slice index; K2 = N among 1/2/4/8/16/32. */
            int n = engine_division_quantize_slices(p->division_beats);
            int idx = engine_division_slice_index(p);
            t0 = (uint8_t)clampf_u8(n <= 1 ? 0 : (idx * 127) / (n - 1));
            t1 = engine_division_cc_center(n);
            t2 = t0;
        }
        break;
    }
    case EPICYCLE_PAGE_MIX:
        t0 = (uint8_t)clampf_u8((int)(p->level * 127.0f + 0.5f));
        if (shift) {
            t1 = (uint8_t)clampf_u8((int)(p->pan * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8((int)((eng->kit_level - 0.05f) / 0.95f * 127.0f + 0.5f));
        } else {
            /* Independent Delay / Reverb sends (Ash 0.3.15) — not one FX. */
            t1 = (uint8_t)clampf_u8((int)(p->send_del * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8((int)(p->send_rev * 127.0f + 0.5f));
        }
        break;
    case EPICYCLE_PAGE_AMP:
        if (shift) {
            t0 = (uint8_t)clampf_u8((int)(p->amp_release * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(p->vel_sens * 127.0f + 0.5f));
            t2 = g_pick_prev[2];
        } else {
            t0 = (uint8_t)clampf_u8((int)(p->amp_attack * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(p->amp_decay * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8((int)(p->amp_sustain * 127.0f + 0.5f));
        }
        break;
    case EPICYCLE_PAGE_TUNE:
        t0 = (uint8_t)clampf_u8(p->semitone + 24);
        t1 = (uint8_t)clampf_u8((int)((p->fine + 100.0f) / 200.0f * 127.0f + 0.5f));
        t2 = (uint8_t)clampf_u8(p->octave + 4);
        /* remap to 0..127 style used by knobs */
        t0 = (uint8_t)clampf_u8((int)(((float)(p->semitone + 24) / 48.0f) * 127.0f + 0.5f));
        t2 = (uint8_t)clampf_u8((int)(((float)(p->octave + 4) / 8.0f) * 127.0f + 0.5f));
        break;
    case EPICYCLE_PAGE_FILTER:
        t0 = (uint8_t)clampf_u8((int)(p->filter_cutoff * 127.0f + 0.5f));
        t1 = (uint8_t)clampf_u8((int)(p->filter_reso * 127.0f + 0.5f));
        t2 = (uint8_t)clampf_u8((int)(p->filter_type * 127.0f / 4.0f + 0.5f));
        break;
    case EPICYCLE_PAGE_FILT_ENV:
        t0 = (uint8_t)clampf_u8((int)(p->filt_attack * 127.0f + 0.5f));
        t1 = (uint8_t)clampf_u8((int)(p->filt_decay * 127.0f + 0.5f));
        t2 = (uint8_t)clampf_u8((int)(p->filt_depth * 127.0f + 0.5f));
        break;
    case EPICYCLE_PAGE_LFO: {
        /* Primary = DEST / DEPTH / RATE; SHIFT = SHAPE / LFO# (Ash 0.3.16). */
        int slot = p->mod_edit_slot;
        if (slot < 0)
            slot = 0;
        if (slot >= EPICYCLE_MOD_SLOTS)
            slot = EPICYCLE_MOD_SLOTS - 1;
        const EpicycleModRoute *r = &p->mods[slot];
        if (shift) {
            t0 = (uint8_t)clampf_u8((int)((float)p->lfos[0].shape /
                                          (float)(EPICYCLE_LFO_SHAPE_COUNT - 1) * 127.0f +
                                      0.5f));
            int li = r->lfo_index >= 0 ? r->lfo_index : 0;
            t1 = (uint8_t)clampf_u8(
                (int)((float)li / (float)(EPICYCLE_LFO_PER_PAD - 1) * 127.0f + 0.5f));
            t2 = g_pick_prev[2];
        } else {
            t0 = (uint8_t)clampf_u8(
                (int)((float)r->dest / (float)(EPICYCLE_MOD_DEST_COUNT - 1) * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(fabsf(r->amount) * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8(
                (int)((p->lfos[0].rate_hz - 0.05f) / 7.95f * 127.0f + 0.5f));
        }
        break;
    }
    case EPICYCLE_PAGE_DELAY:
        if (shift) {
            t0 = (uint8_t)clampf_u8((int)(eng->fx_delay_tone * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(eng->fx_wet * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8((int)((eng->freeze_seconds - 2.0f) / 3.0f * 127.0f + 0.5f));
        } else {
            t0 = (uint8_t)clampf_u8((int)(eng->fx_delay_time * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(eng->fx_delay_fb * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8((int)(eng->fx_delay_wet * 127.0f + 0.5f));
        }
        break;
    case EPICYCLE_PAGE_REVERB:
        if (shift) {
            t0 = (uint8_t)clampf_u8((int)(eng->fx_rev_wet * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(eng->fx_wet * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8((int)((eng->freeze_seconds - 2.0f) / 3.0f * 127.0f + 0.5f));
        } else {
            t0 = (uint8_t)clampf_u8((int)(eng->fx_rev_decay * 127.0f + 0.5f));
            t1 = (uint8_t)clampf_u8((int)(eng->fx_rev_damp * 127.0f + 0.5f));
            t2 = (uint8_t)clampf_u8((int)(eng->fx_rev_size * 127.0f + 0.5f));
        }
        break;
    case EPICYCLE_PAGE_TOMBOLA:
    case EPICYCLE_PAGE_SAMPLE:
    case EPICYCLE_PAGE_HITMOD:
        /* Direct / non-knob pages — pickup not required. */
        t0 = g_pick_prev[0];
        t1 = g_pick_prev[1];
        t2 = g_pick_prev[2];
        break;
    default:
        break;
    }
    g_pick_target[0] = t0;
    g_pick_target[1] = t1;
    g_pick_target[2] = t2;
}

static void pickup_arm(const EpicycleSurfaceState *surf, EpicycleEngine *eng)
{
    const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
    if (eng)
        pickup_targets_from_pad(eng, eng->mode_page, surf->shift_held);
    for (int i = 0; i < 3; i++) {
        g_pick_prev[i] = cur[i];
        g_pick_on[i] = 0;
        g_trim_rel[i] = 0;
        g_trim_edge_cc[i] = cur[i];
    }
    g_trim_pos_idle = 1000;
    if (eng) {
        EpicyclePad *p = &eng->pads[eng->selected_pad];
        g_trim_lock_len = p->window_length;
    }
}

static unsigned pickup_mask(const uint8_t cur[3], unsigned ignore_bits)
{
    unsigned mask = 0;
    for (int i = 0; i < 3; i++) {
        if (ignore_bits & (1u << i)) {
            g_pick_prev[i] = cur[i];
            continue;
        }
        if (!g_pick_on[i]) {
            if (!pickup_crossed((int)g_pick_prev[i], (int)cur[i], (int)g_pick_target[i])) {
                g_pick_prev[i] = cur[i];
                continue;
            }
            g_pick_on[i] = 1;
            g_pick_prev[i] = cur[i];
            mask |= (1u << i);
            continue;
        }
        if (cur[i] == g_pick_prev[i])
            continue;
        g_pick_prev[i] = cur[i];
        mask |= (1u << i);
    }
    return mask;
}

static void trim_refresh_free_targets(EpicycleEngine *eng)
{
    /* After POS slide: update Start/End targets. If already tracking, stay live
     * — POS must never freeze edges (Ash). */
    EpicyclePad *p = &eng->pads[eng->selected_pad];
    float start = p->window_start;
    float end = start + p->window_length;
    if (end > 1.0f)
        end = 1.0f;
    g_pick_target[0] = (uint8_t)clampf_u8((int)(start * 127.0f + 0.5f));
    g_pick_target[1] = (uint8_t)clampf_u8((int)(end * 127.0f + 0.5f));
    for (int i = 0; i < 2; i++) {
        if (g_trim_rel[i])
            g_pick_on[i] = 1; /* keep Start/End live after POS */
        else
            g_pick_on[i] = 0; /* still need first pickup cross */
    }
}

static void trim_refresh_div_pos_targets(EpicycleEngine *eng)
{
    /* After slice slide: refresh K1/K3 slice targets only — never touch K2 (N). */
    EpicyclePad *p = &eng->pads[eng->selected_pad];
    int n = engine_division_quantize_slices(p->division_beats);
    int idx = engine_division_slice_index(p);
    uint8_t t = (uint8_t)clampf_u8(n <= 1 ? 0 : (idx * 127) / (n - 1));
    g_pick_target[0] = t;
    g_pick_target[2] = t;
    g_pick_on[0] = 0;
    /* Keep K2 (division N) pickup armed state; only disarm if N changed elsewhere. */
}

static void trim_set_div_slice_from_cc(EpicycleEngine *eng, int pad, uint8_t cc)
{
    EpicyclePad *p = &eng->pads[pad];
    int n = engine_division_quantize_slices(p->division_beats);
    int idx = n <= 1 ? 0 : (int)(((int)cc * n) / 128);
    if (idx >= n)
        idx = n - 1;
    engine_division_set_slice_index(eng, pad, idx);
    g_trim_lock_len = eng->pads[pad].window_length;
}

static void handle_trim_knobs(EpicycleEngine *eng, const EpicycleSurfaceState *surf)
{
    const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
    EpicyclePad *p = &eng->pads[eng->selected_pad];
    int pos_exclusive = (g_trim_pos_idle < 6);
    /* FREE: Start/End stay live during/after POS (Ash: POS never freezes edges).
     * DIV: ignore K2(N) while sliding slice. */
    unsigned ignore = 0;
    if (pos_exclusive && !p->freeform)
        ignore = 2u;
    unsigned mask = pickup_mask(cur, ignore);

    if (!mask) {
        if (g_trim_pos_idle < 1000)
            g_trim_pos_idle++;
        return;
    }

    /* Same-frame: POS slides length-locked; Start/End still apply if no POS bit.
     * If both fire, prefer length edit over slide (edges must always work). */
    if ((mask & 4u) && (mask & 3u) && p->freeform)
        mask &= ~4u;
    else if ((mask & 4u) && !p->freeform)
        mask &= ~3u; /* DIV: slice slide wins over N in same frame */

    if (surf->shift_held) {
        if (mask & 1u)
            p->crossfade = 0.01f + 0.49f * ((float)surf->k1 / 127.0f);
        if (mask & 2u)
            engine_set_ping_pong(eng, eng->selected_pad, surf->k2 >= 64);
        if (mask & 4u) {
            if (p->freeform) {
                trim_apply_pos(eng, eng->selected_pad, (float)surf->k3 / 127.0f);
                trim_refresh_free_targets(eng);
            } else {
                trim_set_div_slice_from_cc(eng, eng->selected_pad, surf->k3);
                trim_refresh_div_pos_targets(eng);
            }
            g_pick_target[2] =
                (uint8_t)clampf_u8((int)(eng->pads[eng->selected_pad].window_start * 127.0f + 0.5f));
            g_trim_pos_idle = 0;
            g_trim_last_edge = 2;
        }
        return;
    }

    if (p->freeform) {
        /*
         * Wide brace: absolute after pickup (Ash 0.3.9).
         * Short slice / grain: stay relative + frame-fine — absolute 0..127
         * jumps the audible length too far (Ash 0.3.15).
         */
        if (mask & 1u) {
            int short_slice = trim_is_short_slice(eng, eng->selected_pad);
            if (!g_trim_rel[0]) {
                g_trim_rel[0] = 1;
                g_trim_edge_cc[0] = surf->k1;
                /* Soft engage only when wide — never explode a held grain. */
                if (!short_slice)
                    trim_set_edge_abs(eng, eng->selected_pad, 0, surf->k1);
            } else {
                int d = (int)surf->k1 - (int)g_trim_edge_cc[0];
                g_trim_edge_cc[0] = surf->k1;
                if (d && (short_slice || d == 1 || d == -1))
                    trim_nudge_edge(eng, eng->selected_pad, 0, d);
                else if (d)
                    trim_set_edge_abs(eng, eng->selected_pad, 0, surf->k1);
            }
            g_trim_lock_len = eng->pads[eng->selected_pad].window_length;
            g_pick_target[2] = (uint8_t)clampf_u8(
                (int)(eng->pads[eng->selected_pad].window_start * 127.0f + 0.5f));
            g_trim_last_edge = 0;
            g_trim_pos_idle = 1000;
        }
        if (mask & 2u) {
            int short_slice = trim_is_short_slice(eng, eng->selected_pad);
            if (!g_trim_rel[1]) {
                g_trim_rel[1] = 1;
                g_trim_edge_cc[1] = surf->k2;
                if (!short_slice)
                    trim_set_edge_abs(eng, eng->selected_pad, 1, surf->k2);
            } else {
                int d = (int)surf->k2 - (int)g_trim_edge_cc[1];
                g_trim_edge_cc[1] = surf->k2;
                if (d && (short_slice || d == 1 || d == -1))
                    trim_nudge_edge(eng, eng->selected_pad, 1, d);
                else if (d)
                    trim_set_edge_abs(eng, eng->selected_pad, 1, surf->k2);
            }
            g_trim_lock_len = eng->pads[eng->selected_pad].window_length;
            g_pick_target[2] = (uint8_t)clampf_u8(
                (int)(eng->pads[eng->selected_pad].window_start * 127.0f + 0.5f));
            g_trim_last_edge = 1;
            g_trim_pos_idle = 1000;
        }
    } else {
        if (mask & 2u) {
            /* N only changes when sticky map differs — never from pot noise at
             * zone boundaries, LFO, or slice slides. Disarm after change so
             * pickup must re-cross (Ash: spontaneous DIV N jumps). */
            int cur_n = eng->pads[eng->selected_pad].division_beats;
            int n = engine_division_from_cc_sticky(surf->k2, cur_n);
            if (n != engine_division_quantize_slices(cur_n)) {
                engine_set_division(eng, eng->selected_pad, n);
                g_trim_lock_len = eng->pads[eng->selected_pad].window_length;
                trim_refresh_div_pos_targets(eng);
                g_pick_target[1] = engine_division_cc_center(n);
                g_pick_on[1] = 0; /* require re-cross; do not track noise */
                if (g_log_paths)
                    epicycle_log(g_log_paths, "DIV N -> %d (K2)", n);
            } else {
                /* Same N: keep armed target centered so jitter cannot re-fire. */
                g_pick_target[1] = engine_division_cc_center(cur_n);
                g_pick_prev[1] = surf->k2;
            }
            g_trim_pos_idle = 1000;
        }
        if (mask & 1u) {
            /* Slice index only — never rewrite N. */
            trim_set_div_slice_from_cc(eng, eng->selected_pad, surf->k1);
            trim_refresh_div_pos_targets(eng);
            g_trim_pos_idle = 0;
        }
    }
    if (mask & 4u) {
        if (p->freeform) {
            trim_apply_pos(eng, eng->selected_pad, (float)surf->k3 / 127.0f);
            trim_refresh_free_targets(eng);
            g_pick_target[2] =
                (uint8_t)clampf_u8((int)(eng->pads[eng->selected_pad].window_start * 127.0f + 0.5f));
            g_trim_last_edge = 2;
        } else {
            trim_set_div_slice_from_cc(eng, eng->selected_pad, surf->k3);
            trim_refresh_div_pos_targets(eng);
        }
        g_trim_pos_idle = 0;
    }
}

static void handle_encoder_press(EpicycleEngine *eng, EpicycleSurfaceState *st)
{
    if (!eng || !st || !st->wheel_press)
        return;
    st->wheel_press = 0;
    int pad = eng->selected_pad;
    if (eng->mode_page == EPICYCLE_PAGE_TOMBOLA) {
        /* Encoder push = freeze last 2–5 s (HitMod = PAD FX). */
        engine_freeze_toggle(eng);
        if (g_log_paths)
            epicycle_log(g_log_paths, "freeze -> %s (%.1fs)",
                         engine_freeze_active(eng) || eng->freeze_request ? "ON" : "off",
                         (double)eng->freeze_seconds);
        return;
    }
    if (eng->mode_page == EPICYCLE_PAGE_SAMPLE) {
        if (st->sample_held) {
            /* SAMPLE+ENC = rescan folder (after dropping new WAVs). */
            int n = sample_browser_rescan(&g_browser, g_browser_paths);
            if (g_log_paths)
                epicycle_log(g_log_paths, "sample browser rescan -> %d", n);
            return;
        }
        if (st->shift_held) {
            /* SHIFT+ENC = peak-normalize selected pad (quiet WAVs). */
            int rc = engine_normalize_pad(eng, pad);
            if (g_log_paths)
                epicycle_log(g_log_paths, "normalize pad %d -> %s", pad + 1,
                             rc == 0 ? "ok" : "fail");
            snprintf(g_browser.status, sizeof g_browser.status, "%s",
                     rc == 0 ? "normalized" : "norm fail");
            return;
        }
        /* Commit after audition — assign cursor WAV to selected pad. */
        if (sample_browser_assign(&g_browser, eng, pad) == 0 && g_log_paths)
            epicycle_log(g_log_paths, "sample assign pad %d <- %s", pad + 1,
                         g_browser.names[g_browser.cursor]);
        return;
    }
    if (eng->mode_page == EPICYCLE_PAGE_HITMOD) {
        /* Toggle enabled on current hitmod slot. */
        EpicycleTombola *tb = tombola_state();
        int s = tb->hitmod_edit;
        if (s < 0)
            s = 0;
        if (s >= EPICYCLE_HITMOD_SLOTS)
            s = EPICYCLE_HITMOD_SLOTS - 1;
        tb->hitmods[s].enabled = tb->hitmods[s].enabled ? 0 : 1;
        if (g_log_paths)
            epicycle_log(g_log_paths, "hitmod slot %d -> %s", s + 1,
                         tb->hitmods[s].enabled ? "ON" : "off");
        return;
    }
    if (eng->mode_page == EPICYCLE_PAGE_LFO) {
        engine_lfo_toggle_sync(eng, pad);
        return;
    }
    if (eng->mode_page != EPICYCLE_PAGE_TRIM)
        return;
    /* Trim: encoder push toggles freeform ↔ equal-slice DIV.
     * Never changes mode_page — pages are B1/B2 only. */
    EpicyclePad *p = &eng->pads[pad];
    if (p->freeform) {
        int n = p->division_beats > 0 ? p->division_beats : 4;
        n = engine_division_step(n, 0); /* snap to nearest musical N */
        engine_set_division(eng, pad, n);
    } else {
        engine_set_division(eng, pad, 0);
    }
    g_trim_lock_len = eng->pads[pad].window_length;
    pickup_targets_from_pad(eng, eng->mode_page, st->shift_held);
    g_pick_on[0] = g_pick_on[1] = g_pick_on[2] = 0;
    if (g_log_paths) {
        if (eng->pads[pad].freeform)
            epicycle_log(g_log_paths, "trim mode -> FREE");
        else
            epicycle_log(g_log_paths, "trim mode -> DIV%d", eng->pads[pad].division_beats);
    }
}

static void write_host_preview_wav(const char *path, EpicycleEngine *eng, float seconds)
{
    size_t frames = (size_t)(eng->sample_rate * seconds);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return;
    uint32_t data_bytes = (uint32_t)(frames * 2 * 2);
    uint32_t riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, fp);
    fwrite(&riff, 4, 1, fp);
    fwrite("WAVEfmt ", 1, 8, fp);
    uint32_t fmt_sz = 16;
    uint16_t audio_fmt = 1, ch = 2, bps = 16;
    uint32_t rate = eng->sample_rate;
    uint32_t byte_rate = rate * ch * bps / 8;
    uint16_t block = (uint16_t)(ch * bps / 8);
    fwrite(&fmt_sz, 4, 1, fp);
    fwrite(&audio_fmt, 2, 1, fp);
    fwrite(&ch, 2, 1, fp);
    fwrite(&rate, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block, 2, 1, fp);
    fwrite(&bps, 2, 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&data_bytes, 4, 1, fp);

    int32_t L[EPICYCLE_AUDIO_FRAMES], R[EPICYCLE_AUDIO_FRAMES];
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > EPICYCLE_AUDIO_FRAMES)
            n = EPICYCLE_AUDIO_FRAMES;
        engine_render(eng, L, R, n);
        for (size_t i = 0; i < n; i++) {
            int16_t sl = (int16_t)(L[i] >> 16);
            int16_t sr = (int16_t)(R[i] >> 16);
            fwrite(&sl, 2, 1, fp);
            fwrite(&sr, 2, 1, fp);
        }
        done += n;
    }
    fclose(fp);
}

/* Wait on splash until pad / soft key / encoder press. Host skips. */
static void splash_wait_for_input(const EpicyclePaths *paths)
{
    static const char *k_prompt = "PRESS ANY PAD TO CONTINUE";
#ifdef EPICYCLE_ON_DEVICE
    epicycle_log(paths, "splash: waiting for pad / encoder / soft key");
    ui_draw_splash_prompt(k_prompt);
    for (;;) {
        if (g_stop)
            return;
        surface_poll();
        EpicycleSurfaceState *st = surface_state_mut();
        int go = 0;
        if (st->wheel_press) {
            st->wheel_press = 0;
            go = 1;
        }
        if (st->soft_left) {
            st->soft_left = 0;
            go = 1;
        }
        if (st->soft_mid) {
            st->soft_mid = 0;
            go = 1;
        }
        if (st->soft_right) {
            /* Soft-right starts the app on splash; quit only after enter. */
            st->soft_right = 0;
            go = 1;
        }
        for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
            if (st->pad_down[i]) {
                st->pad_down[i] = 0;
                go = 1;
            }
            if (st->pad_up[i])
                st->pad_up[i] = 0;
        }
        if (go) {
            epicycle_log(paths, "splash: dismissed");
            return;
        }
        /* Keep prompt visible; refresh occasionally in case of display glitch. */
        static int blink;
        if ((blink++ % 30) == 0)
            ui_draw_splash_prompt(k_prompt);
        sleep_ms(16);
    }
#else
    (void)paths;
    ui_draw_splash_prompt(k_prompt);
    sleep_ms(200);
#endif
}

int main(int argc, char **argv)
{
    int force_demo = 0;
    const char *wavs[EPICYCLE_PAD_COUNT];
    int wav_n = 0;
    const char *preview_out = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            puts(EPICYCLE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--midi-selftest") == 0) {
            int rc = surface_midi_selftest();
            /* DIV N: musical quantize + sticky CC must not flicker at boundary. */
            {
                int q3 = engine_division_quantize_slices(3);
                if (q3 != 2 && q3 != 4)
                    rc = -1;
                if (engine_division_quantize_slices(7) != 8)
                    rc = -1;
                if (engine_division_quantize_slices(4) != 4)
                    rc = -1;
                /* CC 63/64 boundary: sticky keeps DIV4 until clearly past. */
                if (engine_division_from_cc_sticky(63, 4) != 4)
                    rc = -1;
                if (engine_division_from_cc_sticky(64, 4) != 4)
                    rc = -1; /* deadband — still 4 */
                if (engine_division_from_cc_sticky(70, 4) != 8)
                    rc = -1;
                if (engine_division_from_cc_sticky(64, 8) != 8)
                    rc = -1;
                if (engine_division_from_cc_sticky(55, 8) != 4)
                    rc = -1;
            }
            /* FREE Trim: close to grain, then reopen End (Ash unlock). */
            {
                EpicycleEngine eng;
                engine_init(&eng, 44100);
                engine_host_demo_setup(&eng);
                int pad = 0;
                eng.pads[pad].freeform = 1;
                engine_set_window(&eng, pad, 0.25f, 0.22f); /* Ash 0.25–0.47 case */
                if (eng.pads[pad].window_length < 0.20f)
                    rc = -20;
                for (int n = 0; n < 120; n++)
                    trim_nudge_edge(&eng, pad, 1, -3);
                float frame = trim_frame_frac(&eng, pad);
                if (eng.pads[pad].window_length > frame * 4.0f)
                    rc = -21; /* must reach near-zero */
                float lock = eng.pads[pad].window_length;
                g_trim_lock_len = lock;
                trim_apply_pos(&eng, pad, 0.6f);
                if (eng.pads[pad].window_length > lock * 1.01f + frame)
                    rc = -22; /* POS must not inflate grain */
                engine_set_window(&eng, pad, 0.40f, 0.20f);
                trim_set_edge_abs(&eng, pad, 1, (uint8_t)(0.40f * 127.0f));
                if (eng.pads[pad].window_length > frame * 3.0f)
                    rc = -23;
                /* Ash 0.23–0.25 lock: after narrow, End must reopen. */
                engine_set_window(&eng, pad, 0.23f, 0.02f);
                float w0 = eng.pads[pad].window_length;
                for (int n = 0; n < 48; n++)
                    trim_nudge_edge(&eng, pad, 1, +1);
                if (eng.pads[pad].window_length < w0 + 0.05f)
                    rc = -24; /* End reopen failed */
                /* Ash 0.3.15: tiny grain stays holdable — one open nudge ≠ fat. */
                {
                    float frame = trim_frame_frac(&eng, pad);
                    engine_set_window(&eng, pad, 0.40f, frame);
                    float wg = eng.pads[pad].window_length;
                    trim_nudge_edge(&eng, pad, 1, +1);
                    if (eng.pads[pad].window_length > wg + frame * 8.0f)
                        rc = -27; /* single nudge exploded grain */
                    if (!trim_is_short_slice(&eng, pad) && eng.pads[pad].window_length > 0.02f)
                        rc = -28;
                }
                /* Start/End scrub must not silence a live voice (Ash Trim mute). */
                {
                    engine_set_window(&eng, pad, 0.10f, 0.40f);
                    engine_note_on(&eng, pad, 1.0f);
                    float ph0 = -1.0f;
                    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
                        if (eng.voices[i].active && eng.voices[i].pad_index == pad)
                            ph0 = (float)eng.voices[i].phase;
                    }
                    for (int n = 0; n < 30; n++)
                        trim_nudge_edge(&eng, pad, 0, +1); /* move Start */
                    if (!engine_pad_voice_active(&eng, pad))
                        rc = -25;
                    for (int n = 0; n < 30; n++)
                        trim_nudge_edge(&eng, pad, 1, -1); /* close End */
                    if (!engine_pad_voice_active(&eng, pad))
                        rc = -26;
                    (void)ph0;
                }
                engine_shutdown(&eng);
            }
            puts(rc == 0 ? "midi-selftest: ok" : "midi-selftest: FAIL");
            return rc == 0 ? 0 : 1;
        }
        if (strcmp(argv[i], "--sample-preview-selftest") == 0) {
            int rc = sample_preview_selftest();
            puts(rc == 0 ? "sample-preview-selftest: ok" : "sample-preview-selftest: FAIL");
            if (rc != 0)
                fprintf(stderr, "sample-preview-selftest rc=%d\n", rc);
            return rc == 0 ? 0 : 1;
        }
        if (strcmp(argv[i], "--lfo-selftest") == 0) {
            int rc = engine_lfo_selftest();
            puts(rc == 0 ? "lfo-selftest: ok" : "lfo-selftest: FAIL");
            if (rc != 0)
                fprintf(stderr, "lfo-selftest rc=%d\n", rc);
            return rc == 0 ? 0 : 1;
        }
        if (strcmp(argv[i], "--tombola-selftest") == 0) {
            int rc = tombola_selftest();
            /* Bounce audio: wall hit must peak like pad press (Ash silent bounce). */
            if (rc == 0) {
                EpicycleEngine eng;
                engine_init(&eng, 44100);
                engine_host_demo_setup(&eng);
                for (int p = 0; p < EPICYCLE_PAD_COUNT; p++)
                    eng.pads[p].loop_enable = 0;
                eng.main_sq = eng.main_sq_target = 1.0f;
                eng.fader_gain = 1.0f;
                eng.master_gain = 1.0f;
                EpicycleTombola *tb = tombola_state();
                tombola_init(tb);
                tombola_set_ball_count(tb, 8);
                tombola_set_spin_cc(tb, 110);
                tombola_set_morph(tb, 0.0f);
                int32_t L[EPICYCLE_AUDIO_FRAMES], R[EPICYCLE_AUDIO_FRAMES];
                int hits = 0;
                int32_t peak_hit = 0;
                for (int t = 0; t < 180; t++) {
                    tombola_step(tb, 1.0f / 60.0f);
                    int pads[EPICYCLE_PAD_COUNT];
                    float vels[EPICYCLE_PAD_COUNT];
                    int n = tombola_drain_hits(tb, pads, vels, EPICYCLE_PAD_COUNT);
                    for (int h = 0; h < n; h++) {
                        engine_note_on_impact(&eng, pads[h], vels[h], 1.0f, 0.0f, 1.0f);
                        hits++;
                    }
                    engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
                    for (size_t s = 0; s < EPICYCLE_AUDIO_FRAMES; s++) {
                        int32_t a = L[s] < 0 ? -L[s] : L[s];
                        if (a > peak_hit)
                            peak_hit = a;
                    }
                }
                /* Pad-press reference peak. */
                engine_stop_pad(&eng, 0);
                engine_note_on(&eng, 0, 0.8f);
                int32_t peak_pad = 0;
                for (int t = 0; t < 40; t++) {
                    engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
                    for (size_t s = 0; s < EPICYCLE_AUDIO_FRAMES; s++) {
                        int32_t a = L[s] < 0 ? -L[s] : L[s];
                        if (a > peak_pad)
                            peak_pad = a;
                    }
                }
                if (hits < 1)
                    rc = -30;
                else if (peak_pad < 1000)
                    rc = -31;
                else if (peak_hit * 4 < peak_pad) /* bounce must be in ballpark of pad */
                    rc = -32;
                /* LOOP off: after one-shot window, voice must end (no stuck repeat). */
                {
                    engine_stop_pad(&eng, 1);
                    eng.pads[1].loop_enable = 0;
                    eng.pads[1].freeform = 1;
                    eng.pads[1].lfos[0].enabled = 0;
                    eng.pads[1].lfos[0].depth = 0.0f;
                    eng.pads[1].amp_release = 0.05f; /* snappy for test */
                    eng.pads[1].window_start = 0.0f;
                    eng.pads[1].window_length = 0.01f; /* short brace */
                    engine_sync_mod_window(&eng, 1);
                    engine_note_on_impact(&eng, 1, 0.9f, 1.0f, 0.0f, 1.0f);
                    for (int t = 0; t < 400; t++)
                        engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
                    if (engine_pad_voice_active(&eng, 1))
                        rc = -33; /* stuck repeating */
                }
                engine_shutdown(&eng);
            }
            puts(rc == 0 ? "tombola-selftest: ok" : "tombola-selftest: FAIL");
            if (rc != 0)
                fprintf(stderr, "tombola-selftest rc=%d\n", rc);
            return rc == 0 ? 0 : 1;
        }
        if (strcmp(argv[i], "--demo") == 0) {
            force_demo = 1;
            continue;
        }
        if (strcmp(argv[i], "--preview") == 0 && i + 1 < argc) {
            preview_out = argv[++i];
            force_demo = 1;
            continue;
        }
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc && wav_n < EPICYCLE_PAD_COUNT) {
            wavs[wav_n++] = argv[++i];
            continue;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    EpicyclePaths paths;
    const char *env_dir = getenv("MPC_APP_DIR");
    if (epicycle_paths_resolve(&paths, argv[0], env_dir) != 0) {
        fprintf(stderr, "epicycle: failed to resolve card paths\n");
        return 1;
    }

    g_log_paths = &paths;
    epicycle_log(&paths, "EPICYCLE %s starting (tombola)", EPICYCLE_VERSION);
    epicycle_log(&paths, "app_dir=%s card_root=%s data_dir=%s", paths.app_dir, paths.card_root,
                 paths.data_dir);

    EpicycleEngine eng;
    engine_init(&eng, 44100);

    EpicycleHwStatus hw;
    memset(&hw, 0, sizeof hw);

    hw.display_ok = display_open() == 0;
    snprintf(hw.display_info, sizeof hw.display_info, "%s", display_info());
    epicycle_log(&paths, "display: %s", hw.display_info);
    for (int i = 0; i < display_log_count(); i++)
        epicycle_log(&paths, "display-diag: %s", display_log_line(i));

    hw.audio_ok = audio_open() == 0;
    hw.audio_rate = audio_rate();
    snprintf(hw.audio_info, sizeof hw.audio_info, "%s", audio_info());
    epicycle_log(&paths, "audio: %s", hw.audio_info);

    hw.surface_ok = surface_open() == 0;
    snprintf(hw.surface_info, sizeof hw.surface_info, "%s", surface_info());
    epicycle_log(&paths, "surface: %s", hw.surface_info);

    int loaded = 0;
    for (int i = 0; i < wav_n; i++) {
        if (engine_load_bank_wav(&eng, i, wavs[i]) == 0) {
            engine_assign_sample(&eng, i, i);
            loaded++;
        }
    }
    if (!force_demo && loaded == 0)
        loaded = try_load_card_samples(&eng, &paths); /* always 0 — pads stay empty */
    if (force_demo) {
        engine_host_demo_setup(&eng);
        loaded = EPICYCLE_PAD_COUNT;
        epicycle_log(&paths, "using synth demo bank (--demo / host preview)");
    } else if (loaded == 0) {
        epicycle_log(&paths, "pads empty — assign WAVs via SAMPLE (no pad01 auto-map)");
    } else {
        epicycle_log(&paths, "loaded %d sample slot(s) from --wav (freeform Trim default)", loaded);
    }

    EpicycleTombola *tb = tombola_state();
    tombola_init(tb);
    g_browser_paths = &paths;
    sample_browser_rescan(&g_browser, &paths);
    epicycle_log(&paths, "sample browser: %d wav(s) under Epicycle/samples", g_browser.count);
    /* Open LPF so Hit Mod filter route can darken impacts; still hearable. */
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (eng.pads[i].filter_type == 0) {
            eng.pads[i].filter_type = 1; /* LPF */
            eng.pads[i].filter_cutoff = 1.0f;
            eng.pads[i].mod_filter_cutoff = 1.0f;
        }
        /* One-shot default; LOOP button enables latch for manual pad play. */
        eng.pads[i].loop_enable = 0;
    }

    if (preview_out) {
        /* Seed a few balls so wall hits feed the preview (empty start otherwise). */
        tombola_set_ball_count(tb, 4);
        tombola_set_spin_cc(tb, 100);
        for (int i = 0; i < 90; i++) {
            tombola_step(tb, 1.0f / 60.0f);
            int pads[EPICYCLE_PAD_COUNT];
            float vels[EPICYCLE_PAD_COUNT];
            int n = tombola_drain_hits(tb, pads, vels, EPICYCLE_PAD_COUNT);
            for (int h = 0; h < n; h++) {
                float pm, fa, lm;
                tombola_eval_hitmod(tb, vels[h], &pm, &fa, &lm);
                engine_note_on_impact(&eng, pads[h], vels[h], pm, fa, lm);
            }
            int32_t L[EPICYCLE_AUDIO_FRAMES], R[EPICYCLE_AUDIO_FRAMES];
            engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
        }
        if (!engine_pad_voice_active(&eng, 0)) {
            engine_note_on(&eng, 0, 0.9f);
            engine_note_on(&eng, 2, 0.8f);
            engine_note_on(&eng, 3, 0.8f);
        }
        write_host_preview_wav(preview_out, &eng, 1.5f);
        epicycle_log(&paths, "wrote preview %s", preview_out);
        audio_close();
        display_close();
        engine_shutdown(&eng);
        return 0;
    }

    audio_set_engine(&eng);

    {
        char logo_path[640];
        snprintf(logo_path, sizeof logo_path, "%s/assets/boot.png", paths.app_dir);
        if (ui_load_splash_logo(logo_path) != 0)
            epicycle_log(&paths, "splash logo missing (%s) — text fallback", logo_path);
        else
            epicycle_log(&paths, "splash logo loaded %s", logo_path);
    }

    ui_draw_splash(hw.display_ok ? "online" : "offline");
    for (int i = 0; i < display_log_count(); i++)
        epicycle_log(&paths, "display-diag: %s", display_log_line(i));
    sleep_ms(400);
    splash_wait_for_input(&paths);

#ifndef EPICYCLE_ON_DEVICE
    engine_note_on(&eng, 0, 0.8f);
    engine_note_on(&eng, 2, 0.7f);
    engine_note_on(&eng, 3, 0.7f);
#endif

    int ticks = 0;
    uint8_t last_main = 255;
    uint8_t last_fader = 255;
    int last_shift = -1;
    EpicycleModePage last_page = eng.mode_page;
    int last_sel = eng.selected_pad;
    g_quit_guard_ticks = 45; /* ~0.7s after splash before B3 quit armed */

    surface_poll();
    /* MAIN absolute from dump: physical pot = software level (floor = mute). */
    engine_set_master_gain_cc(&eng, surface_state()->main_vol);
    last_main = surface_state()->main_vol;
    last_fader = surface_state()->fader;
    last_shift = surface_state()->shift_held;
    pickup_arm(surface_state(), &eng);
    surface_set_selected_pad(eng.selected_pad);
    surface_set_loop_led(eng.pads[eng.selected_pad].loop_enable);
    if (eng.pads[0].sample && eng.pads[0].level < 0.5f) {
        eng.pads[0].level = 1.0f;
        eng.pads[0].mod_level = eng.pads[0].level;
    }
    surface_set_loop_led(eng.pads[eng.selected_pad].loop_enable);
    /* Tombola: empty drum; spin/morph/balls engage only after user moves pots
     * (Ash: spin must not jump alone from dump / noise). */
    /* Do not apply K1/K2/K3 dump here. */

    while (!g_stop && !surface_poll_quit()) {
        surface_poll();
        EpicycleSurfaceState *st = surface_state_mut();
        const EpicycleSurfaceState *surf = surface_state();

        if (g_quit_guard_ticks > 0)
            g_quit_guard_ticks--;

        handle_encoder_press(&eng, st);

        if (st->seq_btn) {
            st->seq_btn = 0;
            if (eng.mode_page != EPICYCLE_PAGE_TOMBOLA) {
                eng.mode_page = EPICYCLE_PAGE_TOMBOLA;
                surface_set_hitmod_led(0);
                pickup_arm(surf, &eng);
                epicycle_log(&paths, "page -> TOMBOLA (SEQ)");
            }
        }

        if (st->hitmod_btn) {
            st->hitmod_btn = 0;
            eng.mode_page = EPICYCLE_PAGE_HITMOD;
            surface_set_hitmod_led(1);
            pickup_arm(surf, &eng);
            epicycle_log(&paths, "page -> HITMOD (PAD FX)");
        }

        if (st->loop_btn) {
            st->loop_btn = 0;
            engine_toggle_loop(&eng, eng.selected_pad);
            surface_set_loop_led(eng.pads[eng.selected_pad].loop_enable);
            epicycle_log(&paths, "LOOP pad %d -> %s", eng.selected_pad + 1,
                         eng.pads[eng.selected_pad].loop_enable ? "latch-toggle" : "one-shot");
        }

        /* Pads before soft keys: a real hit in this frame cancels phantom pages. */
        int pad_hit = 0;
        for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
            if (st->pad_down[i]) {
                st->pad_down[i] = 0;
                pad_hit = 1;
                g_quit_guard_ticks = 18; /* ~300ms: block false B3 only */
                if (surf->sample_held) {
                    /* Select pad for the current edit page — do NOT jump to
                     * SAMPLE browser (Ash 0.3.15). Browser only when already
                     * on SAMPLE (B1/B2) or explicit browse. */
                    eng.selected_pad = i;
                    surface_set_selected_pad(i);
                    surface_set_loop_led(eng.pads[i].loop_enable);
                    engine_sync_mod_window(&eng, i);
                    pickup_arm(surf, &eng);
                    epicycle_log(&paths, "SAMPLE+pad %d select (stay %s)", i + 1,
                                 epicycle_page_name(eng.mode_page));
                } else if (surf->mute_held) {
                    /* Hold MUTE + pad: add/remove ball (compose 1–2 then build). */
                    tombola_toggle_ball(tb, i);
                    if (!tombola_ball_active(tb, i))
                        engine_note_off(&eng, i); /* Release fade when removed */
                    epicycle_log(&paths, "MUTE ball %d -> %s", i + 1,
                                 tombola_ball_active(tb, i) ? "in" : "out");
                } else if (eng.mode_page == EPICYCLE_PAGE_SAMPLE) {
                    /* Audition cursor WAV — hold pad to gate, release stops. */
                    sample_browser_preview(&g_browser, &eng, 1);
                } else if (eng.pads[i].loop_enable) {
                    /* LOOP latch: press toggles; off uses ADSR Release fade. */
                    if (engine_pad_voice_active(&eng, i))
                        engine_note_off(&eng, i);
                    else {
                        float vel =
                            surf->pad_vel[i] > 0 ? (float)surf->pad_vel[i] / 127.0f : 0.8f;
                        engine_note_on(&eng, i, vel);
                    }
                } else {
                    /* One-shot: press starts; plays through window (release ignored). */
                    float vel =
                        surf->pad_vel[i] > 0 ? (float)surf->pad_vel[i] / 127.0f : 0.8f;
                    engine_note_on(&eng, i, vel);
                }
            }
            if (st->pad_up[i]) {
                st->pad_up[i] = 0;
                /* Hold-to-preview: release ends audition. */
                if (eng.mode_page == EPICYCLE_PAGE_SAMPLE)
                    engine_preview_stop(&eng);
            }
        }
        if (pad_hit) {
            /* Same-frame soft-key phantoms after a pad strike are dropped. */
            st->soft_left = 0;
            st->soft_mid = 0;
        }
        handle_soft_keys(&eng, st);

        if (eng.selected_pad != last_sel) {
            last_sel = eng.selected_pad;
            surface_set_loop_led(eng.pads[last_sel].loop_enable);
            pickup_arm(surf, &eng);
        }
        if (eng.mode_page != last_page) {
            last_page = eng.mode_page;
            surface_set_hitmod_led(eng.mode_page == EPICYCLE_PAGE_HITMOD);
            pickup_arm(surf, &eng);
            /* Fader never becomes gain on page enter (Ash Trim mute). */
            engine_set_fader_gain_cc(&eng, 127);
            if (eng.mode_page == EPICYCLE_PAGE_TOMBOLA && surf->fader_engaged)
                tombola_set_morph(tb, (float)surf->fader / 127.0f);
        }
        if (surf->shift_held != last_shift) {
            last_shift = surf->shift_held;
            pickup_arm(surf, &eng);
        }

        /* CC19 MAIN VOL → gain only after latch engage. Never restarts voices. */
        if (surf->main_engaged && surf->main_vol != last_main) {
            engine_set_master_gain_cc(&eng, surf->main_vol);
            last_main = surf->main_vol;
        }
        /* Fader = morph on Tombola ONLY. MAIN = sole master volume on every page. */
        engine_set_fader_gain_cc(&eng, 127);
        if (surf->fader_engaged && surf->fader != last_fader) {
            int fd = (int)surf->fader - (int)last_fader;
            if (fd < 0)
                fd = -fd;
            if (eng.mode_page == EPICYCLE_PAGE_TOMBOLA && fd >= 2) {
                last_fader = surf->fader;
                tombola_set_morph(tb, (float)surf->fader / 127.0f);
            } else if (eng.mode_page != EPICYCLE_PAGE_TOMBOLA) {
                last_fader = surf->fader; /* track pot; do not apply as gain */
            }
        }

        if (eng.mode_page == EPICYCLE_PAGE_TOMBOLA) {
            /* K1 spin only. K2 unused on Tombola (Ash: no double morph). */
            static uint8_t tk1 = 255;
            static uint8_t tk1_dump = 255;
            static int tk1_on;
            static uint8_t tk1_pend = 255;
            static int tk1_pend_n;
            if (tk1_dump == 255) {
                tk1_dump = surf->k1;
                tk1 = surf->k1;
            }
            if (!tk1_on) {
                int d = (int)surf->k1 - (int)tk1_dump;
                if (d < 0)
                    d = -d;
                if (d >= 3)
                    tk1_on = 1;
            }
            if (tk1_on && surf->k1 != tk1) {
                /* Confirm spin CC twice — reject single-sample jumps. */
                if (surf->k1 == tk1_pend) {
                    tk1_pend_n++;
                } else {
                    tk1_pend = surf->k1;
                    tk1_pend_n = 1;
                }
                if (tk1_pend_n >= 2) {
                    int d = (int)surf->k1 - (int)tk1;
                    if (d < 0)
                        d = -d;
                    if (d >= 2) {
                        tk1 = surf->k1;
                        tombola_set_spin_cc(tb, surf->k1);
                    }
                    tk1_pend_n = 0;
                }
            }
            /* Ball count: MUTE+pad only. Morph: fader only (no auto shrink). */
        } else if (eng.mode_page == EPICYCLE_PAGE_DELAY) {
            const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
            unsigned mask = pickup_mask(cur, 0);
            if (mask) {
                if (surf->shift_held) {
                    if (mask & 1u)
                        eng.fx_delay_tone = (float)surf->k1 / 127.0f;
                    if (mask & 2u)
                        eng.fx_wet = (float)surf->k2 / 127.0f;
                    if (mask & 4u)
                        engine_set_freeze_seconds(&eng, 2.0f + 3.0f * ((float)surf->k3 / 127.0f));
                } else {
                    engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page, surf->k1,
                                                surf->k2, surf->k3, mask);
                }
            }
        } else if (eng.mode_page == EPICYCLE_PAGE_REVERB) {
            const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
            unsigned mask = pickup_mask(cur, 0);
            if (mask) {
                if (surf->shift_held) {
                    if (mask & 1u)
                        eng.fx_rev_wet = (float)surf->k1 / 127.0f;
                    if (mask & 2u)
                        eng.fx_wet = (float)surf->k2 / 127.0f;
                    if (mask & 4u)
                        engine_set_freeze_seconds(&eng, 2.0f + 3.0f * ((float)surf->k3 / 127.0f));
                } else {
                    engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page, surf->k1,
                                                surf->k2, surf->k3, mask);
                }
            }
        } else if (eng.mode_page == EPICYCLE_PAGE_SAMPLE) {
            /* Knobs unused on SAMPLE — wheel browses, pad previews, ENC assigns. */
        } else if (eng.mode_page == EPICYCLE_PAGE_AMP) {
            const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
            unsigned mask = pickup_mask(cur, 0);
            if (mask) {
                EpicyclePad *p = &eng.pads[eng.selected_pad];
                if (surf->shift_held) {
                    if (mask & 1u)
                        p->amp_release = (float)surf->k1 / 127.0f;
                    if (mask & 2u)
                        p->vel_sens = (float)surf->k2 / 127.0f;
                } else {
                    engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page, surf->k1,
                                                surf->k2, surf->k3, mask);
                }
            }
        } else if (eng.mode_page == EPICYCLE_PAGE_LFO) {
            /* Primary: K1=DEST K2=DEPTH K3=RATE. One depth only (Ash 0.3.16).
             * SHIFT: K1=SHAPE K2=LFO#. Encoder=slot. */
            EpicyclePad *p = &eng.pads[eng.selected_pad];
            if (p->mod_edit_slot < 0)
                p->mod_edit_slot = 0;
            if (p->mod_edit_slot >= EPICYCLE_MOD_SLOTS)
                p->mod_edit_slot = EPICYCLE_MOD_SLOTS - 1;
            EpicycleModRoute *r = &p->mods[p->mod_edit_slot];
            static uint8_t mk1 = 255, mk2 = 255, mk3 = 255;
            if (surf->shift_held) {
                const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
                unsigned mask = pickup_mask(cur, 0);
                if (mask & 1u) {
                    int shape = (int)((float)surf->k1 / 127.0f *
                                         (float)(EPICYCLE_LFO_SHAPE_COUNT - 1) +
                                     0.5f);
                    if (shape < 0)
                        shape = 0;
                    if (shape >= EPICYCLE_LFO_SHAPE_COUNT)
                        shape = EPICYCLE_LFO_SHAPE_COUNT - 1;
                    p->lfos[0].shape = (EpicycleLfoShape)shape;
                }
                if (surf->k2 != mk2) {
                    mk2 = surf->k2;
                    int li = (int)((float)surf->k2 / 127.0f *
                                       (float)(EPICYCLE_LFO_PER_PAD - 1) +
                                   0.5f);
                    if (li < 0)
                        li = 0;
                    if (li >= EPICYCLE_LFO_PER_PAD)
                        li = EPICYCLE_LFO_PER_PAD - 1;
                    if (r->dest != EPICYCLE_MOD_NONE)
                        r->lfo_index = li;
                }
                (void)mask;
            } else {
                if (surf->k1 != mk1) {
                    mk1 = surf->k1;
                    int d = (int)((float)surf->k1 / 127.0f *
                                      (float)(EPICYCLE_MOD_DEST_COUNT - 1) +
                                  0.5f);
                    if (d < 0)
                        d = 0;
                    if (d >= EPICYCLE_MOD_DEST_COUNT)
                        d = EPICYCLE_MOD_DEST_COUNT - 1;
                    r->dest = (EpicycleModDest)d;
                    if (r->dest == EPICYCLE_MOD_NONE) {
                        r->lfo_index = -1;
                        r->amount = 0.0f;
                        engine_lfo_set_enabled(&eng, eng.selected_pad, 0);
                    } else if (r->lfo_index < 0) {
                        r->lfo_index = 0;
                    }
                }
                if (surf->k2 != mk2) {
                    mk2 = surf->k2;
                    r->amount = (float)surf->k2 / 127.0f;
                    p->lfos[0].depth = 1.0f; /* source always full */
                    if (r->dest != EPICYCLE_MOD_NONE) {
                        if (r->lfo_index < 0)
                            r->lfo_index = 0;
                        engine_lfo_set_enabled(&eng, eng.selected_pad, r->amount > 0.02f);
                    }
                }
                if (surf->k3 != mk3) {
                    mk3 = surf->k3;
                    if (p->lfos[0].sync_beats > 0.0f) {
                        static const float beats[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
                        int idx = (int)((float)surf->k3 / 127.0f * 5.0f + 0.5f);
                        if (idx < 0)
                            idx = 0;
                        if (idx > 5)
                            idx = 5;
                        p->lfos[0].sync_beats = beats[idx];
                    } else {
                        p->lfos[0].rate_hz = 0.05f + 7.95f * ((float)surf->k3 / 127.0f);
                    }
                }
            }
        } else if (eng.mode_page == EPICYCLE_PAGE_HITMOD) {
            if (tb->hitmod_edit < 0)
                tb->hitmod_edit = 0;
            if (tb->hitmod_edit >= EPICYCLE_HITMOD_SLOTS)
                tb->hitmod_edit = EPICYCLE_HITMOD_SLOTS - 1;
            EpicycleHitModRoute *r = &tb->hitmods[tb->hitmod_edit];
            /* Match matrix left→right: K1=SRC, K2=DEST, K3=AMT (SHIFT+K3=thr). */
            static uint8_t hk1 = 255, hk2 = 255, hk3 = 255;
            if (surf->k1 != hk1) {
                hk1 = surf->k1;
                int s = (int)((float)surf->k1 / 127.0f *
                                  (float)(EPICYCLE_HITMOD_SRC_COUNT - 1) +
                              0.5f);
                if (s < 0)
                    s = 0;
                if (s >= EPICYCLE_HITMOD_SRC_COUNT)
                    s = EPICYCLE_HITMOD_SRC_COUNT - 1;
                r->src = (EpicycleHitModSrc)s;
            }
            if (surf->k2 != hk2) {
                hk2 = surf->k2;
                int d = (int)((float)surf->k2 / 127.0f * (float)(EPICYCLE_HITMOD_DEST_COUNT - 1) +
                              0.5f);
                if (d < 0)
                    d = 0;
                if (d >= EPICYCLE_HITMOD_DEST_COUNT)
                    d = EPICYCLE_HITMOD_DEST_COUNT - 1;
                r->dest = (EpicycleHitModDest)d;
                r->enabled = (r->dest != EPICYCLE_HITMOD_NONE);
            }
            if (surf->k3 != hk3) {
                hk3 = surf->k3;
                if (surf->shift_held)
                    r->threshold = (float)surf->k3 / 127.0f;
                else
                    r->amount = (float)surf->k3 / 127.0f;
            }
        } else if (eng.mode_page == EPICYCLE_PAGE_TRIM) {
            handle_trim_knobs(&eng, surf);
        } else {
            const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
            unsigned mask = pickup_mask(cur, 0);
            if (mask) {
                if (surf->shift_held && eng.mode_page == EPICYCLE_PAGE_MIX) {
                    /* SHIFT: K1 vol · K2 pan · K3 kit (sends are primary). */
                    if (mask & 1u)
                        engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page,
                                                    surf->k1, surf->k2, surf->k3, 1u);
                    if (mask & 2u)
                        eng.pads[eng.selected_pad].pan = (float)surf->k2 / 127.0f;
                    if (mask & 4u)
                        eng.kit_level = 0.05f + 0.95f * ((float)surf->k3 / 127.0f);
                } else {
                    engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page, surf->k1,
                                                surf->k2, surf->k3, mask);
                }
            }
        }

        /* Always step tombola (hits fire even while editing other pages). */
        tombola_step(tb, 1.0f / 60.0f);
        eng.fx_bus_rev = tb->send_a;
        eng.fx_bus_del = tb->send_b;
        {
            int pads[EPICYCLE_PAD_COUNT];
            float vels[EPICYCLE_PAD_COUNT];
            int n = tombola_drain_hits(tb, pads, vels, EPICYCLE_PAD_COUNT);
            /* Phantom guard: empty drum / inactive ball never triggers. */
            if (tombola_active_count(tb) == 0)
                n = 0;
            for (int h = 0; h < n; h++) {
                if (!tombola_ball_active(tb, pads[h]))
                    continue;
                float pm, fa, lm;
                tombola_eval_hitmod(tb, vels[h], &pm, &fa, &lm);
                engine_note_on_impact(&eng, pads[h], vels[h], pm, fa, lm);
                surface_flash_pad(pads[h]); /* LED flash = wall-hit connection */
            }
        }

        /* Wheel: Trim FREE = slide POS; Trim DIV = change N (never pages). */
        if (surf->wheel_accum != 0) {
            int delta = surf->wheel_accum;
            st->wheel_accum = 0;
            if (eng.mode_page == EPICYCLE_PAGE_SAMPLE) {
                sample_browser_move(&g_browser, delta);
                /* Light one-shot peek when scrolling so browsing isn't blind. */
                if (g_browser.count > 0)
                    sample_browser_preview(&g_browser, &eng, 0);
            } else if (eng.mode_page == EPICYCLE_PAGE_TRIM) {
                EpicyclePad *p = &eng.pads[eng.selected_pad];
                if (surf->shift_held) {
                    p->crossfade += (float)delta * 0.01f;
                    if (p->crossfade < 0.01f)
                        p->crossfade = 0.01f;
                    if (p->crossfade > 0.5f)
                        p->crossfade = 0.5f;
                } else if (p->freeform) {
                    /* Encoder: frame-fine last Start/End edge; else slide POS. */
                    if (g_trim_last_edge == 0 || g_trim_last_edge == 1) {
                        int frames = surf->shift_held ? delta : delta * 8;
                        trim_nudge_edge(&eng, eng.selected_pad, g_trim_last_edge, frames);
                        g_trim_lock_len = eng.pads[eng.selected_pad].window_length;
                    } else {
                        float frame = trim_frame_frac(&eng, eng.selected_pad);
                        trim_apply_pos(&eng, eng.selected_pad,
                                       p->window_start + (float)delta * frame * 8.0f);
                        trim_refresh_free_targets(&eng);
                    }
                    g_trim_pos_idle = 0;
                } else {
                    /* Encoder rotates N among 1/2/4/8/16/32 — never changes page.
                     * Only rewrite when stepped N actually differs. */
                    int step = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
                    if (step != 0) {
                        int n = engine_division_step(p->division_beats, step);
                        if (n != engine_division_quantize_slices(p->division_beats)) {
                            engine_set_division(&eng, eng.selected_pad, n);
                            g_trim_lock_len = eng.pads[eng.selected_pad].window_length;
                            pickup_targets_from_pad(&eng, eng.mode_page, surf->shift_held);
                            g_pick_on[1] = 0;
                            if (g_log_paths)
                                epicycle_log(&paths, "DIV N -> %d (wheel)", n);
                        }
                    }
                }
            } else if (eng.mode_page == EPICYCLE_PAGE_TUNE) {
                EpicyclePad *p = &eng.pads[eng.selected_pad];
                engine_set_pitch(&eng, eng.selected_pad, p->octave, p->semitone + delta, p->fine);
            } else if (eng.mode_page == EPICYCLE_PAGE_LFO) {
                /* Encoder always selects matrix slot (HitMod-style). */
                EpicyclePad *p = &eng.pads[eng.selected_pad];
                int s = p->mod_edit_slot + (delta > 0 ? 1 : (delta < 0 ? -1 : 0));
                if (s < 0)
                    s = EPICYCLE_MOD_SLOTS - 1;
                if (s >= EPICYCLE_MOD_SLOTS)
                    s = 0;
                p->mod_edit_slot = s;
            } else if (eng.mode_page == EPICYCLE_PAGE_MIX) {
                EpicyclePad *p = &eng.pads[eng.selected_pad];
                p->level += (float)delta * 0.01f;
                if (p->level < 0.0f)
                    p->level = 0.0f;
                if (p->level > 1.0f)
                    p->level = 1.0f;
                p->mod_level = p->level;
            } else if (eng.mode_page == EPICYCLE_PAGE_HITMOD) {
                int s = tb->hitmod_edit + (delta > 0 ? 1 : (delta < 0 ? -1 : 0));
                if (s < 0)
                    s = EPICYCLE_HITMOD_SLOTS - 1;
                if (s >= EPICYCLE_HITMOD_SLOTS)
                    s = 0;
                tb->hitmod_edit = s;
            } else if (eng.mode_page == EPICYCLE_PAGE_TOMBOLA) {
                /* Wheel only nudges spin if already moving — never from rest
                 * (Ash: spin must not jump alone). */
                if (tb->spin_speed > 0.05f || tb->spin_speed < -0.05f) {
                    float sp = tb->spin_speed + (float)delta * 0.15f;
                    if (sp < -7.5f)
                        sp = -7.5f;
                    if (sp > 7.5f)
                        sp = 7.5f;
                    /* Crossing zero parks at stop. */
                    if ((tb->spin_speed > 0.0f && sp < 0.0f) ||
                        (tb->spin_speed < 0.0f && sp > 0.0f))
                        sp = 0.0f;
                    tb->spin_speed = sp;
                }
            }
        }

        /* Trim / Tombola / Sample: every frame for responsive UI. */
        if (eng.mode_page == EPICYCLE_PAGE_TRIM || eng.mode_page == EPICYCLE_PAGE_TOMBOLA ||
            eng.mode_page == EPICYCLE_PAGE_SAMPLE || eng.mode_page == EPICYCLE_PAGE_DELAY ||
            eng.mode_page == EPICYCLE_PAGE_REVERB || eng.mode_page == EPICYCLE_PAGE_HITMOD ||
            (ticks % 2) == 0)
            ui_draw_loop(&hw, &eng, surf, tb, &g_browser);

        if (!audio_running()) {
            int32_t L[EPICYCLE_AUDIO_FRAMES], R[EPICYCLE_AUDIO_FRAMES];
            engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
        }

        sleep_ms(16);
        ticks++;

#ifndef EPICYCLE_ON_DEVICE
        if (ticks > 90)
            break;
#endif
    }

    ui_draw_splash("quit");
    sleep_ms(250);

    audio_set_engine(NULL);
    surface_close();
    audio_close();
    display_close();
    engine_shutdown(&eng);
    epicycle_log(&paths, "EPICYCLE exiting cleanly (launcher starts ac50)");
    return 0;
}
