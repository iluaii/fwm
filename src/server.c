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

static void server_dispatch_action(FwmServer *server, const char *action);

/* SIGTERM/SIGINT arrive when the session ends or a dev run is killed. Without
 * these the process dies where it stands, server_destroy never runs, and the
 * control socket is left behind as a dead file. Terminating the display loop
 * instead makes the normal teardown path the only exit path. */
static int handle_signal(int signal, void *data) {
    FwmServer *server = data;
    wlr_log(WLR_INFO, "caught signal %d, shutting down", signal);
    server->running = 0;
    wl_display_terminate(server->wl_display);
    return 0;
}

static int test_action_cb(void *data) {
    FwmServer *server = data;
    if (server->test_action) {
        wlr_log(WLR_INFO, "FWM_TEST_ACTION: %s", server->test_action);
        server_dispatch_action(server, server->test_action);
        free(server->test_action);
        server->test_action = NULL;
    }
    return 0;
}
static void drag_icon_update_position(FwmServer *server);
static void server_shake_tick(FwmServer *server, double dt);
static FwmView *server_find_view(FwmServer *server, uint32_t id);
static FwmView *view_from_surface(FwmServer *server, struct wlr_surface *surface);
static void constraints_follow_focus(FwmServer *server, struct wlr_surface *surface);

/* Any real user input resets the idle timers of ext-idle-notify clients
 * (swayidle and friends). Must be called from every input path, or the session
 * dims while the user is actively working. */
static void server_notify_activity(FwmServer *server) {
    if (server->idle_notifier) {
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    }
    /* Input may set something moving (a throw, a drag, a bind), so leave the
     * idle heartbeat at once rather than waiting it out.
     * ONLY on the idle->busy edge: re-arming the timer on every motion event
     * would push its expiry out again each time, and a 1000Hz mouse would
     * starve the tick completely. */
    if (server->tick_idle && server->physics_timer) {
        server->tick_idle = 0;
        wl_event_source_timer_update(server->physics_timer, 1);
    }
}

/* Only continuous navigation binds should auto-repeat while held. Repeating
 * one-shot actions (killclient, spawn, toggles, view/fullscreen) would be
 * destructive or spammy, so they fire once per press only. */
static bool action_is_repeatable(const char *action) {
    return strncmp(action, "move_camera:", 12) == 0;
}

/* Call around any code that may toggle the launcher. Opening it takes the
 * keyboard away from the focused client via a wl_keyboard leave — per the
 * Wayland spec the client must then treat every key as released, which is
 * what prevents "stuck" keys (e.g. the space of a super+space toggle bind,
 * whose press was already forwarded but whose release we swallow). Closing
 * re-enters the focused surface. */
static void launcher_grab_sync(FwmServer *server, bool was_open) {
    bool open = launcher_is_open(server->launcher);
    if (open == was_open) return;
    if (open) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    } else if (server->focused_view) {
        struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
        struct wlr_surface *surface = view_surface(server->focused_view);
        if (!surface) return;
        if (kbd) {
            wlr_seat_keyboard_notify_enter(server->seat, surface,
                kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
        } else {
            wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
        }
    }
}

static void key_repeat_stop(FwmServer *server) {
    server->repeat_action = NULL;
    server->repeat_l_active = 0;
    server->repeat_keycode = 0;
    if (server->key_repeat_timer) {
        wl_event_source_timer_update(server->key_repeat_timer, 0);
    }
}

static int key_repeat_cb(void *data) {
    FwmServer *server = data;
    if (server->repeat_l_active) {
        // Launcher repeat: only while it is still open (Enter/Escape/click
        // may have closed it since the key went down).
        if (launcher_is_open(server->launcher)) {
            launcher_handle_key(server->launcher, server->repeat_l_sym, server->repeat_l_utf8);
            wl_event_source_timer_update(server->key_repeat_timer, 40);
        } else {
            key_repeat_stop(server);
        }
        return 0;
    }
    if (!server->repeat_action) return 0;
    server_dispatch_action(server, server->repeat_action);
    // Re-arm at the keyboard repeat rate (25/s -> 40ms).
    wl_event_source_timer_update(server->key_repeat_timer, 40);
    return 0;
}

/* Try to match+dispatch a bind for any of `syms`. Returns 1 when consumed. */
static int try_binds(FwmServer *server, struct wlr_keyboard_key_event *event,
                     const xkb_keysym_t *syms, int num_syms, uint32_t active_mods) {
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
                // Consumed by the compositor: the client never sees this key
                // (forwarding it too made super+g type a 'g' into the client).
                if (event->keycode < sizeof(server->key_consumed)) {
                    server->key_consumed[event->keycode] = 1;
                }
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
                return 1;
            }
        }
    }
    return 0;
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
    struct FwmKeyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;
    FwmServer *server = keyboard->server;

    server_notify_activity(server);
    
    // VT switching (Ctrl+Alt+F1..F12): xkb maps the chord to XF86Switch_VT_*
    // keysyms; only meaningful on the DRM backend where we own a session
    // (nested backends have none). Checked before the lock gate below on
    // purpose — switching VT leads to a login prompt on another TTY, not into
    // this session, so it stays available while locked.
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t keycode = event->keycode + 8;
        const xkb_keysym_t *syms;
        int num_syms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);
        for (int i = 0; i < num_syms; i++) {
            xkb_keysym_t sym = syms[i];
            if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
                if (server->session) {
                    wlr_session_change_vt(server->session,
                                          sym - XKB_KEY_XF86Switch_VT_1 + 1);
                }
                if (event->keycode < sizeof(server->key_consumed))
                    server->key_consumed[event->keycode] = 1;
                return;
            }
        }
    }

    /* Locked: the lock surface owns the keyboard. No binds, no launcher, no
     * overlay shortcuts — a bind still firing here (spawn:kitty, EXIT,
     * killclient) would defeat the lock outright. Forward straight to the seat,
     * which is focused on the lock surface, so the password field gets keys. */
    if (lock_is_active(server)) {
        wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
                                     event->keycode, event->state);
        return;
    }

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
                    if (event->keycode < sizeof(server->key_consumed))
                        server->key_consumed[event->keycode] = 1;
                    return;
                }
                if (server->hints_buffer) {
                    cairo_overlay_destroy(server->hints_buffer);
                    server->hints_buffer = NULL;
                    if (event->keycode < sizeof(server->key_consumed))
                        server->key_consumed[event->keycode] = 1;
                    return;
                }
            }
        }
    }
    
    // While the launcher is open it owns the keyboard entirely: feed it
    // presses (keysym + the text they produce) and swallow releases too, so
    // clients never see stray events from launcher typing.
    if (launcher_is_open(server->launcher)) {
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            uint32_t kc = event->keycode + 8;
            xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->wlr_keyboard->xkb_state, kc);
            char utf8[16] = "";
            xkb_state_key_get_utf8(keyboard->wlr_keyboard->xkb_state, kc, utf8, sizeof(utf8));
            launcher_handle_key(server->launcher, sym, utf8);
            launcher_grab_sync(server, true); /* Escape/Enter may have closed it */

            // Wayland auto-repeat is client-side, and while the launcher is
            // open no client sees the keyboard — so repeat held navigation
            // and typing keys ourselves (same delay/rate as bind repeat).
            bool repeatable = sym == XKB_KEY_Up || sym == XKB_KEY_Down
                || sym == XKB_KEY_Tab || sym == XKB_KEY_BackSpace
                || (utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f);
            if (repeatable && server->key_repeat_timer) {
                server->repeat_l_active = 1;
                server->repeat_l_sym = sym;
                memcpy(server->repeat_l_utf8, utf8, sizeof(server->repeat_l_utf8));
                server->repeat_keycode = event->keycode;
                server->repeat_action = NULL;
                wl_event_source_timer_update(server->key_repeat_timer, 300);
            } else {
                key_repeat_stop(server);
            }
        } else if (server->repeat_l_active && event->keycode == server->repeat_keycode) {
            key_repeat_stop(server);
        }
        return;
    }

    if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        // Stop auto-repeat when the held bind key is released.
        if (server->repeat_action && event->keycode == server->repeat_keycode) {
            key_repeat_stop(server);
        }
        // If the press was eaten by a bind, eat the release too — the client
        // never saw the press, it must not see a stray release.
        if (event->keycode < sizeof(server->key_consumed) &&
            server->key_consumed[event->keycode]) {
            server->key_consumed[event->keycode] = 0;
            return;
        }
        wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
        return;
    }
    
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int num_syms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t active_mods = get_active_modifiers(server);

    // Layout-independent binds: with a non-Latin layout active (e.g. Russian),
    // the same key resolves to a Cyrillic keysym and "super+q" would go dead.
    // If the active-layout syms match nothing, retry with layout 0 (the first
    // in [input] kbd_layout — keep Latin first there).
    const xkb_keysym_t *syms0 = NULL;
    int num_syms0 = 0;
    struct xkb_keymap *kmap = keyboard->wlr_keyboard->keymap;
    if (kmap && xkb_keymap_num_layouts(kmap) > 1) {
        num_syms0 = xkb_keymap_key_get_syms_by_level(kmap, keycode, 0, 0, &syms0);
    }

    if (try_binds(server, event, syms, num_syms, active_mods)) return;
    if (try_binds(server, event, syms0, num_syms0, active_mods)) return;

    // No bind matched — only now does the client get the key. Clear any stale
    // consumed mark (e.g. the release was swallowed by the launcher instead of
    // the release path), so this press's release is forwarded normally.
    if (event->keycode < sizeof(server->key_consumed)) {
        server->key_consumed[event->keycode] = 0;
    }
    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
}

// Layout-independent shortcuts INSIDE clients (ctrl+c in a terminal on a
// Russian layout, etc.). We forward keycodes, not keysyms: the client resolves
// them with the xkb group we announce, so on group 1 (ru) ctrl+<key C> becomes
// ctrl+Cyrillic_es and every app-level shortcut dies. Apps cannot fix this —
// the group is ours to report. So while a non-Shift modifier is held we
// announce group 0 (the first entry in [input] kbd_layout — keep Latin first),
// making the client see the Latin keysym. Typing is untouched: Shift alone
// never triggers this, and ctrl/alt/super chords produce no text anyway.
static void notify_client_modifiers(struct FwmKeyboard *keyboard) {
    FwmServer *server = keyboard->server;
    struct wlr_keyboard *kbd = keyboard->wlr_keyboard;
    struct wlr_keyboard_modifiers mods = kbd->modifiers;

    if (mods.group != 0 && kbd->xkb_state &&
        (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ||
         xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_ALT,  XKB_STATE_MODS_EFFECTIVE) > 0 ||
         xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)) {
        mods.group = 0;
    }

    wlr_seat_set_keyboard(server->seat, kbd);
    wlr_seat_keyboard_notify_modifiers(server->seat, &mods);
}

static void handle_keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct FwmKeyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    notify_client_modifiers(keyboard);
    // Layout switches arrive as group changes inside the modifiers event —
    // refresh the tray's layout tag (its signature dedupes the no-op case).
    server_request_tray_redraw(keyboard->server);
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
    struct FwmKeyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

/* Build the keymap from [input] and hand it to one keyboard. Shared by device
 * hotplug and config reload, so a reloaded layout reaches keyboards already
 * attached. */
static void keyboard_apply_input_config(FwmServer *server, struct wlr_keyboard *kb) {
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    // [input] config: multiple layouts + a grp:* switch option give layout
    // switching for free — xkb tracks the active group internally.
    const InputConfig *in = &server->config.input;
    struct xkb_rule_names rules = {
        .layout  = in->kbd_layout[0]  ? in->kbd_layout  : NULL,
        .variant = in->kbd_variant[0] ? in->kbd_variant : NULL,
        .options = in->kbd_options[0] ? in->kbd_options : NULL,
    };
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        wlr_log(WLR_ERROR, "bad [input] xkb config, falling back to environment");
        keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    wlr_keyboard_set_keymap(kb, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(kb, in->repeat_rate, in->repeat_delay);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        struct FwmKeyboard *keyboard = calloc(1, sizeof(struct FwmKeyboard));
        keyboard->server = server;
        keyboard->wlr_keyboard = wlr_keyboard_from_input_device(device);
        
        keyboard_apply_input_config(server, keyboard->wlr_keyboard);
        
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


/* Purely visual animations advance HERE, immediately before the scene is
 * committed — NOT on the physics timer.
 *
 * The timer runs free at 60Hz while the output presents on its own vsync. Two
 * unsynchronised 60Hz clocks beat against each other: some presented frames
 * repeat the previous opacity step and others skip one, so a fade that is
 * perfectly even in the log arrives on screen visibly uneven ("the brightness
 * jumps"). Advancing from measured time at frame time gives every presented
 * frame a correctly timed value.
 *
 * Called once per output frame; a second output in the same instant sees
 * dt ~= 0 and changes nothing. */
/* How far a window rises into place as it opens. */
#define OPEN_RISE_PX 16.0

static void server_animate(FwmServer *server) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!server->last_anim.tv_sec && !server->last_anim.tv_nsec) {
        server->last_anim = now;
        return;
    }
    double dt = (double)(now.tv_sec - server->last_anim.tv_sec)
              + (double)(now.tv_nsec - server->last_anim.tv_nsec) / 1e9;
    if (dt <= 0.0) return;
    /* After a stall (VT switch, a big image decode) do not teleport every
     * animation to its end. */
    if (dt > 0.25) dt = 0.25;
    server->last_anim = now;

    /* Rate-limits itself to one write every few seconds, and only when the set
     * of running applications or their desktops actually changed. */
    session_maybe_save(server);

    server_shake_tick(server, dt);

    /* Squash rides frame time with the other visual ramps. */
    if (server->config.effects.squash > 0.0) {
        FwmView *sv;
        wl_list_for_each(sv, &server->views, link) view_squash_tick(sv, dt);
    }

    // Window open animation. The client's surface is never blended: it is
    // hidden until it has content, then shown fully opaque while a cover rect
    // we draw ourselves fades out over it and the window rises into place.
    if (server->config.decor.fade_in_ms > 0.0) {
        double step = dt * 1000.0 / server->config.decor.fade_in_ms;
        FwmView *fv;
        wl_list_for_each(fv, &server->views, link) {
            if (!fv->open_anim || !fv->scene_tree) continue;

            /* Waiting for the client's first real frame. Capped, so a client
             * that draws once and never commits again still appears. */
            if (fv->open_hold > 0) {
                fv->open_hold_ms += dt * 1000.0;
                if (fv->open_hold_ms < 150.0) continue;
                fv->open_hold = 0;
            }

            /* First frame with content: reveal the window at full opacity and
             * put the cover on top of it. */
            if (!fv->open_cover) {
                const FwmTheme *thm = theme_get();
                float cover[4] = { (float)thm->pill[0], (float)thm->pill[1],
                                   (float)thm->pill[2], 1.0f };
                int cw = fv->width > 0 ? fv->width : 1;
                int ch = fv->height > 0 ? fv->height : 1;
                fv->open_cover = wlr_scene_rect_create(fv->scene_tree, cw, ch, cover);
                if (!fv->open_cover) {   /* no cover: just show the window */
                    fv->open_anim = 0;
                    wlr_scene_node_set_enabled(&fv->scene_tree->node, true);
                    continue;
                }
                wlr_scene_node_raise_to_top(&fv->open_cover->node);
                wlr_scene_node_set_enabled(&fv->scene_tree->node, true);
            }

            fv->open_t += step;
            int done = fv->open_t >= 1.0;
            if (done) fv->open_t = 1.0;

            double t = fv->open_t;
            double e = t * t * (3.0 - 2.0 * t);   /* smoothstep */

            /* Cover fades away; the window rises the last few px into place. */
            const FwmTheme *thm = theme_get();
            float a = (float)(1.0 - e);
            float cover[4] = { (float)thm->pill[0] * a, (float)thm->pill[1] * a,
                               (float)thm->pill[2] * a, a };
            wlr_scene_rect_set_size(fv->open_cover,
                                    fv->width > 0 ? fv->width : 1,
                                    fv->height > 0 ? fv->height : 1);
            wlr_scene_rect_set_color(fv->open_cover, cover);
            wlr_scene_node_set_position(&fv->scene_tree->node,
                                        fv->x - server->camera_x,
                                        fv->y + (int)lround(OPEN_RISE_PX * (1.0 - e)));

            if (done) {
                wlr_scene_node_destroy(&fv->open_cover->node);
                fv->open_cover = NULL;
                fv->open_anim = 0;
                wlr_scene_node_set_position(&fv->scene_tree->node,
                                            fv->x - server->camera_x, fv->y);
            }
        }

        // Window fade-out: ghost snapshots of closed windows ramp 1 -> 0
        // over the same duration, then die (node destroyed, buffer released).
        FwmGhost *g, *g_tmp;
        wl_list_for_each_safe(g, g_tmp, &server->ghosts, link) {
            g->t += step;
            if (g->t >= 1.0) {
                wlr_scene_node_destroy(&g->scene_buffer->node);
                wlr_buffer_unlock(g->buffer);
                wl_list_remove(&g->link);
                free(g);
                continue;
            }
            // Mirror of the fade-in, so closing feels like the reverse of
            // opening rather than a different effect.
            double gt = g->t;
            double o = 1.0 - gt * gt * (3.0 - 2.0 * gt);
            wlr_scene_buffer_set_opacity(g->scene_buffer, (float)o);
            wlr_scene_node_set_position(&g->scene_buffer->node,
                                        (int)lround(g->x) - server->camera_x, (int)lround(g->y));
        }
    }

    // Panel appear animations (hints, errors, welcome).
    cairo_overlay_tick(dt);

    // Wallpaper cross-fade: drop the outgoing set once the new one is opaque.
    if (server->wallpaper_prev && wallpaper_fade_tick(server->wallpaper, dt)) {
        wallpaper_destroy(server->wallpaper_prev);
        server->wallpaper_prev = NULL;
    }
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
    struct FwmOutput *output = wl_container_of(listener, output, frame);
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(output->server->scene, output->wlr_output);
    server_animate(output->server);
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

    /* fwm is single-output: the first one to arrive sizes the whole world (see
     * below), and any further one renders that same world rather than getting
     * a desktop strip of its own. Say so, or a second monitor looks like a
     * rendering bug instead of a missing feature. */
    wlr_log(WLR_INFO, "output %s: %dx%d%s", wlr_output->name,
            wlr_output->width, wlr_output->height,
            server->screen_width == 0 ? " (sizes the world)"
                                      : " — EXTRA OUTPUT, fwm is single-output");
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

        server->welcome_buffer = welcome_show(server->layer_overlay, server->screen_width, server->screen_height, &server->config);

        // A config so broken that the built-in binds had to stand in is worth
        // explaining up front — the user's own keys will not work. Lesser
        // problems only light up the tray pill.
        if (server->config.fallback_binds && server->config.error_count > 0) {
            server->errors_buffer = errors_show(server->layer_overlay, server->screen_width,
                                                server->screen_height, &server->config);
            server_request_tray_redraw(server);
        }

        // Debug: FWM_TEST_CAMERA=N parks the camera on desktop N at startup.
        // Wallpaper panning and the desktop slide are otherwise only reachable
        // through keybinds, which a nested test run cannot press.
        const char *tc = getenv("FWM_TEST_CAMERA");
        if (tc) {
            int d = atoi(tc);
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            server->camera_x = d * server->screen_width;
            server->target_camera_x = server->camera_x;
            if (server->wallpaper) wallpaper_update(server->wallpaper, server->camera_x);
        }

        // Debug: FWM_TEST_ACTION=<action> dispatches one action a second after
        // startup. Key injection into a nested run is impossible, so this is
        // the only way to exercise a bind's code path in a test.
        const char *ta = getenv("FWM_TEST_ACTION");
        if (ta) {
            struct wl_event_loop *el = wl_display_get_event_loop(server->wl_display);
            server->test_action = strdup(ta);
            server->test_action_timer =
                wl_event_loop_add_timer(el, test_action_cb, server);
            if (server->test_action_timer) {
                wl_event_source_timer_update(server->test_action_timer, 1500);
            }
        }

        // Debug: FWM_TEST_GRAVITY=<scale> starts with gravity on. fwm boots in
        // zero-g and gravity is only reachable through cycle_gravity, so a
        // nested run can otherwise never make a window fall — which means the
        // impact effects (shake, squash) cannot be exercised at all.
        const char *tg = getenv("FWM_TEST_GRAVITY");
        if (tg) {
            double g = atof(tg);
            if (g < 0.0) g = 0.0;
            if (g > 2.0) g = 2.0;
            server->physics.gravity_scale = g;
        }

        // Debug: FWM_OPEN_PICKER=1 opens the wallpaper picker at startup —
        // same purpose as FWM_SHOW_HINTS, since a nested run has no way to
        // press the bind.
        if (getenv("FWM_OPEN_PICKER")) {
            launcher_toggle_wallpapers(server->launcher);
        }

        // Debug: FWM_SHOW_HINTS=1 opens the bind help at startup, so the
        // overlay can be checked in a nested run without a keyboard.
        if (getenv("FWM_SHOW_HINTS") && !server->hints_buffer) {
            server->hints_buffer = hints_show(server->layer_overlay, server->screen_width, server->screen_height, &server->config);
        }
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

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
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

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
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

static void handle_cursor_button(struct wl_listener *listener, void *data) {
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

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
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

static void handle_seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
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

static void handle_seat_start_drag(struct wl_listener *listener, void *data) {
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
static void idle_inhibit_refresh(FwmServer *server) {
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

static void handle_new_pointer_constraint(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_pointer_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;

    /* Activate immediately when the pointer is already over the requesting
     * surface — which is the normal case: a game grabs the mouse on click. */
    if (server->seat->pointer_state.focused_surface == constraint->surface) {
        constraint_set_active(server, constraint);
    }
}

/* swayidle and friends turning the display off. Without this nothing can blank
 * the screen: fwm has no idle blanking of its own and the physics tick keeps
 * scheduling frames forever, so the monitor would stay lit indefinitely. */
static void handle_output_power_set_mode(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, output_power_set_mode);
    (void)server;
    const struct wlr_output_power_v1_set_mode_event *event = data;
    if (!event->output || !event->output->allocator) return;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
    wlr_output_commit_state(event->output, &state);
    wlr_output_state_finish(&state);
}

/* cursor-shape-v1: clients name a cursor ("text", "grab") instead of supplying
 * a surface. Older clients keep using wl_pointer.set_cursor, which still works. */
static void handle_cursor_shape_request(struct wl_listener *listener, void *data) {
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

static void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data) {
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

static void handle_seat_request_start_drag(struct wl_listener *listener, void *data) {
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

/* Impacts only matter if the user can see them: a window landing on desktop 7
 * must not shake the view of someone working on desktop 0. */
#define SHAKE_MAX_PX      14.0
#define SHAKE_FULL_SPEED  2000.0  /* px/s that produces a full-strength shake */
#define SHAKE_DECAY       9.0     /* 1/s; ~0.3s until it dies out */

/* Peak deformation for a window taking `speed`. The shake wants gentle impacts
 * damped because it is intrusive; the squash wants them SEEN, so the curve
 * bends the other way — squaring it (measured) turned an ordinary landing at
 * ~600 px/s into a 3.7% dent.
 *
 * Linear was still too flat at the low end: window-on-window contacts in real
 * use land around 200-300 px/s (measured in a nested run: 0.053 and 0.067,
 * i.e. 5-7%, which reads as "there is no animation between windows"), while
 * only a long fall onto the floor reaches four figures. sqrt lifts the gentle
 * half of the range without touching the cap: 200 px/s -> 14%, 500 -> 22%,
 * 900+ -> the full 30%. */
#define SQUASH_FULL_SPEED 900.0
#define SQUASH_MAX_AMOUNT 0.24

static void server_squash_from_impact(FwmServer *server, uint32_t id,
                                      double nx, double ny, double speed) {
    double strength = server->config.effects.squash;
    if (strength <= 0.0 || id == 0) return;  /* id 0 is a wall, nothing to squash */
    FwmView *v = server_find_view(server, id);
    if (!v) return;

    double f = speed / SQUASH_FULL_SPEED;
    if (f > 1.0) f = 1.0;
    view_start_squash(v, nx, ny, strength * SQUASH_MAX_AMOUNT * sqrt(f));
}

static void server_consume_impacts(FwmServer *server) {
    double shake = server->config.effects.camera_shake;
    double squash = server->config.effects.squash;
    if (shake <= 0.0 && squash <= 0.0) { server->physics.impact_count = 0; return; }

    int visible_d = (server->camera_x + server->screen_width / 2) / server->screen_width;
    double want = 0.0;
    for (int i = 0; i < server->physics.impact_count; i++) {
        const PhysicsImpact *im = &server->physics.impacts[i];
        /* A contact point sits ON the surface it hit, so a wall impact lands
         * just OUTSIDE the play area: the right wall's inner face is at
         * 10*screen_width, which divides to desktop 10 — a desktop that does
         * not exist — and every hit against it was silently dropped. (The left
         * wall only escaped because C truncates -2/1920 toward zero.) Clamp
         * into the real range instead of trusting the division. */
        int impact_d = (int)(im->x / server->screen_width);
        if (impact_d < 0) impact_d = 0;
        if (impact_d > 9) impact_d = 9;
        if (impact_d != visible_d) continue;

        /* The normal points from A to B, so it faces the contact for A and
         * away from it for B — flip it for B. */
        server_squash_from_impact(server, im->id_a,  im->nx,  im->ny, im->speed);
        server_squash_from_impact(server, im->id_b, -im->nx, -im->ny, im->speed);

        double f = im->speed / SHAKE_FULL_SPEED;
        if (f > 1.0) f = 1.0;
        /* Squared so gentle bumps stay subtle and only real slams shake hard. */
        double mag = shake * SHAKE_MAX_PX * f * f;
        if (mag > want) want = mag;
    }
    if (shake <= 0.0) want = 0.0;
    /* Take the strongest impact of the frame rather than summing: three windows
     * landing together should not triple the shake. */
    if (want > server->shake_mag) {
        server->shake_mag = want;
        server->shake_t = 0.0;
    }
}

/* Advanced at FRAME time, like the other purely visual ramps (see
 * server_animate) — on the physics timer it would beat against vsync. */
static void server_shake_tick(FwmServer *server, double dt) {
    if (server->shake_mag <= 0.01) {
        if (server->shake_mag != 0.0) {
            server->shake_mag = 0.0;
            if (server->layer_windows)
                wlr_scene_node_set_position(&server->layer_windows->node, 0, 0);
            if (server->layer_background)
                wlr_scene_node_set_position(&server->layer_background->node, 0, 0);
        }
        return;
    }
    server->shake_t += dt;
    server->shake_mag *= exp(-SHAKE_DECAY * dt);

    /* Two different frequencies, or the offset would travel a straight
     * diagonal instead of reading as a shake. */
    int ox = (int)lround(server->shake_mag * sin(server->shake_t * 38.0));
    int oy = (int)lround(server->shake_mag * sin(server->shake_t * 47.0 + 1.3));

    /* Only the world shakes. The tray and panels stay put: UI jittering under
     * the cursor reads as a glitch, not as impact. */
    if (server->layer_windows)
        wlr_scene_node_set_position(&server->layer_windows->node, ox, oy);
    if (server->layer_background)
        wlr_scene_node_set_position(&server->layer_background->node, ox, oy);
}

/* How long the compositor may sit without driving a frame itself. Not a
 * refresh rate: client redraws and scene node moves damage the scene and
 * wlr_scene schedules a frame off that damage, so this is only a heartbeat. */
#define TICK_IDLE_MS 200

/* Is anything actually moving? Everything listed here is driven by our timers
 * rather than by client damage, so the frame loop must keep running for it.
 * Err on the side of "yes": a false busy costs some idle wakeups, a false idle
 * freezes an animation halfway. */
static int server_is_busy(FwmServer *server) {
    if (server->interactive.action != FWM_ACTION_NONE) return 1;
    if (server->camera_x != server->target_camera_x || server->cam_anim) return 1;
    if (server->shake_mag > 0.0) return 1;
    if (server->wallpaper_prev) return 1;              /* wallpaper cross-fade */
    if (!wl_list_empty(&server->ghosts)) return 1;     /* close animations */
    if (launcher_is_open(server->launcher)) return 1;  /* spring tiles */
    if (cairo_overlay_animating()) return 1;

    for (int i = 0; i < server->physics.body_count; i++) {
        const PhysicsBody *b = &server->physics.bodies[i];
        if (b->active && b->flying) return 1;
    }
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->open_anim || v->tile_anim || v->squash_buf) return 1;
    }
    return 0;
}

/* Hand focus to something sensible without waiting for the pointer to move.
 * Prefers whatever sits under the cursor, which is what focus-follows-pointer
 * would have chosen anyway; otherwise takes a window on `desktop` so arriving
 * somewhere never leaves the keyboard pointing at nothing.
 *
 * `skip` is the view being unmapped: it is still in the list and may still own
 * scene nodes when this runs, so it must be excluded explicitly. */
void server_refocus(FwmServer *server, int desktop, struct FwmView *skip) {
    struct wlr_surface *surface = NULL;
    double sx, sy;
    FwmView *under = view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (under && under != skip) {
        server_focus_view(server, under);
        return;
    }

    /* Nothing under the pointer: take the most recently mapped window on the
     * desktop. Views are inserted at the head, so the first match is the
     * newest — closest to what the user last worked with. */
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v == skip) continue;
        PhysicsBody *b = physics_find_body(&server->physics, v->id);
        /* No body means a hidden tab-stack member: not a focus candidate. */
        if (!b || b->desktop_id != desktop) continue;
        server_focus_view(server, v);
        return;
    }

    /* Genuinely empty desktop: drop the keyboard rather than leave it pointed
     * at a window the user can no longer see. */
    server->focused_view = NULL;
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

void server_schedule_frames(FwmServer *server) {
    struct FwmOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
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
    
    // Desktop-switch camera slide: fixed-duration ease-in-out instead of the
    // old exponential chase (fast jump + 1px/tick crawl tail). If the target
    // changes mid-flight, restart from the current position so it stays smooth.
    if (server->camera_x != server->target_camera_x || server->cam_anim) {
        // X11 clients place popups from their last-configured root coords;
        // tell them where they are once the camera comes to rest.
        int cam_settled = 0;

        if (server->cam_free) {
            // Continuous pan under a held bind: framerate-independent
            // exponential chase, same form as the tile glide. Unlike the slide
            // below it has no notion of "start over", so a target that moves
            // every 40ms costs nothing — the camera tracks it immediately and
            // coasts the last few px once the key is released.
            server->cam_anim = 0;
            int gap = server->target_camera_x - server->camera_x;
            if (gap != 0) {
                double speed = server->config.camera.free_speed;
                double k = speed > 0.0 ? 1.0 - exp(-speed * dt) : 1.0;
                int step = (int)lround(gap * k);
                if (step == 0) step = gap > 0 ? 1 : -1; // never stall sub-pixel
                server->camera_x += step;
                // Snap the last pixel: edge auto-scroll only ever fires while
                // camera_x == target_camera_x exactly.
                if (abs(server->target_camera_x - server->camera_x) <= 1) {
                    server->camera_x = server->target_camera_x;
                }
                cam_settled = server->camera_x == server->target_camera_x;
            }
        } else {
            if (!server->cam_anim || server->cam_anim_to != server->target_camera_x) {
                server->cam_anim = 1;
                server->cam_anim_from = server->camera_x;
                server->cam_anim_to = server->target_camera_x;
                server->cam_anim_t = 0.0;
            }
            double cam_ms = server->config.camera.anim_ms;
            server->cam_anim_t += cam_ms > 0.0 ? dt * 1000.0 / cam_ms : 1.0;
            double t = server->cam_anim_t;
            if (t >= 1.0) {
                server->camera_x = server->cam_anim_to;
                server->cam_anim = 0;
                cam_settled = 1;
            } else {
                // Cubic ease-in-out.
                double e = t < 0.5 ? 4.0 * t * t * t
                                   : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
                server->camera_x = server->cam_anim_from
                    + (int)lround((server->cam_anim_to - server->cam_anim_from) * e);
            }
        }

        if (cam_settled) {
            FwmView *xv;
            wl_list_for_each(xv, &server->views, link) view_sync_position(xv);

            /* Arriving on a desktop should hand the keyboard to something
             * there. Otherwise focus stays on the window you left behind and
             * typing goes to a desktop you can no longer see. Covers every way
             * of getting here — the view: binds, the tray, edge auto-scroll. */
            int arrived = server->camera_x / server->screen_width;
            if (arrived != server->focus_desktop) {
                server->focus_desktop = arrived;
                server_refocus(server, arrived, NULL);
            }
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

    // Tab bars follow their window's width (resize/tiling glides).
    group_tick(server);

    // Launcher: tile physics + overlay redraw while open.
    launcher_tick(server->launcher, dt);

    // Physics step
    physics_step(&server->physics, server->screen_width, server->screen_height,
                 drag_win, resize_win, dragged_win, dt);

    /* Impacts are only valid until the next step, so drain them here. */
    server_consume_impacts(server);

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

    idle_inhibit_refresh(server);

    // Parallax: shift each wallpaper layer by a fraction of the camera offset.
    wallpaper_update(server->wallpaper, server->camera_x);

    /* While anything is actually moving we drive the frame loop ourselves at
     * the full tick rate. Once everything settles we drop to a slow heartbeat
     * instead of spinning at 60Hz forever: client redraws and scene node moves
     * damage the scene, and wlr_scene schedules a frame off that damage on its
     * own, so nothing depends on us for those.
     *
     * The heartbeat is not just for the tray clock. It is the safety net that
     * the old unconditional schedule used to provide: should any path ever
     * change what is on screen without damaging the scene, the screen is stale
     * for at most one beat instead of forever. */
    int busy = server_is_busy(server);
    server_schedule_frames(server);

    /* physics_step always advances a FIXED 1/60 regardless of when we are
     * called, so slowing the timer cannot hand Box2D an enormous dt — idle
     * simply means nothing is moving for it to integrate. */
    server->tick_idle = !busy;
    wl_event_source_timer_update(server->physics_timer,
                                 busy ? (int)(1000.0 / PHYSICS_TICK_RATE) : TICK_IDLE_MS);
    return 0;
}

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

static void server_config_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (home) snprintf(buf, cap, "%s%s", home, FWM_CONFIG_PATH);
    else      snprintf(buf, cap, ".config/fwm/config.toml");
}

/* Close the error panel if it is open; the caller decides whether to reopen it
 * against the new config. */
static void server_close_errors_panel(FwmServer *server) {
    if (server->errors_buffer) {
        cairo_overlay_destroy(server->errors_buffer);
        server->errors_buffer = NULL;
    }
}

/* ~/.local/state/fwm/wallpaper — the picker's choice, kept out of config.toml
 * so the user's file (comments, formatting) is never rewritten by us. */
static void server_state_path(char *buf, size_t cap) {
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state && state[0]) snprintf(buf, cap, "%s/fwm/wallpaper", state);
    else if (home)         snprintf(buf, cap, "%s/.local/state/fwm/wallpaper", home);
    else                   snprintf(buf, cap, ".fwm-wallpaper");
}

static void server_state_save_wallpaper(const char *path) {
    char sp[512];
    server_state_path(sp, sizeof(sp));

    /* mkdir -p of the parent, one component at a time. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", sp);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        for (char *p = dir + 1; *p; p++) {
            if (*p != '/') continue;
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
        mkdir(dir, 0755);
    }

    FILE *f = fopen(sp, "w");
    if (!f) {
        wlr_log(WLR_ERROR, "cannot save wallpaper choice to %s", sp);
        return;
    }
    fprintf(f, "%s\n", path);
    fclose(f);
}

/* Apply the remembered wallpaper over the configured one. Called after every
 * config load, so a reload keeps the picked image rather than snapping back. */
static void server_state_apply_wallpaper(FwmServer *server) {
    char sp[512];
    server_state_path(sp, sizeof(sp));
    FILE *f = fopen(sp, "r");
    if (!f) return;

    char line[512];
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        /* A stale entry (image deleted since) must not blank the wallpaper —
         * fall through to whatever the config says. */
        if (line[0] && access(line, R_OK) == 0) {
            if (server->config.wallpaper_count <= 0) {
                WallpaperLayer *w = calloc(1, sizeof(WallpaperLayer));
                if (w) {
                    w->fit = WALLPAPER_FIT_COVER;
                    server->config.wallpapers = w;
                    server->config.wallpaper_count = 1;
                }
            }
            if (server->config.wallpaper_count > 0)
                snprintf(server->config.wallpapers[0].path,
                         sizeof(server->config.wallpapers[0].path), "%s", line);
        }
    }
    fclose(f);
}

void server_set_wallpaper(FwmServer *server, const char *path) {
    if (!path || !path[0]) return;
    if (access(path, R_OK) != 0) {
        wlr_log(WLR_ERROR, "wallpaper '%s' is not readable", path);
        return;
    }

    /* No [[wallpaper]] in the config: start one, keeping "cover" — a lone
     * layer with pan semantics would scroll an image the user never asked to
     * walk across. */
    if (server->config.wallpaper_count <= 0) {
        WallpaperLayer *w = calloc(1, sizeof(WallpaperLayer));
        if (!w) return;
        w->fit = WALLPAPER_FIT_COVER;
        server->config.wallpapers = w;
        server->config.wallpaper_count = 1;
    }
    /* Only the path changes: fit and zoom stay as configured, so a "pan"
     * setup keeps panning with the new image. */
    snprintf(server->config.wallpapers[0].path,
             sizeof(server->config.wallpapers[0].path), "%s", path);

    /* Cross-fade rather than cut: the outgoing set stays underneath (the new
     * one is created later, so the scene draws it on top) until the fade ends.
     * A swap still in flight is finished immediately, so rapid picking cannot
     * pile up wallpapers. */
    if (server->wallpaper_prev) {
        wallpaper_destroy(server->wallpaper_prev);
        server->wallpaper_prev = NULL;
    }
    server->wallpaper_prev = server->wallpaper;
    server->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                         server->screen_width, server->screen_height);
    if (server->wallpaper) {
        wallpaper_update(server->wallpaper, server->camera_x);
        if (server->wallpaper_prev) {
            wallpaper_fade_in(server->wallpaper, server->config.decor.wallpaper_fade_ms);
        }
    }
    if (!server->wallpaper || server->config.decor.wallpaper_fade_ms <= 0.0) {
        if (server->wallpaper_prev) {
            wallpaper_destroy(server->wallpaper_prev);
            server->wallpaper_prev = NULL;
        }
    }

    /* The palette may be derived from the image that just changed. */
    theme_build(&server->config);
    server_request_tray_redraw(server);

    server_state_save_wallpaper(path);
    wlr_log(WLR_INFO, "wallpaper set to %s", path);
}

/* Push the current FwmConfig onto the live compositor.
 *
 * Split out of server_reload_config so that a single `fwmctl set` can reuse
 * exactly the same re-apply path as a full reload — the alternative, a
 * per-option apply hook, is a second place to forget about.
 *
 * rebuild_wallpaper is a parameter rather than always-on because rebuilding
 * decodes the image from disk: fine once per reload, far too expensive for a
 * knob someone is dragging through a range. */
void server_apply_config(FwmServer *server, int rebuild_wallpaper) {
    /* Before anything reads colours: a new wallpaper or color_source repaints
     * the whole system. */
    theme_build(&server->config);

    /* Physics knobs are plain scalars on the live world. */
    server->physics.friction              = server->config.physics.friction;
    server->physics.mass_density          = server->config.physics.mass_density;
    server->physics.throw_speed_multiplier = server->config.physics.throw_speed_multiplier;
    server->physics.max_throw_speed       = server->config.physics.max_throw_speed;
    server->physics.stop_speed_threshold  = server->config.physics.stop_speed_threshold;
    server->physics.restitution           = server->config.physics.restitution;
    server->physics.gravity               = server->config.physics.gravity;

    /* Keyboards: layout/variant/options/repeat may all have changed. */
    struct FwmKeyboard *kb;
    wl_list_for_each(kb, &server->keyboards, link) {
        keyboard_apply_input_config(server, kb->wlr_keyboard);
    }

    /* Borders: width and colours are read per view. */
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        view_update_border_geometry(view);
        view_set_border_color(view, view == server->focused_view
                                    ? theme_get()->border_active
                                    : theme_get()->border_inactive);
    }

    /* Wallpaper layers are baked at load time, so rebuild them wholesale. */
    if (rebuild_wallpaper) {
        if (server->wallpaper_prev) {
            wallpaper_destroy(server->wallpaper_prev);
            server->wallpaper_prev = NULL;
        }
        if (server->wallpaper) {
            wallpaper_destroy(server->wallpaper);
            server->wallpaper = NULL;
        }
        if (server->config.wallpaper_count > 0) {
            server->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                                 server->screen_width, server->screen_height);
            if (server->wallpaper) wallpaper_update(server->wallpaper, server->camera_x);
        }
    }

    /* New gaps / anim settings take effect on tiled desktops. */
    for (int d = 0; d < 10; d++) {
        if (server->desktop_mode[d] == DESKTOP_MODE_TILING) server_apply_tiling(server, d);
    }

    server_request_tray_redraw(server);
}

void server_reload_config(FwmServer *server) {
    /* Held-key repeat points into config.keys[].action, which is about to be
     * freed — disarm it before the old config goes away. */
    server->repeat_action = NULL;
    server->repeat_keycode = 0;
    if (server->key_repeat_timer) wl_event_source_timer_update(server->key_repeat_timer, 0);

    /* Panels are rebuilt from the new config rather than patched. */
    server_close_errors_panel(server);
    if (server->hints_buffer) {
        cairo_overlay_destroy(server->hints_buffer);
        server->hints_buffer = NULL;
    }

    char path[512];
    server_config_path(path, sizeof(path));
    config_free(&server->config);
    config_load(&server->config, path);
    server_state_apply_wallpaper(server);

    /* Rereading the file also discards any `fwmctl set` overrides — the file
     * is the source of truth, and this is the documented way back to it. */
    server_apply_config(server, 1);

    /* Surface whatever the new file got wrong straight away. */
    if (server->config.error_count > 0) {
        server->errors_buffer = errors_show(server->layer_overlay, server->screen_width,
                                            server->screen_height, &server->config);
    }
    wlr_log(WLR_INFO, "config reloaded from %s (%d problem(s))",
            path, server->config.error_total);
}

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
}

/* Kept as the entry point for the toggle_tiling bind: physics <-> tiling, with
 * floating collapsing to tiling so the key never becomes a no-op. */
static void server_toggle_desktop_tiling(FwmServer *server, int d) {
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
static void server_move_view_to_desktop(FwmServer *server, FwmView *view, int target,
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

static void server_toggle_desktop_floating(FwmServer *server, int d) {
    server_set_desktop_mode(server, d,
        server->desktop_mode[d] == DESKTOP_MODE_FLOATING
            ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_FLOATING);
}

static void server_dispatch_action(FwmServer *server, const char *action) {
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

bool server_init(FwmServer *server) {
    memset(server, 0, sizeof(*server));
    server->wl_display = wl_display_create();
    if (!server->wl_display) {
        wlr_log(WLR_ERROR, "failed to create display");
        return false;
    }
    
    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);

    /* MUST happen before the backend/renderer, and thus before the GPU driver
     * spawns its worker threads. wl_event_loop_add_signal blocks the signal
     * with sigprocmask, and a thread inherits the mask AS IT IS AT CREATION —
     * threads that already exist keep accepting the signal. The kernel hands a
     * process-directed signal to any thread that does not block it, so with
     * this registered later the driver's threads took SIGTERM and the default
     * disposition killed us (exit 143) with server_destroy never running.
     * Verified by reading SigBlk from each thread's status under
     * /proc/<pid>/task: it was 0 on those threads. */
    wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, server);
    wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, server);

    server->wlr_backend = wlr_backend_autocreate(event_loop, &server->session);
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

    server->compositor = wlr_compositor_create(server->wl_display, 5, server->wlr_renderer);
    wlr_subcompositor_create(server->wl_display);
    wlr_data_device_manager_create(server->wl_display);
    // Screen capture protocol: lets wf-recorder record and grim screenshot.
    wlr_screencopy_manager_v1_create(server->wl_display);
    // Standard client protocols, all self-contained in wlroots:
    // primary selection = middle-click paste; viewporter + fractional-scale +
    // single-pixel-buffer are expected by games/video players/toolkits;
    // presentation-time gives clients accurate frame timing (mpv, games).
    wlr_primary_selection_v1_device_manager_create(server->wl_display);
    wlr_viewporter_create(server->wl_display);
    wlr_fractional_scale_manager_v1_create(server->wl_display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server->wl_display);
    wlr_presentation_create(server->wl_display, server->wlr_backend, 2);

    server->output_layout = wlr_output_layout_create(server->wl_display);
    // xdg-output: exposes output geometry to clients (grim/wf-recorder need it).
    wlr_xdg_output_manager_v1_create(server->wl_display, server->output_layout);

    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

    // Scene layers, created bottom-to-top: parallax wallpaper sits below the
    // windows, and the tray/hints/welcome overlays sit above them so a raised
    // window can never cover them.
    server->layer_background = wlr_scene_tree_create(&server->scene->tree);
    server->layer_windows = wlr_scene_tree_create(&server->scene->tree);
    server->layer_overlay = wlr_scene_tree_create(&server->scene->tree);

    // Layer-shell trees are woven between ours. Creation order alone cannot
    // express this (new trees always land on top), so place them explicitly:
    // wallpaper < ls_background < ls_bottom < windows < ls_top < our overlays
    // < ls_overlay. A layer-shell overlay therefore outranks even the tray,
    // which is what clients like a screen locker or a menu expect.
    server->ls_background = wlr_scene_tree_create(&server->scene->tree);
    server->ls_bottom = wlr_scene_tree_create(&server->scene->tree);
    server->ls_top = wlr_scene_tree_create(&server->scene->tree);
    server->ls_overlay = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_above(&server->ls_background->node, &server->layer_background->node);
    wlr_scene_node_place_above(&server->ls_bottom->node, &server->ls_background->node);
    wlr_scene_node_place_above(&server->layer_windows->node, &server->ls_bottom->node);
    wlr_scene_node_place_above(&server->ls_top->node, &server->layer_windows->node);
    wlr_scene_node_place_above(&server->layer_overlay->node, &server->ls_top->node);
    wlr_scene_node_place_above(&server->ls_overlay->node, &server->layer_overlay->node);
    /* The lock screen outranks everything, including an external bar's overlay
     * layer — nothing may be drawn over a locked session. Disabled until a
     * lock actually engages. */
    server->layer_lock = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_above(&server->layer_lock->node, &server->ls_overlay->node);
    wlr_scene_node_set_enabled(&server->layer_lock->node, false);

    server->launcher = launcher_create(server);

    wl_list_init(&server->views);
    wl_list_init(&server->groups);
    wl_list_init(&server->ghosts);
    wl_list_init(&server->outputs);
    wl_list_init(&server->keyboards);
    
    server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3); // xdg-shell v3/v6 depending on wlroots version (v3 is standard in 0.17+)
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    layer_shell_init(server);
    lock_init(server);
    foreign_init(server);

    server->new_xdg_popup.notify = handle_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

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

    // Xwayland (lazy: the X server starts on the first X11 client). Managed
    // windows become FwmViews, override-redirect ones bare scene surfaces.
    server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, true);
    if (server->xwayland) {
        server->xwl_ready.notify = handle_xwl_ready;
        wl_signal_add(&server->xwayland->events.ready, &server->xwl_ready);
        server->xwl_new_surface.notify = handle_xwl_new_surface;
        wl_signal_add(&server->xwayland->events.new_surface, &server->xwl_new_surface);
        setenv("DISPLAY", server->xwayland->display_name, true);
        wlr_log(WLR_INFO, "Xwayland on DISPLAY=%s", server->xwayland->display_name);
    } else {
        wlr_log(WLR_ERROR, "Failed to start Xwayland; X11 apps won't work");
    }

    server->request_cursor.notify = handle_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
    server->seat_request_set_selection.notify = handle_seat_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection, &server->seat_request_set_selection);
    server->seat_request_set_primary_selection.notify = handle_seat_request_set_primary_selection;
    wl_signal_add(&server->seat->events.request_set_primary_selection, &server->seat_request_set_primary_selection);
    server->seat_request_start_drag.notify = handle_seat_request_start_drag;
    wl_signal_add(&server->seat->events.request_start_drag, &server->seat_request_start_drag);
    server->seat_start_drag.notify = handle_seat_start_drag;
    wl_signal_add(&server->seat->events.start_drag, &server->seat_start_drag);

    /* No listeners needed: the notifier is driven by server_notify_activity
     * from the input paths, and the inhibit manager keeps its own list, which
     * idle_inhibit_refresh polls each tick. */
    server->relative_pointer = wlr_relative_pointer_manager_v1_create(server->wl_display);
    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->wl_display);
    if (server->pointer_constraints) {
        server->new_pointer_constraint.notify = handle_new_pointer_constraint;
        wl_signal_add(&server->pointer_constraints->events.new_constraint,
                      &server->new_pointer_constraint);
    }

    server->output_power = wlr_output_power_manager_v1_create(server->wl_display);
    if (server->output_power) {
        server->output_power_set_mode.notify = handle_output_power_set_mode;
        wl_signal_add(&server->output_power->events.set_mode, &server->output_power_set_mode);
    }
    server->gamma_control = wlr_gamma_control_manager_v1_create(server->wl_display);
    if (server->gamma_control) {
        /* The scene applies client ramps itself, including re-applying them
         * when an output comes back — far less to get wrong than committing
         * them by hand. We only watch the event to stand our own night light
         * down when a client takes the ramp over. */
        wlr_scene_set_gamma_control_manager_v1(server->scene, server->gamma_control);
    }
    server->cursor_shape = wlr_cursor_shape_manager_v1_create(server->wl_display, 1);
    if (server->cursor_shape) {
        server->cursor_shape_request.notify = handle_cursor_shape_request;
        wl_signal_add(&server->cursor_shape->events.request_set_shape,
                      &server->cursor_shape_request);
    }

    server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
    server->idle_inhibit  = wlr_idle_inhibit_v1_create(server->wl_display);
    server->idle_inhibited = 0;

    server->xdg_activation = wlr_xdg_activation_v1_create(server->wl_display);
    if (server->xdg_activation) {
        server->xdg_activation_request_activate.notify = handle_xdg_activation_request_activate;
        wl_signal_add(&server->xdg_activation->events.request_activate,
                      &server->xdg_activation_request_activate);
    }
    
    server->new_input.notify = handle_new_input;
    wl_signal_add(&server->wlr_backend->events.new_input, &server->new_input);
    
    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->wlr_backend->events.new_output, &server->new_output);
    
    // Load config
    char path[512];
    server_config_path(path, sizeof(path));
    config_load(&server->config, path);
    server_state_apply_wallpaper(server);
    // Palette for every overlay and window border; may sample the wallpaper.
    theme_build(&server->config);

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

    /* Control socket. Named after the Wayland display so several fwm
     * instances (a nested dev run inside a real session) never collide.
     * Failure is non-fatal: fwm works without it, just not scriptably. */
    server->ipc = ipc_create(server, socket);

    server->running = 1;
    return true;
}

void server_run(FwmServer *server) {
    if (!wlr_backend_start(server->wlr_backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return;
    }

    /* After the backend is up (so WAYLAND_DISPLAY is exported and the socket
     * accepts connections) but before the event loop blocks: the relaunched
     * clients connect into the loop we are about to enter. */
    session_restore(server);

    wl_display_run(server->wl_display);
}

/* Detach a listener that may never have been attached: server_init memsets the
 * whole struct, so an unused one still has a zeroed link and wl_list_remove
 * would walk a NULL pointer. */
static void server_remove_listener(struct wl_listener *l) {
    if (l->link.prev) wl_list_remove(&l->link);
}

void server_destroy(FwmServer *server) {
    /* Before anything else: stop accepting commands that would touch state we
     * are about to free, and take the socket file with us. */
    ipc_destroy(server->ipc);
    server->ipc = NULL;

    /* Order matters: the policy lives in the config, which is freed below. */
    session_clear_on_clean_exit(server);
    session_finish(server);
    config_free(&server->config);
    
    // Clean overlays
    if (server->tray_buffer) cairo_overlay_destroy(server->tray_buffer);
    if (server->hints_buffer) cairo_overlay_destroy(server->hints_buffer);
    if (server->welcome_buffer) cairo_overlay_destroy(server->welcome_buffer);
    if (server->errors_buffer) cairo_overlay_destroy(server->errors_buffer);
    if (server->wallpaper_prev) wallpaper_destroy(server->wallpaper_prev);
    if (server->wallpaper) wallpaper_destroy(server->wallpaper);
    launcher_destroy(server->launcher);
    
    if (server->physics_timer) {
        wl_event_source_remove(server->physics_timer);
    }
    if (server->key_repeat_timer) {
        wl_event_source_remove(server->key_repeat_timer);
    }
    physics_destroy(&server->physics);

    FwmGhost *g, *g_tmp;
    wl_list_for_each_safe(g, g_tmp, &server->ghosts, link) {
        wlr_scene_node_destroy(&g->scene_buffer->node);
        wlr_buffer_unlock(g->buffer);
        wl_list_remove(&g->link);
        free(g);
    }

    // Xwayland must go down before the clients/display it hangs off.
    if (server->xwayland) {
        wl_list_remove(&server->xwl_ready.link);
        wl_list_remove(&server->xwl_new_surface.link);
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
    }

    /* Every wlroots global we listen to asserts on destroy that its signals
     * have no listeners left (xdg_shell was the one that caught us), so all of
     * ours have to come off before wl_display_destroy runs them down.
     *
     * None of this ever ran before: without a signal handler the process was
     * killed outright and server_destroy was dead code on the normal exit
     * path. server_init memsets the struct, so a listener that was never added
     * still has a NULL link and is skipped. */
    server_remove_listener(&server->new_xdg_toplevel);
    server_remove_listener(&server->new_xdg_popup);
    server_remove_listener(&server->new_toplevel_decoration);
    server_remove_listener(&server->new_input);
    server_remove_listener(&server->new_output);
    server_remove_listener(&server->cursor_motion);
    server_remove_listener(&server->cursor_motion_absolute);
    server_remove_listener(&server->cursor_button);
    server_remove_listener(&server->cursor_axis);
    server_remove_listener(&server->cursor_frame);
    server_remove_listener(&server->request_cursor);
    server_remove_listener(&server->seat_request_set_selection);
    server_remove_listener(&server->seat_request_set_primary_selection);
    server_remove_listener(&server->seat_request_start_drag);
    server_remove_listener(&server->seat_start_drag);
    server_remove_listener(&server->new_pointer_constraint);
    server_remove_listener(&server->constraint_destroy);
    server_remove_listener(&server->output_power_set_mode);
    server_remove_listener(&server->cursor_shape_request);
    server_remove_listener(&server->xdg_activation_request_activate);
    /* Owned by other modules but attached to globals just the same. */
    server_remove_listener(&server->new_layer_surface);
    server_remove_listener(&server->new_lock);

    wl_display_destroy_clients(server->wl_display);
    wl_display_destroy(server->wl_display);
}

void server_focus_view(FwmServer *server, struct FwmView *view) {
    if (server->focused_view == view) return;
    
    struct FwmView *prev_focus = server->focused_view;
    server->focused_view = view;
    
    if (view) {
        if (view->scene_tree) wlr_scene_node_raise_to_top(&view->scene_tree->node);
        
        struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
        struct wlr_surface *surface = view_surface(view);
        if (kbd && surface) {
            wlr_seat_keyboard_notify_enter(server->seat, surface,
                kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
        }
        view_set_activated(view, true);
        foreign_view_set_activated(view, true);
        
        PhysicsBody *pb = physics_find_body(&server->physics, view->id);
        if (pb) {
            pb->corner_mode = CORNER_ROUND;
        }
        view_set_border_color(view, theme_get()->border_active);
    } else {
        wlr_seat_keyboard_clear_focus(server->seat);
    }

    if (prev_focus) {
        view_set_activated(prev_focus, false);
        foreign_view_set_activated(prev_focus, false);
        PhysicsBody *pb = physics_find_body(&server->physics, prev_focus->id);
        if (pb) {
            int d = pb->desktop_id;
            pb->corner_mode = (server->desktop_mode[d] == DESKTOP_MODE_PHYSICS) ? CORNER_CHAMFER : CORNER_SHARP;
        }
        view_set_border_color(prev_focus, theme_get()->border_inactive);
    }
    
    server_request_tray_redraw(server);
}

void server_apply_tiling(FwmServer *server, int desktop) {
    int cx = desktop * server->screen_width;
    int gin  = server->config.tiling.gaps_in;
    int gout = server->config.tiling.gaps_out;
    /* Start from the area layer-shell clients left us (a bar with an
     * exclusive zone shrinks it), never letting tiles run under our own tray. */
    struct wlr_box work = server->usable_area;
    if (work.width <= 0 || work.height <= 0) {
        work = (struct wlr_box){ 0, 0, server->screen_width, server->screen_height };
    }
    if (work.y < TRAY_BOTTOM) {
        work.height -= TRAY_BOTTOM - work.y;
        work.y = TRAY_BOTTOM;
    }
    int top = work.y + gout;
    int usable_h = work.height - gout * 2;
    int usable_w = work.width - gout * 2;

    bsp_recalc(server->bsp_roots[desktop], cx + work.x + gout, top, usable_w, usable_h, gin);

    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(server->bsp_roots[desktop], leaves, &count, MAX_WINDOWS);

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
        // A tab-stack draws its bar above the window, outside the client area.
        // The layout has to hand that strip over, or the bar hangs into the
        // slot above -- for the top row that is our tray.
        int nx = n->x, ny = n->y, nw = n->w, nh = n->h;
        if (view && view->group && nh > GROUP_TAB_H * 2) {
            ny += GROUP_TAB_H;
            nh -= GROUP_TAB_H;
        }
        pb->width = nw;
        pb->height = nh;

        if (!view) {
            pb->x = nx;
            pb->y = ny;
            continue;
        }

        view->width = pb->width;
        view->height = pb->height;
        view_set_size(view, view->width, view->height);

        if (animate) {
            view->tile_anim = 1;
            view->tile_tx = nx;
            view->tile_ty = ny;
        } else {
            view->tile_anim = 0;
            pb->x = nx;
            pb->y = ny;
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
        /* Fake fullscreen fills the work area: below our tray and clear of
         * any layer-shell bar that reserved space. */
        struct wlr_box work = server->usable_area;
        if (work.width <= 0 || work.height <= 0) {
            work = (struct wlr_box){ 0, 0, server->screen_width, server->screen_height };
        }
        int top = real ? 0 : (work.y > TRAY_BOTTOM + 12 ? work.y : TRAY_BOTTOM + 12);
        view->x = d * server->screen_width + (real ? 0 : work.x);
        view->y = top;
        view->width = real ? server->screen_width : work.width;
        view->height = server->screen_height - top;
        
        // Keep the physics body in sync with the fullscreen geometry, otherwise
        // physics_tick_cb re-syncs the scene node back to the body's stale
        // position every tick and the window snaps out of fullscreen.
        b->x = view->x; b->y = view->y;
        b->width = view->width; b->height = view->height;
        
        view_set_size(view, view->width, view->height);
        view_set_fullscreen_hint(view, real);
        
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
            view_set_size(view, view->width, view->height);
            view_set_fullscreen_hint(view, false);
            
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
    
    // Active keyboard layout tag, shown only when several are configured.
    // Prefer the short name from [input] kbd_layout ("us,ru" -> "US"/"RU");
    // fall back to the xkb layout name's first letters.
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (kbd && kbd->keymap && xkb_keymap_num_layouts(kbd->keymap) > 1) {
        xkb_layout_index_t idx =
            xkb_state_serialize_layout(kbd->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
        const char *src = server->config.input.kbd_layout;
        int cur = 0;
        const char *p = src;
        while (*p && cur < (int)idx) {
            if (*p == ',') cur++;
            p++;
        }
        if (*p && cur == (int)idx) {
            int n = 0;
            while (p[n] && p[n] != ',' && p[n] != '(' && n < 2) n++;
            for (int i = 0; i < n; i++) {
                char c = p[i];
                data.kbd_layout[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            data.kbd_layout[n] = '\0';
        } else {
            const char *name = xkb_keymap_layout_get_name(kbd->keymap, idx);
            if (name) snprintf(data.kbd_layout, sizeof(data.kbd_layout), "%.2s", name);
        }
    }

    data.opacity = server->config.decor.tray_opacity;
    data.error_count = server->config.error_total;
    data.error_expanded = server->errors_buffer != NULL;
    data.active_pos = (double)server->camera_x / server->screen_width;
    if (data.active_pos < 0.0) data.active_pos = 0.0;
    if (data.active_pos > 9.0) data.active_pos = 9.0;
    data.active_desktop = (server->camera_x + server->screen_width / 2) / server->screen_width;
    if (data.active_desktop < 0) data.active_desktop = 0;
    if (data.active_desktop >= 10) data.active_desktop = 9;
    
    if (server->focused_view) {
        PhysicsBody *b = physics_find_body(&server->physics, server->focused_view->id);
        if (b) {
            data.win_name = view_title(server->focused_view);
            if (!data.win_name) data.win_name = "Window";
            data.speed = hypot(b->vx, b->vy);
            data.angle = atan2(b->vy, b->vx) * 180.0 / M_PI;
            data.mass = b->mass;
            data.flying = b->flying;
        }
    }
    
    tray_redraw(server->tray_buffer, &data);
}
