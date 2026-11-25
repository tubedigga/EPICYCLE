/* SPDX-License-Identifier: MIT
 *
 * Thin MIT wrapper around stb_image (public domain / MIT) for boot splash.
 * Decode PNG to opaque 0x00RRGGBB (alpha composited onto black).
 */
#define _POSIX_C_SOURCE 200809L

#include "epicycle.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int epicycle_png_load_rgb(const char *path, uint32_t **out_pixels, int *out_w, int *out_h)
{
    if (!path || !out_pixels || !out_w || !out_h)
        return -1;
    *out_pixels = NULL;
    *out_w = 0;
    *out_h = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    unsigned char *file = (unsigned char *)malloc((size_t)sz);
    if (!file) {
        fclose(fp);
        return -1;
    }
    if (fread(file, 1, (size_t)sz, fp) != (size_t)sz) {
        free(file);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    int w = 0, h = 0, n = 0;
    unsigned char *rgba = stbi_load_from_memory(file, (int)sz, &w, &h, &n, 4);
    free(file);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba)
            stbi_image_free(rgba);
        return -1;
    }

    size_t count = (size_t)w * (size_t)h;
    uint32_t *rgb = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!rgb) {
        stbi_image_free(rgba);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        unsigned r = rgba[i * 4 + 0];
        unsigned g = rgba[i * 4 + 1];
        unsigned b = rgba[i * 4 + 2];
        unsigned a = rgba[i * 4 + 3];
        /* Composite onto black letterbox. */
        r = (r * a) / 255u;
        g = (g * a) / 255u;
        b = (b * a) / 255u;
        rgb[i] = (r << 16) | (g << 8) | b;
    }
    stbi_image_free(rgba);

    *out_pixels = rgb;
    *out_w = w;
    *out_h = h;
    return 0;
}

void epicycle_png_free(uint32_t *pixels)
{
    free(pixels);
}
