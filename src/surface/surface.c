/* SPDX-License-Identifier: MIT
 *
 * Control-surface MIDI (UART3 raw MIDI / ALSA card "Surface").
 * Published map facts from SampleBoot TECHNICAL.md and sample-base HARDWARE.md
 * / controls.h (do not reverse stock; do not vendor AGPL headers):
 *   - pads: channel 10, notes 37,36,42,82,40,38,46,44,48,47,45,43,49,55,51,53
 *   - pad LED SysEx I = printed pad − 1 (0..15); fader light I=16
 *   - knobs K1–K3: CC 16/17/18; main volume CC 19; fader CC 20 (channel 1)
 *   - wheel: CC 100 relative; wheel push ≈ note 111
 *   - SAMPLE=6, SHIFT=49, LOOP=74, MUTE=43 (Pad Play)
 * Soft keys above the screen: notes 12/13/14 — B1 prev page, B2 next, B3 quit.
 * Wheel push (note 111) is an edge only — never quits.
 * Poly aftertouch must NOT steal pad note running-status (phantom pads / POS).
 * Fader: record 0x42 dump but do not engage until the user moves it.
 * Hold SAMPLE + pad = select (app); plain pad = play. Pad LEDs show selection.
 * On open: clear leftover lights + send position-request SysEx 0x42.
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef EPICYCLE_SND_DIR
#define EPICYCLE_SND_DIR "/dev/snd"
#endif

/* Printed pad 1..16 → channel-10 note (public measurement, HARDWARE.md). */
static const uint8_t PAD_NOTES[EPICYCLE_PAD_COUNT] = {
    37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53};

static const uint8_t BUTTON_LED_CCS[] = {
    6, 7, 1, 0, 35, 9, 11, 75, 43, 74, 40, 36, 53, 73, 80, 81, 82};

/* Buttons — sample-base controls.h / HARDWARE.md (MIT rewrite of note numbers). */
enum {
    NOTE_SAMPLE = 6,
    NOTE_SHIFT = 49,
    NOTE_LOOP = 74,
    NOTE_MUTE = 43,
};

static int g_fd = -1;
static EpicycleSurfaceState g_state;
static char g_info[64] = "surface closed";
static int g_selected_pad;
static int g_loop_led_on = -1;

/* MIDI parser */
static int m_status, m_need, m_n, m_sysex;
static int m_voice_status;     /* last channel-voice status; kept across SysEx */
static int m_note_voice_status; /* last note on/off only — AT must not steal this */
static uint8_t m_d[2];
static uint8_t g_pad_lit[EPICYCLE_PAD_COUNT]; /* 1 if we last sent a non-black pad LED */

static int pad_index(uint8_t note)
{
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (PAD_NOTES[i] == note)
            return i;
    }
    return -1;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int midi_write_all(int fd, const uint8_t *buf, size_t n)
{
    size_t done = 0;
    double until = now_s() + 1.0;
    while (done < n) {
        ssize_t w = write(fd, buf + done, n - done);
        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EINTR)
            return -1;
        if (now_s() > until)
            return -1;
        struct pollfd p = {.fd = fd, .events = POLLOUT, .revents = 0};
        poll(&p, 1, 50);
    }
    return 0;
}

static void surface_lights_off(int fd)
{
    uint8_t buf[512];
    size_t n = 0;
    /* Pads 0..15 + fader light I=16 (sample-base HARDWARE.md). */
    for (int i = 0; i <= 16; i++) {
        const uint8_t m[] = {0xf0, 0x47, 0x7f, 0x5c, 0x65, 0x00, 0x04,
                             (uint8_t)i, 0, 0, 0, 0xf7};
        if (n + sizeof m > sizeof buf)
            break;
        memcpy(buf + n, m, sizeof m);
        n += sizeof m;
    }
    for (int cc = 57; cc <= 64; cc++) {
        buf[n++] = 0xb0;
        buf[n++] = (uint8_t)cc;
        buf[n++] = 0;
    }
    for (int cc = 83; cc <= 84; cc++) {
        buf[n++] = 0xb0;
        buf[n++] = (uint8_t)cc;
        buf[n++] = 0;
    }
    for (size_t i = 0; i < sizeof BUTTON_LED_CCS; i++) {
        buf[n++] = 0xb0;
        buf[n++] = BUTTON_LED_CCS[i];
        buf[n++] = 0;
    }
    (void)midi_write_all(fd, buf, n);
}

static void light_pad(int fd, int pad /* 0..15 */, uint8_t r, uint8_t g, uint8_t b)
{
    if (fd < 0 || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    /* sample-base HARDWARE.md: I = printed pad number − 1 (0..15). */
    uint8_t m[] = {0xf0, 0x47, 0x7f, 0x5c, 0x65, 0x00, 0x04,
                   (uint8_t)pad, (uint8_t)(r >> 1), (uint8_t)(g >> 1),
                   (uint8_t)(b >> 1), 0xf7};
    if (midi_write_all(fd, m, sizeof m) == 0)
        g_pad_lit[pad] = (r | g | b) ? 1 : 0;
}

/* Button LED: channel-1 CC = button note; 0 off, 1 dim, 2 full (HARDWARE.md). */
static void light_button(int fd, uint8_t note_cc, uint8_t level)
{
    if (fd < 0)
        return;
    uint8_t m[] = {0xb0, note_cc, level};
    (void)midi_write_all(fd, m, sizeof m);
}

static void refresh_pad_led(int pad)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    if (g_state.pad_vel[pad] > 0)
        light_pad(g_fd, pad, 40, 180, 160); /* playing */
    else if (pad == g_selected_pad)
        light_pad(g_fd, pad, 200, 140, 40); /* selected amber */
    else
        light_pad(g_fd, pad, 0, 0, 0);
}

/* Clear stuck play LEDs; keep selected-pad amber when idle. */
static void sync_pad_lights(int fd)
{
    if (fd < 0)
        return;
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        int want_lit = (g_state.pad_vel[i] > 0) || (i == g_selected_pad);
        if (g_pad_lit[i] && !want_lit)
            light_pad(fd, i, 0, 0, 0);
        else if (!g_pad_lit[i] && want_lit && g_state.pad_vel[i] == 0 && i == g_selected_pad)
            light_pad(fd, i, 200, 140, 40);
    }
}

static void midi_feed(uint8_t b)
{
    if (b >= 0xf8)
        return; /* realtime */
    if (b & 0x80) {
        if (b == 0xf0) {
            m_sysex = 1;
            /* Spec cancels running status; the Surface sometimes keeps
             * sending bare data bytes after our LED SysEx / 0x42 reply.
             * Keep note on/off status so pad note-offs are not dropped. */
            m_status = 0;
            return;
        }
        if (b == 0xf7) {
            m_sysex = 0;
            /* Prefer note voice status so AT/SysEx cannot orphan pad offs. */
            if (m_note_voice_status)
                m_status = m_note_voice_status;
            else if (m_voice_status)
                m_status = m_voice_status;
            return;
        }
        m_sysex = 0;
        if (b < 0xf0) {
            int cmd = b & 0xf0;
            m_status = b;
            if (cmd >= 0x80 && cmd <= 0xe0)
                m_voice_status = b;
            /* Note on/off only — poly/channel pressure must not become the
             * running status for subsequent bare pad note-offs (phantoms). */
            if (cmd == 0x80 || cmd == 0x90)
                m_note_voice_status = b;
            /* Program change / channel pressure: 1 data byte; else 2. */
            m_need = (cmd == 0xc0 || cmd == 0xd0) ? 1 : 2;
            m_n = 0;
        } else {
            m_status = 0;
        }
        return;
    }
    if (m_sysex)
        return;
    /* Bare data: prefer current status, else note status, else any voice. */
    if (!m_status) {
        int restore = m_note_voice_status ? m_note_voice_status : m_voice_status;
        if (!restore)
            return;
        m_status = restore;
        m_need = ((m_status & 0xf0) == 0xc0 || (m_status & 0xf0) == 0xd0) ? 1 : 2;
        m_n = 0;
    }
    m_d[m_n++] = b;
    if (m_n < m_need)
        return;
    m_n = 0;

    int ch = m_status & 0x0f;
    int cmd = m_status & 0xf0;

    /* After completing poly/channel pressure, snap running status back to
     * last note on/off so velocity/AT floods cannot retarget POS via fake CCs
     * or phantom pad note-ons under AT running status. */
    if (cmd == 0xa0 || cmd == 0xd0) {
        if (ch == 9 && cmd == 0xa0) {
            /* Poly AT on pads: ignore completely for play/params (Ash). */
            (void)pad_index(m_d[0]);
        }
        if (m_note_voice_status)
            m_status = m_note_voice_status;
        return;
    }

    if (cmd == 0xb0 && ch == 0) {
        switch (m_d[0]) {
        case 16:
            g_state.k1 = m_d[1];
            break;
        case 17:
            g_state.k2 = m_d[1];
            break;
        case 18:
            g_state.k3 = m_d[1];
            break;
        case 19: {
            uint8_t v = m_d[1];
            /* Debounce spurious main-vol zeros / single-sample glitches. */
            if (!g_state.main_seen) {
                g_state.main_seen = 1;
                g_state.main_vol = v;
                g_state.main_stable = v;
            } else if (v == 0 && g_state.main_stable > 8) {
                /* Ignore lone zero while hardware was clearly up. */
            } else {
                g_state.main_vol = v;
                g_state.main_stable = v;
            }
            break;
        }
        case 20: {
            uint8_t v = m_d[1];
            g_state.fader = v;
            if (!g_state.fader_seen) {
                g_state.fader_seen = 1;
                g_state.fader_dump = v;
                /* Do not engage — keep engine fader_gain at full until move. */
            } else if (!g_state.fader_engaged) {
                int d = (int)v - (int)g_state.fader_dump;
                if (d < 0)
                    d = -d;
                if (d >= 2)
                    g_state.fader_engaged = 1;
            }
            break;
        }
        case 100: /* relative wheel — ignore value 0 (0x42 trailer) */
            if (m_d[1] == 0)
                break;
            if (m_d[1] > 0 && m_d[1] < 64)
                g_state.wheel_accum += (int)m_d[1];
            else if (m_d[1] >= 64)
                g_state.wheel_accum += (int)m_d[1] - 128;
            break;
        default:
            break;
        }
        return;
    }

    if (ch == 0 && (cmd == 0x90 || cmd == 0x80)) {
        /* Channel 1 buttons.
         * Soft-left (12) / soft-mid (13) = page prev/next (main).
         * Soft-right (14) = quit edge only — main sets quit_requested after splash.
         * Wheel push (111) = press edge; never quits.
         * SAMPLE (6) held + pad = select without play (handled in main).
         * LOOP (74) = per-pad loop latch toggle. */
        int on = (cmd == 0x90 && m_d[1] > 0);
        if (m_d[0] == NOTE_SAMPLE) {
            g_state.sample_held = on ? 1 : 0;
            light_button(g_fd, NOTE_SAMPLE, on ? 2 : 0);
            return;
        }
        if (m_d[0] == NOTE_SHIFT) {
            g_state.shift_held = on ? 1 : 0;
            return;
        }
        if (on && m_d[0] == NOTE_LOOP) {
            g_state.loop_btn = 1;
            return;
        }
        if (on && m_d[0] == 111)
            g_state.wheel_press = 1;
        if (on && m_d[0] == 12)
            g_state.soft_left = 1;
        if (on && m_d[0] == 13)
            g_state.soft_mid = 1;
        if (on && m_d[0] == 14)
            g_state.soft_right = 1;
        (void)NOTE_MUTE; /* reserved — Pad Play mute later */
        return;
    }

    if (ch == 9 && (cmd == 0x90 || cmd == 0x80)) {
        int idx = pad_index(m_d[0]);
        if (idx < 0)
            return;
        if (cmd == 0x90 && m_d[1] > 0) {
            g_state.pad_vel[idx] = m_d[1];
            g_state.pad_down[idx] = 1; /* edge for note-on; app clears */
            refresh_pad_led(idx);
        } else {
            /* note-off (0x80) or note-on vel 0 */
            if (g_state.pad_vel[idx] > 0)
                g_state.pad_up[idx] = 1;
            g_state.pad_vel[idx] = 0;
            refresh_pad_led(idx);
        }
    }
}

int surface_open(void)
{
    memset(&g_state, 0, sizeof g_state);
    memset(g_pad_lit, 0, sizeof g_pad_lit);
    g_selected_pad = 0;
    g_loop_led_on = -1;
    /* Software full until fader is engaged; main starts full then tracks CC19. */
    g_state.main_vol = 127;
    g_state.main_stable = 127;
    g_state.fader = 127;
    g_state.fader_dump = 127;
    g_state.fader_engaged = 0;
    g_state.fader_seen = 0;
    g_state.main_seen = 0;
    g_fd = -1;
    m_status = m_need = m_n = m_sysex = 0;
    m_voice_status = 0;
    m_note_voice_status = 0;
    snprintf(g_info, sizeof g_info, "no Surface card");

#ifndef EPICYCLE_ON_DEVICE
    /* Host/CI: pretend open succeeded and auto-request quit so smoke exits. */
    g_state.open = 0;
    g_state.quit_requested = 1;
    snprintf(g_info, sizeof g_info, "host stub (auto-quit)");
    return -1;
#endif

    int card = epicycle_find_alsa_card("Surface");
    if (card < 0)
        return -1;

    char path[128];
    for (int dev = 0; dev < 8; dev++) {
        snprintf(path, sizeof path, "%s/midiC%dD%d", EPICYCLE_SND_DIR, card, dev);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        g_fd = fd;
        g_state.open = 1;
        snprintf(g_info, sizeof g_info, "Surface #%d D%d", card, dev);

        surface_lights_off(fd);
        static const uint8_t req[] = {0xf0, 0x47, 0x7f, 0x5c, 0x42, 0x00, 0x00, 0xf7};
        (void)midi_write_all(fd, req, sizeof req);
        return 0;
    }

    snprintf(g_info, sizeof g_info, "Surface midi open failed");
    return -1;
}

void surface_close(void)
{
    if (g_fd >= 0) {
        surface_lights_off(g_fd);
        close(g_fd);
        g_fd = -1;
    }
    g_state.open = 0;
    snprintf(g_info, sizeof g_info, "surface closed");
}

void surface_poll(void)
{
    if (g_fd < 0)
        return;
    uint8_t buf[256];
    for (;;) {
        ssize_t n = read(g_fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            break;
        }
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++)
            midi_feed(buf[i]);
    }
    /* Reconcile stuck pad LEDs after any missed note-off. */
    sync_pad_lights(g_fd);
}

int surface_poll_quit(void)
{
    return g_state.quit_requested;
}

const EpicycleSurfaceState *surface_state(void)
{
    return &g_state;
}

EpicycleSurfaceState *surface_state_mut(void)
{
    return &g_state;
}

const char *surface_info(void)
{
    return g_info;
}

void surface_set_selected_pad(int pad)
{
    if (pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return;
    if (pad == g_selected_pad)
        return;
    int prev = g_selected_pad;
    g_selected_pad = pad;
    refresh_pad_led(prev);
    refresh_pad_led(pad);
}

void surface_set_loop_led(int on)
{
    int want = on ? 2 : 0;
    if (want == g_loop_led_on)
        return;
    g_loop_led_on = want;
    light_button(g_fd, NOTE_LOOP, (uint8_t)want);
}
