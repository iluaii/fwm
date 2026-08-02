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
#include "ui/hints.h"
#include "ui/errors.h"
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
#include <wlr/util/region.h>
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
    return tree->node.data;
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
    if (tx) *tx = server->cursor->x - o->tray_buffer->node.x;
    if (ty) *ty = server->cursor->y - o->tray_buffer->node.y;
    return o;
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

    bool locked = false;
    if (server->active_constraint && !lock_is_active(server)) {
        if (server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            /* Mouse-look: the cursor does not move at all, but we still need
             * to send motion events with constant coordinates. */
            locked = true;
        } else {
            /* Confined: allow the move only while it stays inside the region. */
            FwmView *cv = view_from_surface(server, server->active_constraint->surface);
            if (cv) {
                double nx = server->cursor->x + event->delta_x;
                double ny = server->cursor->y + event->delta_y;
                double vsx, vsy;
                /* Only testable while the window has a place on a screen. With its
                 * desktop off every monitor there is nothing to measure the region
                 * against, and confining the pointer to a rectangle read out of
                 * uninitialised memory would strand the cursor; let the move
                 * through instead. */
                double surface_x = cv->x;
                double surface_y = cv->y;
                if (cv->type == FWM_VIEW_XDG && cv->xdg_toplevel && cv->xdg_toplevel->base) {
                    surface_x -= cv->xdg_toplevel->base->current.geometry.x;
                    surface_y -= cv->xdg_toplevel->base->current.geometry.y;
                }
                if (server_world_to_screen(server, surface_x, surface_y, &vsx, &vsy)) {
                    double sx1 = server->cursor->x - vsx;
                    double sy1 = server->cursor->y - vsy;
                    double sx2 = nx - vsx;
                    double sy2 = ny - vsy;
                    double sx2_out, sy2_out;
                    
                    if (wlr_region_confine(&server->active_constraint->region,
                                           sx1, sy1, sx2, sy2, &sx2_out, &sy2_out)) {
                        event->delta_x = sx2_out - sx1;
                        event->delta_y = sy2_out - sy1;
                    } else {
                        // The old position was outside the region entirely. Let it move so focus can update and break the constraint.
                        server_notify_activity(server);
                    }
                }
            }
        }
    }

    if (!locked) {
        wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    }
    
    // Process pointer movement
    double lx = server->cursor->x;
    double ly = server->cursor->y;

    server_notify_activity(server);
    if (lock_is_active(server)) return; /* nothing under the lock may be reached */
    drag_icon_update_position(server);

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

    // Focus follows pointer
    {
        struct wlr_surface *surface = NULL;
        double sx, sy;
        FwmView *view = view_at(server, lx, ly, &surface, &sx, &sy);
        if (surface) {
            // view == NULL happens over unmanaged X11 surfaces (menus,
            // tooltips): they still get pointer events, just no focus change.
            if (view && !locked) server_focus_view(server, view);
            if (!locked) {
                wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
                wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
            }
            constraints_follow_focus(server, surface);
        } else {
            // Over the empty background: no client owns the cursor, so restore
            // our default image (otherwise it keeps the last client's cursor or
            // none at all).
            if (!locked) {
                wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
                wlr_seat_pointer_clear_focus(server->seat);
                constraints_follow_focus(server, NULL);
            }
        }
    }
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    server_notify_activity(server);
    if (lock_is_active(server)) return; /* nothing under the lock may be reached */
    drag_icon_update_position(server);
    if (launcher_is_open(server->launcher)) {
        launcher_handle_motion(server->launcher, server->cursor->x, server->cursor->y);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
    }
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
