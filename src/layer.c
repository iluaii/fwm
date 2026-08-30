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

/* A bar belongs to ONE monitor, so each output is arranged on its own: the
 * surfaces that named it, against its box in layout coordinates. Giving every
 * surface the whole world (which is what a single-output compositor could get
 * away with) would stretch a bar across both screens. */
void layer_arrange(FwmServer *server) {
    if (server->screen_width <= 0 || server->screen_height <= 0) return;

    /* One usable area is kept, the primary's: it is what fake fullscreen and
     * the tiling layout read, and our own chrome lives there. */
    struct wlr_box primary_usable = { 0, 0, server->screen_width, server->screen_height };

    struct wlr_output_layout_output *lo;
    wl_list_for_each(lo, &server->output_layout->outputs, link) {
        struct wlr_box full;
        wlr_output_layout_get_box(server->output_layout, lo->output, &full);
        if (full.width <= 0 || full.height <= 0) continue;
        struct wlr_box usable = full;
        FwmOutput *mon = server_output_for(server, lo->output);

        /* Two passes, as the protocol requires: surfaces that reserve space are
         * placed first so the rest see the area that is actually left. */
        for (int pass = 0; pass < 2; pass++) {
            for (int l = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
                 l <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; l++) {
                FwmLayerSurface *ls;
                wl_list_for_each(ls, &server->layer_surfaces, link) {
                    struct wlr_layer_surface_v1 *s = ls->layer_surface;
                    if (!s->surface->mapped || !s->initialized) continue;
                    if (s->output != lo->output) continue;
                    if ((int)s->current.layer != l) continue;
                    int reserves = s->current.exclusive_zone > 0;
                    if (reserves != (pass == 0)) continue;
                    wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
                }
            }
        }

        /* Each monitor keeps its own answer: a bar on the second screen must
         * shrink fake fullscreen THERE and nowhere else. */
        if (mon) {
            mon->usable_area = usable;
            /* Something reserved space along the top of this screen, which is
             * where our own strip lives. Recorded, not acted on: [decor]
             * tray_yield decides what it means, at the point of use.
             *
             * Read off the reserved area rather than off the surfaces: a bar
             * that anchors top without an exclusive zone is asking to float
             * over the screen, not to replace anything. */
            mon->top_reserved = usable.y > full.y;
        }
        if (full.x == 0 && full.y == 0) primary_usable = usable;
    }

    server->usable_area = primary_usable;
}

/* Topmost surface DEMANDING the keyboard, i.e. asking for it exclusively: a
 * menu, a session dialog, a locker-alike. Those are the only ones taken
 * without being asked twice.
 *
 * On-demand is deliberately not a candidate. The protocol reads it as "give me
 * the keyboard when the user interacts with me", and granting it on map
 * instead meant a bar with a search field took the keys the moment it appeared
 * and a click on that same field did nothing. It gets them from
 * layer_keyboard_click and keeps them until the user works somewhere else. */
static FwmLayerSurface *layer_keyboard_candidate(FwmServer *server) {
    FwmLayerSurface *best = NULL;
    int best_layer = -1;

    FwmLayerSurface *ls;
    wl_list_for_each(ls, &server->layer_surfaces, link) {
        struct wlr_layer_surface_v1 *s = ls->layer_surface;
        if (!s->surface->mapped) continue;
        if (s->current.keyboard_interactive !=
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) continue;

        if ((int)s->current.layer > best_layer) {
            best_layer = (int)s->current.layer;
            best = ls;
        }
    }
    return best;
}

struct wlr_surface *layer_keyboard_exclusive(FwmServer *server) {
    FwmLayerSurface *ls = server->focused_layer;
    if (!ls || ls->layer_surface->current.keyboard_interactive !=
               ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) return NULL;
    return ls->layer_surface->surface;
}

/* Is an on-demand surface holding the keyboard because it was clicked into?
 * Asked of what the surface is asking for NOW: a client that drops back to
 * `none` (which is how several menus announce they are done with the keyboard,
 * a commit or two before they unmap) has stopped holding it, and leaving it
 * named as the holder kept the keys off every window until it went away. */
static bool layer_keyboard_on_demand(FwmServer *server) {
    FwmLayerSurface *ls = server->focused_layer;
    return ls && ls->layer_surface->current.keyboard_interactive ==
                 ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
}

bool layer_keyboard_release(FwmServer *server) {
    if (!server->focused_layer || layer_keyboard_exclusive(server)) return false;
    server->focused_layer = NULL;
    return true;
}

/* The layer surface under the pointer, if any. The same scene walk view_at
 * does, and confirmed against the list for the same reason: node.data cannot
 * say what type it holds, so an answer taken from it has to be checked against
 * the surfaces that actually exist. */
static FwmLayerSurface *layer_at(FwmServer *server, double lx, double ly) {
    double sx, sy;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, &sx, &sy);
    if (!node) return NULL;

    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) tree = tree->node.parent;
    if (tree == NULL) return NULL;

    FwmLayerSurface *found = tree->node.data, *ls;
    wl_list_for_each(ls, &server->layer_surfaces, link) {
        if (ls == found) return ls;
    }
    return NULL;
}

bool layer_keyboard_click(FwmServer *server, double lx, double ly) {
    /* An exclusive surface is up: it holds the keyboard against this too. */
    if (layer_keyboard_exclusive(server)) return false;

    FwmLayerSurface *ls = layer_at(server, lx, ly);
    if (!ls || ls == server->focused_layer) return false;
    if (ls->layer_surface->current.keyboard_interactive !=
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) return false;

    server->focused_layer = ls;
    server_keyboard_enter(server, ls->layer_surface->surface);
    return true;
}

/* The keyboard back to whoever should have it now that no layer surface does.
 * Through server_keyboard_target rather than straight to the focused window:
 * an override-redirect surface can be holding the keys (a Wine game's own
 * fullscreen window), and handing them to the window behind it would leave the
 * thing on screen unable to be typed into. */
static void layer_hand_back(FwmServer *server) {
    struct wlr_surface *back = server_keyboard_target(server);
    if (back) server_keyboard_enter(server, back);
    else      server_keyboard_clear(server);
}

/* This surface is going away, or has stopped being shown. If it was holding
 * the keyboard the keys go back HERE, before anything else looks at them:
 * clearing focused_layer and leaving the rest to layer_update_keyboard_focus
 * left that function comparing NULL against NULL, taking its early return, and
 * the keyboard nowhere at all. */
static void layer_drop_keyboard(FwmServer *server, FwmLayerSurface *ls) {
    if (server->focused_layer != ls) return;
    server->focused_layer = NULL;
    layer_hand_back(server);
}

void layer_update_keyboard_focus(FwmServer *server) {
    FwmLayerSurface *want = layer_keyboard_candidate(server);

    /* Nothing is demanding the keyboard and an on-demand surface is holding
     * it: this is not the path that takes it away. A click on a window is
     * (server_focus_view), which is the same rule an ordinary window follows. */
    if (!want && layer_keyboard_on_demand(server)) return;

    if (want == server->focused_layer) return;
    server->focused_layer = want;

    if (want) {
        server_keyboard_enter(server, want->layer_surface->surface);
        return;
    }
    layer_hand_back(server);
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
    layer_drop_keyboard(ls->server, ls);
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
        /* Its own output, not the world — see layer_arrange. */
        struct wlr_box full;
        wlr_output_layout_get_box(ls->server->output_layout, s->output, &full);
        if (full.width <= 0 || full.height <= 0)
            full = (struct wlr_box){ 0, 0, ls->server->screen_width, ls->server->screen_height };
        /* Against ITS monitor's usable area — the one it named, not the
         * primary's — falling back to the whole screen while nothing has
         * reserved anything there yet. */
        FwmOutput *mon = server_output_for(ls->server, s->output);
        struct wlr_box usable = mon ? mon->usable_area : full;
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

void layer_output_gone(FwmServer *server, struct wlr_output *output) {
    FwmLayerSurface *ls, *tmp;
    wl_list_for_each_safe(ls, tmp, &server->layer_surfaces, link) {
        /* Destroying it runs layer_handle_destroy, which takes it off the
         * list — hence the safe walk. */
        if (ls->layer_surface->output == output)
            wlr_layer_surface_v1_destroy(ls->layer_surface);
    }
}

static void layer_handle_destroy(struct wl_listener *listener, void *data) {
    FwmLayerSurface *ls = wl_container_of(listener, ls, destroy);
    (void)data;

    layer_drop_keyboard(ls->server, ls);

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

    /* A client that names no output gets the primary — the one at the layout
     * origin, which is also where our own chrome is. */
    if (!surface->output) {
        struct wlr_output_layout_output *lo;
        struct wlr_output *first = NULL;
        wl_list_for_each(lo, &server->output_layout->outputs, link) {
            if (!first) first = lo->output;
            if (lo->x == 0 && lo->y == 0) { first = lo->output; break; }
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
