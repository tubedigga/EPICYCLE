/* SPDX-License-Identifier: MIT
 *
 * UI: Tombola primary (2D projected drum) + Sample-Mode edit pages + Hit Mod
 * multi-slot matrix (impact → vol/pitch/cut/sends). Freeze capture is phase 2.
 */
#include "epicycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    COL_BG = 0x0014100Cu,
    COL_FG = 0x00F0E6D8u,
    COL_MUTED = 0x008A8074u,
    COL_ACCENT = 0x002A6F8Cu,
    COL_OK = 0x0048A060u,
    COL_WAVE = 0x00385868u,
    COL_WAVE_DIM = 0x00283038u,
    COL_BRACE = 0x00E8A848u,
    COL_BRACE_FILL = 0x00302818u,
    COL_MOD = 0x0060B090u,
    COL_PLAY = 0x00F0F0E8u,
    COL_SOFT = 0x00C8B898u,
    COL_PAGE = 0x00D0C0A0u,
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

/* Bresenham line with mild thickness for smoother polygon edges. */
static void draw_line(uint32_t *px, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(px, x0, y0, c);
        put_px(px, x0 + 1, y0, c);
        put_px(px, x0, y0 + 1, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Filled disk (circle) — smoother balls than squares. */
static void fill_disk(uint32_t *px, int cx, int cy, int r, uint32_t c)
{
    if (r < 1)
        r = 1;
    int r2 = r * r;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r2)
                put_px(px, cx + x, cy + y, c);
        }
    }
}

/* Soft rim ring around a disk. */
static void stroke_disk(uint32_t *px, int cx, int cy, int r, uint32_t c)
{
    if (r < 2)
        return;
    int r2 = r * r;
    int rin = (r - 1) * (r - 1);
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= r2 && d >= rin)
                put_px(px, cx + x, cy + y, c);
        }
    }
}

static void draw_tombola(uint32_t *px, const EpicycleTombola *tb, int sel)
{
    int cx = 160, cy = 116, scale = 90;
    if (!tb)
        return;

    /* Soft ground ellipse (3D hint) — denser fill, less blocky. */
    for (int y = -(int)(scale * 0.55f); y <= (int)(scale * 0.6f); y++) {
        float ty = (float)y / (float)scale;
        int half = (int)((1.0f - ty * ty * 0.85f) * (float)scale);
        if (half < 2)
            continue;
        fill_rect(px, cx - half, cy + y, half * 2, 1, 0x00161612u);
    }

    int faces = tb->face_count;
    if (faces <= 0) {
        /* Dense circle outline via short chords. */
        int prevx = 0, prevy = 0;
        const int segs = 72;
        for (int i = 0; i <= segs; i++) {
            float ox, oy;
            tombola_edge_point(tb, 0, (float)i / (float)segs, &ox, &oy);
            int sx, sy;
            tombola_world_to_screen(tb, ox, oy, cx, cy, scale, &sx, &sy);
            if (i > 0)
                draw_line(px, prevx, prevy, sx, sy, COL_ACCENT);
            prevx = sx;
            prevy = sy;
        }
    } else {
        for (int e = 0; e < faces; e++) {
            float ox0, oy0, ox1, oy1;
            tombola_edge_point(tb, e, 0.0f, &ox0, &oy0);
            tombola_edge_point(tb, e, 1.0f, &ox1, &oy1);
            int x0, y0, x1, y1;
            tombola_world_to_screen(tb, ox0, oy0, cx, cy, scale, &x0, &y0);
            tombola_world_to_screen(tb, ox1, oy1, cx, cy, scale, &x1, &y1);
            draw_line(px, x0, y0, x1, y1, COL_BRACE);
        }
    }

    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (!tb->balls[i].active)
            continue;
        int sx, sy;
        tombola_world_to_screen(tb, tb->balls[i].x, tb->balls[i].y, cx, cy, scale, &sx, &sy);
        uint32_t col = tombola_ball_color(i);
        if (tb->balls[i].hit_flash > 0.2f)
            col = COL_PLAY;
        int r = (i == sel) ? 6 : 5;
        fill_disk(px, sx, sy, r, col);
        if (i == sel)
            stroke_disk(px, sx, sy, r + 2, COL_BRACE);
        else
            stroke_disk(px, sx, sy, r + 1, 0x00202828u);
    }
}

void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf, const EpicycleTombola *tb)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);
    (void)hw;

    int sel = eng ? eng->selected_pad : 0;
    const EpicyclePad *pad =
        (eng && sel >= 0 && sel < EPICYCLE_PAD_COUNT) ? &eng->pads[sel] : NULL;
    EpicycleModePage page = eng ? eng->mode_page : EPICYCLE_PAGE_TOMBOLA;

    draw_soft_labels(px);

    char line[48];
    char v1[24], v2[24], v3[24];

    /* Header: pad + page name + tempo. */
    snprintf(line, sizeof line, "PAD %d  %s", sel + 1, epicycle_page_name(page));
    draw_text(px, 8, 28, line, COL_PAGE, 2);

    if (eng)
        snprintf(line, sizeof line, "%3.0f", (double)eng->tempo_bpm);
    else
        snprintf(line, sizeof line, "---");
    draw_text(px, 248, 28, line, COL_MUTED, 2);
    draw_text(px, 290, 32, "bpm", COL_MUTED, 1);

    if (surf && surf->sample_held)
        draw_text(px, 200, 28, "SEL", COL_BRACE, 2);
    if (surf && surf->mute_held)
        draw_text(px, 168, 28, "MUTE", COL_BRACE, 2);
    if (surf && surf->shift_held)
        draw_text(px, 140, 28, "SHIFT", COL_BRACE, 2);
    if (pad && pad->loop_enable)
        draw_text(px, 290, 4, "LOOP", COL_MOD, 1);
    else
        draw_text(px, 290, 4, "1SHOT", COL_MUTED, 1);

    if (page == EPICYCLE_PAGE_TOMBOLA) {
        draw_tombola(px, tb, sel);
        int faces = tb ? tb->face_count : 0;
        if (faces <= 0)
            snprintf(v1, sizeof v1, "CIRC");
        else
            snprintf(v1, sizeof v1, "%dF", faces);
        {
            float sp = tb ? tb->spin_speed : 0.0f;
            if (sp > -0.05f && sp < 0.05f)
                snprintf(v2, sizeof v2, "STOP");
            else
                snprintf(v2, sizeof v2, "%+.1f", (double)sp);
        }
        int active = tb ? tombola_active_count(tb) : 0;
        snprintf(v3, sizeof v3, "%d", active);
        draw_param_row(px, 188, "SHAPE", v1, "SPIN", v2, "BALLS", v3);
        draw_text(px, 8, 226, "K1 shape  K2 spin+/-  K3/MUTE balls  FULL=HITMOD", COL_MUTED, 1);
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
        snprintf(v1, sizeof v1, "%s", r ? epicycle_hitmod_dest_name(r->dest) : "OFF");
        snprintf(v2, sizeof v2, "%d", r ? (int)(r->amount * 100.0f) : 0);
        snprintf(v3, sizeof v3, "%s", r ? epicycle_hitmod_src_name(r->src) : "IMPA");
        draw_param_row(px, 178, "DEST", v1, "AMT", v2, "SRC", v3);
        draw_text(px, 8, 214, "ENC row  push=tog  K1 dest K2 amt K3 src", COL_MUTED, 1);
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
            snprintf(line, sizeof line, "%.2f", (double)start);
            draw_text(px, UI_COL0, 184, line, COL_FG, 3);
            snprintf(line, sizeof line, "%.2f", (double)end);
            draw_text(px, UI_COL1, 184, line, COL_FG, 3);
            snprintf(line, sizeof line, "%.2f", (double)pos);
            draw_text(px, UI_COL2, 184, line, COL_BRACE, 3);
            /* Large FREE / DIVn badge — encoder push toggles (Ash discoverability). */
            if (pad && pad->freeform) {
                draw_text(px, 214, 52, "FREE", COL_BRACE, 3);
                draw_text(px, 8, 212, "K1/K2 width  K3 slide", COL_MUTED, 1);
                draw_text(px, 8, 226, "ENC push -> DIV  hold SHIFT xfade", COL_MUTED, 1);
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
            snprintf(v3, sizeof v3, "--");
        draw_param_row(px, 100, "VOL", v1, "PAN", v2, surf && surf->shift_held ? "KIT" : "SEND",
                       v3);
        if (surf) {
            snprintf(line, sizeof line, "main %d  fader %s%d", (int)surf->main_vol,
                     surf->fader_engaged ? "" : "~", (int)surf->fader);
            draw_text(px, 8, 160, line, COL_MUTED, 1);
        }
        draw_text(px, 8, 218, "K1 pad vol  K2 pan  SHIFT+K3 kit", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_AMP) {
        draw_text(px, 8, 52, "AMP ENV", COL_FG, 4);
        snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->amp_attack * 127.0f) : 0);
        snprintf(v2, sizeof v2, "%d", pad ? (int)(pad->amp_decay * 127.0f) : 0);
        snprintf(v3, sizeof v3, "%d", pad ? (int)(pad->vel_sens * 127.0f) : 0);
        draw_param_row(px, 100, "ATK", v1, "DEC", v2, "VEL", v3);
        draw_text(px, 8, 218, "K1 attack  K2 decay  K3 vel", COL_MUTED, 1);
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
        draw_text(px, 8, 160, "env modulates cutoff depth", COL_MUTED, 1);
        draw_text(px, 8, 218, "K1 attack  K2 decay  K3 depth", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_LFO) {
        draw_text(px, 8, 56, "LFO", COL_FG, 4);
        const EpicycleLfo *l = pad ? &pad->lfos[0] : NULL;
        snprintf(v1, sizeof v1, "%s", l ? lfo_shape_name(l->shape) : "SIN");
        if (l && l->sync_beats > 0.0f)
            snprintf(v2, sizeof v2, "%.2fb", (double)l->sync_beats);
        else
            snprintf(v2, sizeof v2, "%.2fHz", l ? (double)l->rate_hz : 0.0);
        snprintf(v3, sizeof v3, "%d", l ? (int)(l->depth * 100.0f) : 0);
        draw_param_row(px, 100, "SHAPE", v1, "RATE", v2, "DEPTH", v3);
        if (l && l->sync_beats > 0.0f)
            draw_text(px, 8, 160, "SYNC  enc push -> free", COL_MOD, 2);
        else
            draw_text(px, 8, 160, "FREE  enc push -> sync", COL_MUTED, 2);
        int on = l && l->enabled && l->depth > 0.0f;
        draw_text(px, 8, 190, on ? "POS scrub ON" : "POS scrub off", on ? COL_MOD : COL_MUTED, 1);
        draw_text(px, 8, 218, "K1 shape  K2 rate  K3 depth", COL_MUTED, 1);
    }

    if (surf) {
        fill_rect(px, UI_COL0, 234, 1 + (int)surf->k1 * 90 / 127, 4, COL_ACCENT);
        fill_rect(px, UI_COL1, 234, 1 + (int)surf->k2 * 90 / 127, 4, COL_ACCENT);
        fill_rect(px, UI_COL2, 234, 1 + (int)surf->k3 * 90 / 127, 4, COL_BRACE);
    }

    display_present();
}
