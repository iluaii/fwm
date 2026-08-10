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

#ifndef FWM_SHORTCUTS_H
#define FWM_SHORTCUTS_H

#include <stdbool.h>

struct FwmServer;

/* hyprland-global-shortcuts-v1: lets an external shell own a keybind.
 *
 * A Wayland client cannot see a key it is not focused for, which is why an
 * outside launcher can never answer super+space by itself — the compositor has
 * to hand the press over. That is all this protocol is: the client registers a
 * named action and says nothing about keys; the compositor decides which keys
 * reach it. In Quickshell it is the GlobalShortcut type.
 *
 * fwm binds one the same way it binds anything else, by name:
 *
 *   [binds]
 *   "super+space" = "global:quickshell:launcher"
 *
 * and the built-in launcher is simply not on that key any more — which is what
 * makes this "replacing" fwm's launcher rather than sitting next to it.
 *
 * wlroots does not implement this one, so the interfaces are served here and
 * the XML is vendored in protocols/.
 *
 * The protocol is a privileged one: any client that binds the global can ask
 * for a key. fwm serves it to everything, exactly as it serves layer-shell —
 * a session where untrusted clients can connect at all has already lost this
 * argument (see the security_context note in the README). */
void shortcuts_init(struct FwmServer *server);

/* Fire the shortcut registered as `app_id:id`, the whole press and release.
 * Returns false when no client has registered it — the caller reports that as
 * an unknown action rather than swallowing the key silently. */
bool shortcuts_trigger(struct FwmServer *server, const char *app_id, const char *id);

/* The press and release halves, for a bind that is HELD: a client that dims the
 * screen while its key is down needs both edges, not one event. */
bool shortcuts_press(struct FwmServer *server, const char *app_id, const char *id);
bool shortcuts_release(struct FwmServer *server, const char *app_id, const char *id);

#endif /* FWM_SHORTCUTS_H */
