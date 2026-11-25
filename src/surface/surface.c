/* SPDX-License-Identifier: MIT
 *
 * Control-surface MIDI (UART3 raw MIDI / ALSA card "Surface").
 *
 * Protocol facts (SampleBoot TECHNICAL.md, sample-base HARDWARE.md, community
 * mpc-sample-lab control-surface-protocol — MIT rewrite, no AGPL vendoring):
 *   - MCU emits 3-byte messages with status (no running status from hardware);
 *     still parse running status for fragmented ALSA reads / AT floods.
 *   - pads ch10: notes 37,36,42,82,40,38,46,44,48,47,45,43,49,55,51,53
 *     strike 99 nn vv → poly AT A9 nn pp → release 89 nn 00
 *   - pad LED SysEx I = printed pad − 1 (0..15); fader light I=16
 *   - Main vol CC19; K1–K3 CC16/17/18; fader CC20; wheel CC100 relative
 *   - SAMPLE=6, SHIFT=49, LOOP=74, MUTE=43; soft B1/B2/B3 = 12/13/14
 *   - 0x42 position request → dump CC19,16,17,18,20 + CC100=0
 *
 * Untrusted-stream rules (0.2.8 rewrite after 0.2.7 still false-quit / phantoms):
 *   - Prefer drop junk over acting on it.
 *   - SysEx cancels running status; never restore.
 *   - AT/pressure/pitch-bend/program: consume & ignore (no snap to notes).
 *   - Soft keys need an explicit status byte (no running-status soft keys),
 *     clean note-off between presses, hold timeout to unstick; B3 quit fires
 *     on release after a minimum hold (not a lone phantom note-on).
 *   - Pad LEDs only from real note-on/off (never AT).
 *   - Volume CC path never lights buttons or pages.
 *   - On open: lights off, 0x42, drain RX so dump cannot arm soft keys.
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

/* Printed pad 1..16 → channel-10 note (public measurement). */
static const uint8_t PAD_NOTES[EPICYCLE_PAD_COUNT] = {
    37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53};

static const uint8_t BUTTON_LED_CCS[] = {
    6, 7, 1, 0, 35, 9, 11, 75, 43, 74, 40, 36, 53, 73, 80, 81, 82};

enum {
    NOTE_SAMPLE = 6,
    NOTE_SHIFT = 49,
    NOTE_LOOP = 74,
    NOTE_MUTE = 43,
    NOTE_SOFT0 = 12,
    NOTE_SOFT1 = 13,
    NOTE_SOFT2 = 14,
};

static int g_fd = -1;
static EpicycleSurfaceState g_state;
static char g_info[64] = "surface closed";
static int g_selected_pad;
static int g_loop_led_on = -1;

/* MIDI parser — untrusted byte stream */
static int m_status;        /* running status or 0 */
static int m_need;          /* data bytes expected */
static int m_n;             /* data bytes collected */
static int m_sysex;         /* inside SysEx (drop payload) */
static int m_fresh_status;  /* 1 if status byte opened this message */
static uint8_t m_d[2];
static uint8_t g_pad_lit[EPICYCLE_PAD_COUNT];

/*
 * Soft keys: track down + hold time. B1/B2 fire on release after short hold;
 * B3 quit fires on release after longer hold. Held latch times out so a missed
 * note-off cannot permanently stick paging.
 */
static uint8_t g_soft_down[3];
static double g_soft_down_t[3];
static double g_soft_last_fire_t[3];
#define SOFT_PAGE_HOLD_S 0.025
#define SOFT_QUIT_HOLD_S 0.120
#define SOFT_STUCK_S 1.50
#define SOFT_PAGE_GAP_S 0.080

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
    uint8_t m[] = {0xf0, 0x47, 0x7f, 0x5c, 0x65, 0x00, 0x04,
                   (uint8_t)pad, (uint8_t)(r >> 1), (uint8_t)(g >> 1),
                   (uint8_t)(b >> 1), 0xf7};
    if (midi_write_all(fd, m, sizeof m) == 0)
        g_pad_lit[pad] = (r | g | b) ? 1 : 0;
}

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
        light_pad(g_fd, pad, 40, 180, 160);
    else if (pad == g_selected_pad)
        light_pad(g_fd, pad, 200, 140, 40);
    else
        light_pad(g_fd, pad, 0, 0, 0);
}

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

static void soft_clear_stuck(void)
{
    double t = now_s();
    for (int i = 0; i < 3; i++) {
        if (g_soft_down[i] && (t - g_soft_down_t[i]) > SOFT_STUCK_S) {
            g_soft_down[i] = 0;
            g_soft_down_t[i] = 0;
        }
    }
}

/* Soft key: press tracks down; action on release after min hold. */
static void soft_key_msg(int which /* 0,1,2 */, int on, int explicit_status)
{
    if (which < 0 || which > 2)
        return;
    /* Soft keys never accept running-status continuations — orphans after
     * SysEx / AT often look like (12|13|14, vel) under restored 0x90. */
    if (!explicit_status)
        return;

    double t = now_s();
    if (on) {
        if (g_soft_down[which])
            return;
        g_soft_down[which] = 1;
        g_soft_down_t[which] = t;
        return;
    }

    /* note-off */
    if (!g_soft_down[which])
        return;
    double held = t - g_soft_down_t[which];
    g_soft_down[which] = 0;
    g_soft_down_t[which] = 0;

    double need = (which == 2) ? SOFT_QUIT_HOLD_S : SOFT_PAGE_HOLD_S;
    if (held < need || held > SOFT_STUCK_S)
        return;
    if ((t - g_soft_last_fire_t[which]) < SOFT_PAGE_GAP_S)
        return;
    g_soft_last_fire_t[which] = t;

    if (which == 0)
        g_state.soft_left = 1;
    else if (which == 1)
        g_state.soft_mid = 1;
    else
        g_state.soft_right = 1;
}

static void apply_main_vol_cc(uint8_t v)
{
    /* Isolated from buttons/pages. Latch like fader until dump settles. */
    if (!g_state.main_seen) {
        g_state.main_seen = 1;
        g_state.main_dump = v;
        /* Keep software loud if dump looks like a glitch zero. */
        if (v > 8) {
            g_state.main_vol = v;
            g_state.main_stable = v;
        }
        return;
    }
    if (!g_state.main_engaged) {
        int d = (int)v - (int)g_state.main_dump;
        if (d < 0)
            d = -d;
        if (d < 2)
            return;
        g_state.main_engaged = 1;
    }
    /* Reject lone zero while clearly up (cable/dump glitch). */
    if (v == 0 && g_state.main_stable > 8)
        return;
    g_state.main_vol = v;
    g_state.main_stable = v;
}

static void apply_fader_cc(uint8_t v)
{
    g_state.fader = v;
    if (!g_state.fader_seen) {
        g_state.fader_seen = 1;
        g_state.fader_dump = v;
        return;
    }
    if (!g_state.fader_engaged) {
        int d = (int)v - (int)g_state.fader_dump;
        if (d < 0)
            d = -d;
        if (d >= 2)
            g_state.fader_engaged = 1;
    }
}

static void dispatch_msg(int explicit_status)
{
    int ch = m_status & 0x0f;
    int cmd = m_status & 0xf0;

    /* Aftertouch / channel pressure: ignore; keep status so AT floods stay AT. */
    if (cmd == 0xa0 || cmd == 0xd0)
        return;
    if (cmd == 0xe0 || cmd == 0xc0)
        return;

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
        case 19:
            apply_main_vol_cc(m_d[1]);
            break;
        case 20:
            apply_fader_cc(m_d[1]);
            break;
        case 100:
            if (m_d[1] == 0)
                break; /* 0x42 trailer */
            if (m_d[1] > 0 && m_d[1] < 64)
                g_state.wheel_accum += (int)m_d[1];
            else if (m_d[1] >= 64)
                g_state.wheel_accum += (int)m_d[1] - 128;
            break;
        default:
            break; /* unknown CC — ignore (never treat as notes/LEDs) */
        }
        return;
    }

    if (ch == 0 && (cmd == 0x90 || cmd == 0x80)) {
        int on = (cmd == 0x90 && m_d[1] > 0);
        /* Note-on vel 0 = note-off */
        if (cmd == 0x90 && m_d[1] == 0)
            on = 0;

        if (m_d[0] == NOTE_SAMPLE) {
            if (!explicit_status && on)
                return; /* no running-status SAMPLE */
            g_state.sample_held = on ? 1 : 0;
            light_button(g_fd, NOTE_SAMPLE, on ? 2 : 0);
            return;
        }
        if (m_d[0] == NOTE_SHIFT) {
            g_state.shift_held = on ? 1 : 0;
            return;
        }
        if (m_d[0] == NOTE_LOOP) {
            if (on && explicit_status)
                g_state.loop_btn = 1;
            return;
        }
        if (m_d[0] == 111) {
            if (on && explicit_status)
                g_state.wheel_press = 1;
            return;
        }
        if (m_d[0] == NOTE_SOFT0 || m_d[0] == NOTE_SOFT1 || m_d[0] == NOTE_SOFT2) {
            soft_key_msg((int)m_d[0] - NOTE_SOFT0, on, explicit_status);
            return;
        }
        (void)NOTE_MUTE;
        return;
    }

    if (ch == 9 && (cmd == 0x90 || cmd == 0x80)) {
        int idx = pad_index(m_d[0]);
        if (idx < 0)
            return;
        if (cmd == 0x90 && m_d[1] > 0) {
            g_state.pad_vel[idx] = m_d[1];
            g_state.pad_down[idx] = 1;
            refresh_pad_led(idx); /* LED only on real note-on */
        } else {
            if (g_state.pad_vel[idx] > 0)
                g_state.pad_up[idx] = 1;
            g_state.pad_vel[idx] = 0;
            refresh_pad_led(idx); /* LED only on real note-off */
        }
        return;
    }
}

static void midi_feed(uint8_t b)
{
    if (b >= 0xf8)
        return; /* realtime */

    if (b & 0x80) {
        if (b == 0xf0) {
            m_sysex = 1;
            m_status = 0;
            m_n = 0;
            m_need = 0;
            m_fresh_status = 0;
            return;
        }
        if (b == 0xf7) {
            m_sysex = 0;
            m_status = 0;
            m_n = 0;
            m_need = 0;
            m_fresh_status = 0;
            return;
        }
        m_sysex = 0;
        if (b < 0xf0) {
            int cmd = b & 0xf0;
            m_status = b;
            m_need = (cmd == 0xc0 || cmd == 0xd0) ? 1 : 2;
            m_n = 0;
            m_fresh_status = 1;
        } else {
            m_status = 0;
            m_n = 0;
            m_need = 0;
            m_fresh_status = 0;
        }
        return;
    }

    if (m_sysex)
        return;

    if (!m_status)
        return; /* bare data, no running status — drop */

    m_d[m_n++] = b;
    if (m_n < m_need)
        return;

    int explicit = m_fresh_status;
    m_n = 0;
    m_fresh_status = 0; /* subsequent running-status msgs are not explicit */
    dispatch_msg(explicit);
}

static void midi_drain(int fd, double seconds)
{
    double until = now_s() + seconds;
    uint8_t buf[256];
    while (now_s() < until) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++)
                midi_feed(buf[i]);
            continue;
        }
        struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
        poll(&p, 1, 10);
    }
    /* Soft keys must not arm from the 0x42 dump / LED echo. */
    g_state.soft_left = g_state.soft_mid = g_state.soft_right = 0;
    memset(g_soft_down, 0, sizeof g_soft_down);
}

int surface_open(void)
{
    memset(&g_state, 0, sizeof g_state);
    memset(g_pad_lit, 0, sizeof g_pad_lit);
    memset(g_soft_down, 0, sizeof g_soft_down);
    memset(g_soft_down_t, 0, sizeof g_soft_down_t);
    memset(g_soft_last_fire_t, 0, sizeof g_soft_last_fire_t);
    g_selected_pad = 0;
    g_loop_led_on = -1;
    g_state.main_vol = 127;
    g_state.main_stable = 127;
    g_state.main_dump = 127;
    g_state.main_engaged = 0;
    g_state.main_seen = 0;
    g_state.fader = 127;
    g_state.fader_dump = 127;
    g_state.fader_engaged = 0;
    g_state.fader_seen = 0;
    g_fd = -1;
    m_status = m_need = m_n = m_sysex = m_fresh_status = 0;
    snprintf(g_info, sizeof g_info, "no Surface card");

#ifndef EPICYCLE_ON_DEVICE
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
        /* Absorb 0x42 dump + any LED write echoes before the app arms UI. */
        midi_drain(fd, 0.08);
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
    soft_clear_stuck();
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

static void midi_selftest_reset(void)
{
    memset(&g_state, 0, sizeof g_state);
    memset(g_soft_down, 0, sizeof g_soft_down);
    memset(g_soft_down_t, 0, sizeof g_soft_down_t);
    memset(g_soft_last_fire_t, 0, sizeof g_soft_last_fire_t);
    m_status = m_need = m_n = m_sysex = m_fresh_status = 0;
    g_state.main_vol = 127;
    g_state.main_stable = 127;
    g_state.fader = 127;
}

static void midi_feed_buf(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        midi_feed(b[i]);
}

int surface_midi_selftest(void)
{
    int fails = 0;

    /* 1) Pad note-on + poly AT flood must NOT create other pad triggers. */
    midi_selftest_reset();
    {
        const uint8_t seq[] = {
            0x99, 37, 100, /* pad 1 note-on */
            0xa9, 37, 80,  /* poly AT */
            37, 60,        /* AT running */
            37, 40,
            36, 90, /* AT on pad2 note — must NOT note-on pad 2 */
            0x89, 37, 0,
        };
        midi_feed_buf(seq, sizeof seq);
        if (g_state.pad_vel[0] != 0)
            fails++;
        if (g_state.pad_down[1] || g_state.pad_vel[1])
            fails++;
        if (!g_state.pad_down[0] && !g_state.pad_up[0])
            fails++;
    }

    /* 2) SysEx then orphan data must NOT become soft keys / quit. */
    midi_selftest_reset();
    {
        const uint8_t seq[] = {
            0x90, 6, 127, 0x80, 6, 0,
            0xf0, 0x47, 0x7f, 0x5c, 0x42, 0x00, 0x00, 0xf7,
            12, 100, 13, 100, 14, 100,
        };
        midi_feed_buf(seq, sizeof seq);
        if (g_state.soft_left || g_state.soft_mid || g_state.soft_right)
            fails++;
        if (g_state.sample_held)
            fails++;
    }

    /* 3) Soft keys fire on release after hold; running-status soft key ignored. */
    midi_selftest_reset();
    {
        /* Too-short press — no fire */
        const uint8_t shortp[] = {0x90, 12, 127, 0x80, 12, 0};
        midi_feed_buf(shortp, sizeof shortp);
        if (g_state.soft_left)
            fails++;

        /* Simulate held press: inject down, advance clock via stuck clear path
         * by feeding with sleep — use direct state for unit timing instead:
         * call soft path with artificial times via longer sequence not possible
         * without sleep. Sleep briefly for real hold. */
    }
    midi_selftest_reset();
    {
        const uint8_t down[] = {0x90, 12, 127};
        midi_feed_buf(down, sizeof down);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 40 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        const uint8_t up[] = {0x80, 12, 0};
        midi_feed_buf(up, sizeof up);
        if (!g_state.soft_left)
            fails++;
        g_state.soft_left = 0;

        /* Running-status continuation must NOT arm soft key */
        const uint8_t run[] = {13, 127}; /* would be soft-mid under 0x90 */
        midi_feed_buf(run, sizeof run);
        struct timespec ts2 = {.tv_sec = 0, .tv_nsec = 40 * 1000 * 1000L};
        nanosleep(&ts2, NULL);
        const uint8_t runoff[] = {0x80, 13, 0};
        midi_feed_buf(runoff, sizeof runoff);
        if (g_state.soft_mid)
            fails++;
    }

    /* 4) B3 quit needs longer hold; brief press must not quit. */
    midi_selftest_reset();
    {
        const uint8_t down[] = {0x90, 14, 127};
        midi_feed_buf(down, sizeof down);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 40 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        const uint8_t up[] = {0x80, 14, 0};
        midi_feed_buf(up, sizeof up);
        if (g_state.soft_right)
            fails++; /* 40ms < 120ms quit hold */
    }
    midi_selftest_reset();
    {
        const uint8_t down[] = {0x90, 14, 127};
        midi_feed_buf(down, sizeof down);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 140 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        const uint8_t up[] = {0x80, 14, 0};
        midi_feed_buf(up, sizeof up);
        if (!g_state.soft_right)
            fails++;
    }

    /* 5) Volume CC isolated — does not set soft keys / SAMPLE. */
    midi_selftest_reset();
    {
        const uint8_t seq[] = {0xb0, 19, 40, 0xb0, 19, 80, 0xb0, 16, 10};
        midi_feed_buf(seq, sizeof seq);
        if (g_state.soft_left || g_state.soft_mid || g_state.soft_right)
            fails++;
        if (g_state.sample_held)
            fails++;
        if (g_state.k1 != 10)
            fails++;
        /* First CC19 is dump; second engages */
        if (!g_state.main_engaged || g_state.main_vol != 80)
            fails++;
    }

    /* 6) Unknown messages dropped; known knobs update. */
    midi_selftest_reset();
    {
        const uint8_t seq[] = {0xb0, 16, 40, 0xb0, 99, 50, 0xb0, 17, 80, 0xe0, 0, 64};
        midi_feed_buf(seq, sizeof seq);
        if (g_state.k1 != 40 || g_state.k2 != 80)
            fails++;
    }

    return fails ? -1 : 0;
}
