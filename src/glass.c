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

#include "glass.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "blur.h"
#include "server.h"
#include "view.h"
#include "server_internal.h"
#include "snapshot.h"
#include "sun.h"
#include "ui/cairo_overlay.h"

/* Panes in rotation. Two is the minimum — the scene keeps a lock on the one it
 * is showing — and a third absorbs a frame the renderer is slow to let go of,
 * which is the same count and the same reason as the video wallpaper's pool. */
#define GLASS_PANES 3

typedef struct FwmGlass {
    struct wl_list link;
    FwmServer *server;
    struct wlr_scene_buffer *panel;   /* the overlay we hang under */
    struct wlr_scene_buffer *pane;    /* our own node, one place below it */
    struct wl_listener panel_destroy;

    int pw, ph;      /* the panel's own size */
    int margin;      /* room around it for the shadow to fall into */
    int w, h;        /* the pane: the panel plus that room on all four sides */
    /* What the buffers were built for. The blur radius decides how far down
     * the scratch is scaled and the shadow decides the margin, so a live
     * `fwmctl set glass.radius` has to be able to invalidate all of it. */
    double built_radius, built_shadow;

    struct wlr_buffer *out_buf[GLASS_PANES];
    int out_i;
    struct wlr_buffer *cap;        /* the photograph of the desktop */
    struct wlr_texture *cap_tex;
    BlurScratch scratch[3];

    /* The panel's own pixels, and our lock on them.
     *
     * The lock is not politeness: an overlay drawn once is made static, which
     * drops the overlay's copy the moment the scene has uploaded a texture,
     * and the mask would go with it. Holding one of our own is what lets the
     * hints sheet and the welcome panel be frosted at all. */
    struct wlr_buffer *mask_buf;
    struct wlr_texture *mask_tex;

    /* What the pane on screen was drawn from, and whether there is one.
     *
     * A pane redrawn on every frame would be a compositor that never sleeps:
     * pushing a new buffer damages the output, damage schedules another frame,
     * and that frame pushes again. An idle fwm renders nothing, and the frost
     * must not be the thing that changes that. So a pane is redrawn only when
     * the desktop under it was damaged, or when one of these changed. */
    struct GlassSig {
        int x, y, pw, ph, margin;
        float opacity;
        const void *mask;
        double radius, fill, tint, brightness;
        double sdx, sdy, sradius, salpha;
        float tint_color[3], shadow_color[3], keep[4];
    } sig;
    bool drawn;
} FwmGlass;

static struct wl_list g_panes;
static int g_panes_ready;
/* See glass_init: one compositor, and a dozen panel-building call sites with
 * no reason to know it exists. */
static FwmServer *g_server;

static void panes_init(void) {
    if (!g_panes_ready) {
        wl_list_init(&g_panes);
        g_panes_ready = 1;
    }
}

void glass_init(FwmServer *server) {
    panes_init();
    g_server = server;
}

double glass_fill(const FwmConfig *cfg, double fallback) {
    if (!cfg && g_server) cfg = &g_server->config;
    if (!cfg || !cfg->glass.enabled) return fallback;
    return cfg->glass.fill;
}

bool glass_available(const FwmConfig *cfg) {
    if (!cfg || !cfg->glass.enabled) return false;
    if (!g_server) return true;   /* the config alone says yes; see the header */
    return g_server->wlr_renderer && blur_supported(g_server->wlr_renderer);
}

/* ── the buffers ──────────────────────────────────────────────────────── */

static void scratch_free(BlurScratch *s) {
    if (s->tex) wlr_texture_destroy(s->tex);
    if (s->buf) wlr_buffer_drop(s->buf);
    s->tex = NULL;
    s->buf = NULL;
}

static bool scratch_make(FwmServer *server, BlurScratch *s, int w, int h) {
    s->buf = snapshot_alloc(server, w, h);
    if (!s->buf) return false;
    /* Imported once and kept: a texture of a buffer we own is a view of that
     * memory, not a copy of what was in it when the texture was made. */
    s->tex = wlr_texture_from_buffer(server->wlr_renderer, s->buf);
    if (!s->tex) {
        wlr_buffer_drop(s->buf);
        s->buf = NULL;
        return false;
    }
    return true;
}

static void buffers_free(FwmGlass *g) {
    /* Let the scene go of whichever pane it is holding before we drop our own
     * references to them: our drop is only a reference, but a node left
     * pointing at a buffer nobody is going to redraw is a stale picture that
     * would sit there until the panel next moved. */
    if (g->pane) wlr_scene_buffer_set_buffer(g->pane, NULL);
    for (int i = 0; i < GLASS_PANES; i++) {
        if (g->out_buf[i]) wlr_buffer_drop(g->out_buf[i]);
        g->out_buf[i] = NULL;
    }
    if (g->cap_tex) wlr_texture_destroy(g->cap_tex);
    if (g->cap)     wlr_buffer_drop(g->cap);
    g->cap_tex = NULL;
    g->cap = NULL;
    for (int i = 0; i < 3; i++) scratch_free(&g->scratch[i]);
    g->w = g->h = g->pw = g->ph = 0;
    g->built_radius = g->built_shadow = -1.0;
    g->out_i = 0;
    g->drawn = false;
}

/* How much room the blur and the shadow need on every side. Taken from the
 * length and the penumbra rather than from where the sun happens to be: the sun
 * moves, and a margin that moved with it would rebuild every buffer here as the
 * afternoon went on.
 *
 * Both numbers are reaches — three sigma, see blur.h — so what falls past this
 * margin is the last thousandth of the shadow and nothing the eye can find. */
static int glass_margin(const FwmConfig *cfg) {
    double reach = cfg->glass.radius;
    if (cfg->glass.shadow) {
        double s = cfg->glass.shadow_length + cfg->glass.shadow_blur;
        if (s > reach) reach = s;
    }
    int m = (int)ceil(reach) + 2;
    return m < 2 ? 2 : m;
}

static bool buffers_build(FwmGlass *g, int pw, int ph, const FwmConfig *cfg) {
    int margin = glass_margin(cfg);
    int w = pw + margin * 2, h = ph + margin * 2;
    double shadow_blur = cfg->glass.shadow ? cfg->glass.shadow_blur : 0.0;

    if (g->cap && g->pw == pw && g->ph == ph && g->margin == margin &&
        g->built_radius == cfg->glass.radius && g->built_shadow == shadow_blur)
        return true;

    buffers_free(g);
    if (pw <= 0 || ph <= 0 || w <= 0 || h <= 0) return false;

    double scale = blur_scratch_scale(cfg->glass.radius, shadow_blur, w, h);
    int sw = (int)lround(w * scale), sh = (int)lround(h * scale);
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;

    for (int i = 0; i < GLASS_PANES; i++) {
        g->out_buf[i] = snapshot_alloc(g->server, w, h);
        if (!g->out_buf[i]) { buffers_free(g); return false; }
    }
    g->cap = snapshot_alloc(g->server, w, h);
    if (!g->cap) { buffers_free(g); return false; }
    for (int i = 0; i < 3; i++) {
        if (!scratch_make(g->server, &g->scratch[i], sw, sh)) {
            buffers_free(g);
            return false;
        }
    }

    g->pw = pw; g->ph = ph;
    g->margin = margin;
    g->w = w; g->h = h;
    g->built_radius = cfg->glass.radius;
    g->built_shadow = shadow_blur;
    return true;
}

/* ── the mask ─────────────────────────────────────────────────────────── */

/* Take the panel's current pixels, if they are not the ones we already hold.
 * A panel that has not redrawn hands back the same buffer and this costs a
 * pointer comparison; a panel that has redrawn costs one upload, which is the
 * price of knowing the shape without asking the panel about it. */
static void mask_sync(FwmGlass *g, struct wlr_buffer *buf) {
    if (buf && buf != g->mask_buf) {
        if (g->mask_tex) wlr_texture_destroy(g->mask_tex);
        if (g->mask_buf) wlr_buffer_unlock(g->mask_buf);
        g->mask_tex = NULL;
        g->mask_buf = wlr_buffer_lock(buf);
    }
    /* Also the way back from a renderer that was lost and rebuilt: the pixels
     * are still ours, it is only the texture of them that died with the old
     * context. */
    if (g->mask_buf && !g->mask_tex)
        g->mask_tex = wlr_texture_from_buffer(g->server->wlr_renderer, g->mask_buf);
}

static void mask_free(FwmGlass *g) {
    if (g->mask_tex) wlr_texture_destroy(g->mask_tex);
    if (g->mask_buf) wlr_buffer_unlock(g->mask_buf);
    g->mask_tex = NULL;
    g->mask_buf = NULL;
}

/* Has anything under this pane changed since the last frame was committed?
 *
 * The scene's damage for this screen answers it, and answers it about
 * everything: a window moved, a terminal repainted, a video wallpaper pushed a
 * frame. Read BEFORE the pane touches anything, so what is in there is the
 * frame's own damage — the pane's last push went in before the last commit and
 * was rotated out with it.
 *
 * The box is the pane's, not the whole screen: with two monitors a pane's push
 * lands in both rings, and a screen answering "yes, something changed" to its
 * neighbour's frost is two screens redrawing each other for ever. */
static bool backdrop_dirty(FwmServer *server, FwmOutput *out, FwmGlass *g,
                           int lx, int ly, int w, int h) {
    if (!out || !out->wlr_output) return true;
    struct wlr_scene_output *so =
        wlr_scene_get_scene_output(server->scene, out->wlr_output);
    if (!so) return true;

    pixman_region32_t *dmg = &so->damage_ring.current;
    if (!pixman_region32_not_empty(dmg)) return false;
    /* A rotated screen gets the coarse answer rather than a wrong box: every
     * pane on it redraws whenever anything on it does. */
    if (out->wlr_output->transform != WL_OUTPUT_TRANSFORM_NORMAL) return true;

    double sc = out->wlr_output->scale;
    struct pixman_box32 box = {
        .x1 = (int)floor((lx - so->x) * sc),
        .y1 = (int)floor((ly - so->y) * sc),
        .x2 = (int)ceil((lx - so->x + w) * sc),
        .y2 = (int)ceil((ly - so->y + h) * sc),
    };
    if (box.x2 <= box.x1 || box.y2 <= box.y1) return false;
    return pixman_region32_contains_rectangle(dmg, &box) != PIXMAN_REGION_OUT;
}

/* ── the pane ─────────────────────────────────────────────────────────── */

static void pane_hide(FwmGlass *g) {
    if (g->pane) wlr_scene_node_set_enabled(&g->pane->node, false);
}

static void glass_free(FwmGlass *g) {
    wl_list_remove(&g->link);
    wl_list_remove(&g->panel_destroy.link);
    buffers_free(g);
    mask_free(g);
    if (g->pane) wlr_scene_node_destroy(&g->pane->node);
    free(g);
}

static void handle_panel_destroy(struct wl_listener *listener, void *data) {
    FwmGlass *g = wl_container_of(listener, g, panel_destroy);
    glass_free(g);
}

void glass_attach(struct wlr_scene_buffer *panel) {
    if (!g_server || !panel || !panel->node.parent) return;
    panes_init();

    FwmGlass *g = calloc(1, sizeof(*g));
    if (!g) return;
    g->server = g_server;
    g->panel = panel;
    g->built_radius = g->built_shadow = -1.0;

    g->pane = wlr_scene_buffer_create(panel->node.parent, NULL);
    if (!g->pane) { free(g); return; }
    wlr_scene_node_set_enabled(&g->pane->node, false);
    wlr_scene_node_place_below(&g->pane->node, &panel->node);

    g->panel_destroy.notify = handle_panel_destroy;
    wl_signal_add(&panel->node.events.destroy, &g->panel_destroy);
    wl_list_insert(&g_panes, &g->link);

    /* Whatever the panel has painted so far, taken now.
     *
     * A panel drawn once and made static gives its pixels up as soon as the
     * scene has them, and attach is the last moment they can be had. Panels
     * that redraw are not yet drawn at this point and hand back nothing; the
     * tick picks them up. */
    mask_sync(g, cairo_overlay_buffer(panel));
}

/* Everything the shader needs about the light, worked out once per frame.
 *
 * The direction is the sun's, normalised and walked back out to the panel's
 * own length: a panel lies much closer to the desktop than a window floats
 * above it, so it keeps the bearing and not the distance. Night, or [sun] off,
 * and there is no shadow here either — one light over the desktop means one
 * light, and a panel still casting at midnight would be the only thing on
 * screen disagreeing about that. */
static void light_for(const FwmServer *server, const FwmConfig *cfg,
                      BlurParams *p) {
    p->shadow_alpha = 0.0;
    p->shadow_dx = p->shadow_dy = 0.0;
    p->shadow_radius = 0.0;
    p->shadow_color[0] = p->shadow_color[1] = p->shadow_color[2] = 0.0f;

    if (!cfg->glass.shadow || !cfg->sun.enabled) return;
    const FwmSunLight *l = &server->sun_light;
    if (l->alpha <= 0.0) return;

    double len = hypot(l->dx, l->dy);
    if (len > 0.001) {
        p->shadow_dx = l->dx / len * cfg->glass.shadow_length;
        p->shadow_dy = l->dy / len * cfg->glass.shadow_length;
    }
    p->shadow_radius = cfg->glass.shadow_blur;
    p->shadow_alpha = cfg->glass.shadow_opacity * l->alpha;

    /* Back to straight RGB, the same undoing shadow_update does: colours are
     * stored premultiplied and the alpha in them is not how dark the shadow
     * is — [glass] shadow_opacity is. */
    if (cfg->sun.color[3] > 0.0f)
        for (int c = 0; c < 3; c++)
            p->shadow_color[c] = cfg->sun.color[c] / cfg->sun.color[3];
}

void glass_tick(FwmOutput *out) {
    if (!g_server || !g_panes_ready) return;

    FwmServer *server = g_server;
    const FwmConfig *cfg = &server->config;
    bool on = glass_available(cfg);

    FwmGlass *g;
    wl_list_for_each(g, &g_panes, link) {
        /* Switched off, live: hide the pane and give its buffers back. The
         * tray's are two megabytes that no longer draw anything. */
        if (!on) {
            pane_hide(g);
            if (g->cap) buffers_free(g);
            continue;
        }
        if (!g->panel->node.enabled || g->panel->opacity <= 0.004f) {
            pane_hide(g);
            continue;
        }

        mask_sync(g, cairo_overlay_buffer(g->panel));
        if (!g->mask_buf || !g->mask_tex) { pane_hide(g); continue; }

        FwmOutput *po = server_panel_output(server, g->panel);
        if (!po) { pane_hide(g); continue; }
        /* One screen's frame does not redraw another screen's panes: it would
         * photograph the desktop through the wrong camera and pay for it
         * twice a frame on the way. */
        if (out && po != out) continue;

        int lx, ly;
        if (!wlr_scene_node_coords(&g->panel->node, &lx, &ly)) {
            pane_hide(g);
            continue;
        }

        /* Everything the pane would be drawn from, gathered before anything is
         * drawn: it is both what the shader is told and what decides whether
         * the shader has to run at all. */
        BlurParams p = {
            .radius = cfg->glass.radius,
            .fill = cfg->glass.fill,
            .tint = cfg->glass.tint,
            .brightness = cfg->glass.brightness,
            .panel_w = g->mask_buf->width, .panel_h = g->mask_buf->height,
        };
        if (cfg->glass.tint_color[3] > 0.0f)
            for (int c = 0; c < 3; c++)
                p.tint_color[c] = cfg->glass.tint_color[c] / cfg->glass.tint_color[3];
        light_for(server, cfg, &p);

        int margin = glass_margin(cfg);
        p.panel_x = margin;
        p.panel_y = margin;
        int pane_w = p.panel_w + margin * 2, pane_h = p.panel_h + margin * 2;

        /* How much of the photograph will be of the screen and not of the void
         * beside it. The tray sits eight pixels below the top edge and the
         * frame reaches thirty-four above it, so a third of what the blur
         * would average is nothing at all — and the whole strip comes out
         * darker for it. Measured at 9% before this was here. */
        {
            double cx = lx - margin, cy = ly - margin;
            double x0 = (po->box.x - cx) / pane_w, x1 = (po->box.x + po->box.width  - cx) / pane_w;
            double y0 = (po->box.y - cy) / pane_h, y1 = (po->box.y + po->box.height - cy) / pane_h;
            /* Half a texel in from the boundary: the pixel on the seam is a
             * blend of the screen and the void, and clamping ONTO it would
             * extend that blend rather than the picture. */
            double hx = 0.5 / pane_w, hy = 0.5 / pane_h;
            p.keep[0] = (float)fmin(fmax(x0 + hx, 0.0), 1.0);
            p.keep[1] = (float)fmin(fmax(y0 + hy, 0.0), 1.0);
            p.keep[2] = (float)fmin(fmax(x1 - hx, 0.0), 1.0);
            p.keep[3] = (float)fmin(fmax(y1 - hy, 0.0), 1.0);
            if (p.keep[2] < p.keep[0]) p.keep[2] = p.keep[0];
            if (p.keep[3] < p.keep[1]) p.keep[3] = p.keep[1];
        }

        /* Zeroed whole, padding included: this is compared with memcmp, and
         * padding an initialiser list left unspecified would differ from one
         * frame to the next and redraw the pane for ever. */
        struct GlassSig sig;
        memset(&sig, 0, sizeof(sig));
        sig.x = g->panel->node.x; sig.y = g->panel->node.y;
        sig.pw = p.panel_w; sig.ph = p.panel_h; sig.margin = margin;
        sig.opacity = g->panel->opacity; sig.mask = g->mask_buf;
        sig.radius = p.radius; sig.fill = p.fill; sig.tint = p.tint;
        sig.brightness = p.brightness;
        sig.sdx = p.shadow_dx; sig.sdy = p.shadow_dy;
        sig.sradius = p.shadow_radius; sig.salpha = p.shadow_alpha;
        memcpy(sig.keep, p.keep, sizeof(sig.keep));
        memcpy(sig.tint_color, p.tint_color, sizeof(sig.tint_color));
        memcpy(sig.shadow_color, p.shadow_color, sizeof(sig.shadow_color));

        /* Nothing about the pane changed and nothing under it moved: leave the
         * one on screen exactly where it is. This is the whole of what keeps
         * an idle desktop idle. */
        if (g->drawn && memcmp(&sig, &g->sig, sizeof(sig)) == 0 &&
            !backdrop_dirty(server, po, g, lx - margin, ly - margin,
                            pane_w, pane_h))
            continue;

        if (!buffers_build(g, p.panel_w, p.panel_h, cfg)) {
            pane_hide(g);
            continue;
        }

        /* The photograph, with the panel's whole layer taken out of it.
         *
         * Not just this panel: the tray must not be frosted with a picture of
         * the launcher standing over it, and no pane may photograph itself —
         * which is what feeding a blur its own output looks like, and it looks
         * like a tunnel. Hiding the layer answers both at once, and the panes
         * live in it too. */
        struct wlr_scene_tree *layer = g->panel->node.parent;
        if (!snapshot_lens(server, po, g->cap, lx - g->margin, ly - g->margin,
                           layer ? &layer->node : NULL)) {
            pane_hide(g);
            continue;
        }
        if (!g->cap_tex)
            g->cap_tex = wlr_texture_from_buffer(server->wlr_renderer, g->cap);
        if (!g->cap_tex) { pane_hide(g); continue; }

        struct wlr_buffer *dst = g->out_buf[g->out_i];
        if (!blur_glass(server->wlr_renderer, dst, g->cap_tex, g->mask_tex,
                        &g->scratch[0], &g->scratch[1], &g->scratch[2], &p)) {
            pane_hide(g);
            continue;
        }
        g->out_i = (g->out_i + 1) % GLASS_PANES;

        wlr_scene_buffer_set_buffer(g->pane, dst);
        wlr_scene_node_set_position(&g->pane->node,
                                    g->panel->node.x - g->margin,
                                    g->panel->node.y - g->margin);
        /* Re-asserted every frame rather than only at attach: a panel that
         * raises itself to the top of its layer would otherwise leave its own
         * frost behind, under whatever it was raised over. */
        if (g->pane->node.parent == g->panel->node.parent)
            wlr_scene_node_place_below(&g->pane->node, &g->panel->node);
        /* The panel fades and rises when it appears; the pane under it has to
         * do both with it, or a frosted launcher arrives as a pane of glass
         * with nothing on it yet. */
        wlr_scene_buffer_set_opacity(g->pane, g->panel->opacity);
        wlr_scene_node_set_enabled(&g->pane->node, true);

        g->sig = sig;
        g->drawn = true;
    }
}

void glass_gpu_release(void) {
    if (!g_panes_ready) return;
    FwmGlass *g;
    wl_list_for_each(g, &g_panes, link) {
        pane_hide(g);
        buffers_free(g);
        /* The pixels are kept, the texture of them is not: everything on the
         * GPU belonged to the context that is going away, and mask_sync builds
         * a fresh one against the new renderer. */
        if (g->mask_tex) wlr_texture_destroy(g->mask_tex);
        g->mask_tex = NULL;
    }
    if (g_server && g_server->wlr_renderer) blur_shutdown(g_server->wlr_renderer);
}

void glass_finish(void) {
    if (!g_panes_ready) return;
    FwmGlass *g, *tmp;
    wl_list_for_each_safe(g, tmp, &g_panes, link) glass_free(g);
    if (g_server && g_server->wlr_renderer) blur_shutdown(g_server->wlr_renderer);
    g_server = NULL;
}
