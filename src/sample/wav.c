/* SPDX-License-Identifier: MIT
 *
 * Minimal PCM WAV loader (16/24/32-bit integer or float). Converts to
 * interleaved stereo float. No compressed formats. Host + device.
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sample_free(EpicycleSample *s)
{
    if (!s)
        return;
    free(s->data);
    s->data = NULL;
    s->frames = 0;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

int sample_load_wav(EpicycleSample *out, const char *path)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(fp);
        return -1;
    }

    uint16_t fmt = 0, ch = 0, bps = 0;
    uint32_t rate = 0;
    uint8_t *pcm = NULL;
    uint32_t pcm_bytes = 0;

    for (;;) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, fp) != 8)
            break;
        uint32_t csz = rd32(chdr + 4);
        if (memcmp(chdr, "fmt ", 4) == 0) {
            uint8_t *f = malloc(csz);
            if (!f || fread(f, 1, csz, fp) != csz) {
                free(f);
                fclose(fp);
                return -1;
            }
            fmt = rd16(f);
            ch = rd16(f + 2);
            rate = rd32(f + 4);
            bps = rd16(f + 14);
            free(f);
            if (csz & 1)
                fseek(fp, 1, SEEK_CUR);
        } else if (memcmp(chdr, "data", 4) == 0) {
            pcm = malloc(csz);
            if (!pcm || fread(pcm, 1, csz, fp) != csz) {
                free(pcm);
                fclose(fp);
                return -1;
            }
            pcm_bytes = csz;
            if (csz & 1)
                fseek(fp, 1, SEEK_CUR);
            break;
        } else {
            if (fseek(fp, csz + (csz & 1), SEEK_CUR) != 0)
                break;
        }
    }
    fclose(fp);

    if (!pcm || !rate || !ch || ch > 2 || (fmt != 1 && fmt != 3)) {
        free(pcm);
        return -1;
    }

    int bytes_per = (int)bps / 8;
    if (bytes_per <= 0) {
        free(pcm);
        return -1;
    }
    size_t frames = pcm_bytes / ((size_t)bytes_per * ch);
    if (frames == 0) {
        free(pcm);
        return -1;
    }

    float *data = calloc(frames * 2, sizeof(float));
    if (!data) {
        free(pcm);
        return -1;
    }

    for (size_t i = 0; i < frames; i++) {
        float L = 0, R = 0;
        const uint8_t *p = pcm + i * (size_t)bytes_per * ch;
        for (int c = 0; c < ch; c++) {
            float v = 0;
            const uint8_t *s = p + c * bytes_per;
            if (fmt == 3 && bps == 32) {
                memcpy(&v, s, 4);
            } else if (bps == 16) {
                int16_t n = (int16_t)rd16(s);
                v = (float)n / 32768.0f;
            } else if (bps == 24) {
                int32_t n = (int32_t)((s[0] << 8) | (s[1] << 16) | (s[2] << 24));
                v = (float)(n >> 8) / 8388608.0f;
            } else if (bps == 32 && fmt == 1) {
                int32_t n = (int32_t)rd32(s);
                v = (float)n / 2147483648.0f;
            }
            if (c == 0)
                L = v;
            else
                R = v;
        }
        if (ch == 1)
            R = L;
        data[i * 2] = L;
        data[i * 2 + 1] = R;
    }
    free(pcm);

    out->data = data;
    out->frames = frames;
    out->rate = rate;
    out->channels = 2;
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(out->name, sizeof out->name, "%s", base);
    }
    return 0;
}

int sample_synth_demo(EpicycleSample *out, unsigned rate, float freq_hz, float seconds)
{
    memset(out, 0, sizeof *out);
    if (rate == 0 || seconds <= 0.0f)
        return -1;
    size_t frames = (size_t)(rate * seconds);
    if (frames < 64)
        frames = 64;
    float *data = calloc(frames * 2, sizeof(float));
    if (!data)
        return -1;

    /* Soft saw + sine bed with mild envelope — good for hearing loops / polymeter. */
    for (size_t i = 0; i < frames; i++) {
        float t = (float)i / (float)rate;
        float env = 1.0f;
        float attack = 0.01f;
        float release = 0.05f;
        if (t < attack)
            env = t / attack;
        float rem = seconds - t;
        if (rem < release)
            env *= rem / release;
        float phase = fmodf(t * freq_hz, 1.0f);
        float saw = 2.0f * phase - 1.0f;
        float sine = sinf(2.0f * 3.14159265f * freq_hz * t);
        float v = 0.35f * saw + 0.25f * sine;
        /* Light click markers every beat-ish at 120 BPM for sync demos. */
        float beat = fmodf(t * 2.0f, 1.0f); /* 120 BPM quarter */
        if (beat < 0.01f)
            v += 0.4f * (1.0f - beat / 0.01f);
        v *= env;
        data[i * 2] = v;
        data[i * 2 + 1] = v * 0.95f;
    }

    out->data = data;
    out->frames = frames;
    out->rate = rate;
    out->channels = 2;
    snprintf(out->name, sizeof out->name, "demo-%.0fHz", (double)freq_hz);
    return 0;
}

int sample_normalize(EpicycleSample *s)
{
    if (!s || !s->data || s->frames < 2)
        return -1;
    float peak = 0.0f;
    size_t n = s->frames * 2u;
    for (size_t i = 0; i < n; i++) {
        float a = fabsf(s->data[i]);
        if (a > peak)
            peak = a;
    }
    if (peak < 1e-5f)
        return -1; /* silence — nothing to normalize */
    if (peak >= 0.94f && peak <= 1.0f)
        return 0; /* already hot enough */
    float g = 0.95f / peak;
    for (size_t i = 0; i < n; i++)
        s->data[i] *= g;
    return 0;
}
