/* SPDX-License-Identifier: MIT
 *
 * Card-root resolution follows the SampleBoot OS-AUTHORS pattern:
 * longest /proc/mounts prefix of the app directory (not dirname of $0).
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void unescape_mount(char *dst, size_t dst_sz, const char *src)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dst_sz; ) {
        if (src[i] == '\\' && src[i + 1] == '0' && src[i + 2] == '4' && src[i + 3] == '0') {
            dst[di++] = ' ';
            i += 4;
        } else {
            dst[di++] = src[i++];
        }
    }
    dst[di] = '\0';
}

static int dirname_of(const char *path, char *out, size_t out_sz)
{
    if (!path || !path[0])
        return -1;
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp)
        return -1;
    char *slash = strrchr(tmp, '/');
    if (!slash) {
        if (snprintf(out, out_sz, ".") >= (int)out_sz)
            return -1;
        return 0;
    }
    if (slash == tmp) {
        if (snprintf(out, out_sz, "/") >= (int)out_sz)
            return -1;
        return 0;
    }
    *slash = '\0';
    if (snprintf(out, out_sz, "%s", tmp) >= (int)out_sz)
        return -1;
    return 0;
}

static int longest_mount_prefix(const char *dir, char *out, size_t out_sz)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return -1;

    char line[1024];
    char best[512] = "";
    size_t best_len = 0;
    char needle[520];

    if (snprintf(needle, sizeof needle, "%s/", dir) >= (int)sizeof needle) {
        fclose(fp);
        return -1;
    }

    while (fgets(line, sizeof line, fp)) {
        char dev[256], mnt_esc[512], rest[256];
        if (sscanf(line, "%255s %511s %255s", dev, mnt_esc, rest) < 2)
            continue;
        char mnt[512];
        unescape_mount(mnt, sizeof mnt, mnt_esc);
        if (strcmp(mnt, "/") == 0)
            continue;
        size_t n = strlen(mnt);
        if (n == 0 || n <= best_len)
            continue;
        if (strncmp(needle, mnt, n) == 0 && (needle[n] == '/' || needle[n] == '\0')) {
            memcpy(best, mnt, n + 1);
            best_len = n;
        }
    }
    fclose(fp);

    if (best_len == 0)
        return -1;
    if (snprintf(out, out_sz, "%s", best) >= (int)out_sz)
        return -1;
    return 0;
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) == 0)
        return 0;
    if (errno == EEXIST)
        return 0;
    return -1;
}

int epicycle_paths_resolve(EpicyclePaths *out, const char *argv0, const char *env_app_dir)
{
    memset(out, 0, sizeof *out);

    const char *base = env_app_dir && env_app_dir[0] ? env_app_dir : argv0;
    char resolved[512];

    if (base[0] == '/') {
        if (snprintf(resolved, sizeof resolved, "%s", base) >= (int)sizeof resolved)
            return -1;
    } else {
        char cwd[512];
        if (!getcwd(cwd, sizeof cwd))
            return -1;
        if (snprintf(resolved, sizeof resolved, "%s/%s", cwd, base) >= (int)sizeof resolved)
            return -1;
    }

    /* Directory env (MPC_APP_DIR) stays as-is; file argv0 → its directory. */
    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (snprintf(out->app_dir, sizeof out->app_dir, "%s", resolved) >= (int)sizeof out->app_dir)
            return -1;
    } else if (dirname_of(resolved, out->app_dir, sizeof out->app_dir) != 0) {
        return -1;
    }

    if (longest_mount_prefix(out->app_dir, out->card_root, sizeof out->card_root) != 0) {
        /* Host / CI: treat the app folder as the synthetic card root. */
        if (snprintf(out->card_root, sizeof out->card_root, "%s", out->app_dir) >=
            (int)sizeof out->card_root)
            return -1;
    }

    if (snprintf(out->data_dir, sizeof out->data_dir, "%s/Epicycle", out->card_root) >=
        (int)sizeof out->data_dir)
        return -1;
    if (snprintf(out->log_path, sizeof out->log_path, "%s/logs/last-session.log", out->data_dir) >=
        (int)sizeof out->log_path)
        return -1;

    (void)ensure_dir(out->data_dir);
    char logs[640];
    if (snprintf(logs, sizeof logs, "%s/logs", out->data_dir) < (int)sizeof logs)
        (void)ensure_dir(logs);
    (void)ensure_dir(out->data_dir); /* projects/samples created later */

    return 0;
}

void epicycle_log(const EpicyclePaths *paths, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(stderr, "[%s] %s\n", stamp, buf);

    if (!paths || !paths->log_path[0])
        return;
    FILE *fp = fopen(paths->log_path, "a");
    if (!fp)
        return;
    fprintf(fp, "[%s] %s\n", stamp, buf);
    fclose(fp);
}
