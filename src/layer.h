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

#ifndef FWM_LAYER_H
#define FWM_LAYER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>

struct FwmServer;

/* wlr-layer-shell-unstable-v1: panels, bars, notification daemons and menus
 * that sit outside the normal window stack (waybar, mako, rofi, swaybg).
 *
 * Placement, anchors, margins and exclusive zones are all handled by wlroots'
 * scene helper (wlr_scene_layer_surface_v1_configure); what lives here is the
 * bookkeeping around it: which scene tree each layer maps to, recomputing the
 * usable area when surfaces come and go, and keyboard focus for surfaces that
 * ask for it. */
typedef struct FwmLayerSurface {
    struct wl_list link;                       /* FwmServer.layer_surfaces */
    struct FwmServer *server;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene;
    struct wlr_scene_tree *popups;             /* parent tree for this surface's popups */

    struct wl_listener map, unmap, commit, destroy, new_popup;
} FwmLayerSurface;

void layer_shell_init(struct FwmServer *server);

/* Re-run placement for every mapped layer surface and recompute
 * server->usable_area (the screen area left after exclusive zones). Call after
 * anything that changes the set of surfaces or the output size. */
void layer_arrange(struct FwmServer *server);

/* Close every layer surface that belongs to an output that is going away. A
 * bar keeps a pointer to the monitor it asked for, so leaving it alive would
 * leave that pointer dangling. Call before the output is freed. */
void layer_output_gone(struct FwmServer *server, struct wlr_output *output);

/* Hand the keyboard to the topmost layer surface DEMANDING it (exclusive
 * keyboard-interactivity), or back to whoever should have it when none does.
 * An on-demand surface is not taken into account here — see
 * layer_keyboard_click. */
void layer_update_keyboard_focus(struct FwmServer *server);

/* The surface of a layer surface holding the keyboard against everything else,
 * i.e. one with exclusive keyboard-interactivity; NULL when none is.
 *
 * This is what makes such a surface usable at all under focus-follows-pointer:
 * without it the first twitch of the mouse over a window took the keyboard
 * away from a menu or a session dialog, and — since focused_layer still named
 * it — nothing ever gave the keys back. server_keyboard_enter asks this before
 * handing the keyboard anywhere. */
struct wlr_surface *layer_keyboard_exclusive(struct FwmServer *server);

/* An on-demand layer surface lets go of the keyboard because the user is
 * working somewhere else. Returns true if one was holding it and has now let
 * go, i.e. if the caller must go on and take the keys. An exclusive surface
 * keeps them and this returns false. */
bool layer_keyboard_release(struct FwmServer *server);

/* A press at this point: an on-demand layer surface under it takes the
 * keyboard, which is the interaction "on demand" means. Returns true if one
 * did. */
bool layer_keyboard_click(struct FwmServer *server, double lx, double ly);

#endif /* FWM_LAYER_H */
