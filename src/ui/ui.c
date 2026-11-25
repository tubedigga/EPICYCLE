/* SPDX-License-Identifier: MIT
 *
 * Bring-up UI: splash proves EPICYCLE owns the panel; home shows pad grid,
 * knob readouts, and a quit hint. Loop-engine / waveform UI comes later.
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

/* Compact 5×5 bitmap glyphs (rows separated by spaces). */
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

/* Palette: deep ink + warm paper + teal accent (not purple-on-white). */
enum {
    COL_BG = 0x001A1410u,
    COL_FG = 0x00E8DCC8u,
    COL_MUTED = 0x009A8F82u,
    COL_ACCENT = 0x002A6F8Cu,
    COL_PAD_OFF = 0x002A2420u,
    COL_PAD_ON = 0x0038A090u,
    COL_OK = 0x0048A060u,
    COL_BAD = 0x00A05040u,
};

void ui_draw_splash(const char *status_line)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);
    fill_rect(px, 0, 36, EPICYCLE_FB_W, 3, COL_ACCENT);
    draw_text(px, 28, 78, "EPICYCLE", COL_FG, 4);
    draw_text(px, 28, 128, "v0.1.0-bringup", COL_MUTED, 1);
    if (status_line && status_line[0])
        draw_text(px, 28, 156, status_line, COL_FG, 1);
    draw_text(px, 28, 190, "taking over panel", COL_MUTED, 1);
    fill_rect(px, 0, EPICYCLE_FB_H - 40, EPICYCLE_FB_W, 3, COL_ACCENT);
    display_present();
}

void ui_draw_home(const EpicycleHwStatus *hw, const EpicycleSurfaceState *surf)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);

    fill_rect(px, 0, 0, EPICYCLE_FB_W, 3, COL_ACCENT);
    draw_text(px, 8, 10, "EPICYCLE", COL_FG, 2);
    draw_text(px, 160, 14, "bring-up", COL_MUTED, 1);

    /* Hardware status row */
    char line[48];
    snprintf(line, sizeof line, "fb %s", hw && hw->display_ok ? "ok" : "off");
    draw_text(px, 8, 40, line, hw && hw->display_ok ? COL_OK : COL_BAD, 1);
    snprintf(line, sizeof line, "alsa %s", hw && hw->audio_ok ? "ok" : "off");
    draw_text(px, 90, 40, line, hw && hw->audio_ok ? COL_OK : COL_BAD, 1);
    snprintf(line, sizeof line, "midi %s", hw && hw->surface_ok ? "ok" : "off");
    draw_text(px, 190, 40, line, hw && hw->surface_ok ? COL_OK : COL_BAD, 1);

    /* 4×4 pad grid */
    const int grid_x = 12, grid_y = 58, cell = 28, gap = 4;
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        int col = i % 4;
        int row = i / 4;
        int x = grid_x + col * (cell + gap);
        int y = grid_y + row * (cell + gap);
        int on = surf && surf->pad_vel[i] > 0;
        fill_rect(px, x, y, cell, cell, on ? COL_PAD_ON : COL_PAD_OFF);
        char num[4];
        snprintf(num, sizeof num, "%d", i + 1);
        draw_text(px, x + 8, y + 10, num, on ? COL_FG : COL_MUTED, 1);
    }

    /* Knob / fader readouts */
    int kx = 160;
    int ky = 58;
    if (surf) {
        snprintf(line, sizeof line, "K1 %3u", (unsigned)surf->k1);
        draw_text(px, kx, ky, line, COL_FG, 1);
        fill_rect(px, kx + 60, ky + 2, (int)surf->k1, 6, COL_ACCENT);

        snprintf(line, sizeof line, "K2 %3u", (unsigned)surf->k2);
        draw_text(px, kx, ky + 22, line, COL_FG, 1);
        fill_rect(px, kx + 60, ky + 24, (int)surf->k2, 6, COL_ACCENT);

        snprintf(line, sizeof line, "K3 %3u", (unsigned)surf->k3);
        draw_text(px, kx, ky + 44, line, COL_FG, 1);
        fill_rect(px, kx + 60, ky + 46, (int)surf->k3, 6, COL_ACCENT);

        snprintf(line, sizeof line, "VOL %3u", (unsigned)surf->main_vol);
        draw_text(px, kx, ky + 66, line, COL_MUTED, 1);

        snprintf(line, sizeof line, "wheel %+d", surf->wheel_accum);
        draw_text(px, kx, ky + 88, line, COL_MUTED, 1);
    }

    draw_text(px, 8, 210, "push wheel = quit to stock", COL_MUTED, 1);
    fill_rect(px, 0, EPICYCLE_FB_H - 3, EPICYCLE_FB_W, 3, COL_ACCENT);
    display_present();
}
