/* SPDX-License-Identifier: MIT
 *
 * Loop-engine stubs for the EPICYCLE product spine:
 * per-pad start/end/loop brace, LFO targets, division vs freeform, crossfade.
 * No sample I/O yet — renders silence.
 */
#include "epicycle.h"

#include <string.h>

void engine_init(EpicycleEngine *eng)
{
    memset(eng, 0, sizeof *eng);
    eng->tempo_bpm = 120.0f;
    eng->running = 1;
    for (int i = 0; i < EPICYCLE_PAD_COUNT; i++) {
        eng->pads[i].start = 0.0f;
        eng->pads[i].end = 1.0f;
        eng->pads[i].loop_start = 0.0f;
        eng->pads[i].loop_end = 1.0f;
        eng->pads[i].freeform = 1;
        eng->pads[i].level = 0.0f;
    }
}

void engine_render(EpicycleEngine *eng, int32_t *out_l, int32_t *out_r, size_t frames)
{
    (void)eng;
    for (size_t i = 0; i < frames; i++) {
        out_l[i] = 0;
        out_r[i] = 0;
    }
}
