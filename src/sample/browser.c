/* SPDX-License-Identifier: MIT
 *
 * Live WAV browser for Epicycle/samples — assign any file onto a pad
 * without rebuilding the SD autorun (Ash 0.3.8).
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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
    if (b->count == 0)
        snprintf(b->status, sizeof b->status, "no wav — put in samples/");
    else
        snprintf(b->status, sizeof b->status, "%d wavs  enc=load", b->count);
    return b->count;
}

void sample_browser_move(EpicycleSampleBrowser *b, int delta)
{
    if (!b || b->count <= 0 || delta == 0)
        return;
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
    if (engine_preview_wav(eng, b->paths[b->cursor], hold) != 0) {
        snprintf(b->status, sizeof b->status, "preview fail");
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
    if (engine_load_bank_wav(eng, pad, path) != 0) {
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
