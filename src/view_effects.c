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


#include "view_internal.h"
#include "rubber.h"
#include "server.h"
#include "physics.h"
#include "shadow.h"
#include "rotate.h"
#include "snapshot.h"
#include "theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/log.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── impact squash & stretch ──────────────────────────────────────────── */

/* Tuned for a single soft press rather than a jelly wobble (the user asked for
 * "поспокойнее"). At omega 26 the window crossed its resting size 2-3 times
 * with a -3.7% rebound, which reads as vibration; at 14 it compresses, returns
 * once and is done, rebound about -1%. Keep omega well under the decay's reach
 * or the wobble comes back. */
#define SQUASH_DECAY  12.0   /* 1/s */
#define SQUASH_OMEGA  14.0   /* rad/s — one compression, then rest */
#define SQUASH_BULGE  0.45   /* how much the perpendicular axis bulges */
#define SQUASH_MAX_S  0.45   /* hard cap on deformation, both directions */

/* ── drag wobble ──────────────────────────────────────────────────────────
 *
 * The lattice itself is wobble.c; these are the numbers that turn it into
 * pixels. See the jelly_* fields for how it is drawn. */

/* Slack around the window in the warp target, as a fraction of its larger
 * side, and the bounds on it. A wobbling sheet overshoots its own box, and
 * whatever leaves the buffer is clipped by a straight invisible edge — so this
 * wants to be comfortably past the ~80px an ordinary drag reaches, without
 * paying for a shake nobody performs. */
#define JELLY_MARGIN_FRAC  0.22
#define JELLY_MARGIN_MIN   48
#define JELLY_MARGIN_MAX   192

/* ── composited snapshot of a window ──────────────────────────────────────
 *
 * The compositing itself lives in src/snapshot.c, which the expo strip shares;
 * what belongs to a window is deciding what must not be baked in and how often
 * the picture is worth retaking. */

bool view_snapshot_into(FwmView *view, struct wlr_buffer *buf) {
    FwmServer *server = view->server;
    if (!server->wlr_renderer || !view->scene_tree || !buf) return false;
    struct timespec _t0;
    if (server->fx_debug) clock_gettime(CLOCK_MONOTONIC, &_t0);

    /* The borders are our own nodes and must not be baked in — view_place_borders
     * redraws them around the deformed box on every tick. */
    bool border_was_enabled[4] = {false};
    for (int i = 0; i < 4; i++) {
        if (view->border[i]) {
            border_was_enabled[i] = view->border[i]->node.enabled;
            wlr_scene_node_set_enabled(&view->border[i]->node, false);
        }
    }
    /* And the shadow, for a stronger reason than the borders: what the picture
     * is taken for is a window that is about to be drawn somewhere else — spun,
     * bent, or shrinking into a ghost — and a shadow baked into it would travel
     * with the window instead of being cast by it.
     *
     * The dim comes off for the same length of time. A picture of a window is
     * of the window, not of which one had the keyboard when it was taken:
     * baking it in would leave expo's cards remembering an old focus. */
    shadow_set_enabled(view->shadow, false);
    view->shadow_drawn = false; /* put back by the next view_shadow_update */
    view_dim_suspend(view);

    int lx = 0, ly = 0;
    wlr_scene_node_coords(&view->scene_tree->node, &lx, &ly);
    bool ok = snapshot_subtree(server, buf, &view->scene_tree->node, lx, ly, 1.0);

    view_dim_restore(view);
    view_shadow_update(view);
    for (int i = 0; i < 4; i++) {
        if (view->border[i])
            wlr_scene_node_set_enabled(&view->border[i]->node, border_was_enabled[i]);
    }
    if (server->fx_debug) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        server->fx_snap_us += (t1.tv_sec - _t0.tv_sec) * 1e6
                            + (t1.tv_nsec - _t0.tv_nsec) / 1e3;
    }
    return ok;
}

/* Returns a buffer holding the window as currently composited, or NULL. The
 * caller owns the reference that wlr_allocator_create_buffer hands back. */
struct wlr_buffer *view_snapshot_content(FwmView *view) {
    if (!view->scene_tree) return NULL;
    struct wlr_buffer *buf = snapshot_alloc(view->server, view->width, view->height);
    if (!buf) return NULL;
    if (!view_snapshot_into(view, buf)) {
        wlr_buffer_drop(buf);
        return NULL;
    }
    return buf;
}

/* How often a COMPOSITED picture is retaken, in seconds.
 *
 * With effects.live on, whenever the client has drawn something new — capped,
 * because the pass is cheap but not free (~0.5ms for a large window here, once
 * the textures stopped being re-imported; it was ten times that before, which
 * is what a slow spin juddered on). With it off, the old fixed cadence and no
 * frame callbacks, so the client sleeps and the picture is a still frame. */
#define SNAP_LIVE_S    (1.0 / 30.0)
#define SNAP_REFRESH_S 0.15

/* ── live content behind an effect ────────────────────────────────────────
 *
 * An effect that shows a picture of the window has two ways to go stale, and
 * the difference in what they COST is what decides how far to chase each.
 *
 * The first is the picture. Most windows are ONE surface — a terminal, a video
 * player, anything without subsurfaces or an open menu — and for those the
 * composited snapshot is a copy of a texture the client already handed us.
 * view_live_texture returns that texture instead, so the spin and the wobble
 * draw live content every frame at no cost at all: no allocation, no
 * flatten-the-subtree pass, nothing to keep up to date. That is the whole win,
 * and it is free.
 *
 * The second is the client, and this one is NOT free. Hiding the live nodes
 * takes the window out of the scene's frame-done sweep, so a client waiting on
 * a frame callback stops drawing; view_send_frame_done keeps the callbacks
 * coming. But a client redrawing 60 times a second while it is hidden costs
 * real work on the machine, and on modest graphics that showed up exactly where
 * you would expect — as a spin that juddered under a slow hand, where there is
 * no motion to hide a dropped frame behind.
 *
 * So liveness is spent only where it is cheap. A single-surface window gets
 * both halves: live texture, live client. A window that must be composited gets
 * neither — no frame callbacks, and its picture retaken on the old
 * SNAP_REFRESH_S timer — because there the second half buys a full render pass
 * per client frame, which is precisely the cost that was juddering. Those
 * windows behave exactly as they always have.
 *
 * effects.live = 0 turns the whole thing off and puts every window on the old
 * still-frame path, for hardware where even the free half is not free. */

/* Is this window ONE surface, so that its texture is the whole picture?
 *
 * Kept apart from fetching the texture, because the two say different things
 * and confusing them costs a visible hitch: a window that has grown a
 * subsurface must move to the composited path for good, while a window that
 * merely has no texture this instant (a client between buffers) should keep
 * showing the frame it already has and try again in 16ms. Tearing the effect
 * down and rebuilding it for the latter is several allocations and a full
 * snapshot pass, in the middle of an animation. */
static bool view_is_single_surface(FwmView *view) {
    if (view->server->config.effects.live <= 0.0) return false;
    struct wlr_surface *s = view_surface(view);
    if (!s) return false;
    /* Anything drawn beside the toplevel's own buffer has to be composited, or
     * it is simply missing from the picture — the very bug the snapshot pass
     * was written for (see above). */
    if (!wl_list_empty(&s->current.subsurfaces_above) ||
        !wl_list_empty(&s->current.subsurfaces_below)) return false;
    if (view->type == FWM_VIEW_XDG && view->xdg_toplevel &&
        !wl_list_empty(&view->xdg_toplevel->base->popups)) return false;
    return true;
}

/* Is that one surface the WINDOW, all of it and nothing else?
 *
 * A client-decorated xdg toplevel paints its shadow margins and its resize
 * handles into the same surface, and calls a sub-rectangle of it the window.
 * The scene knows: wlr_scene_xdg_surface_create places the surface by that
 * geometry, so every composited picture is of the window alone. A texture
 * handed straight to an effect carries no such thing — the mesh spans it
 * corner to corner and the margins are squeezed into the window's box along
 * with the picture, by the ratio between the two. A drag on such a window
 * looked as if it were being flattened, and the flattening never let go,
 * because the margins never do.
 *
 * So the live path is for a client whose buffer already IS its window, and
 * everything else is composited. Exactly the test view_rubber_source has made
 * of the same buffer since the rubber was written; shared, now that a second
 * effect needs it. */
static bool view_buffer_is_the_window(FwmView *view) {
    struct wlr_surface *s = view_surface(view);
    if (!s) return false;
    int gw, gh;
    view_committed_size(view, &gw, &gh);
    return s->current.width == gw && s->current.height == gh;
}

static struct wlr_texture *view_live_texture(FwmView *view) {
    if (!view_is_single_surface(view)) return NULL;
    if (!view_buffer_is_the_window(view)) return NULL;
    return wlr_surface_get_texture(view_surface(view));
}

/* Is the composited picture stale enough to be worth retaking? */
static bool view_snapshot_due(FwmView *view, double since) {
    if (view->server->config.effects.live > 0.0)
        return view->content_dirty && since >= SNAP_LIVE_S;
    return since >= SNAP_REFRESH_S;
}

/* Say which path an effect took, once, when it starts (FWM_DEBUG_EFFECTS).
 * Which one a given window lands on is not obvious from the outside — plenty
 * of ordinary clients draw through a subsurface without ever showing one — and
 * that is exactly what has to be known before any judder can be argued about. */
static void view_log_effect_path(FwmView *view, const char *what, bool live) {
    if (!view->server->fx_debug) return;
    struct wlr_surface *s = view_surface(view);
    const char *why = "";
    if (!live && s) {
        if (!wl_list_empty(&s->current.subsurfaces_above) ||
            !wl_list_empty(&s->current.subsurfaces_below)) why = " (subsurfaces)";
        else if (view->type == FWM_VIEW_XDG && view->xdg_toplevel &&
                 !wl_list_empty(&view->xdg_toplevel->base->popups)) why = " (open popup)";
        else if (view->server->config.effects.live <= 0.0) why = " (effects.live = 0)";
        else if (!view_buffer_is_the_window(view)) why = " (decoration margins)";
        else why = " (no texture yet)";
    }
    wlr_log(WLR_INFO, "%s on \"%s\": %s path%s", what,
            view_title(view) ? view_title(view) : "?",
            live ? "live" : "composited", why);
}

static void frame_done_iter(struct wlr_surface *surface, int sx, int sy, void *data) {
    (void)sx; (void)sy;
    wlr_surface_send_frame_done(surface, data);
}

static void view_send_frame_done(FwmView *view) {
    struct wlr_surface *s = view_surface(view);
    if (!s) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_surface_for_each_surface(s, frame_done_iter, &now);
}

/* Show or hide the live content while keeping the borders and whichever
 * snapshot is standing in for the window visible.
 *
 * EVERY node of our own has to be listed here. This is the call that hides the
 * window behind an effect, and each effect makes it immediately after creating
 * the node that replaces it — so a node missing from this list is switched off
 * by the very call that was supposed to reveal it, and the window simply
 * vanishes for as long as the effect runs. */
void view_set_content_enabled(FwmView *view, bool enabled) {
    if (!view->scene_tree) return;
    struct wlr_scene_node *node;
    wl_list_for_each(node, &view->scene_tree->children, link) {
        bool ours = false;
        for (int i = 0; i < 4; i++) {
            if (view->border[i] && node == &view->border[i]->node) ours = true;
        }
        if (view->squash_buf && node == &view->squash_buf->node) ours = true;
        if (view->spin_buf && node == &view->spin_buf->node) ours = true;
        if (view->jelly_buf && node == &view->jelly_buf->node) ours = true;
        if (view->rub_buf && node == &view->rub_buf->node) ours = true;
        for (int i = 0; i < 3; i++)
            if (view->rub_fill[i] && node == &view->rub_fill[i]->node) ours = true;
        /* The shadow is nine nodes of ours whose state belongs to the sun and
         * not to this call: half of them are deliberately dark at any moment,
         * and switching the lot back on would paint the whole image where a
         * zero-width strip belongs. */
        if (shadow_owns_node(view->shadow, node)) ours = true;
        if (!ours) wlr_scene_node_set_enabled(node, enabled);
    }
    /* Every effect passes through here twice — once on the way in, having just
     * created the node that stands in for the window, and once on the way out,
     * having just destroyed it. Both are exactly when the shadow has to be
     * reconsidered: a window being drawn somewhere other than its own box (spun,
     * bent, dented) casts nothing, and gets its shadow back when it lands. One
     * call here rather than six at the sites, because a site that forgets is a
     * shadow left behind at an empty rectangle. */
    view_shadow_update(view);
}

/* ── rubber resize ────────────────────────────────────────────────────────
 *
 * The picture the window already has, stretched to the box the hand is asking
 * for. See the rub_* fields in view.h for why.
 *
 * The scene graph can do the stretching itself — a buffer node has a
 * destination size independent of the picture in it — so unlike the wobble and
 * the spin there is no mesh here and nothing is redrawn per frame. What this
 * costs is one node and, for a window that has to be composited, one snapshot
 * per redraw of the client. */

/* The picture to stretch: the client's own buffer when the window is a single
 * surface (free, and live), a composite of the subtree otherwise. `live` says
 * which, since the two are freed differently. */
static struct wlr_buffer *view_rubber_source(FwmView *view, int *live) {
    /* One surface and nothing else: the client's own buffer IS the picture, so
     * the stretch costs nothing and the content stays live under the hand. The
     * view already holds a lock on it (view.c keeps the last committed buffer),
     * and a new one arrives with every commit.
     *
     * Anything with a subsurface or an open menu has to be composited, and that
     * picture is taken once, here. */
    if (view_is_single_surface(view) && view->last_buffer) {
        /* ... and only when that buffer IS the window, for the reason
         * view_buffer_is_the_window gives: the margins of a decorated client
         * would be squeezed into the window's box along with the picture. */
        if (view_buffer_is_the_window(view)) {
            *live = 1;
            return view->last_buffer;
        }
    }
    *live = 0;
    return view_snapshot_content(view);
}

/* Cut the picture's last column, last row and corner pixel out into buffers of
 * their own. Best-effort: what cannot be made is simply not drawn. */
static void rubber_build_edges(FwmView *view, struct wlr_buffer *src) {
    FwmServer *server = view->server;
    int bw = view->rub_bw, bh = view->rub_bh;
    if (bw <= 0 || bh <= 0) return;

    /* The client's own buffer already has a texture — wlroots imported it when
     * the client committed — and importing a dmabuf again costs about as much
     * as the pass that follows. Borrow it, exactly as snapshot.c does. */
    struct wlr_texture *cached = view->rub_live
        ? wlr_surface_get_texture(view_surface(view)) : NULL;
    struct wlr_texture *tex = cached
        ? cached : wlr_texture_from_buffer(server->wlr_renderer, src);
    if (!tex) return;

    view->rub_edge[0] = snapshot_rect(server, tex, bw - 1, 0, 1, bh);
    view->rub_edge[1] = snapshot_rect(server, tex, 0, bh - 1, bw, 1);
    view->rub_edge[2] = snapshot_rect(server, tex, bw - 1, bh - 1, 1, 1);

    if (!cached) wlr_texture_destroy(tex);
}

/* Lay the picture, and whatever edge fill the box needs, into the box the hand
 * is asking for. Called for every size the drag passes through.
 *
 * All of the arithmetic is in rubber.c, which knows nothing about the scene
 * graph so that it can be tested without a compositor; this is the half that
 * puts the answer into nodes. */
static void rubber_layout(FwmView *view, int w, int h) {
    if (!view->rub_buf) return;

    RubberPart part[RUBBER_PARTS];
    rubber_parts(view->rub_w, view->rub_h, view->rub_bw, view->rub_bh,
                 w, h, part);
    if (!part[RUBBER_PICTURE].on) return;   /* nothing sane to draw */

    struct wlr_scene_buffer *node[RUBBER_PARTS] = {
        view->rub_buf,
        view->rub_fill[0], view->rub_fill[1], view->rub_fill[2],
    };
    for (int i = 0; i < RUBBER_PARTS; i++) {
        struct wlr_scene_buffer *b = node[i];
        if (!b) continue;
        if (!part[i].on) {
            wlr_scene_node_set_enabled(&b->node, false);
            continue;
        }
        if (i == RUBBER_PICTURE) {
            /* The crop. It never asks for less than a whole texel and never
             * scales what it takes. */
            struct wlr_fbox src = { part[i].sx, part[i].sy, part[i].sw, part[i].sh };
            wlr_scene_buffer_set_source_box(b, &src);
        } else if (view->rub_edge[i - 1]) {
            /* The strip's own buffer IS the strip, so the only thing left to
             * say is how much of it the box has room for — which is not all of
             * it when the box is WIDER than the picture and SHORTER: the right
             * fill then stands beside a cropped picture and must be cropped to
             * the same height, or the column is squeezed into it and the band
             * stops matching the edge it continues. Across the strip the box
             * covers the whole texel and the sampler has nowhere else to go,
             * which is the entire point of cutting these out (see view.h). */
            struct wlr_buffer *eb = view->rub_edge[i - 1];
            struct wlr_fbox src = {
                0.0, 0.0,
                i == RUBBER_BOTTOM ? part[i].sw : (double)eb->width,
                i == RUBBER_RIGHT  ? part[i].sh : (double)eb->height,
            };
            wlr_scene_buffer_set_source_box(b, &src);
        }
        wlr_scene_buffer_set_dest_size(b, part[i].w, part[i].h);
        wlr_scene_node_set_position(&b->node, part[i].x, part[i].y);
        wlr_scene_node_set_enabled(&b->node, true);
    }
}

bool view_rubber_begin(FwmView *view) {
    /* A fresh grab cancels whatever the last one was still settling, glide and
     * all: the size in the hand is the only one that means anything now. */
    view->rs_t = 0.0;
    view->rs_glide = view->rs_glide_len = 0.0;
    view->rs_pin_r = view->rs_pin_b = 0;
    view->rub_settling = 0;
    if (view->rub_buf) return true;
    if (!view->scene_tree) return false;
    if (view->server->config.effects.rubber <= 0.0) return false;
    /* One picture of a window at a time. The others are all short animations;
     * a resize can wait for them rather than fight over the same node. */
    if (view->spin_buf || view->jelly || view->squash_buf) return false;

    /* The size the picture actually holds — what the CLIENT last committed,
     * not what we may already have asked it for. Asked of the committed size
     * directly: view_border_box answers with the asked-for box whenever a hand
     * is on the window, which is exactly when this runs. */
    int w, h;
    view_committed_size(view, &w, &h);
    if (w <= 0 || h <= 0) { w = view->width; h = view->height; }
    if (w <= 0 || h <= 0) return false;

    int live = 0;
    struct wlr_buffer *src = view_rubber_source(view, &live);
    if (!src) return false;

    /* The picture's size in its own pixels, read while we still plainly hold
     * the buffer. It is not w x h on a scaled output, and it is what every
     * source box below is measured in. */
    view->rub_bw = src->width;
    view->rub_bh = src->height;

    view->rub_buf = wlr_scene_buffer_create(view->scene_tree, src);
    if (!view->rub_buf) {
        if (!live) wlr_buffer_drop(src);
        return false;
    }
    /* The scene node has taken its own reference; ours is either the view's
     * standing lock on the client buffer or one we keep until the picture is
     * replaced. */
    if (!live) {
        view->rub_lock = wlr_buffer_lock(src);
        wlr_buffer_drop(src);
    }
    view->rub_live = live;
    /* What the picture SPANS, in layout pixels, which the two ways of taking
     * it answer differently. The client's own buffer is the window and nothing
     * else — view_rubber_source only takes that path once it has checked so —
     * and it spans the box the client committed. A composited one is drawn
     * into a buffer of the view's size at scale 1, so it spans that instead.
     * Getting this wrong does not show as a wrong size: everything is drawn
     * 1:1 from here on, and it would show as the fill sampling a column of the
     * picture that is not its edge. */
    view->rub_w = live ? w : view->width;
    view->rub_h = live ? h : view->height;
    view->rub_frame_t = 0.0;
    view_log_effect_path(view, "resize rubber", live);

    /* Under the borders, like the squash: the frame still outlines the window,
     * and here it outlines the box being ASKED for (view_border_box).
     *
     * No input region is set on any of these: the pointer is held by the
     * resize for as long as they exist, so there is nothing for a picture to
     * accept. */
    wlr_scene_node_lower_to_bottom(&view->rub_buf->node);
    view->rub_buf->node.data = view;
    /* Nearest, on all four nodes, and it is not an aesthetic choice.
     *
     * The picture is drawn 1:1, where the two filters agree. The fill is a
     * SINGLE row or column blown out to cover the gap, and a source box does
     * not clamp the sampler: bilinear reaches past it into the neighbouring
     * texels, so the band came out as a gradient running from the colour
     * beside the edge to the edge itself — a smear, which is the one thing
     * this effect is not allowed to produce. Nearest gives the edge exactly,
     * flat across the whole band. */
    wlr_scene_buffer_set_filter_mode(view->rub_buf, WLR_SCALE_FILTER_NEAREST);

    /* The edge fill, made now and left switched off: a drag that only ever
     * shrinks the window never shows it, and one that grows must not be
     * allocating while the hand is moving. See the rub_edge comment in view.h
     * for why each strip gets a buffer of its own instead of a source box into
     * the picture. A failure here is not fatal — the window simply grows with
     * the desktop showing through the strip, as it did before any of this. */
    rubber_build_edges(view, src);
    for (int i = 0; i < 3; i++) {
        if (!view->rub_edge[i]) continue;
        view->rub_fill[i] = wlr_scene_buffer_create(view->scene_tree, view->rub_edge[i]);
        if (!view->rub_fill[i]) continue;
        wlr_scene_node_lower_to_bottom(&view->rub_fill[i]->node);
        view->rub_fill[i]->node.data = view;
        wlr_scene_buffer_set_filter_mode(view->rub_fill[i], WLR_SCALE_FILTER_NEAREST);
        wlr_scene_node_set_enabled(&view->rub_fill[i]->node, false);
    }

    rubber_layout(view, view->width, view->height);
    view_set_content_enabled(view, false);
    view_update_border_geometry(view);
    return true;
}

void view_rubber_to(FwmView *view, int w, int h) {
    if (!view->rub_buf || w <= 0 || h <= 0) return;
    rubber_layout(view, w, h);
    view_update_border_geometry(view);
}

/* Take the size the client actually settled on, with the edges that were not
 * being dragged held where the grab left them. */
static void view_resize_adopt(FwmView *view) {
    int cw, ch;
    view_committed_size(view, &cw, &ch);
    if (cw <= 0 || ch <= 0) return;

    /* Which column the window is in, asked BEFORE anything moves. Holding a
     * far edge means measuring the near one back from it, and the client's
     * answer is not bound by the clamps the drag applied to what we asked for
     * — so a window that answers bigger than the room left walks its near edge
     * out of the desktop, and the desktop is decided from the centre. */
    FwmServer *server = view->server;
    int sw = server->screen_width;
    int col = sw > 0 ? server_desktop_at_x(server, view->x + view->width / 2.0) : 0;

    if (view->rs_pin_r) view->x = view->rs_x1 - cw;
    if (view->rs_pin_b) view->y = view->rs_y1 - ch;
    view->width = cw;
    view->height = ch;

    if (sw > 0) {
        /* Keep the centre in the column it was already in. Not the whole
         * window: one that overhangs its screen is an ordinary thing here, and
         * it is the centre alone that names the desktop. */
        double cx = view->x + cw / 2.0;
        double lo = (double)col * sw, hi = lo + sw - 1;
        if (cx < lo) view->x += (int)lround(lo - cx);
        else if (cx > hi) view->x -= (int)lround(cx - hi);
    }
    physics_sync_body(&server->physics, view->id, view->x, view->y,
                      cw, ch, sw);
    if (view->scene_tree) server_place_view(server, view, view->x, view->y);
}

/* Has the client answered the last size it was asked for?
 *
 * An xdg client says so itself: it acks the configure it is drawing for, and
 * the serial it acked comes back on the surface's committed state. Anything
 * older than the one we sent last is a frame that was already in flight for a
 * size the hand has left behind — including a commit that redraws nothing but
 * a blinking cursor, which is a frame like any other and used to end the wait.
 *
 * X11 has no such handshake, so the question is asked of the size instead: the
 * window has answered when it IS the size we asked for, or when it has at
 * least stopped being the size it was at the release. A client whose units do
 * not divide the request — xterm, in whole character cells — never satisfies
 * the first and always satisfies the second. */
static bool view_resize_answered(FwmView *view) {
    int cw, ch;
    view_committed_size(view, &cw, &ch);
    /* Already the size we asked for. Asked first and without waiting for a
     * frame, because a client that kept up with the drag has nothing left to
     * draw: there may be no commit coming at all, and the point of consulting
     * the serial is to catch frames that arrive EARLY, not to make a window
     * that is already right sit out a third of a second. */
    if (cw == view->rs_w && ch == view->rs_h) return true;

    /* Otherwise the client is redrawing, and the answer arrives as a frame. */
    if (!view->content_dirty) return false;

    if (view->type == FWM_VIEW_XDG) {
        if (!view->xdg_toplevel || !view->rs_serial) return true;
        uint32_t acked = view->xdg_toplevel->base->current.configure_serial;
        return (int32_t)(acked - view->rs_serial) >= 0;
    }
    /* An X11 window has answered when it has at least stopped being the size
     * it was at the release. */
    return cw != view->rs_cw || ch != view->rs_ch;
}

void view_resize_settle(FwmView *view, int pin_r, int pin_b, int x1, int y1) {
    view->rs_pin_r = pin_r;
    view->rs_pin_b = pin_b;
    view->rs_x1 = x1;
    view->rs_y1 = y1;
    /* What the answer will look like when it comes: the configure the client
     * must ack, and — for an X11 window, which acks nothing — the size that
     * configure asked for. */
    view->rs_serial = view->cfg_serial;
    view->rs_w = view->width;
    view->rs_h = view->height;
    /* Long enough for a client to answer one configure, short enough that a
     * client which never answers is not left frozen. */
    view->rs_t = 0.35;
    view->rub_settling = view->rub_buf != NULL;
    /* Frames from here on are candidates for the answer; what came before the
     * release was drawn for a size the hand has already left. */
    view->content_dirty = 0;
    /* The size at the release, so the X11 test above can tell a window that
     * has moved from one that has not. */
    view_committed_size(view, &view->rs_cw, &view->rs_ch);
}

/* THE LAST FEW PIXELS OF A RESIZE.
 *
 * A client is not obliged to take the size it is offered, and plenty do not: a
 * terminal answers in whole character cells, so the last cell of a drag is up
 * to twenty pixels the hand asked for and did not get. The rubber draws what
 * the hand asked for — that is what makes the drag itself smooth — and those
 * pixels therefore all arrived at once, in the frame the picture came down: one
 * jolt at the end of every resize, the window dropping onto a grid it had never
 * shown while it was being dragged.
 *
 * They arrive over a tenth of a second instead. The picture is stretched either
 * way; taking it from the size in the hand to the size the client took is the
 * same stretch, moving. Short enough to read as the window settling and not as
 * an animation of its own. */
#define RS_GLIDE_S 0.10

/* Where the glide is now. Eased out — it is the tail of a movement the hand was
 * already making, so it slows into its place rather than starting from rest. */
static void rubber_glide_apply(FwmView *view) {
    double u = view->rs_glide_len > 0.0
             ? 1.0 - view->rs_glide / view->rs_glide_len : 1.0;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    double e = 1.0 - (1.0 - u) * (1.0 - u);

    int w = (int)lround(view->rs_gw + (view->rs_tw - view->rs_gw) * e);
    int h = (int)lround(view->rs_gh + (view->rs_th - view->rs_gh) * e);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* The edges nobody was holding still do not move — the same pin the wait
     * before this one kept. */
    if (view->rs_pin_r) view->x = view->rs_x1 - w;
    if (view->rs_pin_b) view->y = view->rs_y1 - h;
    view->width = w;
    view->height = h;
    view_rubber_to(view, w, h);
    physics_sync_body(&view->server->physics, view->id, view->x, view->y,
                      w, h, view->server->screen_width);
    if (view->scene_tree) server_place_view(view->server, view, view->x, view->y);
}

/* Start one, if there is anything to ride out. False when the client took
 * exactly what it was given, which is most windows and most drags: then there
 * is nothing between the picture and the window and the rubber can simply go. */
static bool rubber_glide_begin(FwmView *view) {
    if (!view->rub_buf) return false;
    int cw, ch;
    view_committed_size(view, &cw, &ch);
    if (cw <= 0 || ch <= 0) return false;
    if (cw == view->width && ch == view->height) return false;

    view->rs_gw = view->width;
    view->rs_gh = view->height;
    view->rs_tw = cw;
    view->rs_th = ch;
    view->rs_glide = view->rs_glide_len = RS_GLIDE_S;
    return true;
}

/* Whatever it was in the middle of, it is over: the picture comes down and the
 * window becomes the size the client actually holds. */
static void rubber_settle_finish(FwmView *view) {
    view->rs_glide = view->rs_glide_len = 0.0;
    view->rub_settling = 0;
    view_rubber_end(view);       /* no-op when there was no rubber */
    view_resize_adopt(view);
    view->rs_pin_r = view->rs_pin_b = 0;
    view->rs_serial = 0;
}

void view_rubber_tick(FwmView *view, double dt) {
    /* The moment after the release: hold the far edges, and keep the stretched
     * picture up until the client has drawn the size it was last asked for.
     * Without this the window snapped to whatever it had managed to commit
     * mid-drag — a size the hand had already left — and then walked to the
     * real one over the next few frames. */
    if (view->rs_t > 0.0) {
        /* Unless another gesture has taken the window in the meantime — let go
         * of a resize and start dragging it straight away, and where the window
         * goes is the new hand's business. Holding an edge from the last one
         * would teleport it mid-drag. (A new RESIZE clears this in
         * view_rubber_begin, so what lands here is a move or a twist.) */
        FwmInteractiveState *in = &view->server->interactive;
        if (in->action != FWM_ACTION_NONE && in->view == view) {
            view->rs_t = 0.0;
            view->rs_glide = view->rs_glide_len = 0.0;
            view->rub_settling = 0;
            view->rs_pin_r = view->rs_pin_b = 0;
            view->rs_serial = 0;
            view_rubber_end(view);
            return;
        }
        view->rs_t -= dt;
        if (view->rs_t <= 0.0 || view_resize_answered(view)) {
            view->rs_t = 0.0;
            /* The answer is in. If it is not the size the hand let go at, the
             * difference is ridden out rather than jumped; the glide below
             * finishes the settle when it arrives. */
            if (!rubber_glide_begin(view)) rubber_settle_finish(view);
            return;
        }
    }

    /* The tail of the settle. */
    if (view->rs_glide > 0.0) {
        FwmInteractiveState *in = &view->server->interactive;
        if (in->action != FWM_ACTION_NONE && in->view == view) {
            /* Taken by another gesture mid-glide, exactly as above: where the
             * window goes is the new hand's business. */
            rubber_settle_finish(view);
            return;
        }
        /* The window is still behind the picture and still owes the scene its
         * frame callbacks; a tenth of a second of silence is enough for a
         * client that animates to stutter as it comes back. */
        view_send_frame_done(view);
        /* And it may answer again while this runs — a client that took two
         * commits to arrive at its size. Aim at wherever it is now rather than
         * finishing at a size it has already left. */
        {
            int cw, ch;
            view_committed_size(view, &cw, &ch);
            if (cw > 0 && ch > 0) { view->rs_tw = cw; view->rs_th = ch; }
        }
        view->rs_glide -= dt;
        if (view->rs_glide <= 0.0) { rubber_settle_finish(view); return; }
        rubber_glide_apply(view);
        return;
    }

    if (!view->rub_buf) return;

    /* Hiding the live nodes took the window out of the scene's frame-done
     * sweep, and a client that is waiting on a frame callback stops drawing —
     * which during a resize can mean it never answers the size we asked for,
     * and the drag ends on the picture it started with.
     *
     * At the frame's pace, not this function's: it is called once per output
     * frame, so on two monitors it runs twice for the same instant, and a
     * client handed twice as many callbacks as there are frames redraws twice
     * as often for nothing. */
    view->rub_frame_t += dt;
    if (view->rub_frame_t >= 1.0 / 120.0) {
        view->rub_frame_t = 0.0;
        view_send_frame_done(view);
    }

    /* The picture itself is deliberately NOT refreshed while the hand is on the
     * window. It was, at first — the client's newest frame stretched into the
     * hand's box — and that is precisely what made the window shudder: every
     * answer the client sends is a different size, so the same picture was
     * being squeezed a few percent one way and then the other, several times a
     * second, on top of the stretch the drag was already applying. One frozen
     * frame stretched smoothly is the calm version, and the live window comes
     * back at the release. */
    if (!view->rub_settling) view->content_dirty = 0;
}

void view_rubber_end(FwmView *view) {
    if (!view->rub_buf) return;
    for (int i = 0; i < 3; i++) {
        if (view->rub_fill[i]) {
            wlr_scene_node_destroy(&view->rub_fill[i]->node);
            view->rub_fill[i] = NULL;
        }
        if (view->rub_edge[i]) {
            wlr_buffer_drop(view->rub_edge[i]);
            view->rub_edge[i] = NULL;
        }
    }
    wlr_scene_node_destroy(&view->rub_buf->node);
    view->rub_buf = NULL;
    if (view->rub_lock) {
        wlr_buffer_unlock(view->rub_lock);
        view->rub_lock = NULL;
    }
    view->rub_live = 0;
    view->rub_w = view->rub_h = 0;
    view->rub_bw = view->rub_bh = 0;
    view_set_content_enabled(view, true);
    view->server->pointer_resync_due = 1;
    view_update_border_geometry(view);   /* back to the client's own box */
}

void view_stop_squash(FwmView *view) {
    if (!view->squash_buf) return;
    wlr_scene_node_destroy(&view->squash_buf->node);
    view->squash_buf = NULL;
    if (view->squash_lock) {
        wlr_buffer_unlock(view->squash_lock);
        view->squash_lock = NULL;
    }
    view->squash_t = 0.0;
    view->squash_amount = 0.0;
    view_set_content_enabled(view, true);
    view->server->pointer_resync_due = 1;
    view_update_border_geometry(view); /* back to the real box */
}

/* Put the deformable snapshot in place and hide the live content behind it.
 * False if there is nothing to snapshot. */
static bool view_take_deform_snapshot(FwmView *view) {
    /* A composite of the whole subtree, not the toplevel's raw buffer: see
     * view_snapshot_content. We hold the reference the allocator gave us until
     * the scene node has taken its own lock. */
    struct wlr_buffer *snap = view_snapshot_content(view);
    if (!snap) return false;

    view->squash_buf = wlr_scene_buffer_create(view->scene_tree, snap);
    if (!view->squash_buf) {
        wlr_buffer_drop(snap);
        return false;
    }
    view->squash_lock = wlr_buffer_lock(snap);
    wlr_buffer_drop(snap);
    /* Under the borders, so the frame still reads as the window's outline. */
    wlr_scene_node_lower_to_bottom(&view->squash_buf->node);
    view_set_content_enabled(view, false);
    return true;
}

void view_start_squash(FwmView *view, double nx, double ny, double amount) {
    if (!view->scene_tree || !view->last_buffer) return;
    if (amount <= 0.001) return;
    /* A spinning window already has the picture, and a deformation along a
     * screen-axis normal would be visibly wrong on a tilted one. The rubber
     * holds it for the same reason: a window being resized is drawn at a size
     * the client has not reached, and denting THAT is denting a guess. */
    if (view->spin_buf || view->rub_buf) return;

    /* A window still in your hand keeps its wobble: the drag owns the picture,
     * and the impact is not lost anyway — a collision moves the window, and
     * moving the window is exactly what the sheet responds to.
     *
     * One that has been let go is a different matter. Its wobble is ringing
     * out, it is not being driven by anything any more, and landing is the
     * loudest thing that will happen to it — so the landing takes the picture
     * over. Without this the wobble swallowed every impact for as long as it
     * lasted: windows arrived at the floor soft and squashed nothing. */
    if (view->jelly) {
        if (!view->jelly_settling) return;
        view_jelly_stop(view);
    }

    if (view->squash_buf) {
        /* Already deforming: retarget rather than stacking a second snapshot,
         * and keep whichever impact was stronger. */
        if (amount > view->squash_amount) {
            view->squash_amount = amount;
            view->squash_nx = nx;
            view->squash_ny = ny;
            view->squash_t = 0.0;
        }
        return;
    }

    if (!view_take_deform_snapshot(view)) return;

    view->squash_t = 0.0;
    view->squash_amount = amount;
    view->squash_nx = nx;
    view->squash_ny = ny;
    wlr_log(WLR_DEBUG, "squash: view %u amount %.3f normal (%.2f,%.2f)",
            view->id, amount, nx, ny);
}

void view_squash_tick(FwmView *view, double dt) {
    if (!view->squash_buf) return;
    view->squash_t += dt;

    /* Damped oscillation: a hard squash that springs back through a smaller
     * overshoot, rather than a single linear dent.
     * The end test MUST look at the envelope, not at `a`: the cosine crosses
     * zero on every half-wobble, so testing `a` ended the animation ~60ms in,
     * at the exact instant of zero deformation — the spring-back never ran. */
    double env = view->squash_amount * exp(-SQUASH_DECAY * view->squash_t);
    if (env < 0.004) { view_stop_squash(view); return; }
    double a = env * cos(SQUASH_OMEGA * view->squash_t);
    if (a >  SQUASH_MAX_S) a =  SQUASH_MAX_S;
    if (a < -SQUASH_MAX_S) a = -SQUASH_MAX_S;

    int w, h;
    view_border_box(view, &w, &h);
    if (w <= 0 || h <= 0) { view_stop_squash(view); return; }

    /* Compress along the contact normal, bulge across it. */
    double ax = fabs(view->squash_nx), ay = fabs(view->squash_ny);
    double sx = 1.0 - a * ax + a * SQUASH_BULGE * ay;
    double sy = 1.0 - a * ay + a * SQUASH_BULGE * ax;

    int dw = (int)lround(w * sx), dh = (int)lround(h * sy);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    /* Keep the edge that took the hit planted: a window landing on the floor
     * must compress into the floor, not hover above it. */
    int ox = view->squash_nx > 0 ? w - dw : 0;
    int oy = view->squash_ny > 0 ? h - dh : 0;

    wlr_scene_buffer_set_dest_size(view->squash_buf, dw, dh);
    wlr_scene_node_set_position(&view->squash_buf->node, ox, oy);
    view_place_borders(view, ox, oy, dw, dh);
}

/* ── drag wobble ──────────────────────────────────────────────────────── */

/* The warp target is the window plus slack on every side; the mesh is drawn
 * into it and the node sits at -margin so the window's own box still lines up
 * where it always did. */
static int jelly_margin_for(int w, int h) {
    int big = w > h ? w : h;
    int m = (int)lround(JELLY_MARGIN_FRAC * big);
    if (m < JELLY_MARGIN_MIN) m = JELLY_MARGIN_MIN;
    if (m > JELLY_MARGIN_MAX) m = JELLY_MARGIN_MAX;
    return m;
}

/* The warped picture covers the window plus its margin, so left to itself it
 * would swallow clicks in a transparent frame all around — including a good
 * part of whatever is next to it. The lattice's own outline is the honest
 * answer but a point-in-polygon test against 64 quads per motion event is not;
 * the window's box, offset by how far the sheet has actually moved, is within
 * a few px of it and is what the cursor is aiming at anyway. */
static bool jelly_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    FwmView *view = buffer->node.data;
    if (!view || !view->jelly_buf) return true;
    double lx = *sx - view->jelly_margin, ly = *sy - view->jelly_margin;
    double dx = view->jelly_wob.px[0], dy = view->jelly_wob.py[0];   /* top-left point */
    return lx >= dx && ly >= dy &&
           lx <= dx + view->jelly_w && ly <= dy + view->jelly_h;
}

/* Tear the machinery down without touching what it hid — view_jelly_stop does
 * that part, and a mid-wobble resize deliberately does not. */
static void view_jelly_free(FwmView *view) {
    if (view->jelly_buf) {
        wlr_scene_node_destroy(&view->jelly_buf->node);
        view->jelly_buf = NULL;
    }
    if (view->jelly_tex) {
        wlr_texture_destroy(view->jelly_tex);
        view->jelly_tex = NULL;
    }
    if (view->jelly_src) {
        wlr_buffer_unlock(view->jelly_src);
        view->jelly_src = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (view->jelly_dst[i]) {
            wlr_buffer_unlock(view->jelly_dst[i]);
            view->jelly_dst[i] = NULL;
        }
    }
    view->jelly_w = view->jelly_h = view->jelly_margin = 0;
    view->jelly_flip = 0;
    view->jelly_snap_t = 0.0;
    view->jelly_live = 0;
}

void view_jelly_stop(FwmView *view) {
    if (!view->jelly) return;
    int border = view->jelly_border;
    view->jelly = 0;
    view->jelly_settling = 0;
    /* The drop is a shape this sheet was drawn in; there is no sheet now. */
    view->drop_round = view->drop_want = 0.0;
    view->drop_filling = 0;
    view_jelly_free(view);
    view_set_content_enabled(view, true);
    view->server->pointer_resync_due = 1;
    if (border) view_set_border_enabled(view, 1);
    /* The window is itself again and casts again: view->jelly was what kept
     * the shadow out, and the geometry pass puts it back. */
    view_update_border_geometry(view);
}

/* Build (or rebuild) the snapshot, the two warp targets and the scene node.
 * Called when the wobble starts and again whenever the window changes size. */
static bool view_jelly_setup(FwmView *view) {
    FwmServer *server = view->server;
    if (!view->scene_tree || !server->wlr_renderer) return false;

    int w = view->width, h = view->height;
    if (w <= 0 || h <= 0) return false;

    bool restarting = view->jelly_buf != NULL;
    int border = restarting ? view->jelly_border
                            : (view->border[0] && view->border[0]->node.enabled);
    view_jelly_free(view);

    /* The live content has to be visible to be photographed, so the snapshot
     * comes first and the window is hidden only once it succeeded — a failed
     * setup must leave the window on screen, not blank. */
    if (restarting) view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);

    int margin = jelly_margin_for(w, h);

    /* One surface and nothing else: bend the client's own texture, so the
     * window stays live while it is dragged. The wobble only ever runs on the
     * GLES2 path (view_jelly_begin refuses otherwise), so unlike the spin there
     * is no buffer-only fallback to keep a snapshot around for. */
    view->jelly_live = view_live_texture(view) != NULL;
    view_log_effect_path(view, "wobble", view->jelly_live);

    if (!view->jelly_live) {
        struct wlr_buffer *src = view_snapshot_content(view);
        if (!src) goto fail;
        view->jelly_src = wlr_buffer_lock(src);
        wlr_buffer_drop(src);

        view->jelly_tex = wlr_texture_from_buffer(server->wlr_renderer, view->jelly_src);
        if (!view->jelly_tex) goto fail;
    }

    /* Two targets used alternately: the scene may still be reading last
     * frame's while this one is drawn, and overwriting it in place would tear. */
    for (int i = 0; i < 2; i++) {
        struct wlr_buffer *d = snapshot_alloc(server, w + 2 * margin, h + 2 * margin);
        if (!d) goto fail;
        view->jelly_dst[i] = wlr_buffer_lock(d);
        wlr_buffer_drop(d);
    }

    view->jelly_buf = wlr_scene_buffer_create(view->scene_tree, NULL);
    if (!view->jelly_buf) goto fail;
    wlr_scene_node_lower_to_bottom(&view->jelly_buf->node);
    /* view_at() walks up from the node's PARENT to find the view, so this is
     * free for the hit test to use. */
    view->jelly_buf->node.data = view;
    view->jelly_buf->point_accepts_input = jelly_accepts_input;
    wlr_scene_node_set_position(&view->jelly_buf->node, -margin, -margin);

    view->jelly_w = w;
    view->jelly_h = h;
    view->jelly_margin = margin;
    view->jelly_border = border;

    /* The borders are scene rectangles: they cannot bend any more than they
     * can tilt, so they go away for the duration rather than framing a window
     * that is no longer where they say it is. */
    view_set_content_enabled(view, false);
    view_set_border_enabled(view, 0);
    return true;

fail:
    view_jelly_free(view);
    view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);
    return false;
}

static bool view_jelly_blit(FwmView *view, const float *pts);

/* Draw the lattice as it stands. Returns false if the mesh could not be drawn
 * at all, which is the caller's cue to give the live window back. */
static bool view_jelly_draw(FwmView *view, double strength) {
    float pts[WOBBLE_POINTS * 2];

    /* A landing owns the lattice outright: the drop is not wobbling its way
     * into the slot, it is spreading into it, and the two shapes would only
     * fight. The springs are left where they are and simply not consulted. */
    if (view->drop_filling) {
        droplet_fill_points(&view->drop_fill, pts, WOBBLE_GRID,
                            view->jelly_margin, view->jelly_margin);
        return view_jelly_blit(view, pts);
    }

    wobble_points(&view->jelly_wob, pts, view->jelly_margin, view->jelly_margin);

    if (strength != 1.0) {
        /* Move every point toward (or past) where it would rest. Scaling the
         * SHAPE rather than the springs is what keeps the wobble's timing the
         * same at every setting — only its size changes. */
        for (int j = 0; j < WOBBLE_GRID; j++) {
            for (int i = 0; i < WOBBLE_GRID; i++) {
                int k = j * WOBBLE_GRID + i;
                float hx = (float)(view->jelly_margin + (double)view->jelly_w * i / (WOBBLE_GRID - 1));
                float hy = (float)(view->jelly_margin + (double)view->jelly_h * j / (WOBBLE_GRID - 1));
                pts[k * 2 + 0] = hx + (pts[k * 2 + 0] - hx) * (float)strength;
                pts[k * 2 + 1] = hy + (pts[k * 2 + 1] - hy) * (float)strength;
            }
        }
    }

    /* Round it off last, so what gets bent into a drop is the sheet with all of
     * its wobble already in it rather than the other way round. */
    if (view->drop_round > 0.0) {
        droplet_round(pts, WOBBLE_GRID, view->jelly_w, view->jelly_h, view->drop_round);
    }

    return view_jelly_blit(view, pts);
}

/* Draw a finished lattice, whichever of the two shapes above produced it.
 * Returns false only when the mesh could not be drawn at all. */
static bool view_jelly_blit(FwmView *view, const float *pts) {
    /* Live path: whatever the client has on screen this instant. It may be a
     * different texture than last frame — that is the whole point — and NULL
     * only if the window stopped being a single surface, which the tick
     * notices and rebuilds for. */
    struct wlr_texture *tex = view->jelly_live ? view_live_texture(view) : view->jelly_tex;
    /* No texture this instant (a client between buffers). Keep the frame that
     * is already on screen: the next tick is 16ms away and tearing the whole
     * wobble down mid-drag would be a far bigger jump than one repeated frame. */
    if (!tex) return true;

    struct wlr_buffer *dst = view->jelly_dst[view->jelly_flip];
    if (!warp_blit(view->server->wlr_renderer, dst, tex, WOBBLE_GRID, pts)) {
        return false;
    }
    view->jelly_flip ^= 1;
    wlr_scene_buffer_set_buffer(view->jelly_buf, dst);
    /* Stated rather than left to default off the buffer, exactly as the spin
     * states it: the node was created empty, so it starts with no size of its
     * own at all. */
    wlr_scene_buffer_set_dest_size(view->jelly_buf, dst->width, dst->height);
    return true;
}

void view_jelly_begin(FwmView *view, double strength, double grab_lx, double grab_ly) {
    if (!view->scene_tree || !view->last_buffer) return;
    if (strength <= 0.0) return;
    /* Rotation wins, exactly as it does over the impact squash: it owns the
     * picture, and a sheet bending along the screen axes says nothing useful
     * about one that is tilted. A window in the rubber is not being dragged at
     * all — one hand, one gesture — but a stray one must not stack two
     * pictures of the same window. */
    if (view->spin_buf || view->rub_buf) return;
    /* No mesh without the GLES2 path. Unlike the spin there is no degraded
     * version worth showing — a window that does not wobble is just a window. */
    if (!rotate_supported(view->server->wlr_renderer)) return;

    if (view->jelly) {
        view->jelly_settling = 0;
        wobble_grab(&view->jelly_wob, grab_lx, grab_ly);
        return;
    }
    /* A dent from a landing a moment ago is replaced by the wobble rather than
     * fought with — same picture, and the drag is the newer intent. */
    if (view->squash_buf) view_stop_squash(view);
    if (!view_jelly_setup(view)) return;

    view->jelly = 1;
    view->jelly_settling = 0;
    view->jelly_px = view->x;
    view->jelly_py = view->y;
    /* Put the shadow out, now that there is a bent picture where the window
     * used to be. The spin and the squash get this for free — they take a
     * snapshot, and the snapshot hides the shadow for the length of the pass
     * (see view_snapshot_into) — but the wobble's normal path bends the
     * client's own texture and photographs nothing, so nothing was asking. A
     * window shaken with its shadow left behind stands there wobbling over a
     * rectangle that does not move with it. */
    view_shadow_update(view);
    wobble_reset(&view->jelly_wob, view->jelly_w, view->jelly_h);
    wobble_grab(&view->jelly_wob, grab_lx, grab_ly);

    /* The setup above hid the live window behind a scene node with no picture
     * in it yet, and the first tick is a frame away. Draw the undeformed sheet
     * now — it is a plain copy of the window — or the press would blink it out
     * of existence for that frame. */
    if (!view_jelly_draw(view, strength)) view_jelly_stop(view);
}

void view_jelly_carry(FwmView *view, double dx, double dy) {
    if (!view->jelly) return;
    view->jelly_px += dx;
    view->jelly_py += dy;
}

void view_jelly_release(FwmView *view) {
    if (!view->jelly) return;
    view->jelly_settling = 1;
    wobble_release(&view->jelly_wob);
}

/* ── the drop ─────────────────────────────────────────────────────────── */

/* How fast a window rounds off into a drop, and back, 1/s. Fast enough that
 * the shape is there by the time the hand has moved anywhere, slow enough that
 * it is a window turning into a drop rather than one being swapped for one. */
#define DROP_ROUND_RATE 14.0

/* How long the landing takes. Rather longer than the tile glide it replaces:
 * the glide only had to move a window, this has to read as a liquid finding
 * the edges of something. */
#define DROP_FILL_SECONDS 0.42

void view_droplet_begin(FwmView *view, double grab_lx, double grab_ly) {
    double amount = view->server->config.effects.droplet;
    if (amount <= 0.0) return;

    /* The mesh may already be up — a drag arms the wobble before it decides the
     * window is coming out of a tree — and if it is not, this is what arms it.
     * Passing the drop's own strength rather than the wobble's is what lets the
     * drop exist with [effects] jelly off: begin refuses at 0, and the sheet
     * that comes back is then simply a still one. */
    if (!view->jelly) {
        double jelly = view->server->config.effects.jelly;
        view_jelly_begin(view, jelly > 0.0 ? jelly : amount, grab_lx, grab_ly);
        if (!view->jelly) return;
    }

    view->drop_want = amount;
    view->drop_filling = 0;
}

void view_droplet_clear(FwmView *view) {
    view->drop_want = 0.0;
    view->drop_filling = 0;
}

bool view_is_droplet(FwmView *view) {
    return view->drop_want > 0.0 || view->drop_round > 0.0 || view->drop_filling;
}

void view_droplet_fill(FwmView *view, double lx, double ly,
                       double drop_w, double drop_h) {
    if (view->server->config.effects.droplet <= 0.0) return;
    /* Nothing to spread: the drag never got as far as putting a mesh up (a
     * renderer with no GLES2 path, a window that started spinning). The window
     * is already in its slot, which is the part that matters. */
    if (!view->jelly) return;
    if (view->width <= 0 || view->height <= 0) return;

    droplet_fill_begin(&view->drop_fill, view->width, view->height,
                       drop_w, drop_h, lx, ly, DROP_FILL_SECONDS);
    view->drop_filling = 1;
    /* The hand is off, but this must not ring out like an ordinary release —
     * the fill decides when the live window comes back, not the springs. */
    view->jelly_settling = 0;
    wobble_release(&view->jelly_wob);
}

/* Retake the frozen picture. The live nodes come back for the length of the
 * pass and the warped one goes away, or each refresh would bake the last bent
 * frame into the next. Nothing is presented in between, so the window never
 * flashes back to its undeformed self. */
static void view_jelly_resnap(FwmView *view) {
    view_set_content_enabled(view, true);
    wlr_scene_node_set_enabled(&view->jelly_buf->node, false);
    view_snapshot_into(view, view->jelly_src);
    wlr_scene_node_set_enabled(&view->jelly_buf->node, true);
    view_set_content_enabled(view, false);
}

void view_jelly_tick(FwmView *view, double strength, double dt) {
    if (!view->jelly) return;

    /* A drop keeps the sheet up on its own account, so the wobble being off is
     * no longer reason enough to tear it down: the picture is still bent, just
     * not by springs. Turning the DROP off in a reload has to reach a window
     * mid-landing the same way, which is what re-reading it here does. */
    if (view->server->config.effects.droplet <= 0.0) {
        view->drop_want = 0.0;
        view->drop_filling = 0;
    }
    bool dropping = view->drop_want > 0.0 || view->drop_round > 0.0 || view->drop_filling;

    if (view->spin_buf || (strength <= 0.0 && !dropping)) { view_jelly_stop(view); return; }
    if (dt <= 0.0) return;
    if (strength < 0.0) strength = 0.0;

    if (!view->jelly_buf || view->jelly_w != view->width || view->jelly_h != view->height) {
        if (!view_jelly_setup(view)) { view_jelly_stop(view); return; }
        wobble_resize(&view->jelly_wob, view->jelly_w, view->jelly_h);
        /* A client that committed something other than its slot — terminals
         * round to whole character cells — moved the edges the fill is spreading
         * toward while it was spreading toward them. */
        if (view->drop_filling) {
            droplet_fill_retarget(&view->drop_fill, view->jelly_w, view->jelly_h);
        }
    }

    /* For as long as the picture is on screen, whether the hand is still on the
     * window or the wobble is ringing out. Skipping the ring-out was a mistake
     * you could watch: a released window is thrown, and a thrown window keeps
     * moving, so the sheet kept being driven and the effect outlasted the whole
     * flight with a still frame in it. */
    view->jelly_snap_t += dt;
    /* The client is hidden behind the sheet and gets no frame callbacks from
     * the scene; without these it would stop drawing and "live" would mean a
     * live view of a frozen window. */
    if (view->server->config.effects.live > 0.0) view_send_frame_done(view);

    if (view->jelly_live) {

        /* Nothing to retake — view_jelly_draw reads the client's texture as it
         * goes. Only a window that has stopped being a single surface the size
         * of its own geometry (a menu opened under the hand) needs rebuilding
         * onto the snapshot path; a
         * missing texture for one frame is not that, and view_jelly_draw simply
         * keeps the frame already on screen. */
        if (!view_is_single_surface(view) || !view_buffer_is_the_window(view)) {
            if (!view_jelly_setup(view)) { view_jelly_stop(view); return; }
            wobble_resize(&view->jelly_wob, view->jelly_w, view->jelly_h);
        }
        view->content_dirty = 0;
    } else if (view_snapshot_due(view, view->jelly_snap_t)) {
        view->content_dirty = 0;
        view->jelly_snap_t = 0.0;
        view->server->fx_snaps++;
        view_jelly_resnap(view);
    }

    /* The landing runs instead of the springs, not alongside them: the window
     * is already sitting in its slot and nothing is moving it, so there is no
     * translation to drive a wobble with and nothing for one to say. */
    if (view->drop_filling) {
        bool more = droplet_fill_step(&view->drop_fill, dt);
        view->jelly_px = view->x;
        view->jelly_py = view->y;
        view->server->fx_moved++;
        if (!view_jelly_draw(view, strength) || !more) {
            view->drop_filling = 0;
            view->drop_want = view->drop_round = 0.0;
            view_jelly_stop(view);
        }
        return;
    }

    /* Round off toward whatever the drop wants — up when it is picked out of a
     * tree, back down if it turns out not to be going anywhere near one. Frame
     * -rate independent, like every other approach in fwm. */
    {
        double k = 1.0 - exp(-DROP_ROUND_RATE * dt);
        view->drop_round += (view->drop_want - view->drop_round) * k;
        if (fabs(view->drop_want - view->drop_round) < 0.005) {
            view->drop_round = view->drop_want;
        }
    }

    /* What the sheet is allowed to stretch to, this tick. The buffer it is drawn
     * into is the window plus jelly_margin on every side, and `strength` scales
     * the offsets on the way out (view_jelly_draw) — so what has to fit in the
     * margin is the offset TIMES the strength, and a stronger setting saturates
     * proportionally sooner rather than tearing that much earlier. Re-stated
     * every tick because both can change under a live config reload. */
    wobble_set_limit(&view->jelly_wob,
                     strength > 1.0 ? view->jelly_margin / strength
                                    : (double)view->jelly_margin);

    /* The window's own movement is the only thing that ever drives the sheet:
     * the rest positions go where the window went, the points stay where they
     * were, and the springs take it from there.
     *
     * Which stops the moment the hand is off it. The lattice lives in the
     * window's own frame, so simply not translating IS the sheet riding along
     * with the flight — and it has to, because a window sailing at a steady
     * speed holds a steady lag, and a steady lag is never at rest. Left driven,
     * the wobble ran until the physics did, and everything below it (the impact
     * squash, the live window) waited that long too. */
    if (!view->jelly_settling) {
        wobble_translate(&view->jelly_wob, view->x - view->jelly_px, view->y - view->jelly_py);
    }
    view->jelly_px = view->x;
    view->jelly_py = view->y;

    /* `strength` scales how far the sheet is allowed to be from its rest shape,
     * not the springs themselves — the wobble keeps its timing at any setting
     * and only gets bigger or smaller. */
    wobble_step(&view->jelly_wob, dt);

    if (view->jelly_settling && wobble_at_rest(&view->jelly_wob)) {
        view_jelly_stop(view);
        return;
    }

    view->server->fx_moved++;
    if (!view_jelly_draw(view, strength)) view_jelly_stop(view);
}

/* ── free rotation ────────────────────────────────────────────────────── */

bool view_is_spinning(FwmView *view) {
    return view->spin_buf != NULL;
}

/* The rotated snapshot is a square as wide as the window's diagonal, so left
 * to itself it would swallow clicks in a fat transparent border around the
 * window — including, at 45 degrees, most of a neighbouring window's corner.
 * Rotating the point back and testing it against the upright rectangle gives
 * the cursor the tilted window it can actually see. */
static bool spin_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    FwmView *view = buffer->node.data;
    if (!view || !view->spin_buf) return true;

    double half = view->spin_size / 2.0;
    double lx = *sx - half, ly = *sy - half;   /* relative to the center */
    double c = cos(-view->spin_angle), s = sin(-view->spin_angle);
    double ux = c * lx - s * ly;
    double uy = s * lx + c * ly;
    return fabs(ux) <= view->spin_w / 2.0 && fabs(uy) <= view->spin_h / 2.0;
}

/* Tear down the machinery WITHOUT touching what it hid — view_stop_spin does
 * that part, and a mid-spin resize deliberately does not. */
static void view_spin_release(FwmView *view) {
    if (view->spin_buf) {
        wlr_scene_node_destroy(&view->spin_buf->node);
        view->spin_buf = NULL;
    }
    if (view->spin_tex) {
        wlr_texture_destroy(view->spin_tex);
        view->spin_tex = NULL;
    }
    if (view->spin_src) {
        wlr_buffer_unlock(view->spin_src);
        view->spin_src = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (view->spin_dst[i]) {
            wlr_buffer_unlock(view->spin_dst[i]);
            view->spin_dst[i] = NULL;
        }
    }
    view->spin_w = view->spin_h = view->spin_size = 0;
    view->spin_flip = 0;
    view->spin_snap_t = 0.0;
    view->spin_angle = 0.0;
    view->spin_live = 0;
    view->spin_seen = NULL;
}

void view_stop_spin(FwmView *view) {
    if (!view->spin_buf) return;
    int border = view->spin_border;
    view_spin_release(view);
    view_set_content_enabled(view, true);
    view->server->pointer_resync_due = 1;
    if (border) view_set_border_enabled(view, 1);
    view_update_border_geometry(view);
}

/* Build (or rebuild) the snapshot, the two rotation targets and the scene node.
 * Called on the first tick and again whenever the window changes size. */
static bool view_spin_setup(FwmView *view) {
    FwmServer *server = view->server;
    if (!view->scene_tree || !server->wlr_renderer) return false;

    int w = view->width, h = view->height;
    if (w <= 0 || h <= 0) return false;

    bool restarting = view->spin_buf != NULL;
    int border = restarting ? view->spin_border
                            : (view->border[0] && view->border[0]->node.enabled);
    view_spin_release(view);

    /* The live content has to be visible to be photographed, so the snapshot
     * comes first and the window is hidden only once it succeeded — a failed
     * setup must leave the window on screen, not blank. */
    if (restarting) view_set_content_enabled(view, true);

    /* One surface and nothing else: rotate the client's own texture and skip
     * the snapshot machinery entirely. The window then turns LIVE — a video
     * keeps playing as it tumbles — and costs less than the frozen version did.
     * Decided once per spin, and re-decided if the window grows a subsurface or
     * opens a menu while it turns (view_spin_tick).
     *
     * Only where arbitrary rotation actually works: the quarter-turn fallback
     * below hands the SNAPSHOT BUFFER to the scene graph, and a client texture
     * is not a buffer we may hand over. On such a renderer the snapshot is the
     * only path, live or not. */
    view->spin_live = rotate_supported(server->wlr_renderer)
                   && view_live_texture(view) != NULL;
    view_log_effect_path(view, "spin", view->spin_live);

    if (!view->spin_live) {
        struct wlr_buffer *src = snapshot_alloc(server, w, h);
        if (!src) return false;
        if (!view_snapshot_into(view, src)) {
            wlr_buffer_drop(src);
            return false;
        }
        view->spin_src = wlr_buffer_lock(src);
        wlr_buffer_drop(src);

        /* Imported once and kept: the rotation redraws from this texture every
         * frame, and re-importing a dmabuf 60 times a second is pure waste. The
         * refresh below renders into the same buffer, so the texture stays valid
         * across snapshots too. */
        view->spin_tex = wlr_texture_from_buffer(server->wlr_renderer, view->spin_src);
        if (!view->spin_tex) goto fail;
    }

    /* A square of the diagonal holds the window at every angle, so the target
     * never has to be reallocated as it turns. Two of them, used alternately:
     * the scene may still be reading last frame's buffer while this one is
     * drawn, and overwriting it in place would tear. */
    int size = (int)ceil(hypot(w, h)) + 2;
    for (int i = 0; i < 2; i++) {
        struct wlr_buffer *d = snapshot_alloc(server, size, size);
        if (!d) goto fail;
        view->spin_dst[i] = wlr_buffer_lock(d);
        wlr_buffer_drop(d);
    }

    view->spin_buf = wlr_scene_buffer_create(view->scene_tree, NULL);
    if (!view->spin_buf) goto fail;
    wlr_scene_node_lower_to_bottom(&view->spin_buf->node);
    /* view_at() walks up from the node's PARENT to find the view, so this is
     * free for the hit test to use. */
    view->spin_buf->node.data = view;
    view->spin_buf->point_accepts_input = spin_accepts_input;

    view->spin_w = w;
    view->spin_h = h;
    view->spin_size = size;
    view->spin_border = border;

    /* Everything the scene draws upright goes away: the client's own surfaces,
     * and the border rects, which are scene rectangles and cannot be tilted at
     * all (they are not even in the snapshot — it only collects buffers). */
    view_set_content_enabled(view, false);
    view_set_border_enabled(view, 0);
    return true;

fail:
    view_spin_release(view);
    view_set_content_enabled(view, true);
    if (border) view_set_border_enabled(view, 1);
    return false;
}

void view_spin_tick(FwmView *view, double angle, double dt) {
    /* The squash owns the same snapshot slot and deforms an upright window;
     * the two cannot both be showing. Rotation wins — it is the bigger, longer
     * lasting effect, and an impact that arrives mid-spin already shows itself
     * in the way the window tumbles. The drag wobble is a picture of the window
     * too, and gives way for the same reason. */
    if (view->squash_buf) view_stop_squash(view);
    view_jelly_stop(view);
    /* And the resize rubber, which is a picture of the same window at a size
     * the client has not answered yet. A window spun while it was being
     * resized is the one way the two ever meet. */
    view_rubber_end(view);

    bool redraw = false;

    if (!view->spin_buf || view->spin_w != view->width || view->spin_h != view->height) {
        if (!view_spin_setup(view)) {
            view_stop_spin(view);
            return;
        }
        redraw = true;   /* the node has no picture in it yet */
    }

    struct wlr_texture *src = view->spin_tex;

    view->spin_snap_t += dt;
    /* Either path keeps the client drawing while it is hidden; what differs is
     * what showing its frames costs us (view_snapshot_due). */
    if (view->server->config.effects.live > 0.0) view_send_frame_done(view);

    if (view->spin_live) {
        if (!view_is_single_surface(view) || !view_buffer_is_the_window(view)) {
            /* A menu opened, the client grew a subsurface, or its buffer
             * stopped being the window, mid-spin. The
             * live path cannot show that, so rebuild onto the snapshot one —
             * setup re-decides and lands on the snapshot for the same reason
             * we are here. */
            if (!view_spin_setup(view)) { view_stop_spin(view); return; }
            src = view->spin_tex;
            redraw = true;
        } else {
            src = wlr_surface_get_texture(view_surface(view));
            /* A new frame from the client: same window, new texture. */
            if (src && src != view->spin_seen) {
                view->spin_seen = src;
                redraw = true;
            }
        }
        view->content_dirty = 0;
    } else if (view_snapshot_due(view, view->spin_snap_t)) {
        /* The composited path. The live nodes have to come back for the length
         * of the pass; nothing is presented in between, so the window never
         * flashes upright. */
        view->content_dirty = 0;
        view->spin_snap_t = 0.0;
        view->server->fx_snaps++;
        redraw = true;
        view_set_content_enabled(view, true);
        /* The rotated picture is itself a buffer in this subtree, and the
         * snapshot pass collects every enabled buffer it finds — leave it on
         * and each refresh bakes the previous tilted frame into the next one,
         * one ghost image deeper every time. */
        wlr_scene_node_set_enabled(&view->spin_buf->node, false);
        view_snapshot_into(view, view->spin_src);
        wlr_scene_node_set_enabled(&view->spin_buf->node, true);
        view_set_content_enabled(view, false);
    }

    /* A window that has come to rest keeps the angle it stopped at, and the
     * effect then costs nothing per frame: redrawing an unchanged rotation of
     * an unchanged snapshot would just burn the GPU for an identical picture.
     * (Half a milliradian is well under a pixel of travel at any window size.) */
    if (fabs(angle - view->spin_angle) > 5e-4) redraw = true;
    if (!redraw) return;
    view->server->fx_moved++;
    if (view->server->fx_debug && dt > 0.0) {
        /* Wrapped into (-pi, pi]: Box2D reports the angle wrapped, and a
         * crossing is not a 355-degree step, it is a small one. */
        double step = angle - view->spin_angle;
        while (step >  M_PI) step -= 2.0 * M_PI;
        while (step < -M_PI) step += 2.0 * M_PI;
        double omega = step / dt;
        FwmServer *srv = view->server;
        if (srv->fx_omega_have) {
            double ref = fabs(omega) > fabs(srv->fx_omega_prev)
                       ? fabs(omega) : fabs(srv->fx_omega_prev);
            if (ref > 0.05) {   /* below this it is a window at rest, not motion */
                double jump = fabs(omega - srv->fx_omega_prev) / ref;
                if (jump > srv->fx_omega_jump) srv->fx_omega_jump = jump;
            }
        }
        srv->fx_omega_prev = omega;
        srv->fx_omega_have = 1;
    }
    /* Nothing to draw from: a client between buffers, or a snapshot that could
     * not be imported. Keep the last picture rather than blanking the window —
     * the next tick is 16ms away and will very likely have one. */
    if (!src) return;

    int size = view->spin_size;
    struct wlr_buffer *dst = view->spin_dst[view->spin_flip];

    if (rotate_blit(view->server->wlr_renderer, dst, src,
                    view->spin_w, view->spin_h, angle)) {
        view->spin_flip ^= 1;
        wlr_scene_buffer_set_buffer(view->spin_buf, dst);
        wlr_scene_buffer_set_dest_size(view->spin_buf, size, size);
        wlr_scene_buffer_set_transform(view->spin_buf, WL_OUTPUT_TRANSFORM_NORMAL);
        /* The target is centered on the window: half the slack on each side. */
        wlr_scene_node_set_position(&view->spin_buf->node,
                                    -(size - view->spin_w) / 2,
                                    -(size - view->spin_h) / 2);
    } else {
        /* No arbitrary rotation available (a non-GLES2 renderer, a shader that
         * would not build). Rather than dropping the effect entirely, show the
         * snapshot at the nearest quarter turn — those four angles ARE
         * expressible in the scene graph. The window then turns in steps
         * instead of smoothly, which still reads as a window that rotates.
         *
         * This path hands a BUFFER to the scene, which the live path does not
         * have — it draws from the client's texture, and that is not ours to
         * give away. Setup only chooses live when rotation is supported, so
         * arriving here with no snapshot means the blit failed for some other
         * reason; keep the last picture rather than blanking the window. */
        if (!view->spin_src) return;
        int quarter = ((int)lround(angle / (M_PI / 2.0)) % 4 + 4) % 4;
        static const enum wl_output_transform steps[4] = {
            WL_OUTPUT_TRANSFORM_NORMAL, WL_OUTPUT_TRANSFORM_90,
            WL_OUTPUT_TRANSFORM_180,    WL_OUTPUT_TRANSFORM_270,
        };
        /* A quarter turn swaps the window's width and height. */
        int qw = (quarter % 2) ? view->spin_h : view->spin_w;
        int qh = (quarter % 2) ? view->spin_w : view->spin_h;
        wlr_scene_buffer_set_buffer(view->spin_buf, view->spin_src);
        wlr_scene_buffer_set_transform(view->spin_buf, steps[quarter]);
        wlr_scene_buffer_set_dest_size(view->spin_buf, qw, qh);
        wlr_scene_node_set_position(&view->spin_buf->node,
                                    (view->spin_w - qw) / 2,
                                    (view->spin_h - qh) / 2);
    }
    view->spin_angle = angle;
}

