/* SPDX-License-Identifier: MIT
 *
 * 320×240 panel via /dev/fb0 (stock st7796s tinydrm / DRM fb).
 * Queries FBIOGET_VSCREENINFO + FBIOGET_FSCREENINFO for stride and bpp;
 * converts the software 0x00RRGGBB buffer to RGB565 or 32-bit as needed.
 * Patterns documented in SampleBoot TECHNICAL.md and sample-base HARDWARE.md.
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef EPICYCLE_FB_PATH
#define EPICYCLE_FB_PATH "/dev/fb0"
#endif

static uint32_t g_pixels[EPICYCLE_FB_W * EPICYCLE_FB_H];
static int g_fb_fd = -1;
static int g_ok;
static char g_info[64] = "display closed";
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;
static uint8_t *g_map;
static size_t g_map_len;
static int g_use_mmap;

static uint16_t rgb_to_565(uint32_t rgb)
{
    unsigned r = (rgb >> 16) & 0xff;
    unsigned g = (rgb >> 8) & 0xff;
    unsigned b = rgb & 0xff;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t pack_device32(uint32_t rgb)
{
    unsigned r = (rgb >> 16) & 0xff;
    unsigned g = (rgb >> 8) & 0xff;
    unsigned b = rgb & 0xff;
    /* Prefer channel offsets from fb_var when present. */
    if (g_vinfo.red.length || g_vinfo.green.length || g_vinfo.blue.length) {
        uint32_t out = 0;
        out |= (r >> (8 - g_vinfo.red.length)) << g_vinfo.red.offset;
        out |= (g >> (8 - g_vinfo.green.length)) << g_vinfo.green.offset;
        out |= (b >> (8 - g_vinfo.blue.length)) << g_vinfo.blue.offset;
        if (g_vinfo.transp.length)
            out |= ((0xffu >> (8 - g_vinfo.transp.length)) << g_vinfo.transp.offset);
        return out;
    }
    /* SampleBoot / tinydrm default memory layout: 0x00RRGGBB. */
    return rgb & 0x00ffffffu;
}

int display_open(void)
{
    memset(g_pixels, 0, sizeof g_pixels);
    g_ok = 0;
    g_use_mmap = 0;
    g_map = NULL;
    g_map_len = 0;
    snprintf(g_info, sizeof g_info, "no /dev/fb0");

    g_fb_fd = open(EPICYCLE_FB_PATH, O_RDWR | O_CLOEXEC);
    if (g_fb_fd < 0) {
        /* Host / CI: soft-fail so smoke tests still run. */
        snprintf(g_info, sizeof g_info, "fb missing (%s)", strerror(errno));
        return -1;
    }

    memset(&g_vinfo, 0, sizeof g_vinfo);
    memset(&g_finfo, 0, sizeof g_finfo);
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) < 0 ||
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) < 0) {
        snprintf(g_info, sizeof g_info, "fb ioctl failed");
        close(g_fb_fd);
        g_fb_fd = -1;
        return -1;
    }

    if (g_finfo.line_length == 0)
        g_finfo.line_length = (g_vinfo.xres * g_vinfo.bits_per_pixel) / 8;

    g_map_len = g_finfo.smem_len ? g_finfo.smem_len
                                 : (size_t)g_finfo.line_length * g_vinfo.yres_virtual;
    if (g_map_len == 0)
        g_map_len = (size_t)g_finfo.line_length * EPICYCLE_FB_H;

    g_map = mmap(NULL, g_map_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_map != MAP_FAILED) {
        g_use_mmap = 1;
    } else {
        g_map = NULL;
        g_use_mmap = 0;
    }

    g_ok = 1;
    snprintf(g_info, sizeof g_info, "%ux%u %ubpp %s", g_vinfo.xres, g_vinfo.yres,
             g_vinfo.bits_per_pixel, g_use_mmap ? "mmap" : "write");
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

static void blit_row32(uint8_t *dst, const uint32_t *src, int width)
{
    uint32_t *d = (uint32_t *)dst;
    for (int x = 0; x < width; x++)
        d[x] = pack_device32(src[x]);
}

static void blit_row16(uint8_t *dst, const uint32_t *src, int width)
{
    uint16_t *d = (uint16_t *)dst;
    for (int x = 0; x < width; x++)
        d[x] = rgb_to_565(src[x]);
}

void display_present(void)
{
    if (g_fb_fd < 0 || !g_ok)
        return;

    unsigned bpp = g_vinfo.bits_per_pixel ? g_vinfo.bits_per_pixel : 32;
    unsigned xres = g_vinfo.xres ? g_vinfo.xres : EPICYCLE_FB_W;
    unsigned yres = g_vinfo.yres ? g_vinfo.yres : EPICYCLE_FB_H;
    int copy_w = (int)xres < EPICYCLE_FB_W ? (int)xres : EPICYCLE_FB_W;
    int copy_h = (int)yres < EPICYCLE_FB_H ? (int)yres : EPICYCLE_FB_H;
    size_t stride = g_finfo.line_length;

    if (g_use_mmap && g_map) {
        for (int y = 0; y < copy_h; y++) {
            uint8_t *dst = g_map + (size_t)y * stride;
            const uint32_t *src = g_pixels + y * EPICYCLE_FB_W;
            if (bpp <= 16)
                blit_row16(dst, src, copy_w);
            else
                blit_row32(dst, src, copy_w);
        }
        return;
    }

    /* Fallback: assemble a contiguous frame and write(2). */
    size_t row_bytes = (bpp <= 16) ? (size_t)copy_w * 2 : (size_t)copy_w * 4;
    size_t frame = row_bytes * (size_t)copy_h;
    uint8_t *tmp = malloc(frame);
    if (!tmp)
        return;
    for (int y = 0; y < copy_h; y++) {
        uint8_t *dst = tmp + (size_t)y * row_bytes;
        const uint32_t *src = g_pixels + y * EPICYCLE_FB_W;
        if (bpp <= 16)
            blit_row16(dst, src, copy_w);
        else
            blit_row32(dst, src, copy_w);
    }
    if (lseek(g_fb_fd, 0, SEEK_SET) >= 0 || errno == ESPIPE) {
        size_t done = 0;
        while (done < frame) {
            ssize_t n = write(g_fb_fd, tmp + done, frame - done);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            done += (size_t)n;
        }
    }
    free(tmp);
}

void display_close(void)
{
    if (g_map && g_use_mmap) {
        munmap(g_map, g_map_len);
        g_map = NULL;
        g_use_mmap = 0;
    }
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
    }
    g_ok = 0;
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
