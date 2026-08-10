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
#include "layer.h"
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

/* ── drawing the ring ─────────────────────────────────────────────────── */

/* Vertex order everywhere below: top-left, bottom-left, top-right,
 * bottom-right — a triangle strip, and the winding the visibility test reads. */
enum { QTL = 0, QBL, QTR, QBR };

static void expo_quad(FwmExpo *e, ExpoPt a, ExpoPt b, double s0, double s1,
                      double wy0, double wy1, double u0, double u1,
                      struct scene3d_vert q[4]) {
    q[QTL] = expo_project(e, expo_facet_point(e, a, b, s0, wy0), u0, 0.0f);
    q[QBL] = expo_project(e, expo_facet_point(e, a, b, s0, wy1), u0, 1.0f);
    q[QTR] = expo_project(e, expo_facet_point(e, a, b, s1, wy0), u1, 0.0f);
    q[QBR] = expo_project(e, expo_facet_point(e, a, b, s1, wy1), u1, 1.0f);
}

/* Behind the eye, turned away, or off the screen.
 *
 * The winding test is what replaces every angle-based cull the cylinder
 * needed: a facet on the far side of the drum comes out wound backwards, and
 * so does one folded past the horizon. It is also the only test that stays
 * right once the camera can be anywhere. */
static bool expo_quad_front(const struct scene3d_vert q[4]) {
    const struct scene3d_vert *ring[4] = { &q[QTL], &q[QTR], &q[QBR], &q[QBL] };
    double area = 0.0;
    for (int i = 0; i < 4; i++) {
        const struct scene3d_vert *p = ring[i], *n = ring[(i + 1) % 4];
        area += (double)p->x * n->y - (double)n->x * p->y;
    }
    return area > 0.0;                 /* y is down, so front-facing is positive */
}

static bool expo_quad_onscreen(FwmExpo *e, const struct scene3d_vert q[4]) {
    for (int i = 0; i < 4; i++)
        if (q[i].w <= 1.0f) return false;

    float lo = q[0].x, hi = q[0].x, top = q[0].y, bot = q[0].y;
    for (int i = 1; i < 4; i++) {
        if (q[i].x < lo) lo = q[i].x;
        if (q[i].x > hi) hi = q[i].x;
        if (q[i].y < top) top = q[i].y;
        if (q[i].y > bot) bot = q[i].y;
    }
    return hi >= -8.0f && lo <= e->server->screen_width + 8.0f
        && bot >= -8.0f && top <= e->server->screen_height + 8.0f;
}

static bool expo_quad_visible(FwmExpo *e, const struct scene3d_vert q[4]) {
    return expo_quad_onscreen(e, q) && expo_quad_front(q);
}

/* The same quad grown by `m` screen px, for the frame drawn behind a card or a
 * hovered window. Pushing each corner away from the centre is exact enough at
 * these sizes and needs no plane of its own. */
static void expo_inflate(struct scene3d_vert q[4], double m) {
    double cx = 0.0, cy = 0.0;
    for (int i = 0; i < 4; i++) { cx += q[i].x; cy += q[i].y; }
    cx /= 4.0; cy /= 4.0;
    for (int i = 0; i < 4; i++) {
        double dx = q[i].x - cx, dy = q[i].y - cy;
        double len = sqrt(dx * dx + dy * dy);
        if (len < 1e-6) continue;
        q[i].x += (float)(dx / len * m);
        q[i].y += (float)(dy / len * m);
    }
}

static void expo_draw_item(FwmExpo *e, ExpoItem *it, double scale) {
    if (!it->tex) return;
    FwmServer *server = e->server;

    ExpoPt a, b;
    expo_facet_ends(e, it->desktop, &a, &b);
    double sw = server->screen_width;
    double s0 = (it->wx - (double)it->desktop * sw) / sw;
    double s1 = (it->wx + it->w - (double)it->desktop * sw) / sw;

    struct scene3d_vert q[4], frame[4];
    expo_quad(e, a, b, s0, s1, it->wy, it->wy + it->h, 0.0, 1.0, q);
    if (!expo_quad_visible(e, q)) return;

    if (e->hover == it) {
        const FwmTheme *th = theme_get();
        double m = EXPO_HILIGHT_PX * scale;
        if (m < 2.0) m = 2.0;
        memcpy(frame, q, sizeof(frame));
        expo_inflate(frame, m);
        scene3d_quad_solid((float[4]){(float)th->accent[0], (float)th->accent[1],
                                      (float)th->accent[2], 0.9f}, frame);
    }
    scene3d_quad(it->tex, q, 1.0f);
}

static void expo_draw_card(FwmExpo *e, int d, int looking_at, double open,
                           double scale) {
    ExpoPt a, b;
    expo_facet_ends(e, d, &a, &b);

    struct scene3d_vert q[4], frame[4];
    expo_quad(e, a, b, 0.0, 1.0, 0, e->server->screen_height, 0.0, 1.0, q);
    if (!expo_quad_visible(e, q)) return;

    /* The edge is a filled shape UNDER the card, so only its margin is ever
     * seen — the only way to frame a turned quad without four more of them. */
    const FwmTheme *th = theme_get();
    memcpy(frame, q, sizeof(frame));
    expo_inflate(frame, EXPO_EDGE_PX * open);
    if (d == looking_at) {
        scene3d_quad_solid((float[4]){(float)th->accent[0], (float)th->accent[1],
                                      (float)th->accent[2], 1.0f}, frame);
    } else {
        scene3d_quad_solid((float[4]){EXPO_EDGE_GREY, EXPO_EDGE_GREY,
                                      EXPO_EDGE_GREY, 1.0f}, frame);
    }

    scene3d_quad_solid((float[4]){EXPO_CARD_GREY, EXPO_CARD_GREY,
                                  EXPO_CARD_GREY, 1.0f}, q);

    for (int i = 0; i < e->wp_n; i++) {
        struct wlr_texture *tex = e->wp_tex[i];
        if (!tex || tex->width <= 0) continue;
        const struct wlr_fbox *crop = &e->wp_crop[d][i];
        struct scene3d_vert w[4];
        memcpy(w, q, sizeof(w));
        float uu0 = (float)(crop->x / tex->width);
        float uu1 = (float)((crop->x + crop->width) / tex->width);
        w[QTL].u = w[QBL].u = uu0;
        w[QTR].u = w[QBR].u = uu1;
        scene3d_quad(tex, w, 1.0f);
    }

    /* The windows of this desktop, on top of it and under the next card: with
     * the camera lifted, a window on the far side must not be painted over the
     * near side, and per-facet order is what says so. */
    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        if (it->desktop == d && it != e->drag) expo_draw_item(e, it, scale);
    }
}

/* The back of a card, which is the inside of the far wall. Worth drawing rather
 * than culling: looking into an open drum and seeing the desktops printed
 * faintly on the inside of it is what the shape actually is, and it is the
 * answer to why the middle used to be a black plate. */
/* The UI's own panel colour, premultiplied, at `a`. Everything drawn inside the
 * drum uses it: the ring then belongs to the same interface as the tray and the
 * menus rather than being a black shape in the middle of the screen. */
static void expo_ui_tint(float out[4], double a) {
    const FwmTheme *th = theme_get();
    out[0] = (float)(th->pill[0] * a);
    out[1] = (float)(th->pill[1] * a);
    out[2] = (float)(th->pill[2] * a);
    out[3] = (float)a;
}

static void expo_draw_inner(FwmExpo *e, int d, const struct scene3d_vert q[4]) {
    float tint[4];
    expo_ui_tint(tint, EXPO_INNER_ALPHA);
    scene3d_quad_solid(tint, q);

    for (int i = 0; i < e->wp_n; i++) {
        struct wlr_texture *tex = e->wp_tex[i];
        if (!tex || tex->width <= 0) continue;
        const struct wlr_fbox *crop = &e->wp_crop[d][i];
        struct scene3d_vert w[4];
        memcpy(w, q, sizeof(w));
        float uu0 = (float)(crop->x / tex->width);
        float uu1 = (float)((crop->x + crop->width) / tex->width);
        w[QTL].u = w[QBL].u = uu0;
        w[QTR].u = w[QBR].u = uu1;
        scene3d_quad(tex, w, EXPO_INNER_IMAGE);
    }
}

/* The canvas compares a signature to decide whether to redraw, and a buffer
 * repainted under it changes nothing that signature can see. This says so. */
void expo_canvas_dirty(FwmExpo *e) {
    e->drawn_zoom = -1.0;   /* no zoom is negative, so nothing can match it */
}

/* True when the picture would come out identical to the one already on screen. */
static bool expo_canvas_current(FwmExpo *e) {
    FwmServer *server = e->server;
    bool same = e->drawn_zoom == e->zoom
             && e->drawn_seam == e->seam
             && e->drawn_pan == e->pan
             && e->drawn_tilt == e->tilt
             && e->drawn_dist == e->dist
             && e->drawn_cam == e->out->camera_x
             && e->drawn_items == e->n_items
             && e->drawn_wrap == server->config.camera.wrap
             && e->drawn_hover == (void *)e->hover
             && e->drawn_drag == (void *)e->drag
             && (!e->drag || (e->drawn_drag_x == e->drag->wx
                           && e->drawn_drag_y == e->drag->wy));
    if (same) return true;

    e->drawn_zoom = e->zoom;
    e->drawn_seam = e->seam;
    e->drawn_pan = e->pan;
    e->drawn_tilt = e->tilt;
    e->drawn_dist = e->dist;
    e->drawn_cam = e->out->camera_x;
    e->drawn_items = e->n_items;
    e->drawn_wrap = server->config.camera.wrap;
    e->drawn_hover = e->hover;
    e->drawn_drag = e->drag;
    if (e->drag) { e->drawn_drag_x = e->drag->wx; e->drawn_drag_y = e->drag->wy; }
    return false;
}

static void expo_draw_gl(FwmExpo *e) {
    FwmServer *server = e->server;
    if (expo_canvas_current(e)) return;
    int idx = e->canvas_i ^ 1;
    struct wlr_buffer *dst = e->canvas_buf[idx];
    if (!dst || !e->canvas) return;
    if (!scene3d_begin(server->wlr_renderer, dst)) return;  /* keep last frame */

    double open = expo_openness(e);
    double scale = expo_scale(e);
    int looking_at = expo_view_desktop(server);

    /* Far to near. A flat facet cannot intersect another, so the order of the
     * whole card — its edge, its wallpaper and its windows together — is all
     * the depth sorting a drum needs. */
    int order[FWM_DESKTOPS];
    double key[FWM_DESKTOPS];
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        ExpoPt a, b;
        expo_facet_ends(e, d, &a, &b);
        ExpoPt mid = { (a.x + b.x) / 2.0, 0.0, (a.z + b.z) / 2.0 };
        order[d] = d;
        key[d] = expo_project(e, mid, 0, 0).w;
    }
    for (int i = 1; i < FWM_DESKTOPS; i++) {
        for (int j = i; j > 0 && key[order[j]] > key[order[j - 1]]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    }
    /* The inside of the far wall first, then the wall facing us. The drum has
     * no floor and no lid on purpose: a disc across the middle of the ring is
     * a black plate wherever you put it, and without one you simply see
     * through the ring to the wallpaper behind it, which is what a ring does. */
    for (int i = 0; i < FWM_DESKTOPS; i++) {
        int d = order[i];
        ExpoPt a, b;
        expo_facet_ends(e, d, &a, &b);
        struct scene3d_vert q[4];
        expo_quad(e, a, b, 0.0, 1.0, 0, server->screen_height, 0.0, 1.0, q);
        if (expo_quad_onscreen(e, q) && !expo_quad_front(q))
            expo_draw_inner(e, d, q);
    }

    for (int i = 0; i < FWM_DESKTOPS; i++)
        expo_draw_card(e, order[i], looking_at, open, scale);

    /* The window in hand is above everything, wherever it is being carried. */
    if (e->drag) expo_draw_item(e, e->drag, scale);

    scene3d_end();

    wlr_scene_buffer_set_buffer_with_damage(e->canvas, dst, NULL);
    e->canvas_i = idx;
}

/* ── layout ───────────────────────────────────────────────────────────── */

/* Place every node from the current zoom and pan. Cheap enough to run on every
 * frame, which is what lets the whole animation be two scalars. */
void expo_layout(FwmExpo *e) {
    FwmServer *server = e->server;
    double s = expo_scale(e);
    double open = expo_openness(e);
    int looking_at = expo_view_desktop(server);

    if (e->backdrop) {
        /* Fade in with the zoom, so a strip that is barely open is barely
         * darker than the desktop it grew out of. */
        float a = (float)(EXPO_BACKDROP_ALPHA * open);
        wlr_scene_rect_set_color(e->backdrop, (float[4]){0.0f, 0.0f, 0.0f, a});
    }

    if (e->gl) {
        expo_draw_gl(e);
        return;
    }

    for (int d = 0; d < FWM_DESKTOPS; d++) {
        if (!e->cards[d]) continue;
        /* A card's corners in WORLD coordinates: desktop d starts at
         * d * screen_width, not at zero. Passing the desktop-local box here
         * drew all ten cards on top of each other, a gap apart — a stack of
         * slivers where the strip should have been. */
        double x0, y0, x1, y1;
        expo_to_screen(e, (double)d * server->screen_width, 0, d, &x0, &y0);
        expo_to_screen(e, (double)(d + 1) * server->screen_width,
                       server->screen_height, d, &x1, &y1);
        int cw = (int)lround(x1 - x0), chh = (int)lround(y1 - y0);
        if (cw < 1) cw = 1;
        if (chh < 1) chh = 1;
        wlr_scene_node_set_position(&e->cards[d]->node,
                                    (int)lround(x0), (int)lround(y0));
        if (e->card_base[d]) wlr_scene_rect_set_size(e->card_base[d], cw, chh);
        for (int i = 0; i < e->card_layer_n[d]; i++)
            wlr_scene_buffer_set_dest_size(e->card_layers[d][i], cw, chh);

        if (e->card_edge[d]) {
            /* A fixed number of SCREEN px, not scaled with the strip: the edge
             * says where a desktop ends, and at the near zoom step a scaled one
             * would be a third of a pixel. It fades in with the gap so the live
             * view is not framed. */
            int m = (int)lround(EXPO_EDGE_PX * open);
            wlr_scene_rect_set_size(e->card_edge[d], cw + 2 * m, chh + 2 * m);
            wlr_scene_node_set_position(&e->card_edge[d]->node, -m, -m);
            const FwmTheme *th = theme_get();
            bool here = d == looking_at;
            wlr_scene_rect_set_color(e->card_edge[d], here
                ? (float[4]){(float)th->accent[0], (float)th->accent[1],
                             (float)th->accent[2], 1.0f}
                : (float[4]){EXPO_EDGE_GREY, EXPO_EDGE_GREY, EXPO_EDGE_GREY, 1.0f});
        }
    }

    for (int i = 0; i < e->n_items; i++) {
        ExpoItem *it = &e->items[i];
        double x0, y0, x1, y1;
        expo_to_screen(e, it->wx, it->wy, it->desktop, &x0, &y0);
        expo_to_screen(e, it->wx + it->w, it->wy + it->h, it->desktop, &x1, &y1);
        wlr_scene_buffer_set_dest_size(it->node,
                                       (int)lround(x1 - x0), (int)lround(y1 - y0));
        wlr_scene_node_set_position(&it->node->node, (int)lround(x0), (int)lround(y0));
    }

    if (e->hilight) {
        if (e->hover) {
            ExpoItem *it = e->hover;
            int m = (int)lround(EXPO_HILIGHT_PX * s);
            if (m < 1) m = 1;
            double x0, y0, x1, y1;
            expo_to_screen(e, it->wx, it->wy, it->desktop, &x0, &y0);
            expo_to_screen(e, it->wx + it->w, it->wy + it->h, it->desktop, &x1, &y1);
            wlr_scene_rect_set_size(e->hilight,
                                    (int)lround(x1 - x0) + 2 * m,
                                    (int)lround(y1 - y0) + 2 * m);
            wlr_scene_node_set_position(&e->hilight->node,
                                        (int)lround(x0) - m, (int)lround(y0) - m);
            wlr_scene_node_set_enabled(&e->hilight->node, true);
        } else {
            wlr_scene_node_set_enabled(&e->hilight->node, false);
        }
    }
}

/* ── the live scene underneath ────────────────────────────────────────── */

/* What the strip replaces while it is up. Deliberately NOT the whole world:
 *
 *   layer_background stays — the live wallpaper is the strip's own backdrop,
 *   dimmed by the rect over it, so the cards float on a picture rather than on
 *   a black field;
 *   layer_overlay stays — the tray belongs to the session, not to whatever is
 *   underneath it, and a strip that swallowed the clock and the mode pills
 *   read as a different machine. The strip's tree sits BELOW it for that
 *   reason. */
/* Take the desktop away from ONE monitor while its strip is up, and give it
 * back afterwards. The other monitor keeps working: its windows, its bars and
 * its wallpaper are untouched.
 *
 * The windows are parked off the layout rather than disabled — the enabled flag
 * on a window's tree already belongs to the open animation, and two owners of
 * one flag is how a window ends up invisible forever. The wallpaper layers have
 * no such second owner, so those are simply disabled.
 *
 * Only the layers UNDER the windows go: background and bottom are the desktop,
 * and the desktop is what the strip is replacing. A bar on top or overlay is
 * chrome — it stays, exactly as fwm's own status strip does, because a shell
 * that vanishes whenever you look at your desktops is not much of a shell. */
void expo_set_world_visible(FwmServer *server, FwmOutput *out, bool visible) {
    if (!out) return;
    out->hide_world = !visible;
    server_views_place(server);

    FwmLayerSurface *ls;
    wl_list_for_each(ls, &server->layer_surfaces, link) {
        if (ls->layer_surface->output != out->wlr_output || !ls->scene) continue;
        enum zwlr_layer_shell_v1_layer layer = ls->layer_surface->current.layer;
        if (layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND
            && layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) continue;
        wlr_scene_node_set_enabled(&ls->scene->tree->node, visible);
    }
}

