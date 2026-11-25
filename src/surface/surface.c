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
 * Untrusted-stream rules (0.2.8+; 0.2.11 hard-isolates soft keys / SAMPLE):
 *   - Prefer drop junk over acting on it.
 *   - SysEx cancels running status; never restore.
 *   - AT/pressure/pitch-bend/program: consume & ignore (no snap to notes).
 *   - Soft keys need an explicit status byte (no running-status soft keys).
 *   - HARD isolate: B1/B2/B3 fire only on CLEAN RELEASE after a short hold;
 *     any pad / CC / other-button activity during the hold cancels the arm.
 *     Soft keys also suppressed while controls or pads were recently active.
 *   - Pad note-ons need explicit status + vel≥8; status cleared after pads.
 *   - SAMPLE LED tracks sample_held; forced off after open drain (no boot flash).
 *   - Volume CC path never lights buttons or pages.
 *   - On open: lights off, 0x42, drain RX so dump cannot arm soft keys / SAMPLE.
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
 * Soft keys: hard-isolated from pads/knobs.
 * Arm on explicit press; fire on clean release after a short hold.
 * Any non-soft MIDI during the hold cancels the arm (Ash: phantom pages).
 * B3 quit needs a longer hold. Stuck latch times out on missed note-off.
 */
static uint8_t g_soft_down[3];
static double g_soft_down_t[3];
static double g_soft_last_fire_t[3];
#define SOFT_PAGE_HOLD_S 0.035
#define SOFT_QUIT_HOLD_S 0.120
#define SOFT_STUCK_S 1.50
#define SOFT_PAGE_GAP_S 0.100
/* Knobs/encoder/pads must never page — suppress soft keys after bus activity. */
static double g_ctrl_activity_t;
static double g_pad_activity_t;
#define CTRL_PAGE_GUARD_S 0.350
#define PAD_PAGE_GUARD_S 0.400
/* Shift / SAMPLE momentary; auto-clear if note-off was lost. */
static double g_shift_down_t;
static double g_sample_down_t;
static int g_sample_led = -1;
static int g_open_quiet; /* 1 while draining boot dump — no SAMPLE LED flash */
#define SHIFT_STUCK_S 3.00
#define SAMPLE_STUCK_S 8.00

static uint8_t g_main_pending;
static int g_main_pending_n;
static uint8_t g_fader_pending;
static int g_fader_pending_n;

static void light_button(int fd, uint8_t note_cc, uint8_t level);

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void mark_ctrl_activity(void)
{
    g_ctrl_activity_t = now_s();
    /* Cancel any soft-key hold in progress — knobs must never finish a page. */
    for (int i = 0; i < 3; i++) {
        g_soft_down[i] = 0;
        g_soft_down_t[i] = 0;
    }
}

static void mark_pad_activity(void)
{
    g_pad_activity_t = now_s();
    for (int i = 0; i < 3; i++) {
        g_soft_down[i] = 0;
        g_soft_down_t[i] = 0;
    }
}

static int bus_blocks_pages(void)
{
    double t = now_s();
    if (g_ctrl_activity_t > 0.0 && (t - g_ctrl_activity_t) < CTRL_PAGE_GUARD_S)
        return 1;
    if (g_pad_activity_t > 0.0 && (t - g_pad_activity_t) < PAD_PAGE_GUARD_S)
        return 1;
    return 0;
}

static void sample_led_set(int on)
{
    int want = on ? 2 : 0;
    if (want == g_sample_led)
        return;
    g_sample_led = want;
    if (!g_open_quiet)
        light_button(g_fd, NOTE_SAMPLE, (uint8_t)want);
}

static void clear_ch_running(void)
{
    m_status = 0;
    m_need = 0;
    m_n = 0;
    m_fresh_status = 0;
}

static int pad_index(uint8_t note)
{
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        if (PAD_NOTES[i] == note)
            return i;
    }
    return -1;
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
    /* Missed Shift note-off → Trim stuck on xfade/dir until next press. */
    if (g_state.shift_held && g_shift_down_t > 0.0 && (t - g_shift_down_t) > SHIFT_STUCK_S) {
        g_state.shift_held = 0;
        g_shift_down_t = 0.0;
    }
    /* Missed SAMPLE note-off → LED sticky; clear after timeout. */
    if (g_state.sample_held && g_sample_down_t > 0.0 &&
        (t - g_sample_down_t) > SAMPLE_STUCK_S) {
        g_state.sample_held = 0;
        g_sample_down_t = 0.0;
        sample_led_set(0);
    }
}

/* Soft key: arm on press; fire on clean release after hold. Hard-isolated. */
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
        if ((t - g_soft_last_fire_t[which]) < SOFT_PAGE_GAP_S)
            return;
        /* HARD: while knobs/pads are active, ignore soft-key arms entirely. */
        if (bus_blocks_pages())
            return;
        g_soft_down[which] = 1;
        g_soft_down_t[which] = t;
        return;
    }

    /* note-off — fire only if still armed (not cancelled mid-hold). */
    if (!g_soft_down[which])
        return;
    double held = t - g_soft_down_t[which];
    g_soft_down[which] = 0;
    g_soft_down_t[which] = 0;

    if (held > SOFT_STUCK_S)
        return;
    if ((t - g_soft_last_fire_t[which]) < SOFT_PAGE_GAP_S)
        return;
    /* Re-check bus: a late pad/CC must not let a release finish the page. */
    if (bus_blocks_pages())
        return;

    if (which == 0 || which == 1) {
        if (held < SOFT_PAGE_HOLD_S)
            return;
        g_soft_last_fire_t[which] = t;
        if (which == 0)
            g_state.soft_left = 1;
        else
            g_state.soft_mid = 1;
        g_state.shift_held = 0;
        g_shift_down_t = 0.0;
        return;
    }

    /* B3 quit: longer hold. */
    if (held < SOFT_QUIT_HOLD_S)
        return;
    g_soft_last_fire_t[2] = t;
    g_state.soft_right = 1;
}

static int confirm_drop(uint8_t *pending, int *pending_n, uint8_t v, int need)
{
    if (*pending_n > 0 && *pending == v) {
        (*pending_n)++;
        return *pending_n >= need;
    }
    *pending = v;
    *pending_n = 1;
    return 0;
}

static void apply_main_vol_cc(uint8_t v)
{
    /* CC19 → gain only. Never touches pads/soft keys/Trim.
     * Follow hardware from the 0x42 dump (including 0 = silence). Do not
     * leave software louder than the knob (Ash: audio with knob at zero).
     * Large single-sample jumps need one confirm (cable glitch), then apply. */
    if (!g_state.main_seen) {
        g_state.main_seen = 1;
        g_state.main_dump = v;
        g_state.main_vol = v; /* including 0 */
        g_state.main_stable = v;
        g_state.main_engaged = 1;
        g_main_pending_n = 0;
        return;
    }
    g_state.main_engaged = 1;
    /* Confirm large drops only (glitch mute). Ups / small moves apply now so
     * restoring level stays responsive without voice side effects. */
    int drop = (int)g_state.main_stable - (int)v;
    if (drop > 16) {
        if (!confirm_drop(&g_main_pending, &g_main_pending_n, v, 2))
            return;
    }
    g_main_pending_n = 0;
    g_state.main_vol = v;
    g_state.main_stable = v;
}

static void apply_fader_cc(uint8_t v)
{
    /* Engine stays full until engaged. Dump of 0 + noise must not arm silence. */
    if (!g_state.fader_seen) {
        g_state.fader_seen = 1;
        g_state.fader_dump = v;
        g_state.fader = v;
        return;
    }
    if (!g_state.fader_engaged) {
        int d = (int)v - (int)g_state.fader_dump;
        if (d < 0)
            d = -d;
        if (d < 3)
            return;
        /* Refuse to engage at near-silence (Ash: need fader again after drop). */
        if (v < 16)
            return;
        g_state.fader_engaged = 1;
        g_state.fader = v;
        g_fader_pending_n = 0;
        return;
    }
    if (v <= 8 && g_state.fader > 24) {
        if (!confirm_drop(&g_fader_pending, &g_fader_pending_n, v, 2))
            return;
    } else {
        int drop = (int)g_state.fader - (int)v;
        if (drop > 16) {
            if (!confirm_drop(&g_fader_pending, &g_fader_pending_n, v, 2))
                return;
        }
    }
    g_fader_pending_n = 0;
    g_state.fader = v;
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
            mark_ctrl_activity();
            break;
        case 17:
            g_state.k2 = m_d[1];
            mark_ctrl_activity();
            break;
        case 18:
            g_state.k3 = m_d[1];
            mark_ctrl_activity();
            break;
        case 19:
            apply_main_vol_cc(m_d[1]);
            mark_ctrl_activity();
            break;
        case 20:
            apply_fader_cc(m_d[1]);
            mark_ctrl_activity();
            break;
        case 100:
            if (m_d[1] == 0)
                break; /* 0x42 trailer */
            if (m_d[1] > 0 && m_d[1] < 64)
                g_state.wheel_accum += (int)m_d[1];
            else if (m_d[1] >= 64)
                g_state.wheel_accum += (int)m_d[1] - 128;
            mark_ctrl_activity();
            break;
        default:
            break; /* unknown CC — ignore (never treat as notes/LEDs) */
        }
        /* Knobs/wheel never leave note running-status that could fake pages. */
        clear_ch_running();
        return;
    }

    if (ch == 0 && (cmd == 0x90 || cmd == 0x80)) {
        int on = (cmd == 0x90 && m_d[1] > 0);
        /* Note-on vel 0 = note-off */
        if (cmd == 0x90 && m_d[1] == 0)
            on = 0;

        if (m_d[0] == NOTE_SAMPLE) {
            if (!explicit_status && on) {
                clear_ch_running();
                return; /* no running-status SAMPLE */
            }
            g_state.sample_held = on ? 1 : 0;
            if (on) {
                g_sample_down_t = now_s();
                /* SAMPLE edge clears sticky Shift (momentary modifier). */
                g_state.shift_held = 0;
                g_shift_down_t = 0.0;
            } else {
                g_sample_down_t = 0.0;
            }
            sample_led_set(on);
            clear_ch_running();
            return;
        }
        if (m_d[0] == NOTE_SHIFT) {
            /* Momentary: hold = on, release = off. Never latch/toggle.
             * Note-on needs explicit status; note-off always clears (even
             * running-status) so a lost status cannot stick the Trim overlay. */
            if (on) {
                if (!explicit_status) {
                    clear_ch_running();
                    return;
                }
                g_state.shift_held = 1;
                g_shift_down_t = now_s();
            } else {
                g_state.shift_held = 0;
                g_shift_down_t = 0.0;
            }
            clear_ch_running();
            return;
        }
        if (m_d[0] == NOTE_LOOP) {
            if (on && explicit_status)
                g_state.loop_btn = 1;
            clear_ch_running();
            return;
        }
        if (m_d[0] == 111) {
            if (on && explicit_status) {
                g_state.wheel_press = 1;
                mark_ctrl_activity(); /* encoder push never pages */
            }
            clear_ch_running();
            return;
        }
        if (m_d[0] == NOTE_SOFT0 || m_d[0] == NOTE_SOFT1 || m_d[0] == NOTE_SOFT2) {
            soft_key_msg((int)m_d[0] - NOTE_SOFT0, on, explicit_status);
            /* Kill running status so soft-key data cannot chain into pads. */
            clear_ch_running();
            return;
        }
        (void)NOTE_MUTE;
        /* Unknown ch1 note — still clear running status (no phantom chain). */
        clear_ch_running();
        return;
    }

    if (ch == 9 && (cmd == 0x90 || cmd == 0x80)) {
        int idx = pad_index(m_d[0]);
        if (idx < 0) {
            clear_ch_running();
            return;
        }
        if (cmd == 0x90 && m_d[1] > 0) {
            /* Explicit status required: a lost AT status under leftover 0x99
             * turns (nn,pp) into phantom pad note-ons (Ash: random pads). */
            if (!explicit_status) {
                clear_ch_running();
                return;
            }
            if (m_d[1] < 8) {
                clear_ch_running();
                return; /* ignore low-velocity phantoms / AT bleed */
            }
            g_state.pad_vel[idx] = m_d[1];
            g_state.pad_down[idx] = 1;
            mark_pad_activity();
            refresh_pad_led(idx); /* LED only on real note-on */
        } else {
            if (!explicit_status && g_state.pad_vel[idx] == 0) {
                clear_ch_running();
                return;
            }
            if (g_state.pad_vel[idx] > 0)
                g_state.pad_up[idx] = 1;
            g_state.pad_vel[idx] = 0;
            refresh_pad_led(idx); /* LED only on real note-off */
        }
        /* Kill running status so orphan AT data cannot become pad notes. */
        clear_ch_running();
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
    /* Soft keys / SAMPLE must not arm from the 0x42 dump / LED echo. */
    g_state.soft_left = g_state.soft_mid = g_state.soft_right = 0;
    memset(g_soft_down, 0, sizeof g_soft_down);
    g_state.sample_held = 0;
    g_sample_down_t = 0.0;
    g_state.shift_held = 0;
    g_shift_down_t = 0.0;
    clear_ch_running();
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
    g_main_pending_n = 0;
    g_fader_pending_n = 0;
    g_shift_down_t = 0.0;
    g_sample_down_t = 0.0;
    g_sample_led = -1;
    g_ctrl_activity_t = 0.0;
    g_pad_activity_t = 0.0;
    g_open_quiet = 0;
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

        g_open_quiet = 1;
        surface_lights_off(fd);
        static const uint8_t req[] = {0xf0, 0x47, 0x7f, 0x5c, 0x42, 0x00, 0x00, 0xf7};
        (void)midi_write_all(fd, req, sizeof req);
        /* Absorb 0x42 dump + any LED write echoes before the app arms UI. */
        midi_drain(fd, 0.08);
        g_open_quiet = 0;
        /* Force SAMPLE LED dark + state clear — no boot lit→unlit flash. */
        g_state.sample_held = 0;
        g_sample_down_t = 0.0;
        g_sample_led = -1;
        sample_led_set(0);
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
    g_main_pending_n = 0;
    g_fader_pending_n = 0;
    g_shift_down_t = 0.0;
    g_sample_down_t = 0.0;
    g_sample_led = -1;
    g_ctrl_activity_t = 0.0;
    g_pad_activity_t = 0.0;
    g_open_quiet = 0;
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

    /* 3) B1/B2 fire on clean release after hold; press alone does not page. */
    midi_selftest_reset();
    {
        const uint8_t press[] = {0x90, 12, 127};
        midi_feed_buf(press, sizeof press);
        if (g_state.soft_left)
            fails++; /* arm only — no fire on press */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        const uint8_t up[] = {0x80, 12, 0};
        midi_feed_buf(up, sizeof up);
        if (!g_state.soft_left)
            fails++; /* clean release fires */
        g_state.soft_left = 0;
    }
    midi_selftest_reset();
    {
        const uint8_t down[] = {0x90, 12, 127};
        midi_feed_buf(down, sizeof down);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        const uint8_t up[] = {0x80, 12, 0};
        midi_feed_buf(up, sizeof up);
        if (!g_state.soft_left)
            fails++;
        g_state.soft_left = 0;

        /* Running-status continuation must NOT arm soft key */
        const uint8_t run[] = {13, 127}; /* would be soft-mid under 0x90 */
        midi_feed_buf(run, sizeof run);
        const uint8_t runoff[] = {0x80, 13, 0};
        midi_feed_buf(runoff, sizeof runoff);
        if (g_state.soft_mid)
            fails++;
    }
    /* 3b) Knob mid-hold cancels B1 — hard isolate. */
    midi_selftest_reset();
    {
        const uint8_t down[] = {0x90, 12, 127};
        midi_feed_buf(down, sizeof down);
        const uint8_t knob[] = {0xb0, 17, 64};
        midi_feed_buf(knob, sizeof knob);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        const uint8_t up[] = {0x80, 12, 0};
        midi_feed_buf(up, sizeof up);
        if (g_state.soft_left)
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

    /* 5) Volume CC isolated — does not set soft keys / SAMPLE; dump applies. */
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
        /* Dump applies immediately; second updates */
        if (!g_state.main_engaged || g_state.main_vol != 80)
            fails++;
    }

    /* 5b) Dump at 0 → silent (software must not stay above knob). */
    midi_selftest_reset();
    {
        const uint8_t z[] = {0xb0, 19, 0};
        midi_feed_buf(z, sizeof z);
        if (!g_state.main_engaged || g_state.main_vol != 0)
            fails++;
    }

    /* 5c) Lone main-vol plunge rejected; confirmed drop accepted. */
    midi_selftest_reset();
    {
        const uint8_t seq[] = {0xb0, 19, 100, 0xb0, 19, 90, 0xb0, 19, 10};
        midi_feed_buf(seq, sizeof seq);
        if (!g_state.main_engaged || g_state.main_vol != 90)
            fails++; /* single plunge ignored */
        const uint8_t drop2[] = {0xb0, 19, 10};
        midi_feed_buf(drop2, sizeof drop2);
        if (g_state.main_vol != 10)
            fails++; /* second matching drop confirms */
    }

    /* 5d) Fader dump 0 + noise must not engage silence. */
    midi_selftest_reset();
    {
        const uint8_t seq[] = {0xb0, 20, 0, 0xb0, 20, 3};
        midi_feed_buf(seq, sizeof seq);
        if (g_state.fader_engaged)
            fails++;
        const uint8_t move[] = {0xb0, 20, 64};
        midi_feed_buf(move, sizeof move);
        if (!g_state.fader_engaged || g_state.fader != 64)
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

    /* 7) Running-status pad note-on ignored (phantom guard). */
    midi_selftest_reset();
    {
        const uint8_t hit[] = {0x99, 37, 100};
        midi_feed_buf(hit, sizeof hit);
        if (!g_state.pad_down[0])
            fails++;
        g_state.pad_down[0] = 0;
        /* Running-status continuation looks like pad2 note-on — must ignore */
        const uint8_t phantom[] = {36, 90};
        midi_feed_buf(phantom, sizeof phantom);
        if (g_state.pad_down[1] || g_state.pad_vel[1])
            fails++;
        /* Status cleared after pad msg — orphan data also dropped */
        const uint8_t orphan[] = {42, 80};
        midi_feed_buf(orphan, sizeof orphan);
        if (g_state.pad_down[2] || g_state.pad_vel[2])
            fails++;
    }

    /* 8) Volume CC stream must not create pad edges. */
    midi_selftest_reset();
    {
        const uint8_t seq[] = {
            0xb0, 19, 0, 0xb0, 19, 40, 0xb0, 19, 80, 0xb0, 19, 127,
            0xb0, 19, 0, 0xb0, 19, 0,
        };
        midi_feed_buf(seq, sizeof seq);
        for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
            if (g_state.pad_down[i] || g_state.pad_up[i] || g_state.pad_vel[i])
                fails++;
        }
        if (g_state.main_vol != 0)
            fails++;
    }

    /* 9) Shift is momentary — release clears; not a toggle latch. */
    midi_selftest_reset();
    {
        const uint8_t down[] = {0x90, 49, 127};
        midi_feed_buf(down, sizeof down);
        if (!g_state.shift_held)
            fails++;
        const uint8_t up[] = {0x80, 49, 0};
        midi_feed_buf(up, sizeof up);
        if (g_state.shift_held)
            fails++;
        /* Running-status Shift on ignored */
        const uint8_t run[] = {0x90, 6, 127, 49, 127}; /* SAMPLE then orphan shift-looking */
        midi_feed_buf(run, sizeof run);
        /* After SAMPLE on, status is 0x90; data 49,127 would be Shift under running
         * status — must NOT latch Shift. */
        if (g_state.shift_held)
            fails++;
    }

    /* 10) Knobs / encoder NEVER page — B1 suppressed during control activity. */
    midi_selftest_reset();
    {
        const uint8_t knobs[] = {0xb0, 17, 40, 0xb0, 17, 80, 0xb0, 100, 2};
        midi_feed_buf(knobs, sizeof knobs);
        const uint8_t page[] = {0x90, 12, 127};
        midi_feed_buf(page, sizeof page);
        struct timespec hold = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        nanosleep(&hold, NULL);
        const uint8_t up0[] = {0x80, 12, 0};
        midi_feed_buf(up0, sizeof up0);
        if (g_state.soft_left)
            fails++; /* must be blocked by ctrl guard */
        g_state.soft_left = 0;
        /* After guard window, real B1 still works (press + hold + release). */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 400 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        const uint8_t page2[] = {0x90, 12, 127};
        midi_feed_buf(page2, sizeof page2);
        nanosleep(&hold, NULL);
        const uint8_t up2[] = {0x80, 12, 0};
        midi_feed_buf(up2, sizeof up2);
        if (!g_state.soft_left)
            fails++;
    }

    /* 11) Encoder push marks control activity — cannot fake a page. */
    midi_selftest_reset();
    {
        const uint8_t enc[] = {0x90, 111, 127};
        midi_feed_buf(enc, sizeof enc);
        if (!g_state.wheel_press)
            fails++;
        g_state.wheel_press = 0;
        const uint8_t page[] = {0x90, 13, 127};
        midi_feed_buf(page, sizeof page);
        struct timespec hold = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        nanosleep(&hold, NULL);
        const uint8_t up[] = {0x80, 13, 0};
        midi_feed_buf(up, sizeof up);
        if (g_state.soft_mid)
            fails++;
    }

    /* 12) Pad activity cancels in-flight soft key + suppresses pages. */
    midi_selftest_reset();
    {
        const uint8_t soft[] = {0x90, 12, 127};
        midi_feed_buf(soft, sizeof soft);
        const uint8_t pad[] = {0x99, 37, 100};
        midi_feed_buf(pad, sizeof pad);
        struct timespec hold = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        nanosleep(&hold, NULL);
        const uint8_t up[] = {0x80, 12, 0};
        midi_feed_buf(up, sizeof up);
        if (g_state.soft_left)
            fails++;
        if (!g_state.pad_down[0])
            fails++;
    }

    /* 13) Low-velocity pad note-on ignored (phantom guard). */
    midi_selftest_reset();
    {
        const uint8_t soft[] = {0x99, 37, 5};
        midi_feed_buf(soft, sizeof soft);
        if (g_state.pad_down[0] || g_state.pad_vel[0])
            fails++;
    }

    /* 14) SAMPLE explicit on/off; running-status SAMPLE on ignored. */
    midi_selftest_reset();
    {
        const uint8_t on[] = {0x90, 6, 127};
        midi_feed_buf(on, sizeof on);
        if (!g_state.sample_held)
            fails++;
        const uint8_t off[] = {0x80, 6, 0};
        midi_feed_buf(off, sizeof off);
        if (g_state.sample_held)
            fails++;
        const uint8_t run[] = {0x90, 49, 127, 6, 127}; /* Shift then orphan SAMPLE */
        midi_feed_buf(run, sizeof run);
        /* After Shift, status cleared — orphan 6,127 dropped; SAMPLE stays off. */
        if (g_state.sample_held)
            fails++;
    }

    return fails ? -1 : 0;
}
