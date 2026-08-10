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

#include "workspace.h"
#include "server.h"
#include "defines.h"

#include <stdio.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/util/log.h>

static int desktop_of(FwmServer *server, struct wlr_ext_workspace_handle_v1 *handle) {
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        if (server->workspace[d] == handle) return d;
    }
    return -1;
}

/* A client asked for something. Only activation is offered — the ten desktops
 * are fixed, so create/remove/assign are not among the capabilities and a
 * client that sends them anyway is simply ignored. Deactivation has no meaning
 * either: a monitor always shows SOME desktop, so the only way to stop showing
 * this one is to show another. */
static void handle_commit(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, workspace_commit);
    struct wlr_ext_workspace_v1_commit_event *event = data;

    struct wlr_ext_workspace_v1_request *req;
    wl_list_for_each(req, event->requests, link) {
        if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) continue;
        /* The handle may have been destroyed between the request and this
         * commit; wlroots nulls it rather than dropping the request. */
        if (!req->activate.workspace) continue;

        int d = desktop_of(server, req->activate.workspace);
        if (d < 0) continue;

        /* Which screen switches: the one the user is working on, the same
         * answer the super+N bind gives. A desktop already on a screen needs
         * nothing done to it — and switching to it would drag it onto the
         * pointer's monitor, trading two screens' contents over a click that
         * meant nothing. */
        FwmOutput *out = server_active_output(server);
        if (!out || out->desktop == d) continue;
        server_output_show_desktop(server, out, d, 0);
    }
}

/* The manager is going, and wlroots frees every group and workspace with it.
 *
 * Nothing controls the order here: wl_display_destroy takes down the globals
 * and the backend's outputs together, so a monitor can perfectly well be
 * destroyed AFTER this — and workspace_output_gone would then destroy a group
 * that has already been freed. Drop every pointer while they are still ours to
 * drop, and the paths above all become no-ops. */
static void handle_manager_destroy(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, workspace_destroy);

    server->workspace_manager = NULL;
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        server->workspace[d] = NULL;
        server->workspace_group[d] = NULL;
    }
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) out->ws_group = NULL;

    /* Off the signals before they are torn down — the manager asserts that its
     * commit signal has no listeners left. Re-initialised rather than merely
     * removed, so the sweep in server_destroy can safely remove them again. */
    wl_list_remove(&server->workspace_commit.link);
    wl_list_init(&server->workspace_commit.link);
    wl_list_remove(&server->workspace_destroy.link);
    wl_list_init(&server->workspace_destroy.link);
}

void workspace_init(FwmServer *server) {
    server->workspace_manager = wlr_ext_workspace_manager_v1_create(server->wl_display, 1);
    if (!server->workspace_manager) {
        wlr_log(WLR_ERROR, "failed to create ext-workspace manager");
        return;
    }

    server->workspace_commit.notify = handle_commit;
    wl_signal_add(&server->workspace_manager->events.commit, &server->workspace_commit);
    server->workspace_destroy.notify = handle_manager_destroy;
    wl_signal_add(&server->workspace_manager->events.destroy, &server->workspace_destroy);

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        /* The id is what a client may store to recognise this desktop in a
         * later session; the name is what it shows the user, and the binds
         * that reach these desktops are super+1 through super+0. */
        char id[16];
        snprintf(id, sizeof(id), "%d", d);
        server->workspace[d] = wlr_ext_workspace_handle_v1_create(
            server->workspace_manager, id,
            EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
        if (!server->workspace[d]) continue;

        char name[16];
        snprintf(name, sizeof(name), "%d", d + 1);
        wlr_ext_workspace_handle_v1_set_name(server->workspace[d], name);

        /* One coordinate, because the world is one strip: desktop d is the
         * d-th column, and that is the whole of its geometry. It is what lets
         * a bar order the ten without parsing their names. */
        uint32_t coord = (uint32_t)d;
        wlr_ext_workspace_handle_v1_set_coordinates(server->workspace[d], &coord, 1);

        /* Never hidden. The protocol's `hidden` asks a bar NOT TO DRAW the
         * workspace at all — it is for scratchpads and other places the user
         * does not navigate to. All ten of fwm's desktops are ordinary places
         * to be, empty or not, and a strip that showed only the one currently
         * on screen would be a strip of one. Whether a desktop is being shown
         * is what `active` says, and that is a different question. */
        wlr_ext_workspace_handle_v1_set_hidden(server->workspace[d], false);
    }
}

void workspace_output_added(FwmServer *server, FwmOutput *out) {
    if (!server->workspace_manager || out->ws_group) return;
    /* No create_workspace capability: the ten are all there will ever be. */
    out->ws_group = wlr_ext_workspace_group_handle_v1_create(server->workspace_manager, 0);
    if (!out->ws_group) return;
    wlr_ext_workspace_group_handle_v1_output_enter(out->ws_group, out->wlr_output);
}

void workspace_output_gone(FwmServer *server, FwmOutput *out) {
    if (!out->ws_group) return;
    /* The desktops it was showing lose their group here rather than waiting
     * for the next sync, which would read this pointer after it is freed. */
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        if (server->workspace_group[d] != out->ws_group) continue;
        if (server->workspace[d])
            wlr_ext_workspace_handle_v1_set_group(server->workspace[d], NULL);
        server->workspace_group[d] = NULL;
    }
    wlr_ext_workspace_group_handle_v1_destroy(out->ws_group);
    out->ws_group = NULL;
}

void workspace_sync(FwmServer *server) {
    if (!server->workspace_manager) return;

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        if (!server->workspace[d]) continue;

        FwmOutput *mon = server_output_showing(server, d);
        struct wlr_ext_workspace_group_handle_v1 *group =
            (mon && mon->enabled) ? mon->ws_group : NULL;

        if (group != server->workspace_group[d]) {
            wlr_ext_workspace_handle_v1_set_group(server->workspace[d], group);
            server->workspace_group[d] = group;
        }

        /* Active means "on a screen right now". More than one desktop is
         * active at a time on more than one monitor, which is the truth here
         * and why the protocol carries this per workspace and not per group. */
        bool active = group != NULL;
        if (active != server->workspace_active[d]) {
            wlr_ext_workspace_handle_v1_set_active(server->workspace[d], active);
            server->workspace_active[d] = active;
        }
    }
}
