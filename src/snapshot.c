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

#include "snapshot.h"
#include "server.h"
#include "wallpaper.h"
#include <math.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <drm_fourcc.h>

struct snapshot_ctx {
    struct wlr_render_pass *pass;
    struct wlr_renderer *renderer;
    int origin_x, origin_y;      /* subtree point that lands on the buffer's top-left */
    double scale;
};

static void snapshot_add_buffer(struct wlr_scene_buffer *scene_buffer,
                                int sx, int sy, void *data) {
    struct snapshot_ctx *ctx = data;
    if (!scene_buffer->buffer) return;

    /* A buffer that belongs to a client surface already HAS a texture: wlroots
     * imported it when the client committed, and the scene draws the window
     * from it sixty times a second. Importing the same dmabuf again for our own
     * pass — and throwing the import away at the end of it — was costing about
     * as much as the pass itself, which at one pass every 150ms is a 5ms frame
     * every 150ms: a hitch you can see, and the whole reason a slow spin
     * juddered. Borrow the cached one and only import what is genuinely ours
     * (an effect's own buffer, a ghost). */
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(scene_buffer);
    struct wlr_texture *cached = ss ? wlr_surface_get_texture(ss->surface) : NULL;
    struct wlr_texture *tex = cached
        ? cached : wlr_texture_from_buffer(ctx->renderer, scene_buffer->buffer);
    if (!tex) return;

    /* dest_size 0 means "use the buffer size", the same rule the scene follows. */
    int w = scene_buffer->dst_width  ? scene_buffer->dst_width  : (int)tex->width;
    int h = scene_buffer->dst_height ? scene_buffer->dst_height : (int)tex->height;

    /* Scale the two edges rather than the origin and the size: rounding each
     * corner to the same grid is what keeps a row of scaled buffers from
     * showing a one-pixel seam between them. */
    int x0 = (int)lround((sx - ctx->origin_x) * ctx->scale);
    int y0 = (int)lround((sy - ctx->origin_y) * ctx->scale);
    int x1 = (int)lround((sx + w - ctx->origin_x) * ctx->scale);
    int y1 = (int)lround((sy + h - ctx->origin_y) * ctx->scale);

    wlr_render_pass_add_texture(ctx->pass, &(struct wlr_render_texture_options){
        .texture = tex,
        .dst_box = { .x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0 },
        .alpha = &scene_buffer->opacity,
        .transform = scene_buffer->transform,
        .filter_mode = WLR_SCALE_FILTER_BILINEAR,
        .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
    });
    if (!cached) wlr_texture_destroy(tex);   /* only ever destroy our own import */
}

struct wlr_buffer *snapshot_alloc(FwmServer *server, int w, int h) {
    if (!server->wlr_allocator || w <= 0 || h <= 0) return NULL;
    struct wlr_buffer *buf = NULL;
    struct wlr_drm_format_set fmts = {0};
    if (wlr_drm_format_set_add(&fmts, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID)) {
        const struct wlr_drm_format *fmt = wlr_drm_format_set_get(&fmts, DRM_FORMAT_ARGB8888);
        if (fmt) buf = wlr_allocator_create_buffer(server->wlr_allocator, w, h, fmt);
    }
    wlr_drm_format_set_finish(&fmts);
    return buf;
}

bool snapshot_subtree(FwmServer *server, struct wlr_buffer *dst,
                      struct wlr_scene_node *node,
                      int origin_x, int origin_y, double scale) {
    if (!server->wlr_renderer || !node || !dst) return false;

    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(server->wlr_renderer, dst, NULL);
    if (!pass) return false;

    /* Start from transparent: content that does not cover the whole box must
     * not pick up whatever the allocator handed us. */
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .x = 0, .y = 0, .width = dst->width, .height = dst->height },
        .color = { .r = 0, .g = 0, .b = 0, .a = 0 },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    struct snapshot_ctx ctx = {
        .pass = pass, .renderer = server->wlr_renderer,
        .origin_x = origin_x, .origin_y = origin_y, .scale = scale,
    };
    wlr_scene_node_for_each_buffer(node, snapshot_add_buffer, &ctx);

    return wlr_render_pass_submit(pass);
}

bool snapshot_world(FwmServer *server, FwmOutput *out, struct wlr_buffer *dst) {
    if (!server->wlr_renderer || !dst) return false;

    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(server->wlr_renderer, dst, NULL);
    if (!pass) return false;

    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = { .x = 0, .y = 0, .width = dst->width, .height = dst->height },
        .color = { .r = 0, .g = 0, .b = 0, .a = 1 },
        .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    /* The wallpaper comes from its own small copy, not from the layer on
     * screen: those are cairo overlays whose pixels the scene has taken and
     * whose CPU side the wallpaper then freed, so importing them again yields
     * nothing at all. It is the same trap the desktop strip's cards fell into
     * twice; see wallpaper.h. */
    FwmOutput *sout = out ? out : server_active_output(server);
    FwmWallpaper *wp = sout ? sout->wallpaper : NULL;
    for (int i = 0; i < wallpaper_layer_count(wp); i++) {
        struct wlr_buffer *src = wallpaper_layer_buffer(wp, i);
        if (!src) continue;
        struct wlr_texture *tex = wlr_texture_from_buffer(server->wlr_renderer, src);
        if (!tex) continue;
        struct wlr_fbox crop;
        /* The monitor's own size, not the column's: a wallpaper is built for
         * the screen it hangs on (wallpaper_create), so cropping a 1920x1080
         * viewport out of a 1366x768 one hands the pass a box larger than the
         * picture and the photograph comes out squashed. */
        int crop_w = sout && sout->box.width  > 0 ? sout->box.width  : server->screen_width;
        int crop_h = sout && sout->box.height > 0 ? sout->box.height : server->screen_height;
        wallpaper_layer_crop(wp, i, sout ? sout->camera_x : 0, crop_w, crop_h, &crop);
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture = tex,
            .src_box = crop,
            .dst_box = { .x = 0, .y = 0,
                         .width = dst->width, .height = dst->height },
            .filter_mode = WLR_SCALE_FILTER_BILINEAR,
            .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
        });
        wlr_texture_destroy(tex);
    }

    /* Then everything that is not screen furniture. The furniture is left out
     * by disabling those trees for the length of the walk and putting them
     * back, which costs nothing: no frame is drawn in between. */
    struct wlr_scene_tree *furniture[] = {
        server->ls_top, server->ls_overlay, server->layer_overlay,
        server->layer_lock,
    };
    bool was[sizeof(furniture) / sizeof(furniture[0])];
    for (size_t i = 0; i < sizeof(furniture) / sizeof(furniture[0]); i++) {
        was[i] = furniture[i] && furniture[i]->node.enabled;
        if (furniture[i]) wlr_scene_node_set_enabled(&furniture[i]->node, false);
    }

    /* Only this monitor's slice of the layout: the scene is laid out across
     * every screen, and the picture wanted is the one screen. */
    struct snapshot_ctx ctx = {
        .pass = pass, .renderer = server->wlr_renderer,
        .origin_x = sout ? -sout->box.x : 0,
        .origin_y = sout ? -sout->box.y : 0,
        .scale = 1.0,
    };
    wlr_scene_node_for_each_buffer(&server->scene->tree.node, snapshot_add_buffer, &ctx);

    for (size_t i = 0; i < sizeof(furniture) / sizeof(furniture[0]); i++)
        if (furniture[i]) wlr_scene_node_set_enabled(&furniture[i]->node, was[i]);

    return wlr_render_pass_submit(pass);
}
