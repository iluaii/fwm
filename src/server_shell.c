/* Shell surfaces: xdg-toplevel and popups, server-side decorations, and the
 * Xwayland override-redirect path. Split out of server.c; see
 * server_internal.h for why the handle_* callbacks are not static. */
#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "theme.h"
#include "layer.h"
#include "lock.h"
#include "foreign.h"
#include "ipc.h"
#include "session.h"
#include <signal.h>
#include "ui/tray.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include "server_internal.h"

struct FwmDecoration {
    struct wlr_xdg_toplevel_decoration_v1 *deco;
    struct wl_listener destroy;
    struct wl_listener commit;
};

static void deco_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct FwmDecoration *d = wl_container_of(listener, d, destroy);
    wl_list_remove(&d->destroy.link);
    wl_list_remove(&d->commit.link);
    free(d);
}

static void deco_handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct FwmDecoration *d = wl_container_of(listener, d, commit);
    // set_mode() schedules a configure and asserts the surface is initialized,
    // so wait for the toplevel's initial commit before forcing server-side.
    if (!d->deco->toplevel->base->initialized) return;
    wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
        WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    // Only needs to happen once; stop watching commits (re-init so the destroy
    // handler can still safely remove the link).
    wl_list_remove(&d->commit.link);
    wl_list_init(&d->commit.link);
}

static void handle_new_toplevel_decoration(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    // Force server-side decorations. We draw no titlebar/border ourselves, so
    // the client omits its client-side decoration (titlebar, close button, …)
    // and the window renders borderless.
    struct FwmDecoration *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->deco = deco;
    d->destroy.notify = deco_handle_destroy;
    wl_signal_add(&deco->events.destroy, &d->destroy);
    d->commit.notify = deco_handle_commit;
    wl_signal_add(&deco->toplevel->base->surface->events.commit, &d->commit);
}

static void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;
    view_create(toplevel, server);
}

/* ── Xwayland ─────────────────────────────────────────────────────────────
 * Managed X11 windows become regular FwmViews (view_xwl_create). Override-
 * redirect surfaces (menus, tooltips, DnD icons) position themselves in X11
 * root coordinates — which match our screen coordinates, because managed X
 * windows are always configured at screen coords (view_set_size). They get a
 * bare scene surface, no physics/borders/focus. */

struct FwmXwlUnmanaged {
    struct wlr_xwayland_surface *xs;
    FwmServer *server;
    struct wlr_scene_tree *tree;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener destroy;
    struct wl_listener set_geometry;
    struct wl_listener map;
    struct wl_listener unmap;
};

static void xwl_or_handle_map(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, map);
    u->tree = wlr_scene_tree_create(u->server->layer_windows);
    if (!u->tree) return;
    if (!wlr_scene_surface_create(u->tree, u->xs->surface)) {
        wlr_scene_node_destroy(&u->tree->node);
        u->tree = NULL;
        return;
    }
    wlr_scene_node_set_position(&u->tree->node, u->xs->x, u->xs->y);
    wlr_scene_node_raise_to_top(&u->tree->node);
}

static void xwl_or_handle_unmap(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, unmap);
    if (u->tree) {
        wlr_scene_node_destroy(&u->tree->node);
        u->tree = NULL;
    }
}

static void xwl_or_handle_set_geometry(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, set_geometry);
    if (u->tree) {
        wlr_scene_node_set_position(&u->tree->node, u->xs->x, u->xs->y);
    }
}

static void xwl_or_handle_associate(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, associate);
    wl_signal_add(&u->xs->surface->events.map, &u->map);
    wl_signal_add(&u->xs->surface->events.unmap, &u->unmap);
}

static void xwl_or_handle_dissociate(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, dissociate);
    wl_list_remove(&u->map.link);   wl_list_init(&u->map.link);
    wl_list_remove(&u->unmap.link); wl_list_init(&u->unmap.link);
}

static void xwl_or_handle_destroy(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, destroy);
    if (u->tree) wlr_scene_node_destroy(&u->tree->node);
    wl_list_remove(&u->map.link);
    wl_list_remove(&u->unmap.link);
    wl_list_remove(&u->associate.link);
    wl_list_remove(&u->dissociate.link);
    wl_list_remove(&u->set_geometry.link);
    wl_list_remove(&u->destroy.link);
    free(u);
}

static void xwl_unmanaged_create(FwmServer *server, struct wlr_xwayland_surface *xs) {
    struct FwmXwlUnmanaged *u = calloc(1, sizeof(*u));
    if (!u) return;
    u->xs = xs;
    u->server = server;
    u->map.notify = xwl_or_handle_map;     wl_list_init(&u->map.link);
    u->unmap.notify = xwl_or_handle_unmap; wl_list_init(&u->unmap.link);
    u->associate.notify = xwl_or_handle_associate;
    wl_signal_add(&xs->events.associate, &u->associate);
    u->dissociate.notify = xwl_or_handle_dissociate;
    wl_signal_add(&xs->events.dissociate, &u->dissociate);
    u->set_geometry.notify = xwl_or_handle_set_geometry;
    wl_signal_add(&xs->events.set_geometry, &u->set_geometry);
    u->destroy.notify = xwl_or_handle_destroy;
    wl_signal_add(&xs->events.destroy, &u->destroy);
}

static void handle_xwl_ready(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, xwl_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
    // Spawned children inherit DISPLAY, so binds can launch X11 apps.
    // (No wlr_xwayland_set_cursor: the compositor draws the pointer itself,
    // the X-side cursor image is never shown.)
    setenv("DISPLAY", server->xwayland->display_name, true);
}

static void handle_xwl_new_surface(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, xwl_new_surface);
    struct wlr_xwayland_surface *xs = data;
    if (xs->override_redirect) {
        xwl_unmanaged_create(server, xs);
    } else {
        view_xwl_create(xs, server);
    }
}

// xdg popups (context/dropdown menus). wlr_scene_xdg_surface_create on the
// toplevel does NOT cover its popups — each popup needs its own scene tree
// parented into the parent surface's tree, or the menu is invisible while
// its input still works (clicks land through view_at's scene lookup).
struct FwmPopup {
    struct wlr_xdg_popup *popup;
    FwmServer *server;
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void popup_handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct FwmPopup *p = wl_container_of(listener, p, commit);
    // Everything popup-positioning must wait for the initial commit: before it
    // the xdg_surface is uninitialized and unconstrain/schedule_configure
    // assert inside wlroots (that abort looked like "rmb crashes fwm").
    if (!p->popup->base->initial_commit) return;

    // Keep the menu on screen: give the popup the whole output as its
    // constraint box, expressed in the parent's coordinate space.
    FwmServer *server = p->server;
    struct wlr_scene_tree *tree = p->popup->base->data;
    if (tree && tree->node.parent) {
        int px = 0, py = 0;
        wlr_scene_node_coords(&tree->node.parent->node, &px, &py);
        struct wlr_box box = {
            .x = -px,
            .y = -py,
            .width = server->screen_width,
            .height = server->screen_height,
        };
        wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
    }

    // The compositor must answer the popup's initial commit with a configure,
    // same contract as for toplevels (view.c).
    wlr_xdg_surface_schedule_configure(p->popup->base);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct FwmPopup *p = wl_container_of(listener, p, destroy);
    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->destroy.link);
    free(p);
}

static void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_xdg_popup);
    struct wlr_xdg_popup *popup = data;

    if (!popup->parent) return;
    // parent->data is the parent's scene tree: set in view_map for toplevels
    // and below for nested popups. A layer surface is not an xdg_surface, so
    // layer.c stashes its popup tree on the wlr_surface instead.
    struct wlr_scene_tree *parent_tree = NULL;
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (parent && parent->data) {
        parent_tree = parent->data;
    } else if (popup->parent->data) {
        parent_tree = popup->parent->data;
    }
    // NULL means the parent isn't mapped into the scene — nowhere to draw it.
    if (!parent_tree) return;

    struct wlr_scene_tree *tree = wlr_scene_xdg_surface_create(parent_tree, popup->base);
    if (!tree) return;
    popup->base->data = tree;

    struct FwmPopup *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->popup = popup;
    p->server = server;
    p->commit.notify = popup_handle_commit;
    wl_signal_add(&popup->base->surface->events.commit, &p->commit);
    p->destroy.notify = popup_handle_destroy;
    wl_signal_add(&popup->events.destroy, &p->destroy);
}


/* Called once from server_init(), after the objects these listeners hang off
 * exist. The decoration manager is created here because nothing outside this
 * file ever touches it. */
void server_shell_register(FwmServer *server) {
    server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = handle_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

    // Advertise xdg-decoration and force server-side mode so clients drop their
    // client-side titlebars (we draw none) and windows render borderless.
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration =
        wlr_xdg_decoration_manager_v1_create(server->wl_display);
    server->new_toplevel_decoration.notify = handle_new_toplevel_decoration;
    wl_signal_add(&xdg_decoration->events.new_toplevel_decoration, &server->new_toplevel_decoration);

    if (server->xwayland) {
        server->xwl_ready.notify = handle_xwl_ready;
        wl_signal_add(&server->xwayland->events.ready, &server->xwl_ready);
        server->xwl_new_surface.notify = handle_xwl_new_surface;
        wl_signal_add(&server->xwayland->events.new_surface, &server->xwl_new_surface);
    }
}
