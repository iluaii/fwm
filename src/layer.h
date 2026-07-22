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

/* Hand the keyboard to the topmost layer surface that wants it, or back to the
 * focused window when none does. */
void layer_update_keyboard_focus(struct FwmServer *server);

#endif /* FWM_LAYER_H */
