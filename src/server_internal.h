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

/* ── server.c ─────────────────────────────────────────────────────────── */
void server_shake_tick(FwmServer *server, double dt);
void server_video_sync(FwmServer *server);
void server_reclaim_memory(void);
void server_dispatch_action(FwmServer *server, const char *action);
FwmView *server_find_view(FwmServer *server, uint32_t id);

/* ── server_config.c ──────────────────────────────────────────────────── */
void server_config_path(char *buf, size_t cap);
void server_close_errors_panel(FwmServer *server);
void server_state_apply_wallpaper(FwmServer *server);

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

/* ── server_output.c ──────────────────────────────────────────────────── */
void server_output_register(FwmServer *server);

/* ── server_pointer.c ─────────────────────────────────────────────────── */
void server_pointer_register(FwmServer *server);
struct FwmView *view_at(FwmServer *server, double lx, double ly,
                        struct wlr_surface **surface, double *sx, double *sy);
void idle_inhibit_refresh(FwmServer *server);

/* ── server_shell.c ───────────────────────────────────────────────────── */
void server_shell_register(FwmServer *server);

#endif /* FWM_SERVER_INTERNAL_H */
