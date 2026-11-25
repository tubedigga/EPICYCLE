/* SPDX-License-Identifier: MIT
 *
 * EPICYCLE loop engine — Sample-Mode-style soft-key pages for pad params
 * (Trim / Mix / Amp / Tune / Filter / Filt Env / LFO). Not a stock OS clone.
 * Hold SAMPLE + pad = select; B1/B2 cycle pages; B3 quit; encoder push never quits.
 * Device UX 0.2.6: Trim Start/End width + POS slide; Shift xfade/ping-pong;
 * fader latch; LOOP button; pad vol/pan Mix.
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
            "  On device: SAMPLE+pad selects; B1/B2 pages; B3 quits; encoder push never quits.\n",
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

static void page_prev(EpicycleEngine *eng)
{
    int p = (int)eng->mode_page - 1;
    if (p < 0)
        p = (int)EPICYCLE_PAGE_COUNT - 1;
    eng->mode_page = (EpicycleModePage)p;
}

static void page_next(EpicycleEngine *eng)
{
    int p = (int)eng->mode_page + 1;
    if (p >= (int)EPICYCLE_PAGE_COUNT)
        p = 0;
    eng->mode_page = (EpicycleModePage)p;
}

static int g_soft_guard_ticks; /* ignore soft keys briefly after pad hits */

static void handle_soft_keys(EpicycleEngine *eng, EpicycleSurfaceState *st)
{
    if (!eng || !st)
        return;
    /* Debounce: MIDI noise / aftertouch floods were causing random page jumps
     * and false B3 quits that look like crashes. */
    if (g_soft_guard_ticks > 0) {
        st->soft_left = 0;
        st->soft_mid = 0;
        st->soft_right = 0;
        return;
    }
    if (st->soft_left) {
        st->soft_left = 0;
        page_prev(eng);
    }
    if (st->soft_mid) {
        st->soft_mid = 0;
        page_next(eng);
    }
    if (st->soft_right) {
        st->soft_right = 0;
        st->quit_requested = 1; /* explicit soft-key quit only */
    }
}

static void handle_encoder_press(EpicycleEngine *eng, EpicycleSurfaceState *st)
{
    if (!eng || !st || !st->wheel_press)
        return;
    st->wheel_press = 0;
    int pad = eng->selected_pad;
    if (eng->mode_page == EPICYCLE_PAGE_LFO) {
        engine_lfo_toggle_sync(eng, pad);
        return;
    }
    /* Trim (and other pages): encoder push toggles freeform ↔ division. */
    EpicyclePad *p = &eng->pads[pad];
    if (p->freeform)
        engine_set_division(eng, pad, p->division_beats > 0 ? p->division_beats : 4);
    else
        engine_set_division(eng, pad, 0);
}

static void write_host_preview_wav(const char *path, EpicycleEngine *eng, float seconds)
{
    size_t frames = (size_t)(eng->sample_rate * seconds);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return;
    uint32_t data_bytes = (uint32_t)(frames * 2 * 2);
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

/* Wait on splash until pad / soft key / encoder press. Host skips. */
static void splash_wait_for_input(const EpicyclePaths *paths)
{
    static const char *k_prompt = "PRESS ANY PAD TO CONTINUE";
#ifdef EPICYCLE_ON_DEVICE
    epicycle_log(paths, "splash: waiting for pad / encoder / soft key");
    ui_draw_splash_prompt(k_prompt);
    for (;;) {
        if (g_stop)
            return;
        surface_poll();
        EpicycleSurfaceState *st = surface_state_mut();
        int go = 0;
        if (st->wheel_press) {
            st->wheel_press = 0;
            go = 1;
        }
        if (st->soft_left) {
            st->soft_left = 0;
            go = 1;
        }
        if (st->soft_mid) {
            st->soft_mid = 0;
            go = 1;
        }
        if (st->soft_right) {
            /* Soft-right starts the app on splash; quit only after enter. */
            st->soft_right = 0;
            go = 1;
        }
        for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
            if (st->pad_down[i]) {
                st->pad_down[i] = 0;
                go = 1;
            }
            if (st->pad_up[i])
                st->pad_up[i] = 0;
        }
        if (go) {
            epicycle_log(paths, "splash: dismissed");
            return;
        }
        /* Keep prompt visible; refresh occasionally in case of display glitch. */
        static int blink;
        if ((blink++ % 30) == 0)
            ui_draw_splash_prompt(k_prompt);
        sleep_ms(16);
    }
#else
    (void)paths;
    ui_draw_splash_prompt(k_prompt);
    sleep_ms(200);
#endif
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

    epicycle_log(&paths, "EPICYCLE %s starting (mode pages)", EPICYCLE_VERSION);
    epicycle_log(&paths, "app_dir=%s card_root=%s data_dir=%s", paths.app_dir, paths.card_root,
                 paths.data_dir);

    EpicycleEngine eng;
    engine_init(&eng, 44100);

    EpicycleHwStatus hw;
    memset(&hw, 0, sizeof hw);

    hw.display_ok = display_open() == 0;
    snprintf(hw.display_info, sizeof hw.display_info, "%s", display_info());
    epicycle_log(&paths, "display: %s", hw.display_info);
    for (int i = 0; i < display_log_count(); i++)
        epicycle_log(&paths, "display-diag: %s", display_log_line(i));

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
    for (int i = 0; i < display_log_count(); i++)
        epicycle_log(&paths, "display-diag: %s", display_log_line(i));
    sleep_ms(400);
    splash_wait_for_input(&paths);

#ifndef EPICYCLE_ON_DEVICE
    engine_note_on(&eng, 0, 0.8f);
    engine_note_on(&eng, 2, 0.7f);
    engine_note_on(&eng, 3, 0.7f);
#endif

    int ticks = 0;
    uint8_t last_k1 = 255, last_k2 = 255, last_k3 = 255, last_main = 255;
    EpicycleModePage last_page = eng.mode_page;
    int last_sel = eng.selected_pad;
    uint8_t last_pad_vel[EPICYCLE_PAD_COUNT];
    memset(last_pad_vel, 0, sizeof last_pad_vel);
    g_soft_guard_ticks = 45; /* ~0.7s after splash before soft keys armed */

    surface_poll();
    /* Main tracks hardware; fader stays full until user moves it (latch). */
    engine_set_master_gain_cc(&eng, surface_state()->main_vol);
    last_main = surface_state()->main_vol;
    last_k1 = surface_state()->k1;
    last_k2 = surface_state()->k2;
    last_k3 = surface_state()->k3;
    surface_set_selected_pad(eng.selected_pad);
    surface_set_loop_led(eng.pads[eng.selected_pad].loop_enable);

    while (!g_stop && !surface_poll_quit()) {
        surface_poll();
        EpicycleSurfaceState *st = surface_state_mut();
        const EpicycleSurfaceState *surf = surface_state();

        if (g_soft_guard_ticks > 0)
            g_soft_guard_ticks--;

        handle_soft_keys(&eng, st);
        handle_encoder_press(&eng, st);

        if (st->loop_btn) {
            st->loop_btn = 0;
            engine_toggle_loop(&eng, eng.selected_pad);
            surface_set_loop_led(eng.pads[eng.selected_pad].loop_enable);
            epicycle_log(&paths, "LOOP pad %d -> %s", eng.selected_pad + 1,
                         eng.pads[eng.selected_pad].loop_enable ? "latch" : "one-shot");
        }

        for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
            if (st->pad_down[i]) {
                st->pad_down[i] = 0;
                g_soft_guard_ticks = 12; /* ~200ms guard against phantom soft keys */
                if (surf->sample_held) {
                    eng.selected_pad = i;
                    surface_set_selected_pad(i);
                    surface_set_loop_led(eng.pads[i].loop_enable);
                    engine_sync_mod_window(&eng, i);
                } else {
                    float vel =
                        surf->pad_vel[i] > 0 ? (float)surf->pad_vel[i] / 127.0f : 0.8f;
                    engine_note_on(&eng, i, vel);
                }
            }
            if (st->pad_up[i]) {
                st->pad_up[i] = 0;
                engine_note_off(&eng, i);
            } else if (last_pad_vel[i] > 0 && surf->pad_vel[i] == 0) {
                engine_note_off(&eng, i);
            }
            last_pad_vel[i] = surf->pad_vel[i];
        }

        if (eng.selected_pad != last_sel) {
            last_sel = eng.selected_pad;
            surface_set_loop_led(eng.pads[last_sel].loop_enable);
            /* Absorb parked pot positions so pad change doesn't jump params. */
            last_k1 = surf->k1;
            last_k2 = surf->k2;
            last_k3 = surf->k3;
        }
        if (eng.mode_page != last_page) {
            last_page = eng.mode_page;
            last_k1 = surf->k1;
            last_k2 = surf->k2;
            last_k3 = surf->k3;
        }

        if (surf->main_vol != last_main) {
            engine_set_master_gain_cc(&eng, surf->main_vol);
            last_main = surf->main_vol;
        }
        /* Fader: ignore 0x42 dump + spurious CC until user moves from dump. */
        if (surf->fader_engaged)
            engine_set_fader_gain_cc(&eng, surf->fader);

        {
            unsigned mask = 0;
            if (surf->k1 != last_k1)
                mask |= 1u;
            if (surf->k2 != last_k2)
                mask |= 2u;
            if (surf->k3 != last_k3)
                mask |= 4u;
            if (mask) {
                if (surf->shift_held && eng.mode_page == EPICYCLE_PAGE_TRIM) {
                    EpicyclePad *p = &eng.pads[eng.selected_pad];
                    if (mask & 1u)
                        p->crossfade = 0.01f + 0.49f * ((float)surf->k1 / 127.0f);
                    if (mask & 2u)
                        engine_set_ping_pong(&eng, eng.selected_pad, surf->k2 >= 64);
                    if (mask & 4u)
                        engine_trim_set_position(&eng, eng.selected_pad,
                                                 (float)surf->k3 / 127.0f);
                } else if (surf->shift_held && eng.mode_page == EPICYCLE_PAGE_MIX) {
                    if (mask & 4u)
                        eng.kit_level = 0.05f + 0.95f * ((float)surf->k3 / 127.0f);
                    if (mask & 1u)
                        engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page,
                                                    surf->k1, surf->k2, surf->k3, 1u);
                    if (mask & 2u)
                        engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page,
                                                    surf->k1, surf->k2, surf->k3, 2u);
                } else {
                    engine_apply_page_knobs_mask(&eng, eng.selected_pad, eng.mode_page, surf->k1,
                                                surf->k2, surf->k3, mask);
                }
                last_k1 = surf->k1;
                last_k2 = surf->k2;
                last_k3 = surf->k3;
            }
        }

        /* Wheel: Trim = slide POS; Tune = semi; etc. */
        if (surf->wheel_accum != 0) {
            int delta = surf->wheel_accum;
            st->wheel_accum = 0;
            if (eng.mode_page == EPICYCLE_PAGE_TRIM) {
                if (surf->shift_held) {
                    EpicyclePad *p = &eng.pads[eng.selected_pad];
                    p->crossfade += (float)delta * 0.01f;
                    if (p->crossfade < 0.01f)
                        p->crossfade = 0.01f;
                    if (p->crossfade > 0.5f)
                        p->crossfade = 0.5f;
                } else {
                    EpicyclePad *p = &eng.pads[eng.selected_pad];
                    engine_trim_set_position(&eng, eng.selected_pad,
                                             p->window_start + (float)delta * 0.002f);
                }
            } else if (eng.mode_page == EPICYCLE_PAGE_TUNE) {
                EpicyclePad *p = &eng.pads[eng.selected_pad];
                engine_set_pitch(&eng, eng.selected_pad, p->octave, p->semitone + delta, p->fine);
            } else if (eng.mode_page == EPICYCLE_PAGE_LFO) {
                EpicycleLfo *l = &eng.pads[eng.selected_pad].lfos[0];
                if (l->sync_beats > 0.0f) {
                    l->sync_beats += (float)delta * 0.25f;
                    if (l->sync_beats < 0.25f)
                        l->sync_beats = 0.25f;
                    if (l->sync_beats > 8.0f)
                        l->sync_beats = 8.0f;
                } else {
                    l->rate_hz += (float)delta * 0.02f;
                    if (l->rate_hz < 0.05f)
                        l->rate_hz = 0.05f;
                    if (l->rate_hz > 8.0f)
                        l->rate_hz = 8.0f;
                }
            } else if (eng.mode_page == EPICYCLE_PAGE_MIX) {
                EpicyclePad *p = &eng.pads[eng.selected_pad];
                p->level += (float)delta * 0.01f;
                if (p->level < 0.0f)
                    p->level = 0.0f;
                if (p->level > 1.0f)
                    p->level = 1.0f;
                p->mod_level = p->level;
            }
        }

        if ((ticks % 3) == 0)
            ui_draw_loop(&hw, &eng, surf);

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
