/* Desktop modes: entering and leaving BSP tiling, floating, and moving a view
 * between the desktops of the world strip. Split out of server.c; see
 * server_internal.h. */
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

/* Switch one desktop between physics and tiling. Extracted so the per-desktop
 * bind and the all-desktops bind cannot drift apart — the restore path in
 * particular (saved geometry, cleared tile state, the outward shove) is easy to
 * get subtly wrong twice. */
static void desktop_enter_tiling(FwmServer *server, int d) {
    /* Remember where physics had put things, so leaving tiling can undo it. */
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (b && b->desktop_id == d && !b->tiling_saved) {
            b->sav_x = b->x; b->sav_y = b->y;
            b->sav_w = b->width; b->sav_h = b->height;
            b->tiling_saved = 1;
        }
    }

    bsp_free(server->bsp_roots[d]);
    server->bsp_roots[d] = NULL;

    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (b && !b->shaped && !b->fullscreen && b->desktop_id == d) {
            bsp_insert(&server->bsp_roots[d], 0, view->id);
        }
    }
    server_apply_tiling(server, d);
}

static void desktop_leave_tiling(FwmServer *server, int d) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (!b || b->desktop_id != d) continue;

        if (b->tiling_saved) {
            b->x = b->sav_x; b->y = b->sav_y;
            b->width = b->sav_w; b->height = b->sav_h;
            b->tiling_saved = 0;
        } else {
            b->width = server->screen_width / 2;
            b->height = server->screen_height / 2;
            b->x = d * server->screen_width + (server->screen_width - b->width) / 2;
            b->y = (server->screen_height - b->height) / 2;
        }
        b->vx = 0; b->vy = 0; b->flying = 0;
        b->tiled = 0;
        view->tile_anim = 0;

        view->x = b->x; view->y = b->y;
        view->width = b->width; view->height = b->height;
        view_set_size(view, view->width, view->height);
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node,
                                        (int)lround(view->x - server->camera_x),
                                        (int)lround(view->y));
        }
    }

    bsp_free(server->bsp_roots[d]);
    server->bsp_roots[d] = NULL;
}

/* Floating is the "normal desktop environment" mode: windows stay exactly
 * where you drop them and overlap freely. Both halves of that are already
 * expressible per window (pinned = immovable anchor, no_collide = passes
 * through other windows), so the desktop mode just raises one flag and lets
 * physics.c apply the same two rules it already knows. */
static void desktop_set_floating(FwmServer *server, int d, int on) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (!b || b->desktop_id != d) continue;
        b->floating = on;
        if (on) { b->vx = 0; b->vy = 0; b->flying = 0; }
    }
}

/* Scatter everything on the desktop, so switching back to physics reads as the
 * world waking up rather than as nothing having happened. */
static void desktop_shove(FwmServer *server, int d) {
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        if (!b || b->desktop_id != d) continue;
        double angle = ((double)(view->id % 628)) / 100.0;
        b->vx = cos(angle) * 200.0;
        b->vy = sin(angle) * 200.0;
        b->flying = 1;
    }
}

/* Move one desktop between physics, tiling and floating.
 *
 * Written as leave-old then enter-new rather than as a switch over pairs: with
 * three modes there are six transitions, and the pairwise form is where the
 * restore path (saved geometry, cleared tile state, the outward shove) starts
 * getting subtly wrong in some of them. */
void server_set_desktop_mode(FwmServer *server, int d, int mode) {
    if (d < 0 || d >= 10) return;
    int old = server->desktop_mode[d];
    if (old == mode) return;

    if (old == DESKTOP_MODE_TILING)        desktop_leave_tiling(server, d);
    else if (old == DESKTOP_MODE_FLOATING) desktop_set_floating(server, d, 0);

    server->desktop_mode[d] = mode;

    if (mode == DESKTOP_MODE_TILING)        desktop_enter_tiling(server, d);
    else if (mode == DESKTOP_MODE_FLOATING) desktop_set_floating(server, d, 1);
    else                                    desktop_shove(server, d);

    server_request_tray_redraw(server);
    ipc_emit_mode(server->ipc, d, mode);
}

/* Kept as the entry point for the toggle_tiling bind: physics <-> tiling, with
 * floating collapsing to tiling so the key never becomes a no-op. */
void server_toggle_desktop_tiling(FwmServer *server, int d) {
    server_set_desktop_mode(server, d,
        server->desktop_mode[d] == DESKTOP_MODE_TILING
            ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_TILING);
}

/* Send a window to another desktop.
 *
 * In physics and floating modes you can already drag a window across the
 * desktop boundary, but a tiled window is a static body owned by the layout —
 * dragging cannot take it out, which left tiling with no way to move a window
 * off its desktop at all. This works the same in all three modes so the bind
 * does not silently mean different things depending on where you are. */
void server_move_view_to_desktop(FwmServer *server, FwmView *view, int target,
                                        int follow) {
    if (!view || target < 0 || target >= 10 || server->screen_width <= 0) return;

    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    int src = b ? b->desktop_id : view->x / server->screen_width;
    if (src == target) return;

    /* A tab-stack is a single stack on a single desktop; pulling a member out
     * of it is the only sensible reading of "move this window away". */
    if (view->group) group_remove(server, view);

    /* Leave the old layout before the coordinates move, or the re-tile would
     * lay out the window that is on its way out. */
    if (server->desktop_mode[src] == DESKTOP_MODE_TILING) {
        bsp_remove(&server->bsp_roots[src], view->id);
        server_apply_tiling(server, src);
    }

    if (b) {
        /* Keep the offset within the desktop; desktops are one screen apart. */
        b->x += (double)(target - src) * server->screen_width;
        b->desktop_id = target;
        b->vx = 0; b->vy = 0; b->flying = 0;
        b->tiled = 0;
        b->tiling_saved = 0;
        b->floating = (server->desktop_mode[target] == DESKTOP_MODE_FLOATING);
        view->x = (int)lround(b->x);
        view->y = (int)lround(b->y);
    } else {
        view->x += (target - src) * server->screen_width;
    }
    view->tile_anim = 0;
    view_sync_position(view);

    if (server->desktop_mode[target] == DESKTOP_MODE_TILING) {
        bsp_insert(&server->bsp_roots[target], 0, view->id);
        server_apply_tiling(server, target);
    }

    if (follow) {
        /* Relocating a window in order to keep working in it: travel with it
         * and keep it focused. Claiming focus_desktop here stops the camera's
         * arrival handler from handing the keyboard to whatever else lives on
         * the destination. */
        server->target_camera_x = target * server->screen_width;
        server->cam_free = 0;
        server->focus_desktop = target;
        server_focus_view(server, view);
    } else if (server->focused_view == view) {
        /* The camera stays put (i3/sway convention: moving is not following),
         * so the keyboard must not follow the window off-screen either. */
        server_refocus(server, src, view);
    }

    server_request_tray_redraw(server);
}

void server_toggle_desktop_floating(FwmServer *server, int d) {
    server_set_desktop_mode(server, d,
        server->desktop_mode[d] == DESKTOP_MODE_FLOATING
            ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_FLOATING);
}
