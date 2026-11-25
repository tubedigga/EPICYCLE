/* SPDX-License-Identifier: MIT
 *
 * Loop UI + Sample-Mode-style pages (0.2.6): Trim Start/End/POS + Shift
 * overlays (crossfade, ping-pong); equidistant param columns; filter names.
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

void ui_draw_loop(const EpicycleHwStatus *hw, const EpicycleEngine *eng,
                  const EpicycleSurfaceState *surf)
{
    uint32_t *px = display_pixels();
    display_clear(COL_BG);
    (void)hw;

    int sel = eng ? eng->selected_pad : 0;
    const EpicyclePad *pad =
        (eng && sel >= 0 && sel < EPICYCLE_PAD_COUNT) ? &eng->pads[sel] : NULL;
    EpicycleModePage page = eng ? eng->mode_page : EPICYCLE_PAGE_TRIM;

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
    if (surf && surf->shift_held)
        draw_text(px, 168, 28, "SHIFT", COL_BRACE, 2);
    if (pad && pad->loop_enable)
        draw_text(px, 290, 4, "LOOP", COL_MOD, 1);
    else
        draw_text(px, 290, 4, "1SHOT", COL_MUTED, 1);

    if (page == EPICYCLE_PAGE_TRIM) {
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
            if (pad && pad->freeform)
                draw_text(px, 8, 212, "K1/K2 width  K3 slide  FREE", COL_MUTED, 1);
            else
                draw_text(px, 8, 212, "K2 div  K3 slide  DIV", COL_MUTED, 1);
            draw_text(px, 8, 226, "enc: free/div  SHIFT: xfade/dir", COL_MUTED, 1);
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
        draw_text(px, 8, 160, "(DSP stub — values stored)", COL_MUTED, 1);
        draw_text(px, 8, 218, "K1 cutoff  K2 reso  K3 type", COL_MUTED, 1);
    } else if (page == EPICYCLE_PAGE_FILT_ENV) {
        draw_text(px, 8, 56, "FILT ENV", COL_FG, 4);
        snprintf(v1, sizeof v1, "%d", pad ? (int)(pad->filt_attack * 127.0f) : 0);
        snprintf(v2, sizeof v2, "%d", pad ? (int)(pad->filt_decay * 127.0f) : 0);
        snprintf(v3, sizeof v3, "%d", pad ? (int)(pad->filt_depth * 127.0f) : 0);
        draw_param_row(px, 100, "ATK", v1, "DEC", v2, "DEP", v3);
        draw_text(px, 8, 160, "(follows filter stub)", COL_MUTED, 1);
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
