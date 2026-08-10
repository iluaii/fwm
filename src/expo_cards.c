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

#include "expo_internal.h"
#include "snapshot.h"
#include "wallpaper.h"
#include "physics.h"
#include "theme.h"
#include "server_internal.h"
#include <math.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* ── the pictures ─────────────────────────────────────────────────────── */

void expo_build_cards(FwmExpo *e) {
    FwmServer *server = e->server;
    FwmOutput *cout = server_active_output(server);
    FwmWallpaper *wp = cout ? cout->wallpaper : NULL;
    int layers = wallpaper_layer_count(wp);
    if (layers > EXPO_MAX_WP_LAYERS) layers = EXPO_MAX_WP_LAYERS;

    if (e->gl) {
        /* The curved path needs textures and crops, not nodes: the cards are
         * drawn from them every frame. Imported once — re-importing a buffer
         * sixty times a second is pure waste, and the wallpaper's copy never
         * changes anyway. */
        for (int i = 0; i < layers; i++) {
            struct wlr_buffer *buf = wallpaper_layer_buffer(wp, i);
            if (!buf) continue;
            struct wlr_texture *tex =
                wlr_texture_from_buffer(server->wlr_renderer, buf);
            if (!tex) continue;
            int idx = e->wp_n++;
            e->wp_tex[idx] = tex;
            for (int d = 0; d < FWM_DESKTOPS; d++)
                wallpaper_layer_crop(wp, i, d * server->screen_width,
                                     server->screen_width, server->screen_height,
                                     &e->wp_crop[d][idx]);
        }
        wlr_log(WLR_DEBUG, "expo: curved strip, %d wallpaper layer(s)", e->wp_n);
        return;
    }

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        e->cards[d] = wlr_scene_tree_create(e->tree);
        if (!e->cards[d]) continue;

        /* Bottom of the card: the edge, then the base over all but its margin.
         * A desktop with no wallpaper (or one whose image failed to load) still
         * has to read as a slot on the strip rather than as a hole in it. */
        e->card_edge[d] = wlr_scene_rect_create(e->cards[d], 1, 1,
            (float[4]){EXPO_EDGE_GREY, EXPO_EDGE_GREY, EXPO_EDGE_GREY, 1.0f});
        e->card_base[d] = wlr_scene_rect_create(e->cards[d], 1, 1,
            (float[4]){EXPO_CARD_GREY, EXPO_CARD_GREY, EXPO_CARD_GREY, 1.0f});

        for (int i = 0; i < layers; i++) {
            struct wlr_buffer *buf = wallpaper_layer_buffer(wp, i);
            if (!buf) continue;
            struct wlr_scene_buffer *node = wlr_scene_buffer_create(e->cards[d], buf);
            if (!node) continue;
            /* The pan layers are wider than the screen and scroll behind it;
             * the crop is where that layer stands when the camera is parked on
             * THIS desktop, which is what makes the ten cards differ. */
            struct wlr_fbox crop;
            wallpaper_layer_crop(wp, i, d * server->screen_width,
                                 server->screen_width, server->screen_height, &crop);
            wlr_scene_buffer_set_source_box(node, &crop);
            e->card_layers[d][e->card_layer_n[d]++] = node;
        }
    }
    /* One line, because "the cards are grey" has now twice turned out to mean
     * "the wallpaper handed us nothing", and that is invisible from the outside. */
    wlr_log(WLR_DEBUG, "expo: %d cards, %d wallpaper layer(s) each",
            FWM_DESKTOPS, e->card_layer_n[0]);
}

/* Photograph one window onto the strip. False when it has nothing to
 * photograph yet — a client that has not painted, or one already gone. */
bool expo_add_item(FwmExpo *e, FwmView *view) {
    FwmServer *server = e->server;
    if (e->n_items >= MAX_WINDOWS) return false;
    if (!view->scene_tree || !view->scene_tree->node.enabled) return false;
    if (view->width <= 0 || view->height <= 0) return false;

    int bw = (int)lround(view->width  * EXPO_SNAP_SCALE);
    int bh = (int)lround(view->height * EXPO_SNAP_SCALE);
    struct wlr_buffer *buf = snapshot_alloc(server, bw, bh);
    if (!buf) return false;

    int lx = 0, ly = 0;
    wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly);
    if (!snapshot_subtree(server, buf, &view->scene_tree->node,
                          lx, ly, EXPO_SNAP_SCALE)) {
        wlr_buffer_drop(buf);
        return false;
    }

    struct wlr_scene_buffer *node = NULL;
    struct wlr_texture *tex = NULL;
    if (e->gl) {
        tex = wlr_texture_from_buffer(server->wlr_renderer, buf);
        if (!tex) { wlr_buffer_drop(buf); return false; }
    } else {
        node = wlr_scene_buffer_create(e->tree, buf);
        if (!node) { wlr_buffer_drop(buf); return false; }
    }

    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    int d = b ? b->desktop_id : view->x / server->screen_width;
    if (d < 0) d = 0;
    if (d >= FWM_DESKTOPS) d = FWM_DESKTOPS - 1;

    ExpoItem *it = &e->items[e->n_items++];
    it->view = view;
    it->node = node;
    it->tex = tex;
    it->buf = wlr_buffer_lock(buf);
    it->wx = view->x;
    it->wy = view->y;
    it->w = view->width;
    it->h = view->height;
    it->desktop = d;
    /* Slots are recycled, so these are not merely tidiness: inheriting another
     * window's `seen` can match this one's buffer by accident and skip its
     * first live refresh, and inheriting a `snapped` in the future skips them
     * all. */
    it->seen = view->last_buffer;
    it->snapped = 0.0;
    wlr_buffer_drop(buf);
    return true;
}

void expo_snap_windows(FwmExpo *e) {
    FwmView *view;
    wl_list_for_each(view, &e->server->views, link) expo_add_item(e, view);
}

/* Hand a frame callback to whatever the window is made of. Without this the
 * client sleeps: the strip hides the real windows, and a surface that is not
 * being composited is never told to draw. */
static void expo_frame_done(struct wlr_scene_buffer *buffer, int sx, int sy,
                            void *data) {
    (void)sx; (void)sy;
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
    if (ss) wlr_surface_send_frame_done(ss->surface, data);
}

/* Retake one window's picture into the buffer it already owns. The texture is
 * a view of that same buffer, so it comes along for free — the trick the spin
 * has always used. False when the window changed size and the whole item has to
 * be rebuilt instead. */
bool expo_resnap_item(FwmExpo *e, ExpoItem *it) {
    FwmView *view = it->view;
    if (!view->scene_tree || view->width != it->w || view->height != it->h)
        return false;

    int lx = 0, ly = 0;
    wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly);
    snapshot_subtree(e->server, it->buf, &view->scene_tree->node,
                     lx, ly, EXPO_SNAP_SCALE);
    it->wx = view->x;
    it->wy = view->y;
    it->seen = view->last_buffer;
    return true;
}

/* Keep one desktop alive.
 *
 * Everything on the strip is a photograph, and that is what makes ten desktops
 * cost nothing to look at. But the desktop in FRONT of you being a photograph
 * is the difference between a window manager and a screenshot of one: the
 * terminal stops blinking, the video stops, the clock stops.
 *
 * Two halves, and the first is the one that is easy to miss. A hidden client
 * gets no frame callbacks, so it stops drawing of its own accord and there is
 * nothing new to photograph however often you ask — the strip has to hand the
 * callbacks out itself. The second half is then the cheap one: retake the
 * picture when the client has actually committed something, no faster than
 * EXPO_LIVE_S.
 *
 * Only the desktop being looked at, and only with effects.live on — the same
 * switch that decides whether a spinning window chases its own content. */
void expo_live_pass(FwmExpo *e, double now) {
    FwmServer *server = e->server;
    if (server->config.effects.live <= 0.0) return;

    int live_d = expo_view_desktop(server);
    if (live_d < 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        if (it->desktop != live_d || !it->view->scene_tree) continue;

        wlr_scene_node_for_each_buffer(&it->view->scene_tree->node,
                                       expo_frame_done, &ts);

        if (it->view->last_buffer == it->seen) continue;
        if (now - it->snapped < EXPO_LIVE_S) continue;
        it->snapped = now;
        if (!expo_resnap_item(e, it)) {
            /* Hold the window in a local FIRST: expo_forget_view compacts the
             * slots, so `it` no longer describes this window afterwards — it
             * holds whatever moved into the hole, or nothing at all when this
             * was the last slot. Reading it->view back out of it re-added a
             * stranger, and at the end of the array handed expo_add_item a NULL
             * view to dereference. */
            FwmView *view = it->view;
            expo_forget_view(server, view);
            expo_add_item(e, view);
            i = -1;                       /* the array moved under us */
            continue;
        }
        expo_canvas_dirty(e);
    }
}

/* A window that opened while the strip was up.
 *
 * Polled rather than hooked into view_map, because at map time a client has
 * usually painted nothing yet — fwm holds a new window invisible for a few
 * commits for exactly that reason — and a photograph taken then is a blank
 * rectangle. Checking each frame costs a walk of a list that is at most a few
 * dozen long, and the window appears on the strip the moment it has a face. */
void expo_sync_items(FwmExpo *e) {
    FwmView *view;
    wl_list_for_each(view, &e->server->views, link) {
        bool known = false;
        for (int i = 0; i < e->n_items && !known; i++)
            if (e->items[i].view == view) known = true;
        if (!known) expo_add_item(e, view);
    }
}

