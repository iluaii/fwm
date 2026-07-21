/* Pointer and seat: cursor motion and buttons, interactive move/resize entry,
 * selections, drag-and-drop, and pointer constraints. Split out of server.c;
 * see server_internal.h. */
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
#include <linux/input-event-codes.h>
#include "server_internal.h"

/* Defined further down this file; used before their definitions. */
static void drag_icon_update_position(FwmServer *server);
static FwmView *view_from_surface(FwmServer *server, struct wlr_surface *surface);
static void constraints_follow_focus(FwmServer *server, struct wlr_surface *surface);

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
#include "server_internal.h"

struct FwmView *view_at(FwmServer *server, double lx, double ly,
                               struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    /* A buffer that is NOT a client surface is one of ours — during an impact
     * squash the live surface is hidden and our snapshot is what the cursor
     * lands on. Returning NULL here made the window inert for the ~250ms the
     * effect ran: no focus, no super+drag, no resize. Fall through with a NULL
     * surface instead, so the window is still grabbable; every caller already
     * distinguishes "no view" from "view with nothing to send events to". */
    *surface = scene_surface ? scene_surface->surface : NULL;

    // Walk up to find the tree node holding the FwmView (set in view_map)
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    if (tree == NULL) {
        return NULL;
    }
    return tree->node.data;
}

void handle_cursor_motion(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    /* Relative motion goes out regardless of whether the cursor itself is
     * allowed to move: a locked pointer is exactly the case where the client
     * steers from deltas and the cursor must stay put. */
    if (server->relative_pointer) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer, server->seat,
            (uint64_t)event->time_msec * 1000, event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
    }

    if (server->active_constraint && !lock_is_active(server)) {
        if (server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
            /* Mouse-look: the cursor does not move at all. */
            server_notify_activity(server);
            return;
        }
        /* Confined: allow the move only while it stays inside the region. */
        FwmView *cv = view_from_surface(server, server->active_constraint->surface);
        if (cv) {
            double nx = server->cursor->x + event->delta_x;
            double ny = server->cursor->y + event->delta_y;
            double sx = nx - (cv->x - server->camera_x);
            double sy = ny - cv->y;
            if (!pixman_region32_contains_point(&server->active_constraint->region,
                                                (int)sx, (int)sy, NULL)) {
                server_notify_activity(server);
                return;
            }
        }
    }

    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    
    // Process pointer movement
    double lx = server->cursor->x;
    double ly = server->cursor->y;

    server_notify_activity(server);
    if (lock_is_active(server)) return; /* nothing under the lock may be reached */
    drag_icon_update_position(server);

    // While the launcher is open the pointer belongs to it: hover moves the
    // selection, clients get no motion (and no pointer focus).
    if (launcher_is_open(server->launcher)) {
        launcher_handle_motion(server->launcher, lx, ly);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    if (server->interactive.action == FWM_ACTION_MOVE) {
        FwmView *view = server->interactive.view;
        double dx = lx - server->interactive.start_x;
        double dy = ly - server->interactive.start_y;
        
        // Keep the window fully inside the play area while dragging. Because the
        // dragged body is kinematic it would otherwise pass straight through the
        // (static) boundary walls and, on release, either get stuck outside them
        // or be shot out at 90 degrees as Box2D resolves the wall penetration.
        int min_world_x = 0;
        int max_world_x = 10 * server->screen_width - server->interactive.view_start_width;
        int min_y = 0;
        int max_y = server->screen_height - server->interactive.view_start_height;
        if (max_world_x < min_world_x) max_world_x = min_world_x;
        if (max_y < min_y) max_y = min_y;
        
        int target_world_x = server->interactive.view_start_x + server->camera_x + dx;
        int target_world_y = server->interactive.view_start_y + dy;
        int want_x = target_world_x, want_y = target_world_y;
        
        if (target_world_x < min_world_x) target_world_x = min_world_x;
        if (target_world_x > max_world_x) target_world_x = max_world_x;
        if (target_world_y < min_y) target_world_y = min_y;
        if (target_world_y > max_y) target_world_y = max_y;

        // When the clamp engages, re-base the grab anchor onto the clamped
        // position: while the window is pinned against a wall the cursor keeps
        // travelling, and without this the whole overshoot has to be dragged
        // back before the window moves again — magnet-stuck to the edge.
        // Only on an actual clamp: doing it unconditionally accumulates
        // int-truncation error every motion event and the window drifts away
        // from the cursor.
        if (target_world_x != want_x) {
            server->interactive.view_start_x += target_world_x - want_x;
        }
        if (target_world_y != want_y) {
            server->interactive.view_start_y += target_world_y - want_y;
        }

        view->x = target_world_x;
        view->y = target_world_y;

        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node,
                (int)lround(view->x - server->camera_x), (int)lround(view->y));
        }

        // Shift velocity history
        for (int i = 0; i < 3; i++) {
            server->interactive.hist_x[i] = server->interactive.hist_x[i+1];
            server->interactive.hist_y[i] = server->interactive.hist_y[i+1];
            server->interactive.hist_time[i] = server->interactive.hist_time[i+1];
        }
        server->interactive.hist_x[3] = lx;
        server->interactive.hist_y[3] = ly;
        server->interactive.hist_time[3] = now;
        if (server->interactive.hist_count < 4) server->interactive.hist_count++;
        
        if (server->interactive.hist_count >= 2) {
            int oldest = 4 - server->interactive.hist_count;
            double dt = (double)(now.tv_sec - server->interactive.hist_time[oldest].tv_sec) +
                        (double)(now.tv_nsec - server->interactive.hist_time[oldest].tv_nsec) / 1e9;
            if (dt > 0.001) {
                server->interactive.vx = (lx - server->interactive.hist_x[oldest]) / dt;
                server->interactive.vy = (ly - server->interactive.hist_y[oldest]) / dt;
            }
        }
        
        // Sync position in physics
        physics_sync_body(&server->physics, view->id, view->x, view->y, view->width, view->height, server->screen_width);
        
        // Auto camera scroll at edges
        if (server->camera_x == server->target_camera_x) {
            int current_d = server->target_camera_x / server->screen_width;
            if (lx >= server->screen_width - 10 && current_d < 9) {
                server->target_camera_x = (current_d + 1) * server->screen_width;
                server->cam_free = 0; // discrete jump: use the eased slide
            } else if (lx <= 10 && current_d > 0) {
                server->target_camera_x = (current_d - 1) * server->screen_width;
                server->cam_free = 0;
            }
        }
    } else if (server->interactive.action == FWM_ACTION_RESIZE) {
        FwmView *view = server->interactive.view;
        double dx = lx - server->interactive.start_x;
        double dy = ly - server->interactive.start_y;
        
        int new_w = server->interactive.view_start_width + dx;
        int new_h = server->interactive.view_start_height + dy;
        int max_w = server->screen_width - server->interactive.view_start_x;
        int max_h = server->screen_height - server->interactive.view_start_y;
        
        if (new_w < 50) new_w = 50;
        if (new_h < 50) new_h = 50;
        if (new_w > max_w) new_w = max_w;
        if (new_h > max_h) new_h = max_h;
        
        view->width = new_w;
        view->height = new_h;
        
        view_set_size(view, view->width, view->height);
        physics_sync_body(&server->physics, view->id, view->x, view->y, view->width, view->height, server->screen_width);
    } else if (server->interactive.action == FWM_ACTION_BSP_RESIZE) {
        BspNode *n = server->interactive.bsp_node;
        float delta;
        if (!n->split_h) {
            delta = (float)(lx - server->interactive.start_x) / n->w;
        } else {
            delta = (float)(ly - server->interactive.start_y) / n->h;
        }
        n->ratio = server->interactive.bsp_start_ratio + delta;
        if (n->ratio < 0.1f) n->ratio = 0.1f;
        if (n->ratio > 0.9f) n->ratio = 0.9f;
        
        int d = server->target_camera_x / server->screen_width;
        server_apply_tiling(server, d);
    } else if (server->interactive.action == FWM_ACTION_SWAP) {
        server->interactive.cur_x = lx;
        server->interactive.cur_y = ly;
    } else {
        // Focus follows pointer
        struct wlr_surface *surface = NULL;
        double sx, sy;
        FwmView *view = view_at(server, lx, ly, &surface, &sx, &sy);
        if (surface) {
            // view == NULL happens over unmanaged X11 surfaces (menus,
            // tooltips): they still get pointer events, just no focus change.
            if (view) server_focus_view(server, view);
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
            constraints_follow_focus(server, surface);
        } else {
            // Over the empty background: no client owns the cursor, so restore
            // our default image (otherwise it keeps the last client's cursor or
            // none at all).
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
            wlr_seat_pointer_clear_focus(server->seat);
            constraints_follow_focus(server, NULL);
        }
    }
}

void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    server_notify_activity(server);
    if (lock_is_active(server)) return; /* nothing under the lock may be reached */
    drag_icon_update_position(server);
    if (launcher_is_open(server->launcher)) {
        launcher_handle_motion(server->launcher, server->cursor->x, server->cursor->y);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
    }
}

void handle_cursor_button(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    server_notify_activity(server);
    if (lock_is_active(server)) return; /* no clicks reach anything under the lock */

    bool l_was_open = launcher_is_open(server->launcher);
    if (launcher_handle_button(server->launcher, server->cursor->x, server->cursor->y,
                               event->state == WL_POINTER_BUTTON_STATE_PRESSED)) {
        launcher_grab_sync(server, l_was_open); /* click may have closed it */
        return; /* launcher consumed the click; nothing reaches clients */
    }

    // Config-error pill in the tray: toggles the detail panel. Handled before
    // anything else so the click never reaches a window underneath.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE && server->tray_buffer &&
        server->config.error_count > 0) {
        double tx = server->cursor->x - server->tray_buffer->node.x;
        double ty = server->cursor->y - server->tray_buffer->node.y;
        if (tray_error_pill_hit(tx, ty)) {
            server_dispatch_action(server, "show_errors");
            server->group_click = 1; /* swallow the matching release */
            return;
        }
    }

    // Desktop indicators: a left click jumps to that desktop.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE && server->tray_buffer) {
        double tx = server->cursor->x - server->tray_buffer->node.x;
        double ty = server->cursor->y - server->tray_buffer->node.y;
        int d = tray_desktop_hit(tx, ty);
        if (d >= 0) {
            server->target_camera_x = d * server->screen_width;
            server->cam_free = 0;    /* discrete jump: the eased slide */
            server->group_click = 1; /* swallow the matching release */
            return;
        }
    }

    // Tab-stack bars: a left click on a tab switches the stack's window and
    // stays in the compositor (its release is swallowed too).
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT &&
        server->interactive.action == FWM_ACTION_NONE) {
        int tab;
        FwmGroup *bg = group_bar_at(server, server->cursor->x, server->cursor->y, &tab);
        if (bg) {
            group_set_active(server, bg, tab);
            server->group_click = 1;
            return;
        }
    }
    if (server->group_click && event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->group_click = 0;
        return;
    }

    // Clicks that belong to a compositor gesture stay in the compositor: any
    // button event while a drag/resize/swap is running, and any press with
    // the drag modifier (Super) held. Forwarding them made clients count
    // phantom clicks during window drags.
    uint32_t fwd_mods = get_active_modifiers(server);
    if (server->interactive.action == FWM_ACTION_NONE && !(fwd_mods & FWM_MOD_LOGO)) {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    }
    
    double lx = server->cursor->x;
    double ly = server->cursor->y;
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        struct wlr_surface *surface = NULL;
        double sx, sy;
        FwmView *view = view_at(server, lx, ly, &surface, &sx, &sy);
        
        if (view) {
            server->last_touched_view = view;
            /* Touching a window ends its impact squash: while one runs the
             * live surface is hidden behind a snapshot, so without this the
             * grabbed window would keep wobbling and stay unresponsive inside
             * for the rest of the effect. The user taking hold of it outranks
             * the animation. */
            view_stop_squash(view);
            server_focus_view(server, view);
            physics_stop_body(&server->physics, view->id);
            
            uint32_t active_mods = get_active_modifiers(server);
            
            // Check for drag gestures: Mod (Super/Logo) + Button1
            if (active_mods & FWM_MOD_LOGO) {
                PhysicsBody *pb = physics_find_body(&server->physics, view->id);
                int tiling = pb ? (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) : 0;
                
                if (event->button == BTN_LEFT) {
                    if (tiling) {
                        if (active_mods & FWM_MOD_SHIFT) {
                            server->interactive.action = FWM_ACTION_SWAP;
                            server->interactive.view = view;
                            server->interactive.start_x = lx;
                            server->interactive.start_y = ly;
                            server->interactive.cur_x = lx;
                            server->interactive.cur_y = ly;
                        }
                    } else {
                        server->interactive.action = FWM_ACTION_MOVE;
                        server->interactive.view = view;
                        server->interactive.start_x = lx;
                        server->interactive.start_y = ly;
                        server->interactive.view_start_x = view->x - server->camera_x;
                        server->interactive.view_start_y = view->y;
                        server->interactive.view_start_width = view->width;
                        server->interactive.view_start_height = view->height;
                        server->interactive.last_x = lx;
                        server->interactive.last_y = ly;
                        server->interactive.last_time = now;
                        server->interactive.vx = 0;
                        server->interactive.vy = 0;
                        server->interactive.hist_count = 0;
                        server->interactive.collision_disabled = (active_mods & FWM_MOD_SHIFT) ? 1 : 0;
                    }
                } else if (event->button == BTN_RIGHT) {
                    if (tiling) {
                        int d = pb ? pb->desktop_id : 0;
                        BspNode *node = bsp_find_border(server->bsp_roots[d], lx + server->camera_x, ly, 40);
                        if (node) {
                            server->interactive.action = FWM_ACTION_BSP_RESIZE;
                            server->interactive.bsp_node = node;
                            server->interactive.bsp_start_ratio = node->ratio;
                            server->interactive.start_x = lx;
                            server->interactive.start_y = ly;
                        }
                    } else {
                        server->interactive.action = FWM_ACTION_RESIZE;
                        server->interactive.view = view;
                        server->interactive.start_x = lx;
                        server->interactive.start_y = ly;
                        server->interactive.view_start_x = view->x - server->camera_x;
                        server->interactive.view_start_y = view->y;
                        server->interactive.view_start_width = view->width;
                        server->interactive.view_start_height = view->height;
                    }
                }
            }
        }
    } else {
        // Button release
        if (server->interactive.action == FWM_ACTION_MOVE) {
            FwmView *view = server->interactive.view;
            // Dropping an ungrouped window onto a tab bar adds it to that stack.
            int tab;
            FwmGroup *bg = view ? group_bar_at(server, lx, ly, &tab) : NULL;
            if (view && bg && !view->group && group_add(server, bg, view)) {
                // adopted by the group — no throw
            } else if (view) {
                PhysicsBody *pb = physics_find_body(&server->physics, view->id);
                if (pb) {
                    if (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) {
                        server_apply_tiling(server, pb->desktop_id);
                    } else {
                        physics_push_overlapping(&server->physics, view->id, 280.0);
                        physics_throw_body(&server->physics, view->id, server->interactive.vx, server->interactive.vy);
                    }
                }
            }
        } else if (server->interactive.action == FWM_ACTION_SWAP) {
            FwmView *src_view = server->interactive.view;
            if (src_view) {
                PhysicsBody *src_pb = physics_find_body(&server->physics, src_view->id);
                if (src_pb) {
                    int d = src_pb->desktop_id;
                    BspNode *leaves[MAX_WINDOWS];
                    int count = 0;
                    bsp_collect_leaves(server->bsp_roots[d], leaves, &count, MAX_WINDOWS);
                    
                    uint32_t target_id = 0;
                    for (int i = 0; i < count; i++) {
                        BspNode *n = leaves[i];
                        int sx = n->x - server->camera_x;
                        if (server->interactive.cur_x >= sx && server->interactive.cur_x <= sx + n->w &&
                            server->interactive.cur_y >= n->y && server->interactive.cur_y <= n->y + n->h) {
                            target_id = n->id;
                            break;
                        }
                    }
                    
                    if (target_id != 0 && target_id != src_view->id) {
                        bsp_swap(server->bsp_roots[d], src_view->id, target_id);
                        server_apply_tiling(server, d);
                    }
                }
            }
        }
        
        // X11 windows: push the final position after a drag/resize so the
        // client's idea of its root coordinates matches reality again.
        if (server->interactive.view) view_sync_position(server->interactive.view);

        server->interactive.action = FWM_ACTION_NONE;
        server->interactive.view = NULL;
    }
}

void handle_cursor_axis(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    server_notify_activity(server);
    if (lock_is_active(server)) return;

    // Scrolling over the desktop island steps between desktops. Consumed, so
    // it never also scrolls whatever window happens to be under the tray.
    // Vertical only: a touchpad sends both axes in one frame, and honouring
    // horizontal too would step two desktops per gesture.
    if (server->tray_buffer && event->delta != 0.0 &&
        event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        double tx = server->cursor->x - server->tray_buffer->node.x;
        double ty = server->cursor->y - server->tray_buffer->node.y;
        if (tray_desktop_island_hit(tx, ty)) {
            /* Step from where the camera is HEADED, not where it is: spinning
             * the wheel several notches must advance several desktops rather
             * than fight the slide still in flight. */
            int d = server->target_camera_x / server->screen_width;
            d += event->delta > 0.0 ? 1 : -1;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            server->target_camera_x = d * server->screen_width;
            server->cam_free = 0;
            return;
        }
    }

    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source, event->relative_direction);
}

void handle_cursor_frame(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

void handle_request_cursor(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

void handle_seat_request_set_selection(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void handle_seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

/* The icon rides the cursor directly in layout coordinates. It is NOT offset by
 * camera_x: it belongs to the pointer, not to the desktop under it, so it must
 * not slide when the camera pans mid-drag. */
static void drag_icon_update_position(FwmServer *server) {
    if (!server->drag_icon) return;
    wlr_scene_node_set_position(&server->drag_icon->node,
                                (int)lround(server->cursor->x),
                                (int)lround(server->cursor->y));
}

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, drag_icon_destroy);
    (void)data;
    /* wlroots destroys the scene tree along with the icon; only drop our
     * pointer, or the next drag would raise a dangling node. */
    server->drag_icon = NULL;
    wl_list_remove(&server->drag_icon_destroy.link);
}

void handle_seat_start_drag(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_start_drag);
    struct wlr_drag *drag = data;
    if (!drag->icon) return; /* a drag without an icon is perfectly legal */

    /* layer_overlay sits above windows and holds the tray/hints/launcher;
     * raising puts the icon above those too, so it is never covered. */
    server->drag_icon = wlr_scene_drag_icon_create(server->layer_overlay, drag->icon);
    if (!server->drag_icon) return;
    wlr_scene_node_raise_to_top(&server->drag_icon->node);
    drag_icon_update_position(server);

    server->drag_icon_destroy.notify = handle_drag_icon_destroy;
    wl_signal_add(&drag->icon->events.destroy, &server->drag_icon_destroy);
}

/* Which view owns a surface. Takes the root surface, so a subsurface (a video
 * player's content layer, an inhibitor's surface) resolves to its toplevel. */
static FwmView *view_from_surface(FwmServer *server, struct wlr_surface *surface) {
    if (!surface) return NULL;
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (view_surface(v) == root) return v;
    }
    return NULL;
}

/* An inhibitor only counts while its surface is actually on screen — the
 * protocol says inhibitors apply "while this surface is visible", and on fwm a
 * video paused on desktop 7 must not keep the whole session awake. Recomputed
 * from scratch each tick: the list is empty or has one entry in practice, and
 * polling avoids hooking every path that can change visibility (map, unmap,
 * desktop switch, camera slide). */
void idle_inhibit_refresh(FwmServer *server) {
    if (!server->idle_notifier || !server->idle_inhibit) return;

    int visible_d = (server->camera_x + server->screen_width / 2) / server->screen_width;
    int inhibited = 0;
    struct wlr_idle_inhibitor_v1 *inh;
    wl_list_for_each(inh, &server->idle_inhibit->inhibitors, link) {
        FwmView *v = view_from_surface(server, inh->surface);
        if (!v) continue;
        PhysicsBody *pb = physics_find_body(&server->physics, v->id);
        /* No body means a hidden group member — not visible by definition. */
        if (pb && pb->desktop_id == visible_d) { inhibited = 1; break; }
    }

    if (inhibited != server->idle_inhibited) {
        wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited != 0);
        server->idle_inhibited = inhibited;
    }
}

/* ── pointer constraints ──────────────────────────────────────────────── */

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, constraint_destroy);
    (void)data;
    server->active_constraint = NULL;
    wl_list_remove(&server->constraint_destroy.link);
}

/* A constraint may only hold the pointer while its surface actually has
 * pointer focus, or a background window could capture the mouse forever. */
static void constraint_set_active(FwmServer *server,
                                  struct wlr_pointer_constraint_v1 *constraint) {
    if (server->active_constraint == constraint) return;

    if (server->active_constraint) {
        wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
        wl_list_remove(&server->constraint_destroy.link);
        server->active_constraint = NULL;
    }
    if (constraint) {
        server->active_constraint = constraint;
        server->constraint_destroy.notify = handle_constraint_destroy;
        wl_signal_add(&constraint->events.destroy, &server->constraint_destroy);
        wlr_pointer_constraint_v1_send_activated(constraint);
    }
}

/* Called whenever pointer focus may have changed. */
static void constraints_follow_focus(FwmServer *server, struct wlr_surface *surface) {
    if (!server->pointer_constraints) return;
    struct wlr_pointer_constraint_v1 *found = NULL;
    if (surface) {
        found = wlr_pointer_constraints_v1_constraint_for_surface(
            server->pointer_constraints, surface, server->seat);
    }
    constraint_set_active(server, found);
}

void handle_new_pointer_constraint(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_pointer_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;

    /* Activate immediately when the pointer is already over the requesting
     * surface — which is the normal case: a game grabs the mouse on click. */
    if (server->seat->pointer_state.focused_surface == constraint->surface) {
        constraint_set_active(server, constraint);
    }
}

/* cursor-shape-v1: clients name a cursor ("text", "grab") instead of supplying
 * a surface. Older clients keep using wl_pointer.set_cursor, which still works. */
void handle_cursor_shape_request(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_shape_request);
    const struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

    /* Same rule as wl_pointer.set_cursor: only the client the pointer is
     * actually over may change the cursor. */
    struct wlr_seat_client *focused =
        server->seat->pointer_state.focused_client;
    if (event->seat_client != focused) return;

    const char *name = wlr_cursor_shape_v1_name(event->shape);
    if (name) wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
}

void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, xdg_activation_request_activate);
    const struct wlr_xdg_activation_v1_request_activate_event *event = data;

    FocusActivatePolicy policy = server->config.focus.on_activate;
    if (policy == FOCUS_ACTIVATE_NEVER) return;

    /* Filters out clients that do not even claim to be acting on user input.
     * NOT real focus-stealing protection: wlroots stores whatever serial the
     * client sends without checking it against any input event, so a
     * determined app passes this trivially. Validating the serial against
     * recent input would be the real fix. */
    if (!event->token->seat) return;

    FwmView *view = view_from_surface(server, event->surface);
    if (!view) return;

    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    int target_d = (pb && pb->desktop_id >= 0 && pb->desktop_id < 10) ? pb->desktop_id : -1;
    int visible_d = (server->camera_x + server->screen_width / 2) / server->screen_width;

    if (target_d >= 0 && target_d != visible_d) {
        /* Off-screen window. Panning the camera away from what the user is
         * working on is the disruptive part of activation, so only "always"
         * may do it; "same_desktop" drops the request instead. */
        if (policy != FOCUS_ACTIVATE_ALWAYS) return;
        server->target_camera_x = target_d * server->screen_width;
        server->cam_free = 0; /* discrete jump: eased slide, not the free chase */
    }
    server_focus_view(server, view);
}

void handle_seat_request_start_drag(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;

    /* Only honour a drag the client can prove it owns, otherwise any client
     * could start one at will. A rejected request MUST destroy the source or
     * the requesting client waits forever. */
    if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
    } else if (event->drag->source) {
        wlr_data_source_destroy(event->drag->source);
    }
}
