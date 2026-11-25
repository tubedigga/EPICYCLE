/* SPDX-License-Identifier: MIT
 *
 * Control-surface MIDI stub (UART3 raw MIDI / ALSA "Surface").
 * Published map facts (do not reverse stock for these):
 *   - pads: channel 10, nonsequential note map
 *   - knobs / buttons / wheel / fader: channel 1
 *   - wheel push ≈ note 111; wheel turn ≈ CC 100
 * Clear pad/button lights on start (SampleBoot / sample-base docs).
 */
#include "epicycle.h"

#include <stdio.h>

static int g_open;
static int g_quit_requested;

int surface_open(void)
{
    g_open = 1;
    g_quit_requested = 0;
    fprintf(stderr, "surface: stub open (no MIDI port bound in scaffold)\n");
    /* Host/CI: auto-request quit so the process exits cleanly. */
#ifndef EPICYCLE_ON_DEVICE
    g_quit_requested = 1;
#endif
    return 0;
}

void surface_close(void)
{
    g_open = 0;
    (void)g_open;
}

int surface_poll_quit(void)
{
    return g_quit_requested;
}
