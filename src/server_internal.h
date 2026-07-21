#ifndef FWM_SERVER_INTERNAL_H
#define FWM_SERVER_INTERNAL_H

/* Shared between the server_*.c translation units only — never include this
 * from outside them. The public surface of the compositor is server.h.
 *
 * server.c used to be one 3.4k-line file where all of this was `static`. The
 * split turned exactly the calls that cross a module boundary into the
 * declarations below; everything else stayed private to its own file.
 *
 * The handle_* listener callbacks are here for one reason: server_init() wires
 * every listener itself, so it needs their addresses. Giving each module its
 * own registration function would let them go back to being static — worth
 * doing, but it changes server_init() rather than merely moving code, so it is
 * deliberately left as a follow-up. */

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
/* Passed as a wl_event_loop timer callback by handle_new_output(). */
int test_action_cb(void *data);
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
uint32_t get_active_modifiers(FwmServer *server);
void server_notify_activity(FwmServer *server);
void launcher_grab_sync(FwmServer *server, bool was_open);
void keyboard_apply_input_config(FwmServer *server, struct wlr_keyboard *kb);
/* Passed as a wl_event_loop timer callback by server_init(). */
int key_repeat_cb(void *data);
void handle_new_input(struct wl_listener *listener, void *data);

/* ── server_output.c ──────────────────────────────────────────────────── */
void handle_new_output(struct wl_listener *listener, void *data);
void handle_output_power_set_mode(struct wl_listener *listener, void *data);

/* ── server_pointer.c ─────────────────────────────────────────────────── */
struct FwmView *view_at(FwmServer *server, double lx, double ly,
                        struct wlr_surface **surface, double *sx, double *sy);
void idle_inhibit_refresh(FwmServer *server);
void handle_cursor_motion(struct wl_listener *listener, void *data);
void handle_cursor_motion_absolute(struct wl_listener *listener, void *data);
void handle_cursor_button(struct wl_listener *listener, void *data);
void handle_cursor_axis(struct wl_listener *listener, void *data);
void handle_cursor_frame(struct wl_listener *listener, void *data);
void handle_request_cursor(struct wl_listener *listener, void *data);
void handle_seat_request_set_selection(struct wl_listener *listener, void *data);
void handle_seat_request_set_primary_selection(struct wl_listener *listener, void *data);
void handle_seat_request_start_drag(struct wl_listener *listener, void *data);
void handle_seat_start_drag(struct wl_listener *listener, void *data);
void handle_new_pointer_constraint(struct wl_listener *listener, void *data);
void handle_cursor_shape_request(struct wl_listener *listener, void *data);
void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data);

/* ── server_shell.c ───────────────────────────────────────────────────── */
void handle_new_xdg_toplevel(struct wl_listener *listener, void *data);
void handle_new_xdg_popup(struct wl_listener *listener, void *data);
void handle_new_toplevel_decoration(struct wl_listener *listener, void *data);
void handle_xwl_ready(struct wl_listener *listener, void *data);
void handle_xwl_new_surface(struct wl_listener *listener, void *data);

#endif /* FWM_SERVER_INTERNAL_H */
