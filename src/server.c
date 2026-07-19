#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "ui/tray.h"
#include "ui/hints.h"
#include "ui/welcome.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct FwmOutput {
    struct wl_list link;
    FwmServer *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener destroy;
};

struct FwmKeyboard {
    struct wl_list link;
    FwmServer *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

static uint32_t get_active_modifiers(FwmServer *server) {
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (!kbd || !kbd->xkb_state) return 0;
    
    uint32_t mods = 0;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= FWM_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)  mods |= FWM_MOD_CTRL;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)   mods |= FWM_MOD_ALT;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)  mods |= FWM_MOD_LOGO;
    return mods;
}

static struct FwmView *view_at(FwmServer *server, double lx, double ly,
                               struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }
    *surface = scene_surface->surface;

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

static void server_dispatch_action(FwmServer *server, const char *action);

/* Only continuous navigation binds should auto-repeat while held. Repeating
 * one-shot actions (killclient, spawn, toggles, view/fullscreen) would be
 * destructive or spammy, so they fire once per press only. */
static bool action_is_repeatable(const char *action) {
    return strncmp(action, "move_camera:", 12) == 0;
}

static void key_repeat_stop(FwmServer *server) {
    server->repeat_action = NULL;
    server->repeat_keycode = 0;
    if (server->key_repeat_timer) {
        wl_event_source_timer_update(server->key_repeat_timer, 0);
    }
}

static int key_repeat_cb(void *data) {
    FwmServer *server = data;
    if (!server->repeat_action) return 0;
    server_dispatch_action(server, server->repeat_action);
    // Re-arm at the keyboard repeat rate (25/s -> 40ms).
    wl_event_source_timer_update(server->key_repeat_timer, 40);
    return 0;
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
    struct FwmKeyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;
    FwmServer *server = keyboard->server;
    
    // Handle overlays dismissal first
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t keycode = event->keycode + 8;
        const xkb_keysym_t *syms;
        int num_syms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);
        
        for (int i = 0; i < num_syms; i++) {
            xkb_keysym_t sym = syms[i];
            if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return) {
                if (server->welcome_buffer) {
                    welcome_set_welcomed();
                    cairo_overlay_destroy(server->welcome_buffer);
                    server->welcome_buffer = NULL;
                    return;
                }
                if (server->hints_buffer) {
                    cairo_overlay_destroy(server->hints_buffer);
                    server->hints_buffer = NULL;
                    return;
                }
            }
        }
    }
    
    // Pass event to seat first (e.g. for client hotkeys/typing)
    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
    
    if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        // Stop auto-repeat when the held bind key is released.
        if (server->repeat_action && event->keycode == server->repeat_keycode) {
            key_repeat_stop(server);
        }
        return;
    }
    
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int num_syms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t active_mods = get_active_modifiers(server);

    for (int i = 0; i < num_syms; i++) {
        // Compare case-insensitively: xkb_state_key_get_syms() reflects the
        // live CapsLock state, so with Caps Lock on, a letter key resolves to
        // its uppercase keysym (e.g. 'q' -> 'Q') while binds are parsed from
        // lowercase config strings. Without normalizing, every letter-based
        // bind silently fails to match whenever Caps Lock is toggled on,
        // while digit/Return binds (unaffected by Caps Lock) keep working.
        xkb_keysym_t sym = xkb_keysym_to_lower(syms[i]);
        for (int j = 0; j < server->config.key_count; j++) {
            KeyBind *bind = &server->config.keys[j];
            if (xkb_keysym_to_lower(bind->key) == sym && bind->mod == active_mods) {
                server_dispatch_action(server, bind->action);
                // Arm auto-repeat for repeatable binds (e.g. move_camera) so
                // holding the key keeps scrolling; delay before first repeat.
                if (action_is_repeatable(bind->action) && server->key_repeat_timer) {
                    server->repeat_action = bind->action;
                    server->repeat_keycode = event->keycode;
                    wl_event_source_timer_update(server->key_repeat_timer, 300);
                } else {
                    key_repeat_stop(server);
                }
                return;
            }
        }
    }
}

static void handle_keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct FwmKeyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
    struct FwmKeyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        struct FwmKeyboard *keyboard = calloc(1, sizeof(struct FwmKeyboard));
        keyboard->server = server;
        keyboard->wlr_keyboard = wlr_keyboard_from_input_device(device);
        
        struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(keyboard->wlr_keyboard, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        wlr_keyboard_set_repeat_info(keyboard->wlr_keyboard, 25, 600);
        
        keyboard->modifiers.notify = handle_keyboard_modifiers;
        keyboard->key.notify = handle_keyboard_key;
        keyboard->destroy.notify = handle_keyboard_destroy;
        
        wl_signal_add(&keyboard->wlr_keyboard->events.modifiers, &keyboard->modifiers);
        wl_signal_add(&keyboard->wlr_keyboard->events.key, &keyboard->key);
        wl_signal_add(&device->events.destroy, &keyboard->destroy);
        
        wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
        wl_list_insert(&server->keyboards, &keyboard->link);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(server->cursor, device);
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
    struct FwmOutput *output = wl_container_of(listener, output, frame);
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
    struct FwmOutput *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;
    
    wlr_output_init_render(wlr_output, server->wlr_allocator, server->wlr_renderer);
    
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
    
    struct FwmOutput *output = calloc(1, sizeof(struct FwmOutput));
    output->server = server;
    output->wlr_output = wlr_output;
    output->frame.notify = handle_output_frame;
    output->destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    wl_list_insert(&server->outputs, &output->link);
    
    struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
    
    if (server->screen_width == 0) {
        server->screen_width = wlr_output->width;
        server->screen_height = wlr_output->height;

        // Parallax wallpaper (below the window layer)
        server->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                             server->screen_width, server->screen_height);

        // Initialize UI panels
        server->tray_buffer = tray_init(server->layer_overlay, server->screen_width);
        server_request_tray_redraw(server);

        server->welcome_buffer = welcome_show(server->layer_overlay, server->screen_width, server->screen_height);
    }
}

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

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    
    // Process pointer movement
    double lx = server->cursor->x;
    double ly = server->cursor->y;
    
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
        
        if (target_world_x < min_world_x) target_world_x = min_world_x;
        if (target_world_x > max_world_x) target_world_x = max_world_x;
        if (target_world_y < min_y) target_world_y = min_y;
        if (target_world_y > max_y) target_world_y = max_y;
        
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
            } else if (lx <= 10 && current_d > 0) {
                server->target_camera_x = (current_d - 1) * server->screen_width;
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
        
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
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
        if (view && surface) {
            server_focus_view(server, view);
            wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(server->seat, event->time_msec, sx, sy);
        } else {
            // Over the empty background: no client owns the cursor, so restore
            // our default image (otherwise it keeps the last client's cursor or
            // none at all).
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
            wlr_seat_pointer_clear_focus(server->seat);
        }
    }
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    
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
            if (view) {
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
                    bsp_collect_leaves(server->bsp_roots[d], leaves, &count);
                    
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
        
        server->interactive.action = FWM_ACTION_NONE;
        server->interactive.view = NULL;
    }
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_request_cursor(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void handle_seat_request_set_selection(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, seat_request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static int physics_tick_cb(void *data) {
    FwmServer *server = data;
    
    // Tick intervals
    double dt = 1.0 / PHYSICS_TICK_RATE;
    
    uint32_t drag_win = (server->interactive.action == FWM_ACTION_MOVE && server->interactive.collision_disabled) ? server->interactive.view->id : 0;
    uint32_t dragged_win = (server->interactive.action == FWM_ACTION_MOVE) ? server->interactive.view->id : 0;
    // Freeze the window being resized into a static anchor (skip_b): it must not
    // sink under gravity, get shoved by neighbors, or jitter while its collision
    // box is rebuilt every motion event — the mouse owns it entirely.
    uint32_t resize_win = (server->interactive.action == FWM_ACTION_RESIZE && server->interactive.view) ? server->interactive.view->id : 0;

    if (resize_win) {
        PhysicsBody *rb = physics_find_body(&server->physics, resize_win);
        if (rb) { rb->flying = 0; rb->vx = 0; rb->vy = 0; }
    }
    
    if (server->interactive.action == FWM_ACTION_MOVE && server->interactive.view) {
        physics_set_velocity(&server->physics, server->interactive.view->id, server->interactive.vx, server->interactive.vy);
    }
    
    // Smooth camera scrolling
    if (server->camera_x != server->target_camera_x) {
        double diff = server->target_camera_x - server->camera_x;
        int step = (int)lround(diff * 10.0 * dt);
        if (step == 0) {
            step = diff > 0 ? 1 : -1;
        }
        if (abs(step) >= fabs(diff)) {
            server->camera_x = server->target_camera_x;
        } else {
            server->camera_x += step;
        }
        
        // Sync non-dragged windows relative to camera
        FwmView *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->id != dragged_win && view->scene_tree) {
                PhysicsBody *body = physics_find_body(&server->physics, view->id);
                if (body) {
                    wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(body->x - server->camera_x), (int)lround(body->y));
                }
            }
        }
    }
    
    // Tile-glide animations: ease windows toward their tile slots (Hyprland-
    // style) instead of teleporting. Exponential approach is frame-rate
    // independent; the physics bridge sees these as external writes and keeps
    // the Box2D body glued to the glide.
    {
        double k = 1.0 - exp(-server->config.tiling.anim_speed * dt);
        FwmView *av;
        wl_list_for_each(av, &server->views, link) {
            if (!av->tile_anim) continue;
            PhysicsBody *pb = physics_find_body(&server->physics, av->id);
            if (!pb) { av->tile_anim = 0; continue; }
            double dx = av->tile_tx - pb->x;
            double dy = av->tile_ty - pb->y;
            if (fabs(dx) < 1.0 && fabs(dy) < 1.0) {
                pb->x = av->tile_tx;
                pb->y = av->tile_ty;
                av->tile_anim = 0;
            } else {
                pb->x += dx * k;
                pb->y += dy * k;
            }
            pb->vx = 0; pb->vy = 0; pb->flying = 0;
            av->x = pb->x;
            av->y = pb->y;
        }
    }

    // Window fade-in: ramp buffer opacity 0 -> 1 after map.
    if (server->config.decor.fade_in_ms > 0.0) {
        double step = dt * 1000.0 / server->config.decor.fade_in_ms;
        FwmView *fv;
        wl_list_for_each(fv, &server->views, link) {
            if (!fv->fade_anim) continue;
            fv->fade_t += step;
            if (fv->fade_t >= 1.0) {
                fv->fade_t = 1.0;
                fv->fade_anim = 0;
            }
            // Ease-out: fast start, soft landing.
            double t = fv->fade_t;
            view_set_opacity(fv, 1.0 - (1.0 - t) * (1.0 - t));
        }
    }

    // Physics step
    physics_step(&server->physics, server->screen_width, server->screen_height,
                 drag_win, resize_win, dragged_win, dt);
                 
    // Synchronize scene tree nodes to physics coordinates
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->scene_tree) {
            PhysicsBody *body = physics_find_body(&server->physics, view->id);
            if (body && !body->pinned && view->id != dragged_win) {
                view->x = body->x;
                view->y = body->y;
                wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(body->x - server->camera_x), (int)lround(body->y));
            }
        }
    }
    
    // Hide the tray while a real-fullscreen window occupies the active desktop
    // (overlays outrank windows in the scene, so the surface can't cover it).
    // Fake fullscreen keeps the tray — that's its point. Checking every tick
    // also covers desktop switches and the fullscreen window closing.
    if (server->tray_buffer) {
        int active_d = (server->camera_x + server->screen_width / 2) / server->screen_width;
        bool hide = false;
        FwmView *fsv;
        wl_list_for_each(fsv, &server->views, link) {
            if (!fsv->fs_real) continue;
            PhysicsBody *fb = physics_find_body(&server->physics, fsv->id);
            if (fb && fb->fullscreen && fb->desktop_id == active_d) {
                hide = true;
                break;
            }
        }
        wlr_scene_node_set_enabled(&server->tray_buffer->node, !hide);
    }

    // Redraw tray if data changed
    server_request_tray_redraw(server);

    // Parallax: shift each wallpaper layer by a fraction of the camera offset.
    wallpaper_update(server->wallpaper, server->camera_x);

    // The physics timer drives all continuous updates (camera scroll, window
    // motion, tray/overlay content). Scene damage alone does not keep the
    // output's frame loop alive once it goes idle, so explicitly schedule a
    // frame every tick to guarantee the compositor keeps presenting.
    struct FwmOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }

    wl_event_source_timer_update(server->physics_timer, (int)(1000.0 / PHYSICS_TICK_RATE));
    return 0;
}

/* Directional tile navigation: among the leaves of `desktop`, find the one
 * nearest to `from` in direction `dir` ('l','r','u','d'), judged by tile
 * centers. Returns NULL if there is nothing that way. */
static BspNode *tile_neighbor(FwmServer *server, int desktop, BspNode *from, char dir) {
    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(server->bsp_roots[desktop], leaves, &count);

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

static FwmView *server_find_view(FwmServer *server, uint32_t id) {
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->id == id) return v;
    }
    return NULL;
}

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

static void server_dispatch_action(FwmServer *server, const char *action) {
    if (strcmp(action, "killclient") == 0) {
        if (server->focused_view) {
            wlr_xdg_toplevel_send_close(server->focused_view->xdg_toplevel);
        }
    } else if (strcmp(action, "toggle_tiling") == 0) {
        int d = server->target_camera_x / server->screen_width;
        if (server->desktop_mode[d] == DESKTOP_MODE_PHYSICS) {
            server->desktop_mode[d] = DESKTOP_MODE_TILING;
            
            // Save state for physics
            FwmView *view;
            wl_list_for_each(view, &server->views, link) {
                PhysicsBody *b = physics_find_body(&server->physics, view->id);
                if (b && b->desktop_id == d) {
                    if (!b->tiling_saved) {
                        b->sav_x = b->x; b->sav_y = b->y;
                        b->sav_w = b->width; b->sav_h = b->height;
                        b->tiling_saved = 1;
                    }
                }
            }
            
            bsp_free(server->bsp_roots[d]);
            server->bsp_roots[d] = NULL;
            
            // Insert views to BSP tree
            wl_list_for_each(view, &server->views, link) {
                PhysicsBody *b = physics_find_body(&server->physics, view->id);
                if (b && !b->shaped && !b->fullscreen && b->desktop_id == d) {
                    bsp_insert(&server->bsp_roots[d], 0, view->id);
                }
            }
            server_apply_tiling(server, d);
        } else {
            server->desktop_mode[d] = DESKTOP_MODE_PHYSICS;
            
            // Restore physics coordinates
            FwmView *view;
            wl_list_for_each(view, &server->views, link) {
                PhysicsBody *b = physics_find_body(&server->physics, view->id);
                if (b && b->desktop_id == d) {
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
                    wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
                    if (view->scene_tree) {
                        wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(view->x - server->camera_x), (int)lround(view->y));
                    }
                }
            }
            
            // Push views flying
            wl_list_for_each(view, &server->views, link) {
                PhysicsBody *b = physics_find_body(&server->physics, view->id);
                if (b && b->desktop_id == d) {
                    double angle = ((double)(view->id % 628)) / 100.0;
                    b->vx = cos(angle) * 200.0;
                    b->vy = sin(angle) * 200.0;
                    b->flying = 1;
                }
            }
        }
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
            server->hints_buffer = hints_show(server->layer_overlay, server->screen_width, server->screen_height);
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
    } else if (strncmp(action, "view:", 5) == 0) {
        int desktop = atoi(action + 5);
        if (desktop >= 0 && desktop < 10) {
            server->target_camera_x = desktop * server->screen_width;
        }
    }
}

bool server_init(FwmServer *server) {
    memset(server, 0, sizeof(*server));
    server->wl_display = wl_display_create();
    if (!server->wl_display) {
        wlr_log(WLR_ERROR, "failed to create display");
        return false;
    }
    
    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);
    server->wlr_backend = wlr_backend_autocreate(event_loop, NULL);
    if (!server->wlr_backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return false;
    }
    
    server->wlr_renderer = wlr_renderer_autocreate(server->wlr_backend);
    if (!server->wlr_renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return false;
    }
    wlr_renderer_init_wl_display(server->wlr_renderer, server->wl_display);
    
    server->wlr_allocator = wlr_allocator_autocreate(server->wlr_backend, server->wlr_renderer);
    if (!server->wlr_allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return false;
    }

    wlr_compositor_create(server->wl_display, 5, server->wlr_renderer);
    wlr_subcompositor_create(server->wl_display);
    wlr_data_device_manager_create(server->wl_display);

    server->output_layout = wlr_output_layout_create(server->wl_display);

    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

    // Scene layers, created bottom-to-top: parallax wallpaper sits below the
    // windows, and the tray/hints/welcome overlays sit above them so a raised
    // window can never cover them.
    server->layer_background = wlr_scene_tree_create(&server->scene->tree);
    server->layer_windows = wlr_scene_tree_create(&server->scene->tree);
    server->layer_overlay = wlr_scene_tree_create(&server->scene->tree);

    wl_list_init(&server->views);
    wl_list_init(&server->outputs);
    wl_list_init(&server->keyboards);
    
    server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3); // xdg-shell v3/v6 depending on wlroots version (v3 is standard in 0.17+)
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);

    // Advertise xdg-decoration and force server-side mode so clients drop their
    // client-side titlebars (we draw none) and windows render borderless.
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration =
        wlr_xdg_decoration_manager_v1_create(server->wl_display);
    server->new_toplevel_decoration.notify = handle_new_toplevel_decoration;
    wl_signal_add(&xdg_decoration->events.new_toplevel_decoration, &server->new_toplevel_decoration);
    
    server->cursor = wlr_cursor_create();
    server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    // Load the theme and show a default cursor image immediately. Without this
    // the pointer has no image until a client sets one, so it looks "gone" for
    // the first few seconds after startup (and over the empty background).
    wlr_xcursor_manager_load(server->cursor_mgr, 1);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    server->cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
    server->cursor_button.notify = handle_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = handle_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
    
    server->seat = wlr_seat_create(server->wl_display, "seat0");
    server->request_cursor.notify = handle_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
    server->seat_request_set_selection.notify = handle_seat_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection, &server->seat_request_set_selection);
    
    server->new_input.notify = handle_new_input;
    wl_signal_add(&server->wlr_backend->events.new_input, &server->new_input);
    
    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->wlr_backend->events.new_output, &server->new_output);
    
    // Load config
    char path[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s%s", home, FWM_CONFIG_PATH);
    } else {
        snprintf(path, sizeof(path), ".config/fwm/config.toml");
    }
    config_load(&server->config, path);
    
    // Init physics
    physics_init(&server->physics);
    server->physics.friction = server->config.physics.friction;
    server->physics.mass_density = server->config.physics.mass_density;
    server->physics.throw_speed_multiplier = server->config.physics.throw_speed_multiplier;
    server->physics.max_throw_speed = server->config.physics.max_throw_speed;
    server->physics.stop_speed_threshold = server->config.physics.stop_speed_threshold;
    server->physics.restitution = server->config.physics.restitution;
    server->physics.gravity = server->config.physics.gravity;
    
    server->camera_x = 0;
    server->target_camera_x = 0;
    
    // Setup physics timer callback (60Hz)
    server->physics_timer = wl_event_loop_add_timer(event_loop, physics_tick_cb, server);
    wl_event_source_timer_update(server->physics_timer, (int)(1000.0 / PHYSICS_TICK_RATE));

    // Held-key auto-repeat timer for repeatable binds (armed on demand).
    server->key_repeat_timer = wl_event_loop_add_timer(event_loop, key_repeat_cb, server);
    server->repeat_action = NULL;
    server->repeat_keycode = 0;

    // Must be created after the backend (which may itself connect to an
    // upstream WAYLAND_DISPLAY when nested) so we don't clobber that env var
    // with our own socket before the backend has a chance to read it.
    const char *socket = wl_display_add_socket_auto(server->wl_display);
    if (!socket) {
        wlr_log(WLR_ERROR, "failed to create socket");
        return false;
    }
    wlr_log(WLR_INFO, "Wayland socket: %s", socket);
    setenv("WAYLAND_DISPLAY", socket, 1);

    server->running = 1;
    return true;
}

void server_run(FwmServer *server) {
    if (!wlr_backend_start(server->wlr_backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return;
    }
    
    wl_display_run(server->wl_display);
}

void server_destroy(FwmServer *server) {
    config_free(&server->config);
    
    // Clean overlays
    if (server->tray_buffer) cairo_overlay_destroy(server->tray_buffer);
    if (server->hints_buffer) cairo_overlay_destroy(server->hints_buffer);
    if (server->welcome_buffer) cairo_overlay_destroy(server->welcome_buffer);
    if (server->wallpaper) wallpaper_destroy(server->wallpaper);
    
    if (server->physics_timer) {
        wl_event_source_remove(server->physics_timer);
    }
    if (server->key_repeat_timer) {
        wl_event_source_remove(server->key_repeat_timer);
    }
    physics_destroy(&server->physics);

    wl_display_destroy_clients(server->wl_display);
    wl_display_destroy(server->wl_display);
}

void server_focus_view(FwmServer *server, struct FwmView *view) {
    if (server->focused_view == view) return;
    
    struct FwmView *prev_focus = server->focused_view;
    server->focused_view = view;
    
    if (view) {
        wlr_scene_node_raise_to_top(&view->scene_tree->node);
        
        struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
        if (kbd) {
            wlr_seat_keyboard_notify_enter(server->seat, view->xdg_toplevel->base->surface,
                kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
        }
        
        PhysicsBody *pb = physics_find_body(&server->physics, view->id);
        if (pb) {
            pb->corner_mode = CORNER_ROUND;
        }
        view_set_border_color(view, server->config.decor.col_active);
    } else {
        wlr_seat_keyboard_clear_focus(server->seat);
    }

    if (prev_focus) {
        PhysicsBody *pb = physics_find_body(&server->physics, prev_focus->id);
        if (pb) {
            int d = pb->desktop_id;
            pb->corner_mode = (server->desktop_mode[d] == DESKTOP_MODE_PHYSICS) ? CORNER_CHAMFER : CORNER_SHARP;
        }
        view_set_border_color(prev_focus, server->config.decor.col_inactive);
    }
    
    server_request_tray_redraw(server);
}

void server_apply_tiling(FwmServer *server, int desktop) {
    int cx = desktop * server->screen_width;
    int gin  = server->config.tiling.gaps_in;
    int gout = server->config.tiling.gaps_out;
    int top = TRAY_HEIGHT + gout;
    int usable_h = server->screen_height - top - gout;
    int usable_w = server->screen_width - gout * 2;

    bsp_recalc(server->bsp_roots[desktop], cx + gout, top, usable_w, usable_h, gin);

    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(server->bsp_roots[desktop], leaves, &count);

    // No glide while the user drags a BSP border: the layout must track the
    // mouse 1:1, a lagging animation there feels like jelly.
    int animate = server->config.tiling.anim_speed > 0.0 &&
                  server->interactive.action != FWM_ACTION_BSP_RESIZE;

    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        PhysicsBody *pb = physics_find_body(&server->physics, n->id);
        if (!pb) continue;
        // Size applies immediately (clients resize asynchronously anyway);
        // position glides there via the tile animation in physics_tick_cb.
        // `tiled` turns the body into a static anchor: the layout owns tiles,
        // physics must never shove them (transient overlaps while several
        // windows glide to new slots used to scatter finished ones).
        pb->tiled = 1;
        pb->width = n->w;
        pb->height = n->h;
        pb->vx = 0;
        pb->vy = 0;
        pb->flying = 0;

        // Update client view size and position
        FwmView *view = NULL;
        FwmView *v;
        wl_list_for_each(v, &server->views, link) {
            if (v->id == n->id) {
                view = v;
                break;
            }
        }
        if (!view) {
            pb->x = n->x;
            pb->y = n->y;
            continue;
        }

        view->width = pb->width;
        view->height = pb->height;
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);

        if (animate) {
            view->tile_anim = 1;
            view->tile_tx = n->x;
            view->tile_ty = n->y;
        } else {
            view->tile_anim = 0;
            pb->x = n->x;
            pb->y = n->y;
            view->x = pb->x;
            view->y = pb->y;
            if (view->scene_tree) {
                wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(view->x - server->camera_x), (int)lround(view->y));
            }
        }
    }
}

void server_start_interactive_move(FwmServer *server, struct FwmView *view, uint32_t serial) {
    (void)serial;
    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    if (!pb || pb->pinned) return;
    
    int tiling = (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING);
    if (tiling) {
        // Swap drag in tiling mode requires shift key, handled in handle_cursor_button
        return;
    }
    
    server->interactive.action = FWM_ACTION_MOVE;
    server->interactive.view = view;
    server->interactive.start_x = server->cursor->x;
    server->interactive.start_y = server->cursor->y;
    server->interactive.view_start_x = view->x - server->camera_x;
    server->interactive.view_start_y = view->y;
    server->interactive.view_start_width = view->width;
    server->interactive.view_start_height = view->height;
    server->interactive.last_x = server->cursor->x;
    server->interactive.last_y = server->cursor->y;
    clock_gettime(CLOCK_MONOTONIC, &server->interactive.last_time);
    server->interactive.vx = 0;
    server->interactive.vy = 0;
    server->interactive.hist_count = 0;
    server->interactive.collision_disabled = 0;
    
    physics_stop_body(&server->physics, view->id);
}

void server_start_interactive_resize(FwmServer *server, struct FwmView *view, uint32_t edges, uint32_t serial) {
    (void)serial;
    (void)edges;
    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    if (!pb || pb->pinned) return;
    if (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) return;
    
    server->interactive.action = FWM_ACTION_RESIZE;
    server->interactive.view = view;
    server->interactive.start_x = server->cursor->x;
    server->interactive.start_y = server->cursor->y;
    server->interactive.view_start_x = view->x - server->camera_x;
    server->interactive.view_start_y = view->y;
    server->interactive.view_start_width = view->width;
    server->interactive.view_start_height = view->height;
    
    physics_stop_body(&server->physics, view->id);
}

void server_set_fullscreen(FwmServer *server, struct FwmView *view, bool fullscreen, bool real) {
    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    if (!b) return;
    
    int d = b->desktop_id;
    if (fullscreen) {
        if (!b->fullscreen) {
            b->sav_x = b->x; b->sav_y = b->y;
            b->sav_w = b->width; b->sav_h = b->height;
        }
        b->fullscreen = 1;
        b->flying = 0; b->vx = 0; b->vy = 0;
        
        // Real fullscreen covers the whole output; fake fullscreen fills the
        // work area below the status bar so the tray stays visible.
        int top = real ? 0 : (TRAY_HEIGHT + 20);
        view->x = d * server->screen_width;
        view->y = top;
        view->width = server->screen_width;
        view->height = server->screen_height - top;
        
        // Keep the physics body in sync with the fullscreen geometry, otherwise
        // physics_tick_cb re-syncs the scene node back to the body's stale
        // position every tick and the window snaps out of fullscreen.
        b->x = view->x; b->y = view->y;
        b->width = view->width; b->height = view->height;
        
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
        wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, real);
        
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(view->x - server->camera_x), (int)lround(view->y));
            wlr_scene_node_raise_to_top(&view->scene_tree->node);
        }
        view_set_border_enabled(view, 0); // borderless fullscreen
        view->fs_real = real ? 1 : 0;
    } else {
        if (b->fullscreen) {
            b->fullscreen = 0;
            b->x = b->sav_x; b->y = b->sav_y;
            b->width = b->sav_w; b->height = b->sav_h;
            
            view->x = b->x; view->y = b->y;
            view->width = b->width; view->height = b->height;
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->width, view->height);
            wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, false);
            
            if (view->scene_tree) {
                wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(view->x - server->camera_x), (int)lround(view->y));
            }
            view_set_border_enabled(view, 1);
            view->fs_real = 0;
            if (server->desktop_mode[d] == DESKTOP_MODE_TILING) {
                server_apply_tiling(server, d);
            }
        }
    }
    
    server_request_tray_redraw(server);
}

void server_request_tray_redraw(FwmServer *server) {
    if (!server->tray_buffer) return;
    
    TrayData data = {0};
    for (int i = 0; i < 10; i++) {
        data.desktop_window_counts[i] = 0;
    }
    
    // Count windows per desktop
    for (int i = 0; i < server->physics.body_count; i++) {
        PhysicsBody *body = &server->physics.bodies[i];
        if (body->active) {
            int d = (int)((body->x + body->width / 2.0) / server->screen_width);
            if (d >= 0 && d < 10) {
                data.desktop_window_counts[d]++;
            }
        }
    }
    
    data.active_desktop = (server->camera_x + server->screen_width / 2) / server->screen_width;
    if (data.active_desktop < 0) data.active_desktop = 0;
    if (data.active_desktop >= 10) data.active_desktop = 9;
    
    if (server->focused_view) {
        PhysicsBody *b = physics_find_body(&server->physics, server->focused_view->id);
        if (b) {
            data.win_name = server->focused_view->xdg_toplevel->title;
            if (!data.win_name) data.win_name = "Window";
            data.speed = hypot(b->vx, b->vy);
            data.angle = atan2(b->vy, b->vx) * 180.0 / M_PI;
            data.mass = b->mass;
            data.flying = b->flying;
        }
    }
    
    tray_redraw(server->tray_buffer, &data);
}
