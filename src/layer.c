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

#include "layer.h"
#include "server.h"
#include "view.h"

#include <stdlib.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

static struct wlr_scene_tree *tree_for_layer(FwmServer *server,
                                             enum zwlr_layer_shell_v1_layer layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return server->ls_background;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return server->ls_bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return server->ls_top;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return server->ls_overlay;
    }
    return server->ls_top;
}

void layer_arrange(FwmServer *server) {
    if (server->screen_width <= 0 || server->screen_height <= 0) return;

    struct wlr_box full = { 0, 0, server->screen_width, server->screen_height };
    struct wlr_box usable = full;

    /* Two passes, as the protocol requires: surfaces that reserve space are
     * placed first so the rest see the area that is actually left. */
    for (int pass = 0; pass < 2; pass++) {
        for (int l = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
             l <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; l++) {
            FwmLayerSurface *ls;
            wl_list_for_each(ls, &server->layer_surfaces, link) {
                struct wlr_layer_surface_v1 *s = ls->layer_surface;
                if (!s->surface->mapped || !s->initialized) continue;
                if ((int)s->current.layer != l) continue;
                int reserves = s->current.exclusive_zone > 0;
                if (reserves != (pass == 0)) continue;
                wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
            }
        }
    }

    server->usable_area = usable;
}

/* Topmost surface asking for the keyboard wins; EXCLUSIVE outranks ON_DEMAND
 * so a lock/menu cannot be stolen from by a bar underneath it. */
static FwmLayerSurface *layer_keyboard_candidate(FwmServer *server) {
    FwmLayerSurface *best = NULL;
    int best_rank = -1;

    FwmLayerSurface *ls;
    wl_list_for_each(ls, &server->layer_surfaces, link) {
        struct wlr_layer_surface_v1 *s = ls->layer_surface;
        if (!s->surface->mapped) continue;
        if (s->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
            continue;

        int rank = (int)s->current.layer * 2;
        if (s->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) rank++;
        if (rank > best_rank) {
            best_rank = rank;
            best = ls;
        }
    }
    return best;
}

void layer_update_keyboard_focus(FwmServer *server) {
    FwmLayerSurface *want = layer_keyboard_candidate(server);

    if (want == server->focused_layer) return;
    server->focused_layer = want;

    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (want) {
        if (kbd) {
            wlr_seat_keyboard_notify_enter(server->seat, want->layer_surface->surface,
                                           kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
        } else {
            wlr_seat_keyboard_notify_enter(server->seat, want->layer_surface->surface,
                                           NULL, 0, NULL);
        }
        return;
    }

    /* Nothing wants it any more — give the keyboard back to the focused
     * window, the same way closing the built-in launcher does. */
    if (server->focused_view) {
        struct wlr_surface *surface = view_surface(server->focused_view);
        if (!surface) return;
        if (kbd) {
            wlr_seat_keyboard_notify_enter(server->seat, surface,
                                           kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
        } else {
            wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
        }
    } else {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
}

/* ── per-surface events ──────────────────────────────────────────────── */

static void layer_handle_map(struct wl_listener *listener, void *data) {
    FwmLayerSurface *ls = wl_container_of(listener, ls, map);
    (void)data;
    layer_arrange(ls->server);
    layer_update_keyboard_focus(ls->server);
    server_request_tray_redraw(ls->server);
}

static void layer_handle_unmap(struct wl_listener *listener, void *data) {
    FwmLayerSurface *ls = wl_container_of(listener, ls, unmap);
    (void)data;
    if (ls->server->focused_layer == ls) ls->server->focused_layer = NULL;
    layer_arrange(ls->server);
    layer_update_keyboard_focus(ls->server);
}

static void layer_handle_commit(struct wl_listener *listener, void *data) {
    FwmLayerSurface *ls = wl_container_of(listener, ls, commit);
    (void)data;
    struct wlr_layer_surface_v1 *s = ls->layer_surface;

    /* The client must be told a size before it can map — and it is not mapped
     * yet, so layer_arrange() (which only places mapped surfaces) would skip
     * it and the client would wait forever. Configure this one directly. */
    if (s->initial_commit) {
        struct wlr_box full = { 0, 0, ls->server->screen_width, ls->server->screen_height };
        struct wlr_box usable = ls->server->usable_area;
        if (usable.width <= 0 || usable.height <= 0) usable = full;
        wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
        return;
    }

    /* A live surface can move between layers, re-anchor or change its
     * exclusive zone at any time. */
    struct wlr_scene_tree *want = tree_for_layer(ls->server, s->current.layer);
    if (want && ls->scene->tree->node.parent != want) {
        wlr_scene_node_reparent(&ls->scene->tree->node, want);
    }
    layer_arrange(ls->server);
    layer_update_keyboard_focus(ls->server);
}

static void layer_handle_destroy(struct wl_listener *listener, void *data) {
    FwmLayerSurface *ls = wl_container_of(listener, ls, destroy);
    (void)data;

    if (ls->server->focused_layer == ls) ls->server->focused_layer = NULL;

    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->new_popup.link);
    wl_list_remove(&ls->link);

    FwmServer *server = ls->server;
    free(ls);

    layer_arrange(server);
    layer_update_keyboard_focus(server);
}

static void layer_handle_new_popup(struct wl_listener *listener, void *data) {
    FwmLayerSurface *ls = wl_container_of(listener, ls, new_popup);
    struct wlr_xdg_popup *popup = data;
    /* server.c's generic popup handler finds its parent tree through
     * wlr_surface->data; point it at this surface's popup tree. */
    popup->parent->data = ls->popups;
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *surface = data;

    /* Single-output compositor: clients that do not name an output get ours. */
    if (!surface->output) {
        struct wlr_output_layout_output *lo;
        struct wlr_output *first = NULL;
        wl_list_for_each(lo, &server->output_layout->outputs, link) {
            first = lo->output;
            break;
        }
        if (!first) {
            wlr_log(WLR_ERROR, "layer surface with no output to place it on");
            wlr_layer_surface_v1_destroy(surface);
            return;
        }
        surface->output = first;
    }

    FwmLayerSurface *ls = calloc(1, sizeof(*ls));
    if (!ls) return;
    ls->server = server;
    ls->layer_surface = surface;

    struct wlr_scene_tree *parent = tree_for_layer(server, surface->pending.layer);
    ls->scene = wlr_scene_layer_surface_v1_create(parent, surface);
    if (!ls->scene) {
        free(ls);
        return;
    }
    /* Popups belong above their surface but must not be clipped by it. */
    ls->popups = wlr_scene_tree_create(parent);

    surface->data = ls;
    ls->scene->tree->node.data = ls;

    ls->map.notify = layer_handle_map;
    wl_signal_add(&surface->surface->events.map, &ls->map);
    ls->unmap.notify = layer_handle_unmap;
    wl_signal_add(&surface->surface->events.unmap, &ls->unmap);
    ls->commit.notify = layer_handle_commit;
    wl_signal_add(&surface->surface->events.commit, &ls->commit);
    ls->destroy.notify = layer_handle_destroy;
    wl_signal_add(&surface->events.destroy, &ls->destroy);
    ls->new_popup.notify = layer_handle_new_popup;
    wl_signal_add(&surface->events.new_popup, &ls->new_popup);

    wl_list_insert(&server->layer_surfaces, &ls->link);
}

void layer_shell_init(FwmServer *server) {
    wl_list_init(&server->layer_surfaces);
    server->focused_layer = NULL;
    server->usable_area = (struct wlr_box){ 0, 0, 0, 0 };

    server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
    if (!server->layer_shell) {
        wlr_log(WLR_ERROR, "failed to create layer shell");
        return;
    }
    server->new_layer_surface.notify = handle_new_layer_surface;
    wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}
