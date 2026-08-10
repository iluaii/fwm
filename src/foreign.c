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

#include <stdlib.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>

#include "foreign.h"
#include "server.h"
#include "view.h"
#include "physics.h"

static void handle_request_activate(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, ftl_request_activate);
    (void)data;
    /* A panel click on a window living on another desktop is meaningless
     * unless the camera goes there — unlike xdg-activation, this IS a direct
     * user action on that window, so it always pans. */
    PhysicsBody *pb = physics_find_body(&view->server->physics, view->id);
    if (pb && pb->desktop_id >= 0 && pb->desktop_id < 10) {
        /* Bring the window's desktop up on the monitor the user is at. */
        server_output_show_desktop(view->server, server_active_output(view->server),
                                   pb->desktop_id, 0);
    }
    server_focus_view(view->server, view);
}

/* fwm has no separate maximised state: FAKE fullscreen already is one — the
 * window fills the work area, keeps its frame of reference and leaves the tray
 * and any bar visible. So that is what a panel's maximise button means here,
 * and real fullscreen is what its fullscreen button means. */
static void handle_request_maximize(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, ftl_request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    server_set_fullscreen(view->server, view, event->maximized, false);
}

static void handle_request_close(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, ftl_request_close);
    (void)data;
    view_send_close(view);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, ftl_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    server_set_fullscreen(view->server, view, event->fullscreen, true);
}

void foreign_init(FwmServer *server) {
    server->foreign_toplevel = wlr_foreign_toplevel_manager_v1_create(server->wl_display);
    if (!server->foreign_toplevel) {
        wlr_log(WLR_ERROR, "failed to create foreign-toplevel manager");
    }
}

void foreign_view_map(FwmView *view) {
    FwmServer *server = view->server;
    if (!server->foreign_toplevel || view->ftl) return;

    view->ftl = wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel);
    if (!view->ftl) return;

    foreign_view_title_changed(view);

    view->ftl_request_activate.notify = handle_request_activate;
    wl_signal_add(&view->ftl->events.request_activate, &view->ftl_request_activate);
    view->ftl_request_close.notify = handle_request_close;
    wl_signal_add(&view->ftl->events.request_close, &view->ftl_request_close);
    view->ftl_request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&view->ftl->events.request_fullscreen, &view->ftl_request_fullscreen);
    view->ftl_request_maximize.notify = handle_request_maximize;
    wl_signal_add(&view->ftl->events.request_maximize, &view->ftl_request_maximize);

    foreign_view_sync_output(view);
}

void foreign_view_unmap(FwmView *view) {
    if (!view->ftl) return;
    wl_list_remove(&view->ftl_request_activate.link);
    wl_list_remove(&view->ftl_request_close.link);
    wl_list_remove(&view->ftl_request_fullscreen.link);
    wl_list_remove(&view->ftl_request_maximize.link);
    wlr_foreign_toplevel_handle_v1_destroy(view->ftl);
    view->ftl = NULL;
    view->ftl_output = NULL;
}

void foreign_view_title_changed(FwmView *view) {
    if (!view->ftl) return;
    const char *title = view_title(view);
    const char *app_id = view_app_id(view);
    if (title)  wlr_foreign_toplevel_handle_v1_set_title(view->ftl, title);
    if (app_id) wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, app_id);
}

void foreign_view_set_activated(FwmView *view, bool activated) {
    if (!view->ftl) return;
    wlr_foreign_toplevel_handle_v1_set_activated(view->ftl, activated);
}

void foreign_view_set_fullscreen(FwmView *view, bool fullscreen) {
    if (!view->ftl) return;
    wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftl, fullscreen);
}

void foreign_view_sync_fullscreen(FwmView *view) {
    if (!view->ftl) return;
    PhysicsBody *b = physics_find_body(&view->server->physics, view->id);
    bool big = b && b->fullscreen;
    /* The two are exclusive here: one flag with two meanings, split by which
     * kind of fullscreen it is (see handle_request_maximize). */
    wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftl, big && view->fs_real);
    wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, big && !view->fs_real);
}

/* A window lives on a DESKTOP, and a desktop is shown by at most one monitor —
 * so the screen a panel should file this window under is whichever monitor is
 * showing its desktop, and a window on a desktop nobody is showing is on no
 * screen at all. That last case is the one that matters: without it a taskbar
 * filtered to its own monitor shows every window in the world. */
void foreign_view_sync_output(FwmView *view) {
    if (!view->ftl) return;

    struct wlr_output *want = NULL;
    PhysicsBody *b = physics_find_body(&view->server->physics, view->id);
    if (b) {
        FwmOutput *mon = server_output_showing(view->server, b->desktop_id);
        if (mon && mon->enabled) want = mon->wlr_output;
    }

    if (want == view->ftl_output) return;
    if (view->ftl_output)
        wlr_foreign_toplevel_handle_v1_output_leave(view->ftl, view->ftl_output);
    if (want)
        wlr_foreign_toplevel_handle_v1_output_enter(view->ftl, want);
    view->ftl_output = want;
}

void foreign_output_gone(FwmServer *server, struct wlr_output *output) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->ftl_output != output) continue;
        if (view->ftl) wlr_foreign_toplevel_handle_v1_output_leave(view->ftl, output);
        view->ftl_output = NULL;
    }
}
