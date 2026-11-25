/* SPDX-License-Identifier: MIT
 *
 * Resolve ALSA card numbers from /proc/asound/cards by short id
 * (e.g. "I2S", "Surface"). Public pattern used by SampleBoot / community OSes.
 */
#define _POSIX_C_SOURCE 200809L
#include "epicycle.h"

#include <stdio.h>
#include <string.h>

int epicycle_find_alsa_card(const char *want_id)
{
    if (!want_id || !want_id[0])
        return -1;

    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f)
        return -1;

    char line[256];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        int n;
        char id[64];
        if (sscanf(line, " %d [%63[^]]", &n, id) != 2)
            continue;
        /* Trim trailing spaces inside the bracket id. */
        size_t len = strlen(id);
        while (len > 0 && id[len - 1] == ' ')
            id[--len] = '\0';
        if (strcmp(id, want_id) == 0) {
            found = n;
            break;
        }
        /* Surface also appears as "Control Surface" in the long name field. */
        if (strcmp(want_id, "Surface") == 0 && strstr(line, "Control Surface")) {
            found = n;
            break;
        }
    }
    fclose(f);
    return found;
}
