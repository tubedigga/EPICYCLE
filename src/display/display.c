/* SPDX-License-Identifier: MIT
 *
 * Bring-up scaffold for the 320×240 panel (ST7796S / tinydrm → /dev/fb0).
 * Real DRM/fb presentation and click-free handoff are Phase 1 hardware work.
 * Patterns documented in SampleBoot TECHNICAL.md and sample-base HARDWARE.md.
 */
#include "epicycle.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef EPICYCLE_FB_PATH
#define EPICYCLE_FB_PATH "/dev/fb0"
#endif

static uint32_t g_pixels[EPICYCLE_FB_W * EPICYCLE_FB_H];
static int g_fb_fd = -1;
static int g_opened;

int display_open(void)
{
    memset(g_pixels, 0, sizeof g_pixels);
    g_fb_fd = open(EPICYCLE_FB_PATH, O_RDWR);
    g_opened = 1;
    /* Missing fb0 is expected on host builds — soft-fail. */
    return 0;
}

uint32_t *display_pixels(void)
{
    return g_pixels;
}

void display_clear(uint32_t bgrx)
{
    for (size_t i = 0; i < (size_t)EPICYCLE_FB_W * EPICYCLE_FB_H; i++)
        g_pixels[i] = bgrx;
}

void display_present(void)
{
    if (g_fb_fd < 0)
        return;
    /* Stub: write raw BGRX; real code must honor stride / format from fb_var. */
    if (lseek(g_fb_fd, 0, SEEK_SET) < 0)
        return;
    ssize_t n = write(g_fb_fd, g_pixels, sizeof g_pixels);
    (void)n;
}

void display_close(void)
{
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
    }
    g_opened = 0;
    (void)g_opened;
}
