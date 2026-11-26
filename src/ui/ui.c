/* SPDX-License-Identifier: MIT
 *
 * UI: Tombola primary (2D projected drum) + Sample-Mode edit pages + Hit Mod
 * multi-slot matrix + FX editor + live SAMPLE browser + freeze status.
 */
#include "epicycle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float clampf_ui(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

/* Cached splash logo (decoded once). */
static uint32_t *g_logo;
static int g_logo_w;
static int g_logo_h;

static void fill_rect(uint32_t *px, int x, int y, int w, int h, uint32_t c)
{
    for (int row = y; row < y + h && row < EPICYCLE_FB_H; row++) {
        if (row < 0)
            continue;
        for (int col = x; col < x + w && col < EPICYCLE_FB_W; col++) {
            if (col < 0)
                continue;
            px[row * EPICYCLE_FB_W + col] = c;
        }
    }
}

static const char *glyph(char ch)
{
    switch (ch) {
    case 'A': return "01110 10001 11111 10001 10001";
    case 'B': return "11110 10001 11110 10001 11110";
    case 'C': return "01111 10000 10000 10000 01111";
    case 'D': return "11110 10001 10001 10001 11110";
    case 'E': return "11111 10000 11110 10000 11111";
    case 'F': return "11111 10000 11110 10000 10000";
    case 'G': return "01111 10000 10111 10001 01110";
    case 'H': return "10001 10001 11111 10001 10001";
    case 'I': return "11111 00100 00100 00100 11111";
    case 'K': return "10001 10010 11100 10010 10001";
    case 'L': return "10000 10000 10000 10000 11111";
    case 'M': return "10001 11011 10101 10001 10001";
    case 'N': return "10001 11001 10101 10011 10001";
    case 'O': return "01110 10001 10001 10001 01110";
    case 'P': return "11110 10001 11110 10000 10000";
    case 'Q': return "01110 10001 10001 10010 01101";
    case 'R': return "11110 10001 11110 10010 10001";
    case 'S': return "01111 10000 01110 00001 11110";
    case 'T': return "11111 00100 00100 00100 00100";
    case 'U': return "10001 10001 10001 10001 01110";
    case 'V': return "10001 10001 10001 01010 00100";
    case 'W': return "10001 10001 10101 11011 10001";
    case 'X': return "10001 01010 00100 01010 10001";
    case 'Y': return "10001 01010 00100 00100 00100";
    case 'Z': return "11111 00010 00100 01000 11111";
    case 'a': return "00000 01110 00001 01111 01111";
    case 'b': return "10000 10000 11110 10001 11110";
    case 'c': return "00000 01111 10000 10000 01111";
    case 'd': return "00001 00001 01111 10001 01111";
    case 'e': return "00000 01110 11111 10000 01110";
    case 'f': return "00110 01000 11100 01000 01000";
    case 'g': return "00000 01111 10001 01111 00001";
    case 'h': return "10000 10000 11110 10001 10001";
    case 'i': return "00100 00000 01100 00100 01110";
    case 'k': return "10000 10010 11100 10010 10001";
    case 'l': return "01100 00100 00100 00100 01110";
    case 'm': return "00000 11010 10101 10101 10001";
    case 'n': return "00000 11110 10001 10001 10001";
    case 'o': return "00000 01110 10001 10001 01110";
    case 'p': return "00000 11110 10001 11110 10000";
    case 'q': return "00000 01111 10001 01111 00001";
    case 'r': return "00000 10110 11000 10000 10000";
    case 's': return "00000 01111 01110 00001 11110";
    case 't': return "01000 11100 01000 01000 00110";
    case 'u': return "00000 10001 10001 10001 01111";
    case 'v': return "00000 10001 10001 01010 00100";
    case 'w': return "00000 10001 10101 11011 10001";
    case 'x': return "00000 10001 01110 01110 10001";
    case 'y': return "00000 10001 01111 00001 01110";
    case '0': return "01110 10011 10101 11001 01110";
    case '1': return "00100 01100 00100 00100 01110";
    case '2': return "01110 10001 00010 00100 11111";
    case '3': return "11110 00001 01110 00001 11110";
    case '4': return "10010 10010 11111 00010 00010";
    case '5': return "11111 10000 11110 00001 11110";
    case '6': return "01110 10000 11110 10001 01110";
    case '7': return "11111 00001 00010 00100 00100";
    case '8': return "01110 10001 01110 10001 01110";
    case '9': return "01110 10001 01111 00001 01110";
    case ' ': return "00000 00000 00000 00000 00000";
    case '.': return "00000 00000 00000 00000 00100";
    case '-': return "00000 00000 11111 00000 00000";
    case '/': return "00001 00010 00100 01000 10000";
    case ':': return "00000 00100 00000 00100 00000";
    case '#': return "01010 11111 01010 11111 01010";
    case '+': return "00000 00100 01110 00100 00000";
    case '!': return "00100 00100 00100 00000 00100";
    case '?': return "01110 10001 00010 00000 00100";
    case '(': return "00010 00100 00100 00100 00010";
    case ')': return "01000 00100 00100 00100 01000";
    case '~': return "00000 01000 10101 00010 00000";
    case '%': return "11001 11010 00100 01011 10011";
    case '<': return "00010 00100 01000 00100 00010";
    case '>': return "01000 00100 00010 00100 01000";
    default: return "11111 10001 10001 10001 11111";
    }
}

static void draw_char(uint32_t *px, int x, int y, char ch, uint32_t fg, int scale)
{
    const char *g = glyph(ch);
    int row = 0, col = 0;
    for (const char *p = g; *p; p++) {
        if (*p == ' ') {
            row++;
            col = 0;
            continue;
        }
        if (*p == '1')
            fill_rect(px, x + col * scale, y + row * scale, scale, scale, fg);
        col++;
    }
}

static void draw_text(uint32_t *px, int x, int y, const char *s, uint32_t fg, int scale)
{
    for (; *s; s++) {
        draw_char(px, x, y, *s, fg, scale);
        x += 6 * scale;
    }
}

enum {
    /* Firmware chrome — cool slate, high contrast, no toy yellow (Ash 0.3.14). */
    COL_BG = 0x00060A10u,
    COL_BG_TOP = 0x000E1620u,
    COL_FG = 0x00F2F6FAu,
    COL_MUTED = 0x00586878u,
    COL_ACCENT = 0x0030C8D8u,
    COL_OK = 0x0030D080u,
    COL_WAVE = 0x00384858u,
    COL_WAVE_DIM = 0x00141C24u,
    COL_BRACE = 0x0028E0F0u,
    COL_BRACE_FILL = 0x00101C28u,
    COL_MESH = 0x00D0E4ECu, /* bright silver silhouette */
    COL_MESH_DIM = 0x00384850u,
    COL_MESH_CORE = 0x00F0F8FCu,
    COL_MOD = 0x00E85870u,
    COL_PLAY = 0x00F8FCFFu,
    COL_SOFT = 0x008898A0u,
    COL_PAGE = 0x00D8E4ECu,
    COL_RULE = 0x00182830u,
};

int ui_load_splash_logo(const char *path)
{
    if (g_logo) {
        epicycle_png_free(g_logo);
        g_logo = NULL;
        g_logo_w = g_logo_h = 0;
    }
    if (!path || !path[0])
        return -1;
    if (epicycle_png_load_rgb(path, &g_logo, &g_logo_w, &g_logo_h) != 0)
        return -1;
    return 0;
}

static void blit_logo_letterbox(uint32_t *px)
{
    if (!g_logo || g_logo_w <= 0 || g_logo_h <= 0)
        return;
    int x0 = (EPICYCLE_FB_W - g_logo_w) / 2;
    int y0 = (EPICYCLE_FB_H - g_logo_h) / 2;
    for (int row = 0; row < g_logo_h; row++) {
        int dy = y0 + row;
        if (dy < 0 || dy >= EPICYCLE_FB_H)
            continue;
        for (int col = 0; col < g_logo_w; col++) {
            int dx = x0 + col;
            if (dx < 0 || dx >= EPICYCLE_FB_W)
                continue;
            px[dy * EPICYCLE_FB_W + dx] = g_logo[row * g_logo_w + col];
        }
    }
}

void ui_draw_splash(const char *status_line)
{
    uint32_t *px = display_pixels();
    /* Ash boot logo: 320×226 on 320×240 black letterbox. */
    display_clear(0x00000000u);
    if (g_logo) {
        blit_logo_letterbox(px);
    } else {
        draw_text(px, 28, 100, "EPICYCLE", COL_FG, 4);
        draw_text(px, 28, 150, "v" EPICYCLE_VERSION, COL_MUTED, 1);
    }
    if (status_line && status_line[0]) {
        int y = g_logo ? (EPICYCLE_FB_H + g_logo_h) / 2 : EPICYCLE_FB_H - 12;
        if (y > EPICYCLE_FB_H - 5)
            y = EPICYCLE_FB_H - 5;
        draw_text(px, 8, y, status_line, COL_MUTED, 1);
    }
    display_present();
}

void ui_draw_splash_prompt(const char *prompt_line)
{
    uint32_t *px = display_pixels();
    display_clear(0x00000000u);
    if (g_logo) {
        blit_logo_letterbox(px);
    } else {
        draw_text(px, 28, 90, "EPICYCLE", COL_FG, 4);
        draw_text(px, 28, 140, "v" EPICYCLE_VERSION, COL_MUTED, 1);
    }
    /* Centered ALL CAPS under logo (letterbox strip below 226-tall boot.png). */
    const char *msg =
        (prompt_line && prompt_line[0]) ? prompt_line : "PRESS ANY PAD TO CONTINUE";
    int scale = 1;
    int text_w = 0;
    for (const char *p = msg; *p; p++)
        text_w += 6 * scale;
    int x = (EPICYCLE_FB_W - text_w) / 2;
    if (x < 2)
        x = 2;
    int y = g_logo ? (EPICYCLE_FB_H + g_logo_h) / 2 + 2 : EPICYCLE_FB_H - 14;
    if (y > EPICYCLE_FB_H - 10)
        y = EPICYCLE_FB_H - 10;
    fill_rect(px, 0, y - 2, EPICYCLE_FB_W, 12, 0x00000000u);
    draw_text(px, x, y, msg, COL_SOFT, scale);
    display_present();
}

static void draw_waveform(uint32_t *px, const EpicyclePad *pad, float playhead, int x, int y,
                          int w, int h)
{
    fill_rect(px, x, y, w, h, 0x00181010u);
    fill_rect(px, x, y + h / 2, w, 1, 0x00282020u);

    if (!pad || !pad->sample || !pad->sample->data || pad->sample->frames == 0) {
        draw_text(px, x + 12, y + h / 2 - 8, "no sample", COL_MUTED, 2);
        return;
    }

    const EpicycleSample *s = pad->sample;
    float w0 = pad->mod_window_start;
    float w1 = pad->mod_window_start + pad->mod_window_length;
    if (w1 > 1.0f)
        w1 = 1.0f;

    int bx0 = x + (int)(w0 * (float)w);
    int bx1 = x + (int)(w1 * (float)w);
    if (bx1 < bx0 + 2)
        bx1 = bx0 + 2;
    fill_rect(px, bx0, y, bx1 - bx0, h, COL_BRACE_FILL);

    for (int col = 0; col < w; col++) {
        size_t i0 = (size_t)col * s->frames / (size_t)w;
        size_t i1 = (size_t)(col + 1) * s->frames / (size_t)w;
        if (i1 <= i0)
            i1 = i0 + 1;
        if (i1 > s->frames)
            i1 = s->frames;
        float peak = 0;
        for (size_t i = i0; i < i1; i++) {
            float a = s->data[i * 2];
            if (a < 0)
                a = -a;
            if (a > peak)
                peak = a;
        }
        int amp = (int)(peak * (float)(h / 2 - 2));
        if (amp < 1)
            amp = 1;
        float t = (float)col / (float)w;
        uint32_t c = (t >= w0 && t <= w1) ? COL_WAVE : COL_WAVE_DIM;
        fill_rect(px, x + col, y + h / 2 - amp, 1, amp * 2, c);
    }

    /* Equal-slice grid lines when in DIV mode (Ash discoverability). */
    if (!pad->freeform) {
        int n = pad->division_beats < 1 ? 1 : pad->division_beats;
        if (n > EPICYCLE_MAX_DIVISION)
            n = EPICYCLE_MAX_DIVISION;
        /* Cap drawn lines so DIV32 stays readable on 320px. */
        int draw_n = n;
        if (draw_n > 32)
            draw_n = 32;
        for (int i = 1; i < draw_n; i++) {
            int gx = x + (int)(((float)i / (float)n) * (float)w);
            fill_rect(px, gx, y, 1, h, 0x00384838u);
        }
    }

    int ux = x + (int)(pad->window_start * (float)w);
    int uw = (int)(pad->window_length * (float)w);
    if (uw < 3)
        uw = 3;
    fill_rect(px, ux, y, 3, h, COL_BRACE);
    fill_rect(px, ux + uw - 3, y, 3, h, COL_BRACE);
    fill_rect(px, ux, y, uw, 3, COL_BRACE);
    fill_rect(px, ux, y + h - 3, uw, 3, COL_BRACE);
    fill_rect(px, bx0, y + h - 5, bx1 - bx0, 5, COL_MOD);

    if (playhead >= 0.0f) {
        int pxh = x + (int)(playhead * (float)w);
        if (pxh < x)
            pxh = x;
        if (pxh >= x + w)
            pxh = x + w - 1;
        fill_rect(px, pxh, y, 2, h, COL_PLAY);
    }
}

/* Equidistant bottom param columns (320px / 3). */
enum { UI_COL0 = 8, UI_COL1 = 112, UI_COL2 = 216 };

static void draw_param_row(uint32_t *px, int y, const char *k1n, const char *k1v, const char *k2n,
                           const char *k2v, const char *k3n, const char *k3v)
{
    draw_text(px, UI_COL0, y, k1n, COL_MUTED, 2);
    draw_text(px, UI_COL1, y, k2n, COL_MUTED, 2);
    draw_text(px, UI_COL2, y, k3n, COL_MUTED, 2);
    draw_text(px, UI_COL0, y + 16, k1v, COL_FG, 2);
    draw_text(px, UI_COL1, y + 16, k2v, COL_FG, 2);
    draw_text(px, UI_COL2, y + 16, k3v, COL_FG, 2);
}

static const char *lfo_shape_name(EpicycleLfoShape s)
{
    switch (s) {
    case EPICYCLE_LFO_TRI:
        return "TRI";
    case EPICYCLE_LFO_SAW:
        return "SAW";
    case EPICYCLE_LFO_SQUARE:
        return "SQR";
    case EPICYCLE_LFO_SINE:
    default:
        return "SIN";
    }
}

static void draw_soft_labels(uint32_t *px)
{
    draw_text(px, 8, 4, "< PAGE", COL_SOFT, 2);
    draw_text(px, 118, 4, "PAGE >", COL_SOFT, 2);
    draw_text(px, 240, 4, "QUIT", COL_MUTED, 2);
}

static void put_px(uint32_t *px, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= EPICYCLE_FB_W || y >= EPICYCLE_FB_H)
        return;
    px[y * EPICYCLE_FB_W + x] = c;
}

static uint32_t blend_rgb(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.0f)
        return a;
    if (t >= 1.0f)
        return b;
    int ar = (int)((a >> 16) & 0xff), ag = (int)((a >> 8) & 0xff), ab = (int)(a & 0xff);
    int br = (int)((b >> 16) & 0xff), bg = (int)((b >> 8) & 0xff), bb = (int)(b & 0xff);
    int r = ar + (int)((float)(br - ar) * t);
    int g = ag + (int)((float)(bg - ag) * t);
    int bl = ab + (int)((float)(bb - ab) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* Soft put — blends toward colour for anti-aliased edges. */
static void put_px_soft(uint32_t *px, int x, int y, uint32_t c, float a)
{
    if (x < 0 || y < 0 || x >= EPICYCLE_FB_W || y >= EPICYCLE_FB_H || a <= 0.0f)
        return;
    if (a >= 0.95f) {
        put_px(px, x, y, c);
        return;
    }
    uint32_t *dst = &px[y * EPICYCLE_FB_W + x];
    *dst = blend_rgb(*dst, c, a);
}

static uint32_t shade_rgb(uint32_t c, float lit)
{
    int r = (int)(((c >> 16) & 0xff) * lit);
    int g = (int)(((c >> 8) & 0xff) * lit);
    int b = (int)((c & 0xff) * lit);
    if (r > 255)
        r = 255;
    if (g > 255)
        g = 255;
    if (b > 255)
        b = 255;
    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Firm shaded sphere — NW key light, cool fill, tight specular, soft AA rim. */
static void fill_sphere(uint32_t *px, int cx, int cy, int r, uint32_t base)
{
    if (r < 1)
        r = 1;
    float rf = (float)r;
    float inv_r = 1.0f / rf;
    int r_outer = r + 2;
    for (int y = -r_outer; y <= r_outer; y++) {
        for (int x = -r_outer; x <= r_outer; x++) {
            float fx = (float)x + 0.5f;
            float fy = (float)y + 0.5f;
            float d = sqrtf(fx * fx + fy * fy);
            if (d > rf + 1.25f)
                continue;
            float edge_a = 1.0f;
            if (d > rf - 0.85f)
                edge_a = clampf_ui((rf + 0.85f - d) / 1.7f);
            if (edge_a <= 0.0f)
                continue;
            float nx = fx * inv_r;
            float ny = fy * inv_r;
            float nz2 = 1.0f - nx * nx - ny * ny;
            float nz = nz2 > 0.0f ? sqrtf(nz2) : 0.0f;
            /* Key (NW) + cool rim fill — Sample-firmware ball, not flat pixel. */
            float ndot = 0.55f * (-nx) + 0.38f * (-ny) + 0.74f * nz;
            float lit = 0.16f + 0.72f * clampf_ui(ndot) + 0.14f * clampf_ui(nz);
            /* Specular lobe. */
            float hx = -nx * 0.35f + 0.28f;
            float hy = -ny * 0.35f + 0.42f;
            float hz = nz + 0.55f;
            float hn = sqrtf(hx * hx + hy * hy + hz * hz);
            if (hn > 1e-4f) {
                hx /= hn;
                hy /= hn;
                hz /= hn;
            }
            float spec = hx * 0.32f + hy * 0.38f + hz * 0.86f;
            if (spec > 0.0f) {
                float s2 = spec * spec;
                lit += s2 * s2 * s2 * 0.72f;
            }
            /* Slight contact shadow under ball. */
            lit *= 0.92f + 0.08f * clampf_ui(0.55f - ny);
            if (lit < 0.08f)
                lit = 0.08f;
            if (lit > 1.55f)
                lit = 1.55f;
            uint32_t col = shade_rgb(base, lit);
            if (edge_a >= 0.92f)
                put_px(px, cx + x, cy + y, col);
            else
                put_px_soft(px, cx + x, cy + y, col, edge_a);
        }
    }
}

/* Soft rim ring around a sphere. */
static void stroke_disk(uint32_t *px, int cx, int cy, int r, uint32_t c)
{
    if (r < 2)
        return;
    float rf = (float)r;
    float half = 1.35f;
    for (int y = -r - 2; y <= r + 2; y++) {
        for (int x = -r - 2; x <= r + 2; x++) {
            float d = sqrtf((float)(x * x + y * y));
            float a = 1.0f - fabsf(d - rf) / half;
            if (a <= 0.0f)
                continue;
            if (a > 1.0f)
                a = 1.0f;
            put_px_soft(px, cx + x, cy + y, c, a * 0.90f);
        }
    }
}

/* Thick anti-aliased stroke (distance-to-segment) — intentional firmware line. */
static void draw_line_aa(uint32_t *px, int x0, int y0, int x1, int y1, uint32_t c, float half_w)
{
    float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1;
    float dx = fx1 - fx0, dy = fy1 - fy0;
    float len2 = dx * dx + dy * dy;
    float len = sqrtf(len2);
    if (len < 0.5f) {
        put_px_soft(px, x0, y0, c, 1.0f);
        return;
    }
    float inv_len = 1.0f / len;
    float px_n = -dy * inv_len, py_n = dx * inv_len;
    int pad = (int)(half_w + 2.0f);
    int minx = (x0 < x1 ? x0 : x1) - pad;
    int maxx = (x0 > x1 ? x0 : x1) + pad;
    int miny = (y0 < y1 ? y0 : y1) - pad;
    int maxy = (y0 > y1 ? y0 : y1) + pad;
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx >= EPICYCLE_FB_W)
        maxx = EPICYCLE_FB_W - 1;
    if (maxy >= EPICYCLE_FB_H)
        maxy = EPICYCLE_FB_H - 1;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float qx = (float)x + 0.5f - fx0;
            float qy = (float)y + 0.5f - fy0;
            float t = (qx * dx + qy * dy) * (len2 > 1e-6f ? 1.0f / len2 : 0.0f);
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            float cx = fx0 + dx * t;
            float cy = fy0 + dy * t;
            float ddx = (float)x + 0.5f - cx;
            float ddy = (float)y + 0.5f - cy;
            float dist = sqrtf(ddx * ddx + ddy * ddy);
            /* Slight round caps via endpoint distance already covered by clamp t. */
            float a = 1.0f - (dist - (half_w - 0.55f));
            if (a <= 0.0f)
                continue;
            if (a > 1.0f)
                a = 1.0f;
            /* Mild bias along normal for cleaner silhouette. */
            float along = fabsf(qx * px_n + qy * py_n);
            (void)along;
            put_px_soft(px, x, y, c, a);
        }
    }
}

static void draw_tombola(uint32_t *px, const EpicycleTombola *tb, int sel)
{
    /* Drum under title with margin; morph feel kept (Ash). */
    int cx = 160, cy = 116, scale = 64;
    if (!tb)
        return;

    /* Layered elliptical floor — vignette depth. */
    for (int y = -(int)(scale * 0.62f); y <= (int)(scale * 0.68f); y++) {
        float ty = (float)y / (float)scale;
        float fall = 1.0f - ty * ty * 0.88f;
        if (fall < 0.04f)
            continue;
        int half = (int)(fall * (float)scale * 1.02f);
        if (half < 2)
            continue;
        float shade = 0.35f + 0.55f * fall;
        uint32_t gcol = shade_rgb(0x000C141Cu, shade);
        fill_rect(px, cx - half, cy + y, half * 2, 1, gcol);
    }

    /* Multi-pass AA silhouette — dim halo → body → bright core (no spokes). */
    {
        int faces = tb->face_count;
        if (faces <= 0) {
            int prevx = 0, prevy = 0;
            const int segs = 160;
            for (int i = 0; i <= segs; i++) {
                float a = (float)i / (float)segs * (float)(2.0 * M_PI) + tb->spin_rad;
                float ox = cosf(a) * tb->radius;
                float oy = sinf(a) * tb->radius;
                int sx, sy;
                tombola_world_to_screen(tb, ox, oy, cx, cy, scale, &sx, &sy);
                if (i > 0) {
                    draw_line_aa(px, prevx, prevy, sx, sy, COL_MESH_DIM, 2.85f);
                    draw_line_aa(px, prevx, prevy, sx, sy, COL_MESH, 1.55f);
                    draw_line_aa(px, prevx, prevy, sx, sy, COL_MESH_CORE, 0.70f);
                }
                prevx = sx;
                prevy = sy;
            }
            /* Inner rim — firmware depth cue. */
            prevx = prevy = 0;
            for (int i = 0; i <= segs; i++) {
                float a = (float)i / (float)segs * (float)(2.0 * M_PI) + tb->spin_rad;
                float ox = cosf(a) * tb->radius * 0.86f;
                float oy = sinf(a) * tb->radius * 0.86f;
                int sx, sy;
                tombola_world_to_screen(tb, ox, oy, cx, cy, scale, &sx, &sy);
                if (i > 0)
                    draw_line_aa(px, prevx, prevy, sx, sy, shade_rgb(COL_MESH, 0.35f), 0.85f);
                prevx = sx;
                prevy = sy;
            }
        } else {
            for (int e = 0; e < faces; e++) {
                float x0, y0, x1, y1;
                tombola_edge_point(tb, e, 0.0f, &x0, &y0);
                tombola_edge_point(tb, e, 1.0f, &x1, &y1);
                int sx0, sy0, sx1, sy1;
                tombola_world_to_screen(tb, x0, y0, cx, cy, scale, &sx0, &sy0);
                tombola_world_to_screen(tb, x1, y1, cx, cy, scale, &sx1, &sy1);
                draw_line_aa(px, sx0, sy0, sx1, sy1, COL_MESH_DIM, 2.90f);
                draw_line_aa(px, sx0, sy0, sx1, sy1, COL_MESH, 1.60f);
                draw_line_aa(px, sx0, sy0, sx1, sy1, COL_MESH_CORE, 0.72f);
            }
        }
    }

    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        int sx, sy;
        uint32_t col = tombola_ball_color(i);
        if (!tb->balls[i].active) {
            float ang = (float)i * (float)(2.0 * M_PI / EPICYCLE_PAD_COUNT);
            float ox = cosf(ang) * 0.70f;
            float oy = sinf(ang) * 0.70f;
            tombola_world_to_screen(tb, ox, oy, cx, cy, scale, &sx, &sy);
            stroke_disk(px, sx, sy, 3, shade_rgb(col, 0.22f));
            continue;
        }
        tombola_world_to_screen(tb, tb->balls[i].x, tb->balls[i].y, cx, cy, scale, &sx, &sy);
        /* Soft contact shadow. */
        fill_rect(px, sx - 4, sy + 4, 8, 2, 0x00101820u);
        if (tb->balls[i].hit_flash > 0.15f)
            col = shade_rgb(col, 1.42f);
        int r = (i == sel) ? 8 : 6;
        fill_sphere(px, sx, sy, r, col);
        stroke_disk(px, sx - 1, sy - 1, r - 2, shade_rgb(0x00FFFFFF, 0.22f));
        if (i == sel)
            stroke_disk(px, sx, sy, r + 2, COL_ACCENT);
    }
}

void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf, const EpicycleTombola *tb,
                  const EpicycleSampleBrowser *browser)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);
    (void)hw;

    int sel = eng ? eng->selected_pad : 0;
    const EpicyclePad *pad =
        (eng && sel >= 0 && sel < EPICYCLE_PAD_COUNT) ? &eng->pads[sel] : NULL;
    EpicycleModePage page = eng ? eng->mode_page : EPICYCLE_PAGE_TOMBOLA;

    /* Top chrome band + rule — clearer hierarchy vs flat toy UI. */
    for (int y = 0; y < 26; y++) {
        float t = (float)y / 26.0f;
        uint32_t c = shade_rgb(COL_BG_TOP, 0.75f + 0.25f * (1.0f - t));
        fill_rect(px, 0, y, EPICYCLE_FB_W, 1, c);
    }
    fill_rect(px, 0, 26, EPICYCLE_FB_W, 1, COL_RULE);
    draw_soft_labels(px);

    char line[48];
    char v1[24], v2[24], v3[24];

    /* Header: pad + page name + tempo. */
    snprintf(line, sizeof line, "PAD %d  %s", sel + 1, epicycle_page_name(page));
    draw_text(px, 8, 30, line, COL_PAGE, 2);

    if (eng)
        snprintf(line, sizeof line, "%3.0f", (double)eng->tempo_bpm);
    else
        snprintf(line, sizeof line, "---");
    draw_text(px, 248, 30, line, COL_MUTED, 2);
    draw_text(px, 290, 34, "bpm", COL_MUTED, 1);

    if (surf && surf->sample_held)
        draw_text(px, 200, 30, "SEL", COL_BRACE, 2);
    if (surf && surf->mute_held)
        draw_text(px, 168, 30, "MUTE", COL_BRACE, 2);
    if (surf && surf->shift_held)
        draw_text(px, 140, 30, "SHIFT", COL_BRACE, 2);
    if (eng && engine_freeze_active(eng))
        draw_text(px, 250, 4, "FRZ", COL_MOD, 1);
    if (pad && pad->loop_enable)
        draw_text(px, 290, 4, "LOOP", COL_MOD, 1);
    else
        draw_text(px, 290, 4, "1SHOT", COL_MUTED, 1);

    if (page == EPICYCLE_PAGE_TOMBOLA) {
        draw_tombola(px, tb, sel);
        /* Labels MUST match knobs (Ash): K1=spin, K2 unused, K3=balls status.
         * Shape morph is fader-only — never label a spin knob "SHAPE". */
        {
            float sp = tb ? tb->spin_speed : 0.0f;
            if (sp > -0.05f && sp < 0.05f)
                snprintf(v1, sizeof v1, "STOP");
            else
                snprintf(v1, sizeof v1, "%+.1f", (double)sp);
        }
        v2[0] = '\0';
        int active = tb ? tombola_active_count(tb) : 0;
        snprintf(v3, sizeof v3, "%d", active);
        draw_param_row(px, 188, "SPIN", v1, "", v2, "BALLS", v3);
        {
            int faces = tb ? tb->face_count : 0;
            if (faces <= 0)
                snprintf(line, sizeof line, "FADER CIRC  MAIN  K1 spin  ENC=freeze");
            else
                snprintf(line, sizeof line, "FADER %dF  MAIN  K1 spin  ENC=freeze", faces);
            draw_text(px, 8, 226, line, COL_MUTED, 1);
        }
    } else if (page == EPICYCLE_PAGE_SAMPLE) {
        /* Live assign: any WAV by filename — no pad01.wav rename required. */
        draw_text(px, 8, 48, "SAMPLE", COL_FG, 2);
        draw_text(px, 120, 52, "any wav -> pad", COL_MUTED, 1);
        if (pad && pad->sample && pad->sample->name[0]) {
            snprintf(line, sizeof line, "now: %.36s", pad->sample->name);
            draw_text(px, 8, 68, line, COL_MOD, 1);
        } else {
            draw_text(px, 8, 68, "now: (empty)", COL_MUTED, 1);
        }
        int count = browser ? browser->count : 0;
        int cur = browser ? browser->cursor : 0;
        if (count <= 0) {
            draw_text(px, 8, 100, "drop WAVs in", COL_FG, 2);
            draw_text(px, 8, 120, "Epicycle/samples/", COL_PAGE, 2);
            draw_text(px, 8, 150, "any name — then ENC rescan", COL_MUTED, 1);
        } else {
            int visible = 7;
            int top = cur - visible / 2;
            if (top < 0)
                top = 0;
            if (top > count - visible)
                top = count - visible;
            if (top < 0)
                top = 0;
            int row_y = 86;
            for (int i = 0; i < visible; i++) {
                int idx = top + i;
                if (idx >= count)
                    break;
                uint32_t fg = (idx == cur) ? COL_FG : COL_MUTED;
                if (idx == cur)
                    fill_rect(px, 4, row_y - 1, 312, 14, 0x00182024u);
                snprintf(line, sizeof line, "%s", browser->names[idx]);
                draw_text(px, 12, row_y, line, fg, 1);
                row_y += 14;
            }
        }
        if (browser && browser->status[0]) {
            /* Soft-fail messages stay visible; successes stay green. */
            uint32_t sc = COL_OK;
            if (strstr(browser->status, "fail") || strstr(browser->status, "bad") ||
                strstr(browser->status, "too big") || strstr(browser->status, "nothing"))
                sc = COL_MUTED;
            draw_text(px, 8, 200, browser->status, sc, 1);
        }
        draw_text(px, 8, 214, "pad=preview  ENC=assign  SHIFT+ENC=norm", COL_MUTED, 1);
        draw_text(px, 8, 226, "SAMPLE+pad=target  SAMPLE+ENC=rescan", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_HITMOD) {
        draw_text(px, 8, 48, "HIT MOD", COL_FG, 2);
        draw_text(px, 120, 52, "src->dest matrix", COL_MUTED, 1);
        int slot = tb ? tb->hitmod_edit : 0;
        if (slot < 0)
            slot = 0;
        if (slot >= EPICYCLE_HITMOD_SLOTS)
            slot = EPICYCLE_HITMOD_SLOTS - 1;
        /* Scroll window so ~6 rows fit; selected stays visible. */
        int top = slot - EPICYCLE_HITMOD_VISIBLE / 2;
        if (top < 0)
            top = 0;
        if (top > EPICYCLE_HITMOD_SLOTS - EPICYCLE_HITMOD_VISIBLE)
            top = EPICYCLE_HITMOD_SLOTS - EPICYCLE_HITMOD_VISIBLE;
        if (top < 0)
            top = 0;
        int row_y = 68;
        draw_text(px, 8, row_y - 12, "#  SRC  DEST  AMT", COL_MUTED, 1);
        for (int i = 0; i < EPICYCLE_HITMOD_VISIBLE; i++) {
            int s = top + i;
            if (s >= EPICYCLE_HITMOD_SLOTS)
                break;
            const EpicycleHitModRoute *rr = tb ? &tb->hitmods[s] : NULL;
            uint32_t fg = (s == slot) ? COL_BRACE : COL_FG;
            uint32_t dim = (s == slot) ? COL_PAGE : COL_MUTED;
            if (s == slot)
                fill_rect(px, 4, row_y - 1, 312, 14, 0x00201810u);
            snprintf(line, sizeof line, "%d", s + 1);
            draw_text(px, 8, row_y, line, dim, 1);
            snprintf(line, sizeof line, "%s",
                     rr ? epicycle_hitmod_src_name(rr->src) : "----");
            draw_text(px, 28, row_y, line, fg, 1);
            snprintf(line, sizeof line, "%s",
                     rr ? epicycle_hitmod_dest_name(rr->dest) : "OFF");
            draw_text(px, 80, row_y, line, fg, 1);
            snprintf(line, sizeof line, "%3d%%", rr ? (int)(rr->amount * 100.0f) : 0);
            draw_text(px, 140, row_y, line, fg, 1);
            snprintf(line, sizeof line, "%s", rr && rr->enabled ? "ON" : "--");
            draw_text(px, 200, row_y, line, rr && rr->enabled ? COL_OK : COL_MUTED, 1);
            if (rr && (rr->dest == EPICYCLE_HITMOD_SEND_A || rr->dest == EPICYCLE_HITMOD_SEND_B))
                draw_text(px, 236, row_y, "stub", COL_MUTED, 1);
            row_y += 16;
        }
        const EpicycleHitModRoute *r = tb ? &tb->hitmods[slot] : NULL;
        snprintf(v1, sizeof v1, "%s", r ? epicycle_hitmod_src_name(r->src) : "IMPA");
        snprintf(v2, sizeof v2, "%s", r ? epicycle_hitmod_dest_name(r->dest) : "OFF");
        snprintf(v3, sizeof v3, "%d", r ? (int)(r->amount * 100.0f) : 0);
        draw_param_row(px, 178, "SRC", v1, "DEST", v2, "AMT", v3);
        draw_text(px, 8, 214, "ENC row  push=tog  K1 src K2 dest K3 amt", COL_MUTED, 1);
        draw_text(px, 8, 226, "SHIFT+K3 thr  VOL/PITCH/CUT/SNDA/SNDB", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_TRIM) {
        float playhead = eng ? engine_pad_playhead(eng, sel) : -1.0f;
        draw_waveform(px, pad, playhead, 4, 52, 312, 110);

        float start = pad ? pad->window_start : 0.0f;
        float len = pad ? pad->window_length : 0.0f;
        float end = start + len;
        if (end > 1.0f)
            end = 1.0f;
        /* POS = brace window offset (not playhead; playhead is the white line). */
        float pos = start;

        int shift = surf && surf->shift_held;
        if (shift) {
            /* Shift overlay: crossfade + loop direction (Forward / Ping-pong). */
            snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->crossfade * 100.0f) : 0);
            snprintf(v2, sizeof v2, "%s", (pad && pad->ping_pong) ? "PINGPONG" : "FORWARD");
            snprintf(v3, sizeof v3, "%.2f", (double)pos);
            draw_text(px, UI_COL0, 168, "XFADE", COL_BRACE, 2);
            draw_text(px, UI_COL1, 168, "DIR", COL_BRACE, 2);
            draw_text(px, UI_COL2, 168, "POS", COL_MUTED, 2);
            draw_text(px, UI_COL0, 184, v1, COL_FG, 2);
            draw_text(px, UI_COL1, 184, v2, COL_FG, 2);
            draw_text(px, UI_COL2, 184, v3, COL_MUTED, 2);
            draw_text(px, 8, 212, "SHIFT+K1 xfade  SHIFT+K2 dir", COL_MUTED, 1);
            draw_text(px, 8, 226, "release SHIFT for Start/End/Pos", COL_MUTED, 1);
        } else {
            draw_text(px, UI_COL0, 168, "START", COL_MUTED, 2);
            draw_text(px, UI_COL1, 168, "END", COL_MUTED, 2);
            draw_text(px, UI_COL2, 168, "POS", COL_MUTED, 2);
            /* Show more digits when brace is tiny so Ash sees grain close. */
            if (len < 0.05f) {
                snprintf(line, sizeof line, "%.3f", (double)start);
                draw_text(px, UI_COL0, 184, line, COL_FG, 2);
                snprintf(line, sizeof line, "%.3f", (double)end);
                draw_text(px, UI_COL1, 184, line, COL_FG, 2);
            } else {
                snprintf(line, sizeof line, "%.2f", (double)start);
                draw_text(px, UI_COL0, 184, line, COL_FG, 3);
                snprintf(line, sizeof line, "%.2f", (double)end);
                draw_text(px, UI_COL1, 184, line, COL_FG, 3);
            }
            snprintf(line, sizeof line, "%.2f", (double)pos);
            draw_text(px, UI_COL2, 184, line, COL_BRACE, 3);
            /* Large FREE / DIVn badge — encoder push toggles (Ash discoverability). */
            if (pad && pad->freeform) {
                draw_text(px, 214, 52, "FREE", COL_BRACE, 3);
                if (len < 0.01f) {
                    draw_text(px, 214, 78, "GRN", COL_MOD, 2);
                    snprintf(line, sizeof line, "w=%.4f", (double)len);
                    draw_text(px, 248, 82, line, COL_MUTED, 1);
                }
                draw_text(px, 8, 212, "K1/K2 absolute Start/End  K3 POS", COL_MUTED, 1);
                draw_text(px, 8, 226, "frame-quantized  ENC fine  SEQ=home", COL_MUTED, 1);
            } else {
                snprintf(line, sizeof line, "DIV%d",
                         pad ? engine_division_quantize_slices(pad->division_beats) : 4);
                draw_text(px, 200, 52, line, COL_MOD, 3);
                draw_text(px, 8, 212, "K2=N slices  K1/K3 pick slice", COL_MUTED, 1);
                draw_text(px, 8, 226, "ENC wheel=N  push=FREE  B1/B2 page", COL_MUTED, 1);
            }
        }
    } else if (page == EPICYCLE_PAGE_MIX) {
        draw_text(px, 8, 56, "MIX", COL_FG, 4);
        snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->level * 100.0f) : 0);
        if (pad) {
            int pan = (int)((pad->pan - 0.5f) * 100.0f);
            if (pan < 0)
                snprintf(v2, sizeof v2, "%dL", -pan);
            else if (pan > 0)
                snprintf(v2, sizeof v2, "%dR", pan);
            else
                snprintf(v2, sizeof v2, "C");
        } else {
            snprintf(v2, sizeof v2, "C");
        }
        if (surf && surf->shift_held)
            snprintf(v3, sizeof v3, "%d", eng ? (int)(eng->kit_level * 100.0f) : 0);
        else
            snprintf(v3, sizeof v3, "%d", pad ? (int)(pad->send_rev * 100.0f) : 0);
        draw_param_row(px, 100, "VOL", v1, "PAN", v2, surf && surf->shift_held ? "KIT" : "FX",
                       v3);
        if (surf) {
            snprintf(line, sizeof line, "main %d  fader morph-only", (int)surf->main_vol);
            draw_text(px, 8, 160, line, COL_MUTED, 1);
        }
        draw_text(px, 8, 218, "K1 vol  K2 pan  K3 FX send  SHIFT+K3 kit", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_DELAY) {
        draw_text(px, 8, 52, "DELAY", COL_FG, 4);
        int shift = surf && surf->shift_held;
        if (shift) {
            snprintf(v1, sizeof v1, "%d", eng ? (int)(eng->fx_delay_tone * 100.0f) : 0);
            snprintf(v2, sizeof v2, "%d", eng ? (int)(eng->fx_wet * 100.0f) : 0);
            snprintf(v3, sizeof v3, "%.1fs", eng ? (double)eng->freeze_seconds : 3.0);
            draw_param_row(px, 100, "TONE", v1, "MWET", v2, "FRZ", v3);
            draw_text(px, 8, 160, "SHIFT: feedback tone / master wet / freeze", COL_MUTED, 1);
            draw_text(px, 8, 218, "K1 tone  K2 master wet  K3 freeze 2-5s", COL_MUTED, 1);
        } else {
            int dms = eng ? (40 + (int)(eng->fx_delay_time * 710.0f)) : 340;
            snprintf(v1, sizeof v1, "%dms", dms);
            snprintf(v2, sizeof v2, "%d", eng ? (int)(eng->fx_delay_fb * 100.0f) : 0);
            snprintf(v3, sizeof v3, "%d", eng ? (int)(eng->fx_delay_wet * 100.0f) : 0);
            draw_param_row(px, 100, "TIME", v1, "FB", v2, "WET", v3);
            draw_text(px, 8, 160, "stereo delay — Mix K3 is pad send", COL_MUTED, 1);
            draw_text(px, 8, 218, "K1 time  K2 feedback  K3 delay wet", COL_MUTED, 1);
        }
    } else if (page == EPICYCLE_PAGE_REVERB) {
        draw_text(px, 8, 52, "REVERB", COL_FG, 3);
        int shift = surf && surf->shift_held;
        if (shift) {
            snprintf(v1, sizeof v1, "%d", eng ? (int)(eng->fx_rev_wet * 100.0f) : 0);
            snprintf(v2, sizeof v2, "%d", eng ? (int)(eng->fx_wet * 100.0f) : 0);
            snprintf(v3, sizeof v3, "%.1fs", eng ? (double)eng->freeze_seconds : 3.0);
            draw_param_row(px, 100, "RWET", v1, "MWET", v2, "FRZ", v3);
            draw_text(px, 8, 160, "SHIFT: reverb wet / master wet / freeze", COL_MUTED, 1);
            draw_text(px, 8, 218, "K1 rev wet  K2 master  K3 freeze 2-5s", COL_MUTED, 1);
        } else {
            snprintf(v1, sizeof v1, "%d", eng ? (int)(eng->fx_rev_decay * 100.0f) : 0);
            snprintf(v2, sizeof v2, "%d", eng ? (int)(eng->fx_rev_damp * 100.0f) : 0);
            snprintf(v3, sizeof v3, "%d", eng ? (int)(eng->fx_rev_size * 100.0f) : 0);
            draw_param_row(px, 100, "DECAY", v1, "DAMP", v2, "SIZE", v3);
            draw_text(px, 8, 160, "room reverb — Mix K3 is pad send", COL_MUTED, 1);
            draw_text(px, 8, 218, "K1 decay  K2 damp  K3 size", COL_MUTED, 1);
        }
    } else if (page == EPICYCLE_PAGE_AMP) {
        draw_text(px, 8, 52, "AMP ADSR", COL_FG, 3);
        int shift = surf && surf->shift_held;
        if (shift) {
            snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->amp_release * 127.0f) : 0);
            snprintf(v2, sizeof v2, "%d", pad ? (int)(pad->vel_sens * 127.0f) : 0);
            snprintf(v3, sizeof v3, "--");
            draw_param_row(px, 100, "REL", v1, "VEL", v2, "", v3);
            draw_text(px, 8, 160, "LOOP bounce: hit=loop  hit again=Release fade", COL_MUTED, 1);
            draw_text(px, 8, 218, "SHIFT+K1 release  SHIFT+K2 vel", COL_MUTED, 1);
        } else {
            snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->amp_attack * 127.0f) : 0);
            snprintf(v2, sizeof v2, "%d", pad ? (int)(pad->amp_decay * 127.0f) : 0);
            snprintf(v3, sizeof v3, "%d", pad ? (int)(pad->amp_sustain * 127.0f) : 0);
            draw_param_row(px, 100, "ATK", v1, "DEC", v2, "SUS", v3);
            draw_text(px, 8, 160, "full ADSR  hold SHIFT for Release", COL_MUTED, 1);
            draw_text(px, 8, 218, "K1 attack  K2 decay  K3 sustain", COL_MUTED, 1);
        }
    } else if (page == EPICYCLE_PAGE_TUNE) {
        draw_text(px, 8, 52, "TUNE", COL_FG, 4);
        snprintf(v1, sizeof v1, "%+d", pad ? pad->semitone : 0);
        snprintf(v2, sizeof v2, "%+.0f", pad ? (double)pad->fine : 0.0);
        snprintf(v3, sizeof v3, "%+d", pad ? pad->octave : 0);
        draw_param_row(px, 100, "SEMI", v1, "FINE", v2, "OCT", v3);
        draw_text(px, 8, 160, "0 / 0 / 0 = no pitch shift", COL_MUTED, 1);
        draw_text(px, 8, 218, "K1 semi  K2 fine  K3 oct", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_FILTER) {
        draw_text(px, 8, 56, "FILTER", COL_FG, 4);
        snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->filter_cutoff * 127.0f) : 0);
        snprintf(v2, sizeof v2, "%d", pad ? (int)(pad->filter_reso * 127.0f) : 0);
        snprintf(v3, sizeof v3, "%s",
                 epicycle_filter_type_name(pad ? pad->filter_type : 0));
        draw_param_row(px, 100, "CUT", v1, "RES", v2, "TYPE", v3);
        draw_text(px, 8, 160, "default OFF = open/hearable", COL_MUTED, 1);
        draw_text(px, 8, 218, "K1 cutoff  K2 reso  K3 type", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_FILT_ENV) {
        draw_text(px, 8, 56, "FILT ENV", COL_FG, 4);
        snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->filt_attack * 127.0f) : 0);
        snprintf(v2, sizeof v2, "%d", pad ? (int)(pad->filt_decay * 127.0f) : 0);
        snprintf(v3, sizeof v3, "%d", pad ? (int)(pad->filt_depth * 127.0f) : 0);
        draw_param_row(px, 100, "ATK", v1, "DEC", v2, "DEP", v3);
        draw_text(px, 8, 160, "depth>0 auto-LPF  opens then closes", COL_MUTED, 1);
        draw_text(px, 8, 218, "K1 attack  K2 decay  K3 depth", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_LFO) {
        draw_text(px, 8, 48, "LFO", COL_FG, 3);
        const EpicycleLfo *l = pad ? &pad->lfos[0] : NULL;
        int slot = pad ? pad->mod_edit_slot : 0;
        if (slot < 0)
            slot = 0;
        if (slot >= EPICYCLE_MOD_SLOTS)
            slot = EPICYCLE_MOD_SLOTS - 1;
        const EpicycleModRoute *mr = pad ? &pad->mods[slot] : NULL;
        int shift = surf && surf->shift_held;
        if (!shift) {
            snprintf(v1, sizeof v1, "%s", l ? lfo_shape_name(l->shape) : "SIN");
            if (l && l->sync_beats > 0.0f)
                snprintf(v2, sizeof v2, "%.2fb", (double)l->sync_beats);
            else
                snprintf(v2, sizeof v2, "%.2fHz", l ? (double)l->rate_hz : 0.0);
            snprintf(v3, sizeof v3, "%d", l ? (int)(l->depth * 100.0f) : 0);
            draw_param_row(px, 92, "SHAPE", v1, "RATE", v2, "DEPTH", v3);
            draw_text(px, 8, 148, "source — depth alone does nothing", COL_MUTED, 1);
        } else {
            snprintf(v1, sizeof v1, "%s", mr ? epicycle_mod_dest_name(mr->dest) : "OFF");
            snprintf(v2, sizeof v2, "%d", mr ? (int)(fabsf(mr->amount) * 100.0f) : 0);
            snprintf(v3, sizeof v3, "L%d", mr && mr->lfo_index >= 0 ? mr->lfo_index : 0);
            draw_param_row(px, 92, "DEST", v1, "AMT", v2, "LFO", v3);
            draw_text(px, 8, 148, "matrix route (per pad)", COL_MOD, 1);
        }
        snprintf(line, sizeof line, "slot %d/%d  dest=%s amt=%d", slot + 1, EPICYCLE_MOD_SLOTS,
                 mr ? epicycle_mod_dest_name(mr->dest) : "OFF",
                 mr ? (int)(fabsf(mr->amount) * 100.0f) : 0);
        draw_text(px, 8, 168, line, COL_PAGE, 1);
        if (mr && mr->dest == EPICYCLE_MOD_BRACE_START && fabsf(mr->amount) > 0.02f && l &&
            l->enabled && l->depth > 0.02f)
            draw_text(px, 8, 186, "POS offset ON", COL_OK, 1);
        else
            draw_text(px, 8, 186, "POS off until DEST=POS+AMT", COL_MUTED, 1);
        draw_text(px, 8, 210, shift ? "SHIFT: DEST AMT LFO#  enc=slot" : "K1/K2/K3 source  SHIFT=matrix",
                  COL_MUTED, 1);
        draw_text(px, 8, 224, "SAMPLE+pad select  then route", COL_MUTED, 1);
    }

    if (surf) {
        fill_rect(px, UI_COL0, 234, 1 + (int)surf->k1 * 90 / 127, 4, COL_ACCENT);
        fill_rect(px, UI_COL1, 234, 1 + (int)surf->k2 * 90 / 127, 4, COL_ACCENT);
        fill_rect(px, UI_COL2, 234, 1 + (int)surf->k3 * 90 / 127, 4, COL_BRACE);
    }

    display_present();
}
