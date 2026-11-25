/* SPDX-License-Identifier: MIT
 *
 * Soft 2D tombola: morphing N-gon / circle drum, coloured balls, wall-hit
 * triggers. K1=shape morph, K2=bipolar spin, K3=ball count; MUTE+pad toggles.
 * Empty start — add balls one by one. Freeze/capture is phase 2.
 */
#include "epicycle.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TB_RADIUS 1.0f
#define TB_BALL_R 0.085f
#define TB_HIT_COOLDOWN 0.055f
#define TB_MAX_HITS 16
#define TB_DAMP 0.9992f
#define TB_WALL_RESTITUTION 0.92f
#define TB_SPIN_KICK 0.35f
#define TB_GRAVITY 0.55f
#define TB_SPIN_MAX 7.5f

static EpicycleTombola g_tb;
static float g_cooldown[EPICYCLE_PAD_COUNT];

/* Distinct pad colours (0x00RRGGBB). */
static const uint32_t BALL_RGB[EPICYCLE_PAD_COUNT] = {
    0x00E85A4Fu, 0x00F0A030u, 0x00E8D040u, 0x0068C070u, 0x0040C8A0u, 0x0040A8E0u,
    0x005070E8u, 0x009050E0u, 0x00D050C0u, 0x00E06090u, 0x00C07050u, 0x00A09060u,
    0x0070A080u, 0x006090A0u, 0x00B0B0B0u, 0x00E0E0D0u,
};

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static float len2(float x, float y)
{
    return sqrtf(x * x + y * y);
}

static void place_ball(EpicycleTombolaBall *b, int pad)
{
    float ang = (float)pad * (float)(2.0 * M_PI / EPICYCLE_PAD_COUNT);
    float r = 0.15f + 0.55f * ((float)((pad * 7) % 10) / 10.0f);
    b->x = cosf(ang) * r;
    b->y = sinf(ang) * r * 0.85f;
    b->vx = -sinf(ang) * (0.4f + 0.15f * (float)(pad % 5));
    b->vy = cosf(ang) * (0.35f + 0.12f * (float)(pad % 3));
    b->hit_flash = 0.0f;
}

/* Face count from morph: 0 → circle; else 3..16 (circle → more faces). */
int tombola_face_count(float morph01)
{
    float m = clampf(morph01, 0.0f, 1.0f);
    if (m < 0.04f)
        return 0; /* circle */
    /* Map remaining range to 3..16 faces. */
    int n = 3 + (int)((m - 0.04f) / 0.96f * 13.99f);
    if (n < 3)
        n = 3;
    if (n > 16)
        n = 16;
    return n;
}

uint32_t tombola_ball_color(int pad)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return 0x00FFFFFF;
    return BALL_RGB[pad];
}

void tombola_init(EpicycleTombola *tb)
{
    if (!tb)
        tb = &g_tb;
    memset(tb, 0, sizeof *tb);
    tb->morph = 0.28f; /* start with a few faces visible */
    tb->spin_speed = 0.0f; /* idle until K2; empty drum */
    tb->radius = TB_RADIUS;
    tb->face_count = tombola_face_count(tb->morph);
    tb->ball_target = 0;
    tb->hitmod_edit = 0;
    /* Multi-slot physics matrix defaults (impact → pitch / LPF / level). */
    tb->hitmods[0].src = EPICYCLE_HITMOD_SRC_IMPACT;
    tb->hitmods[0].dest = EPICYCLE_HITMOD_PITCH;
    tb->hitmods[0].enabled = 1;
    tb->hitmods[0].amount = 0.55f;
    tb->hitmods[0].threshold = 0.12f;
    tb->hitmods[0].range = 7.0f;
    tb->hitmods[1].src = EPICYCLE_HITMOD_SRC_IMPACT;
    tb->hitmods[1].dest = EPICYCLE_HITMOD_FILTER;
    tb->hitmods[1].enabled = 1;
    tb->hitmods[1].amount = 0.65f;
    tb->hitmods[1].threshold = 0.08f;
    tb->hitmods[1].range = 0.55f;
    tb->hitmods[2].src = EPICYCLE_HITMOD_SRC_IMPACT;
    tb->hitmods[2].dest = EPICYCLE_HITMOD_LEVEL;
    tb->hitmods[2].enabled = 0;
    tb->hitmods[2].amount = 0.4f;
    tb->hitmods[2].threshold = 0.2f;
    tb->hitmods[2].range = 0.5f;
    tb->hitmods[3].src = EPICYCLE_HITMOD_SRC_HARDNESS;
    tb->hitmods[3].dest = EPICYCLE_HITMOD_SEND_A;
    tb->hitmods[3].enabled = 0;
    tb->hitmods[3].amount = 0.5f;
    tb->hitmods[3].threshold = 0.15f;
    tb->hitmods[3].range = 1.0f;
    for (int i = 4; i < EPICYCLE_HITMOD_SLOTS; i++) {
        tb->hitmods[i].src = EPICYCLE_HITMOD_SRC_IMPACT;
        tb->hitmods[i].dest = EPICYCLE_HITMOD_NONE;
        tb->hitmods[i].enabled = 0;
        tb->hitmods[i].amount = 0.5f;
        tb->hitmods[i].threshold = 0.1f;
        tb->hitmods[i].range = 0.5f;
    }
    /* Empty start — MUTE+pad (or K3) adds balls one by one. */
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        place_ball(&tb->balls[i], i);
        tb->balls[i].active = 0;
        tb->balls[i].vx = 0;
        tb->balls[i].vy = 0;
        g_cooldown[i] = 0.0f;
    }
    if (tb != &g_tb)
        g_tb = *tb;
}

void tombola_eval_hitmod(EpicycleTombola *tb, float impact, float *pitch_mul, float *filt_add,
                         float *level_mul)
{
    float pitch = 1.0f, filt = 0.0f, level = 1.0f;
    if (!tb) {
        if (pitch_mul)
            *pitch_mul = pitch;
        if (filt_add)
            *filt_add = filt;
        if (level_mul)
            *level_mul = level;
        return;
    }
    float imp = clampf(impact, 0.0f, 1.0f);
    /* Angularity: more faces / sharper shape → harder hits → more mod. */
    float ang = 0.0f;
    if (tb->face_count <= 0)
        ang = 0.0f; /* circle = soft */
    else if (tb->face_count <= 4)
        ang = 1.0f; /* tri/square = hardest */
    else
        ang = clampf(1.0f - (float)(tb->face_count - 4) / 12.0f, 0.15f, 1.0f);
    float hard = clampf(imp * (0.65f + 0.55f * ang), 0.0f, 1.0f);
    tb->send_a *= 0.92f; /* decay stub sends each eval */
    tb->send_b *= 0.92f;

    for (int i = 0; i < EPICYCLE_HITMOD_SLOTS; i++) {
        const EpicycleHitModRoute *r = &tb->hitmods[i];
        if (!r->enabled || r->dest == EPICYCLE_HITMOD_NONE)
            continue;
        /* Resolve source value (IMPACT wired; ANGLE/HARDNESS stubbed from shape). */
        float src_v = imp;
        if (r->src == EPICYCLE_HITMOD_SRC_ANGLE)
            src_v = hard;
        else if (r->src == EPICYCLE_HITMOD_SRC_HARDNESS)
            src_v = ang;
        if (src_v < r->threshold)
            continue;
        float t = (src_v - r->threshold) / (1.0f - r->threshold + 1e-5f);
        t = clampf(t, 0.0f, 1.0f) * r->amount * (0.7f + 0.3f * ang);
        t = clampf(t * (0.85f + 0.3f * hard), 0.0f, 1.0f);
        switch (r->dest) {
        case EPICYCLE_HITMOD_PITCH: {
            float semis = (t * 2.0f - 0.35f) * r->range;
            pitch *= powf(2.0f, semis / 12.0f);
            break;
        }
        case EPICYCLE_HITMOD_FILTER:
            filt += t * r->range;
            break;
        case EPICYCLE_HITMOD_LEVEL:
            level *= 1.0f + t * r->range;
            break;
        case EPICYCLE_HITMOD_SEND_A:
            tb->send_a = clampf(tb->send_a + t * r->range, 0.0f, 1.0f);
            break;
        case EPICYCLE_HITMOD_SEND_B:
            tb->send_b = clampf(tb->send_b + t * r->range, 0.0f, 1.0f);
            break;
        default:
            break;
        }
    }
    if (pitch_mul)
        *pitch_mul = clampf(pitch, 0.25f, 4.0f);
    if (filt_add)
        *filt_add = clampf(filt, -0.9f, 0.9f);
    if (level_mul)
        *level_mul = clampf(level, 0.05f, 2.0f);
}

EpicycleTombola *tombola_state(void)
{
    return &g_tb;
}

void tombola_set_morph(EpicycleTombola *tb, float morph01)
{
    if (!tb)
        tb = &g_tb;
    tb->morph = clampf(morph01, 0.0f, 1.0f);
    tb->face_count = tombola_face_count(tb->morph);
}

void tombola_set_spin_cc(EpicycleTombola *tb, uint8_t k2)
{
    if (!tb)
        tb = &g_tb;
    /* Bipolar: 64 ± wide deadband = stop (Ash: spin must not jump alone). */
    int c = (int)k2 - 64;
    if (c > -8 && c < 8) {
        tb->spin_speed = 0.0f;
        return;
    }
    float n = (float)c / 56.0f; /* map outside deadband → ±1 */
    if (n < -1.0f)
        n = -1.0f;
    if (n > 1.0f)
        n = 1.0f;
    tb->spin_speed = n * TB_SPIN_MAX;
}

void tombola_set_ball_count(EpicycleTombola *tb, int count)
{
    if (!tb)
        tb = &g_tb;
    if (count < 0)
        count = 0;
    if (count > EPICYCLE_PAD_COUNT)
        count = EPICYCLE_PAD_COUNT;
    tb->ball_target = count;
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        int want = i < count;
        if (want && !tb->balls[i].active) {
            place_ball(&tb->balls[i], i);
            tb->balls[i].active = 1;
            /* Nudge so newly added balls start moving. */
            float ang = (float)i * (float)(2.0 * M_PI / EPICYCLE_PAD_COUNT);
            tb->balls[i].vx = -sinf(ang) * 0.55f;
            tb->balls[i].vy = cosf(ang) * 0.45f;
        } else if (!want && tb->balls[i].active) {
            tb->balls[i].active = 0;
            tb->balls[i].vx = 0;
            tb->balls[i].vy = 0;
            tb->balls[i].hit_flash = 0;
        }
    }
}

int tombola_active_count(const EpicycleTombola *tb)
{
    if (!tb)
        tb = &g_tb;
    int n = 0;
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++)
        if (tb->balls[i].active)
            n++;
    return n;
}

void tombola_toggle_ball(EpicycleTombola *tb, int pad)
{
    if (!tb)
        tb = &g_tb;
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    tb->balls[pad].active = tb->balls[pad].active ? 0 : 1;
    if (!tb->balls[pad].active) {
        tb->balls[pad].vx = 0;
        tb->balls[pad].vy = 0;
        tb->balls[pad].hit_flash = 0;
    } else {
        float ang = (float)pad * (float)(2.0 * M_PI / EPICYCLE_PAD_COUNT);
        tb->balls[pad].x = cosf(ang) * 0.25f;
        tb->balls[pad].y = sinf(ang) * 0.2f;
        tb->balls[pad].vx = -sinf(ang) * 0.5f;
        tb->balls[pad].vy = cosf(ang) * 0.4f;
    }
    tb->ball_target = tombola_active_count(tb);
}

int tombola_ball_active(const EpicycleTombola *tb, int pad)
{
    if (!tb)
        tb = &g_tb;
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return 0;
    return tb->balls[pad].active ? 1 : 0;
}

static void push_hit(EpicycleTombola *tb, int pad, float vel)
{
    if (tb->hit_n >= TB_MAX_HITS)
        return;
    if (g_cooldown[pad] > 0.0f)
        return;
    tb->hit_pad[tb->hit_n] = pad;
    tb->hit_vel[tb->hit_n] = clampf(vel, 0.08f, 1.0f);
    tb->hit_n++;
    g_cooldown[pad] = TB_HIT_COOLDOWN;
    tb->balls[pad].hit_flash = 1.0f;
}

/*
 * Collision in the drum's local (unrotated) frame.
 * Circle: radial clamp. N-gon: push inside each half-plane, reflect.
 */
static int collide_local(EpicycleTombola *tb, int pad, float *x, float *y, float *vx, float *vy,
                         float spin_speed)
{
    float R = tb->radius - TB_BALL_R;
    if (R < 0.2f)
        R = 0.2f;
    int faces = tb->face_count;
    int hit = 0;
    float impact = 0.0f;

    if (faces <= 0) {
        float d = len2(*x, *y);
        if (d > R && d > 1e-5f) {
            float nx = *x / d;
            float ny = *y / d;
            float vn = (*vx) * nx + (*vy) * ny;
            if (vn > 0.0f) {
                float tx = -ny;
                float ty = nx;
                *vx -= (1.0f + TB_WALL_RESTITUTION) * vn * nx;
                *vy -= (1.0f + TB_WALL_RESTITUTION) * vn * ny;
                *vx += tx * spin_speed * TB_SPIN_KICK * R;
                *vy += ty * spin_speed * TB_SPIN_KICK * R;
                impact = vn + fabsf(spin_speed) * 0.15f;
                hit = 1;
            }
            *x = nx * R;
            *y = ny * R;
        }
    } else {
        for (int e = 0; e < faces; e++) {
            float a0 = (float)e * (float)(2.0 * M_PI / faces) - (float)M_PI / (float)faces;
            float a1 = (float)(e + 1) * (float)(2.0 * M_PI / faces) - (float)M_PI / (float)faces;
            float x0 = cosf(a0) * R, y0 = sinf(a0) * R;
            float x1 = cosf(a1) * R, y1 = sinf(a1) * R;
            float ex = x1 - x0, ey = y1 - y0;
            float el = len2(ex, ey);
            if (el < 1e-6f)
                continue;
            float nx = ey / el;
            float ny = -ex / el;
            float midx = 0.5f * (x0 + x1), midy = 0.5f * (y0 + y1);
            if (midx * nx + midy * ny < 0.0f) {
                nx = -nx;
                ny = -ny;
            }
            float dist = (*x - x0) * nx + (*y - y0) * ny;
            if (dist > 0.0f) {
                float vn = (*vx) * nx + (*vy) * ny;
                if (vn > 0.0f) {
                    float tx = -ny, ty = nx;
                    *vx -= (1.0f + TB_WALL_RESTITUTION) * vn * nx;
                    *vy -= (1.0f + TB_WALL_RESTITUTION) * vn * ny;
                    *vx += tx * spin_speed * TB_SPIN_KICK * R;
                    *vy += ty * spin_speed * TB_SPIN_KICK * R;
                    impact = vn + fabsf(spin_speed) * 0.15f;
                    hit = 1;
                }
                *x -= nx * dist;
                *y -= ny * dist;
            }
        }
        float d = len2(*x, *y);
        if (d > R * 1.05f && d > 1e-5f) {
            *x *= R / d;
            *y *= R / d;
        }
    }

    if (hit)
        push_hit(tb, pad, clampf(impact * 0.55f, 0.12f, 1.0f));
    return hit;
}

void tombola_step(EpicycleTombola *tb, float dt)
{
    if (!tb)
        tb = &g_tb;
    if (dt <= 0.0f)
        return;
    if (dt > 0.05f)
        dt = 0.05f;

    tb->hit_n = 0;
    tb->face_count = tombola_face_count(tb->morph);
    tb->spin_rad += tb->spin_speed * dt;
    if (tb->spin_rad > (float)(2.0 * M_PI) || tb->spin_rad < (float)(-2.0 * M_PI))
        tb->spin_rad = fmodf(tb->spin_rad, (float)(2.0 * M_PI));

    float c = cosf(tb->spin_rad);
    float s = sinf(tb->spin_rad);

    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (g_cooldown[i] > 0.0f) {
            g_cooldown[i] -= dt;
            if (g_cooldown[i] < 0.0f)
                g_cooldown[i] = 0.0f;
        }
        EpicycleTombolaBall *b = &tb->balls[i];
        if (b->hit_flash > 0.0f) {
            b->hit_flash -= dt * 4.0f;
            if (b->hit_flash < 0.0f)
                b->hit_flash = 0.0f;
        }
        if (!b->active)
            continue;

        b->vy -= TB_GRAVITY * dt;
        b->vx *= TB_DAMP;
        b->vy *= TB_DAMP;
        b->x += b->vx * dt;
        b->y += b->vy * dt;

        float lx = b->x * c + b->y * s;
        float ly = -b->x * s + b->y * c;
        float lvx = b->vx * c + b->vy * s;
        float lvy = -b->vx * s + b->vy * c;
        collide_local(tb, i, &lx, &ly, &lvx, &lvy, tb->spin_speed);
        b->x = lx * c - ly * s;
        b->y = lx * s + ly * c;
        b->vx = lvx * c - lvy * s;
        b->vy = lvx * s + lvy * c;

        float spd = len2(b->vx, b->vy);
        if (spd > 4.5f) {
            b->vx *= 4.5f / spd;
            b->vy *= 4.5f / spd;
        }
    }

    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (!tb->balls[i].active)
            continue;
        for (int j = i + 1; j < EPICYCLE_PAD_COUNT; j++) {
            if (!tb->balls[j].active)
                continue;
            float dx = tb->balls[j].x - tb->balls[i].x;
            float dy = tb->balls[j].y - tb->balls[i].y;
            float d = len2(dx, dy);
            float min_d = TB_BALL_R * 2.0f;
            if (d < min_d && d > 1e-5f) {
                float nx = dx / d, ny = dy / d;
                float push = (min_d - d) * 0.5f;
                tb->balls[i].x -= nx * push;
                tb->balls[i].y -= ny * push;
                tb->balls[j].x += nx * push;
                tb->balls[j].y += ny * push;
                float vi = tb->balls[i].vx * nx + tb->balls[i].vy * ny;
                float vj = tb->balls[j].vx * nx + tb->balls[j].vy * ny;
                float share = 0.5f * (vi - vj);
                tb->balls[i].vx -= share * nx;
                tb->balls[i].vy -= share * ny;
                tb->balls[j].vx += share * nx;
                tb->balls[j].vy += share * ny;
            }
        }
    }
}

int tombola_drain_hits(EpicycleTombola *tb, int *pads_out, float *vels_out, int max_n)
{
    if (!tb)
        tb = &g_tb;
    if (!pads_out || !vels_out || max_n <= 0)
        return 0;
    int n = tb->hit_n;
    if (n > max_n)
        n = max_n;
    for (int i = 0; i < n; i++) {
        pads_out[i] = tb->hit_pad[i];
        vels_out[i] = tb->hit_vel[i];
    }
    tb->hit_n = 0;
    return n;
}

void tombola_world_to_screen(const EpicycleTombola *tb, float x, float y, int cx, int cy,
                             int scale, int *sx, int *sy)
{
    (void)tb;
    float depth = 1.0f + y * 0.08f;
    float px = x * depth;
    float py = y * 0.72f * depth;
    *sx = cx + (int)(px * (float)scale);
    *sy = cy - (int)(py * (float)scale);
}

void tombola_edge_point(const EpicycleTombola *tb, int edge, float t, float *ox, float *oy)
{
    if (!tb) {
        *ox = *oy = 0;
        return;
    }
    float R = tb->radius;
    int faces = tb->face_count;
    float ang_spin = tb->spin_rad;
    if (faces <= 0) {
        float a = t * (float)(2.0 * M_PI) + ang_spin;
        *ox = cosf(a) * R;
        *oy = sinf(a) * R;
        return;
    }
    if (edge < 0)
        edge = 0;
    edge %= faces;
    float a0 = (float)edge * (float)(2.0 * M_PI / faces) - (float)M_PI / (float)faces + ang_spin;
    float a1 =
        (float)(edge + 1) * (float)(2.0 * M_PI / faces) - (float)M_PI / (float)faces + ang_spin;
    float x0 = cosf(a0) * R, y0 = sinf(a0) * R;
    float x1 = cosf(a1) * R, y1 = sinf(a1) * R;
    *ox = x0 + (x1 - x0) * t;
    *oy = y0 + (y1 - y0) * t;
}

int tombola_selftest(void)
{
    EpicycleTombola tb;
    tombola_init(&tb);
    /* Empty start. */
    if (tombola_active_count(&tb) != 0)
        return -10;
    tombola_set_ball_count(&tb, 8);
    if (tombola_active_count(&tb) != 8)
        return -11;
    tombola_set_morph(&tb, 0.0f); /* circle */
    tombola_set_spin_cc(&tb, 100); /* CW */
    if (tb.spin_speed <= 0.0f)
        return -12;
    tombola_set_spin_cc(&tb, 20); /* CCW */
    if (tb.spin_speed >= 0.0f)
        return -13;
    tombola_set_spin_cc(&tb, 64); /* stop */
    if (fabsf(tb.spin_speed) > 0.01f)
        return -14;
    tombola_set_spin_cc(&tb, 100);

    int hits_total = 0;
    for (int i = 0; i < 400; i++) {
        tombola_step(&tb, 1.0f / 60.0f);
        hits_total += tb.hit_n;
        for (int b = 0; b < EPICYCLE_PAD_COUNT; b++) {
            if (!tb.balls[b].active)
                continue;
            float d = len2(tb.balls[b].x, tb.balls[b].y);
            if (d > tb.radius + 0.25f)
                return -1; /* escaped */
        }
    }
    if (hits_total < 1)
        return -2; /* should have bounced */

    tombola_set_morph(&tb, 1.0f);
    if (tb.face_count < 3)
        return -3;
    for (int i = 0; i < 120; i++)
        tombola_step(&tb, 1.0f / 60.0f);

    tombola_toggle_ball(&tb, 3);
    if (tombola_ball_active(&tb, 3))
        return -4;
    tombola_toggle_ball(&tb, 3);
    if (!tombola_ball_active(&tb, 3))
        return -5;

    tombola_set_ball_count(&tb, 0);
    if (tombola_active_count(&tb) != 0)
        return -6;

    return 0;
}
