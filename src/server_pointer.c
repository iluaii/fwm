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

/* Pointer and seat: cursor motion and buttons, interactive move/resize entry,
 * selections, drag-and-drop, and pointer constraints. Split out of server.c;
 * see server_internal.h. */
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
#include "ui/modes.h"
#include "ui/stats_menu.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "screenshot.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"
#include "expo.h"
#include <linux/input-event-codes.h>

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
#include "server_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct FwmView *view_at(FwmServer *server, double lx, double ly,
                               struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    /* A buffer that is NOT a client surface is one of ours — during an impact
     * squash the live surface is hidden and our snapshot is what the cursor
     * lands on. Returning NULL here made the window inert for the ~250ms the
     * effect ran: no focus, no super+drag, no resize. Fall through with a NULL
     * surface instead, so the window is still grabbable; every caller already
     * distinguishes "no view" from "view with nothing to send events to". */
    *surface = scene_surface ? scene_surface->surface : NULL;

    // Walk up to find the tree node holding the FwmView (set in view_map)
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    if (tree == NULL) {
        return NULL;
    }

    /* A tag in node.data is NOT proof of a view. A layer-shell surface stows
     * its own FwmLayerSurface in the same field (layer.c), and a bar or a dock
     * is exactly what the cursor crosses on its way to a window — so this
     * handed back a pointer into an unrelated struct, which server_focus_view
     * then walked. Hovering a dock killed the compositor outright.
     *
     * Confirmed against the list rather than trusted: the field cannot say what
     * type it holds, and the answer has to stay right for whatever puts
     * something there next. The walk is over at most MAX_WINDOWS entries, once
     * per pointer motion.
     *
     * Returning NULL here does not silence the bar: *surface is already set
     * from the scene, and the callers send pointer events to a surface with no
     * view behind it — the path unmanaged X11 menus have always taken. */
    FwmView *found = tree->node.data;
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v == found) return found;
    }
    return NULL;
}

/* The monitor whose status strip is under the pointer, and where in that strip
 * the cursor is. NULL when the pointer is not over a monitor that has one.
 *
 * Every tray hit test below goes through this and then asks THAT monitor's
 * TrayStrip: the islands sit at different x on strips of different widths, so
 * the geometry and the coordinates have to come from the same screen. */
static FwmOutput *tray_under_pointer(FwmServer *server,
                                     double *tx, double *ty) {
    FwmOutput *o = server_output_at(server, server->cursor->x, server->cursor->y);
    if (!o || !o->tray_buffer) return NULL;
    /* A strip that is not on screen is not under the pointer either. It still
     * EXISTS while hidden — the buffer and its islands are kept, only the node
     * is disabled — so without this every pill, desktop marker and island went
     * on answering clicks from behind a fullscreen game: an invisible bar along
     * the bottom of the screen that swallowed shots and switched desktops.
     *
     * Asked of the scene rather than a flag, which covers both ways the strip
     * goes away (the user hiding it, a fullscreen window covering it) and, by
     * returning layout coordinates, is also the right origin to measure the
     * cursor against — node.x is relative to the node's parent. */
    int lx, ly;
    if (!wlr_scene_node_coords(&o->tray_buffer->node, &lx, &ly)) return NULL;
    if (tx) *tx = server->cursor->x - lx;
    if (ty) *ty = server->cursor->y - ly;
    return o;
}

/* Remember where the surface under the pointer sits, so that a button pressed
 * on it can go on being steered after the cursor has left it (see
 * pointer_grab_deliver). Called for every motion that is not already inside a
 * grab, which is exactly the state a press starts from. */
static void pointer_note_surface(FwmServer *server, struct wlr_surface *surface,
                                 FwmView *view, double lx, double ly,
                                 double sx, double sy) {
    server->ptr_surface = surface;
    server->ptr_view = surface ? view : NULL;
    server->ptr_ox = lx - sx;
    server->ptr_oy = ly - sy;
    server->ptr_node_have = 0;
    if (server->ptr_view && server->ptr_view->scene_tree) {
        int nx, ny;
        if (wlr_scene_node_coords(&server->ptr_view->scene_tree->node, &nx, &ny)) {
            server->ptr_node_x = nx;
            server->ptr_node_y = ny;
            server->ptr_node_have = 1;
        }
    }
}

/* A button is down: the surface it was pressed on owns the pointer until it
 * comes back up, and it owns it OUTSIDE its own edges too.
 *
 * This is the implicit grab the pointer protocol is built on, and without it
 * every drag a client does for itself comes apart the moment the cursor
 * strays: the compositor hands the pointer to whatever is now underneath, the
 * dragging surface is sent a leave, and it stops the drag it was doing.
 * Dragging a splitter or a panel edge is exactly that kind of drag — the hand
 * runs past the divider it is pushing, and by then the pointer is over a
 * different subsurface, a neighbouring window, or nothing — so panels could
 * not be resized in any application at all.
 *
 * The coordinates handed over stay measured from the grabbed surface even when
 * they fall outside it (negative, or past its width): that is what tells the
 * client how far the hand has pushed, and clients clamp it themselves.
 *
 * Returns false when there is nothing being grabbed, and the caller should
 * carry on with the ordinary "what is under the cursor" path. */
static bool pointer_grab_deliver(FwmServer *server, double lx, double ly,
                                 uint32_t time_msec) {
    if (server->seat->pointer_state.button_count == 0) return false;
    if (!server->ptr_surface ||
        server->ptr_surface != server->seat->pointer_state.focused_surface) {
        return false;
    }

    double ox = server->ptr_ox, oy = server->ptr_oy;
    /* Follow the window if it has moved since the press — the camera sliding
     * under the hand moves it just as a physics nudge does, and either way the
     * surface-local origin has to move with it or the drag drifts. */
    if (server->ptr_node_have && server->ptr_view && server->ptr_view->scene_tree) {
        int nx, ny;
        if (wlr_scene_node_coords(&server->ptr_view->scene_tree->node, &nx, &ny)) {
            ox += nx - server->ptr_node_x;
            oy += ny - server->ptr_node_y;
        }
    }

    wlr_seat_pointer_notify_motion(server->seat, time_msec, lx - ox, ly - oy);
    return true;
}

/* What the cursor is over, told to the client that owns it. Shared by motion
 * and by the release that ends a grab: letting go over a different window has
 * to put the pointer where it actually is, and there is no motion event to do
 * it with. */
static void pointer_update_focus(FwmServer *server, double lx, double ly,
                                 uint32_t time_msec) {
    struct wlr_surface *surface = NULL;
    double sx = 0, sy = 0;
    FwmView *view = view_at(server, lx, ly, &surface, &sx, &sy);
    pointer_note_surface(server, surface, view, lx, ly, sx, sy);
    if (surface) {
        // view == NULL happens over unmanaged X11 surfaces (menus,
        // tooltips): they still get pointer events, just no focus change.
        if (view) server_focus_view(server, view);
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
        constraints_follow_focus(server, surface);
    } else {
        // Over the empty background: no client owns the cursor, so restore
        // our default image (otherwise it keeps the last client's cursor or
        // none at all).
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
        constraints_follow_focus(server, NULL);
    }
}

/* Where a surface's top-left corner sits in layout coordinates.
 *
 * The pointer bookkeeping already holds it exactly, measured from the scene
 * itself, for the surface the pointer is over — and a constraint only ever
 * holds the pointer over the surface it belongs to. The fallback is the
 * window's own position, which is a world coordinate and needs mapping to the
 * screen; it is wrong by the xdg geometry offset, but it is only reached when
 * the pointer is not on the surface at all. */
static bool pointer_surface_origin(FwmServer *server, struct wlr_surface *surface,
                                   double *ox, double *oy) {
    if (surface && surface == server->ptr_surface) {
        *ox = server->ptr_ox;
        *oy = server->ptr_oy;
        return true;
    }
    FwmView *view = view_from_surface(server, surface);
    if (!view) return false;
    return server_world_to_screen(server, view->x, view->y, ox, oy);
}

/* A locked pointer may say, before it lets go, where the cursor should be
 * found afterwards — zwp_locked_pointer_v1.set_cursor_position_hint.
 *
 * This is not a nicety. It is the ONLY way a client can move the cursor on
 * Wayland, and it is what both SDL's warp and Xwayland's XWarpPointer are
 * built on: they take a momentary lock, name the place they want the pointer,
 * and drop it again. Ignoring the hint leaves our cursor wherever the hand
 * happened to leave it while the game steered by deltas, so the moment the
 * game hands the pointer back — a menu, a cutscene, alt-tab — the cursor
 * appears somewhere unrelated to where the game had been drawing its own.
 * That is the "the cursor jumps somewhere random" bug, and it is why games
 * behaved on compositors that honour the hint and not here. */
void pointer_apply_constraint_hint(FwmServer *server,
                                   struct wlr_pointer_constraint_v1 *constraint) {
    if (!constraint || constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED) return;
    if (!(constraint->current.committed & WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT))
        return;

    double ox, oy;
    if (!pointer_surface_origin(server, constraint->surface, &ox, &oy)) return;

    double sx = constraint->current.cursor_hint.x;
    double sy = constraint->current.cursor_hint.y;
    wlr_cursor_warp(server->cursor, NULL, ox + sx, oy + sy);

    /* Say it to the client too, if it is still the one holding the pointer:
     * the cursor moved without any input device moving, so there is no motion
     * event coming behind this to carry the new position. Only a motion, never
     * a re-enter — pointer_update_focus would run the whole focus path,
     * constraints included, and we are inside a constraint changing state. */
    if (server->seat->pointer_state.focused_surface == constraint->surface) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t msec = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
        wlr_seat_pointer_notify_motion(server->seat, msec, sx, sy);
    }
}

/* True when a confining constraint would let the cursor stand at (nx, ny). */
static bool constraint_allows_at(FwmServer *server, double nx, double ny) {
    struct wlr_pointer_constraint_v1 *c = server->active_constraint;
    if (!c || c->type != WLR_POINTER_CONSTRAINT_V1_CONFINED) return true;

    double ox, oy;
    /* Only testable while the surface has a place on a screen. With its desktop
     * off every monitor there is nothing to measure the region against, and
     * confining the pointer to a rectangle read out of nowhere would strand the
     * cursor; let the move through instead. */
    if (!pointer_surface_origin(server, c->surface, &ox, &oy)) return true;
    return pixman_region32_contains_point(&c->region, (int)(nx - ox), (int)(ny - oy),
                                          NULL);
}

static void process_cursor_motion(FwmServer *server, uint32_t time_msec) {
    double lx = server->cursor->x;
    double ly = server->cursor->y;

    server_notify_activity(server);
    if (lock_is_active(server)) return; /* nothing under the lock may be reached */
    drag_icon_update_position(server);

    /* The region selector is aiming, not pointing: it owns every motion and
     * no client hears about any of them. First, because it sits over
     * everything else that could claim one. */
    if (screenshot_handle_motion(server, lx, ly)) {
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    /* Same rule as the launcher below, for the same reason: while the desktop
     * strip is up the pointer is aiming at snapshots, not at the windows they
     * are pictures of, and no client may be told the cursor is over it. */
    if (expo_handle_motion(server, lx, ly)) {
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    // While the launcher is open the pointer belongs to it: hover moves the
    // selection, clients get no motion (and no pointer focus).
    if (launcher_is_open(server->launcher)) {
        launcher_handle_motion(server->launcher, lx, ly);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    /* A gesture in progress owns the motion; only otherwise does the cursor
     * mean "what is under me". */
    if (server_drag_motion(server, lx, ly, &now)) return;

    /* A drag the CLIENT is doing: the pointer is spoken for, and neither the
     * focus nor the surface under the cursor may change until the button is
     * let go. */
    if (pointer_grab_deliver(server, lx, ly, time_msec)) return;

    // Focus follows pointer
    pointer_update_focus(server, lx, ly, time_msec);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    /* Relative motion goes out regardless of whether the cursor itself is
     * allowed to move: a locked pointer is exactly the case where the client
     * steers from deltas and the cursor must stay put. */
    if (server->relative_pointer) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer, server->seat,
            (uint64_t)event->time_msec * 1000, event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
    }

    if (server->active_constraint && !lock_is_active(server)) {
        if (server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            /* Mouse-look: the cursor does not move at all. */
            server_notify_activity(server);
            return;
        }
        /* Confined: allow the move only while it stays inside the region. */
        if (!constraint_allows_at(server, server->cursor->x + event->delta_x,
                                  server->cursor->y + event->delta_y)) {
            server_notify_activity(server);
            return;
        }
    }

    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    /* A device that reports where it IS rather than how far it moved — a
     * tablet, a touchscreen, a virtual machine's pointer. The constraint has
     * to be honoured here too, or a game holding the pointer for mouse-look
     * sees the cursor jump straight to the far corner the moment one of these
     * speaks: an absolute event carries a position, and obeying it is a
     * teleport. Turn it into the delta the client expects instead. */
    double nx, ny;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base,
                                         event->x, event->y, &nx, &ny);
    double dx = nx - server->cursor->x, dy = ny - server->cursor->y;

    if (server->relative_pointer) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer, server->seat,
            (uint64_t)event->time_msec * 1000, dx, dy, dx, dy);
    }
    if (server->active_constraint && !lock_is_active(server)) {
        if (server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            /* Mouse-look: the cursor does not move at all. */
            server_notify_activity(server);
            return;
        }
        /* Confined: allow the move only while it stays inside the region. */
        if (!constraint_allows_at(server, nx, ny)) {
            server_notify_activity(server);
            return;
        }
    }

    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

/* Kernel button code -> the enum [mouse] binds are written in. Anything else
 * (a mouse with ten side buttons) is unbindable rather than mismapped. */
int button_to_fwm(uint32_t button) {
    switch (button) {
    case BTN_LEFT:   return FWM_BTN_LEFT;
    case BTN_RIGHT:  return FWM_BTN_RIGHT;
    case BTN_MIDDLE: return FWM_BTN_MIDDLE;
    case BTN_SIDE:   return FWM_BTN_SIDE;
    case BTN_EXTRA:  return FWM_BTN_EXTRA;
    default:         return -1;
    }
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    server_notify_activity(server);
    if (lock_is_active(server)) return; /* no clicks reach anything under the lock */

    /* The selector's own drag. Ahead of everything: the press that starts it
     * and the release that takes the picture must reach nothing else. */
    if (screenshot_handle_button(server, event->state == WL_POINTER_BUTTON_STATE_PRESSED))
        return;

    bool l_was_open = launcher_is_open(server->launcher);
    if (launcher_handle_button(server->launcher, server->cursor->x, server->cursor->y,
                               event->state == WL_POINTER_BUTTON_STATE_PRESSED)) {
        launcher_grab_sync(server, l_was_open); /* click may have closed it */
        return; /* launcher consumed the click; nothing reaches clients */
    }

    // Config-error pill in the tray: toggles the detail panel. Handled before
    // anything else so the click never reaches a window underneath.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE &&
        server->config.error_count > 0) {
        double tx, ty;
        FwmOutput *to = tray_under_pointer(server, &tx, &ty);
        if (to && tray_error_pill_hit(&to->tray_strip, tx, ty)) {
            server_dispatch_action(server, "show_errors");
            server->group_click = 1; /* swallow the matching release */
            return;
        }
    }

    // Modes menu, while it is open: a click inside works a control, a click
    // anywhere else dismisses it. Handled before the pill below, so the click
    // that closes the menu is not also the click that reopens it.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE && server->modes_buffer) {
        double mx = server->cursor->x - server->modes_buffer->node.x;
        double my = server->cursor->y - server->modes_buffer->node.y;
        int mw, mh;
        modes_menu_size(&mw, &mh);
        if (mx >= 0 && mx < mw && my >= 0 && my < mh) {
            int seg = -1;
            int row = modes_menu_hit(mx, my, &seg);
            if (row != MODES_ROW_NONE) server_modes_menu_click(server, row, seg);
            server->group_click = 1; /* swallow the matching release */
            return;
        }
        /* Outside: dismiss, unless the click is on the pill itself — that is a
         * toggle and the pill branch below owns it. The tray-buffer test comes
         * FIRST because the coordinates are derived from it. */
        int on_pill = 0;
        {
            double tx, ty;
            FwmOutput *to = tray_under_pointer(server, &tx, &ty);
            if (to) on_pill = tray_modes_pill_hit(&to->tray_strip, tx, ty);
        }
        if (!on_pill) {
            server_close_modes_menu(server);
            server_request_tray_redraw(server);
            /* Deliberately NOT swallowed: dismissing a menu should not also
             * eat the click that was aimed at whatever is underneath. */
        }
    }

    // Modes pill in the tray: opens and closes the menu.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE) {
        double tx, ty;
        FwmOutput *to = tray_under_pointer(server, &tx, &ty);
        if (to && tray_modes_pill_hit(&to->tray_strip, tx, ty)) {
            server_toggle_modes_menu(server);
            server->group_click = 1;
            return;
        }
    }

    /* Stats menu, while it is open: the same three cases as the modes menu
     * above — a click on a row works its switch, a click on the pill is the
     * toggle that closes it, and a click anywhere else dismisses it without
     * being eaten. */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE && server->stats_buffer) {
        double mx = server->cursor->x - server->stats_buffer->node.x;
        double my = server->cursor->y - server->stats_buffer->node.y;
        int mw, mh;
        stats_menu_size(server->stats, &mw, &mh);
        if (mx >= 0 && mx < mw && my >= 0 && my < mh) {
            int row = stats_menu_hit(server->stats, mx, my);
            if (row >= 0) server_stats_menu_click(server, row);
            server->group_click = 1; /* swallow the matching release */
            return;
        }
        int on_pill = 0;
        {
            double tx, ty;
            FwmOutput *to = tray_under_pointer(server, &tx, &ty);
            if (to) on_pill = tray_stats_pill_hit(&to->tray_strip, tx, ty);
        }
        if (!on_pill) {
            server_close_stats_menu(server);
            server_request_tray_redraw(server);
        }
    }

    /* Stats pill: opens and closes its menu, on the same button as every other
     * island in the strip. A readout is not obviously a control, so the one
     * thing it must not do is need a different click from its neighbour to
     * open — a menu nobody thinks to try the left button on is a menu nobody
     * opens. */
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE) {
        double tx, ty;
        FwmOutput *to = tray_under_pointer(server, &tx, &ty);
        if (to && tray_stats_pill_hit(&to->tray_strip, tx, ty)) {
            server_toggle_stats_menu(server);
            server->group_click = 1;
            return;
        }
    }

    // Desktop indicators: a left click jumps to that desktop.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE) {
        double tx, ty;
        FwmOutput *to = tray_under_pointer(server, &tx, &ty);
        int d = to ? tray_desktop_hit(&to->tray_strip, tx, ty) : -1;
        if (d >= 0) {
            server_goto_desktop(server, d, 0);
            server->group_click = 1; /* swallow the matching release */
            return;
        }
    }

    /* Everything above this line is the tray, and the tray goes on working while
     * the desktop strip is up: the marker follows the strip, clicking a desktop
     * indicator sends the strip there (it rides camera_x like everything else),
     * and the pills still open what they open. Only below here does a click
     * mean "a window", and those windows are the strip's cards, not the live
     * ones underneath. */
    if (expo_handle_button(server, event->button,
                           event->state == WL_POINTER_BUTTON_STATE_PRESSED,
                           server->cursor->x, server->cursor->y)) return;

    // Tab-stack bars: a left click on a tab switches the stack's window and
    // stays in the compositor (its release is swallowed too).
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE) {
        int tab;
        FwmGroup *bg = group_bar_at(server, server->cursor->x, server->cursor->y, &tab);
        if (bg) {
            group_set_active(server, bg, tab);
            server->group_click = 1;
            return;
        }
    }
    if (server->group_click && event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->group_click = 0;
        return;
    }

    // Clicks that belong to a compositor gesture stay in the compositor: any
    // button event while a drag/resize/swap is running, and any press with
    // the drag modifier (Super) held. Forwarding them made clients count
    // phantom clicks during window drags.
    uint32_t fwd_mods = get_active_modifiers(server);
    /* A button [mouse] has claimed is ours too, even without Super — otherwise
     * a bind on a side button would fire AND reach the client. Super stays a
     * blanket claim: the modifier that starts every window gesture must not
     * leak a stray click into the window being grabbed, bound chord or not. */
    bool claimed = config_match_mouse(&server->config, button_to_fwm(event->button),
                                      fwd_mods) != NULL;
    if (server->interactive.action == FWM_ACTION_NONE && !(fwd_mods & FWM_MOD_LOGO) && !claimed) {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    }
    
    double lx = server->cursor->x;
    double ly = server->cursor->y;
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (server_drag_press(server, event->button, lx, ly, &now)) return;
    } else {
        server_drag_release(server, lx, ly);
        /* The grab is over. Whatever is under the cursor now owns the pointer
         * again, and there is no motion event coming to say so — a hand that
         * lets go without moving would otherwise leave the pointer with the
         * window it was dragging in until it twitched. */
        if (server->seat->pointer_state.button_count == 0) {
            pointer_update_focus(server, lx, ly, event->time_msec);
        }
    }
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    server_notify_activity(server);
    if (lock_is_active(server)) return;

    // Scrolling over the desktop island steps between desktops. Consumed, so
    // it never also scrolls whatever window happens to be under the tray.
    // Vertical only: a touchpad sends both axes in one frame, and honouring
    // horizontal too would step two desktops per gesture.
    double stx, sty;
    FwmOutput *sto = NULL;
    if (event->delta != 0.0 &&
        event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
        (sto = tray_under_pointer(server, &stx, &sty))) {
        if (tray_desktop_island_hit(&sto->tray_strip, stx, sty)) {
            /* Step from where the camera is HEADED, not where it is: spinning
             * the wheel several notches must advance several desktops rather
             * than fight the slide still in flight. */
            int here = expo_target_desktop(server);
            if (here < 0) here = server_active_desktop(server);
            int d = here + (event->delta > 0.0 ? 1 : -1);
            int seam = 0;
            if (d < 0 || d >= FWM_DESKTOPS) {
                if (!server->config.camera.wrap) d = here;
                else { d = (d + FWM_DESKTOPS) % FWM_DESKTOPS; seam = 1; }
            }
            server_goto_desktop(server, d, seam);
            return;
        }
    }

    /* After the island, so the tray keeps its own meaning for the wheel while
     * the strip is up: over the island it still steps one desktop at a time,
     * anywhere else it pans the strip freely. */
    if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
        expo_handle_axis(server, event->delta)) return;

    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

void server_pointer_register(FwmServer *server) {
    server->cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
    server->cursor_button.notify = handle_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = handle_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

    server_seat_register(server);
}
