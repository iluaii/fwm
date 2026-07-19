#include "view.h"
#include "server.h"
#include "physics.h"
#include "bsp.h"
#include <stdlib.h>
#include <stdio.h>

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
    // client pick its own initial size.
    if (view->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
    }
    // Track the actual committed surface size so borders hug the real window.
    view_update_border_geometry(view);
}

/* ── focus border ─────────────────────────────────────────────────────── */

void view_update_border_geometry(FwmView *view) {
    if (!view->border[0]) return;
    int bw = view->server->config.decor.border_width;
    int w = view->xdg_toplevel->base->current.geometry.width;
    int h = view->xdg_toplevel->base->current.geometry.height;
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

static void set_opacity_iter(struct wlr_scene_buffer *buffer, int sx, int sy, void *user) {
    (void)sx; (void)sy;
    wlr_scene_buffer_set_opacity(buffer, *(double *)user > 1.0 ? 1.0f : (float)*(double *)user);
}

void view_set_opacity(FwmView *view, double opacity) {
    if (!view->scene_tree) return;
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, set_opacity_iter, &opacity);
}

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
    // Under wlroots-0.20, check the requested state on the toplevel object.
    // A client-initiated fullscreen request maps to real (whole-output) fullscreen.
    bool fullscreen = view->xdg_toplevel->requested.fullscreen;
    server_set_fullscreen(view->server, view, fullscreen, true);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, set_title);
    server_request_tray_redraw(view->server);
}

FwmView *view_create(struct wlr_xdg_toplevel *toplevel, struct FwmServer *server) {
    FwmView *view = calloc(1, sizeof(FwmView));
    if (!view) return NULL;
    
    static uint32_t next_id = 1;
    view->id = next_id++;
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
    wl_list_remove(&view->link);
    
    if (view->xdg_toplevel->base->surface->mapped) {
        view_unmap(view);
    }
    
    free(view);
}

void view_map(FwmView *view) {
    // In wlroots-0.20, use wlr_scene_xdg_surface_create on xdg_toplevel->base.
    // Parented to the windows layer (below the overlay layer) so raising a
    // view to top on focus can never cover the tray/hints/welcome overlays.
    view->scene_tree = wlr_scene_xdg_surface_create(view->server->layer_windows, view->xdg_toplevel->base);
    if (!view->scene_tree) {
        fprintf(stderr, "Failed to create scene tree for view\n");
        return;
    }
    
    view->scene_tree->node.data = view;

    // Focus border rects (created disabled-color as "inactive"; focus recolors).
    int bw = view->server->config.decor.border_width;
    if (bw > 0) {
        for (int i = 0; i < 4; i++) {
            view->border[i] = wlr_scene_rect_create(view->scene_tree, 1, 1,
                                                    view->server->config.decor.col_inactive);
        }
    }

    // Fade-in from fully transparent.
    if (view->server->config.decor.fade_in_ms > 0.0) {
        view->fade_anim = 1;
        view->fade_t = 0.0;
        view_set_opacity(view, 0.0);
    }

    int initial_w = view->xdg_toplevel->base->current.geometry.width;
    int initial_h = view->xdg_toplevel->base->current.geometry.height;
    if (initial_w <= 0) initial_w = view->server->screen_width / 2;
    if (initial_h <= 0) initial_h = view->server->screen_height / 2;
    
    view->width = initial_w;
    view->height = initial_h;
    
    int current_desktop = view->server->target_camera_x / view->server->screen_width;
    int cx = current_desktop * view->server->screen_width + (view->server->screen_width - initial_w) / 2;
    int cy = (view->server->screen_height - initial_h) / 2;
    
    view->x = cx;
    view->y = cy;
    
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
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
    
    server_request_tray_redraw(view->server);
}

void view_unmap(FwmView *view) {
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
    
    if (view->scene_tree) {
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
    }
    // Border rects were children of scene_tree — destroyed with it.
    for (int i = 0; i < 4; i++) view->border[i] = NULL;
    view->fade_anim = 0;

    server_request_tray_redraw(view->server);
}
