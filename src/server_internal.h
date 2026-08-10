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

#ifndef FWM_SERVER_INTERNAL_H
#define FWM_SERVER_INTERNAL_H

/* Shared between the server_*.c translation units only — never include this
 * from outside them. The public surface of the compositor is server.h.
 *
 * server.c used to be one 3.4k-line file where all of this was `static`. The
 * split turned exactly the calls that cross a module boundary into the
 * declarations below; everything else stayed private to its own file.
 *
 * Each module wires its own listeners through its *_register() function, so
 * the wl_listener callbacks are not declared here — they are static again, in
 * the file that implements them. */

#include "server.h"

struct wlr_pointer_constraint_v1;

/* Pointers are tracked only so a config reload can reach the touchpads that
 * are already plugged in — wlr_cursor keeps its device list private, and the
 * libinput settings are per device. */
struct FwmPointer {
    struct wl_list link;
    FwmServer *server;
    struct wlr_input_device *device;
    struct wl_listener destroy;
};

/* A lid or tablet-mode switch. Kept only so the listeners can be removed when
 * the device goes; the state it reports is acted on immediately and never
 * cached — the outputs are the record of what the lid did. */
struct FwmSwitch {
    struct wl_list link;
    FwmServer *server;
    struct wlr_switch *wlr_switch;
    struct wl_listener toggle;
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

/* ── server.c ─────────────────────────────────────────────────────────── */
void server_shake_tick(FwmServer *server, double dt);
/* Create the physics and video timers (server_tick.c owns both callbacks). */
void server_tick_register(FwmServer *server, struct wl_event_loop *event_loop);
void server_video_sync(FwmServer *server);
/* Bring the visualiser in line with [cava] mode: build it, tear it down, or
 * leave it be. Called from the tick, so `fwmctl set cava.mode` and a config
 * reload both land through the same path. */
void server_cava_sync(FwmServer *server);
/* Bring the collision mixer in line with [sound]: start it, stop it, or just
 * push a new volume at it. Called from the tick, like the cava sync. */
void server_sound_sync(FwmServer *server);
/* Bring what every window weighs in line with [physics] mass. Does nothing at
 * all while mass = "size" (a body's area already decides), and walks /proc on a
 * timer of its own while mass = "ram" — so the tick can call it unconditionally.
 * Called from the tick, so the menu, `fwmctl set` and a reload all land here. */
void server_mass_sync(FwmServer *server);
void server_reclaim_memory(void);
void server_dispatch_action(FwmServer *server, const char *action);
FwmView *server_find_view(FwmServer *server, uint32_t id);
void server_camera_settled(FwmServer *server);

/* The slide across the ring's join: the camera has jumped, and this makes that
 * look like one screen of travel instead of a cut. `dir` is +1 when the view
 * moved right off the last desktop onto the first. Stop is idempotent and safe
 * at any time — closing the strip and losing an output both call it. */
void server_wrap_slide_start(FwmServer *server, FwmOutput *out, int dir);
void server_wrap_slide_stop(FwmServer *server, FwmOutput *out);
/* Park the camera on desktop `d`. `seam` marks a step that crossed the ring's
 * join, which is jumped rather than slid. Also the one place that hands the
 * move to the desktop strip when it is open. */
void server_goto_desktop(FwmServer *server, int d, int seam);
/* How much room a window has on `desktop`, in world coordinates: the monitor's
 * usable area minus our status strip minus [tiling] gaps_out. The tiling layout
 * and fake fullscreen share it, so "as large as a window may be" has one
 * answer. See the definition in server_tiling.c. */
void server_work_area(FwmServer *server, int desktop, int *x, int *y, int *w, int *h);
/* Place a spinning window that is being dragged under its grab point. Called
 * once per frame as well as from the physics tick — see the definition. */
void server_drag_swing_place(FwmServer *server);
/* The angle a body should be DRAWN at right now: its simulated angle advanced
 * to this instant. See the definition — this is what keeps a slow rotation from
 * juddering against the display's clock. */
double server_render_angle(FwmServer *server, const PhysicsBody *b);

/* ── server_gestures.c ────────────────────────────────────────────────── */
void server_gestures_register(FwmServer *server);

/* ── server_actions.c ─────────────────────────────────────────────────── */
/* Whether physics is free to rotate this body (see spin_window). */
bool server_can_spin(const PhysicsBody *b);

/* ── server_config.c ──────────────────────────────────────────────────── */
void server_config_path(char *buf, size_t cap);
void server_close_errors_panel(FwmServer *server);

/* ── modes menu (server_actions.c) ────────────────────────────────────── */
/* Declared rather than included: server_internal.h is pulled in by most of the
 * server and has no business dragging cairo along for four ints. */
struct ModesState;
/* Current mode state, for the tray pill and the menu. */
void server_modes_state(FwmServer *server, struct ModesState *out);
/* Open the menu, or close it if it is already open. */
void server_toggle_modes_menu(FwmServer *server);
void server_close_modes_menu(FwmServer *server);
/* Immediate, unanimated close, for teardown. */
void server_kill_modes_menu(FwmServer *server);
/* Apply a click on menu row `row`. `seg` is the segment under the pointer for
 * the rows that carry a segmented control (cava, mass) and ignored for the
 * switch rows. Returns 1 if anything changed. */
int server_modes_menu_click(FwmServer *server, int row, int seg);

/* ── stats menu (server_actions.c) ────────────────────────────────────── */
/* The same three verbs as the modes menu, for the pill next door, and on the
 * same button (see FwmServer.stats_buffer). */
void server_toggle_stats_menu(FwmServer *server);
void server_close_stats_menu(FwmServer *server);
void server_kill_stats_menu(FwmServer *server);
/* Flip sensor `row` on or off. Returns 1 if anything changed. */
int server_stats_menu_click(FwmServer *server, int row);
void server_state_apply_wallpaper(FwmServer *server);
/* The modes menu's choices, remembered across restarts in
 * ~/.local/state/fwm/modes. Applied over the config after every load; saved the
 * moment one of them is clicked. */
void server_state_apply_modes(FwmServer *server);
void server_state_save_modes(FwmServer *server);

/* ── server_desktop.c ─────────────────────────────────────────────────── */
void server_toggle_desktop_tiling(FwmServer *server, int d);
void server_toggle_desktop_floating(FwmServer *server, int d);
void server_move_view_to_desktop(FwmServer *server, FwmView *view, int target,
                                 int from_drag);

/* ── server_input.c ───────────────────────────────────────────────────── */
void server_input_register(FwmServer *server);
uint32_t get_active_modifiers(FwmServer *server);
void server_notify_activity(FwmServer *server);
void launcher_grab_sync(FwmServer *server, bool was_open);
void keyboard_apply_input_config(FwmServer *server, struct wlr_keyboard *kb);
void pointer_apply_input_config(FwmServer *server, struct wlr_input_device *device);

/* ── server_output.c ──────────────────────────────────────────────────── */
void server_output_register(FwmServer *server);
/* Re-place every monitor from the current [[output]] entries. Called on reload;
 * a monitor arriving applies its own entry as it joins. */
void server_outputs_apply_config(FwmServer *server);

/* ── server_pointer.c ─────────────────────────────────────────────────── */
void server_pointer_register(FwmServer *server);
struct FwmView *view_at(FwmServer *server, double lx, double ly,
                        struct wlr_surface **surface, double *sx, double *sy);
void idle_inhibit_refresh(FwmServer *server);
/* Put the cursor where a lock that is letting go asked for it to be found. */
void pointer_apply_constraint_hint(FwmServer *server,
                                   struct wlr_pointer_constraint_v1 *constraint);

/* ── server_shell.c ───────────────────────────────────────────────────── */
void server_shell_register(FwmServer *server);

/* ── server_seat.c ────────────────────────────────────────────────────────
 * The seat's own business: selections, drag-and-drop, pointer constraints,
 * cursor shape, activation. The cursor half calls into these three. */
void server_seat_register(FwmServer *server);
void drag_icon_update_position(FwmServer *server);
FwmView *view_from_surface(FwmServer *server, struct wlr_surface *surface);
void constraints_follow_focus(FwmServer *server, struct wlr_surface *surface);
void constraints_drop_unless(FwmServer *server, struct wlr_surface *surface);

/* ── server_drag.c ────────────────────────────────────────────────────────
 * What a held mouse button MEANS: moving, resizing, turning, swapping. All of
 * it lives between a press and its release, and ends by handing momentum to
 * the simulation. */
bool server_drag_motion(FwmServer *server, double lx, double ly,
                        const struct timespec *now);
bool server_drag_press(FwmServer *server, uint32_t button, double lx, double ly,
                       const struct timespec *now);
void server_drag_release(FwmServer *server, double lx, double ly);
/* Bring a dragged window along when the camera slides under it — edge
 * auto-scroll, above all. Called from the tick while any camera is moving. */
void server_drag_follow_camera(FwmServer *server);

/* A wlroots button code as [mouse] names it, or -1. */
int button_to_fwm(uint32_t button);

#endif /* FWM_SERVER_INTERNAL_H */
