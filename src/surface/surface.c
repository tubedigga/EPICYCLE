/* SPDX-License-Identifier: MIT
 *
 * Control-surface MIDI (UART3 raw MIDI / ALSA card "Surface").
 * Published map facts from SampleBoot TECHNICAL.md and sample-base HARDWARE.md
 * (do not reverse stock for these; do not vendor AGPL headers):
 *   - pads: channel 10, notes 37,36,42,82,40,38,46,44,48,47,45,43,49,55,51,53
 *   - knobs K1–K3: CC 16/17/18; main volume CC 19; fader CC 20 (channel 1)
 *   - wheel: CC 100 relative; wheel push ≈ note 111
 * Soft keys above the screen: notes 12/13/14 — right soft key also quits.
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

static int g_fd = -1;
static EpicycleSurfaceState g_state;
static char g_info[64] = "surface closed";

/* MIDI parser */
static int m_status, m_need, m_n, m_sysex;
static uint8_t m_d[2];

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
    /* Pad index in SysEx is 1..16. */
    uint8_t m[] = {0xf0, 0x47, 0x7f, 0x5c, 0x65, 0x00, 0x04,
                   (uint8_t)(pad + 1), (uint8_t)(r >> 1), (uint8_t)(g >> 1),
                   (uint8_t)(b >> 1), 0xf7};
    (void)midi_write_all(fd, m, sizeof m);
}

static void midi_feed(uint8_t b)
{
    if (b >= 0xf8)
        return; /* realtime */
    if (b & 0x80) {
        if (b == 0xf0) {
            m_sysex = 1;
            m_status = 0;
            return;
        }
        if (b == 0xf7) {
            m_sysex = 0;
            return;
        }
        m_sysex = 0;
        if (b < 0xf0) {
            m_status = b;
            /* Program change / channel pressure: 1 data byte; else 2. */
            m_need = ((b & 0xf0) == 0xc0 || (b & 0xf0) == 0xd0) ? 1 : 2;
            m_n = 0;
        } else {
            m_status = 0;
        }
        return;
    }
    if (m_sysex || !m_status)
        return;
    m_d[m_n++] = b;
    if (m_n < m_need)
        return;
    m_n = 0;

    int ch = m_status & 0x0f;
    int cmd = m_status & 0xf0;

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
            g_state.main_vol = m_d[1];
            break;
        case 20:
            g_state.fader = m_d[1];
            break;
        case 100: /* relative wheel */
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
        /* Channel 1 buttons. Wheel push = note 111; soft-right = 14.
         * Soft-left (12) = toggle free/division; soft-mid (13) = toggle LFO scrub. */
        int on = (cmd == 0x90 && m_d[1] > 0);
        if (on && (m_d[0] == 111 || m_d[0] == 14))
            g_state.quit_requested = 1;
        if (on && m_d[0] == 12)
            g_state.soft_left = 1;
        if (on && m_d[0] == 13)
            g_state.soft_mid = 1;
        return;
    }

    if (ch == 9 && (cmd == 0x90 || cmd == 0x80 || cmd == 0xa0)) {
        int idx = pad_index(m_d[0]);
        if (idx < 0)
            return;
        if (cmd == 0xa0) {
            /* polyphonic pressure — keep lit while > 0 */
            if (m_d[1] == 0 && g_state.pad_vel[idx]) {
                /* pressure zero while held is common; leave velocity */
            }
            return;
        }
        if (cmd == 0x90 && m_d[1] > 0) {
            g_state.pad_vel[idx] = m_d[1];
            g_state.pad_down[idx] = 1; /* edge for note-on; app clears */
            light_pad(g_fd, idx, 40, 180, 160);
        } else {
            g_state.pad_vel[idx] = 0;
            light_pad(g_fd, idx, 0, 0, 0);
        }
    }
}

int surface_open(void)
{
    memset(&g_state, 0, sizeof g_state);
    g_state.main_vol = 100;
    g_fd = -1;
    m_status = m_need = m_n = m_sysex = 0;
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
