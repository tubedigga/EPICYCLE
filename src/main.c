/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE bring-up scaffold — boots far enough to resolve paths, open stubs,
 * draw a splash, and exit restoring stock when run under the SD launcher.
 * Not hardware-tested.
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
            "EPICYCLE %s — bring-up scaffold (not hardware-tested)\n"
            "usage: %s [--help] [--version]\n"
            "  MPC_APP_DIR   set by the SD launcher to the payload folder\n",
            EPICYCLE_VERSION, argv0);
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

    epicycle_log(&paths, "EPICYCLE %s starting (scaffold)", EPICYCLE_VERSION);
    epicycle_log(&paths, "app_dir=%s card_root=%s data_dir=%s", paths.app_dir, paths.card_root,
                 paths.data_dir);

    EpicycleEngine eng;
    engine_init(&eng);

    if (display_open() != 0)
        epicycle_log(&paths, "display: open failed (continuing)");
    if (audio_open() != 0)
        epicycle_log(&paths, "audio: open failed (continuing)");
    if (surface_open() != 0)
        epicycle_log(&paths, "surface: open failed (continuing)");

    ui_draw_splash("bring-up scaffold");

    /* Host/CI: stubs request quit immediately. On device, wait for signal/quit. */
    int ticks = 0;
    while (!g_stop && !surface_poll_quit() && ticks < 30) {
        int32_t L[EPICYCLE_AUDIO_FRAMES];
        int32_t R[EPICYCLE_AUDIO_FRAMES];
        engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
        (void)audio_running();
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        ticks++;
    }

    surface_close();
    audio_close();
    display_close();
    epicycle_log(&paths, "EPICYCLE exiting cleanly");
    return 0;
}
