/* SPDX-License-Identifier: MIT
 *
 * Live WAV browser for Epicycle/samples — assign any file onto a pad
 * without rebuilding the SD autorun (Ash 0.3.8).
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void sample_browser_init(EpicycleSampleBrowser *b)
{
    if (!b)
        return;
    memset(b, 0, sizeof *b);
    b->last_pad = -1;
    snprintf(b->status, sizeof b->status, "scan samples/");
}

static int ends_with_wav(const char *name)
{
    size_t n = strlen(name);
    if (n < 5)
        return 0;
    const char *e = name + n - 4;
    return strcasecmp(e, ".wav") == 0;
}

static int already_listed(const EpicycleSampleBrowser *b, const char *path)
{
    for (int i = 0; i < b->count; i++) {
        if (strcmp(b->paths[i], path) == 0)
            return 1;
    }
    return 0;
}

int sample_browser_scan(EpicycleSampleBrowser *b, const char *dir)
{
    if (!b || !dir || !dir[0])
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int added = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        if (!ends_with_wav(ent->d_name))
            continue;
        if (b->count >= EPICYCLE_BROWSER_MAX)
            break;
        char path[512];
        if (snprintf(path, sizeof path, "%s/%s", dir, ent->d_name) >= (int)sizeof path)
            continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (already_listed(b, path))
            continue;
        int i = b->count;
        snprintf(b->names[i], sizeof b->names[i], "%s", ent->d_name);
        snprintf(b->paths[i], sizeof b->paths[i], "%s", path);
        b->count++;
        added++;
    }
    closedir(d);
    return added;
}

static void sort_browser(EpicycleSampleBrowser *b)
{
    /* Simple insertion sort by filename — small N. */
    for (int i = 1; i < b->count; i++) {
        char ntmp[EPICYCLE_BROWSER_NAME];
        char ptmp[512];
        snprintf(ntmp, sizeof ntmp, "%s", b->names[i]);
        snprintf(ptmp, sizeof ptmp, "%s", b->paths[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(b->names[j], ntmp) > 0) {
            snprintf(b->names[j + 1], sizeof b->names[j + 1], "%s", b->names[j]);
            snprintf(b->paths[j + 1], sizeof b->paths[j + 1], "%s", b->paths[j]);
            j--;
        }
        snprintf(b->names[j + 1], sizeof b->names[j + 1], "%s", ntmp);
        snprintf(b->paths[j + 1], sizeof b->paths[j + 1], "%s", ptmp);
    }
}

int sample_browser_rescan(EpicycleSampleBrowser *b, const EpicyclePaths *paths)
{
    if (!b)
        return 0;
    int keep_cursor = b->cursor;
    char keep_name[EPICYCLE_BROWSER_NAME];
    keep_name[0] = '\0';
    if (b->count > 0 && b->cursor >= 0 && b->cursor < b->count)
        snprintf(keep_name, sizeof keep_name, "%s", b->names[b->cursor]);

    sample_browser_init(b);
    if (paths) {
        char dir[640];
        if (paths->data_dir[0]) {
            snprintf(dir, sizeof dir, "%s/samples", paths->data_dir);
            sample_browser_scan(b, dir);
            /* Also loose WAVs directly under Epicycle/ */
            sample_browser_scan(b, paths->data_dir);
        }
        if (paths->card_root[0]) {
            snprintf(dir, sizeof dir, "%s/samples", paths->card_root);
            sample_browser_scan(b, dir);
        }
        if (paths->app_dir[0]) {
            /* Bundled demo tones next to the binary (host / fallback). */
            sample_browser_scan(b, paths->app_dir);
        }
    }
    sort_browser(b);
    b->cursor = 0;
    if (keep_name[0]) {
        for (int i = 0; i < b->count; i++) {
            if (strcmp(b->names[i], keep_name) == 0) {
                b->cursor = i;
                break;
            }
        }
    } else if (keep_cursor > 0 && keep_cursor < b->count) {
        b->cursor = keep_cursor;
    }
    if (b->count == 0) {
        b->cursor = 0;
        snprintf(b->status, sizeof b->status, "no wav — put in samples/");
    } else {
        if (b->cursor < 0)
            b->cursor = 0;
        if (b->cursor >= b->count)
            b->cursor = b->count - 1;
        snprintf(b->status, sizeof b->status, "%d wavs  enc=load", b->count);
    }
    return b->count;
}

void sample_browser_move(EpicycleSampleBrowser *b, int delta)
{
    if (!b || b->count <= 0 || delta == 0)
        return;
    /* Clamp cursor first — rescan / empty list must never leave it unbound. */
    if (b->cursor < 0)
        b->cursor = 0;
    if (b->cursor >= b->count)
        b->cursor = b->count - 1;
    int c = b->cursor + delta;
    while (c < 0)
        c += b->count;
    while (c >= b->count)
        c -= b->count;
    b->cursor = c;
}

int sample_browser_preview(EpicycleSampleBrowser *b, EpicycleEngine *eng, int hold)
{
    if (!b || !eng) {
        return -1;
    }
    if (b->count <= 0 || b->cursor < 0 || b->cursor >= b->count) {
        snprintf(b->status, sizeof b->status, "nothing to preview");
        return -1;
    }
    int rc = engine_preview_wav(eng, b->paths[b->cursor], hold);
    if (rc == EPICYCLE_WAV_ERR_TOO_BIG) {
        snprintf(b->status, sizeof b->status, "too big (skip)");
        return -1;
    }
    if (rc != 0) {
        snprintf(b->status, sizeof b->status, "bad wav (skip)");
        return -1;
    }
    snprintf(b->status, sizeof b->status, hold ? "hold %s" : "play %s", b->names[b->cursor]);
    return 0;
}

int sample_browser_assign(EpicycleSampleBrowser *b, EpicycleEngine *eng, int pad)
{
    if (!b || !eng || pad < 0 || pad >= EPICYCLE_PAD_COUNT)
        return -1;
    if (b->count <= 0 || b->cursor < 0 || b->cursor >= b->count) {
        snprintf(b->status, sizeof b->status, "nothing to load");
        return -1;
    }
    const char *path = b->paths[b->cursor];
    engine_preview_stop(eng);
    /* Load into this pad's bank slot so each pad can hold a different WAV. */
    int rc = engine_load_bank_wav(eng, pad, path);
    if (rc == EPICYCLE_WAV_ERR_TOO_BIG) {
        snprintf(b->status, sizeof b->status, "too big %s", b->names[b->cursor]);
        return -1;
    }
    if (rc != 0) {
        snprintf(b->status, sizeof b->status, "fail %s", b->names[b->cursor]);
        return -1;
    }
    if (engine_assign_sample(eng, pad, pad) != 0) {
        snprintf(b->status, sizeof b->status, "assign fail");
        return -1;
    }
    /* Sensible FREE window for new material — full file, user trims. */
    eng->pads[pad].freeform = 1;
    eng->pads[pad].window_start = 0.0f;
    eng->pads[pad].window_length = 1.0f;
    engine_sync_mod_window(eng, pad);
    b->last_pad = pad;
    snprintf(b->status, sizeof b->status, "PAD%d <- %s", pad + 1, b->names[b->cursor]);
    return 0;
}

/* --- host regression: SAMPLE browse / preview must never crash --- */

static void wr16(FILE *fp, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, fp);
}

static void wr32(FILE *fp, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff),
                    (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff)};
    fwrite(b, 1, 4, fp);
}

/* Write a tiny PCM WAV (fmt 1). ch=1|2, frames of silence/tone. */
static int write_pcm16_wav(const char *path, int ch, unsigned rate, size_t frames, int tone)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    uint32_t data_bytes = (uint32_t)(frames * (size_t)ch * 2u);
    uint32_t riff = 36u + data_bytes;
    fwrite("RIFF", 1, 4, fp);
    wr32(fp, riff);
    fwrite("WAVEfmt ", 1, 8, fp);
    wr32(fp, 16);
    wr16(fp, 1); /* PCM */
    wr16(fp, (uint16_t)ch);
    wr32(fp, rate);
    wr32(fp, rate * (uint32_t)ch * 2u);
    wr16(fp, (uint16_t)(ch * 2));
    wr16(fp, 16);
    fwrite("data", 1, 4, fp);
    wr32(fp, data_bytes);
    for (size_t i = 0; i < frames; i++) {
        int16_t s = 0;
        if (tone)
            s = (int16_t)(((i * 37) & 0xff) << 6);
        for (int c = 0; c < ch; c++)
            wr16(fp, (uint16_t)s);
    }
    fclose(fp);
    return 0;
}

/* Header-only oversized data chunk — loader must reject before malloc bomb. */
static int write_oversized_header_wav(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    fwrite("RIFF", 1, 4, fp);
    wr32(fp, 36);
    fwrite("WAVEfmt ", 1, 8, fp);
    wr32(fp, 16);
    wr16(fp, 1);
    wr16(fp, 2);
    wr32(fp, 44100);
    wr32(fp, 44100 * 4);
    wr16(fp, 4);
    wr16(fp, 16);
    fwrite("data", 1, 4, fp);
    /* Claim more frames than EPICYCLE_WAV_MAX_FRAMES allows. */
    uint32_t bomb = (uint32_t)((EPICYCLE_WAV_MAX_FRAMES + 8u) * 4u);
    wr32(fp, bomb);
    /* No payload — fread would fail anyway; size check must trip first. */
    fclose(fp);
    return 0;
}

static int write_bad_header_wav(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;
    fwrite("NOTWAVE!!!!", 1, 12, fp);
    fclose(fp);
    return 0;
}

int sample_preview_selftest(void)
{
    char dir[] = "/tmp/epicycle-preview-XXXXXX";
    if (!mkdtemp(dir))
        return -1;

    char path_mono[256], path_stereo[256], path_short[256], path_empty[256];
    char path_huge[256], path_bad[256], path_empty_name[256];
    snprintf(path_mono, sizeof path_mono, "%s/mono.wav", dir);
    snprintf(path_stereo, sizeof path_stereo, "%s/stereo.wav", dir);
    snprintf(path_short, sizeof path_short, "%s/short.wav", dir);
    snprintf(path_empty, sizeof path_empty, "%s/empty.wav", dir);
    snprintf(path_huge, sizeof path_huge, "%s/huge.wav", dir);
    snprintf(path_bad, sizeof path_bad, "%s/bad.wav", dir);
    snprintf(path_empty_name, sizeof path_empty_name, "%s/z_empty_pcm.wav", dir);

    int rc = 0;
    if (write_pcm16_wav(path_mono, 1, 44100, 2048, 1) != 0)
        rc = -10;
    if (write_pcm16_wav(path_stereo, 2, 44100, 4096, 1) != 0)
        rc = -11;
    if (write_pcm16_wav(path_short, 2, 44100, 64, 1) != 0)
        rc = -12;
    if (write_pcm16_wav(path_empty_name, 2, 44100, 16, 0) != 0)
        rc = -13;
    if (write_oversized_header_wav(path_huge) != 0)
        rc = -14;
    if (write_bad_header_wav(path_bad) != 0)
        rc = -15;
    /* Zero-byte "wav" — fopen ok, header fail. */
    {
        FILE *fp = fopen(path_empty, "wb");
        if (fp)
            fclose(fp);
        else
            rc = -16;
    }
    if (rc != 0)
        goto done;

    /* Direct loader checks. */
    {
        EpicycleSample s;
        memset(&s, 0, sizeof s);
        if (sample_load_wav(&s, path_mono) != 0 || !s.data || s.frames < 2) {
            rc = -20;
            sample_free(&s);
            goto done;
        }
        sample_free(&s);
        if (sample_load_wav(&s, path_stereo) != 0) {
            rc = -21;
            goto done;
        }
        sample_free(&s);
        if (sample_load_wav(&s, path_bad) == 0) {
            sample_free(&s);
            rc = -22;
            goto done;
        }
        if (sample_load_wav(&s, path_empty) == 0) {
            sample_free(&s);
            rc = -23;
            goto done;
        }
        if (sample_load_wav(&s, path_huge) != EPICYCLE_WAV_ERR_TOO_BIG) {
            sample_free(&s);
            rc = -24;
            goto done;
        }
        sample_free(&s);
        /* Missing path */
        if (sample_load_wav(&s, "/tmp/epicycle-no-such-file.wav") == 0) {
            sample_free(&s);
            rc = -25;
            goto done;
        }
    }

    EpicycleEngine eng;
    engine_init(&eng, 44100);
    eng.running = 1;

    EpicyclePaths paths;
    memset(&paths, 0, sizeof paths);
    snprintf(paths.data_dir, sizeof paths.data_dir, "%s", dir);

    EpicycleSampleBrowser b;
    sample_browser_init(&b);
    int n = sample_browser_rescan(&b, &paths);
    if (n < 4) {
        rc = -30;
        goto shut;
    }
    if (b.cursor < 0 || b.cursor >= b.count) {
        rc = -31;
        goto shut;
    }

    /* Fuzz scroll + wheel peek across every entry (incl. bad/huge). */
    int32_t L[EPICYCLE_AUDIO_FRAMES];
    int32_t R[EPICYCLE_AUDIO_FRAMES];
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < b.count + 5; i++) {
            sample_browser_move(&b, 1);
            (void)sample_browser_preview(&b, &eng, 0); /* must not crash */
            for (int k = 0; k < 6; k++)
                engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
        }
        for (int i = 0; i < b.count + 3; i++) {
            sample_browser_move(&b, -1);
            (void)sample_browser_preview(&b, &eng, 1); /* hold preview */
            engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
            engine_preview_stop(&eng);
            engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);
        }
    }

    /* Unbound cursor / empty browser must be soft failures. */
    {
        EpicycleSampleBrowser empty;
        sample_browser_init(&empty);
        if (sample_browser_preview(&empty, &eng, 0) == 0) {
            rc = -40;
            goto shut;
        }
        empty.count = 0;
        empty.cursor = 99;
        if (sample_browser_preview(&empty, &eng, 1) == 0) {
            rc = -41;
            goto shut;
        }
        sample_browser_move(&empty, 5); /* no-op, no crash */
    }

    /* Assign a known-good file; skip until stereo.wav is under cursor. */
    {
        int found = 0;
        for (int i = 0; i < b.count; i++) {
            b.cursor = i;
            if (strcmp(b.names[i], "stereo.wav") == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            rc = -50;
            goto shut;
        }
        if (sample_browser_assign(&b, &eng, 0) != 0) {
            rc = -51;
            goto shut;
        }
        if (!eng.pads[0].sample || !eng.pads[0].sample->data) {
            rc = -52;
            goto shut;
        }
        /* Oversized assign must fail softly. */
        for (int i = 0; i < b.count; i++) {
            if (strcmp(b.names[i], "huge.wav") == 0) {
                b.cursor = i;
                if (sample_browser_assign(&b, &eng, 1) == 0) {
                    rc = -53;
                    goto shut;
                }
                break;
            }
        }
    }

    /* Drain retire TTLs. */
    for (int k = 0; k < 16; k++)
        engine_render(&eng, L, R, EPICYCLE_AUDIO_FRAMES);

shut:
    engine_shutdown(&eng);

done:
    /* Best-effort cleanup of temp files. */
    unlink(path_mono);
    unlink(path_stereo);
    unlink(path_short);
    unlink(path_empty);
    unlink(path_huge);
    unlink(path_bad);
    unlink(path_empty_name);
    rmdir(dir);
    return rc;
}
