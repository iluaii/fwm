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

/* Keyboard input: modifier state, the bind table, key repeat, and input-device
 * hotplug. Split out of server.c; see server_internal.h. */
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

static int key_repeat_cb(void *data);

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

uint32_t get_active_modifiers(FwmServer *server) {
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (!kbd || !kbd->xkb_state) return 0;
    
    uint32_t mods = 0;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= FWM_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)  mods |= FWM_MOD_CTRL;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)   mods |= FWM_MOD_ALT;
    if (xkb_state_mod_name_is_active(kbd->xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)  mods |= FWM_MOD_LOGO;
    return mods;
}

/* Any real user input resets the idle timers of ext-idle-notify clients
 * (swayidle and friends). Must be called from every input path, or the session
 * dims while the user is actively working. */
void server_notify_activity(FwmServer *server) {
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
/* Call around any code that may toggle the launcher. Opening it takes the
 * keyboard away from the focused client via a wl_keyboard leave — per the
 * Wayland spec the client must then treat every key as released, which is
 * what prevents "stuck" keys (e.g. the space of a super+space toggle bind,
 * whose press was already forwarded but whose release we swallow). Closing
 * re-enters the focused surface. */
void launcher_grab_sync(FwmServer *server, bool was_open) {
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
        /* Matching lives in config.c so it can be tested without a
         * compositor; the case-insensitive comparison is explained there. */
        const KeyBind *bind = config_match_bind(&server->config, syms[i], active_mods);
        if (!bind) continue;

        // Consumed by the compositor: the client never sees this key
        // (forwarding it too made super+g type a 'g' into the client).
        if (event->keycode < sizeof(server->key_consumed)) {
            server->key_consumed[event->keycode] = 1;
        }
        server_dispatch_action(server, bind->action);
        // Arm auto-repeat for repeatable binds (e.g. move_camera) so
        // holding the key keeps scrolling; delay before first repeat.
        if (config_action_is_repeatable(bind->action) && server->key_repeat_timer) {
            server->repeat_action = bind->action;
            server->repeat_keycode = event->keycode;
            wl_event_source_timer_update(server->key_repeat_timer, 300);
        } else {
            key_repeat_stop(server);
        }
        return 1;
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
void keyboard_apply_input_config(FwmServer *server, struct wlr_keyboard *kb) {
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


/* Called once from server_init(). Key repeat is driven entirely from this
 * file, so the timer is created here too; it stays disarmed until a bind that
 * repeats is actually held. */
void server_input_register(FwmServer *server) {
    server->new_input.notify = handle_new_input;
    wl_signal_add(&server->wlr_backend->events.new_input, &server->new_input);

    server->key_repeat_timer = wl_event_loop_add_timer(
        wl_display_get_event_loop(server->wl_display), key_repeat_cb, server);
}
