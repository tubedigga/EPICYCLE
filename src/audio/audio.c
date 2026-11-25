/* SPDX-License-Identifier: MIT
 *
 * AK4558 I2S playback via plain ALSA kernel ioctls (no alsa-lib). Opens the
 * "I2S" card at 44.1 kHz stereo S32_LE. Realtime thread calls engine_render
 * when an engine is bound; otherwise writes silence.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

#ifndef EPICYCLE_SND_DIR
#define EPICYCLE_SND_DIR "/dev/snd"
#endif

#define AUDIO_PERIOD 128
#define AUDIO_PERIODS 4

static int g_pcm_fd = -1;
static int g_running;
static unsigned g_rate = 44100;
static char g_info[64] = "audio closed";
static pthread_t g_thread;
static volatile int g_stop;
static int g_thread_started;
static EpicycleEngine *g_engine;
static pthread_mutex_t g_eng_mu = PTHREAD_MUTEX_INITIALIZER;

static void hw_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m->bits, 0, sizeof m->bits);
    m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void hw_int(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v;
    i->integer = 1;
}

static int configure_pcm(int fd, unsigned period)
{
    struct snd_pcm_hw_params hp;
    memset(&hp, 0, sizeof hp);
    for (int n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; n++)
        memset(hp.masks[n].bits, 0xff, sizeof hp.masks[n].bits);
    for (int n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n++) {
        hp.intervals[n].min = 0;
        hp.intervals[n].max = ~0u;
    }
    hp.rmask = ~0u;
    hp.cmask = 0;
    hp.info = ~0u;

    hw_mask(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_mask(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S32_LE);
    hw_mask(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hw_int(&hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 32);
    hw_int(&hp, SNDRV_PCM_HW_PARAM_FRAME_BITS, 64);
    hw_int(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    hw_int(&hp, SNDRV_PCM_HW_PARAM_RATE, g_rate);
    hw_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
    hw_int(&hp, SNDRV_PCM_HW_PARAM_PERIODS, AUDIO_PERIODS);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0)
        return -1;

    unsigned long buffer = (unsigned long)period * AUDIO_PERIODS;
    struct snd_pcm_sw_params sp;
    memset(&sp, 0, sizeof sp);
    sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sp.period_step = 1;
    sp.avail_min = period;
    sp.start_threshold = period * 2;
    sp.stop_threshold = buffer;
    sp.boundary = buffer;
    while (sp.boundary * 2 <= (unsigned long)(~0ul >> 1) - buffer)
        sp.boundary *= 2;

    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) < 0)
        return -1;
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
        return -1;
    return 0;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    static int32_t interleaved[AUDIO_PERIOD * 2];
    static int32_t L[AUDIO_PERIOD];
    static int32_t R[AUDIO_PERIOD];

    while (!g_stop && g_pcm_fd >= 0) {
        EpicycleEngine *eng = NULL;
        pthread_mutex_lock(&g_eng_mu);
        eng = g_engine;
        pthread_mutex_unlock(&g_eng_mu);

        if (eng)
            engine_render(eng, L, R, AUDIO_PERIOD);
        else
            memset(L, 0, sizeof L), memset(R, 0, sizeof R);

        for (int i = 0; i < AUDIO_PERIOD; i++) {
            interleaved[i * 2] = L[i];
            interleaved[i * 2 + 1] = R[i];
        }

        struct snd_xferi x = {
            .buf = interleaved,
            .frames = AUDIO_PERIOD,
            .result = 0,
        };
        while (!g_stop && ioctl(g_pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE || errno == ESTRPIPE) {
                ioctl(g_pcm_fd, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            usleep(2000);
            break;
        }
    }
    return NULL;
}

void audio_set_engine(EpicycleEngine *eng)
{
    pthread_mutex_lock(&g_eng_mu);
    g_engine = eng;
    pthread_mutex_unlock(&g_eng_mu);
}

int audio_open(void)
{
    g_running = 0;
    g_stop = 0;
    g_thread_started = 0;
    g_pcm_fd = -1;
    snprintf(g_info, sizeof g_info, "no I2S card");

    int card = epicycle_find_alsa_card("I2S");
    if (card < 0) {
        /* Host / CI soft-fail — still allow host offline render via engine. */
        return -1;
    }

    char path[128];
    snprintf(path, sizeof path, "%s/pcmC%dD0p", EPICYCLE_SND_DIR, card);
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(g_info, sizeof g_info, "pcmC%dD0p open fail", card);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    unsigned period = AUDIO_PERIOD;
    if (configure_pcm(fd, period) != 0) {
        period = 512;
        if (configure_pcm(fd, period) != 0) {
            snprintf(g_info, sizeof g_info, "hw params refused");
            close(fd);
            return -1;
        }
    }

    g_pcm_fd = fd;
    g_running = 1;
    snprintf(g_info, sizeof g_info, "I2S #%d %uHz engine", card, g_rate);

    if (pthread_create(&g_thread, NULL, audio_thread, NULL) == 0)
        g_thread_started = 1;
    return 0;
}

void audio_close(void)
{
    g_stop = 1;
    audio_set_engine(NULL);
    if (g_thread_started) {
        pthread_join(g_thread, NULL);
        g_thread_started = 0;
    }
    if (g_pcm_fd >= 0) {
        ioctl(g_pcm_fd, SNDRV_PCM_IOCTL_DROP);
        close(g_pcm_fd);
        g_pcm_fd = -1;
    }
    g_running = 0;
    snprintf(g_info, sizeof g_info, "audio closed");
}

int audio_running(void)
{
    return g_running;
}

unsigned audio_rate(void)
{
    return g_rate;
}

const char *audio_info(void)
{
    return g_info;
}
