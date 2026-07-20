#include "view.h"
#include "theme.h"
#include "server.h"
#include "physics.h"
#include "bsp.h"
#include "group.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

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

/* ── shell-agnostic accessors ─────────────────────────────────────────── */

struct wlr_surface *view_surface(FwmView *view) {
    if (view->type == FWM_VIEW_XDG) return view->xdg_toplevel->base->surface;
    return view->xwl_surface->surface; /* NULL until the X11 window associates */
}

const char *view_title(FwmView *view) {
    return view->type == FWM_VIEW_XDG ? view->xdg_toplevel->title
                                      : view->xwl_surface->title;
}

void view_set_size(FwmView *view, int width, int height) {
    if (view->type == FWM_VIEW_XDG) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
    } else {
        // X11 configure carries position too; send screen coords (X clients
        // use them as global root coordinates for e.g. popup placement).
        wlr_xwayland_surface_configure(view->xwl_surface,
            (int16_t)(view->x - view->server->camera_x), (int16_t)view->y,
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

void view_update_border_geometry(FwmView *view) {
    if (!view->border[0]) return;
    int bw = view->server->config.decor.border_width;
    int w, h;
    if (view->type == FWM_VIEW_XDG) {
        w = view->xdg_toplevel->base->current.geometry.width;
        h = view->xdg_toplevel->base->current.geometry.height;
    } else {
        struct wlr_surface *s = view->xwl_surface->surface;
        w = s ? s->current.width : 0;
        h = s ? s->current.height : 0;
    }
    if (w <= 0) w = view->width;
    if (h <= 0) h = view->height;

    // top, bottom, left, right — hugging the outside of the window
    wlr_scene_node_set_position(&view->border[0]->node, -bw, -bw);
    wlr_scene_rect_set_size(view->border[0], w + 2 * bw, bw);
    wlr_scene_node_set_position(&view->border[1]->node, -bw, h);
    wlr_scene_rect_set_size(view->border[1], w + 2 * bw, bw);
    wlr_scene_node_set_position(&view->border[2]->node, -bw, 0);
    wlr_scene_rect_set_size(view->border[2], bw, h);
    wlr_scene_node_set_position(&view->border[3]->node, w, 0);
    wlr_scene_rect_set_size(view->border[3], bw, h);
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
    server_request_tray_redraw(view->server);
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
    
    int current_desktop = view->server->target_camera_x / view->server->screen_width;
    int cx = current_desktop * view->server->screen_width + (view->server->screen_width - initial_w) / 2;
    int cy = (view->server->screen_height - initial_h) / 2;
    
    view->x = cx;
    view->y = cy;
    
    view_set_size(view, view->width, view->height);
    wlr_scene_node_set_position(&view->scene_tree->node, view->x - view->server->camera_x, view->y);
    view_update_border_geometry(view);

    PhysicsBody *body = physics_sync_body(&view->server->physics, view->id, view->x, view->y, view->width, view->height, view->server->screen_width);
    
    if (body) {
        body->shaped = 0;
        body->corner_mode = (view->server->desktop_mode[body->desktop_id] == DESKTOP_MODE_PHYSICS) ? CORNER_CHAMFER : CORNER_SHARP;
    }
    
    server_focus_view(view->server, view);
    
    int desktop = body ? body->desktop_id : current_desktop;
    if (view->server->desktop_mode[desktop] == DESKTOP_MODE_TILING) {
        bsp_insert(&view->server->bsp_roots[desktop], view->server->focused_view ? view->server->focused_view->id : 0, view->id);
        server_apply_tiling(view->server, desktop);
    } else {
        physics_push_overlapping(&view->server->physics, view->id, 300.0);
    }

    /* Focus, tiling and sizing above may have re-enabled or repositioned
     * things; the window must stay hidden until its content is ready. */
    if (view->open_anim && view->open_hold > 0) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }

    server_request_tray_redraw(view->server);
}

void view_unmap(FwmView *view) {
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
    
    if (view->server->focused_view == view) {
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
            wlr_scene_node_set_position(&ghost->scene_buffer->node,
                                        (int)ghost->x - view->server->camera_x, (int)ghost->y);
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

    server_request_tray_redraw(view->server);
}
