/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE loop engine — load samples to pads, edit scrollable brace with
 * knobs, time-division or free-run against tempo, hear sync/polymeter.
 * Soft-left toggles free/div; soft-mid toggles LFO brace scrub on selected pad.
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <math.h>
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
            "EPICYCLE %s — loop engine for MPC Sample\n"
            "usage: %s [--help] [--version] [--demo] [--wav PATH]...\n"
            "  --demo       synth demo tones onto pads (host / no card samples)\n"
            "  --wav PATH   load WAV into next bank slot (up to 16)\n"
            "  MPC_APP_DIR  set by the SD launcher to the payload folder\n"
            "  On device: pads play; knobs edit brace; wheel push quits.\n",
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

static int try_load_card_samples(EpicycleEngine *eng, const EpicyclePaths *paths)
{
    int loaded = 0;
    char path[768];
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        snprintf(path, sizeof path, "%s/samples/pad%02d.wav", paths->data_dir, i + 1);
        if (engine_load_bank_wav(eng, i, path) == 0) {
            engine_assign_sample(eng, i, i);
            loaded++;
            continue;
        }
        snprintf(path, sizeof path, "%s/samples/%d.wav", paths->data_dir, i + 1);
        if (engine_load_bank_wav(eng, i, path) == 0) {
            engine_assign_sample(eng, i, i);
            loaded++;
        }
    }
    /* Also try bundled demos next to the binary. */
    if (loaded == 0) {
        static const char *names[] = {"demo-a.wav", "demo-b.wav", "demo-c.wav", "demo-d.wav", NULL};
        for (int i = 0; names[i]; i++) {
            snprintf(path, sizeof path, "%s/%s", paths->app_dir, names[i]);
            if (engine_load_bank_wav(eng, i, path) == 0) {
                engine_assign_sample(eng, i, i);
                engine_assign_sample(eng, i + 4, i);
                loaded++;
            }
        }
    }
    return loaded;
}

static void handle_soft_keys(EpicycleEngine *eng, EpicycleSurfaceState *st)
{
    if (!eng || !st)
        return;
    int pad = eng->selected_pad;
    if (st->soft_left) {
        st->soft_left = 0;
        EpicyclePad *p = &eng->pads[pad];
        if (p->freeform)
            engine_set_division(eng, pad, p->division_beats > 0 ? p->division_beats : 4);
        else
            engine_set_division(eng, pad, 0);
    }
    if (st->soft_mid) {
        st->soft_mid = 0;
        EpicyclePad *p = &eng->pads[pad];
        /* Toggle LFO0 brace-scroll; respect scrub budget via allow_scrub. */
        if (!p->lfos[0].enabled) {
            p->lfos[0].enabled = 1;
            if (p->lfos[0].depth < 0.1f)
                p->lfos[0].depth = 0.35f;
            p->mods[0].lfo_index = 0;
            p->mods[0].dest = EPICYCLE_MOD_BRACE_START;
            p->mods[0].amount = 1.0f;
            p->allow_scrub = 1;
        } else {
            p->lfos[0].enabled = 0;
            p->lfos[0].depth = 0.0f;
        }
    }
}

static void write_host_preview_wav(const char *path, EpicycleEngine *eng, float seconds)
{
    /* Tiny offline bounce so CI can prove the engine makes non-silent audio. */
    size_t frames = (size_t)(eng->sample_rate * seconds);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return;
    uint32_t data_bytes = (uint32_t)(frames * 2 * 2); /* 16-bit stereo */
    uint32_t riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, fp);
    fwrite(&riff, 4, 1, fp);
    fwrite("WAVEfmt ", 1, 8, fp);
    uint32_t fmt_sz = 16;
    uint16_t audio_fmt = 1, ch = 2, bps = 16;
    uint32_t rate = eng->sample_rate;
    uint32_t byte_rate = rate * ch * bps / 8;
    uint16_t block = (uint16_t)(ch * bps / 8);
    fwrite(&fmt_sz, 4, 1, fp);
    fwrite(&audio_fmt, 2, 1, fp);
    fwrite(&ch, 2, 1, fp);
    fwrite(&rate, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block, 2, 1, fp);
    fwrite(&bps, 2, 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&data_bytes, 4, 1, fp);

    int32_t L[EPICYCLE_AUDIO_FRAMES], R[EPICYCLE_AUDIO_FRAMES];
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > EPICYCLE_AUDIO_FRAMES)
            n = EPICYCLE_AUDIO_FRAMES;
        engine_render(eng, L, R, n);
        for (size_t i = 0; i < n; i++) {
            int16_t sl = (int16_t)(L[i] >> 16);
            int16_t sr = (int16_t)(R[i] >> 16);
            fwrite(&sl, 2, 1, fp);
            fwrite(&sr, 2, 1, fp);
        }
        done += n;
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    int force_demo = 0;
    const char *wavs[EPICYCLE_PAD_COUNT];
    int wav_n = 0;
    const char *preview_out = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            puts(EPICYCLE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--demo") == 0) {
            force_demo = 1;
            continue;
        }
        if (strcmp(argv[i], "--preview") == 0 && i + 1 < argc) {
            preview_out = argv[++i];
            force_demo = 1;
            continue;
        }
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc && wav_n < EPICYCLE_PAD_COUNT) {
            wavs[wav_n++] = argv[++i];
            continue;
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

    epicycle_log(&paths, "EPICYCLE %s starting (loop engine)", EPICYCLE_VERSION);
    epicycle_log(&paths, "app_dir=%s card_root=%s data_dir=%s", paths.app_dir, paths.card_root,
                 paths.data_dir);

    EpicycleEngine eng;
    engine_init(&eng, 44100);

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

    int loaded = 0;
    for (int i = 0; i < wav_n; i++) {
        if (engine_load_bank_wav(&eng, i, wavs[i]) == 0) {
            engine_assign_sample(&eng, i, i);
            loaded++;
        }
    }
    if (!force_demo && loaded == 0)
        loaded = try_load_card_samples(&eng, &paths);
    if (force_demo || loaded == 0) {
        engine_host_demo_setup(&eng);
        epicycle_log(&paths, "using synth demo bank (polymeter pads 2/3 = 3 vs 5 beats)");
    } else {
        /* Default musical layout when WAVs present: pad0/1 synced 4-beat, 2/3 polymeter. */
        if (eng.pads[0].sample)
            engine_set_division(&eng, 0, 4);
        if (eng.pads[1].sample)
            engine_set_division(&eng, 1, 4);
        if (eng.pads[2].sample)
            engine_set_division(&eng, 2, 3);
        if (eng.pads[3].sample)
            engine_set_division(&eng, 3, 5);
        epicycle_log(&paths, "loaded %d sample slot(s)", loaded);
    }

    if (preview_out) {
        /* Host CI path: trigger a few pads, bounce audio, exit. */
        engine_note_on(&eng, 0, 0.9f);
        engine_note_on(&eng, 2, 0.8f);
        engine_note_on(&eng, 3, 0.8f);
        write_host_preview_wav(preview_out, &eng, 1.5f);
        epicycle_log(&paths, "wrote preview %s", preview_out);
        audio_close();
        display_close();
        engine_shutdown(&eng);
        return 0;
    }

    audio_set_engine(&eng);

    {
        char logo_path[640];
        snprintf(logo_path, sizeof logo_path, "%s/assets/boot.png", paths.app_dir);
        if (ui_load_splash_logo(logo_path) != 0)
            epicycle_log(&paths, "splash logo missing (%s) — text fallback", logo_path);
        else
            epicycle_log(&paths, "splash logo loaded %s", logo_path);
    }

    ui_draw_splash(hw.display_ok ? "online" : "offline");
    sleep_ms(800);

    /* Auto-fire demo pads on host so something is audible if audio exists. */
#ifndef EPICYCLE_ON_DEVICE
    engine_note_on(&eng, 0, 0.8f);
    engine_note_on(&eng, 2, 0.7f);
    engine_note_on(&eng, 3, 0.7f);
#endif

    int ticks = 0;
    uint8_t last_k1 = 255, last_k2 = 255, last_k3 = 255, last_main = 255;

    /* Apply any main-volume reply from surface position SysEx 0x42. */
    surface_poll();
    engine_set_master_gain_cc(&eng, surface_state()->main_vol);
    last_main = surface_state()->main_vol;

    while (!g_stop && !surface_poll_quit()) {
        surface_poll();
        EpicycleSurfaceState *st = surface_state_mut();
        const EpicycleSurfaceState *surf = surface_state();

        handle_soft_keys(&eng, st);

        for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
            if (st->pad_down[i]) {
                st->pad_down[i] = 0;
                eng.selected_pad = i;
                float vel = surf->pad_vel[i] > 0 ? (float)surf->pad_vel[i] / 127.0f : 0.8f;
                engine_note_on(&eng, i, vel);
            }
        }

        if (surf->main_vol != last_main) {
            engine_set_master_gain_cc(&eng, surf->main_vol);
            last_main = surf->main_vol;
        }

        if (surf->k1 != last_k1 || surf->k2 != last_k2 || surf->k3 != last_k3) {
            engine_apply_knobs(&eng, eng.selected_pad, surf->k1, surf->k2, surf->k3);
            last_k1 = surf->k1;
            last_k2 = surf->k2;
            last_k3 = surf->k3;
        }

        /* Wheel without push: nudge selected pad window start (scroll brace). */
        if (surf->wheel_accum != 0) {
            EpicyclePad *p = &eng.pads[eng.selected_pad];
            float d = (float)surf->wheel_accum * 0.002f;
            st->wheel_accum = 0;
            engine_set_window(&eng, eng.selected_pad, p->window_start + d, p->window_length);
        }

        if ((ticks % 3) == 0)
            ui_draw_loop(&hw, &eng, surf);

        /* Host without audio thread: pump engine so preview logic stays warm. */
        if (!audio_running()) {
            int32_t L[EPICYCLE_AUDIO_FRAMES], R[EPICYCLE_AUDIO_FRAMES];
            engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
        }

        sleep_ms(16);
        ticks++;

#ifndef EPICYCLE_ON_DEVICE
        if (ticks > 90)
            break;
#endif
    }

    ui_draw_splash("quit");
    sleep_ms(250);

    audio_set_engine(NULL);
    surface_close();
    audio_close();
    display_close();
    engine_shutdown(&eng);
    epicycle_log(&paths, "EPICYCLE exiting cleanly (launcher starts ac50)");
    return 0;
}
