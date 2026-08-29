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

#include "urgent.h"
#include "server.h"
#include "ipc.h"
#include "defines.h"

/* The one place the flag changes, so the event and the repaint cannot be
 * forgotten by a caller. Returns true when it actually moved. */
static bool urgent_set(FwmServer *server, int d, bool on) {
    if (!server || d < 0 || d >= FWM_DESKTOPS) return false;
    if (server->desktop_urgent[d] == on) return false;

    server->desktop_urgent[d] = on;
    ipc_emit_urgent(server->ipc, d, on);
    server_request_tray_redraw(server);
    return true;
}

bool urgent_raise(FwmServer *server, int d) {
    if (!server || d < 0 || d >= FWM_DESKTOPS) return false;
    /* Already in front of someone: nothing to point at. Checked here rather
     * than left to the sweep, so a script gets the honest answer in the reply
     * instead of a flag that exists for one tick. */
    if (server_output_showing(server, d)) return false;
    urgent_set(server, d, true);
    return true;
}

void urgent_drop(FwmServer *server, int d) {
    urgent_set(server, d, false);
}

bool urgent_get(const FwmServer *server, int d) {
    if (!server || d < 0 || d >= FWM_DESKTOPS) return false;
    return server->desktop_urgent[d];
}

void urgent_sweep(FwmServer *server) {
    if (!server) return;
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        if (!server->desktop_urgent[d]) continue;
        if (server_output_showing(server, d)) urgent_set(server, d, false);
    }
}
