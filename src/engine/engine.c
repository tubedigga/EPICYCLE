/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE loop engine v0:
 *  - Per-pad sample with scrollable loop brace (window start + length)
 *  - Equal-slice division (sample / N) or free-run length
 *  - Crossfade looping; pitch via octave/semi/fine (no warp)
 *  - Up to 16 voices; LFO brace scrub budgeted to EPICYCLE_SCRUB_BUDGET pads
 *  - Per-pad modulation matrix hooks (multi-LFO → brace/pitch/level/filter)
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* Near-zero FREE width: 1 sample frame when bank known, else tiny epsilon.
 * No forced musical min-gap (Ash: Start/End froze at the old ~2% clamp). */
static float trim_min_len(const EpicyclePad *p)
{
    if (p && p->sample && p->sample->frames > 2)
        return 1.0f / (float)p->sample->frames;
    return 1e-6f;
}

static float pitch_ratio(int octave, int semi, float fine)
{
    float semis = (float)(octave * 12 + semi) + fine / 100.0f;
    return powf(2.0f, semis / 12.0f);
}

static float lfo_wave(EpicycleLfoShape shape, float phase01)
{
    float p = phase01 - floorf(phase01);
    switch (shape) {
    case EPICYCLE_LFO_TRI:
        return p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
    case EPICYCLE_LFO_SAW:
        return 2.0f * p - 1.0f;
    case EPICYCLE_LFO_SQUARE:
        return p < 0.5f ? 1.0f : -1.0f;
    case EPICYCLE_LFO_SINE:
    default:
        return sinf((float)(2.0 * M_PI) * p);
    }
}

static void pad_defaults(EpicyclePad *p)
{
    memset(p, 0, sizeof *p);
    p->window_start = 0.0f;
    p->window_length = 1.0f;
    p->freeform = 1;
    p->division_beats = 4;
    p->level = 0.8f;
    p->pan = 0.5f;
    p->octave = 0;
    p->semitone = 0;
    p->fine = 0.0f;
    p->crossfade = 0.05f;
    p->loop_enable = 0; /* Tombola default: one-shot; LOOP button enables latch */
    p->ping_pong = 0;   /* Forward; SHIFT on Trim selects Ping-pong */
    p->allow_scrub = 1;
    p->amp_attack = 0.02f;  /* slight soft attack — less clicky/cheap */
    p->amp_decay = 0.22f;
    p->amp_sustain = 0.70f;
    p->amp_release = 0.28f;
    p->vel_sens = 1.0f;
    p->filter_cutoff = 1.0f; /* open */
    p->filter_reso = 0.0f;
    p->filter_type = 0; /* OFF = fully hearable until user picks a type */
    p->filt_attack = 0.05f;
    p->filt_decay = 0.40f;
    p->filt_depth = 0.0f; /* FEnv audible once depth > 0 (auto-LPF) */
    p->send_rev = 0.18f;  /* light default wet — dry-only was a complaint */
    p->send_del = 0.10f;
    p->mod_window_start = 0.0f;
    p->mod_window_length = 1.0f;
    p->mod_pitch_ratio = 1.0f;
    p->mod_level = 0.8f;
    p->mod_filter_cutoff = 1.0f;

    /* Default mod matrix: LFO0 → brace start (off until enabled); LFO1 → pitch stub. */
    for (int i = 0; i < EPICYCLE_LFO_PER_PAD; i++) {
        p->lfos[i].shape = EPICYCLE_LFO_SINE;
        p->lfos[i].rate_hz = 0.25f + 0.1f * (float)i;
        p->lfos[i].sync_beats = 0.0f;
        p->lfos[i].depth = 0.0f;
        p->lfos[i].phase = 0.0f;
        p->lfos[i].enabled = 0;
    }
    for (int i = 0; i < EPICYCLE_MOD_SLOTS; i++) {
        p->mods[i].lfo_index = -1;
        p->mods[i].dest = EPICYCLE_MOD_NONE;
        p->mods[i].amount = 0.0f;
    }
    p->mods[0].lfo_index = 0;
    p->mods[0].dest = EPICYCLE_MOD_BRACE_START;
    p->mods[0].amount = 1.0f;
    p->mods[1].lfo_index = 1;
    p->mods[1].dest = EPICYCLE_MOD_PITCH;
    p->mods[1].amount = 0.0f;
}

const char *epicycle_page_name(EpicycleModePage page)
{
    switch (page) {
    case EPICYCLE_PAGE_TOMBOLA:
        return "TOMBOLA";
    case EPICYCLE_PAGE_TRIM:
        return "TRIM";
    case EPICYCLE_PAGE_MIX:
        return "MIX";
    case EPICYCLE_PAGE_AMP:
        return "AMP";
    case EPICYCLE_PAGE_TUNE:
        return "TUNE";
    case EPICYCLE_PAGE_FILTER:
        return "FILT";
    case EPICYCLE_PAGE_FILT_ENV:
        return "FENV";
    case EPICYCLE_PAGE_LFO:
        return "LFO";
    case EPICYCLE_PAGE_HITMOD:
        return "HITMOD";
    default:
        return "?";
    }
}

const char *epicycle_hitmod_dest_name(EpicycleHitModDest dest)
{
    switch (dest) {
    case EPICYCLE_HITMOD_LEVEL:
        return "VOL";
    case EPICYCLE_HITMOD_PITCH:
        return "PITCH";
    case EPICYCLE_HITMOD_FILTER:
        return "CUT";
    case EPICYCLE_HITMOD_SEND_A:
        return "SNDA";
    case EPICYCLE_HITMOD_SEND_B:
        return "SNDB";
    case EPICYCLE_HITMOD_NONE:
    default:
        return "OFF";
    }
}

const char *epicycle_hitmod_src_name(EpicycleHitModSrc src)
{
    switch (src) {
    case EPICYCLE_HITMOD_SRC_ANGLE:
        return "ANGL";
    case EPICYCLE_HITMOD_SRC_HARDNESS:
        return "HARD";
    case EPICYCLE_HITMOD_SRC_IMPACT:
    default:
        return "IMPA";
    }
}

const char *epicycle_filter_type_name(int type)
{
    switch (type) {
    case 1:
        return "LPF";
    case 2:
        return "HPF";
    case 3:
        return "BPF";
    case 4:
        return "NOTCH";
    case 0:
    default:
        return "OFF";
    }
}

/* Musical slice counts for encoder / discoverability (Ash equal-slice grid). */
static const int k_div_steps[] = {1, 2, 4, 8, 16, 32};
#define K_DIV_STEP_N ((int)(sizeof k_div_steps / sizeof k_div_steps[0]))

float engine_division_slice_frac(int slices)
{
    int n = slices < 1 ? 1 : slices;
    if (n > EPICYCLE_MAX_DIVISION)
        n = EPICYCLE_MAX_DIVISION;
    return 1.0f / (float)n;
}

int engine_division_quantize_slices(int slices)
{
    /* Only musical powers of two — never leave N at 3/5/6/… (Ash: spontaneous jumps). */
    if (slices < 1)
        return 1;
    int best = k_div_steps[0];
    int best_d = 999;
    for (int i = 0; i < K_DIV_STEP_N; i++) {
        int d = k_div_steps[i] - slices;
        if (d < 0)
            d = -d;
        if (d < best_d) {
            best_d = d;
            best = k_div_steps[i];
        }
    }
    return best;
}

int engine_division_from_cc(uint8_t cc)
{
    /* Map pot across musical powers of two (not linear 1..32 — Ash DIV32 was
     * "full sample" under old tempo math; equal-slice wants DIV32 = 1/32). */
    int idx = (int)((cc * (K_DIV_STEP_N - 1) + 63) / 127);
    if (idx < 0)
        idx = 0;
    if (idx >= K_DIV_STEP_N)
        idx = K_DIV_STEP_N - 1;
    return k_div_steps[idx];
}

int engine_division_from_cc_sticky(uint8_t cc, int current_n)
{
    /* Schmitt-trigger around step boundaries so MIDI pot noise cannot flicker
     * N (e.g. CC 63↔64 = DIV4↔DIV8). Keep current until clearly in a new zone. */
    int cur = engine_division_quantize_slices(current_n);
    int raw = engine_division_from_cc(cc);
    if (raw == cur)
        return cur;

    int cur_i = 0, raw_i = 0;
    for (int i = 0; i < K_DIV_STEP_N; i++) {
        if (k_div_steps[i] == cur)
            cur_i = i;
        if (k_div_steps[i] == raw)
            raw_i = i;
    }
    /* Zone centers at i*127/5; boundaries midway. Require CC past midpoint
     * by a deadband before accepting the step change. */
    int lo = cur_i < raw_i ? cur_i : raw_i;
    int hi = cur_i < raw_i ? raw_i : cur_i;
    int boundary = (lo * 127 + hi * 127) / (2 * (K_DIV_STEP_N - 1));
    const int dead = 4;
    if (raw_i > cur_i) {
        if ((int)cc < boundary + dead)
            return cur;
    } else {
        if ((int)cc > boundary - dead)
            return cur;
    }
    return raw;
}

uint8_t engine_division_cc_center(int slices)
{
    int n = engine_division_quantize_slices(slices);
    int si = 0;
    for (int i = 0; i < K_DIV_STEP_N; i++) {
        if (k_div_steps[i] == n) {
            si = i;
            break;
        }
    }
    return (uint8_t)((si * 127) / (K_DIV_STEP_N - 1));
}

int engine_division_step(int slices, int delta)
{
    int cur = engine_division_quantize_slices(slices);
    int best = 0;
    int best_d = 999;
    for (int i = 0; i < K_DIV_STEP_N; i++) {
        int d = k_div_steps[i] - cur;
        if (d < 0)
            d = -d;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    best += delta;
    if (best < 0)
        best = 0;
    if (best >= K_DIV_STEP_N)
        best = K_DIV_STEP_N - 1;
    return k_div_steps[best];
}

int engine_division_slice_index(const EpicyclePad *pad)
{
    if (!pad)
        return 0;
    int n = engine_division_quantize_slices(pad->division_beats);
    float slice = engine_division_slice_frac(n);
    int idx = (int)(pad->window_start / slice + 0.5f);
    if (idx < 0)
        idx = 0;
    if (idx >= n)
        idx = n - 1;
    return idx;
}

void engine_division_snap_brace(EpicycleEngine *eng, int pad)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    if (p->freeform)
        return;
    int n = engine_division_quantize_slices(p->division_beats);
    float slice = engine_division_slice_frac(n);
    int idx = engine_division_slice_index(p);
    p->division_beats = n;
    p->window_length = slice;
    p->window_start = (float)idx * slice;
    if (p->window_start + p->window_length > 1.0f)
        p->window_start = 1.0f - p->window_length;
    if (p->window_start < 0.0f)
        p->window_start = 0.0f;
    engine_sync_mod_window(eng, pad);
}

void engine_division_set_slice_index(EpicycleEngine *eng, int pad, int slice_index)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    if (p->freeform)
        return;
    int n = engine_division_quantize_slices(p->division_beats);
    if (slice_index < 0)
        slice_index = 0;
    if (slice_index >= n)
        slice_index = n - 1;
    p->window_length = engine_division_slice_frac(n);
    p->window_start = (float)slice_index * p->window_length;
    engine_sync_mod_window(eng, pad);
}

static float division_length_frac(const EpicyclePad *pad)
{
    /* Equal-slice: length = 1/N of the sample (Ash musical chops). */
    return engine_division_slice_frac(pad->division_beats);
}

static void resolve_window(const EpicycleEngine *eng, EpicyclePad *pad, float *start_out,
                           float *len_out)
{
    (void)eng;
    float len = pad->freeform ? pad->window_length : division_length_frac(pad);
    len = clampf(len, trim_min_len(pad), 1.0f);
    float start = clampf(pad->window_start, 0.0f, 1.0f - len);
    *start_out = start;
    *len_out = len;
}

static int pad_wants_brace_scrub(const EpicyclePad *pad)
{
    if (!pad->allow_scrub)
        return 0;
    for (int m = 0; m < EPICYCLE_MOD_SLOTS; m++) {
        if (pad->mods[m].dest != EPICYCLE_MOD_BRACE_START &&
            pad->mods[m].dest != EPICYCLE_MOD_BRACE_LEN)
            continue;
        int li = pad->mods[m].lfo_index;
        if (li < 0 || li >= EPICYCLE_LFO_PER_PAD)
            continue;
        if (pad->lfos[li].enabled && pad->lfos[li].depth > 0.0f)
            return 1;
    }
    return 0;
}

void engine_sync_mod_window(EpicycleEngine *eng, int pad)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    float start, len;
    resolve_window(eng, p, &start, &len);
    /* Only freeze to base window when brace scrub LFO is off. */
    if (!pad_wants_brace_scrub(p)) {
        p->mod_window_start = start;
        p->mod_window_length = len;
    }
    p->mod_pitch_ratio = pitch_ratio(p->octave, p->semitone, p->fine);
    p->mod_level = p->level;
    p->mod_filter_cutoff = p->filter_cutoff;
}

/* First EPICYCLE_SCRUB_BUDGET pads (by index) that want brace scrub may scan;
 * later pads keep pitch/level LFO but freeze the window. */
static int pad_may_scrub(const EpicycleEngine *eng, int pad_index)
{
    int rank = 0;
    for (int i = 0; i <= pad_index; i++) {
        if (pad_wants_brace_scrub(&eng->pads[i])) {
            if (i == pad_index)
                return rank < EPICYCLE_SCRUB_BUDGET;
            rank++;
        }
    }
    return 0;
}

static void apply_modulation(EpicycleEngine *eng, int pad_index, size_t frames)
{
    EpicyclePad *pad = &eng->pads[pad_index];
    float base_start, base_len;
    resolve_window(eng, pad, &base_start, &base_len);

    float d_start = 0, d_len = 0, d_pitch = 0, d_level = 0, d_filt = 0;

    float dt = (float)frames / (float)eng->sample_rate;
    float beats_adv = (eng->tempo_bpm / 60.0f) * dt;

    for (int li = 0; li < EPICYCLE_LFO_PER_PAD; li++) {
        EpicycleLfo *l = &pad->lfos[li];
        if (!l->enabled)
            continue;
        float adv;
        if (l->sync_beats > 0.0f)
            adv = beats_adv / l->sync_beats;
        else
            adv = l->rate_hz * dt;
        l->phase = l->phase + adv;
        l->phase -= floorf(l->phase);
    }

    int scrub_ok = pad_may_scrub(eng, pad_index);

    for (int m = 0; m < EPICYCLE_MOD_SLOTS; m++) {
        EpicycleModRoute *r = &pad->mods[m];
        if (r->lfo_index < 0 || r->lfo_index >= EPICYCLE_LFO_PER_PAD)
            continue;
        if (r->dest == EPICYCLE_MOD_NONE)
            continue;
        EpicycleLfo *l = &pad->lfos[r->lfo_index];
        if (!l->enabled || l->depth <= 0.0f)
            continue;
        float w = lfo_wave(l->shape, l->phase) * l->depth * r->amount;
        switch (r->dest) {
        case EPICYCLE_MOD_BRACE_START:
            /* Brace scrub = Position slide (window offset), not Start/End width.
             * Manual Trim knobs set base window; LFO adds on top — keep this path. */
            if (scrub_ok)
                d_start += w * 0.5f; /* ±0.5 of sample at full depth */
            break;
        case EPICYCLE_MOD_BRACE_LEN:
            /* Width modulation reserved; scanning LFO must not change length. */
            (void)w;
            break;
        case EPICYCLE_MOD_PITCH:
            d_pitch += w * 2.0f; /* ±2 semitones at full */
            break;
        case EPICYCLE_MOD_LEVEL:
            d_level += w * 0.5f;
            break;
        case EPICYCLE_MOD_FILTER:
            d_filt += w * 0.5f;
            break;
        default:
            break;
        }
    }

    float len = clampf(base_len + d_len, trim_min_len(pad), 1.0f);
    float start = clampf(base_start + d_start, 0.0f, 1.0f - len);
    pad->mod_window_start = start;
    pad->mod_window_length = len;
    pad->mod_pitch_ratio = pitch_ratio(pad->octave, pad->semitone, pad->fine + d_pitch * 100.0f);
    pad->mod_level = clampf(pad->level + d_level, 0.0f, 1.0f);
    pad->mod_filter_cutoff = clampf(pad->filter_cutoff + d_filt, 0.0f, 1.0f);
}

static void recount_scrub(EpicycleEngine *eng)
{
    int n = 0;
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (pad_wants_brace_scrub(&eng->pads[i]))
            n++;
    }
    eng->scrub_active = n > EPICYCLE_SCRUB_BUDGET ? EPICYCLE_SCRUB_BUDGET : n;
}

static int steal_voice(EpicycleEngine *eng)
{
    int best = 0;
    float best_env = 2.0f;
    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        if (!eng->voices[i].active)
            return i;
        if (eng->voices[i].env < best_env) {
            best_env = eng->voices[i].env;
            best = i;
        }
    }
    eng->voices[best].active = 0;
    return best;
}

static void read_sample(const EpicycleSample *s, double pos, float *L, float *R)
{
    if (!s || !s->data || s->frames == 0) {
        *L = *R = 0;
        return;
    }
    if (pos < 0)
        pos = 0;
    size_t i0 = (size_t)pos;
    if (i0 >= s->frames - 1) {
        *L = s->data[(s->frames - 1) * 2];
        *R = s->data[(s->frames - 1) * 2 + 1];
        return;
    }
    float frac = (float)(pos - (double)i0);
    float l0 = s->data[i0 * 2], r0 = s->data[i0 * 2 + 1];
    float l1 = s->data[(i0 + 1) * 2], r1 = s->data[(i0 + 1) * 2 + 1];
    *L = l0 + (l1 - l0) * frac;
    *R = r0 + (r1 - r0) * frac;
}

void engine_init(EpicycleEngine *eng, unsigned sample_rate)
{
    memset(eng, 0, sizeof *eng);
    eng->tempo_bpm = 120.0f;
    eng->sample_rate = sample_rate ? sample_rate : 44100;
    eng->running = 1;
    eng->selected_pad = 0;
    eng->main_sq = 1.0f; /* until surface CC19 arrives */
    eng->main_sq_target = 1.0f;
    eng->fader_gain = 1.0f; /* always full — MAIN VOL sole master (Ash Trim mute) */
    eng->master_gain = 1.0f;
    eng->kit_level = 1.0f;
    eng->fx_bus_rev = 0.0f;
    eng->fx_bus_del = 0.0f;
    eng->mode_page = EPICYCLE_PAGE_TOMBOLA;
    eng->bank_count = EPICYCLE_PAD_COUNT;
    eng->bank = calloc((size_t)eng->bank_count, sizeof(EpicycleSample));
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++)
        pad_defaults(&eng->pads[i]);
}

void engine_shutdown(EpicycleEngine *eng)
{
    if (!eng)
        return;
    if (eng->bank) {
        for (int i = 0; i < eng->bank_count; i++)
            sample_free(&eng->bank[i]);
        free(eng->bank);
        eng->bank = NULL;
    }
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++)
        eng->pads[i].sample = NULL;
}

int engine_load_bank_wav(EpicycleEngine *eng, int slot, const char *path)
{
    if (!eng || !eng->bank || slot < 0 || slot >= eng->bank_count)
        return -1;
    sample_free(&eng->bank[slot]);
    return sample_load_wav(&eng->bank[slot], path);
}

int engine_assign_sample(EpicycleEngine *eng, int pad, int bank_slot)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return -1;
    if (bank_slot < 0 || bank_slot >= eng->bank_count || !eng->bank[bank_slot].data) {
        eng->pads[pad].sample = NULL;
        return -1;
    }
    eng->pads[pad].sample = &eng->bank[bank_slot];
    eng->pads[pad].window_start = 0.0f;
    eng->pads[pad].window_length =
        eng->pads[pad].freeform ? 0.5f : division_length_frac(&eng->pads[pad]);
    if (!eng->pads[pad].freeform)
        engine_division_snap_brace(eng, pad);
    else
        engine_sync_mod_window(eng, pad);
    return 0;
}

void engine_set_tempo(EpicycleEngine *eng, float bpm)
{
    /* Tempo still drives LFO sync; equal-slice division is tempo-independent. */
    eng->tempo_bpm = clampf(bpm, 40.0f, 300.0f);
}

void engine_set_division(EpicycleEngine *eng, int pad, int slices)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    if (slices <= 0) {
        p->freeform = 1;
        engine_sync_mod_window(eng, pad);
        return;
    }
    p->freeform = 0;
    p->division_beats = engine_division_quantize_slices(slices);
    engine_division_snap_brace(eng, pad);
}

/* Keep live voices inside a moved brace — Start/End must not mute (Ash). */
static void rebase_voices_to_window(EpicycleEngine *eng, int pad, float old_start, float old_len,
                                    float new_start, float new_len)
{
    EpicyclePad *p = &eng->pads[pad];
    if (!p->sample || !p->sample->data || p->sample->frames < 2)
        return;
    double frames = (double)p->sample->frames;
    double o0 = (double)old_start * frames;
    double o1 = o0 + (double)old_len * frames;
    double n0 = (double)new_start * frames;
    double n1 = n0 + (double)new_len * frames;
    double ospan = o1 - o0;
    double nspan = n1 - n0;
    if (nspan < 2.0)
        nspan = 2.0;
    if (n1 <= n0 + 1.0)
        n1 = n0 + 2.0;

    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        EpicycleVoice *v = &eng->voices[i];
        if (!v->active || v->pad_index != pad)
            continue;
        if (ospan > 2.0) {
            double rel = (v->phase - o0) / ospan;
            if (rel < 0.0)
                rel = 0.0;
            if (rel > 1.0)
                rel = 1.0;
            v->phase = n0 + rel * nspan;
        } else {
            if (v->phase < n0)
                v->phase = n0;
            if (v->phase >= n1)
                v->phase = n1 - 1.0;
        }
        if (v->phase < n0)
            v->phase = n0;
        if (v->phase >= n1)
            v->phase = n1 - 1.0;
        /* Stay in current amp stage — never force Release just for a trim move. */
        v->in_crossfade = 0;
    }
}

void engine_set_window(EpicycleEngine *eng, int pad, float start, float length)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    float old_start = p->window_start;
    float old_len = p->window_length;
    if (!p->freeform) {
        /* Division: length locked to 1/N; start snapped to grid. */
        p->window_start = clampf(start, 0.0f, 1.0f);
        engine_division_snap_brace(eng, pad);
        rebase_voices_to_window(eng, pad, old_start, old_len, p->window_start, p->window_length);
        return;
    }
    {
        float min_l = trim_min_len(p);
        p->window_length = clampf(length, min_l, 1.0f);
        p->window_start = clampf(start, 0.0f, 1.0f - p->window_length);
    }
    engine_sync_mod_window(eng, pad);
    rebase_voices_to_window(eng, pad, old_start, old_len, p->window_start, p->window_length);
}

void engine_set_pitch(EpicycleEngine *eng, int pad, int octave, int semi, float fine)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    /* Clamp to neutral-friendly ranges; 0/0/0 = unity pitch. */
    if (octave < -4)
        octave = -4;
    if (octave > 4)
        octave = 4;
    if (semi < -24)
        semi = -24;
    if (semi > 24)
        semi = 24;
    p->octave = octave;
    p->semitone = semi;
    p->fine = clampf(fine, -100.0f, 100.0f);
    p->mod_pitch_ratio = pitch_ratio(p->octave, p->semitone, p->fine);
}

void engine_toggle_loop(EpicycleEngine *eng, int pad)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    eng->pads[pad].loop_enable = eng->pads[pad].loop_enable ? 0 : 1;
    /* LOOP off: force every live voice for this pad to one-shot (no stuck wrap). */
    if (!eng->pads[pad].loop_enable) {
        for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
            if (eng->voices[i].active && eng->voices[i].pad_index == pad) {
                eng->voices[i].force_oneshot = 1;
                eng->voices[i].in_crossfade = 0;
            }
        }
    }
}

void engine_set_ping_pong(EpicycleEngine *eng, int pad, int on)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    eng->pads[pad].ping_pong = on ? 1 : 0;
}

void engine_trim_set_start(EpicycleEngine *eng, int pad, float start01)
{
    /* Move left edge; keep End fixed → changes region length/width only. */
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    float end = p->window_start + p->window_length;
    if (end > 1.0f)
        end = 1.0f;
    float min_l = trim_min_len(p);
    float start = clampf(start01, 0.0f, end - min_l);
    engine_set_window(eng, pad, start, end - start);
}

void engine_trim_set_end(EpicycleEngine *eng, int pad, float end01)
{
    /* Move right edge; keep Start fixed → changes region length/width only. */
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    float start = p->window_start;
    float min_l = trim_min_len(p);
    float end = clampf(end01, start + min_l, 1.0f);
    engine_set_window(eng, pad, start, end - start);
}

void engine_trim_set_position(EpicycleEngine *eng, int pad, float pos01)
{
    /* Slide whole brace; length unchanged. This is the LFO scrub target. */
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    float min_l = trim_min_len(p);
    float len = p->window_length;
    if (len < min_l)
        len = min_l;
    float start = clampf(pos01, 0.0f, 1.0f - len);
    engine_set_window(eng, pad, start, len);
}

enum { ENV_A = 0, ENV_D = 1, ENV_S = 2, ENV_R = 3 };

/* Map 0..1 knob → seconds (A/D/R). Squared curve: snappy lows, musical highs. */
static float adsr_time_s(float kn, float min_s, float max_s)
{
    float t = clampf(kn, 0.0f, 1.0f);
    float t2 = t * t;
    return min_s + (max_s - min_s) * t2;
}

static void voice_start_attack(EpicycleVoice *v, const EpicyclePad *p, float velocity)
{
    v->active = 1;
    v->phase = (double)p->mod_window_start * (double)p->sample->frames;
    v->velocity = clampf(velocity, 0.05f, 1.0f);
    v->env = 0.0f;
    v->env_stage = ENV_A;
    v->gate = 1;
    v->in_crossfade = 0;
    v->xfade_pos = 0;
    v->direction = 1;
    v->filt_z1_l = v->filt_z2_l = 0.0f;
    v->filt_z1_r = v->filt_z2_r = 0.0f;
    v->filt_env = 0.0f;
    v->filt_env_stage = 0; /* attack */
    v->impact_pitch = 1.0f;
    v->impact_filt = 0.0f;
    v->impact_level = 1.0f;
    /* Default one-shot; clear only when LOOP allows sustain/loop. */
    v->force_oneshot = 1;
}

void engine_note_on(EpicycleEngine *eng, int pad, float velocity)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    if (!p->sample || !p->sample->data)
        return;

    /* Retrigger same pad: reuse existing voice if playing. */
    int vidx = -1;
    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        if (eng->voices[i].active && eng->voices[i].pad_index == pad) {
            vidx = i;
            break;
        }
    }
    if (vidx < 0)
        vidx = steal_voice(eng);

    apply_modulation(eng, pad, 0);

    EpicycleVoice *v = &eng->voices[vidx];
    v->pad_index = pad;
    voice_start_attack(v, p, velocity);
    /* Pad press: loop sample only when LOOP latch is on for this pad. */
    v->force_oneshot = p->loop_enable ? 0 : 1;
}

void engine_note_on_impact(EpicycleEngine *eng, int pad, float velocity, float pitch_mul,
                           float filt_add, float level_mul)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    if (!p->sample || !p->sample->data)
        return;

    /* LOOP on: bounce toggles — sustaining → Release fade; else start loop. */
    if (p->loop_enable) {
        for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
            EpicycleVoice *v = &eng->voices[i];
            if (!v->active || v->pad_index != pad)
                continue;
            if (v->gate && v->env_stage != ENV_R) {
                v->gate = 0;
                v->env_stage = ENV_R;
                return; /* Release fades the loop out (Ash) */
            }
            break;
        }
    }

    /*
     * Wall hit = same voice path as pad press (Ash: bounce was near-silent).
     * Old path cleared gate → ADSR skipped sustain and died in ~15 ms.
     * Keep gate high so one-shot plays through the brace like a pad hit;
     * force_oneshot still blocks brace wrap when LOOP is off.
     */
    engine_note_on(eng, pad, velocity);
    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        if (eng->voices[i].active && eng->voices[i].pad_index == pad) {
            eng->voices[i].impact_pitch = clampf(pitch_mul, 0.25f, 4.0f);
            eng->voices[i].impact_filt = clampf(filt_add, -0.9f, 0.9f);
            eng->voices[i].impact_level = clampf(level_mul, 0.05f, 2.0f);
            eng->voices[i].force_oneshot = p->loop_enable ? 0 : 1;
            /* gate stays 1 — identical to pad press until brace end / LOOP off. */
            break;
        }
    }
}

void engine_stop_pad(EpicycleEngine *eng, int pad)
{
    /* Hard silence (MUTE ball remove). Prefer engine_note_off for musical fade. */
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        if (eng->voices[i].active && eng->voices[i].pad_index == pad)
            eng->voices[i].active = 0;
    }
}

void engine_note_off(EpicycleEngine *eng, int pad)
{
    /* Enter Release — amp ADSR fades out (loop or one-shot gate end). */
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        EpicycleVoice *v = &eng->voices[i];
        if (!v->active || v->pad_index != pad)
            continue;
        v->gate = 0;
        if (v->env_stage != ENV_R)
            v->env_stage = ENV_R;
    }
}

/* Advance ADSR one sample; returns 0 if voice finished Release.
 * Expo approach curves — less cheap/linear than prior ramp (Ash). */
static int voice_adsr_tick(EpicycleVoice *v, const EpicyclePad *pad, unsigned sr)
{
    float atk = adsr_time_s(pad->amp_attack, 0.001f, 2.0f);
    float dec = adsr_time_s(pad->amp_decay, 0.008f, 3.0f);
    float sus = clampf(pad->amp_sustain, 0.0f, 1.0f);
    float rel = adsr_time_s(pad->amp_release, 0.010f, 4.0f);
    float inv_sr = 1.0f / (float)(sr ? sr : 44100);

    /* Impact one-shots (gate already low): A→D then straight to Release (no S hold). */
    int skip_sustain = (!v->gate && v->force_oneshot && v->env_stage != ENV_R);

    if (!v->gate && v->env_stage == ENV_S)
        v->env_stage = ENV_R;

    switch (v->env_stage) {
    case ENV_A: {
        /* 1 - e^(-t/tau): soft start, still reaches ~1 quickly. */
        float coeff = 1.0f - expf(-inv_sr / (atk * 0.28f + 1e-5f));
        v->env += (1.0f - v->env) * coeff;
        if (v->env >= 0.995f) {
            v->env = 1.0f;
            v->env_stage = ENV_D;
        }
        break;
    }
    case ENV_D: {
        float target = skip_sustain ? 0.0f : sus;
        float coeff = 1.0f - expf(-inv_sr / (dec * 0.35f + 1e-5f));
        v->env += (target - v->env) * coeff;
        if (fabsf(v->env - target) <= 0.004f) {
            v->env = target;
            if (skip_sustain || !v->gate) {
                v->env_stage = ENV_R;
            } else {
                v->env_stage = ENV_S;
            }
        }
        break;
    }
    case ENV_S:
        v->env = sus;
        if (!v->gate)
            v->env_stage = ENV_R;
        break;
    case ENV_R:
    default: {
        /* Exponential release toward silence. */
        float coeff = 1.0f - expf(-inv_sr / (rel * 0.32f + 1e-5f));
        v->env += (0.0f - v->env) * coeff;
        if (v->env <= 0.001f) {
            v->env = 0.0f;
            v->active = 0;
            return 0;
        }
        break;
    }
    }
    return 1;
}

int engine_pad_voice_active(const EpicycleEngine *eng, int pad)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return 0;
    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        if (eng->voices[i].active && eng->voices[i].pad_index == pad)
            return 1;
    }
    return 0;
}

float engine_pad_playhead(const EpicycleEngine *eng, int pad)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return -1.0f;
    const EpicyclePad *p = &eng->pads[pad];
    if (!p->sample || !p->sample->data || p->sample->frames == 0)
        return -1.0f;
    for (int i = 0; i < EPICYCLE_VOICE_COUNT; i++) {
        if (!eng->voices[i].active || eng->voices[i].pad_index != pad)
            continue;
        double ph = eng->voices[i].phase / (double)p->sample->frames;
        if (ph < 0.0)
            ph = 0.0;
        if (ph > 1.0)
            ph = 1.0;
        return (float)ph;
    }
    return -1.0f;
}

void engine_lfo_set_enabled(EpicycleEngine *eng, int pad, int on)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    if (on) {
        p->lfos[0].enabled = 1;
        if (p->lfos[0].depth < 0.05f)
            p->lfos[0].depth = 0.35f;
        p->mods[0].lfo_index = 0;
        p->mods[0].dest = EPICYCLE_MOD_BRACE_START;
        p->mods[0].amount = 1.0f;
        p->allow_scrub = 1;
    } else {
        p->lfos[0].enabled = 0;
        p->lfos[0].depth = 0.0f;
    }
}

void engine_lfo_toggle_sync(EpicycleEngine *eng, int pad)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicycleLfo *l = &eng->pads[pad].lfos[0];
    if (l->sync_beats > 0.0f) {
        l->sync_beats = 0.0f;
        if (l->rate_hz < 0.05f)
            l->rate_hz = 0.25f;
    } else {
        l->sync_beats = 1.0f; /* 1 beat cycle */
    }
}

void engine_apply_page_knobs_mask(EpicycleEngine *eng, int pad, EpicycleModePage page, uint8_t k1,
                                  uint8_t k2, uint8_t k3, unsigned mask)
{
    if (!eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT || mask == 0)
        return;
    EpicyclePad *p = &eng->pads[pad];
    float t1 = (float)k1 / 127.0f;
    float t2 = (float)k2 / 127.0f;
    float t3 = (float)k3 / 127.0f;

    switch (page) {
    case EPICYCLE_PAGE_TRIM: {
        /*
         * Primary (no Shift): K1 Start + K2 End = region width (other edge fixed);
         * K3 Position = slide whole brace. Crossfade / ping-pong are Shift overlays
         * in main (not applied here).
         * Division mode: equal-slice grid — K2 picks N (1/2/4/8/16/32),
         * K1/K3 pick/slide brace onto a slice. Length always 1/N.
         */
        if (p->freeform) {
            if (mask & 1u)
                engine_trim_set_start(eng, pad, t1);
            if (mask & 2u)
                engine_trim_set_end(eng, pad, t2);
        } else {
            if (mask & 2u) {
                int n = engine_division_from_cc_sticky(k2, p->division_beats);
                if (n != engine_division_quantize_slices(p->division_beats))
                    engine_set_division(eng, pad, n);
            }
            if (mask & 1u) {
                int n = engine_division_quantize_slices(p->division_beats);
                int idx = (int)((t1 * (float)n));
                if (idx >= n)
                    idx = n - 1;
                engine_division_set_slice_index(eng, pad, idx);
            }
        }
        if (mask & 4u) {
            if (p->freeform) {
                engine_trim_set_position(eng, pad, t3);
            } else {
                int n = engine_division_quantize_slices(p->division_beats);
                int idx = (int)((t3 * (float)n));
                if (idx >= n)
                    idx = n - 1;
                engine_division_set_slice_index(eng, pad, idx);
            }
        }
        break;
    }
    case EPICYCLE_PAGE_MIX:
        /* Pad vol + pan; K3 = FX send (rev+delay); kit via SHIFT+K3 in main. */
        if (mask & 1u) {
            p->level = t1;
            p->mod_level = t1;
        }
        if (mask & 2u)
            p->pan = t2;
        if (mask & 4u) {
            p->send_rev = t3;
            p->send_del = t3 * 0.55f;
        }
        break;
    case EPICYCLE_PAGE_AMP:
        /* K1 A · K2 D · K3 S; SHIFT: K1 R · K2 vel (handled in main for shift). */
        if (mask & 1u)
            p->amp_attack = t1;
        if (mask & 2u)
            p->amp_decay = t2;
        if (mask & 4u)
            p->amp_sustain = t3;
        break;
    case EPICYCLE_PAGE_TUNE: {
        /* Neutral at CC 64 ≈ 0. Only update axes the user moved. */
        int semi = p->semitone;
        float fine = p->fine;
        int oct = p->octave;
        if (mask & 1u)
            semi = (int)(t1 * 48.0f + 0.5f) - 24; /* -24..+24; 64 → 0 */
        if (mask & 2u)
            fine = t2 * 200.0f - 100.0f; /* -100..+100; 64 → 0 */
        if (mask & 4u)
            oct = (int)(t3 * 8.0f + 0.5f) - 4; /* -4..+4; 64 → 0 */
        engine_set_pitch(eng, pad, oct, semi, fine);
        break;
    }
    case EPICYCLE_PAGE_FILTER:
        if (mask & 1u)
            p->filter_cutoff = t1;
        if (mask & 2u)
            p->filter_reso = t2;
        if (mask & 4u) {
            /* 0=OFF … 4=NOTCH */
            int ft = (int)(t3 * 4.0f + 0.5f);
            if (ft < 0)
                ft = 0;
            if (ft > 4)
                ft = 4;
            p->filter_type = ft;
        }
        break;
    case EPICYCLE_PAGE_FILT_ENV:
        if (mask & 1u)
            p->filt_attack = t1;
        if (mask & 2u)
            p->filt_decay = t2;
        if (mask & 4u)
            p->filt_depth = t3;
        break;
    case EPICYCLE_PAGE_LFO: {
        if (mask & 1u) {
            int shape = (int)(t1 * (float)(EPICYCLE_LFO_SHAPE_COUNT - 1) + 0.5f);
            if (shape < 0)
                shape = 0;
            if (shape >= EPICYCLE_LFO_SHAPE_COUNT)
                shape = EPICYCLE_LFO_SHAPE_COUNT - 1;
            p->lfos[0].shape = (EpicycleLfoShape)shape;
        }
        if (mask & 2u) {
            if (p->lfos[0].sync_beats > 0.0f) {
                static const float beats[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
                int idx = (int)(t2 * 5.0f + 0.5f);
                if (idx < 0)
                    idx = 0;
                if (idx > 5)
                    idx = 5;
                p->lfos[0].sync_beats = beats[idx];
            } else {
                p->lfos[0].rate_hz = 0.05f + 7.95f * t2;
            }
        }
        if (mask & 4u) {
            p->lfos[0].depth = t3;
            engine_lfo_set_enabled(eng, pad, t3 > 0.02f);
        }
        break;
    }
    default:
        break;
    }
}

void engine_apply_page_knobs(EpicycleEngine *eng, int pad, EpicycleModePage page, uint8_t k1,
                             uint8_t k2, uint8_t k3)
{
    engine_apply_page_knobs_mask(eng, pad, page, k1, k2, k3, 7u);
}

void engine_apply_knobs(EpicycleEngine *eng, int pad, uint8_t k1, uint8_t k2, uint8_t k3)
{
    if (!eng)
        return;
    engine_apply_page_knobs(eng, pad, eng->mode_page, k1, k2, k3);
}

static void recompute_master(EpicycleEngine *eng)
{
    /* Either control at 0 → true silence. Gain only — never restarts voices. */
    eng->master_gain = eng->main_sq * eng->fader_gain;
}

void engine_set_master_gain_cc(EpicycleEngine *eng, uint8_t cc19)
{
    if (!eng)
        return;
    /* CC19 → target only. Render smooths toward it so level restores without
     * clicks; never touches voices / windows / MIDI. */
    float t = (float)cc19 / 127.0f;
    eng->main_sq_target = t * t;
    if (cc19 == 0)
        eng->main_sq_target = 0.0f;
    /* Snap to silence immediately when knob hits 0. */
    if (cc19 == 0) {
        eng->main_sq = 0.0f;
        recompute_master(eng);
    }
}

void engine_set_fader_gain_cc(EpicycleEngine *eng, uint8_t cc20)
{
    if (!eng)
        return;
    /* Fader is morph on Tombola — never master volume (Ash Trim-page mute).
     * Keep API; force full so page changes cannot attenuate. */
    (void)cc20;
    eng->fader_gain = 1.0f;
    recompute_master(eng);
}

/* Chamberlin SVF — one sample in, selected mode out. z1=band, z2=low. */
static float voice_filter_tick(float in, float *z1, float *z2, float f, float q, int type)
{
    float band = *z1;
    float low = *z2;
    low += f * band;
    float high = in - low - q * band;
    band += f * high;
    /* Mild clamp against blow-ups at high reso */
    if (band > 4.0f)
        band = 4.0f;
    else if (band < -4.0f)
        band = -4.0f;
    if (low > 4.0f)
        low = 4.0f;
    else if (low < -4.0f)
        low = -4.0f;
    *z1 = band;
    *z2 = low;
    switch (type) {
    case 2:
        return high; /* HPF */
    case 3:
        return band; /* BPF */
    case 4:
        return high + low; /* NOTCH */
    case 1:
    default:
        return low; /* LPF */
    }
}

static void voice_apply_filter(EpicycleVoice *v, const EpicyclePad *pad, unsigned sr, float *L,
                               float *R)
{
    int type = pad->filter_type;
    float depth = pad->filt_depth;
    /* Depth > 0 with type OFF → auto LPF so FEnv is audible (Ash: felt stubbed). */
    if ((type <= 0 || type > 4) && depth > 0.02f)
        type = 1;
    if (type <= 0 || type > 4)
        return; /* OFF — bypass, fully hearable */

    float cut = clampf(pad->mod_filter_cutoff + v->impact_filt, 0.0f, 1.0f);
    if (depth > 0.001f) {
        float atk = 0.002f + pad->filt_attack * pad->filt_attack * 0.80f;
        float dec = 0.015f + pad->filt_decay * pad->filt_decay * 1.60f;
        float inv_sr = 1.0f / (float)(sr ? sr : 44100);
        if (v->filt_env_stage == 0) {
            float coeff = 1.0f - expf(-inv_sr / (atk * 0.30f + 1e-5f));
            v->filt_env += (1.0f - v->filt_env) * coeff;
            if (v->filt_env >= 0.995f) {
                v->filt_env = 1.0f;
                v->filt_env_stage = 1;
            }
        } else {
            float coeff = 1.0f - expf(-inv_sr / (dec * 0.35f + 1e-5f));
            v->filt_env += (0.0f - v->filt_env) * coeff;
            if (v->filt_env < 0.0f)
                v->filt_env = 0.0f;
        }
        /* Classic: start closed by depth, open with env peak, decay closed. */
        cut = clampf(cut - depth + depth * v->filt_env, 0.0f, 1.0f);
    }

    /* Exponential-ish cutoff: ~80 Hz .. 0.45 * Nyquist */
    float ny = 0.45f * (float)sr;
    float hz = 80.0f * powf(ny / 80.0f, clampf(cut, 0.0f, 1.0f));
    float f = 2.0f * sinf((float)M_PI * hz / (float)sr);
    if (f > 0.95f)
        f = 0.95f;
    if (f < 0.001f)
        f = 0.001f;
    /* Chamberlin "q" is damping: lower = more resonance. Map knob so
     * CC 0 = min reso (high damp), CC 127 = max reso (low damp). */
    float damp = 0.90f - clampf(pad->filter_reso, 0.0f, 1.0f) * 0.85f;
    float q = damp;
    if (f * q > 0.95f)
        q = 0.95f / f;

    *L = voice_filter_tick(*L, &v->filt_z1_l, &v->filt_z2_l, f, q, type);
    *R = voice_filter_tick(*R, &v->filt_z1_r, &v->filt_z2_r, f, q, type);
}

/* Lightweight stereo delay + 4-comb reverb (static buffers; MIT, no libs). */
enum { FX_DELAY_MAX = 22050 }; /* ~500 ms @ 44.1k */
static float g_fx_delay_l[FX_DELAY_MAX];
static float g_fx_delay_r[FX_DELAY_MAX];
static int g_fx_delay_w;
static float g_fx_ap_l, g_fx_ap_r;
static int g_fx_comb_i[4];
static float g_fx_comb_buf_l[4][1373];
static float g_fx_comb_buf_r[4][1373];
static const int FX_COMB_LEN[4] = {1117, 1187, 1279, 1351};

static void fx_process(float in_l, float in_r, float send_rev, float send_del, float *wet_l,
                       float *wet_r, unsigned sr)
{
    (void)sr;
    float dl = 0.0f, dr = 0.0f;
    /* Tempo-ish fixed delay (~280 ms) with feedback. */
    {
        int dlen = FX_DELAY_MAX * 280 / 500;
        if (dlen < 64)
            dlen = 64;
        if (dlen >= FX_DELAY_MAX)
            dlen = FX_DELAY_MAX - 1;
        int ri = g_fx_delay_w - dlen;
        if (ri < 0)
            ri += FX_DELAY_MAX;
        dl = g_fx_delay_l[ri];
        dr = g_fx_delay_r[ri];
        float fb = 0.38f;
        g_fx_delay_l[g_fx_delay_w] = in_l * send_del + dl * fb;
        g_fx_delay_r[g_fx_delay_w] = in_r * send_del + dr * fb;
        g_fx_delay_w++;
        if (g_fx_delay_w >= FX_DELAY_MAX)
            g_fx_delay_w = 0;
    }

    float rl = 0.0f, rr = 0.0f;
    float rin_l = in_l * send_rev + dl * 0.25f;
    float rin_r = in_r * send_rev + dr * 0.25f;
    for (int c = 0; c < 4; c++) {
        int len = FX_COMB_LEN[c];
        int i = g_fx_comb_i[c];
        float y_l = g_fx_comb_buf_l[c][i];
        float y_r = g_fx_comb_buf_r[c][i];
        float fb = 0.72f - 0.04f * (float)c;
        g_fx_comb_buf_l[c][i] = rin_l + y_l * fb;
        g_fx_comb_buf_r[c][i] = rin_r + y_r * fb;
        i++;
        if (i >= len)
            i = 0;
        g_fx_comb_i[c] = i;
        rl += y_l;
        rr += y_r;
    }
    rl *= 0.25f;
    rr *= 0.25f;
    /* One allpass for denser tail. */
    {
        float g = 0.5f;
        float yl = -g * rl + g_fx_ap_l;
        g_fx_ap_l = rl + g * yl;
        float yr = -g * rr + g_fx_ap_r;
        g_fx_ap_r = rr + g * yr;
        rl = yl;
        rr = yr;
    }
    *wet_l = dl * 0.85f + rl * 0.95f;
    *wet_r = dr * 0.85f + rr * 0.95f;
}

void engine_render(EpicycleEngine *eng, int32_t *out_l, int32_t *out_r, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        out_l[i] = 0;
        out_r[i] = 0;
    }
    if (!eng || !eng->running)
        return;

    recount_scrub(eng);
    for (int p = 0; p < EPICYCLE_PAD_COUNT; p++)
        apply_modulation(eng, p, frames);

    /* One-pole coeff toward main_sq_target (~5 ms) — computed once per block. */
    float main_a = 1.0f - expf(-1.0f / (0.005f * (float)eng->sample_rate + 1.0f));
    float bus_rev = clampf(eng->fx_bus_rev, 0.0f, 1.0f);
    float bus_del = clampf(eng->fx_bus_del, 0.0f, 1.0f);

    for (size_t n = 0; n < frames; n++) {
        float mixL = 0, mixR = 0;
        float sndL = 0, sndR = 0;
        float send_rev_acc = 0.0f, send_del_acc = 0.0f;
        int send_n = 0;

        for (int vi = 0; vi < EPICYCLE_VOICE_COUNT; vi++) {
            EpicycleVoice *v = &eng->voices[vi];
            if (!v->active)
                continue;
            EpicyclePad *pad = &eng->pads[v->pad_index];
            EpicycleSample *s = pad->sample;
            if (!s || !s->data) {
                v->active = 0;
                continue;
            }

            double win0 = (double)pad->mod_window_start * (double)s->frames;
            double win1 = win0 + (double)pad->mod_window_length * (double)s->frames;
            if (win1 <= win0 + 2.0) {
                /* Tiny grain: hold last in-brace phase instead of hard mute. */
                if (v->phase < win0)
                    v->phase = win0;
                if (v->phase >= win1 && win1 > win0 + 1.0)
                    v->phase = win1 - 1.0;
                if (win1 <= win0 + 1.0) {
                    v->active = 0;
                    continue;
                }
            }

            double xfade_frames =
                (double)pad->crossfade * (win1 - win0);
            if (xfade_frames < 2.0)
                xfade_frames = 2.0;
            if (xfade_frames > (win1 - win0) * 0.45)
                xfade_frames = (win1 - win0) * 0.45;

            float L = 0, R = 0;
            float pitch = pad->mod_pitch_ratio * (v->impact_pitch > 0.01f ? v->impact_pitch : 1.0f);
            double step = (double)pitch;
            if (v->direction < 0)
                step = -step;

            /* LOOP+bounce loops sample under ADSR; LOOP off = one-shot bounce. */
            int do_loop = pad->loop_enable && !v->force_oneshot;
            float edge_mul = 1.0f;
            if (do_loop && pad->ping_pong) {
                read_sample(s, v->phase, &L, &R);
                v->phase += step;
                if (v->phase >= win1) {
                    v->phase = win1 - 1.0;
                    if (v->phase < win0)
                        v->phase = win0;
                    v->direction = -1;
                    v->in_crossfade = 0;
                } else if (v->phase <= win0) {
                    v->phase = win0;
                    v->direction = 1;
                    v->in_crossfade = 0;
                }
                double edge = xfade_frames;
                double dist_end = win1 - v->phase;
                double dist_start = v->phase - win0;
                double near = dist_end < dist_start ? dist_end : dist_start;
                if (near < edge && edge > 1.0)
                    edge_mul = (float)(0.55 + 0.45 * (near / edge));
            } else if (do_loop) {
                if (v->direction < 0)
                    v->direction = 1;
                if (v->phase >= win1 - xfade_frames && !v->in_crossfade) {
                    v->in_crossfade = 1;
                    v->xfade_pos = 0;
                }
                if (v->in_crossfade) {
                    double fade = v->xfade_pos / xfade_frames;
                    if (fade > 1.0)
                        fade = 1.0;
                    float L0, R0, L1, R1;
                    read_sample(s, v->phase, &L0, &R0);
                    double restart = win0 + (v->phase - (win1 - xfade_frames));
                    read_sample(s, restart, &L1, &R1);
                    float a = (float)fade;
                    L = L0 * (1.0f - a) + L1 * a;
                    R = R0 * (1.0f - a) + R1 * a;
                    v->xfade_pos += pitch;
                    v->phase += pitch;
                    if (fade >= 1.0) {
                        v->phase = restart;
                        v->in_crossfade = 0;
                        v->xfade_pos = 0;
                    }
                } else {
                    read_sample(s, v->phase, &L, &R);
                    v->phase += pitch;
                    if (v->phase >= win1)
                        v->phase = win0;
                    if (v->phase < win0 || v->phase >= win1)
                        v->phase = win0 + fmod(v->phase - win0 + (win1 - win0) * 8.0, win1 - win0);
                }
            } else {
                /* One-shot: play through brace, then ADSR Release fades out.
                 * Phase outside after Trim rebase is clamped — only end-of-brace
                 * (natural playthrough) enters Release, not Start/End scrub. */
                if (v->phase >= win1) {
                    if (v->env_stage != ENV_R) {
                        v->gate = 0;
                        v->env_stage = ENV_R;
                    }
                    v->phase = win1 - 1.0;
                } else if (v->phase < win0) {
                    /* Brace moved under playhead — stay audible at new start. */
                    v->phase = win0;
                } else {
                    v->phase += pitch;
                    if (v->phase >= win1) {
                        if (v->env_stage != ENV_R) {
                            v->gate = 0;
                            v->env_stage = ENV_R;
                        }
                        v->phase = win1 - 1.0;
                    }
                }
                read_sample(s, v->phase, &L, &R);
            }

            if (!voice_adsr_tick(v, pad, eng->sample_rate))
                continue;

            /* Filter DSP (OFF = bypass). LFO→POS scrub untouched above. */
            voice_apply_filter(v, pad, eng->sample_rate, &L, &R);

            float vel = v->velocity;
            if (pad->vel_sens < 0.99f)
                vel = pad->vel_sens * v->velocity + (1.0f - pad->vel_sens);
            float ilev = v->impact_level > 0.01f ? v->impact_level : 1.0f;
            float g = pad->mod_level * eng->kit_level * vel * v->env * ilev * edge_mul;
            /* Equal-power-ish pan from Mix page. */
            float pan = clampf(pad->pan, 0.0f, 1.0f);
            float gl = cosf(pan * (float)(0.5 * M_PI));
            float gr = sinf(pan * (float)(0.5 * M_PI));
            float vl = L * g * gl;
            float vr = R * g * gr;
            mixL += vl;
            mixR += vr;
            float sr_amt = clampf(pad->send_rev + bus_rev, 0.0f, 1.5f);
            float sd_amt = clampf(pad->send_del + bus_del, 0.0f, 1.5f);
            if (sr_amt > 0.001f || sd_amt > 0.001f) {
                sndL += vl;
                sndR += vr;
                send_rev_acc += sr_amt;
                send_del_acc += sd_amt;
                send_n++;
            }
        }

        /* Smooth main volume toward target — gain only, no voice restart. */
        {
            float tgt = eng->main_sq_target;
            eng->main_sq += (tgt - eng->main_sq) * main_a;
            if (tgt == 0.0f && eng->main_sq < 1e-6f)
                eng->main_sq = 0.0f;
            eng->master_gain = eng->main_sq * eng->fader_gain;
        }
        float wetL = 0.0f, wetR = 0.0f;
        if (send_n > 0) {
            float inv = 1.0f / (float)send_n;
            float sr = clampf(send_rev_acc * inv, 0.0f, 1.0f);
            float sd = clampf(send_del_acc * inv, 0.0f, 1.0f);
            fx_process(sndL, sndR, sr, sd, &wetL, &wetR, eng->sample_rate);
        } else {
            /* Keep delay line ticking so tails decay when voices stop. */
            fx_process(0.0f, 0.0f, 0.0f, 0.0f, &wetL, &wetR, eng->sample_rate);
        }
        float mg = eng->master_gain;
        mixL = (mixL + wetL * 0.70f) * mg;
        mixR = (mixR + wetR * 0.70f) * mg;
        /* Soft clip to S32 */
        mixL = tanhf(mixL * 0.8f);
        mixR = tanhf(mixR * 0.8f);
        out_l[n] = (int32_t)(mixL * 2147483647.0f * 0.5f);
        out_r[n] = (int32_t)(mixR * 2147483647.0f * 0.5f);
    }

    /* Decay HitMod bus bumps slowly (main copies fresh each tick). */
    eng->fx_bus_rev *= 0.995f;
    eng->fx_bus_del *= 0.995f;

    eng->transport_frames += frames;
}

void engine_host_demo_setup(EpicycleEngine *eng)
{
    if (!eng || !eng->bank)
        return;
    /* Distinct tones so sync vs polymeter is audible on host. */
    static const float freqs[4] = {110.0f, 146.8f, 164.8f, 220.0f};
    for (int i = 0; i < 4; i++) {
        sample_free(&eng->bank[i]);
        sample_synth_demo(&eng->bank[i], eng->sample_rate, freqs[i], 4.0f);
        engine_assign_sample(eng, i, i);
        engine_assign_sample(eng, i + 4, i);
    }
    /* Pads 0,1: shared 4-slice equal grid (quarter chops). */
    engine_set_division(eng, 0, 4);
    engine_set_division(eng, 1, 4);
    /* Pads 2,3: 8 vs 16 slice grids (different chop density). */
    engine_set_division(eng, 2, 8);
    engine_set_division(eng, 3, 16);
    /* Pads 4+: freeform with optional LFO scrub on first scrub-budget pads. */
    for (int i = 4; i < 8; i++) {
        eng->pads[i].freeform = 1;
        eng->pads[i].window_length = 0.35f;
        eng->pads[i].window_start = 0.1f * (float)(i - 4);
        eng->pads[i].lfos[0].enabled = 1;
        eng->pads[i].lfos[0].depth = 0.35f;
        eng->pads[i].lfos[0].rate_hz = 0.15f + 0.05f * (float)(i - 4);
        eng->pads[i].lfos[0].shape = EPICYCLE_LFO_SINE;
        eng->pads[i].allow_scrub = 1;
    }
    /* Pads 8–15: static (scrub disallowed) so budget stays under 8. */
    for (int i = 8; i < EPICYCLE_PAD_COUNT; i++) {
        eng->pads[i].allow_scrub = 0;
        if (eng->bank[i % 4].data)
            engine_assign_sample(eng, i, i % 4);
        eng->pads[i].freeform = 1;
        eng->pads[i].window_length = 0.25f;
        eng->pads[i].level = 0.5f;
    }
    recount_scrub(eng);
}
