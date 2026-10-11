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

/* The planes a window can stand in: back, middle and front, one behind the
 * other. A window in front is drawn over every window behind it, however
 * recently that one was raised — a raise lifts a window within its own plane —
 * and windows in different planes pass through one another: a window only
 * meets the windows in its own (PhysicsBody.layer). The liquid lies between
 * the back plane and the middle one, so it runs over a window at the back,
 * stands round one in the middle, and runs behind one in front.
 *
 * Until somebody puts it somewhere, a window is in the middle on a desktop the
 * physics or floating mode has, and in front on a tiling one — where the tiles
 * stand before the liquid like the glass of an aquarium. A dialog that nobody
 * put anywhere stands with the window it belongs to, since it belongs over it.
 * Fullscreen draws a window in front whatever its plane, for as long as it
 * lasts: it owns the glass it is shown on. */

#include "server.h"
#include "view.h"

#include <string.h>

#include <wlr/types/wlr_scene.h>

#include "physics.h"

int server_view_plane(FwmServer *server, FwmView *view) {
    /* Up through the dialogs to the first window somebody placed, or to the
     * window at the top; bounded, like every walk up view_parent. */
    for (int depth = 0; view && depth < 16; depth++) {
        if (view->plane != PLANE_AUTO) return view->plane;
        FwmView *parent = view_parent(view);
        if (!parent) break;
        view = parent;
    }
    if (!view) return PLANE_MIDDLE;
    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    int d = b ? b->desktop_id : -1;
    bool tiling = d >= 0 && d < FWM_DESKTOPS
               && server->desktop_mode[d] == DESKTOP_MODE_TILING;
    return tiling ? PLANE_FRONT : PLANE_MIDDLE;
}

const char *plane_name(int plane) {
    switch (plane) {
    case PLANE_BACK:   return "back";
    case PLANE_MIDDLE: return "middle";
    case PLANE_FRONT:  return "front";
    default:           return "auto";
    }
}

int plane_parse(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "back") == 0)   return PLANE_BACK;
    if (strcmp(s, "middle") == 0) return PLANE_MIDDLE;
    if (strcmp(s, "front") == 0)  return PLANE_FRONT;
    if (strcmp(s, "auto") == 0)   return PLANE_AUTO;
    return -1;
}

/* Into the tree it is drawn in, and the physics told which windows it meets.
 * A window coming into a tree comes in on top of it, as a raise would put it,
 * with its dialogs over it and a real fullscreen window over them. */
void server_view_plane_sync(FwmServer *server, FwmView *view) {
    int plane = server_view_plane(server, view);
    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    if (b) b->layer = plane - PLANE_MIDDLE;
    if (!view->scene_tree) return;

    struct wlr_scene_tree *want =
        b && b->fullscreen   ? server->layer_front
      : plane == PLANE_BACK  ? server->layer_back
      : plane == PLANE_FRONT ? server->layer_front
      :                        server->layer_windows;
    if (view->scene_tree->node.parent == want) return;
    wlr_scene_node_reparent(&view->scene_tree->node, want);
    server_restack_children(server, view);
    server_restack_fullscreen(server);
}

/* Once a tick, before the physics step: a window nobody placed changes plane
 * when its desktop changes mode or it is carried onto another, and a dialog
 * when the window it belongs to does — there is no one moment to catch that
 * in, and it costs a walk of the window list. */
void server_planes_sync(FwmServer *server) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link)
        server_view_plane_sync(server, view);
}

void server_view_set_plane(FwmServer *server, FwmView *view, int plane) {
    view->plane = plane;
    server_view_plane_sync(server, view);
    /* Its dialogs that nobody placed go with it, now rather than next tick. */
    FwmView *v;
    wl_list_for_each(v, &server->views, link)
        if (v != view && v->plane == PLANE_AUTO && view_parent(v))
            server_view_plane_sync(server, v);
}
