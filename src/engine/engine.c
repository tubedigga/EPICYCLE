/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE loop engine v0:
 *  - Per-pad sample with scrollable loop brace (window start + length)
 *  - Time-locked division (1..32 beats) or free-run length
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
    p->crossfade = 0.05f;
    p->loop_enable = 1;
    p->allow_scrub = 1;
    p->mod_window_start = 0.0f;
    p->mod_window_length = 1.0f;
    p->mod_pitch_ratio = 1.0f;
    p->mod_level = 0.8f;

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

static float division_length_frac(const EpicycleEngine *eng, const EpicyclePad *pad)
{
    if (!pad->sample || pad->sample->frames == 0)
        return pad->window_length;
    float beats = (float)(pad->division_beats < 1 ? 1 : pad->division_beats);
    float seconds = beats * (60.0f / eng->tempo_bpm);
    float frames = seconds * (float)pad->sample->rate;
    float frac = frames / (float)pad->sample->frames;
    return clampf(frac, 1.0f / (float)pad->sample->frames, 1.0f);
}

static void resolve_window(const EpicycleEngine *eng, EpicyclePad *pad, float *start_out,
                           float *len_out)
{
    float len = pad->freeform ? pad->window_length : division_length_frac(eng, pad);
    len = clampf(len, 0.001f, 1.0f);
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

    float d_start = 0, d_len = 0, d_pitch = 0, d_level = 0;

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
            if (scrub_ok)
                d_start += w * 0.5f; /* ±0.5 of sample at full depth */
            break;
        case EPICYCLE_MOD_BRACE_LEN:
            if (scrub_ok)
                d_len += w * 0.25f;
            break;
        case EPICYCLE_MOD_PITCH:
            d_pitch += w * 2.0f; /* ±2 semitones at full */
            break;
        case EPICYCLE_MOD_LEVEL:
            d_level += w * 0.5f;
            break;
        case EPICYCLE_MOD_FILTER:
            /* stub — reserved for later filter cutoff */
            break;
        default:
            break;
        }
    }

    float len = clampf(base_len + d_len, 0.001f, 1.0f);
    float start = clampf(base_start + d_start, 0.0f, 1.0f - len);
    pad->mod_window_start = start;
    pad->mod_window_length = len;
    pad->mod_pitch_ratio = pitch_ratio(pad->octave, pad->semitone, pad->fine + d_pitch * 100.0f);
    pad->mod_level = clampf(pad->level + d_level, 0.0f, 1.0f);
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
    eng->pads[pad].window_length = eng->pads[pad].freeform ? 0.5f : division_length_frac(eng, &eng->pads[pad]);
    return 0;
}

void engine_set_tempo(EpicycleEngine *eng, float bpm)
{
    eng->tempo_bpm = clampf(bpm, 40.0f, 300.0f);
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (!eng->pads[i].freeform)
            eng->pads[i].window_length = division_length_frac(eng, &eng->pads[i]);
    }
}

void engine_set_division(EpicycleEngine *eng, int pad, int beats)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    if (beats <= 0) {
        p->freeform = 1;
        return;
    }
    p->freeform = 0;
    p->division_beats = beats > EPICYCLE_MAX_DIVISION ? EPICYCLE_MAX_DIVISION : beats;
    p->window_length = division_length_frac(eng, p);
    if (p->window_start + p->window_length > 1.0f)
        p->window_start = 1.0f - p->window_length;
}

void engine_set_window(EpicycleEngine *eng, int pad, float start, float length)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    p->window_length = clampf(length, 0.001f, 1.0f);
    p->window_start = clampf(start, 0.0f, 1.0f - p->window_length);
    if (!p->freeform)
        p->window_length = division_length_frac(eng, p);
}

void engine_set_pitch(EpicycleEngine *eng, int pad, int octave, int semi, float fine)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];
    p->octave = octave;
    p->semitone = semi;
    p->fine = fine;
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
    v->active = 1;
    v->pad_index = pad;
    v->phase = (double)p->mod_window_start * (double)p->sample->frames;
    v->velocity = clampf(velocity, 0.05f, 1.0f);
    v->env = 1.0f;
    v->in_crossfade = 0;
    v->xfade_pos = 0;
}

void engine_note_off(EpicycleEngine *eng, int pad)
{
    /* Loops keep running; one-shots already decay. Gate-off reserved. */
    (void)eng;
    (void)pad;
}

void engine_apply_knobs(EpicycleEngine *eng, int pad, uint8_t k1, uint8_t k2, uint8_t k3)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    EpicyclePad *p = &eng->pads[pad];

    /* K1 = window start (scroll brace across sample)
     * K2 = window length (freeform) OR unused when division-locked (length from tempo)
     * K3 = crossfade amount (loops) / level blend
     */
    float start = (float)k1 / 127.0f;
    float length = 0.02f + 0.98f * ((float)k2 / 127.0f);
    if (p->freeform)
        engine_set_window(eng, pad, start, length);
    else {
        /* Division-locked: K1 scrolls the fixed-length window; K2 maps to division 1..32. */
        int beats = 1 + (int)((k2 * (EPICYCLE_MAX_DIVISION - 1)) / 127);
        engine_set_division(eng, pad, beats);
        p->window_start = clampf(start, 0.0f, 1.0f - p->window_length);
    }
    p->crossfade = 0.01f + 0.49f * ((float)k3 / 127.0f);
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

    for (size_t n = 0; n < frames; n++) {
        float mixL = 0, mixR = 0;

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
                v->active = 0;
                continue;
            }

            double xfade_frames =
                (double)pad->crossfade * (win1 - win0);
            if (xfade_frames < 2.0)
                xfade_frames = 2.0;
            if (xfade_frames > (win1 - win0) * 0.45)
                xfade_frames = (win1 - win0) * 0.45;

            float L = 0, R = 0;
            if (pad->loop_enable) {
                /* Keep phase inside window; near end, crossfade to window start. */
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
                    v->xfade_pos += pad->mod_pitch_ratio;
                    v->phase += pad->mod_pitch_ratio;
                    if (fade >= 1.0) {
                        v->phase = restart;
                        v->in_crossfade = 0;
                        v->xfade_pos = 0;
                    }
                } else {
                    read_sample(s, v->phase, &L, &R);
                    v->phase += pad->mod_pitch_ratio;
                    if (v->phase >= win1)
                        v->phase = win0;
                    /* If the window scrolled under us, wrap into new bounds. */
                    if (v->phase < win0 || v->phase >= win1)
                        v->phase = win0 + fmod(v->phase - win0 + (win1 - win0) * 8.0, win1 - win0);
                }
                v->env = 1.0f;
            } else {
                if (v->phase >= win1) {
                    v->active = 0;
                    continue;
                }
                read_sample(s, v->phase, &L, &R);
                v->phase += pad->mod_pitch_ratio;
                /* short release near end */
                double rem = win1 - v->phase;
                if (rem < 64.0)
                    v->env = (float)(rem / 64.0);
            }

            float g = pad->mod_level * v->velocity * v->env;
            mixL += L * g;
            mixR += R * g;
        }

        /* Soft clip to S32 */
        mixL = tanhf(mixL * 0.8f);
        mixR = tanhf(mixR * 0.8f);
        out_l[n] = (int32_t)(mixL * 2147483647.0f * 0.5f);
        out_r[n] = (int32_t)(mixR * 2147483647.0f * 0.5f);
    }

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
    /* Pads 0,1: shared 4-beat division (synced braces, different content). */
    engine_set_division(eng, 0, 4);
    engine_set_division(eng, 1, 4);
    /* Pads 2,3: 3 vs 5 beat polymeter. */
    engine_set_division(eng, 2, 3);
    engine_set_division(eng, 3, 5);
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
