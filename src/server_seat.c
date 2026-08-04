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

/* The seat: selections, drag-and-drop, pointer constraints, cursor shape and
 * xdg-activation. Split out of server_pointer.c, which is about where the
 * cursor IS and what it does there; this is about what the seat OWNS. */
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
#include <xkbcommon/xkbcommon.h>
#include "server_internal.h"

static void handle_request_cursor(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void handle_seat_request_set_selection(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

/* The icon rides the cursor directly in layout coordinates. It is NOT offset by
 * camera_x: it belongs to the pointer, not to the desktop under it, so it must
 * not slide when the camera pans mid-drag. */
void drag_icon_update_position(FwmServer *server) {
    if (!server->drag_icon) return;
    wlr_scene_node_set_position(&server->drag_icon->node,
                                (int)lround(server->cursor->x),
                                (int)lround(server->cursor->y));
}

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, drag_icon_destroy);
    (void)data;
    /* wlroots destroys the scene tree along with the icon; only drop our
     * pointer, or the next drag would raise a dangling node. */
    server->drag_icon = NULL;
    wl_list_remove(&server->drag_icon_destroy.link);
}

static void handle_seat_start_drag(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_start_drag);
    struct wlr_drag *drag = data;
    if (!drag->icon) return; /* a drag without an icon is perfectly legal */

    /* layer_overlay sits above windows and holds the tray/hints/launcher;
     * raising puts the icon above those too, so it is never covered. */
    server->drag_icon = wlr_scene_drag_icon_create(server->layer_overlay, drag->icon);
    if (!server->drag_icon) return;
    wlr_scene_node_raise_to_top(&server->drag_icon->node);
    drag_icon_update_position(server);

    server->drag_icon_destroy.notify = handle_drag_icon_destroy;
    wl_signal_add(&drag->icon->events.destroy, &server->drag_icon_destroy);
}

/* Which view owns a surface. Takes the root surface, so a subsurface (a video
 * player's content layer, an inhibitor's surface) resolves to its toplevel. */
FwmView *view_from_surface(FwmServer *server, struct wlr_surface *surface) {
    if (!surface) return NULL;
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (view_surface(v) == root) return v;
    }
    return NULL;
}

/* An inhibitor only counts while its surface is actually on screen — the
 * protocol says inhibitors apply "while this surface is visible", and on fwm a
 * video paused on desktop 7 must not keep the whole session awake. Recomputed
 * from scratch each tick: the list is empty or has one entry in practice, and
 * polling avoids hooking every path that can change visibility (map, unmap,
 * desktop switch, camera slide). */
void idle_inhibit_refresh(FwmServer *server) {
    if (!server->idle_notifier || !server->idle_inhibit) return;

    int visible_d = server_active_desktop(server);
    int inhibited = 0;
    struct wlr_idle_inhibitor_v1 *inh;
    wl_list_for_each(inh, &server->idle_inhibit->inhibitors, link) {
        FwmView *v = view_from_surface(server, inh->surface);
        if (!v) continue;
        PhysicsBody *pb = physics_find_body(&server->physics, v->id);
        /* No body means a hidden group member — not visible by definition. */
        if (pb && pb->desktop_id == visible_d) { inhibited = 1; break; }
    }

    if (inhibited != server->idle_inhibited) {
        wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited != 0);
        server->idle_inhibited = inhibited;
    }
}

/* ── pointer constraints ──────────────────────────────────────────────── */

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, constraint_destroy);
    (void)data;
    server->active_constraint = NULL;
    wl_list_remove(&server->constraint_destroy.link);
}

/* A constraint may only hold the pointer while its surface actually has
 * pointer focus, or a background window could capture the mouse forever. */
static void constraint_set_active(FwmServer *server,
                                  struct wlr_pointer_constraint_v1 *constraint) {
    if (server->active_constraint == constraint) return;

    if (server->active_constraint) {
        wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
        wl_list_remove(&server->constraint_destroy.link);
        server->active_constraint = NULL;
    }
    if (constraint) {
        server->active_constraint = constraint;
        server->constraint_destroy.notify = handle_constraint_destroy;
        wl_signal_add(&constraint->events.destroy, &server->constraint_destroy);
        wlr_pointer_constraint_v1_send_activated(constraint);
    }
}

/* Called whenever pointer focus may have changed. */
void constraints_follow_focus(FwmServer *server, struct wlr_surface *surface) {
    if (!server->pointer_constraints) return;
    struct wlr_pointer_constraint_v1 *found = NULL;
    if (surface) {
        found = wlr_pointer_constraints_v1_constraint_for_surface(
            server->pointer_constraints, surface, server->seat);
    }
    constraint_set_active(server, found);
}

static void handle_new_pointer_constraint(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_pointer_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;

    /* Activate immediately when the pointer is already over the requesting
     * surface — which is the normal case: a game grabs the mouse on click. */
    if (server->seat->pointer_state.focused_surface == constraint->surface) {
        constraint_set_active(server, constraint);
    }
}

/* cursor-shape-v1: clients name a cursor ("text", "grab") instead of supplying
 * a surface. Older clients keep using wl_pointer.set_cursor, which still works. */
static void handle_cursor_shape_request(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_shape_request);
    const struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

    /* Same rule as wl_pointer.set_cursor: only the client the pointer is
     * actually over may change the cursor. */
    struct wlr_seat_client *focused =
        server->seat->pointer_state.focused_client;
    if (event->seat_client != focused) return;

    const char *name = wlr_cursor_shape_v1_name(event->shape);
    if (name) wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
}

static void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, xdg_activation_request_activate);
    const struct wlr_xdg_activation_v1_request_activate_event *event = data;

    FocusActivatePolicy policy = server->config.focus.on_activate;
    if (policy == FOCUS_ACTIVATE_NEVER) return;

    /* Filters out clients that do not even claim to be acting on user input.
     * NOT real focus-stealing protection: wlroots stores whatever serial the
     * client sends without checking it against any input event, so a
     * determined app passes this trivially. Validating the serial against
     * recent input would be the real fix. */
    if (!event->token->seat) return;

    FwmView *view = view_from_surface(server, event->surface);
    if (!view) return;

    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    int target_d = (pb && pb->desktop_id >= 0 && pb->desktop_id < FWM_DESKTOPS) ? pb->desktop_id : -1;
    int visible_d = server_active_desktop(server);

    if (target_d >= 0 && target_d != visible_d) {
        /* Off-screen window. Panning the camera away from what the user is
         * working on is the disruptive part of activation, so only "always"
         * may do it; "same_desktop" drops the request instead. */
        if (policy != FOCUS_ACTIVATE_ALWAYS) return;
        server_output_show_desktop(server, server_active_output(server), target_d, 0);
    }
    server_focus_view(server, view);
}

static void handle_seat_request_start_drag(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;

    /* Only honour a drag the client can prove it owns, otherwise any client
     * could start one at will. A rejected request MUST destroy the source or
     * the requesting client waits forever. */
    if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
    } else if (event->drag->source) {
        wlr_data_source_destroy(event->drag->source);
    }
}


/* Called once from server_init(). The managers guarded below are optional:
 * wlroots returns NULL when one cannot be advertised, and the compositor is
 * expected to run without them. */
/* The seat's own listeners. Split out of server_pointer_register with the
 * handlers themselves: what a cursor DOES on screen and what a seat OWNS —
 * selections, drag-and-drop, constraints, activation — are two subjects that
 * only ever shared a file. */
void server_seat_register(FwmServer *server) {
    server->request_cursor.notify = handle_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
    server->seat_request_set_selection.notify = handle_seat_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection, &server->seat_request_set_selection);
    server->seat_request_set_primary_selection.notify = handle_seat_request_set_primary_selection;
    wl_signal_add(&server->seat->events.request_set_primary_selection, &server->seat_request_set_primary_selection);
    server->seat_request_start_drag.notify = handle_seat_request_start_drag;
    wl_signal_add(&server->seat->events.request_start_drag, &server->seat_request_start_drag);
    server->seat_start_drag.notify = handle_seat_start_drag;
    wl_signal_add(&server->seat->events.start_drag, &server->seat_start_drag);

    if (server->pointer_constraints) {
        server->new_pointer_constraint.notify = handle_new_pointer_constraint;
        wl_signal_add(&server->pointer_constraints->events.new_constraint,
                      &server->new_pointer_constraint);
    }
    if (server->cursor_shape) {
        server->cursor_shape_request.notify = handle_cursor_shape_request;
        wl_signal_add(&server->cursor_shape->events.request_set_shape,
                      &server->cursor_shape_request);
    }
    if (server->xdg_activation) {
        server->xdg_activation_request_activate.notify = handle_xdg_activation_request_activate;
        wl_signal_add(&server->xdg_activation->events.request_activate,
                      &server->xdg_activation_request_activate);
    }
}
