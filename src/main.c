/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE hardware bring-up — open display / surface MIDI / ALSA, show a
 * splash + home UI that reacts to pads and knobs, quit on wheel push.
 * Launcher restores stock via systemctl start ac50. Not yet sample-tested
 * for the loop engine (that stays stubbed).
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "EPICYCLE %s — panel bring-up for MPC Sample\n"
            "usage: %s [--help] [--version]\n"
            "  MPC_APP_DIR   set by the SD launcher to the payload folder\n"
            "  On device: push the data wheel (or right soft key) to quit.\n",
            EPICYCLE_VERSION, argv0);
}

static void sleep_ms(int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            puts(EPICYCLE_VERSION);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    EpicyclePaths paths;
    const char *env_dir = getenv("MPC_APP_DIR");
    if (epicycle_paths_resolve(&paths, argv[0], env_dir) != 0) {
        fprintf(stderr, "epicycle: failed to resolve card paths\n");
        return 1;
    }

    epicycle_log(&paths, "EPICYCLE %s starting (bring-up)", EPICYCLE_VERSION);
    epicycle_log(&paths, "app_dir=%s card_root=%s data_dir=%s", paths.app_dir, paths.card_root,
                 paths.data_dir);

    EpicycleEngine eng;
    engine_init(&eng);

    EpicycleHwStatus hw;
    memset(&hw, 0, sizeof hw);

    hw.display_ok = display_open() == 0;
    snprintf(hw.display_info, sizeof hw.display_info, "%s", display_info());
    epicycle_log(&paths, "display: %s", hw.display_info);

    hw.audio_ok = audio_open() == 0;
    hw.audio_rate = audio_rate();
    snprintf(hw.audio_info, sizeof hw.audio_info, "%s", audio_info());
    epicycle_log(&paths, "audio: %s", hw.audio_info);

    hw.surface_ok = surface_open() == 0;
    snprintf(hw.surface_info, sizeof hw.surface_info, "%s", surface_info());
    epicycle_log(&paths, "surface: %s", hw.surface_info);

    ui_draw_splash(hw.display_ok ? "panel online" : "panel offline (host?)");
    sleep_ms(700);

    /* Host/CI: stubs request quit immediately. On device, run until quit. */
    int ticks = 0;
    int dirty = 1;
    while (!g_stop && !surface_poll_quit()) {
        surface_poll();

        /* Keep the silent audio callback shape warm for later engine work. */
        int32_t L[EPICYCLE_AUDIO_FRAMES];
        int32_t R[EPICYCLE_AUDIO_FRAMES];
        engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
        (void)audio_running();

        if (dirty || (ticks % 4) == 0) {
            ui_draw_home(&hw, surface_state());
            dirty = 0;
        }

        sleep_ms(16);
        ticks++;

#ifndef EPICYCLE_ON_DEVICE
        /* Safety: never hang the host smoke test. */
        if (ticks > 60)
            break;
#endif
        /* Redraw when pads/knobs change — cheap full refresh at ~15 Hz above. */
        dirty = 1;
    }

    ui_draw_splash("quitting to stock");
    sleep_ms(250);

    surface_close();
    audio_close();
    display_close();
    epicycle_log(&paths, "EPICYCLE exiting cleanly (launcher starts ac50)");
    return 0;
}
