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

/* A see-through background for a window that does not draw one. See bgkey.h. */

#include "bgkey.h"
#include "rotate.h"
#include "server.h"
#include "snapshot.h"
#include "view.h"

#include <pixman.h>
#include <stdlib.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>

/* One surface of the window, and the two pictures its keyed copies alternate
 * between: the scene may still be showing the last one while the next is
 * drawn. Forgotten when the scene node goes — a popup closing, a subsurface
 * the client dropped — rather than looked for and found missing. */
struct BgSurf {
    struct wl_list link;
    struct wlr_scene_buffer *sb;
    struct wl_listener destroy;
    struct wlr_buffer *dst[2];
    int flip;
};

static void bgsurf_free(struct BgSurf *s) {
    wl_list_remove(&s->link);
    wl_list_remove(&s->destroy.link);
    for (int i = 0; i < 2; i++)
        if (s->dst[i]) wlr_buffer_unlock(s->dst[i]);
    free(s);
}

static void bgsurf_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct BgSurf *s = wl_container_of(listener, s, destroy);
    bgsurf_free(s);
}

static struct BgSurf *bgsurf_for(FwmView *view, struct wlr_scene_buffer *sb) {
    struct BgSurf *s;
    wl_list_for_each(s, &view->bg_surfs, link)
        if (s->sb == sb) return s;

    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->sb = sb;
    s->destroy.notify = bgsurf_handle_destroy;
    wl_signal_add(&sb->node.events.destroy, &s->destroy);
    wl_list_insert(&view->bg_surfs, &s->link);
    return s;
}

/* Both pictures the size of the client's, reallocated when it changes. */
static bool bgsurf_fit(FwmServer *server, struct BgSurf *s, int w, int h) {
    for (int i = 0; i < 2; i++) {
        if (s->dst[i] && s->dst[i]->width == w && s->dst[i]->height == h) continue;
        if (s->dst[i]) wlr_buffer_unlock(s->dst[i]);
        s->dst[i] = NULL;
        struct wlr_buffer *b = snapshot_alloc(server, w, h);
        if (!b) return false;
        s->dst[i] = wlr_buffer_lock(b);
        wlr_buffer_drop(b);
    }
    return true;
}

static void key_buffer(struct wlr_scene_buffer *sb, int sx, int sy, void *data) {
    (void)sx; (void)sy;
    FwmView *view = data;
    FwmServer *server = view->server;

    /* The window's own surfaces only: the shadow and every effect's stand-in
     * live in the same tree, and none of them is a client's picture. */
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
    if (!ss || !sb->buffer) return;

    struct BgSurf *s = bgsurf_for(view, sb);
    if (!s) return;
    /* Still showing a copy we made: nothing new from the client since. */
    if (sb->buffer == s->dst[0] || sb->buffer == s->dst[1]) return;

    struct wlr_texture *tex = wlr_surface_get_texture(ss->surface);
    if (!tex) return;
    if (!bgsurf_fit(server, s, (int)tex->width, (int)tex->height)) return;

    struct wlr_buffer *dst = s->dst[s->flip];
    if (!key_blit(server->wlr_renderer, dst, tex,
                  view->bg_auto ? NULL : view->bg_color, (float)view->bg_alpha))
        return;   /* the client's own picture stays: unkeyed beats blank */
    s->flip ^= 1;

    wlr_scene_buffer_set_buffer(sb, dst);
    /* The client said this surface was opaque, and it no longer is: left
     * standing, the hint would have the scene skip drawing what is behind it,
     * and the background would come out black instead of see-through. */
    pixman_region32_t none;
    pixman_region32_init(&none);
    wlr_scene_buffer_set_opaque_region(sb, &none);
    pixman_region32_fini(&none);
}

void bgkey_view_frame(FwmView *view) {
    if (!view->bg_key || !view->scene_tree) return;
    if (!rotate_supported(view->server->wlr_renderer)) return;
    if (!view->bg_surfs.next) wl_list_init(&view->bg_surfs);
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, key_buffer, view);
}

void bgkey_view_free(FwmView *view) {
    if (!view->bg_surfs.next) return;
    struct BgSurf *s, *tmp;
    wl_list_for_each_safe(s, tmp, &view->bg_surfs, link) bgsurf_free(s);
}
