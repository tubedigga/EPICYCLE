/* SPDX-License-Identifier: MIT
 *
 * 320×240 panel on the MPC Sample (st7796s tinydrm).
 *
 * Ash's 1.4 last-session.log (v0.2.2-panel-fix): after stopping ac50,
 * `/dev/fb0` is gone ("fb missing"). SampleBoot's write(2) path therefore
 * cannot work once stock has exited. Community MIT apps (MPC-600-S,
 * mpc-sample-tracker) take over via DRM/KMS on `/dev/dri/card0`:
 *   SET_MASTER → CREATE_DUMB → ADDFB2 (XRGB8888) → MAP_DUMB → SETCRTC → DIRTYFB
 *
 * Primary path: true DRM dumb buffers (double-buffered) + DIRTYFB.
 * Fallback: `/dev/fb0` write/mmap if the node exists (host / older fw), with
 * RGB565 vs BGRX from ioctl bpp, stride packing, FBIOBLANK unblank, pan.
 *
 * Software pixels stay 0x00RRGGBB; LE memory is B,G,R,X = XRGB8888 / BGRX.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"
#include "drm_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef EPICYCLE_FB_PATH
#define EPICYCLE_FB_PATH "/dev/fb0"
#endif
#ifndef EPICYCLE_DRM_PATH
#define EPICYCLE_DRM_PATH "/dev/dri/card0"
#endif

#define FRAME_PX ((size_t)EPICYCLE_FB_W * (size_t)EPICYCLE_FB_H)
#define DIAG_MAX 24
#define DIAG_LEN 160

enum { DISP_NONE = 0, DISP_DRM, DISP_FBDEV };

typedef struct {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    uint8_t *map;
} DrmBuf;

static uint32_t g_pixels[EPICYCLE_FB_W * EPICYCLE_FB_H];
static int g_ok;
static int g_mode = DISP_NONE;
static char g_info[128] = "display closed";

static char g_diag[DIAG_MAX][DIAG_LEN];
static int g_diag_n;
static int g_diag_head;

/* --- DRM state --- */
static int g_drm_fd = -1;
static uint32_t g_crtc_id, g_conn_id;
static struct epic_drm_modeinfo g_modeinfo;
static DrmBuf g_drm_buf[2];
static int g_drm_front; /* index currently on CRTC */
static int g_drm_have_mode;

/* --- fbdev fallback --- */
static int g_fb_fd = -1;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;
static int g_have_ioctl;
static int g_fb_bpp; /* 16 or 32 */
static int g_fb_bgr; /* 1 = write B,G,R order for 32bpp */
static uint8_t *g_fb_map;
static size_t g_fb_map_sz;

/* present stats */
static size_t g_last_write_bytes;
static int g_last_errno;
static unsigned g_present_count;

static void dlog(const char *fmt, ...)
{
    char line[DIAG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    snprintf(g_diag[g_diag_head], DIAG_LEN, "%s", line);
    g_diag_head = (g_diag_head + 1) % DIAG_MAX;
    if (g_diag_n < DIAG_MAX)
        g_diag_n++;
    fprintf(stderr, "display: %s\n", line);
}

static int ioctl_retry(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static void drm_destroy_buf(DrmBuf *b)
{
    if (!b)
        return;
    if (b->map && b->map != MAP_FAILED && b->size) {
        munmap(b->map, (size_t)b->size);
        b->map = NULL;
    }
    if (g_drm_fd >= 0 && b->fb_id) {
        uint32_t id = b->fb_id;
        (void)ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_RMFB, &id);
        b->fb_id = 0;
    }
    if (g_drm_fd >= 0 && b->handle) {
        struct epic_drm_destroy_dumb d = {.handle = b->handle};
        (void)ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        b->handle = 0;
    }
    b->pitch = 0;
    b->size = 0;
}

static int drm_create_buf(DrmBuf *b)
{
    memset(b, 0, sizeof *b);
    struct epic_drm_create_dumb dumb = {
        .height = EPICYCLE_FB_H,
        .width = EPICYCLE_FB_W,
        .bpp = 32,
    };
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
        dlog("CREATE_DUMB: %s", strerror(errno));
        return -1;
    }
    b->handle = dumb.handle;
    b->pitch = dumb.pitch;
    b->size = dumb.size;

    struct epic_drm_fb_cmd2 add;
    memset(&add, 0, sizeof add);
    add.width = EPICYCLE_FB_W;
    add.height = EPICYCLE_FB_H;
    add.pixel_format = EPIC_DRM_FORMAT_XRGB8888;
    add.handles[0] = dumb.handle;
    add.pitches[0] = dumb.pitch;
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_ADDFB2, &add) < 0) {
        dlog("ADDFB2: %s", strerror(errno));
        drm_destroy_buf(b);
        return -1;
    }
    b->fb_id = add.fb_id;

    struct epic_drm_map_dumb map = {.handle = dumb.handle};
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        dlog("MAP_DUMB: %s", strerror(errno));
        drm_destroy_buf(b);
        return -1;
    }
    void *p = mmap(NULL, (size_t)dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_drm_fd,
                   (off_t)map.offset);
    if (p == MAP_FAILED) {
        dlog("mmap dumb: %s", strerror(errno));
        drm_destroy_buf(b);
        return -1;
    }
    b->map = p;
    memset(b->map, 0, (size_t)b->size);
    return 0;
}

static int drm_dirty(uint32_t fb_id)
{
    struct epic_drm_clip_rect clip = {0, 0, EPICYCLE_FB_W, EPICYCLE_FB_H};
    struct epic_drm_fb_dirty_cmd cmd = {
        .fb_id = fb_id,
        .num_clips = 1,
        .clips_ptr = (uint64_t)(uintptr_t)&clip,
    };
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_DIRTYFB, &cmd) < 0) {
        g_last_errno = errno;
        dlog("DIRTYFB fb=%u: %s", fb_id, strerror(errno));
        return -1;
    }
    g_last_errno = 0;
    return 0;
}

static int drm_setcrtc(uint32_t fb_id)
{
    struct epic_drm_crtc set;
    memset(&set, 0, sizeof set);
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&g_conn_id;
    set.count_connectors = 1;
    set.crtc_id = g_crtc_id;
    set.fb_id = fb_id;
    set.mode = g_modeinfo;
    set.mode_valid = 1;
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        dlog("SETCRTC fb=%u: %s", fb_id, strerror(errno));
        return -1;
    }
    return 0;
}

static void drm_blit(DrmBuf *b)
{
    /* 0x00RRGGBB LE == B,G,R,X == XRGB8888 */
    for (int y = 0; y < EPICYCLE_FB_H; y++) {
        memcpy(b->map + (size_t)y * b->pitch, g_pixels + (size_t)y * EPICYCLE_FB_W,
               (size_t)EPICYCLE_FB_W * 4u);
    }
}

static void drm_close(void)
{
    if (g_drm_fd >= 0 && g_drm_buf[0].map) {
        memset(g_drm_buf[g_drm_front].map, 0, (size_t)g_drm_buf[g_drm_front].size);
        (void)drm_dirty(g_drm_buf[g_drm_front].fb_id);
    }
    drm_destroy_buf(&g_drm_buf[0]);
    drm_destroy_buf(&g_drm_buf[1]);
    if (g_drm_fd >= 0) {
        (void)ioctl(g_drm_fd, EPIC_DRM_IOCTL_DROP_MASTER, 0);
        close(g_drm_fd);
        g_drm_fd = -1;
    }
    g_drm_have_mode = 0;
}

static int drm_open(void)
{
    g_drm_fd = open(EPICYCLE_DRM_PATH, O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) {
        dlog("open %s: %s", EPICYCLE_DRM_PATH, strerror(errno));
        return -1;
    }

    /* Stock may still hold master briefly after systemctl stop ac50. */
    int master = 0;
    for (int i = 0; i < 50 && !master; i++) {
        if (ioctl(g_drm_fd, EPIC_DRM_IOCTL_SET_MASTER, 0) == 0) {
            master = 1;
            break;
        }
        if (errno != EBUSY) {
            dlog("SET_MASTER: %s", strerror(errno));
            break;
        }
        usleep(20000);
    }
    if (!master) {
        dlog("SET_MASTER failed (ac50 still holding?)");
        drm_close();
        return -1;
    }
    dlog("SET_MASTER ok on %s", EPICYCLE_DRM_PATH);

    uint32_t crtcs[4] = {0}, conns[4] = {0};
    struct epic_drm_card_res res = {
        .crtc_id_ptr = (uint64_t)(uintptr_t)crtcs,
        .connector_id_ptr = (uint64_t)(uintptr_t)conns,
        .count_crtcs = 4,
        .count_connectors = 4,
    };
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.count_crtcs ||
        !res.count_connectors) {
        dlog("GETRESOURCES: %s (crtcs=%u conns=%u)", strerror(errno), res.count_crtcs,
             res.count_connectors);
        drm_close();
        return -1;
    }
    g_crtc_id = crtcs[0];
    g_conn_id = conns[0];

    struct epic_drm_get_connector con;
    memset(&con, 0, sizeof con);
    con.connector_id = g_conn_id;
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0 || !con.count_modes) {
        dlog("GETCONNECTOR: %s modes=%u", strerror(errno), con.count_modes);
        drm_close();
        return -1;
    }
    uint32_t nmodes = con.count_modes;
    if (nmodes > 32)
        nmodes = 32;
    struct epic_drm_modeinfo modes[32];
    memset(modes, 0, sizeof modes);
    memset(&con, 0, sizeof con);
    con.connector_id = g_conn_id;
    con.count_modes = nmodes;
    con.modes_ptr = (uint64_t)(uintptr_t)modes;
    if (ioctl_retry(g_drm_fd, EPIC_DRM_IOCTL_MODE_GETCONNECTOR, &con) < 0) {
        dlog("GETCONNECTOR modes: %s", strerror(errno));
        drm_close();
        return -1;
    }

    int m = -1;
    for (uint32_t i = 0; i < nmodes; i++) {
        if (modes[i].hdisplay == EPICYCLE_FB_W && modes[i].vdisplay == EPICYCLE_FB_H) {
            m = (int)i;
            break;
        }
    }
    if (m < 0) {
        dlog("no %dx%d mode (got %u modes, first %ux%u)", EPICYCLE_FB_W, EPICYCLE_FB_H, nmodes,
             modes[0].hdisplay, modes[0].vdisplay);
        drm_close();
        return -1;
    }
    g_modeinfo = modes[m];
    g_drm_have_mode = 1;

    if (drm_create_buf(&g_drm_buf[0]) < 0 || drm_create_buf(&g_drm_buf[1]) < 0) {
        drm_close();
        return -1;
    }

    g_drm_front = 0;
    if (drm_setcrtc(g_drm_buf[0].fb_id) < 0) {
        drm_close();
        return -1;
    }
    if (drm_dirty(g_drm_buf[0].fb_id) < 0) {
        /* Some kernels accept SETCRTC but still need DIRTYFB for SPI push. */
        dlog("initial DIRTYFB failed — continuing");
    }

    snprintf(g_info, sizeof g_info, "drm %ux%u XR24 pitch=%u dblbuf dirtyfb",
             g_modeinfo.hdisplay, g_modeinfo.vdisplay, g_drm_buf[0].pitch);
    dlog("open ok: %s handles=%u/%u fb=%u/%u", g_info, g_drm_buf[0].handle, g_drm_buf[1].handle,
         g_drm_buf[0].fb_id, g_drm_buf[1].fb_id);
    g_mode = DISP_DRM;
    g_ok = 1;
    return 0;
}

static void fbdev_close(void)
{
    if (g_fb_map && g_fb_map != MAP_FAILED) {
        munmap(g_fb_map, g_fb_map_sz);
        g_fb_map = NULL;
        g_fb_map_sz = 0;
    }
    if (g_fb_fd >= 0) {
        close(g_fb_fd);
        g_fb_fd = -1;
    }
    g_have_ioctl = 0;
}

static int fbdev_open(void)
{
    /* SampleBoot uses O_WRONLY; try that first, then RDWR for mmap. */
    g_fb_fd = open(EPICYCLE_FB_PATH, O_WRONLY | O_CLOEXEC);
    if (g_fb_fd < 0)
        g_fb_fd = open(EPICYCLE_FB_PATH, O_RDWR | O_CLOEXEC);
    if (g_fb_fd < 0) {
        dlog("open %s: %s", EPICYCLE_FB_PATH, strerror(errno));
        return -1;
    }

    g_have_ioctl = 0;
    g_fb_bpp = 32;
    g_fb_bgr = 1;
    memset(&g_vinfo, 0, sizeof g_vinfo);
    memset(&g_finfo, 0, sizeof g_finfo);
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &g_vinfo) == 0 &&
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &g_finfo) == 0) {
        g_have_ioctl = 1;
        g_fb_bpp = (int)g_vinfo.bits_per_pixel;
        /* Prefer BGRX when blue is in the low bits (SampleBoot default). */
        g_fb_bgr = (g_vinfo.blue.offset < g_vinfo.red.offset) ? 1 : 0;
        dlog("fbdev ioctl %ux%u %ubpp stride=%u r@%u g@%u b@%u", g_vinfo.xres, g_vinfo.yres,
             g_vinfo.bits_per_pixel, g_finfo.line_length, g_vinfo.red.offset, g_vinfo.green.offset,
             g_vinfo.blue.offset);
    } else {
        dlog("fbdev open (no ioctl) assuming 32bpp BGRX");
    }

    /* Unblank if the node supports it. */
    {
        int blank = 0; /* VESA_NO_BLANKING */
        if (ioctl(g_fb_fd, FBIOBLANK, blank) < 0 && errno != EINVAL && errno != ENOTTY)
            dlog("FBIOBLANK: %s", strerror(errno));
        else
            dlog("FBIOBLANK unblank ok/noop");
    }

    /* Optional mmap for stride-aware present when RDWR. */
    if (g_have_ioctl && g_finfo.smem_len > 0) {
        int rdwr = open(EPICYCLE_FB_PATH, O_RDWR | O_CLOEXEC);
        if (rdwr >= 0) {
            close(g_fb_fd);
            g_fb_fd = rdwr;
            g_fb_map_sz = g_finfo.smem_len;
            g_fb_map = mmap(NULL, g_fb_map_sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
            if (g_fb_map == MAP_FAILED) {
                dlog("fbdev mmap: %s — will use write(2)", strerror(errno));
                g_fb_map = NULL;
                g_fb_map_sz = 0;
            } else {
                dlog("fbdev mmap %zu bytes", g_fb_map_sz);
            }
        }
    }

    if (g_have_ioctl)
        snprintf(g_info, sizeof g_info, "fbdev %ux%u %ubpp stride=%u %s", g_vinfo.xres, g_vinfo.yres,
                 g_vinfo.bits_per_pixel, g_finfo.line_length, g_fb_bpp <= 16 ? "rgb565" : (g_fb_bgr ? "bgrx" : "rgbx"));
    else
        snprintf(g_info, sizeof g_info, "fbdev write-bgrx (no ioctl)");

    g_mode = DISP_FBDEV;
    g_ok = 1;
    dlog("fbdev fallback: %s", g_info);
    return 0;
}

static void fbdev_present(void)
{
    uint32_t stride = g_have_ioctl && g_finfo.line_length ? g_finfo.line_length
                                                          : (uint32_t)EPICYCLE_FB_W * 4u;
    g_last_write_bytes = 0;
    g_last_errno = 0;

    if (g_fb_bpp <= 16) {
        /* RGB565 pack; honour red/blue offset when ioctl present. */
        static uint16_t line[EPICYCLE_FB_W];
        int swap_rb = g_have_ioctl && g_vinfo.blue.offset > g_vinfo.red.offset;
        for (int y = 0; y < EPICYCLE_FB_H; y++) {
            for (int x = 0; x < EPICYCLE_FB_W; x++) {
                uint32_t p = g_pixels[y * EPICYCLE_FB_W + x];
                uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                if (swap_rb) {
                    uint32_t t = r;
                    r = b;
                    b = t;
                }
                line[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            if (g_fb_map) {
                memcpy(g_fb_map + (size_t)y * stride, line, sizeof line);
                g_last_write_bytes += sizeof line;
            } else {
                off_t off = (off_t)y * (off_t)stride;
                if (lseek(g_fb_fd, off, SEEK_SET) < 0) {
                    g_last_errno = errno;
                    break;
                }
                size_t done = 0;
                while (done < sizeof line) {
                    ssize_t n = write(g_fb_fd, (uint8_t *)line + done, sizeof line - done);
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        g_last_errno = errno;
                        break;
                    }
                    if (n == 0)
                        break;
                    done += (size_t)n;
                }
                g_last_write_bytes += done;
                if (g_last_errno)
                    break;
            }
        }
    } else if (g_fb_map) {
        for (int y = 0; y < EPICYCLE_FB_H; y++) {
            uint8_t *dst = g_fb_map + (size_t)y * stride;
            for (int x = 0; x < EPICYCLE_FB_W; x++) {
                uint32_t p = g_pixels[y * EPICYCLE_FB_W + x];
                if (g_fb_bgr) {
                    dst[x * 4 + 0] = (uint8_t)(p & 255);
                    dst[x * 4 + 1] = (uint8_t)((p >> 8) & 255);
                    dst[x * 4 + 2] = (uint8_t)((p >> 16) & 255);
                    dst[x * 4 + 3] = 0;
                } else {
                    dst[x * 4 + 0] = (uint8_t)((p >> 16) & 255);
                    dst[x * 4 + 1] = (uint8_t)((p >> 8) & 255);
                    dst[x * 4 + 2] = (uint8_t)(p & 255);
                    dst[x * 4 + 3] = 0;
                }
            }
            g_last_write_bytes += (size_t)EPICYCLE_FB_W * 4u;
        }
    } else {
        /* Contiguous BGRX write — SampleBoot path when node exists. */
        static uint8_t out[FRAME_PX * 4u];
        for (size_t i = 0; i < FRAME_PX; i++) {
            uint32_t p = g_pixels[i];
            if (g_fb_bgr) {
                out[i * 4 + 0] = (uint8_t)(p & 255);
                out[i * 4 + 1] = (uint8_t)((p >> 8) & 255);
                out[i * 4 + 2] = (uint8_t)((p >> 16) & 255);
            } else {
                out[i * 4 + 0] = (uint8_t)((p >> 16) & 255);
                out[i * 4 + 1] = (uint8_t)((p >> 8) & 255);
                out[i * 4 + 2] = (uint8_t)(p & 255);
            }
            out[i * 4 + 3] = 0;
        }
        if (lseek(g_fb_fd, 0, SEEK_SET) < 0 && errno != ESPIPE) {
            g_last_errno = errno;
            dlog("lseek: %s", strerror(errno));
            return;
        }
        size_t done = 0;
        size_t total = FRAME_PX * 4u;
        while (done < total) {
            ssize_t n = write(g_fb_fd, out + done, total - done);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                g_last_errno = errno;
                break;
            }
            if (n == 0)
                break;
            done += (size_t)n;
        }
        g_last_write_bytes = done;
    }

    if (g_have_ioctl) {
        g_vinfo.xoffset = 0;
        g_vinfo.yoffset = 0;
        if (ioctl(g_fb_fd, FBIOPAN_DISPLAY, &g_vinfo) < 0 && errno != EINVAL && errno != ENOTTY)
            dlog("FBIOPAN_DISPLAY: %s", strerror(errno));
    }

    if (g_present_count < 3 || g_last_errno)
        dlog("fbdev present #%u wrote=%zu errno=%d (%s)", g_present_count, g_last_write_bytes,
             g_last_errno, g_last_errno ? strerror(g_last_errno) : "ok");
}

static void drm_present(void)
{
    int back = 1 - g_drm_front;
    DrmBuf *b = &g_drm_buf[back];
    drm_blit(b);
    /* Flip CRTC to back buffer, then dirty-flush SPI (tinydrm only paints on DIRTYFB). */
    if (drm_setcrtc(b->fb_id) == 0) {
        g_drm_front = back;
    } else {
        /* Flip failed — paint the front buffer in place. */
        b = &g_drm_buf[g_drm_front];
        drm_blit(b);
    }
    (void)drm_dirty(b->fb_id);
    g_last_write_bytes = FRAME_PX * 4u;
    if (g_present_count < 3 || g_last_errno)
        dlog("drm present #%u front=%d fb=%u pitch=%u dirty_errno=%d", g_present_count, g_drm_front,
             b->fb_id, b->pitch, g_last_errno);
}

int display_open(void)
{
    memset(g_pixels, 0, sizeof g_pixels);
    g_ok = 0;
    g_mode = DISP_NONE;
    g_present_count = 0;
    g_last_write_bytes = 0;
    g_last_errno = 0;
    g_diag_n = 0;
    g_diag_head = 0;
    snprintf(g_info, sizeof g_info, "display closed");

    dlog("probe: prefer %s (fb0 often missing after ac50 stop)", EPICYCLE_DRM_PATH);
    if (drm_open() == 0)
        return 0;

    dlog("DRM failed — trying %s", EPICYCLE_FB_PATH);
    if (fbdev_open() == 0)
        return 0;

    snprintf(g_info, sizeof g_info, "no drm/fb (%s)", strerror(errno));
    dlog("open failed: %s", g_info);
    return -1;
}

uint32_t *display_pixels(void)
{
    return g_pixels;
}

void display_clear(uint32_t rgb)
{
    for (size_t i = 0; i < FRAME_PX; i++)
        g_pixels[i] = rgb & 0x00ffffffu;
}

void display_present(void)
{
    if (!g_ok)
        return;
    if (g_mode == DISP_DRM)
        drm_present();
    else if (g_mode == DISP_FBDEV)
        fbdev_present();
    g_present_count++;
}

void display_close(void)
{
    if (g_mode == DISP_DRM)
        drm_close();
    else if (g_mode == DISP_FBDEV)
        fbdev_close();
    g_mode = DISP_NONE;
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

int display_log_count(void)
{
    return g_diag_n;
}

const char *display_log_line(int i)
{
    if (i < 0 || i >= g_diag_n)
        return "";
    int idx;
    if (g_diag_n < DIAG_MAX)
        idx = i;
    else
        idx = (g_diag_head + i) % DIAG_MAX;
    return g_diag[idx];
}
