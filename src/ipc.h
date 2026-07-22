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

#ifndef FWM_IPC_H
#define FWM_IPC_H

#include <stdint.h>

#include "ipc_events.h"

struct FwmServer;
struct FwmView;
typedef struct FwmIpc FwmIpc;

/* Control socket: lets anything outside the compositor read its state and run
 * the same actions the keybinds run, so customisation no longer requires
 * editing C and rebuilding. Path is $XDG_RUNTIME_DIR/fwm-<wl_socket>.sock and
 * is exported as FWM_SOCKET for children (fwmctl reads either).
 *
 * Returns NULL if the socket could not be created; that is NOT fatal, the
 * compositor runs fine without it. */
FwmIpc *ipc_create(struct FwmServer *server, const char *wl_socket);
void ipc_destroy(FwmIpc *ipc);

/* ── event stream ──────────────────────────────────────────────────────
 * `subscribe [events]` turns a connection into a one-way feed of
 * newline-delimited JSON instead of closing it after one reply. That is the
 * whole extension point: a "plugin" is any process that subscribes at one
 * end and calls `dispatch` at the other, written in any language, whose own
 * crash cannot take the session down with it.
 *
 * Every emitter below is a no-op when `ipc` is NULL (the socket is optional
 * and the compositor runs fine without it) or when nobody subscribed to that
 * event, so call sites need no guard of their own. */
void ipc_emit_window(FwmIpc *ipc, uint32_t event, struct FwmView *view);
void ipc_emit_desktop(FwmIpc *ipc, int desktop);
void ipc_emit_mode(FwmIpc *ipc, int desktop, int mode);
void ipc_emit_gravity(FwmIpc *ipc, double gravity_scale);
void ipc_emit_config_reload(FwmIpc *ipc);

#endif /* FWM_IPC_H */
