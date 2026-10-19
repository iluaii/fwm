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

#include "space_draw.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "server.h"
#include "snapshot.h"
#include "space_gl.h"

struct FwmSpaceDraw {
    FwmServer *server;
    struct wlr_scene_buffer *buf;
    struct wlr_buffer *gpu[2];
    int next;
    int w, h;          /* the screen */
    int bw, bh;        /* the buffers */
    double scale;
};

/* Clicks go through to the desktop, as they do through the star. */
static bool declines_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    (void)buffer; (void)sx; (void)sy;
    return false;
}

FwmSpaceDraw *space_draw_create(FwmServer *server, struct wlr_scene_tree *parent,
                                int w, int h, double scale) {
    if (!server || !parent || w <= 0 || h <= 0) return NULL;
    if (!space_gl_supported(server->wlr_renderer)) {
        wlr_log(WLR_INFO, "space: this renderer is not GLES2 — the wallpaper under it stays");
        return NULL;
    }
    FwmSpaceDraw *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->server = server;
    d->w = w;
    d->h = h;
    d->scale = scale;
    d->bw = (int)lround(w * scale);
    d->bh = (int)lround(h * scale);
    if (d->bw < 16) d->bw = 16;
    if (d->bh < 16) d->bh = 16;
    for (int i = 0; i < 2; i++) {
        d->gpu[i] = snapshot_alloc(server, d->bw, d->bh);
        if (!d->gpu[i]) { space_draw_destroy(d); return NULL; }
    }
    /* Not shown until the first frame is drawn: an unwritten buffer is
     * whatever the allocator last held. */
    d->buf = wlr_scene_buffer_create(parent, NULL);
    if (!d->buf) { space_draw_destroy(d); return NULL; }
    d->buf->point_accepts_input = declines_input;
    wlr_scene_buffer_set_dest_size(d->buf, w, h);
    wlr_log(WLR_INFO, "space: drawing %dx%d at %dx%d", w, h, d->bw, d->bh);
    return d;
}

bool space_draw_fits(const FwmSpaceDraw *d, int w, int h, double scale) {
    return d && d->w == w && d->h == h && fabs(d->scale - scale) < 1e-6;
}

void space_draw_place(FwmSpaceDraw *d, int x, int y) {
    if (d && d->buf) wlr_scene_node_set_position(&d->buf->node, x, y);
}

SpaceGlResult space_draw_render(FwmSpaceDraw *d, const SpaceUniforms *u) {
    if (!d || !d->buf) return SPACE_GL_FAILED;
    struct wlr_buffer *target = d->gpu[d->next];
    SpaceGlResult r = space_gl_render(d->server->wlr_renderer, target, u);
    if (r != SPACE_GL_DRAWN) return r;
    wlr_scene_buffer_set_buffer_with_damage(d->buf, target, NULL);
    /* Set again: handing a buffer over resets nothing, but a node that was
     * created empty has no size of its own until it has one. */
    wlr_scene_buffer_set_dest_size(d->buf, d->w, d->h);
    d->next ^= 1;
    return SPACE_GL_DRAWN;
}

void space_draw_raise(FwmSpaceDraw *d) {
    if (d && d->buf) wlr_scene_node_raise_to_top(&d->buf->node);
}

void space_draw_destroy(FwmSpaceDraw *d) {
    if (!d) return;
    if (d->buf) wlr_scene_node_destroy(&d->buf->node);
    for (int i = 0; i < 2; i++)
        if (d->gpu[i]) wlr_buffer_drop(d->gpu[i]);
    free(d);
}
