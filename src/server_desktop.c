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

/* Desktop modes: entering and leaving BSP tiling, floating, and moving a view
 * between the desktops of the world strip. Split out of server.c; see
 * server_internal.h. */
#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "theme.h"
#include "layer.h"
#include "lock.h"
#include "foreign.h"
#include "ipc.h"
#include "session.h"
#include <signal.h>
#include "ui/tray.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"
#include "expo.h"

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
#include "server_internal.h"

/* Switch one desktop between physics and tiling. Extracted so the per-desktop
 * bind and the all-desktops bind cannot drift apart — the restore path in
 * particular (saved geometry, cleared tile state, the outward shove) is easy to
 * get subtly wrong twice. */
static void desktop_enter_tiling(FwmServer *server, int d) {
    /* Remember where physics had put things, so leaving tiling can undo it. */
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        /* A fullscreen window keeps its own pre-fullscreen geometry in sav_*,
         * and is excluded from the tile tree below — don't hand it a tiling
         * restore point derived from its (temporary) fullscreen geometry. */
        if (b && b->desktop_id == d && !b->fullscreen && !b->tiling_saved) {
            b->tile_sav_x = b->x; b->tile_sav_y = b->y;
            b->tile_sav_w = b->width; b->tile_sav_h = b->height;
            b->tiling_saved = 1;
        }
    }

    bsp_free(server->bsp_roots[d]);
    server->bsp_roots[d] = NULL;

    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (b && !b->shaped && !b->fullscreen && b->desktop_id == d) {
            bsp_insert(&server->bsp_roots[d], 0, view->id);
        }
    }
    server_apply_tiling(server, d);
}

static void desktop_leave_tiling(FwmServer *server, int d) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (!b || b->desktop_id != d) continue;

        if (b->tiling_saved) {
            b->x = b->tile_sav_x; b->y = b->tile_sav_y;
            b->width = b->tile_sav_w; b->height = b->tile_sav_h;
            b->tiling_saved = 0;
        } else {
            b->width = server->screen_width / 2;
            b->height = server->screen_height / 2;
            b->x = d * server->screen_width + (server->screen_width - b->width) / 2;
            b->y = (server->screen_height - b->height) / 2;
        }
        b->vx = 0; b->vy = 0; b->flying = 0;
        b->tiled = 0;
        view->tile_anim = 0;

        view->x = b->x; view->y = b->y;
        view->width = b->width; view->height = b->height;
        view_set_size(view, view->width, view->height);
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node,
                                        (int)lround(view->x),
                                        (int)lround(view->y));
        }
    }

    bsp_free(server->bsp_roots[d]);
    server->bsp_roots[d] = NULL;
}

/* Floating is the "normal desktop environment" mode: windows stay exactly
 * where you drop them and overlap freely. Both halves of that are already
 * expressible per window (pinned = immovable anchor, no_collide = passes
 * through other windows), so the desktop mode just raises one flag and lets
 * physics.c apply the same two rules it already knows. */
static void desktop_set_floating(FwmServer *server, int d, int on) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (!b || b->desktop_id != d) continue;
        b->floating = on;
        if (on) { b->vx = 0; b->vy = 0; b->flying = 0; }
    }
}

/* Scatter everything on the desktop, so switching back to physics reads as the
 * world waking up rather than as nothing having happened. */
static void desktop_shove(FwmServer *server, int d) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (!b || b->desktop_id != d) continue;
        double angle = ((double)(view->id % 628)) / 100.0;
        b->vx = cos(angle) * 200.0;
        b->vy = sin(angle) * 200.0;
        b->flying = 1;
    }
}

/* Move one desktop between physics, tiling and floating.
 *
 * Written as leave-old then enter-new rather than as a switch over pairs: with
 * three modes there are six transitions, and the pairwise form is where the
 * restore path (saved geometry, cleared tile state, the outward shove) starts
 * getting subtly wrong in some of them. */
void server_set_desktop_mode(FwmServer *server, int d, int mode) {
    if (d < 0 || d >= 10) return;
    int old = server->desktop_mode[d];
    if (old == mode) return;

    if (old == DESKTOP_MODE_TILING)        desktop_leave_tiling(server, d);
    else if (old == DESKTOP_MODE_FLOATING) desktop_set_floating(server, d, 0);

    server->desktop_mode[d] = mode;

    if (mode == DESKTOP_MODE_TILING)        desktop_enter_tiling(server, d);
    else if (mode == DESKTOP_MODE_FLOATING) desktop_set_floating(server, d, 1);
    else                                    desktop_shove(server, d);

    server_request_tray_redraw(server);
    ipc_emit_mode(server->ipc, d, mode);
}

/* Kept as the entry point for the toggle_tiling bind: physics <-> tiling, with
 * floating collapsing to tiling so the key never becomes a no-op. */
void server_toggle_desktop_tiling(FwmServer *server, int d) {
    server_set_desktop_mode(server, d,
        server->desktop_mode[d] == DESKTOP_MODE_TILING
            ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_TILING);
}

/* Send a window to another desktop.
 *
 * In physics and floating modes you can already drag a window across the
 * desktop boundary, but a tiled window is a static body owned by the layout —
 * dragging cannot take it out, which left tiling with no way to move a window
 * off its desktop at all. This works the same in all three modes so the bind
 * does not silently mean different things depending on where you are. */
void server_move_view_to_desktop(FwmServer *server, FwmView *view, int target,
                                        int follow) {
    if (!view || target < 0 || target >= 10 || server->screen_width <= 0) return;

    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    int src = b ? b->desktop_id : view->x / server->screen_width;
    if (src == target) return;

    /* Between the two ends of a closed ring this is one step, not nine: the
     * camera jumps the join rather than sliding back across every desktop in
     * between, and a window crossing it goes the same way — there is no
     * picture of the join to fly through, so there is nothing to fly through
     * it. */
    int seam = server->config.camera.wrap
            && ((src == FWM_DESKTOPS - 1 && target == 0)
             || (src == 0 && target == FWM_DESKTOPS - 1));

    /* A tab-stack is a single stack on a single desktop; pulling a member out
     * of it is the only sensible reading of "move this window away". */
    if (view->group) {
        group_remove(server, view);
        /* Popping the front window out of a stack hands the stack's physics
         * body to whichever tab came up behind it (group.c), so the body this
         * window had a line ago is not its own any more — and it may have none
         * at all until the next commit builds one. */
        b = physics_find_body(&server->physics, view->id);
    }

    /* A send moves a window a whole screen at least, and moving it there
     * outright is a teleport: the window is simply gone, and on the desktop it
     * landed on it was simply always there. So the bookkeeping below happens
     * at once — the layouts, the focus, the body's desktop — and the flight
     * armed at the end walks only the PICTURE back to where the window stood
     * and eases it across the strip.
     *
     * Never the window in your hand: a drag owns its position frame by frame
     * and would spend the whole flight fighting it for the same field. That
     * one still teleports, and still carries its wobble with it. Nor under
     * expo, which has every desktop on screen at once and moves a window by
     * carrying its card there — a flight would be a second, contradictory
     * picture of the same move, drawn from the position the card is read
     * back from. */
    int animate = b && !seam && server->config.camera.anim_ms > 0.0
               && server->interactive.view != view && !expo_active(server);
    double from_x = b ? b->x : (double)view->x;
    double from_y = b ? b->y : (double)view->y;

    /* Leave the old layout before the coordinates move, or the re-tile would
     * lay out the window that is on its way out. */
    if (server->desktop_mode[src] == DESKTOP_MODE_TILING) {
        bsp_remove(&server->bsp_roots[src], view->id);
        server_apply_tiling(server, src);
    }

    if (b) {
        /* Keep the offset within the desktop; desktops are one screen apart. */
        b->x += (double)(target - src) * server->screen_width;
        b->desktop_id = target;
        b->vx = 0; b->vy = 0; b->flying = 0;
        b->tiled = 0;
        b->tiling_saved = 0;
        b->floating = (server->desktop_mode[target] == DESKTOP_MODE_FLOATING);
        view->x = (int)lround(b->x);
        view->y = (int)lround(b->y);
    } else {
        view->x += (target - src) * server->screen_width;
    }
    view->tile_anim = TILE_ANIM_NONE;
    /* Sent a screen sideways without travelling: a window still wobbling from
     * the drag that sent it must not be shoved by the coordinate change. A
     * flight is not a coordinate change — it travels, and the wobble is
     * entitled to feel it. */
    if (!animate)
        view_jelly_carry(view, (double)(target - src) * server->screen_width, 0.0);
    view_sync_position(view);

    if (server->desktop_mode[target] == DESKTOP_MODE_TILING) {
        bsp_insert(&server->bsp_roots[target], 0, view->id);
        server_apply_tiling(server, target);
    }

    if (follow) {
        /* Relocating a window in order to keep working in it: travel with it
         * and keep it focused. Claiming focus_desktop here stops the camera's
         * arrival handler from handing the keyboard to whatever else lives on
         * the destination. */
        server->focus_desktop = target;
        /* At the ring's join that is one step, not nine — jump it and slide
         * across the jump, the same as any other step (see `seam` above). */
        server_goto_desktop(server, target, seam);
        server_focus_view(server, view);
    } else if (server->focused_view == view) {
        /* The camera stays put (i3/sway convention: moving is not following),
         * so the keyboard must not follow the window off-screen either. */
        server_refocus(server, src, view);
    }

    /* Last, so the window flies to whatever everything above settled on: a tile
     * slot on the destination, the nudge a monitor change applied to it, or
     * just the same spot one desktop over. */
    if (animate) {
        view->tile_tx = view->tile_anim ? view->tile_tx : b->x;
        view->tile_ty = view->tile_anim ? view->tile_ty : b->y;
        view->tile_fx = from_x;
        view->tile_fy = from_y;
        view->tile_t = 0.0;
        view->tile_anim = TILE_ANIM_FLIGHT;
        /* Back where it stood — the picture, and only the picture. The body
         * goes on saying `target` for the whole crossing: a physics step
         * derives a desktop from a position and would hand back whichever
         * column the window is passing over, which is the right answer for a
         * window dragged across the boundary by hand and the wrong one for a
         * window that has already been sent (flight_desktop, server_tick.c). */
        b->x = from_x;
        b->y = from_y;
        view->x = (int)lround(from_x);
        view->y = (int)lround(from_y);
        /* A key press has already left the idle heartbeat behind; a send that
         * came down the control socket has not, and would spend its first
         * fifth of a second standing still. */
        server_tick_wake(server);
    }

    server_request_tray_redraw(server);
}

void server_toggle_desktop_floating(FwmServer *server, int d) {
    server_set_desktop_mode(server, d,
        server->desktop_mode[d] == DESKTOP_MODE_FLOATING
            ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_FLOATING);
}
