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
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
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
    struct wl_list link;              /* FwmServer.xwl_unmanaged */
    /* Where the surface is in the WORLD, and which desktop that puts it on.
     *
     * The client only ever speaks screen coordinates — the X root window is
     * one screen, not the ten-column strip — so what it says is read against
     * the camera at the moment it says it and kept as a world position from
     * then on. Without this the surface was nailed to the screen: switching
     * desktops slid every window away and left a fullscreen game hanging over
     * all ten of them. */
    double wx, wy;
    int desktop;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener destroy;
    struct wl_listener set_geometry;
    struct wl_listener set_override_redirect;
    struct wl_listener map;
    struct wl_listener unmap;
};

/* Hand the keyboard back to the focused window after an unmanaged surface that
 * had it goes away. Nothing to do in the ordinary case, where the keyboard was
 * never taken.
 *
 * Nothing is said to X on the way out either, for the same reason as on the
 * way in: the X focus was never ours to move for an override-redirect
 * window. */
static void xwl_or_drop_focus(FwmServer *server, struct wlr_xwayland_surface *xs) {
    if (server->focused_unmanaged != xs) return;
    server->focused_unmanaged = NULL;
    if (server->focused_view) {
        server_keyboard_enter(server, view_surface(server->focused_view));
    } else {
        wlr_seat_keyboard_clear_focus(server->seat);
    }
}

/* Read the position the client just gave us — screen coordinates — and keep it
 * as a world one. Only meaningful while the surface's desktop is the one on
 * screen: with it panned away there is no screen point to read the client's
 * numbers against, and the world position we already have is the truth. */
static void xwl_or_note_geometry(struct FwmXwlUnmanaged *u) {
    double wx, wy;
    if (!server_screen_to_world(u->server, u->xs->x, u->xs->y, &wx, &wy)) return;
    u->wx = wx;
    u->wy = wy;
    u->desktop = server_desktop_at_x(u->server, wx);
}

static void xwl_or_place(struct FwmXwlUnmanaged *u) {
    if (u->tree) server_place_node(u->server, &u->tree->node, u->wx, u->wy);
}

void server_xwl_unmanaged_place(FwmServer *server) {
    struct FwmXwlUnmanaged *u;
    wl_list_for_each(u, &server->xwl_unmanaged, link) xwl_or_place(u);
}

bool server_xwl_unmanaged_refocus(FwmServer *server, int desktop) {
    struct FwmXwlUnmanaged *u;
    wl_list_for_each(u, &server->xwl_unmanaged, link) {
        if (!u->tree) continue;   /* not mapped: nothing on screen to type into */
        if (u->desktop != desktop) continue;
        if (!wlr_xwayland_surface_override_redirect_wants_focus(u->xs)) continue;
        /* Through server_focus_view(NULL) first, so the window that had the
         * focus is told it lost it — otherwise a window on the desktop we just
         * left keeps its lit border and, for an X client, its idea of being
         * frontmost. */
        server_focus_view(server, NULL);
        server_keyboard_enter(server, u->xs->surface);
        server->focused_unmanaged = u->xs;
        return true;
    }
    return false;
}

static void xwl_or_handle_map(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, map);
    u->tree = wlr_scene_tree_create(u->server->layer_windows);
    if (!u->tree) return;
    if (!wlr_scene_surface_create(u->tree, u->xs->surface)) {
        wlr_scene_node_destroy(&u->tree->node);
        u->tree = NULL;
        return;
    }
    xwl_or_note_geometry(u);
    xwl_or_place(u);
    wlr_scene_node_raise_to_top(&u->tree->node);

    /* Most unmanaged surfaces are menus and tooltips, which take pointer
     * events and want nothing to do with the keyboard. Some are not: a Wine
     * game's own fullscreen window arrives here (winex11 makes it
     * override-redirect to keep the window manager out of it), and so do the
     * dialogs of X clients that set an input hint on a popup. Those are, from
     * the user's side, simply the thing they are looking at, and without the
     * keyboard the window renders while every key goes somewhere else.
     *
     * The keyboard goes to the surface directly, not through
     * server_focus_view: there is no view to focus, and inventing one would
     * give a menu a physics body and a place in the layout. So focused_view
     * stays where it was — its border stays lit, which is the honest picture:
     * the window is still the one in front, this is a surface on top of it.
     * The seat is what actually decides where keys land, and it is pointed
     * here until the surface unmaps or focus moves on its own.
     *
     * Only the seat is told. Restacking the window in X is not ours to do —
     * wlroots asserts against it for override-redirect surfaces (xwm.c), which
     * is the whole point of them: the client stacks itself and the window
     * manager stays out. The scene node above is where our stacking happens. */
    if (wlr_xwayland_surface_override_redirect_wants_focus(u->xs)) {
        /* The seat, and only the seat. wlr_xwayland_surface_activate returns
         * without doing anything for an override-redirect surface (xwm.c), so
         * there is no X input focus to be had here and nothing to ask for: the
         * X window keeps whatever focus X gives it, usually PointerRoot.
         *
         * Pointing the seat here is still what decides everything, because it
         * is what makes Xwayland receive the key events at all. Where they go
         * inside X is then X's business — the client's own keyboard grab, or
         * the pointer being inside the window, which for something covering
         * the whole screen it is. This is the same thing sway does with
         * override-redirect surfaces, and it is what the wlroots hint above is
         * for. */
        server_keyboard_enter(u->server, u->xs->surface);
        u->server->focused_unmanaged = u->xs;
    }
}

static void xwl_or_handle_unmap(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, unmap);
    xwl_or_drop_focus(u->server, u->xs);
    if (u->tree) {
        wlr_scene_node_destroy(&u->tree->node);
        u->tree = NULL;
    }
}

static void xwl_or_handle_set_geometry(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, set_geometry);
    if (!u->tree) return;
    /* Only believe the client's coordinates while its desktop is the one being
     * shown; see xwl_or_note_geometry. */
    if (server_output_showing(u->server, u->desktop)) xwl_or_note_geometry(u);
    xwl_or_place(u);
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

/* Everything the unmanaged surface owns, given up. Shared by the destroy event
 * and by the surface changing hands to a managed view, which is the same
 * teardown minus the client going away. */
static void xwl_or_destroy(struct FwmXwlUnmanaged *u) {
    xwl_or_drop_focus(u->server, u->xs);
    if (u->tree) wlr_scene_node_destroy(&u->tree->node);
    wl_list_remove(&u->map.link);
    wl_list_remove(&u->unmap.link);
    wl_list_remove(&u->associate.link);
    wl_list_remove(&u->dissociate.link);
    wl_list_remove(&u->set_geometry.link);
    wl_list_remove(&u->set_override_redirect.link);
    wl_list_remove(&u->destroy.link);
    wl_list_remove(&u->link);
    free(u);
}

static void xwl_or_handle_destroy(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, destroy);
    xwl_or_destroy(u);
}

/* The mirror of xwl_handle_set_override_redirect in view.c: a surface that
 * stops being override-redirect is a window asking to be managed after all —
 * Wine clears the flag when what it created unmanaged becomes an ordinary
 * top-level. It gets a real view, with the physics body, borders and focus
 * that come with one. */
static void xwl_or_handle_set_override_redirect(struct wl_listener *listener, void *data) {
    struct FwmXwlUnmanaged *u = wl_container_of(listener, u, set_override_redirect);
    struct wlr_xwayland_surface *xs = u->xs;
    if (xs->override_redirect) return;

    FwmServer *server = u->server;
    xwl_or_destroy(u);
    view_xwl_adopt(view_xwl_create(xs, server));
}

void server_xwl_unmanaged_create(FwmServer *server, struct wlr_xwayland_surface *xs) {
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
    u->set_override_redirect.notify = xwl_or_handle_set_override_redirect;
    wl_signal_add(&xs->events.set_override_redirect, &u->set_override_redirect);
    u->destroy.notify = xwl_or_handle_destroy;
    wl_signal_add(&xs->events.destroy, &u->destroy);
    wl_list_insert(&server->xwl_unmanaged, &u->link);

    /* Arriving from a view rather than from new_surface: the wlr_surface is
     * already there and may already be on screen, so the associate and map
     * events that would have set this up have both been and gone. */
    if (xs->surface) {
        xwl_or_handle_associate(&u->associate, NULL);
        if (xs->surface->mapped) xwl_or_handle_map(&u->map, NULL);
    }
}

static void handle_xwl_ready(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, xwl_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);

    /* The root window's cursor. We do draw the pointer ourselves, but an X
     * client that never defines a cursor of its own (GLFW calls
     * XUndefineCursor whenever no custom cursor is set — Minecraft's whole
     * menu is like that) inherits the root's, and Xwayland then asks us for a
     * cursor with no surface at all: the pointer simply vanishes over the
     * window. Defining the root cursor gives those clients an arrow to
     * inherit. Clients that do set their own are unaffected. */
    struct wlr_xcursor *xcursor =
        wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1);
    if (xcursor && xcursor->image_count > 0) {
        struct wlr_xcursor_image *image = xcursor->images[0];
        struct wlr_buffer *buffer = wlr_xcursor_image_get_buffer(image);
        if (buffer) {
            wlr_xwayland_set_cursor(server->xwayland, buffer,
                                    (int32_t)image->hotspot_x,
                                    (int32_t)image->hotspot_y);
        }
    }

    // Spawned children inherit DISPLAY, so binds can launch X11 apps.
    setenv("DISPLAY", server->xwayland->display_name, true);
}

static void handle_xwl_new_surface(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, xwl_new_surface);
    struct wlr_xwayland_surface *xs = data;
    if (xs->override_redirect) {
        server_xwl_unmanaged_create(server, xs);
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
    //
    // THE output, not the first one: the box is in layout coordinates, and a
    // box at the layout origin is the primary monitor. A menu opened on the
    // second screen was therefore constrained to the first one and slid all
    // the way there — the whole width of the primary monitor away from the
    // window it belongs to. The monitor the parent is standing on is the one
    // the menu must stay inside.
    FwmServer *server = p->server;
    struct wlr_scene_tree *tree = p->popup->base->data;
    if (tree && tree->node.parent) {
        int px = 0, py = 0;
        wlr_scene_node_coords(&tree->node.parent->node, &px, &py);

        /* The parent's top-left can hang off its own screen (a window pushed
         * against the left edge, a nested submenu); its middle is the more
         * honest answer to "which screen is this on", and the monitor the user
         * is at is the last resort. */
        FwmOutput *mon = server_output_at(server, px, py);
        if (!mon && p->popup->parent) {
            mon = server_output_at(server,
                                   px + p->popup->parent->current.width / 2.0,
                                   py + p->popup->parent->current.height / 2.0);
        }
        if (!mon) mon = server_active_output(server);

        struct wlr_box box = {
            .x = (mon ? mon->box.x : 0) - px,
            .y = (mon ? mon->box.y : 0) - py,
            .width  = mon ? mon->box.width  : server->screen_width,
            .height = mon ? mon->box.height : server->screen_height,
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
