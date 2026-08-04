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

    /* A tiled client that commits a size other than the one it was given
     * shifts where its neighbours belong — terminals do this on every resize,
     * rounding to whole character cells. Re-run the layout's positioning pass,
     * but only when the size actually moved: this runs on every commit. */
    {
        int cw, ch;
        view_committed_size(view, &cw, &ch);
        if (cw != view->aligned_w || ch != view->aligned_h) {
            view->aligned_w = cw;
            view->aligned_h = ch;
            PhysicsBody *pb = physics_find_body(&view->server->physics, view->id);
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
         * loop. */
        if (cw > 0 && ch > 0 && (cw != view->width || ch != view->height)) {
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
        // X11 configure carries position too; send screen coords (X clients
        // use them as global root coordinates for e.g. popup placement).
        double sx = view->x, sy = view->y;
        bool on_screen = server_world_to_screen(view->server, view->x, view->y, &sx, &sy);
        int isx = (int)lround(sx);
        int isy = (int)lround(sy);
        
        // Only allow the early-out if it's actually on-screen
        if (on_screen && view->last_sync_x == isx && view->last_sync_y == isy &&
            view->last_sync_w == width && view->last_sync_h == height) {
            return;
        }
        
        if (on_screen) {
            view->last_sync_x = isx;
            view->last_sync_y = isy;
        } else {
            // Invalidate the cache completely so it doesn't accidentally match later
            view->last_sync_x = -999999;
            view->last_sync_y = -999999;
        }
        
        view->last_sync_w = width;
        view->last_sync_h = height;
        
        wlr_xwayland_surface_configure(view->xwl_surface,
            (int16_t)isx, (int16_t)isy,
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
    // Mapped: the compositor owns the position, honor only the size.
    view->width = ev->width;
    view->height = ev->height;
    physics_sync_body(&view->server->physics, view->id, view->x, view->y,
                      view->width, view->height, view->server->screen_width);
    view_sync_position(view);
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
    view->last_sync_x = -999999;
    view->last_sync_y = -999999;
    view->last_sync_w = -1;
    view->last_sync_h = -1;

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
     * middle of whichever monitor is showing it. */
    int cx = current_desktop * view->server->screen_width
           + (view->server->screen_width - initial_w) / 2;
    int cy = (view->server->screen_height - initial_h) / 2;
    
    view->x = cx;
    view->y = cy;
    
    view_set_size(view, view->width, view->height);
    server_place_node(view->server, &view->scene_tree->node, view->x, view->y);
    view_update_border_geometry(view);

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
