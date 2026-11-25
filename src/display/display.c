/* SPDX-License-Identifier: MIT
 *
 * 320×240 panel via /dev/fb0 (stock st7796s tinydrm / DRM fb).
 *
 * SampleBoot TECHNICAL.md + sampleboot-ui present path (public docs / MIT UI):
 *   write contiguous 320×240 BGRX (B,G,R,0 per pixel) with write(2) after
 *   lseek(0). Do NOT rely on mmap alone — DRM fbdev often never dirty-flushes
 *   mmap writes to the SPI panel, which shows as a blank/black screen while
 *   audio still runs.
 *
 * ioctl VSCREENINFO/FSCREENINFO are logged for Ash's session log (bpp/stride);
 * they do not select the present format. Hardware fact: panel expects BGRX.
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef EPICYCLE_FB_PATH
#define EPICYCLE_FB_PATH "/dev/fb0"
#endif

#define FRAME_BYTES ((size_t)EPICYCLE_FB_W * (size_t)EPICYCLE_FB_H * 4u)

static uint32_t g_pixels[EPICYCLE_FB_W * EPICYCLE_FB_H];
static int g_fb_fd = -1;
static int g_ok;
static char g_info[96] = "display closed";
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;
static int g_have_ioctl;

int display_open(void)
{
    memset(g_pixels, 0, sizeof g_pixels);
    g_ok = 0;
    g_have_ioctl = 0;
    memset(&g_vinfo, 0, sizeof g_vinfo);
    memset(&g_finfo, 0, sizeof g_finfo);
    snprintf(g_info, sizeof g_info, "no /dev/fb0");

    /* SampleBoot opens O_WRONLY; O_RDWR is fine and keeps write() working. */
    g_fb_fd = open(EPICYCLE_FB_PATH, O_RDWR | O_CLOEXEC);
    if (g_fb_fd < 0)
        g_fb_fd = open(EPICYCLE_FB_PATH, O_WRONLY | O_CLOEXEC);
    if (g_fb_fd < 0) {
        snprintf(g_info, sizeof g_info, "fb missing (%s)", strerror(errno));
        return -1;
    }

    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) == 0 &&
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) == 0) {
        g_have_ioctl = 1;
        snprintf(g_info, sizeof g_info, "%ux%u %ubpp stride=%u write-bgrx",
                 g_vinfo.xres, g_vinfo.yres, g_vinfo.bits_per_pixel,
                 g_finfo.line_length);
    } else {
        snprintf(g_info, sizeof g_info, "fb open (no ioctl) write-bgrx");
    }

    g_ok = 1;
    return 0;
}

uint32_t *display_pixels(void)
{
    return g_pixels;
}

void display_clear(uint32_t rgb)
{
    for (size_t i = 0; i < (size_t)EPICYCLE_FB_W * EPICYCLE_FB_H; i++)
        g_pixels[i] = rgb & 0x00ffffffu;
}

void display_present(void)
{
    if (g_fb_fd < 0 || !g_ok)
        return;

    /* Contiguous BGRX — same byte order SampleBoot uses for /dev/fb0. */
    static uint8_t out[FRAME_BYTES];
    for (size_t i = 0; i < (size_t)EPICYCLE_FB_W * EPICYCLE_FB_H; i++) {
        uint32_t p = g_pixels[i];
        out[i * 4 + 0] = (uint8_t)(p & 255u);        /* B */
        out[i * 4 + 1] = (uint8_t)((p >> 8) & 255u);  /* G */
        out[i * 4 + 2] = (uint8_t)((p >> 16) & 255u); /* R */
        out[i * 4 + 3] = 0;                           /* X */
    }

    if (lseek(g_fb_fd, 0, SEEK_SET) < 0 && errno != ESPIPE)
        return;

    size_t done = 0;
    while (done < FRAME_BYTES) {
        ssize_t n = write(g_fb_fd, out + done, FRAME_BYTES - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }

    /* Some DRM fbdev backends still need an explicit pan to push SPI. */
    if (g_have_ioctl) {
        g_vinfo.xoffset = 0;
        g_vinfo.yoffset = 0;
        (void)ioctl(g_fb_fd, FBIOPAN_DISPLAY, &g_vinfo);
    }
}

void display_close(void)
{
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
    }
    g_ok = 0;
    g_have_ioctl = 0;
    snprintf(g_info, sizeof g_info, "display closed");
}

int display_ok(void)
{
    return g_ok;
}

const char *display_info(void)
{
    return g_info;
}
