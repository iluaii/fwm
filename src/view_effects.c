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

static struct wlr_texture *view_live_texture(FwmView *view) {
    if (!view_is_single_surface(view)) return NULL;
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
     * screen-axis normal would be visibly wrong on a tilted one. */
    if (view->spin_buf) return;

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
    if (border) view_set_border_enabled(view, 1);
    view_update_border_geometry(view);
    /* The window is itself again and casts again — and this is the only place
     * that says so on the live path, since view->jelly is what was keeping the
     * shadow out. */
    view_shadow_update(view);
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
     * about one that is tilted. */
    if (view->spin_buf) return;
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
         * goes. Only a window that has STOPPED being a single surface (a menu
         * opened under the hand) needs rebuilding onto the snapshot path; a
         * missing texture for one frame is not that, and view_jelly_draw simply
         * keeps the frame already on screen. */
        if (!view_is_single_surface(view)) {
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
        if (!view_is_single_surface(view)) {
            /* A menu opened, or the client grew a subsurface, mid-spin. The
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

