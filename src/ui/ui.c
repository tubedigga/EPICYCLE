/* SPDX-License-Identifier: MIT
 *
 * Loop-engine UI: brand splash + pad grid, selected pad waveform with brace
 * window, knob roles, division/free + LFO scrub hints.
 */
#include "epicycle.h"

#include <stdio.h>
#include <string.h>

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
    COL_BG = 0x001A1410u,
    COL_FG = 0x00E8DCC8u,
    COL_MUTED = 0x009A8F82u,
    COL_ACCENT = 0x002A6F8Cu,
    COL_PAD_OFF = 0x002A2420u,
    COL_PAD_ON = 0x0038A090u,
    COL_PAD_SEL = 0x00C4A060u,
    COL_OK = 0x0048A060u,
    COL_BAD = 0x00A05040u,
    COL_WAVE = 0x00406070u,
    COL_BRACE = 0x00E0A050u,
    COL_MOD = 0x0060B090u,
};

void ui_draw_splash(const char *status_line)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);
    fill_rect(px, 0, 36, EPICYCLE_FB_W, 3, COL_ACCENT);
    draw_text(px, 28, 78, "EPICYCLE", COL_FG, 4);
    draw_text(px, 28, 128, "v0.2.0-loop", COL_MUTED, 1);
    if (status_line && status_line[0])
        draw_text(px, 28, 156, status_line, COL_FG, 1);
    draw_text(px, 28, 190, "loop engine", COL_MUTED, 1);
    fill_rect(px, 0, EPICYCLE_FB_H - 40, EPICYCLE_FB_W, 3, COL_ACCENT);
    display_present();
}

static void draw_waveform(uint32_t *px, const EpicyclePad *pad, int x, int y, int w, int h)
{
    fill_rect(px, x, y, w, h, 0x00201814u);
    fill_rect(px, x, y + h / 2, w, 1, 0x00302824u);

    if (!pad || !pad->sample || !pad->sample->data || pad->sample->frames == 0)
        return;

    const EpicycleSample *s = pad->sample;
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
        int amp = (int)(peak * (float)(h / 2 - 1));
        if (amp < 1)
            amp = 1;
        fill_rect(px, x + col, y + h / 2 - amp, 1, amp * 2, COL_WAVE);
    }

    /* Base brace (user) and modulated window (live). */
    int bx = x + (int)(pad->window_start * (float)w);
    int bw = (int)(pad->window_length * (float)w);
    if (bw < 2)
        bw = 2;
    fill_rect(px, bx, y, 2, h, COL_BRACE);
    fill_rect(px, bx + bw - 2, y, 2, h, COL_BRACE);
    fill_rect(px, bx, y, bw, 2, COL_BRACE);

    int mx = x + (int)(pad->mod_window_start * (float)w);
    int mw = (int)(pad->mod_window_length * (float)w);
    if (mw < 2)
        mw = 2;
    fill_rect(px, mx, y + h - 3, mw, 3, COL_MOD);
}

void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);

    fill_rect(px, 0, 0, EPICYCLE_FB_W, 3, COL_ACCENT);
    draw_text(px, 8, 8, "EPICYCLE", COL_FG, 2);

    char line[56];
    if (eng)
        snprintf(line, sizeof line, "%3.0f bpm", (double)eng->tempo_bpm);
    else
        snprintf(line, sizeof line, "--- bpm");
    draw_text(px, 160, 12, line, COL_MUTED, 1);

    snprintf(line, sizeof line, "%s", hw && hw->audio_ok ? "alsa" : "silent");
    draw_text(px, 250, 12, line, hw && hw->audio_ok ? COL_OK : COL_MUTED, 1);

    /* Compact 4×4 pad grid */
    const int grid_x = 8, grid_y = 36, cell = 22, gap = 3;
    int sel = eng ? eng->selected_pad : 0;
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        int col = i % 4;
        int row = i / 4;
        int x = grid_x + col * (cell + gap);
        int y = grid_y + row * (cell + gap);
        int on = surf && surf->pad_vel[i] > 0;
        uint32_t c = COL_PAD_OFF;
        if (i == sel)
            c = COL_PAD_SEL;
        if (on)
            c = COL_PAD_ON;
        fill_rect(px, x, y, cell, cell, c);
        if (eng && eng->pads[i].sample)
            fill_rect(px, x + 2, y + cell - 4, cell - 4, 2, COL_ACCENT);
    }

    const EpicyclePad *pad = (eng && sel >= 0 && sel < EPICYCLE_PAD_COUNT) ? &eng->pads[sel] : NULL;

    /* Waveform + brace */
    draw_waveform(px, pad, 120, 36, 192, 56);

    if (pad) {
        snprintf(line, sizeof line, "pad %d", sel + 1);
        draw_text(px, 120, 96, line, COL_FG, 1);
        if (pad->sample && pad->sample->name[0])
            draw_text(px, 170, 96, pad->sample->name, COL_MUTED, 1);

        if (pad->freeform)
            snprintf(line, sizeof line, "FREE win %.2f+%.2f", (double)pad->window_start,
                     (double)pad->window_length);
        else
            snprintf(line, sizeof line, "DIV %db win %.2f", pad->division_beats,
                     (double)pad->window_start);
        draw_text(px, 8, 148, line, COL_FG, 1);

        snprintf(line, sizeof line, "xf %.2f  pitch %d:%d:%+.0f", (double)pad->crossfade,
                 pad->octave, pad->semitone, (double)pad->fine);
        draw_text(px, 8, 162, line, COL_MUTED, 1);

        int lfo_on = pad->lfos[0].enabled && pad->lfos[0].depth > 0;
        snprintf(line, sizeof line, "LFO %s scrub %s  matrix ok", lfo_on ? "on" : "off",
                 pad->allow_scrub ? "yes" : "cap");
        draw_text(px, 8, 176, line, lfo_on ? COL_MOD : COL_MUTED, 1);
    }

    /* Knob legend */
    draw_text(px, 8, 194, "K1 start  K2 len/div  K3 xfade", COL_MUTED, 1);
    draw_text(px, 8, 210, "softL free/div  softM LFO  wheel quit", COL_MUTED, 1);
    if (surf) {
        fill_rect(px, 250, 194, (int)surf->k1 / 2, 4, COL_ACCENT);
        fill_rect(px, 250, 202, (int)surf->k2 / 2, 4, COL_ACCENT);
        fill_rect(px, 250, 210, (int)surf->k3 / 2, 4, COL_BRACE);
    }
    fill_rect(px, 0, EPICYCLE_FB_H - 3, EPICYCLE_FB_W, 3, COL_ACCENT);
    display_present();
}
