/* Action dispatch: the single table mapping a bind or an IPC command to what
 * the compositor actually does, plus the tiling-context helpers only it uses.
 * Split out of server.c; see server_internal.h. */
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

/* Directional tile navigation: among the leaves of `desktop`, find the one
 * nearest to `from` in direction `dir` ('l','r','u','d'), judged by tile
 * centers. Returns NULL if there is nothing that way. */
static BspNode *tile_neighbor(FwmServer *server, int desktop, BspNode *from, char dir) {
    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(server->bsp_roots[desktop], leaves, &count, MAX_WINDOWS);

    double fx = from->x + from->w / 2.0;
    double fy = from->y + from->h / 2.0;

    BspNode *best = NULL;
    double best_dist = 0;
    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        if (n == from) continue;
        double cx = n->x + n->w / 2.0;
        double cy = n->y + n->h / 2.0;
        double dx = cx - fx, dy = cy - fy;

        int ok = 0;
        switch (dir) {
        case 'l': ok = dx < -1; break;
        case 'r': ok = dx >  1; break;
        case 'u': ok = dy < -1; break;
        case 'd': ok = dy >  1; break;
        }
        if (!ok) continue;

        // Prefer the closest tile, weighting the off-axis offset heavier so
        // "focus left" picks the tile actually beside us, not one far diagonal.
        double axis  = (dir == 'l' || dir == 'r') ? fabs(dx) : fabs(dy);
        double cross = (dir == 'l' || dir == 'r') ? fabs(dy) : fabs(dx);
        double dist = axis + cross * 2.0;
        if (!best || dist < best_dist) {
            best = n;
            best_dist = dist;
        }
    }
    return best;
}

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



/* Shared setup for the tile_* actions: resolves the focused view's desktop,
 * checks it is tiling, and finds its leaf. Returns 0 if not applicable. */
static int tile_action_ctx(FwmServer *server, int *out_d, BspNode **out_leaf) {
    if (!server->focused_view) return 0;
    PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
    if (!pb) return 0;
    int d = pb->desktop_id;
    if (server->desktop_mode[d] != DESKTOP_MODE_TILING) return 0;
    BspNode *leaf = bsp_find(server->bsp_roots[d], server->focused_view->id);
    if (!leaf) return 0;
    *out_d = d;
    *out_leaf = leaf;
    return 1;
}

void server_dispatch_action(FwmServer *server, const char *action) {
    if (strcmp(action, "killclient") == 0) {
        if (server->focused_view) {
            view_send_close(server->focused_view);
        }
    } else if (strcmp(action, "toggle_tiling") == 0) {
        server_toggle_desktop_tiling(server, server->target_camera_x / server->screen_width);
    } else if (strncmp(action, "tile_focus:", 11) == 0) {
        int d; BspNode *leaf;
        if (tile_action_ctx(server, &d, &leaf)) {
            BspNode *n = tile_neighbor(server, d, leaf, action[11]);
            if (n) {
                FwmView *v = server_find_view(server, n->id);
                if (v) server_focus_view(server, v);
            }
        }
    } else if (strncmp(action, "tile_move:", 10) == 0) {
        int d; BspNode *leaf;
        if (tile_action_ctx(server, &d, &leaf)) {
            BspNode *n = tile_neighbor(server, d, leaf, action[10]);
            if (n) {
                bsp_swap(server->bsp_roots[d], leaf->id, n->id);
                server_apply_tiling(server, d);
            }
        }
    } else if (strcmp(action, "toggle_split") == 0) {
        int d; BspNode *leaf;
        if (tile_action_ctx(server, &d, &leaf) && leaf->parent) {
            leaf->parent->split_h = !leaf->parent->split_h;
            server_apply_tiling(server, d);
        }
    } else if (strcmp(action, "EXIT") == 0) {
        server->running = 0;
        wl_display_terminate(server->wl_display);
    } else if (strcmp(action, "show_hints") == 0) {
        if (server->hints_buffer) {
            cairo_overlay_destroy(server->hints_buffer);
            server->hints_buffer = NULL;
        } else {
            server->hints_buffer = hints_show(server->layer_overlay, server->screen_width, server->screen_height, &server->config);
        }
    } else if (strcmp(action, "wallpaper_picker") == 0) {
        bool was_open = launcher_is_open(server->launcher);
        launcher_toggle_wallpapers(server->launcher);
        launcher_grab_sync(server, was_open);
    } else if (strcmp(action, "reload_config") == 0) {
        server_reload_config(server);
    } else if (strcmp(action, "show_errors") == 0) {
        if (server->errors_buffer) {
            server_close_errors_panel(server);
        } else {
            server->errors_buffer = errors_show(server->layer_overlay, server->screen_width,
                                                server->screen_height, &server->config);
        }
        server_request_tray_redraw(server);
    } else if (strcmp(action, "group_toggle") == 0) {
        FwmView *v = server->focused_view;
        if (v) {
            if (v->group) group_dissolve(server, v->group);
            else group_create(server, v);
        }
    } else if (strcmp(action, "group_next") == 0) {
        if (server->focused_view && server->focused_view->group) {
            group_cycle(server, server->focused_view->group, 1);
        }
    } else if (strcmp(action, "group_prev") == 0) {
        if (server->focused_view && server->focused_view->group) {
            group_cycle(server, server->focused_view->group, -1);
        }
    } else if (strcmp(action, "group_add") == 0) {
        // Join the focused window into the group of any grouped window it
        // overlaps (drag-dropping onto a tab bar does the same with the mouse).
        FwmView *v = server->focused_view;
        if (v && !v->group) {
            FwmView *o;
            wl_list_for_each(o, &server->views, link) {
                if (o == v || !o->group || !o->scene_tree ||
                    !o->scene_tree->node.enabled) continue;
                if (v->x < o->x + o->width && v->x + v->width > o->x &&
                    v->y < o->y + o->height && v->y + v->height > o->y) {
                    group_add(server, o->group, v);
                    break;
                }
            }
        }
    } else if (strcmp(action, "cycle_gravity") == 0) {
        double g = server->physics.gravity_scale;
        if (g == 0.0)       server->physics.gravity_scale = 0.15;
        else if (g == 0.15) server->physics.gravity_scale = 1.0;
        else                server->physics.gravity_scale = 0.0;
    } else if (strcmp(action, "pin_window") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            if (pb) {
                pb->pinned ^= 1;
                pb->vx = 0; pb->vy = 0; pb->flying = 0;
            }
        }
    } else if (strcmp(action, "toggle_nocollide") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            if (pb) pb->no_collide ^= 1;
        }
    } else if (strcmp(action, "toggle_nocollide_all") == 0) {
        /* For app launchers that spit out a pile of windows at once, where
         * turning collision off one window at a time is hopeless. Uniform
         * rather than per-window XOR: after any press every window is in the
         * same state, which is the only predictable meaning of "all". */
        int all_off = 1;
        for (int i = 0; i < server->physics.body_count; i++) {
            const PhysicsBody *b = &server->physics.bodies[i];
            if (b->active && !b->no_collide) { all_off = 0; break; }
        }
        int want = all_off ? 0 : 1;
        for (int i = 0; i < server->physics.body_count; i++) {
            PhysicsBody *b = &server->physics.bodies[i];
            if (b->active) b->no_collide = want;
        }
    } else if (strcmp(action, "toggle_tiling_all") == 0) {
        /* Same rule: bring every desktop to one mode rather than flipping each
         * independently. Tiling wins unless everything is already tiled. */
        int all_tiled = 1;
        for (int d = 0; d < 10; d++) {
            if (server->desktop_mode[d] != DESKTOP_MODE_TILING) { all_tiled = 0; break; }
        }
        int want = all_tiled ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_TILING;
        /* Set the mode outright instead of toggling: a floating desktop would
         * otherwise be flipped INTO tiling on the "everything back to physics"
         * pass, which is the opposite of what was asked. */
        for (int d = 0; d < 10; d++) server_set_desktop_mode(server, d, want);
    } else if (strcmp(action, "toggle_floating") == 0) {
        server_toggle_desktop_floating(server, server->target_camera_x / server->screen_width);
    } else if (strcmp(action, "toggle_floating_all") == 0) {
        int all_floating = 1;
        for (int d = 0; d < 10; d++) {
            if (server->desktop_mode[d] != DESKTOP_MODE_FLOATING) { all_floating = 0; break; }
        }
        int want = all_floating ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_FLOATING;
        for (int d = 0; d < 10; d++) server_set_desktop_mode(server, d, want);
    } else if (strcmp(action, "calm_all") == 0) {
        for (int i = 0; i < server->physics.body_count; i++) {
            PhysicsBody *b = &server->physics.bodies[i];
            if (!b->active) continue;
            b->vx = 0; b->vy = 0; b->flying = 0;
        }
    } else if (strcmp(action, "fake_fullscreen") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            bool on = pb && pb->fullscreen;
            server_set_fullscreen(server, server->focused_view, !on, false);
        }
    } else if (strcmp(action, "real_fullscreen") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            bool on = pb && pb->fullscreen;
            server_set_fullscreen(server, server->focused_view, !on, true);
        }
    } else if (strncmp(action, "spawn:", 6) == 0) {
        const char *cmd = action + 6;
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            exit(1);
        }
    } else if (strncmp(action, "move_camera:", 12) == 0) {
        int amt = atoi(action + 12);
        int new_target = server->target_camera_x + amt;
        if (new_target < 0) new_target = 0;
        if (new_target > 9 * server->screen_width) new_target = 9 * server->screen_width;
        server->target_camera_x = new_target;
        server->cam_free = 1; // continuous pan, not a desktop jump
    } else if (strcmp(action, "launcher") == 0) {
        bool was_open = launcher_is_open(server->launcher);
        launcher_toggle(server->launcher);
        launcher_grab_sync(server, was_open);
    } else if (strncmp(action, "view:", 5) == 0) {
        int desktop = atoi(action + 5);
        if (desktop >= 0 && desktop < 10) {
            server->target_camera_x = desktop * server->screen_width;
            server->cam_free = 0; // discrete jump: use the eased slide
        }
    } else if (strncmp(action, "move_to:", 8) == 0) {
        server_move_view_to_desktop(server, server->focused_view, atoi(action + 8), 0);
    } else if (strncmp(action, "move_to_view:", 13) == 0) {
        server_move_view_to_desktop(server, server->focused_view, atoi(action + 13), 1);
    }
}

/* Run a config action on behalf of something that is not the keyboard (the
 * control socket). Deliberately the SAME entry point as a keybind, so an
 * action never behaves differently depending on how it was triggered.
 *
 * The caller is responsible for the locked-session check; the keyboard path
 * does its own before it ever reaches here. */
void server_dispatch_action_external(FwmServer *server, const char *action) {
    wlr_log(WLR_DEBUG, "ipc: dispatch %s", action);
    server_dispatch_action(server, action);
}
