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

/* Tiling geometry: where the BSP layout puts each window, and the glide that
 * takes it there. The layout itself is bsp.c; this is the part that has to
 * know about physics bodies and scene nodes. Split out of server.c; see
 * server_internal.h. */
#include "server.h"
#include "view.h"
#include "physics.h"
#include "group.h"   /* GROUP_TAB_H: a stacked window's tab bar eats into its slot */
#include "bsp.h"
#include <signal.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "ui/tray.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "server_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* The area tiles live in: the space layer-shell clients left us, minus our own
 * tray and the outer gap. Shared by the layout and the alignment pass so the
 * two cannot drift apart. */
static void tile_area(FwmServer *server, int desktop, int *x, int *y, int *w, int *h) {
    int gout = server->config.tiling.gaps_out;
    struct wlr_box work = server->usable_area;
    if (work.width <= 0 || work.height <= 0) {
        work = (struct wlr_box){ 0, 0, server->screen_width, server->screen_height };
    }
    /* A hidden tray reserves nothing — that is what makes it *gone* rather than
     * merely invisible. Layer-shell exclusive zones still apply. */
    if (!server->tray_hidden && work.y < TRAY_BOTTOM) {
        work.height -= TRAY_BOTTOM - work.y;
        work.y = TRAY_BOTTOM;
    }
    *x = desktop * server->screen_width + work.x + gout;
    *y = work.y + gout;
    *w = work.width  - gout * 2;
    *h = work.height - gout * 2;
}

/* Move one tile to where the alignment pass decided it goes. */
static void tile_move_to(FwmServer *server, FwmView *view, PhysicsBody *pb, int nx, int ny) {
    int animate = server->config.tiling.anim_speed > 0.0 &&
                  server->interactive.action != FWM_ACTION_BSP_RESIZE;
    if (!view) { pb->x = nx; pb->y = ny; return; }

    if (animate) {
        view->tile_anim = 1;
        view->tile_tx = nx;
        view->tile_ty = ny;
    } else {
        view->tile_anim = 0;
        pb->x = nx;
        pb->y = ny;
        view->x = pb->x;
        view->y = pb->y;
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node,
                                        (int)lround(view->x),
                                        (int)lround(view->y));
        }
    }
}

/* Re-run tile positioning against the sizes clients actually committed.
 *
 * A tile is asked to be its slot's size, but a client may commit something
 * smaller and terminals routinely do — they round to whole character cells.
 * Anchored at the slot's top-left, that leftover ends up BETWEEN windows, so a
 * gap meant to be gaps_in reads as gaps_in plus a stray 6-16px. It is why the
 * layout looked right with two windows and wrong from three: two side-by-side
 * tiles are both full height, so there is no interior gap to get wrong yet.
 *
 * bsp_place_actual() does the arithmetic; this feeds it the committed sizes
 * and moves the windows to what it decided. No size is touched, so this cannot
 * provoke the commits that would call it again.
 */
void server_align_tiles(FwmServer *server, int desktop) {
    if (desktop < 0 || desktop >= FWM_DESKTOPS) return;
    BspNode *root = server->bsp_roots[desktop];
    if (!root) return;

    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(root, leaves, &count, MAX_WINDOWS);
    if (count == 0) return;

    BspActual actual[MAX_WINDOWS];
    int bar[MAX_WINDOWS];
    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        FwmView *view = server_find_view(server, n->id);
        int w = n->aw, h = n->ah;
        if (view) view_committed_size(view, &w, &h);
        /* A tab-stack's bar sits above the client but inside the slot, so it
         * counts toward the space this leaf occupies. */
        bar[i] = (view && view->group && n->ah > GROUP_TAB_H * 2) ? GROUP_TAB_H : 0;
        actual[i] = (BspActual){ .id = n->id, .w = w, .h = h + bar[i] };
    }

    int x, y, w, h;
    tile_area(server, desktop, &x, &y, &w, &h);
    bsp_place_actual(root, x, y, w, h, server->config.tiling.gaps_in, actual, count);

    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        PhysicsBody *pb = physics_find_body(&server->physics, n->id);
        if (!pb) continue;
        pb->tiled = 1;
        pb->vx = 0;
        pb->vy = 0;
        pb->flying = 0;

        FwmView *view = server_find_view(server, n->id);
        pb->width  = n->aw;
        pb->height = n->ah - bar[i];
        if (view) {
            view->width  = pb->width;
            view->height = pb->height;
            view_set_size(view, view->width, view->height);
        }
        tile_move_to(server, view, pb, n->ax, n->ay + bar[i]);
    }
}

void server_apply_tiling(FwmServer *server, int desktop) {
    int x, y, usable_w, usable_h;
    tile_area(server, desktop, &x, &y, &usable_w, &usable_h);

    /* The slot grid. Sizes and positions no longer come from it —
     * server_align_tiles() derives those from what clients committed — but
     * bsp_find_border() hit-tests dragging against x/y/w/h, and the ratio each
     * split is recalculated from lives here. */
    bsp_recalc(server->bsp_roots[desktop], x, y, usable_w, usable_h,
               server->config.tiling.gaps_in);

    server_align_tiles(server, desktop);
}
