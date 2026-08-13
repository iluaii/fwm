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
#include "shadow.h"
#include "rotate.h"
#include "snapshot.h"
#include "expo.h"
#include "theme.h"
#include "server.h"
#include "physics.h"
#include "bsp.h"
#include "group.h"
#include "session.h"
#include "foreign.h"
#include "ipc.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/log.h>
#include <drm_fourcc.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void handle_map(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, map);
    view_map(view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, unmap);
    view_unmap(view);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, commit);
    // The compositor must reply to the xdg_surface's initial commit with a
    // configure before the client is allowed to map its surface. Let the
    // client pick its own initial size. (X11 windows size themselves.)
    if (view->type == FWM_VIEW_XDG && view->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
    }
    // The mapping commit lands here immediately after view_map; the one after
    // it is the first with content the client actually drew, which is when the
    // fade may start.
    if (view->open_hold > 0) view->open_hold--;

    // Track the actual committed surface size so borders hug the real window.
    view_update_border_geometry(view);
    view_shadow_update(view);
    /* A client that has just added a subsurface handed us a scene buffer at
     * full strength; bring it down to whatever the rest of the window is at. */
    view_dim_apply(view);

    /* A tiled client that commits a size other than the one it was given
     * shifts where its neighbours belong — terminals do this on every resize,
     * rounding to whole character cells. Re-run the layout's positioning pass,
     * but only when the size actually moved: this runs on every commit. */
    {
        int cw, ch;
        view_committed_size(view, &cw, &ch);
        PhysicsBody *pb = physics_find_body(&view->server->physics, view->id);
        if (cw != view->aligned_w || ch != view->aligned_h) {
            view->aligned_w = cw;
            view->aligned_h = ch;
            if (pb && pb->tiled) server_align_tiles(view->server, pb->desktop_id);
        }

        /* Adopt the size the client actually took. A resize ASKS for a size;
         * what the window ends up being is the client's answer, and terminals
         * answer with the next size down that is a whole number of character
         * cells. Until this, the compositor kept believing the number it asked
         * for: the physics box stayed up to a cell taller and wider than the
         * window drawn inside it, and since clients paint from their top-left
         * corner, all of that slack showed as a gap along the bottom and right
         * — a resized window came to rest visibly above the floor, while the
         * top and left, where box and window still met, looked correct.
         *
         * Only our bookkeeping changes; the client is not configured back at
         * its own size, which is how this stays a single exchange and not a
         * loop.
         *
         * Not while fullscreen: there the geometry is the screen's, not the
         * client's to answer with. A game whose buffer is smaller than the
         * screen (its own resolution, scaled up for us) would otherwise shrink
         * the window it is filling, one commit at a time. */
        bool ours = pb && pb->fullscreen;
        if (!ours && cw > 0 && ch > 0 && (cw != view->width || ch != view->height)) {
            view->width = cw;
            view->height = ch;
            physics_sync_body(&view->server->physics, view->id, view->x, view->y,
                              cw, ch, view->server->screen_width);
        }
    }

    /* New content. An effect standing in front of the window reads this to
     * decide whether its picture is stale (view_spin_tick, view_jelly_tick). */
    view->content_dirty = 1;

    // Keep our own lock on the latest committed buffer: at unmap time the
    // client's buffer may already be gone, but the close animation needs the
    // last frame as a snapshot.
    struct wlr_surface *surface = view_surface(view);
    if (surface && surface->buffer) {
        wlr_buffer_lock(&surface->buffer->base);
        if (view->last_buffer) wlr_buffer_unlock(view->last_buffer);
        view->last_buffer = &surface->buffer->base;
    }
}

void view_place_borders(FwmView *view, int x, int y, int w, int h);
void view_border_box(FwmView *view, int *w, int *h);

/* ── shell-agnostic accessors ─────────────────────────────────────────── */

struct wlr_surface *view_surface(FwmView *view) {
    if (view->type == FWM_VIEW_XDG) return view->xdg_toplevel->base->surface;
    return view->xwl_surface->surface; /* NULL until the X11 window associates */
}

const char *view_title(FwmView *view) {
    return view->type == FWM_VIEW_XDG ? view->xdg_toplevel->title
                                      : view->xwl_surface->title;
}

/* The pid behind a view's client. xdg clients are asked through the Wayland
 * connection; XWayland surfaces carry the pid themselves. 0 when there is
 * nobody to ask — an unmapped X11 surface, or a client that has gone. */
pid_t view_pid(FwmView *view) {
    if (view->type == FWM_VIEW_XDG) {
        if (!view->xdg_toplevel || !view->xdg_toplevel->resource) return 0;
        struct wl_client *client = wl_resource_get_client(view->xdg_toplevel->resource);
        if (!client) return 0;
        pid_t pid = 0;
        wl_client_get_credentials(client, &pid, NULL, NULL);
        return pid;
    }
    return view->xwl_surface ? (pid_t)view->xwl_surface->pid : 0;
}

/* X11's closest equivalent of an app id is the WM_CLASS class. */
const char *view_app_id(FwmView *view) {
    return view->type == FWM_VIEW_XDG ? view->xdg_toplevel->app_id
                                      : view->xwl_surface->class;
}

void view_set_size(FwmView *view, int width, int height) {
    if (view->type == FWM_VIEW_XDG) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
    } else {
        /* X11 configure carries position too, and X clients read it as global
         * root coordinates — which are our SCREEN coordinates, not the world
         * ones view->x lives in. Sending the world position put every window
         * from the second desktop on thousands of pixels off the X screen (the
         * root is only as big as the monitors), and clients that steer by root
         * coords — GLFW warps the pointer to the window centre for mouse-look —
         * then aimed at nowhere and stopped responding to the mouse.
         *
         * With the window's desktop off every monitor there is no screen
         * position to map to. Leave the X one where it is; server_camera_settled
         * resyncs every view once the camera stops, so it is corrected the
         * moment the window can be seen again. */
        double sx = view->xwl_surface->x, sy = view->xwl_surface->y;
        server_world_to_screen(view->server, view->x, view->y, &sx, &sy);
        wlr_xwayland_surface_configure(view->xwl_surface,
            (int16_t)lround(sx), (int16_t)lround(sy),
            (uint16_t)width, (uint16_t)height);
    }
}

void view_sync_position(FwmView *view) {
    if (view->type != FWM_VIEW_XWAYLAND) return;
    view_set_size(view, view->width, view->height);
}

void view_send_close(FwmView *view) {
    if (view->type == FWM_VIEW_XDG) wlr_xdg_toplevel_send_close(view->xdg_toplevel);
    else wlr_xwayland_surface_close(view->xwl_surface);
}

void view_set_activated(FwmView *view, bool activated) {
    if (view->type == FWM_VIEW_XDG) {
        wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
    } else {
        wlr_xwayland_surface_activate(view->xwl_surface, activated);
        if (activated) {
            wlr_xwayland_surface_restack(view->xwl_surface, NULL, XCB_STACK_MODE_ABOVE);
        }
    }
}

void view_set_fullscreen_hint(FwmView *view, bool fullscreen) {
    if (view->type == FWM_VIEW_XDG) {
        wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
    } else {
        wlr_xwayland_surface_set_fullscreen(view->xwl_surface, fullscreen);
    }
}

/* ── focus border ─────────────────────────────────────────────────────── */

/* Border box in scene-tree-local coordinates. Split out so the squash can hug
 * a deformed, offset box instead of the window's real geometry. */
void view_place_borders(FwmView *view, int x, int y, int w, int h) {
    if (!view->border[0]) return;
    int bw = view->server->config.decor.border_width;

    // top, bottom, left, right — hugging the outside of the window
    wlr_scene_node_set_position(&view->border[0]->node, x - bw, y - bw);
    wlr_scene_rect_set_size(view->border[0], w + 2 * bw, bw);
    wlr_scene_node_set_position(&view->border[1]->node, x - bw, y + h);
    wlr_scene_rect_set_size(view->border[1], w + 2 * bw, bw);
    wlr_scene_node_set_position(&view->border[2]->node, x - bw, y);
    wlr_scene_rect_set_size(view->border[2], bw, h);
    wlr_scene_node_set_position(&view->border[3]->node, x + w, y);
    wlr_scene_rect_set_size(view->border[3], bw, h);
}

/* The window's own box, as committed. */
void view_border_box(FwmView *view, int *w, int *h) {
    if (view->type == FWM_VIEW_XDG) {
        *w = view->xdg_toplevel->base->current.geometry.width;
        *h = view->xdg_toplevel->base->current.geometry.height;
    } else {
        struct wlr_surface *s = view->xwl_surface->surface;
        *w = s ? s->current.width : 0;
        *h = s ? s->current.height : 0;
    }
    if (*w <= 0) *w = view->width;
    if (*h <= 0) *h = view->height;
}

void view_committed_size(FwmView *view, int *w, int *h) {
    view_border_box(view, w, h);
}

/* The smallest size the client says it will accept, in the same units as
 * view_committed_size. Zero when it has not said — which is most windows, and
 * why the tiling layout also learns a floor from what a window does when it is
 * offered less (see tile_actuals). */
void view_min_size(FwmView *view, int *w, int *h) {
    *w = 0; *h = 0;
    if (view->type == FWM_VIEW_XDG) {
        if (!view->xdg_toplevel) return;
        *w = view->xdg_toplevel->current.min_width;
        *h = view->xdg_toplevel->current.min_height;
    } else {
        if (!view->xwl_surface || !view->xwl_surface->size_hints) return;
        *w = view->xwl_surface->size_hints->min_width;
        *h = view->xwl_surface->size_hints->min_height;
    }
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

void view_update_border_geometry(FwmView *view) {
    if (!view->border[0]) return;
    if (view->squash_buf) return; /* the squash owns the border box meanwhile */
    int w, h;
    view_border_box(view, &w, &h);
    view_place_borders(view, 0, 0, w, h);
}

void view_set_border_color(FwmView *view, const float color[4]) {
    if (!view->border[0]) return;

    /* No fade compensation needed: during the open animation the window is
     * either hidden entirely or fully opaque under our own cover rect. */
    for (int i = 0; i < 4; i++) {
        wlr_scene_rect_set_color(view->border[i], color);
    }
}

void view_set_border_enabled(FwmView *view, int enabled) {
    if (!view->border[0]) return;
    for (int i = 0; i < 4; i++) {
        wlr_scene_node_set_enabled(&view->border[i]->node, enabled);
    }
}

/* ── the shadow ───────────────────────────────────────────────────────── */

void view_shadow_update(FwmView *view) {
    if (!view->shadow) return;

    /* A window filling the screen has nothing to cast onto — and a real
     * fullscreen client is entitled to every pixel of it. */
    PhysicsBody *body = physics_find_body(&view->server->physics, view->id);
    if (body && body->fullscreen) {
        shadow_set_enabled(view->shadow, false);
        return;
    }
    /* The effects that replace the window with a picture of itself (the spin,
     * the wobble, the squash) draw it somewhere other than its own box, and a
     * shadow left behind at the old one would be a rectangle the window is no
     * longer standing in. They put it out for their duration. */
    if (view->spin_buf || view->jelly || view->squash_buf) {
        shadow_set_enabled(view->shadow, false);
        return;
    }

    int w, h;
    view_border_box(view, &w, &h);
    shadow_update(view->shadow, w, h, &view->server->config.sun,
                  &view->server->sun_light);
}

/* ── the unfocused dim ────────────────────────────────────────────────── */

struct dim_ctx {
    FwmView *view;
    float    opacity;
};

static void dim_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
    (void)sx; (void)sy;
    struct dim_ctx *ctx = data;
    /* The shadow is in this tree too, and its strength is the sun's business:
     * fading a window's shadow along with the window would make the light
     * follow the keyboard around. */
    if (shadow_owns_buffer(ctx->view->shadow, buffer)) return;
    wlr_scene_buffer_set_opacity(buffer, ctx->opacity);
}

void view_dim_apply(FwmView *view) {
    if (!view->scene_tree) return;
    /* While the window is opening it is covered by our own fade rect and must
     * stay fully opaque underneath — blending a client's first frames is
     * exactly what the open animation exists to avoid. The dim lands when the
     * animation lets go. */
    if (view->open_anim) return;
    struct dim_ctx ctx = { .view = view, .opacity = (float)view->dim };
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, dim_iter, &ctx);
}

void view_dim_set(FwmView *view, double target, bool immediate) {
    if (target < 0.0) target = 0.0;
    if (target > 1.0) target = 1.0;
    view->dim_target = target;
    if (immediate) {
        view->dim = target;
        view_dim_apply(view);
    }
}

bool view_dim_tick(FwmView *view, double dt) {
    if (view->dim == view->dim_target) return false;

    double ms = view->server->config.decor.dim_ms;
    if (ms <= 0.0) {
        view->dim = view->dim_target;
    } else {
        /* Framerate-independent exponential chase, the same form the tile
         * glide and the camera use. `ms` reads as the time to cover most of
         * the distance, not all of it, so the tail is cut off below. */
        double k = 1.0 - exp(-dt * 3000.0 / ms);
        view->dim += (view->dim_target - view->dim) * k;
        if (fabs(view->dim_target - view->dim) < 0.004) view->dim = view->dim_target;
    }
    view_dim_apply(view);
    return true;
}

void view_dim_suspend(FwmView *view) {
    if (!view->scene_tree || view->dim >= 1.0) return;
    struct dim_ctx ctx = { .view = view, .opacity = 1.0f };
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, dim_iter, &ctx);
}

void view_dim_restore(FwmView *view) {
    if (!view->scene_tree || view->dim >= 1.0) return;
    view_dim_apply(view);
}

/* ── fade-in ──────────────────────────────────────────────────────────── */

static void handle_destroy(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, destroy);
    view_destroy(view);
}

static void handle_request_move(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_move);
    struct wlr_xdg_toplevel_move_event *event = data;
    server_start_interactive_move(view->server, view, event->serial);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    server_start_interactive_resize(view->server, view, event->edges, event->serial);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_fullscreen);
    // Under wlroots-0.20, check the requested state on the shell object.
    // A client-initiated fullscreen request maps to real (whole-output) fullscreen.
    bool fullscreen = view->type == FWM_VIEW_XDG
        ? view->xdg_toplevel->requested.fullscreen
        : view->xwl_surface->fullscreen;
    server_set_fullscreen(view->server, view, fullscreen, true);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, set_title);
    if (view->group) group_redraw(view->server, view->group);
    foreign_view_title_changed(view);
    server_request_tray_redraw(view->server);

    /* Clients announce a title before mapping too; reporting those would have
     * a subscriber hear about windows it was never told had opened. */
    struct wlr_surface *surface = view_surface(view);
    if (surface && surface->mapped)
        ipc_emit_window(view->server->ipc, FWM_EV_WINDOW_TITLE, view);
}

/* ── X11 (Xwayland) handlers ──────────────────────────────────────────── */

static void xwl_handle_associate(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, xwl_associate);
    // The wlr_surface exists only now: hook map/unmap/commit here, where the
    // xdg path hooks them at create time.
    wl_signal_add(&view->xwl_surface->surface->events.map, &view->map);
    wl_signal_add(&view->xwl_surface->surface->events.unmap, &view->unmap);
    wl_signal_add(&view->xwl_surface->surface->events.commit, &view->commit);
}

static void xwl_handle_dissociate(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, xwl_dissociate);
    // Re-init after removal so view_destroy can remove them again safely.
    wl_list_remove(&view->map.link);    wl_list_init(&view->map.link);
    wl_list_remove(&view->unmap.link);  wl_list_init(&view->unmap.link);
    wl_list_remove(&view->commit.link); wl_list_init(&view->commit.link);
}

static void xwl_handle_request_configure(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, xwl_request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    struct wlr_surface *surface = view->xwl_surface->surface;
    if (!surface || !surface->mapped) {
        // Not mapped yet: let the client have exactly what it asked for.
        wlr_xwayland_surface_configure(view->xwl_surface, ev->x, ev->y, ev->width, ev->height);
        return;
    }
    /* Mapped: the compositor owns the position — and where it also owns the
     * SIZE, the answer to a client asking for a different one is no, said the
     * only way X has of saying it: a configure carrying the geometry the window
     * actually has (ICCCM 4.1.5), so the client learns nothing moved.
     *
     * Fullscreen is where this matters. A game changing its resolution asks for
     * the new mode's size; Xwayland scales that buffer up to the screen for us
     * and needs the window to STAY screen-sized for the emulation to hold.
     * Resizing it instead left the window a fraction of the screen while it was
     * still flagged fullscreen — borderless, tray hidden, geometry nobody
     * agreed on — and the game, drawing at a size the window no longer had,
     * came apart (CS2 segfaults there).
     *
     * Only fullscreen. A tile that insists on its own size is let have it — the
     * alignment pass is built to absorb exactly that (a terminal rounding to
     * character cells), and refusing there means answering a client that will
     * ask again, which is a configure loop at socket speed rather than a
     * layout. A free-floating window is ordinary: physics carries the new box.
     *
     * The refusal is said once per size asked for. A client that repeats itself
     * has already been told; answering again is the same loop. */
    PhysicsBody *b = physics_find_body(&view->server->physics, view->id);
    if (b && b->fullscreen) {
        if (ev->width != view->cfg_denied_w || ev->height != view->cfg_denied_h) {
            view->cfg_denied_w = ev->width;
            view->cfg_denied_h = ev->height;
            view_sync_position(view);
        }
        return;
    }
    view->cfg_denied_w = view->cfg_denied_h = 0;
    view->width = ev->width;
    view->height = ev->height;
    physics_sync_body(&view->server->physics, view->id, view->x, view->y,
                      view->width, view->height, view->server->screen_width);
    view_sync_position(view);
}

/* Override-redirect is not a property a window is born with and keeps.
 *
 * It is an attribute, and X11 lets a client change it on a live window —
 * Wine does exactly that: winex11 creates a window unmanaged while it is
 * still being set up, or when the game asks for a borderless fullscreen it
 * means to own outright, and clears the flag later when the same window
 * becomes an ordinary top-level. Deciding once, at creation, left such a
 * window in the wrong half of the compositor for the rest of its life: a
 * bare scene surface with no body, no border, no focus and no place in the
 * layout, which is a game that renders but cannot be typed into.
 *
 * So the surface changes hands. The view is torn down (unmapping first if it
 * was mapped, which is what view_destroy already does) and the same
 * wlr_xwayland_surface is handed to the unmanaged path, which adopts it
 * mapped or not. The window blinks — it loses its physics body and gets a
 * new one on the way back — and that is honest: it stopped being the kind of
 * thing that had one. */
static void xwl_handle_set_override_redirect(struct wl_listener *listener, void *data) {
    (void)data;
    FwmView *view = wl_container_of(listener, view, xwl_set_override_redirect);
    struct wlr_xwayland_surface *xs = view->xwl_surface;
    if (!xs->override_redirect) return;

    FwmServer *server = view->server;
    view_destroy(view);
    server_xwl_unmanaged_create(server, xs);
}

void view_xwl_adopt(FwmView *view) {
    if (!view || view->type != FWM_VIEW_XWAYLAND) return;
    struct wlr_surface *surface = view->xwl_surface->surface;
    if (!surface) return;   /* not associated yet: the normal path still applies */

    xwl_handle_associate(&view->xwl_associate, NULL);
    if (surface->mapped) view_map(view);
}

static void xwl_handle_request_move(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_move);
    server_start_interactive_move(view->server, view, 0);
}

static void xwl_handle_request_resize(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, request_resize);
    struct wlr_xwayland_resize_event *event = data;
    server_start_interactive_resize(view->server, view, event->edges, 0);
}

static uint32_t next_view_id = 1;

FwmView *view_xwl_create(struct wlr_xwayland_surface *xsurface, struct FwmServer *server) {
    FwmView *view = calloc(1, sizeof(FwmView));
    if (!view) return NULL;

    view->id = next_view_id++;
    view->type = FWM_VIEW_XWAYLAND;
    view->xwl_surface = xsurface;
    view->server = server;

    // map/unmap/commit attach on associate (no wlr_surface yet); init the
    // links so removal in view_destroy is safe even if it never associates.
    view->map.notify = handle_map;       wl_list_init(&view->map.link);
    view->unmap.notify = handle_unmap;   wl_list_init(&view->unmap.link);
    view->commit.notify = handle_commit; wl_list_init(&view->commit.link);

    view->destroy.notify = handle_destroy;
    wl_signal_add(&xsurface->events.destroy, &view->destroy);
    view->xwl_associate.notify = xwl_handle_associate;
    wl_signal_add(&xsurface->events.associate, &view->xwl_associate);
    view->xwl_dissociate.notify = xwl_handle_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &view->xwl_dissociate);
    view->xwl_request_configure.notify = xwl_handle_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &view->xwl_request_configure);
    view->xwl_set_override_redirect.notify = xwl_handle_set_override_redirect;
    wl_signal_add(&xsurface->events.set_override_redirect, &view->xwl_set_override_redirect);
    view->request_move.notify = xwl_handle_request_move;
    wl_signal_add(&xsurface->events.request_move, &view->request_move);
    view->request_resize.notify = xwl_handle_request_resize;
    wl_signal_add(&xsurface->events.request_resize, &view->request_resize);
    view->request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &view->request_fullscreen);
    view->set_title.notify = handle_set_title;
    wl_signal_add(&xsurface->events.set_title, &view->set_title);

    wl_list_insert(&server->views, &view->link);
    return view;
}

FwmView *view_create(struct wlr_xdg_toplevel *toplevel, struct FwmServer *server) {
    FwmView *view = calloc(1, sizeof(FwmView));
    if (!view) return NULL;
    
    view->id = next_view_id++;
    view->type = FWM_VIEW_XDG;
    view->xdg_toplevel = toplevel;
    view->server = server;
    
    view->map.notify = handle_map;
    view->unmap.notify = handle_unmap;
    view->commit.notify = handle_commit;
    view->destroy.notify = handle_destroy;
    view->request_move.notify = handle_request_move;
    view->request_resize.notify = handle_request_resize;
    view->request_fullscreen.notify = handle_request_fullscreen;
    view->set_title.notify = handle_set_title;
    
    // In wlroots-0.20, map/unmap are on wlr_surface
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    // Must be the toplevel's own destroy event, not the xdg_surface's: wlroots
    // asserts all toplevel listeners (e.g. request_fullscreen) are removed
    // before the toplevel itself is destroyed, and that happens before the
    // underlying xdg_surface's destroy event fires.
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
    wl_signal_add(&toplevel->events.request_move, &view->request_move);
    wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
    wl_signal_add(&toplevel->events.set_title, &view->set_title);
    
    wl_list_insert(&server->views, &view->link);
    
    return view;
}

void view_destroy(FwmView *view) {
    if (!view) return;

    /* Cheap when the window was already unmapped, and the only cover for a
     * client that is destroyed without unmapping first. */
    expo_forget_view(view->server, view);
    
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->set_title.link);
    if (view->type == FWM_VIEW_XWAYLAND) {
        wl_list_remove(&view->xwl_associate.link);
        wl_list_remove(&view->xwl_dissociate.link);
        wl_list_remove(&view->xwl_request_configure.link);
        wl_list_remove(&view->xwl_set_override_redirect.link);
    }
    wl_list_remove(&view->link);
    
    struct wlr_surface *surface = view_surface(view);
    if (surface && surface->mapped) {
        view_unmap(view);
    }
    if (view->last_buffer) {
        wlr_buffer_unlock(view->last_buffer);
        view->last_buffer = NULL;
    }
    
    free(view);
}

void view_map(FwmView *view) {
    // Parented to the windows layer (below the overlay layer) so raising a
    // view to top on focus can never cover the tray/hints/welcome overlays.
    if (view->type == FWM_VIEW_XDG) {
        // In wlroots-0.20, use wlr_scene_xdg_surface_create on xdg_toplevel->base.
        view->scene_tree = wlr_scene_xdg_surface_create(view->server->layer_windows, view->xdg_toplevel->base);
    } else {
        view->scene_tree = wlr_scene_tree_create(view->server->layer_windows);
        if (view->scene_tree &&
            !wlr_scene_surface_create(view->scene_tree, view->xwl_surface->surface)) {
            wlr_scene_node_destroy(&view->scene_tree->node);
            view->scene_tree = NULL;
        }
    }
    if (!view->scene_tree) {
        fprintf(stderr, "Failed to create scene tree for view\n");
        return;
    }
    
    view->scene_tree->node.data = view;
    if (view->type == FWM_VIEW_XDG) {
        // Popups look their parent's scene tree up through xdg_surface->data
        // (see handle_new_xdg_popup in server.c).
        view->xdg_toplevel->base->data = view->scene_tree;
    }

    // Focus border rects (created disabled-color as "inactive"; focus recolors).
    int bw = view->server->config.decor.border_width;
    if (bw > 0) {
        for (int i = 0; i < 4; i++) {
            view->border[i] = wlr_scene_rect_create(view->scene_tree, 1, 1,
                                                    theme_get()->border_inactive);
        }
    }

    /* The shadow, under both. Created for every window while [sun] is on,
     * whatever the light is doing at the time: night is nine hidden nodes, and
     * building them at sunrise instead would mean walking every window on the
     * system at the one moment the user is watching the light change. */
    if (view->server->config.sun.enabled) view->shadow = shadow_create(view->scene_tree);

    /* Full strength until focus says otherwise — which server_focus_view does
     * a few lines after the window is mapped, through view_dim_set. */
    view->dim = view->dim_target = 1.0;

    // Open animation: hide the window outright until the client has painted
    // something real. Disabling the node is absolute — unlike opacity 0 it
    // cannot be undone by a new scene node appearing on a client commit.
    if (view->server->config.decor.fade_in_ms > 0.0) {
        view->open_anim = 1;
        view->open_t = 0.0;
        view->open_hold = 2;
        view->open_hold_ms = 0.0;
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }

    int initial_w, initial_h;
    if (view->type == FWM_VIEW_XDG) {
        initial_w = view->xdg_toplevel->base->current.geometry.width;
        initial_h = view->xdg_toplevel->base->current.geometry.height;
    } else {
        initial_w = view->xwl_surface->width;
        initial_h = view->xwl_surface->height;
    }
    if (initial_w <= 0) initial_w = view->server->screen_width / 2;
    if (initial_h <= 0) initial_h = view->server->screen_height / 2;
    
    view->width = initial_w;
    view->height = initial_h;

    /* Window rules ([[rule]] in config.toml) are matched once, here, because
     * app_id and title are what the client announced before mapping. Matching
     * BEFORE the desktop is chosen means the position, the physics body and
     * bsp_insert below all agree on where the window lives — nothing
     * downstream needs to know a rule was involved. */
    ConfigRule rule;
    int have_rule = config_match_rules(&view->server->config,
                                       view_app_id(view), view_title(view), &rule);

    int current_desktop = server_active_desktop(view->server);
    if (have_rule && rule.desktop >= 0) current_desktop = rule.desktop;

    /* A window from an application this session relaunched goes back where it
     * was. Checked after [[rule]] so that a rule the user wrote by hand still
     * wins over what merely happened to be true last time. */
    int restored = session_claim_desktop(view->server, view);
    if (restored >= 0 && !(have_rule && rule.desktop >= 0)) current_desktop = restored;
    /* Centred on its desktop — which IS one screen now, so this lands in the
     * middle of whichever monitor is showing it. Centred on that monitor's
     * size rather than the column's: a column is the primary monitor's shape,
     * and centring a window in it puts it below the middle of a shorter screen
     * and above the middle of a taller one. Nobody showing this desktop leaves
     * the column as the only size there is. */
    FwmOutput *mon = server_output_showing(view->server, current_desktop);
    int mon_w = mon ? mon->box.width  : view->server->screen_width;
    int mon_h = mon ? mon->box.height : view->server->screen_height;
    int cx = current_desktop * view->server->screen_width
           + (mon_w - initial_w) / 2;
    int cy = (mon_h - initial_h) / 2;
    
    view->x = cx;
    view->y = cy;
    
    view_set_size(view, view->width, view->height);
    server_place_node(view->server, &view->scene_tree->node, view->x, view->y);
    view_update_border_geometry(view);
    view_shadow_update(view);

    PhysicsBody *body = physics_sync_body(&view->server->physics, view->id, view->x, view->y, view->width, view->height, view->server->screen_width);

    /* No body means the window is past MAX_WINDOWS: it will map and be usable,
     * but physics, collisions and tiling will all skip it. That used to happen
     * in complete silence, leaving one inexplicably inert window; say it once,
     * through the same tray pill that reports config problems. */
    if (!body && !view->server->warned_window_limit) {
        view->server->warned_window_limit = 1;
        config_report_error(&view->server->config,
                            "window limit reached (%d) — new windows open without physics",
                            MAX_WINDOWS);
        wlr_log(WLR_ERROR, "MAX_WINDOWS (%d) reached; window %u has no physics body",
                MAX_WINDOWS, view->id);
        server_request_tray_redraw(view->server);
    }
    
    if (body) {
        body->shaped = 0;
        body->corner_mode = (view->server->desktop_mode[body->desktop_id] == DESKTOP_MODE_PHYSICS) ? CORNER_CHAMFER : CORNER_SHARP;
        /* Rule properties live on the physics body, not the view. */
        if (have_rule) {
            if (rule.nocollide >= 0) body->no_collide = rule.nocollide;
            if (rule.pin       >= 0) body->pinned     = rule.pin;
            /* Material: copied across as-is, NANs included, because NAN is
             * exactly what the body wants for "defer to the desktop". */
            body->rule_mass      = rule.mass;
            body->rule_gravity   = rule.gravity;
            body->rule_bounce    = rule.bounce;
            body->rule_friction  = rule.friction;
            body->rule_toughness = rule.toughness;
            body->rule_hardness  = rule.hardness;
        }
    }
    
    /* Publish to external panels BEFORE focusing, so the activation state that
     * server_focus_view pushes lands on an existing handle. */
    foreign_view_map(view);
    server_focus_view(view->server, view);

    int desktop = body ? body->desktop_id : current_desktop;
    if (view->server->desktop_mode[desktop] == DESKTOP_MODE_TILING) {
        bsp_insert(&view->server->bsp_roots[desktop], view->server->focused_view ? view->server->focused_view->id : 0, view->id);
        server_apply_tiling(view->server, desktop);
    } else if (view->server->desktop_mode[desktop] == DESKTOP_MODE_FLOATING) {
        /* Overlapping is the whole point of floating — shoving the new window
         * clear of the others would be the physics behaviour this mode exists
         * to switch off. */
        if (body) body->floating = 1;
    } else {
        physics_push_overlapping(&view->server->physics, view->id, 300.0);
    }

    /* A window may already be asking to open fullscreen, and until now nobody
     * was listening. The request arrives before the window exists to grant it
     * to: an X client sets _NET_WM_STATE_FULLSCREEN on the window while it is
     * still unmapped (EWMH's way of asking for an initial state), an xdg client
     * calls set_fullscreen before its first commit — and both land in a
     * request_fullscreen handler that finds no physics body yet and drops the
     * request on the floor. The window then opened as an ordinary one, its
     * default size, while the client believed it had the whole screen.
     *
     * That is how a game came apart on a resolution change: applying one makes
     * it build a fresh fullscreen window at the new mode, which is exactly this
     * path — it drew for a screen it had not been given, and Xwayland had no
     * screen-sized window to scale its mode into. Asked here, after the body,
     * the desktop and the tile are settled, so fullscreen geometry is the last
     * word on all three. */
    bool wants_fullscreen = view->type == FWM_VIEW_XDG
        ? view->xdg_toplevel->requested.fullscreen
        : view->xwl_surface->fullscreen;
    if (wants_fullscreen) {
        server_set_fullscreen(view->server, view, true, true);
    }

    /* Focus, tiling and sizing above may have re-enabled or repositioned
     * things; the window must stay hidden until its content is ready. */
    if (view->open_anim && view->open_hold > 0) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }

    server_request_tray_redraw(view->server);

    /* Last, so a subscriber that reacts by dispatching an action finds the
     * window already placed, tiled and focused rather than half-mapped. */
    ipc_emit_window(view->server->ipc, FWM_EV_WINDOW_OPEN, view);
}

void view_unmap(FwmView *view) {
    /* First, while the window is still whole: below this line its body, its
     * tile and its title are being taken apart. */
    ipc_emit_window(view->server->ipc, FWM_EV_WINDOW_CLOSE, view);

    foreign_view_unmap(view);
    /* A card on the desktop strip stands in for this window; it must go with
     * it, or the strip keeps offering a picture of something that is gone. */
    expo_forget_view(view->server, view);
    /* Before anything else: the snapshot lives in scene_tree, which is about to
     * go, and it holds a buffer lock the close ghost may want back. */
    view_stop_squash(view);
    view_jelly_stop(view);
    view_stop_spin(view);

    /* Which desktop to re-home the keyboard on, read before the body goes. */
    PhysicsBody *ub = physics_find_body(&view->server->physics, view->id);
    int home_desktop = ub ? ub->desktop_id
                          : server_active_desktop(view->server);

    group_remove(view->server, view); /* no-op when not grouped */
    physics_remove_body(&view->server->physics, view->id);
    
    for (int i = 0; i < 10; i++) {
        if (bsp_find(view->server->bsp_roots[i], view->id)) {
            bsp_remove(&view->server->bsp_roots[i], view->id);
            if (view->server->desktop_mode[i] == DESKTOP_MODE_TILING) {
                server_apply_tiling(view->server, i);
            }
        }
    }
    
    int was_focused = view->server->focused_view == view;
    if (was_focused) {
        view->server->focused_view = NULL;
    }
    if (view->server->last_touched_view == view) {
        view->server->last_touched_view = NULL;
    }
    if (view->server->interactive.view == view) {
        view->server->interactive.view = NULL;
    }
    /* The window the pointer was last measured against (implicit grab). */
    if (view->server->ptr_view == view) {
        view->server->ptr_view = NULL;
        view->server->ptr_surface = NULL;
        view->server->ptr_node_have = 0;
    }
    
    // Close animation: leave a snapshot of the last frame fading out (the
    // mirror of the map fade-in). The ghost takes over the buffer lock; the
    // physics tick fades it and frees it.
    if (view->last_buffer && view->server->config.decor.fade_in_ms > 0.0) {
        FwmGhost *ghost = calloc(1, sizeof(*ghost));
        if (ghost) {
            ghost->scene_buffer = wlr_scene_buffer_create(view->server->layer_windows, view->last_buffer);
        }
        if (ghost && ghost->scene_buffer) {
            // The raw buffer's top-left sits above/left of the xdg geometry
            // (CSD shadows) — compensate like the xdg scene helper does.
            // X11 surfaces have no geometry box: the buffer IS the window.
            struct wlr_box geo = {0};
            if (view->type == FWM_VIEW_XDG) {
                geo = view->xdg_toplevel->base->current.geometry;
            }
            ghost->buffer = view->last_buffer;
            view->last_buffer = NULL;
            ghost->x = view->x - geo.x;
            ghost->y = view->y - geo.y;
            /* A window broken by a collision collapses; one merely closed
             * fades, as it always has. */
            ghost->kind = view->dying ? GHOST_IMPLODE : GHOST_FADE;
            ghost->w = ghost->buffer ? ghost->buffer->width  : view->width;
            ghost->h = ghost->buffer ? ghost->buffer->height : view->height;
            wlr_scene_node_set_position(&ghost->scene_buffer->node,
                                        (int)ghost->x, (int)ghost->y);
            wlr_scene_node_raise_to_top(&ghost->scene_buffer->node);
            wl_list_insert(&view->server->ghosts, &ghost->link);
        } else {
            free(ghost);
        }
    }
    if (view->last_buffer) {
        wlr_buffer_unlock(view->last_buffer);
        view->last_buffer = NULL;
    }

    /* Before the tree: the shadow owns nodes inside it AND a struct of its
     * own, and destroying the tree first would leave the second to leak. */
    shadow_destroy(view->shadow);
    view->shadow = NULL;

    if (view->scene_tree) {
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
        if (view->type == FWM_VIEW_XDG) view->xdg_toplevel->base->data = NULL;
    }
    // Border rects and the open-animation cover were children of scene_tree —
    // destroyed with it.
    for (int i = 0; i < 4; i++) view->border[i] = NULL;
    view->open_cover = NULL;
    view->open_anim = 0;
    view->open_hold = 0;

    /* Only now, with this window's scene nodes gone, is it safe to ask what is
     * under the cursor. Without this the keyboard sits nowhere until the
     * pointer happens to move — closing the top window of a stack left the one
     * underneath unfocused even though the cursor was already over it. */
    if (was_focused) {
        server_refocus(view->server, home_desktop, view);
    }

    server_request_tray_redraw(view->server);
}
