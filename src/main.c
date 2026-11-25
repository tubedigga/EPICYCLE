/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE tombola — primary 2D projected drum; wall hits trigger pads.
 * SAMPLE+pad select; MUTE+pad toggle ball; FULL LEVEL → Hit Mod matrix;
 * B1/B2 cycle Tombola / Trim / Mix / … / HitMod; B3 quit.
 * 0.3.1: K1=shape K2=spin±dir K3=balls; empty start; bounce=one-shot; HitMod rows.
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
            "[--tombola-selftest] [--wav PATH]...\n"
            "  --demo             synth demo tones onto pads (host / no card samples)\n"
            "  --midi-selftest    host MIDI parser regression checks\n"
            "  --tombola-selftest physics containment + hit + mute ball checks\n"
            "  --wav PATH         load WAV into next bank slot (up to 16)\n"
            "  MPC_APP_DIR  set by the SD launcher to the payload folder\n"
            "  On device: K1 shape K2 spin K3 balls; MUTE+pad; FULL LEVEL=HitMod; B3 quit.\n",
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

static int try_load_card_samples(EpicycleEngine *eng, const EpicyclePaths *paths)
{
    int loaded = 0;
    char path[768];
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        snprintf(path, sizeof path, "%s/samples/pad%02d.wav", paths->data_dir, i + 1);
        if (engine_load_bank_wav(eng, i, path) == 0) {
            engine_assign_sample(eng, i, i);
            loaded++;
            continue;
        }
        snprintf(path, sizeof path, "%s/samples/%d.wav", paths->data_dir, i + 1);
        if (engine_load_bank_wav(eng, i, path) == 0) {
            engine_assign_sample(eng, i, i);
            loaded++;
        }
    }
    if (loaded == 0) {
        static const char *names[] = {"demo-a.wav", "demo-b.wav", "demo-c.wav", "demo-d.wav", NULL};
        for (int i = 0; names[i]; i++) {
            snprintf(path, sizeof path, "%s/%s", paths->app_dir, names[i]);
            if (engine_load_bank_wav(eng, i, path) == 0) {
                engine_assign_sample(eng, i, i);
                engine_assign_sample(eng, i + 4, i);
                loaded++;
            }
        }
    }
    return loaded;
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

/* Nudge Start/End by CC delta in frames (tiny grains on long samples). */
static void trim_nudge_edge(EpicycleEngine *eng, int pad, int end_edge, int dcc)
{
    if (!dcc)
        return;
    EpicyclePad *p = &eng->pads[pad];
    float frame = trim_frame_frac(eng, pad);
    size_t frames = (p->sample && p->sample->frames > 2) ? p->sample->frames : 44100;
    float step;
    if (p->window_length < 0.03f || (dcc >= -2 && dcc <= 2)) {
        /* Near-tiny or single ticks: true frame steps. */
        step = frame * (float)dcc;
    } else {
        /* Coarse slew: ~half-sample per full pot throw. */
        float fpc = (float)frames / 512.0f;
        if (fpc < 1.0f)
            fpc = 1.0f;
        step = frame * fpc * (float)dcc;
    }
    if (end_edge) {
        float end = p->window_start + p->window_length + step;
        engine_trim_set_end(eng, pad, end);
    } else {
        engine_trim_set_start(eng, pad, p->window_start + step);
    }
}

static void trim_apply_pos(EpicycleEngine *eng, int pad, float pos01)
{
    /* Preserve locked width exactly — even near-zero (no 2% inflate). */
    float lock = g_trim_lock_len > 0.0f ? g_trim_lock_len : eng->pads[pad].window_length;
    if (lock < 0.0f)
        lock = eng->pads[pad].window_length;
    eng->pads[pad].window_length = lock;
    engine_trim_set_position(eng, pad, pos01);
    /* engine_trim_set_position may nudge to 1-frame min; re-read actual. */
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
        t1 = (uint8_t)clampf_u8((int)(p->pan * 127.0f + 0.5f));
        t2 = shift ? (uint8_t)clampf_u8((int)((eng->kit_level - 0.05f) / 0.95f * 127.0f + 0.5f))
                   : g_pick_prev[2];
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
    case EPICYCLE_PAGE_LFO:
        t0 = (uint8_t)clampf_u8((int)((float)p->lfos[0].shape /
                                      (float)(EPICYCLE_LFO_SHAPE_COUNT - 1) * 127.0f +
                                  0.5f));
        t1 = (uint8_t)clampf_u8((int)((p->lfos[0].rate_hz - 0.05f) / 7.95f * 127.0f + 0.5f));
        t2 = (uint8_t)clampf_u8((int)(p->lfos[0].depth * 127.0f + 0.5f));
        break;
    case EPICYCLE_PAGE_TOMBOLA:
    case EPICYCLE_PAGE_HITMOD:
        /* Tombola/HitMod knobs applied directly — pickup not required. */
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
    /* After POS slide in FREE: Start/End follow brace — require re-cross. */
    EpicyclePad *p = &eng->pads[eng->selected_pad];
    float start = p->window_start;
    float end = start + p->window_length;
    if (end > 1.0f)
        end = 1.0f;
    g_pick_target[0] = (uint8_t)clampf_u8((int)(start * 127.0f + 0.5f));
    g_pick_target[1] = (uint8_t)clampf_u8((int)(end * 127.0f + 0.5f));
    g_pick_on[0] = 0;
    g_pick_on[1] = 0;
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
    /* FREE: ignore Start/End while POS active. DIV: ignore K2(N) while sliding. */
    unsigned ignore = 0;
    if (pos_exclusive)
        ignore = p->freeform ? 3u : 2u;
    unsigned mask = pickup_mask(cur, ignore);

    if (!mask) {
        if (g_trim_pos_idle < 1000)
            g_trim_pos_idle++;
        return;
    }

    /* Same-frame POS wins: never resize / change N while sliding. */
    if (mask & 4u)
        mask &= ~3u;

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
        if (mask & 1u) {
            /* First catch: absolute snap; then frame-fine relative (Ash grains). */
            if (!g_trim_rel[0]) {
                engine_trim_set_start(eng, eng->selected_pad, (float)surf->k1 / 127.0f);
                g_trim_rel[0] = 1;
                g_trim_edge_cc[0] = surf->k1;
            } else {
                int d = (int)surf->k1 - (int)g_trim_edge_cc[0];
                g_trim_edge_cc[0] = surf->k1;
                trim_nudge_edge(eng, eng->selected_pad, 0, d);
            }
            g_trim_lock_len = eng->pads[eng->selected_pad].window_length;
            g_pick_target[2] = (uint8_t)clampf_u8(
                (int)(eng->pads[eng->selected_pad].window_start * 127.0f + 0.5f));
            /* Do NOT disarm End — fighting Start/End made tiny grains laggy. */
            g_pick_on[2] = 0; /* POS re-cross only */
            g_trim_rel[2] = 0;
            g_trim_pos_idle = 1000;
            g_trim_last_edge = 0;
        }
        if (mask & 2u) {
            if (!g_trim_rel[1]) {
                engine_trim_set_end(eng, eng->selected_pad, (float)surf->k2 / 127.0f);
                g_trim_rel[1] = 1;
                g_trim_edge_cc[1] = surf->k2;
            } else {
                int d = (int)surf->k2 - (int)g_trim_edge_cc[1];
                g_trim_edge_cc[1] = surf->k2;
                trim_nudge_edge(eng, eng->selected_pad, 1, d);
            }
            g_trim_lock_len = eng->pads[eng->selected_pad].window_length;
            g_pick_target[2] = (uint8_t)clampf_u8(
                (int)(eng->pads[eng->selected_pad].window_start * 127.0f + 0.5f));
            g_pick_on[2] = 0;
            g_trim_rel[2] = 0;
            g_trim_pos_idle = 1000;
            g_trim_last_edge = 1;
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
        /* Spare gesture: jump to Hit Mod physics matrix stub. */
        eng->mode_page = EPICYCLE_PAGE_HITMOD;
        surface_set_hitmod_led(1);
        if (g_log_paths)
            epicycle_log(g_log_paths, "page -> HITMOD (enc)");
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
            puts(rc == 0 ? "midi-selftest: ok" : "midi-selftest: FAIL");
            return rc == 0 ? 0 : 1;
        }
        if (strcmp(argv[i], "--tombola-selftest") == 0) {
            int rc = tombola_selftest();
            puts(rc == 0 ? "tombola-selftest: ok" : "tombola-selftest: FAIL");
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
        loaded = try_load_card_samples(&eng, &paths);
    if (force_demo || loaded == 0) {
        engine_host_demo_setup(&eng);
        epicycle_log(&paths, "using synth demo bank (DIV4/DIV8/DIV16 equal-slice pads)");
    } else {
        /* Stay freeform so Trim Start/End work on every pad; encoder push
         * toggles division when wanted (forced division made Start look dead). */
        epicycle_log(&paths, "loaded %d sample slot(s) (freeform Trim default)", loaded);
    }

    EpicycleTombola *tb = tombola_state();
    tombola_init(tb);
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
    /* CC19 follows hardware dump (0 = silence). Gain only — no voice restart. */
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

        if (st->hitmod_btn) {
            st->hitmod_btn = 0;
            eng.mode_page = EPICYCLE_PAGE_HITMOD;
            surface_set_hitmod_led(1);
            pickup_arm(surf, &eng);
            epicycle_log(&paths, "page -> HITMOD (FULL LEVEL)");
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
                    eng.selected_pad = i;
                    surface_set_selected_pad(i);
                    surface_set_loop_led(eng.pads[i].loop_enable);
                    engine_sync_mod_window(&eng, i);
                    pickup_arm(surf, &eng);
                } else if (surf->mute_held) {
                    /* Hold MUTE + pad: add/remove ball (compose 1–2 then build). */
                    tombola_toggle_ball(tb, i);
                    if (!tombola_ball_active(tb, i))
                        engine_note_off(&eng, i); /* Release fade when removed */
                    epicycle_log(&paths, "MUTE ball %d -> %s", i + 1,
                                 tombola_ball_active(tb, i) ? "in" : "out");
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
            /* Pad release never stops audio — latch toggles on press; one-shot
             * runs to the end of the brace in the renderer. */
            if (st->pad_up[i])
                st->pad_up[i] = 0;
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
        }
        if (surf->shift_held != last_shift) {
            last_shift = surf->shift_held;
            pickup_arm(surf, &eng);
        }

        /* CC19 → gain only (smoothed in engine). Never restarts voices / Trim. */
        if (surf->main_seen && surf->main_vol != last_main) {
            engine_set_master_gain_cc(&eng, surf->main_vol);
            last_main = surf->main_vol;
        }
        /* Fader = master gain on every page (shape morph moved to K1). */
        if (surf->fader_engaged && surf->fader != last_fader) {
            last_fader = surf->fader;
            engine_set_fader_gain_cc(&eng, surf->fader);
        }

        if (eng.mode_page == EPICYCLE_PAGE_TOMBOLA) {
            /* Engage latches: ignore dump + pot noise until user moves ≥3. */
            static uint8_t tk1 = 255, tk2 = 255, tk3 = 255;
            static uint8_t tk1_dump = 255, tk2_dump = 255, tk3_dump = 255;
            static int tk1_on, tk2_on, tk3_on;
            static uint8_t tk2_pend = 255;
            static int tk2_pend_n;
            if (tk1_dump == 255) {
                tk1_dump = surf->k1;
                tk2_dump = surf->k2;
                tk3_dump = surf->k3;
                tk1 = surf->k1;
                tk2 = surf->k2;
                tk3 = surf->k3;
            }
            if (!tk1_on) {
                int d = (int)surf->k1 - (int)tk1_dump;
                if (d < 0)
                    d = -d;
                if (d >= 3)
                    tk1_on = 1;
            }
            if (tk1_on && surf->k1 != tk1) {
                tk1 = surf->k1;
                tombola_set_morph(tb, (float)surf->k1 / 127.0f);
            }
            if (!tk2_on) {
                int d = (int)surf->k2 - (int)tk2_dump;
                if (d < 0)
                    d = -d;
                if (d >= 3)
                    tk2_on = 1;
            }
            if (tk2_on && surf->k2 != tk2) {
                /* Confirm spin CC twice — reject single-sample jumps (Ash). */
                if (surf->k2 == tk2_pend) {
                    tk2_pend_n++;
                } else {
                    tk2_pend = surf->k2;
                    tk2_pend_n = 1;
                }
                if (tk2_pend_n >= 2) {
                    int d = (int)surf->k2 - (int)tk2;
                    if (d < 0)
                        d = -d;
                    if (d >= 2) {
                        tk2 = surf->k2;
                        tombola_set_spin_cc(tb, surf->k2);
                    }
                    tk2_pend_n = 0;
                }
            }
            /* K3 ball count optional; MUTE+pad is primary compose path. */
            if (!tk3_on) {
                int d = (int)surf->k3 - (int)tk3_dump;
                if (d < 0)
                    d = -d;
                if (d >= 3)
                    tk3_on = 1;
            }
            if (tk3_on && surf->k3 != tk3) {
                tk3 = surf->k3;
                int n = (int)(((int)surf->k3 * EPICYCLE_PAD_COUNT + 63) / 127);
                if (n > EPICYCLE_PAD_COUNT)
                    n = EPICYCLE_PAD_COUNT;
                tombola_set_ball_count(tb, n);
                for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
                    if (!tombola_ball_active(tb, i))
                        engine_note_off(&eng, i);
                }
            }
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
        } else if (eng.mode_page == EPICYCLE_PAGE_HITMOD) {
            if (tb->hitmod_edit < 0)
                tb->hitmod_edit = 0;
            if (tb->hitmod_edit >= EPICYCLE_HITMOD_SLOTS)
                tb->hitmod_edit = EPICYCLE_HITMOD_SLOTS - 1;
            EpicycleHitModRoute *r = &tb->hitmods[tb->hitmod_edit];
            /* Matrix row edit: K1=dest, K2=amount, K3=src (shift: threshold). */
            static uint8_t hk1 = 255, hk2 = 255, hk3 = 255;
            if (surf->k1 != hk1) {
                hk1 = surf->k1;
                int d = (int)((float)surf->k1 / 127.0f * (float)(EPICYCLE_HITMOD_DEST_COUNT - 1) +
                              0.5f);
                if (d < 0)
                    d = 0;
                if (d >= EPICYCLE_HITMOD_DEST_COUNT)
                    d = EPICYCLE_HITMOD_DEST_COUNT - 1;
                r->dest = (EpicycleHitModDest)d;
                r->enabled = (r->dest != EPICYCLE_HITMOD_NONE);
            }
            if (surf->k2 != hk2) {
                hk2 = surf->k2;
                r->amount = (float)surf->k2 / 127.0f;
            }
            if (surf->k3 != hk3) {
                hk3 = surf->k3;
                if (surf->shift_held) {
                    r->threshold = (float)surf->k3 / 127.0f;
                } else {
                    int s = (int)((float)surf->k3 / 127.0f *
                                      (float)(EPICYCLE_HITMOD_SRC_COUNT - 1) +
                                  0.5f);
                    if (s < 0)
                        s = 0;
                    if (s >= EPICYCLE_HITMOD_SRC_COUNT)
                        s = EPICYCLE_HITMOD_SRC_COUNT - 1;
                    r->src = (EpicycleHitModSrc)s;
                }
            }
        } else if (eng.mode_page == EPICYCLE_PAGE_TRIM) {
            handle_trim_knobs(&eng, surf);
        } else {
            const uint8_t cur[3] = {surf->k1, surf->k2, surf->k3};
            unsigned mask = pickup_mask(cur, 0);
            if (mask) {
                if (surf->shift_held && eng.mode_page == EPICYCLE_PAGE_MIX) {
                    if (mask & 4u)
                        eng.kit_level = 0.05f + 0.95f * ((float)surf->k3 / 127.0f);
                    if (mask & 1u)
                        engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page,
                                                    surf->k1, surf->k2, surf->k3, 1u);
                    if (mask & 2u)
                        engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page,
                                                    surf->k1, surf->k2, surf->k3, 2u);
                } else {
                    engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page, surf->k1,
                                                surf->k2, surf->k3, mask);
                }
            }
        }

        /* Always step tombola (hits fire even while editing other pages). */
        tombola_step(tb, 1.0f / 60.0f);
        {
            int pads[EPICYCLE_PAD_COUNT];
            float vels[EPICYCLE_PAD_COUNT];
            int n = tombola_drain_hits(tb, pads, vels, EPICYCLE_PAD_COUNT);
            for (int h = 0; h < n; h++) {
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
            if (eng.mode_page == EPICYCLE_PAGE_TRIM) {
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
                EpicycleLfo *l = &eng.pads[eng.selected_pad].lfos[0];
                if (l->sync_beats > 0.0f) {
                    l->sync_beats += (float)delta * 0.25f;
                    if (l->sync_beats < 0.25f)
                        l->sync_beats = 0.25f;
                    if (l->sync_beats > 8.0f)
                        l->sync_beats = 8.0f;
                } else {
                    l->rate_hz += (float)delta * 0.02f;
                    if (l->rate_hz < 0.05f)
                        l->rate_hz = 0.05f;
                    if (l->rate_hz > 8.0f)
                        l->rate_hz = 8.0f;
                }
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

        /* Trim / Tombola: every frame for responsive brace + spheres.
         * Other pages can skip a couple ticks. */
        if (eng.mode_page == EPICYCLE_PAGE_TRIM || eng.mode_page == EPICYCLE_PAGE_TOMBOLA ||
            eng.mode_page == EPICYCLE_PAGE_HITMOD || (ticks % 2) == 0)
            ui_draw_loop(&hw, &eng, surf, tb);

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
