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

#include "shortcuts.h"
#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "hyprland-global-shortcuts-v1-protocol.h"

/* One registered shortcut: a name, and the client resource to send edges to. */
typedef struct {
    struct wl_list link;              /* FwmServer.shortcuts */
    struct wl_resource *resource;
    char *app_id;
    char *id;
} FwmShortcut;

static void shortcut_handle_resource_destroy(struct wl_resource *resource) {
    FwmShortcut *sc = wl_resource_get_user_data(resource);
    if (!sc) return;
    wl_list_remove(&sc->link);
    free(sc->app_id);
    free(sc->id);
    free(sc);
}

static void shortcut_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct hyprland_global_shortcut_v1_interface shortcut_impl = {
    .destroy = shortcut_handle_destroy,
};

static FwmShortcut *shortcut_find(FwmServer *server, const char *app_id, const char *id) {
    FwmShortcut *sc;
    wl_list_for_each(sc, &server->shortcuts, link) {
        if (strcmp(sc->app_id, app_id) == 0 && strcmp(sc->id, id) == 0) return sc;
    }
    return NULL;
}

static void manager_handle_register(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t new_id, const char *id, const char *app_id,
                                    const char *description, const char *trigger_description) {
    FwmServer *server = wl_resource_get_user_data(resource);

    /* The protocol makes a duplicate an error rather than a silent overwrite:
     * two clients answering one key is not something either of them could have
     * made sense of. */
    if (shortcut_find(server, app_id, id)) {
        wl_resource_post_error(resource,
                               HYPRLAND_GLOBAL_SHORTCUTS_MANAGER_V1_ERROR_ALREADY_TAKEN,
                               "shortcut %s:%s is already registered", app_id, id);
        return;
    }

    FwmShortcut *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        wl_client_post_no_memory(client);
        return;
    }
    sc->app_id = strdup(app_id);
    sc->id = strdup(id);
    if (!sc->app_id || !sc->id) {
        free(sc->app_id);
        free(sc->id);
        free(sc);
        wl_client_post_no_memory(client);
        return;
    }

    sc->resource = wl_resource_create(client, &hyprland_global_shortcut_v1_interface,
                                      wl_resource_get_version(resource), new_id);
    if (!sc->resource) {
        free(sc->app_id);
        free(sc->id);
        free(sc);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(sc->resource, &shortcut_impl, sc,
                                   shortcut_handle_resource_destroy);
    wl_list_insert(&server->shortcuts, &sc->link);

    wlr_log(WLR_INFO, "global shortcut registered: %s:%s (%s)", app_id, id,
            description && *description ? description : "no description");
    (void)trigger_description;   /* what the CLIENT would draw; fwm owns the keys */
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct hyprland_global_shortcuts_manager_v1_interface manager_impl = {
    .register_shortcut = manager_handle_register,
    .destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    FwmServer *server = data;
    struct wl_resource *resource = wl_resource_create(
        client, &hyprland_global_shortcuts_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    /* No per-manager state: the shortcuts it creates outlive it by protocol,
     * so they hang off the server and the manager is only a factory. */
    wl_resource_set_implementation(resource, &manager_impl, server, NULL);
}

void shortcuts_init(FwmServer *server) {
    wl_list_init(&server->shortcuts);
    if (!wl_global_create(server->wl_display, &hyprland_global_shortcuts_manager_v1_interface,
                          1, server, manager_bind)) {
        wlr_log(WLR_ERROR, "failed to create the global-shortcuts manager");
    }
}

/* Both edges carry a timestamp, split across three uints because a 64-bit
 * seconds count does not fit one. */
static void send_edge(FwmShortcut *sc, bool pressed) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t hi = (uint32_t)((uint64_t)now.tv_sec >> 32);
    uint32_t lo = (uint32_t)((uint64_t)now.tv_sec & 0xffffffff);
    if (pressed) {
        hyprland_global_shortcut_v1_send_pressed(sc->resource, hi, lo, (uint32_t)now.tv_nsec);
    } else {
        hyprland_global_shortcut_v1_send_released(sc->resource, hi, lo, (uint32_t)now.tv_nsec);
    }
}

bool shortcuts_press(FwmServer *server, const char *app_id, const char *id) {
    FwmShortcut *sc = shortcut_find(server, app_id, id);
    if (!sc) return false;
    send_edge(sc, true);
    return true;
}

bool shortcuts_release(FwmServer *server, const char *app_id, const char *id) {
    FwmShortcut *sc = shortcut_find(server, app_id, id);
    if (!sc) return false;
    send_edge(sc, false);
    return true;
}

bool shortcuts_trigger(FwmServer *server, const char *app_id, const char *id) {
    FwmShortcut *sc = shortcut_find(server, app_id, id);
    if (!sc) return false;
    /* Press and release together. A bind fires as one event in fwm — there is
     * no held state to mirror — and a client left holding a key down forever
     * would be worse than one that never saw it go down at all. */
    send_edge(sc, true);
    send_edge(sc, false);
    return true;
}
