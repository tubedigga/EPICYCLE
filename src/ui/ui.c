/* SPDX-License-Identifier: MIT
 *
 * Loop UI (Ash 0.2.4): one large waveform + brace for the selected pad.
 * No 16-pad grid, no top-left brand chrome. Soft-key labels big;
 * start / end / position readable at the bottom. Pad LEDs show selection.
 */
#include "epicycle.h"

#include <stdio.h>
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
        /* Fallback if assets/boot.png missing (host without package copy). */
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

static void draw_waveform(uint32_t *px, const EpicyclePad *pad, float playhead,
                          int x, int y, int w, int h)
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

    /* Dim fill under the live brace. */
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

    /* Brace edges + top rail (user window in brace color). */
    int ux = x + (int)(pad->window_start * (float)w);
    int uw = (int)(pad->window_length * (float)w);
    if (uw < 3)
        uw = 3;
    fill_rect(px, ux, y, 3, h, COL_BRACE);
    fill_rect(px, ux + uw - 3, y, 3, h, COL_BRACE);
    fill_rect(px, ux, y, uw, 3, COL_BRACE);
    fill_rect(px, ux, y + h - 3, uw, 3, COL_BRACE);

    /* Modulated window marker along the bottom. */
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

void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);
    (void)hw;

    int sel = eng ? eng->selected_pad : 0;
    const EpicyclePad *pad =
        (eng && sel >= 0 && sel < EPICYCLE_PAD_COUNT) ? &eng->pads[sel] : NULL;

    /* Soft-key labels under the three physical keys (big, scannable). */
    draw_text(px, 8, 4, "FREE/DIV", COL_SOFT, 2);
    draw_text(px, 130, 4, "LFO", COL_SOFT, 2);
    draw_text(px, 230, 4, "QUIT", COL_MUTED, 2);

    char line[48];
    /* Selected pad + mode + tempo — one short header, no brand chrome. */
    if (pad) {
        if (pad->freeform)
            snprintf(line, sizeof line, "PAD %d  FREE", sel + 1);
        else
            snprintf(line, sizeof line, "PAD %d  DIV %d", sel + 1, pad->division_beats);
    } else {
        snprintf(line, sizeof line, "PAD %d", sel + 1);
    }
    draw_text(px, 8, 28, line, COL_FG, 2);

    if (eng)
        snprintf(line, sizeof line, "%3.0f", (double)eng->tempo_bpm);
    else
        snprintf(line, sizeof line, "---");
    draw_text(px, 248, 28, line, COL_MUTED, 2);
    draw_text(px, 290, 32, "bpm", COL_MUTED, 1);

    if (surf && surf->sample_held)
        draw_text(px, 200, 28, "SEL", COL_BRACE, 2);

    float playhead = eng ? engine_pad_playhead(eng, sel) : -1.0f;
    /* Large waveform: nearly full width, ~half the screen. */
    draw_waveform(px, pad, playhead, 4, 52, 312, 118);

    /* Bottom readouts: start / end / position at readable size. */
    float start = pad ? pad->window_start : 0.0f;
    float len = pad ? pad->window_length : 0.0f;
    float end = start + len;
    if (end > 1.0f)
        end = 1.0f;
    float pos = playhead >= 0.0f ? playhead : start;

    draw_text(px, 8, 178, "START", COL_MUTED, 1);
    draw_text(px, 118, 178, "END", COL_MUTED, 1);
    draw_text(px, 220, 178, "POS", COL_MUTED, 1);

    snprintf(line, sizeof line, "%.2f", (double)start);
    draw_text(px, 8, 190, line, COL_FG, 3);
    snprintf(line, sizeof line, "%.2f", (double)end);
    draw_text(px, 118, 190, line, COL_FG, 3);
    snprintf(line, sizeof line, "%.2f", (double)pos);
    draw_text(px, 220, 190, line, COL_BRACE, 3);

    /* Knob legend + meters. */
    draw_text(px, 8, 218, "K1 start", COL_MUTED, 1);
    draw_text(px, 100, 218, "K2 len", COL_MUTED, 1);
    draw_text(px, 180, 218, "K3 xfade", COL_MUTED, 1);
    if (pad && pad->lfos[0].enabled && pad->lfos[0].depth > 0)
        draw_text(px, 268, 218, "LFO", COL_MOD, 1);
    else
        draw_text(px, 268, 218, "LFO", COL_MUTED, 1);

    if (surf) {
        fill_rect(px, 8, 230, 1 + (int)surf->k1 * 80 / 127, 6, COL_ACCENT);
        fill_rect(px, 100, 230, 1 + (int)surf->k2 * 70 / 127, 6, COL_ACCENT);
        fill_rect(px, 180, 230, 1 + (int)surf->k3 * 70 / 127, 6, COL_BRACE);
    }

    display_present();
}
