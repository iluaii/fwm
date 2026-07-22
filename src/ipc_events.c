/*
 * fwm — a Wayland compositor
 * Copyright (C) 2026 Ilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "ipc_events.h"

#include <stdio.h>
#include <string.h>

/* Indexed by bit position: names[i] is the name of bit (1u << i). */
static const char *const names[FWM_EV_COUNT] = {
    "window_open",
    "window_close",
    "window_focus",
    "window_title",
    "desktop",
    "mode",
    "gravity",
    "config_reload",
};

uint32_t fwm_ipc_event_bit(const char *name, size_t len) {
    if (!name || len == 0) return 0;
    for (int i = 0; i < FWM_EV_COUNT; i++) {
        if (strlen(names[i]) == len && strncmp(names[i], name, len) == 0)
            return 1u << i;
    }
    return 0;
}

const char *fwm_ipc_event_name(uint32_t bit) {
    for (int i = 0; i < FWM_EV_COUNT; i++)
        if (bit == (1u << i)) return names[i];
    return NULL;
}

void fwm_ipc_event_list(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < FWM_EV_COUNT; i++) {
        int n = snprintf(out + len, cap - len, "%s%s", i ? " " : "", names[i]);
        if (n < 0 || (size_t)n >= cap - len) return;  /* truncated, still valid */
        len += (size_t)n;
    }
}

static bool is_sep(char c) { return c == ',' || c == ' ' || c == '\t'; }

bool fwm_ipc_events_parse(const char *list, uint32_t *mask, char *err, size_t errcap) {
    /* Skip leading separators first, so "  " and ", ," read as "no argument"
     * rather than as a list of empty names. */
    while (list && *list && is_sep(*list)) list++;

    if (!list || !*list) { *mask = FWM_EV_ALL; return true; }

    if (strcmp(list, "all") == 0) { *mask = FWM_EV_ALL; return true; }

    uint32_t built = 0;
    for (const char *p = list; *p; ) {
        while (*p && is_sep(*p)) p++;
        if (!*p) break;

        const char *start = p;
        while (*p && !is_sep(*p)) p++;
        size_t len = (size_t)(p - start);

        uint32_t bit = fwm_ipc_event_bit(start, len);
        if (!bit) {
            /* "all" is accepted only on its own: mixing it into a list would
             * make the rest of the list a lie about what arrives. */
            char known[256];
            fwm_ipc_event_list(known, sizeof known);
            snprintf(err, errcap, "unknown event \"%.*s\" (known: %s)",
                     (int)len, start, known);
            return false;
        }
        built |= bit;
    }

    if (!built) { snprintf(err, errcap, "subscribe: no events named"); return false; }

    *mask = built;
    return true;
}
