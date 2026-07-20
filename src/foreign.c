#include <stdlib.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>

#include "foreign.h"
#include "server.h"
#include "view.h"
#include "physics.h"

static void handle_request_activate(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, ftl_request_activate);
    (void)data;
    /* A panel click on a window living on another desktop is meaningless
     * unless the camera goes there — unlike xdg-activation, this IS a direct
     * user action on that window, so it always pans. */
    PhysicsBody *pb = physics_find_body(&view->server->physics, view->id);
    if (pb && pb->desktop_id >= 0 && pb->desktop_id < 10) {
        view->server->target_camera_x = pb->desktop_id * view->server->screen_width;
        view->server->cam_free = 0;
    }
    server_focus_view(view->server, view);
}

static void handle_request_close(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, ftl_request_close);
    (void)data;
    view_send_close(view);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
    FwmView *view = wl_container_of(listener, view, ftl_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    server_set_fullscreen(view->server, view, event->fullscreen, true);
}

void foreign_init(FwmServer *server) {
    server->foreign_toplevel = wlr_foreign_toplevel_manager_v1_create(server->wl_display);
    if (!server->foreign_toplevel) {
        wlr_log(WLR_ERROR, "failed to create foreign-toplevel manager");
    }
}

void foreign_view_map(FwmView *view) {
    FwmServer *server = view->server;
    if (!server->foreign_toplevel || view->ftl) return;

    view->ftl = wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel);
    if (!view->ftl) return;

    foreign_view_title_changed(view);

    view->ftl_request_activate.notify = handle_request_activate;
    wl_signal_add(&view->ftl->events.request_activate, &view->ftl_request_activate);
    view->ftl_request_close.notify = handle_request_close;
    wl_signal_add(&view->ftl->events.request_close, &view->ftl_request_close);
    view->ftl_request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&view->ftl->events.request_fullscreen, &view->ftl_request_fullscreen);
}

void foreign_view_unmap(FwmView *view) {
    if (!view->ftl) return;
    wl_list_remove(&view->ftl_request_activate.link);
    wl_list_remove(&view->ftl_request_close.link);
    wl_list_remove(&view->ftl_request_fullscreen.link);
    wlr_foreign_toplevel_handle_v1_destroy(view->ftl);
    view->ftl = NULL;
}

void foreign_view_title_changed(FwmView *view) {
    if (!view->ftl) return;
    const char *title = view_title(view);
    const char *app_id = view_app_id(view);
    if (title)  wlr_foreign_toplevel_handle_v1_set_title(view->ftl, title);
    if (app_id) wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, app_id);
}

void foreign_view_set_activated(FwmView *view, bool activated) {
    if (!view->ftl) return;
    wlr_foreign_toplevel_handle_v1_set_activated(view->ftl, activated);
}

void foreign_view_set_fullscreen(FwmView *view, bool fullscreen) {
    if (!view->ftl) return;
    wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftl, fullscreen);
}
