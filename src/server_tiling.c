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
#include <limits.h>
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
/* The room a window has on `desktop`: the space layer-shell clients left us,
 * minus our own status strip, minus the outer gap. In world coordinates, so the
 * desktop's own column offset is already in x.
 *
 * The tiling layout, the alignment pass and FAKE FULLSCREEN all come through
 * here. That last one is the point of the function being shared rather than
 * copied: "fake fullscreen" means "as large as a window is allowed to be", and
 * the moment that answer is written down twice the two drift — which is exactly
 * what had happened. The old copy in server_set_fullscreen reserved a hardcoded
 * band under the tray instead of the gap, and measured its height from the
 * screen rather than the work area, so a dock along the bottom was covered
 * over. */
void server_work_area(FwmServer *server, int desktop, int *x, int *y, int *w, int *h) {
    int gout = server->config.tiling.gaps_out;

    struct wlr_box screen = { 0, 0, server->screen_width, server->screen_height };
    struct wlr_box work = screen;

    /* The monitor showing this desktop is the one whose bars apply; with nobody
     * showing it, the desktop's own size is the answer. Per monitor, so a bar
     * on the second screen does not shrink the first. */
    FwmOutput *mon = server_output_showing(server, desktop);
    if (mon && mon->usable_area.width > 0 && mon->usable_area.height > 0) {
        /* usable_area is in layout coordinates; a desktop's frame starts at 0,0. */
        work = (struct wlr_box){
            mon->usable_area.x - mon->box.x, mon->usable_area.y - mon->box.y,
            mon->usable_area.width, mon->usable_area.height,
        };
        if (!wlr_box_intersection(&work, &work, &screen)) work = screen;
    }

    /* A hidden tray reserves nothing — that is what makes it *gone* rather than
     * merely invisible. Layer-shell exclusive zones still apply. A strip that
     * stood down for an external bar ([decor] tray_yield) is hidden in exactly
     * the same sense: the bar's own zone is already in `work`, and reserving on
     * top of that would leave a second empty band under it. */
    int tray_gone = server->tray_hidden
                  || (mon && mon->top_reserved && server->config.decor.tray_yield);
    if (!tray_gone && work.y < TRAY_BOTTOM) {
        work.height -= TRAY_BOTTOM - work.y;
        work.y = TRAY_BOTTOM;
    }

    *x = desktop * server->screen_width + work.x + gout;
    *y = work.y + gout;
    *w = work.width  - gout * 2;
    *h = work.height - gout * 2;

    /* Gaps wider than the screen they are cut from would ask for a window of
     * negative size, and every caller would hand that to a client. */
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static void tile_area(FwmServer *server, int desktop, int *x, int *y, int *w, int *h) {
    server_work_area(server, desktop, x, y, w, h);
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
        /* The size configure above went out with the OLD position — an X11
         * client that believed it would put its menus at the slot it just
         * left. Tell it where it actually is now. */
        view_sync_position(view);
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
/* What every window on this desktop is, and what it will not go below.
 *
 * Two sources for the floor, and the second is the one that needs care. A
 * client may declare a minimum (xdg-shell set_min_size, ICCCM WM_NORMAL_HINTS)
 * and then that is simply the answer — but plenty of the windows that HAVE a
 * minimum never declare it and just commit it instead, so the layout also
 * learns from what a window does when it is offered less.
 *
 * Being too big has to LAST to count. This runs on every layout pass — many
 * times a second while a divider is being dragged — and a client is always a
 * frame or two behind the size it has been asked for. Reading that lag as a
 * refusal makes the window's CURRENT size its floor, and the divider is pinned
 * where it stands: one small resize, then nothing moves. A client that has
 * simply not answered yet answers well inside TILE_FLOOR_SETTLE; one that is
 * never going to answer is still too big when the timer runs out. */
#define TILE_FLOOR_SETTLE 0.2   /* s an oversize must stand before it is a floor */

static double tile_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static int tile_actuals(FwmServer *server, BspNode **leaves, int count,
                        BspActual *actual, int *bar) {
    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        FwmView *view = server_find_view(server, n->id);
        int w = n->aw, h = n->ah;
        if (view) view_committed_size(view, &w, &h);
        /* A tab-stack's bar sits above the client but inside the slot, so it
         * counts toward the space this leaf occupies. */
        bar[i] = (view && view->group && n->ah > GROUP_TAB_H * 2) ? GROUP_TAB_H : 0;

        int min_w = 0, min_h = 0;
        if (view) {
            view_min_size(view, &min_w, &min_h);
            if (n->aw > 0 && n->ah > 0) {
                double now = tile_now();
                if (w > n->aw) {
                    if (view->tile_over_w != w) {
                        view->tile_over_w = w;
                        view->tile_over_tw = now;
                    } else if (now - view->tile_over_tw > TILE_FLOOR_SETTLE) {
                        view->tile_floor_w = w;
                    }
                } else {
                    view->tile_over_w = 0;
                    /* Fitting inside what it was offered proves the floor is no
                     * higher than that — which is how a window that has grown a
                     * minimum, or was measured wrong, loses it again. */
                    if (w < view->tile_floor_w) view->tile_floor_w = w;
                }
                if (h > n->ah) {
                    if (view->tile_over_h != h) {
                        view->tile_over_h = h;
                        view->tile_over_th = now;
                    } else if (now - view->tile_over_th > TILE_FLOOR_SETTLE) {
                        view->tile_floor_h = h;
                    }
                } else {
                    view->tile_over_h = 0;
                    if (h < view->tile_floor_h) view->tile_floor_h = h;
                }
            }
            if (view->tile_floor_w > min_w) min_w = view->tile_floor_w;
            if (view->tile_floor_h > min_h) min_h = view->tile_floor_h;
        }

        actual[i] = (BspActual){
            .id = n->id, .w = w, .h = h + bar[i],
            .min_w = min_w,
            .min_h = min_h ? min_h + bar[i] : 0,
        };
    }
    return count;
}

/* How far one split may be dragged before it starts squeezing a window under
 * its floor. The border drag asks, so that it stops where the layout stops
 * rather than running on into travel that changes nothing — and so that pulling
 * back moves the divider at once instead of first spending that dead travel. */
void server_tile_ratio_limits(FwmServer *server, int desktop, BspNode *node,
                              float *lo, float *hi) {
    *lo = 0.05f;
    *hi = 0.95f;
    if (desktop < 0 || desktop >= 10) return;
    BspNode *root = server->bsp_roots[desktop];
    if (!root || !node) return;

    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(root, leaves, &count, MAX_WINDOWS);
    if (count == 0) return;

    BspActual actual[MAX_WINDOWS];
    int bar[MAX_WINDOWS];
    tile_actuals(server, leaves, count, actual, bar);
    bsp_ratio_limits(node, server->config.tiling.gaps_in, actual, count, lo, hi);
}

void server_align_tiles(FwmServer *server, int desktop) {
    if (desktop < 0 || desktop >= 10) return;
    BspNode *root = server->bsp_roots[desktop];
    if (!root) return;

    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(root, leaves, &count, MAX_WINDOWS);
    if (count == 0) return;

    BspActual actual[MAX_WINDOWS];
    int bar[MAX_WINDOWS];
    tile_actuals(server, leaves, count, actual, bar);

    /* While a divider is being dragged, place from the SLOTS instead — each
     * window gets exactly the space the ratio gives it, whatever it has
     * committed so far.
     *
     * Laying out against committed sizes is what makes the interior gaps come
     * out exactly gaps_in at rest, but it also hands the client's own size
     * quantisation to its neighbour: a terminal rounds to whole character
     * cells, so the window beside it could only move when the terminal jumped a
     * cell, and the whole drag advanced in steps of 8-10px however smoothly the
     * hand moved. The floors stay in force — they are what a window will not go
     * below, not what it happens to be — so nothing may still be squeezed under
     * one. The exact gaps come back at the release, which lays the desktop out
     * again the ordinary way. */
    bool live = server->interactive.action == FWM_ACTION_BSP_RESIZE
             && server->interactive.bsp_desktop == desktop;
    if (live) {
        for (int i = 0; i < count; i++) {
            actual[i].w = INT_MAX;   /* "took all it was offered" — see bsp.h */
            actual[i].h = INT_MAX;
        }
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
