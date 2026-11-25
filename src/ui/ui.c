/* SPDX-License-Identifier: MIT
 *
 * Tiny splash for the bring-up scaffold. Real waveform + brace UI comes later.
 */
#include "epicycle.h"

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

/* 5x7-ish glyphs for a few characters — enough for "EPICYCLE" branding. */
static const char *glyph(char ch)
{
    switch (ch) {
    case 'E': return "11110 10000 11100 10000 11110";
    case 'P': return "11100 10010 11100 10000 10000";
    case 'I': return "11100 01000 01000 01000 11100";
    case 'C': return "01110 10000 10000 10000 01110";
    case 'Y': return "10001 01010 00100 00100 00100";
    case 'L': return "10000 10000 10000 10000 11110";
    case ' ': return "00000 00000 00000 00000 00000";
    case '0': return "01110 10001 10001 10001 01110";
    case '.': return "00000 00000 00000 00000 00100";
    case '-': return "00000 00000 11100 00000 00000";
    case 'v': return "00000 10001 10001 01010 00100";
    case 's': return "01110 10000 01110 00001 11110";
    case 'c': return "00000 01110 10000 10000 01110";
    case 'a': return "00000 01110 00001 01111 10001";
    case 'f': return "00110 01000 11100 01000 01000";
    case 'o': return "00000 01110 10001 10001 01110";
    case 'l': return "01000 01000 01000 01000 00110";
    case 'd': return "00001 00001 01111 10001 01111";
    default:  return "11111 10001 10001 10001 11111";
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

void ui_draw_splash(const char *status_line)
{
    uint32_t *px = display_pixels();
    /* Deep ink + warm accent — not purple-on-white AI defaults. */
    const uint32_t bg = 0xFF1A1410u;   /* BGRX-ish dark brown-black */
    const uint32_t fg = 0xFFE8DCC8u;   /* warm paper */
    const uint32_t accent = 0xFF2A6F8Cu; /* teal bar */

    display_clear(bg);
    fill_rect(px, 0, 40, EPICYCLE_FB_W, 3, accent);
    draw_text(px, 24, 80, "EPICYCLE", fg, 3);
    draw_text(px, 24, 130, "v0.0.1-scaffold", fg, 1);
    if (status_line && status_line[0])
        draw_text(px, 24, 160, status_line, fg, 1);
    fill_rect(px, 0, EPICYCLE_FB_H - 43, EPICYCLE_FB_W, 3, accent);
    display_present();
}
