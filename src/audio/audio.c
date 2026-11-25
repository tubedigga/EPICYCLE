/* SPDX-License-Identifier: MIT
 *
 * ALSA I2S (AK4558) stub. Target: stereo S32, 128-frame callback, 44.1 kHz
 * (firmware 1.4 device-tree locks rate to 44.1). No click-free open sequence yet.
 */
#include "epicycle.h"

#include <stdio.h>

static int g_running;

int audio_open(void)
{
    /* Real implementation: open ALSA PCM on the I2S card, S32_LE stereo. */
    g_running = 0;
    fprintf(stderr, "audio: stub open (no ALSA device bound in scaffold)\n");
    return 0;
}

void audio_close(void)
{
    g_running = 0;
}

int audio_running(void)
{
    return g_running;
}
