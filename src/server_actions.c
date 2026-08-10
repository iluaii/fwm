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
#include "shortcuts.h"
#include "ipc.h"
#include "session.h"
#include <signal.h>
#include "ui/tray.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/modes.h"
#include "ui/stats_menu.h"
#include "screenshot.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"
#include "expo.h"

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
#include <sys/wait.h>
#include <errno.h>
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



/* A body physics is free to turn. Every state on this list makes the body an
 * immovable anchor in physics_step, so a "spinning" window in one of them
 * would hold an angle of zero forever — and since the effect swaps the live
 * window for a snapshot, that reads as the window having frozen. Refusing is
 * the honest answer; the same test also ends a spin the moment a window is
 * tiled or made fullscreen underneath it. */
bool server_can_spin(const PhysicsBody *b) {
    return b && !b->pinned && !b->fullscreen && !b->tiled && !b->floating;
}

/* Angular velocity a spin_window press hands the window, in rad/s. Deliberately
 * a drift — a tenth of a turn a second, gone in a few seconds — because the
 * press is not the effect: it is what puts the window in physics' hands. The
 * spinning itself comes from what happens to the window afterwards: what it is
 * thrown into, and stirring the mouse while dragging it. Scaled by
 * effects.spin. */
#define SPIN_KICK 0.6

/* Run a command as a detached child. Through a shell, so a bind can carry
 * quoting, arguments and $VARIABLES — `spawn:$BROWSER --new-window` works for
 * exactly the reason it looks like it should. */
static void server_spawn(const char *cmd) {
    pid_t pid = fork();

    if (pid == 0) {
        /* Child: double-fork to orphan the grandchild process.
         * Only async-signal-safe functions (setsid, execl, _exit) may be called here. */
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(1);
        }
        _exit(0);
    } else if (pid > 0) {
        /* Bounded wait: the middle child calls _exit(0) immediately,
         * so this waitpid never blocks the event loop. */
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    } else {
        wlr_log(WLR_ERROR, "fwm: failed to fork process for command: %s", cmd);
    }
}

/* Is `name` an executable somewhere on PATH? */
static bool on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char *sep = strchr(path, ':');
        size_t len = sep ? (size_t)(sep - path) : strlen(path);
        if (len == 0) { len = 1; path = "."; }   /* empty entry means cwd */
        char full[512];
        if (snprintf(full, sizeof(full), "%.*s/%s", (int)len, path, name) < (int)sizeof(full)
            && access(full, X_OK) == 0) return true;
        if (!sep) break;
        path = sep + 1;
    }
    return false;
}

/* What `terminal` should run.
 *
 * $TERMINAL first — it is the variable the user sets when they have an opinion,
 * and it may carry arguments, which is fine because we go through a shell.
 * Deliberately NOT $TERM: that is the terminfo entry name, so honouring it
 * would try to run "xterm-256color" from inside kitty.
 *
 * Otherwise the first emulator actually installed, cheapest to start first —
 * on old integrated graphics the difference between foot and kitty is two
 * seconds of staring at nothing. A list is still a choice made for the user,
 * but unlike a hard-coded name it is one they can override with either half of
 * the mechanism: the variable, or `spawn:` with whatever they please. */
static const char *terminal_command(FwmServer *server) {
    const char *env = getenv("TERMINAL");
    if (env && *env) return env;

    static const char *known[] = {
        "foot", "alacritty", "kitty", "wezterm", "ghostty", "st", "urxvt",
        "konsole", "gnome-terminal", "xfce4-terminal", "lxterminal", "xterm",
        NULL,
    };
    for (int i = 0; known[i]; i++)
        if (on_path(known[i])) return known[i];

    /* Nothing to run. Said once, through the tray pill that reports config
     * problems, because a bind that silently does nothing is the worst way to
     * find out that no terminal is installed. */
    static bool warned = false;
    if (!warned) {
        warned = true;
        config_report_error(&server->config,
                            "`terminal`: no terminal emulator found — set $TERMINAL "
                            "or bind spawn:<your terminal>");
        server_request_tray_redraw(server);
    }
    return NULL;
}

/* The desktop an action's argument names: a number ("view:3"), or "next" /
 * "prev" relative to where the camera is HEADED — which is what the user is
 * aiming at when a gesture or a held key fires this twice in a row. Returns -1
 * when it names nothing that exists, including the ends of the strip, so the
 * caller does nothing rather than wrapping around. */
/* Resolve a desktop argument. `seam` (may be NULL) reports that the answer was
 * reached by stepping off one end of the strip onto the other, which the caller
 * must jump rather than slide — see server_goto_desktop.
 *
 * Only next/prev wrap. A bare number is a destination, not a step: "view:0"
 * from desktop 9 means that particular desktop, and travelling there past the
 * eight in between is what the user asked for. */
static int resolve_desktop_ex(FwmServer *server, const char *arg, int *seam) {
    /* Where "here" is. While the desktop strip is up that is where the STRIP is
     * looking, not where camera_x is parked — the camera does not move until
     * the strip closes, so stepping from the parked desktop meant next/prev
     * resolved to the same neighbour however far you had already travelled. */
    int here = expo_target_desktop(server);
    if (here < 0) here = server_active_desktop(server);
    int d, step = 0;
    if (strcmp(arg, "next") == 0) {
        d = here + 1; step = 1;
    } else if (strcmp(arg, "prev") == 0) {
        d = here - 1; step = 1;
    } else {
        char *end;
        long v = strtol(arg, &end, 10);
        if (end == arg) return -1;
        d = (int)v;
    }

    if (step && server->config.camera.wrap && (d < 0 || d >= FWM_DESKTOPS)) {
        d = (d + FWM_DESKTOPS) % FWM_DESKTOPS;
        if (seam) *seam = 1;
    }
    return (d >= 0 && d < FWM_DESKTOPS) ? d : -1;
}

static int resolve_desktop(FwmServer *server, const char *arg) {
    return resolve_desktop_ex(server, arg, NULL);
}

/* Park the camera on a desktop. A `seam` move — the ring's join — is jumped
 * outright: sliding it would drag the view backwards across every desktop in
 * between, and there is no picture of the join to slide through, because the
 * ten desktops are one straight strip in world coordinates and always were. */
void server_goto_desktop(FwmServer *server, int d, int seam) {
    if (d < 0 || d >= FWM_DESKTOPS || server->screen_width <= 0) return;
    if (expo_goto_desktop(server, d)) return;

    /* The monitor under the pointer is the one that switches: on two screens a
     * desktop bind has to mean one of them, and "the one you are working on"
     * is the only answer that needs no extra keys. */
    FwmOutput *out = server_active_output(server);
    if (!out || out->desktop == d) return;

    int was = out->camera_x;
    out->cam_free = 0;
    server_output_show_desktop(server, out, d, seam);
    if (!seam) return;

    /* Photographed before the camera moves, and started after: the slide shows
     * the desktop being left travelling off one side while the one arriving
     * comes in the other. Direction is the way the STEP went, not the way
     * camera_x jumped — those are opposite at the join, which is the whole
     * reason the jump needs covering. */
    server_wrap_slide_start(server, out, out->target_camera_x < was ? 1 : -1);
    server_camera_settled(server);
    wallpaper_update(out->wallpaper, out->camera_x);
    server_request_tray_redraw(server);
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

void server_dispatch_action(FwmServer *server, const char *action) {
    /* The desktop strip owns the session while it is up: anything else firing
     * here would act on a world nobody can see. Two exceptions, both of which
     * mean "look somewhere else" and are things the strip can do itself —
     * closing it, and jumping to a desktop, which is how super+1..0 stays the
     * fastest way across ten of them instead of a lot of scrolling. */
    if (expo_active(server)) {
        if (strncmp(action, "view:", 5) == 0) {
            int d = resolve_desktop(server, action + 5);
            if (d >= 0) expo_goto_desktop(server, d);
            return;
        }
        /* `screenshot` too: it photographs the frame the strip is drawing,
         * which is the one thing here that is ABOUT what is on screen. The
         * region selector is not on the list — its pointer grab and the
         * strip's would be aiming at the same events. */
        if (strcmp(action, "expo") != 0 && strcmp(action, "toggle_wrap") != 0 &&
            strcmp(action, "screenshot") != 0) return;
    }

    if (strcmp(action, "killclient") == 0) {
        if (server->focused_view) {
            view_send_close(server->focused_view);
        }
    } else if (strcmp(action, "toggle_tiling") == 0) {
        server_toggle_desktop_tiling(server, server_active_desktop(server));
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
            server_panel_to_active_output(server, server->hints_buffer);
        }
    } else if (strcmp(action, "screenshot") == 0) {
        screenshot_full(server);
    } else if (strcmp(action, "screenshot_region") == 0) {
        screenshot_region(server);
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
            server_panel_to_active_output(server, server->errors_buffer);
        }
        server_request_tray_redraw(server);
    } else if (strcmp(action, "modes_menu") == 0) {
        server_toggle_modes_menu(server);
    } else if (strcmp(action, "stats_menu") == 0) {
        server_toggle_stats_menu(server);
    } else if (strcmp(action, "output_off") == 0) {
        /* The monitor the pointer is on goes dark. Its desktop is handed back
         * to the pool and the pointer is moved to a screen that still exists,
         * so the session goes on working on what is left — the answer to
         * "I want this screen off" that does not need the config file. */
        server_output_set_enabled(server, server_active_output(server), 0);
    } else if (strcmp(action, "toggle_internal_output") == 0) {
        /* The built-in panel specifically, whichever screen the pointer is on:
         * the one people actually want a key for, and the only one they cannot
         * aim at once it is dark. */
        FwmOutput *panel = server_internal_output(server);
        if (panel) server_output_set_enabled(server, panel, !panel->enabled);
    } else if (strcmp(action, "outputs_on") == 0) {
        /* Everything back. The way out of "I turned off the screen I was
         * looking at": it needs no pointer and no visible monitor, only a key
         * that still works. */
        FwmOutput *o;
        wl_list_for_each(o, &server->outputs, link)
            server_output_set_enabled(server, o, 1);
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
    } else if (strncmp(action, FWM_MODE_ACTION, strlen(FWM_MODE_ACTION)) == 0) {
        /* Step into a submap, or back out of one. Unknown names return to the
         * root map rather than leaving the keyboard in a mode that does not
         * exist — the safe direction when a config reload has just removed the
         * mode the user was standing in. */
        const char *name = action + strlen(FWM_MODE_ACTION);
        if (strcmp(name, FWM_MODE_DEFAULT) == 0) server->key_mode = -1;
        else                                     server->key_mode = config_mode_find(&server->config, name);
        server_request_tray_redraw(server);
    } else if (strcmp(action, "cycle_gravity") == 0) {
        /* Walk the ladder [physics] gravity_steps sets (zero-g, a lick of it,
         * earth by default). The current value is matched by proximity rather
         * than equality: it may have come from `fwmctl set` or a config reload
         * and land between two steps, and the sensible answer to "cycle from
         * here" is then the step after the nearest one. */
        const PhysicsConfig *pc = &server->config.physics;
        int n = pc->gravity_step_count;
        if (n > 0) {
            int nearest = 0;
            double best = fabs(server->physics.gravity_scale - pc->gravity_steps[0]);
            for (int i = 1; i < n; i++) {
                double d = fabs(server->physics.gravity_scale - pc->gravity_steps[i]);
                if (d < best) { best = d; nearest = i; }
            }
            server->physics.gravity_scale = pc->gravity_steps[(nearest + 1) % n];
            ipc_emit_gravity(server->ipc, server->physics.gravity_scale);
        }
    } else if (strcmp(action, "pin_window") == 0) {
        if (server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            if (pb) {
                pb->pinned ^= 1;
                pb->vx = 0; pb->vy = 0; pb->flying = 0;
            }
        }
    } else if (strcmp(action, "spin_window") == 0) {
        /* Experimental. Frees the focused window's rotation and kicks it, or
         * settles it back upright if it is already spinning. The kick's
         * direction alternates so pressing the bind twice in a row on two
         * windows does not produce a pair of synchronised clones. */
        double strength = server->config.effects.spin;
        if (strength > 0.0 && server->focused_view) {
            PhysicsBody *pb = physics_find_body(&server->physics, server->focused_view->id);
            if (pb && pb->spin) {
                physics_unspin_body(&server->physics, server->focused_view->id);
                view_stop_spin(server->focused_view);
            } else if (pb && server_can_spin(pb)) {
                static int flip = 0;
                flip = !flip;
                physics_spin_body(&server->physics, server->focused_view->id,
                                  (flip ? 1.0 : -1.0) * SPIN_KICK * strength);
            }
        }
    } else if (strcmp(action, "spin_all") == 0) {
        /* Same rule as toggle_nocollide_all: uniform, not per-window XOR — the
         * only predictable meaning of "all" is that after a press everything is
         * in the same state. Spinning wins unless everything already spins.
         * Alternating the direction per window keeps the desktop from looking
         * like a set of gears turning in lockstep. */
        double strength = server->config.effects.spin;
        if (strength > 0.0) {
            int all_spinning = 1;
            for (int i = 0; i < server->physics.body_count; i++) {
                const PhysicsBody *b = &server->physics.bodies[i];
                if (b->active && server_can_spin(b) && !b->spin) { all_spinning = 0; break; }
            }
            int sign = 1;
            for (int i = 0; i < server->physics.body_count; i++) {
                PhysicsBody *b = &server->physics.bodies[i];
                if (!b->active) continue;
                if (all_spinning || !server_can_spin(b)) {
                    if (b->spin) {
                        physics_unspin_body(&server->physics, b->id);
                        FwmView *sv = server_find_view(server, b->id);
                        if (sv) view_stop_spin(sv);
                    }
                } else if (!b->spin) {
                    physics_spin_body(&server->physics, b->id, sign * SPIN_KICK * strength);
                    sign = -sign;
                }
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
    } else if (strcmp(action, "toggle_tray") == 0) {
        /* Hide the tray outright: the node goes away AND the strip it reserved
         * comes back, so tiles and fake-fullscreen windows grow into the top of
         * the screen instead of leaving a bar-shaped hole. Physics windows need
         * no help — nothing ever kept them out of that strip. */
        server->tray_hidden = !server->tray_hidden;
        FwmOutput *to;
        wl_list_for_each(to, &server->outputs, link) {
            if (to->tray_buffer)
                wlr_scene_node_set_enabled(&to->tray_buffer->node, !server->tray_hidden);
        }

        for (int d = 0; d < 10; d++) {
            if (server->desktop_mode[d] == DESKTOP_MODE_TILING)
                server_apply_tiling(server, d);
        }
        /* Re-run fake fullscreen for the geometry, not the state: the call is a
         * no-op on a body that is already fullscreen except for recomputing the
         * work area, which is exactly what changed. Real fullscreen never used
         * the strip, so it is left alone. */
        FwmView *fv;
        wl_list_for_each(fv, &server->views, link) {
            PhysicsBody *fb = physics_find_body(&server->physics, fv->id);
            if (fb && fb->fullscreen && !fv->fs_real)
                server_set_fullscreen(server, fv, true, false);
        }
        server_request_tray_redraw(server);
    } else if (strcmp(action, "toggle_floating") == 0) {
        server_toggle_desktop_floating(server, server_active_desktop(server));
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
    } else if (strcmp(action, "terminal") == 0) {
        const char *cmd = terminal_command(server);
        if (cmd) server_spawn(cmd);
    } else if (strncmp(action, "spawn:", 6) == 0) {
        const char *cmd = action + 6;
        server_spawn(cmd);
    } else if (strncmp(action, "global:", 7) == 0) {
        /* Hand the key to an external shell — "global:<app_id>:<name>", the
         * shortcut it registered over hyprland-global-shortcuts (shortcuts.h).
         * This is how a Quickshell launcher takes super+space off the built-in
         * one: bind the key to the client's action instead of to `launcher`. */
        const char *spec = action + 7;
        const char *colon = strchr(spec, ':');
        if (!colon || colon == spec || !colon[1]) {
            wlr_log(WLR_ERROR, "global: expects app_id:name, got \"%s\"", spec);
            return;
        }
        char app_id[128];
        size_t n = (size_t)(colon - spec);
        if (n >= sizeof(app_id)) {
            wlr_log(WLR_ERROR, "global: app_id too long in \"%s\"", spec);
            return;
        }
        memcpy(app_id, spec, n);
        app_id[n] = '\0';
        /* Nothing registered it: say so rather than swallow the key. A shell
         * that is not running yet is the usual reason, and a bind that does
         * nothing at all looks like a broken keyboard. */
        if (!shortcuts_trigger(server, app_id, colon + 1)) {
            wlr_log(WLR_ERROR, "no client has registered the shortcut %s", spec);
        }
    } else if (strncmp(action, "move_camera:", 12) == 0) {
        int amt = atoi(action + 12);
        FwmOutput *out = server_active_output(server);
        if (!out) return;
        int last = (FWM_DESKTOPS - 1) * server->screen_width;
        int new_target = out->target_camera_x + amt;
        int seam = 0;

        /* On a ring the free pan runs off one end onto the other, keeping
         * whatever it had left to travel. This was left clamped at first, on
         * the theory that a continuous pan crossing the join would feel like a
         * glitch — but held panning is how a lot of people move around, and a
         * ring that stops dead at desktop ten is not a ring to them. The jump
         * is the same one every wrapping step makes: the join is never drawn,
         * the camera is simply on the other side of it. */
        if (server->config.camera.wrap) {
            if (new_target > last)    { new_target -= last; seam = 1; }
            else if (new_target < 0)  { new_target += last; seam = 1; }
        } else {
            if (new_target < 0) new_target = 0;
            if (new_target > last) new_target = last;
        }
        out->target_camera_x = new_target;
        out->cam_free = 1; // continuous pan, not a desktop jump
        /* A free pan is not a desktop switch, but it does change which desktop
         * this monitor is on top of — and the swap rule has to keep holding. */
        int panned_d = server_desktop_at_x(server, new_target + server->screen_width / 2.0);
        FwmOutput *clash = server_output_showing(server, panned_d);
        if (clash && clash != out) {
            clash->desktop = out->desktop;
            clash->camera_x = clash->target_camera_x = clash->desktop * server->screen_width;
            clash->cam_anim = 0;
            wallpaper_update(clash->wallpaper, clash->camera_x);
        }
        out->desktop = panned_d;
        if (seam) {
            server_wrap_slide_start(server, out, amt > 0 ? 1 : -1);
            out->camera_x = new_target;
            out->cam_anim = 0;
            server_camera_settled(server);
            wallpaper_update(out->wallpaper, out->camera_x);
            server_request_tray_redraw(server);
        }
        server_views_place(server);
    } else if (strcmp(action, "expo") == 0) {
        expo_toggle(server);
    } else if (strcmp(action, "launcher") == 0) {
        bool was_open = launcher_is_open(server->launcher);
        launcher_toggle(server->launcher);
        launcher_grab_sync(server, was_open);
    } else if (strncmp(action, "view:", 5) == 0) {
        int seam = 0;
        int desktop = resolve_desktop_ex(server, action + 5, &seam);
        if (desktop >= 0) server_goto_desktop(server, desktop, seam);
    } else if (strcmp(action, "toggle_wrap") == 0) {
        server->config.camera.wrap = !server->config.camera.wrap;
        wlr_log(WLR_INFO, "desktop strip is %s",
                server->config.camera.wrap ? "a ring" : "a line");
        server_request_tray_redraw(server);
    } else if (strncmp(action, "move_to:", 8) == 0) {
        int desktop = resolve_desktop(server, action + 8);
        if (desktop >= 0)
            server_move_view_to_desktop(server, server->focused_view, desktop, 0);
    } else if (strncmp(action, "move_to_view:", 13) == 0) {
        int desktop = resolve_desktop(server, action + 13);
        if (desktop >= 0)
            server_move_view_to_desktop(server, server->focused_view, desktop, 1);
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

/* ── modes menu ───────────────────────────────────────────────────────── */

void server_modes_state(FwmServer *server, ModesState *out) {
    int d = server_active_desktop(server);
    if (d < 0) d = 0;
    if (d > 9) d = 9;
    out->tiling   = server->desktop_mode[d] == DESKTOP_MODE_TILING;
    out->floating = server->desktop_mode[d] == DESKTOP_MODE_FLOATING;
    out->gravity  = server->physics.gravity_scale > 0.0;
    out->mass     = server->config.physics.mass_mode;
    out->sound    = server->config.sound.collisions;
    out->cava     = server->config.cava.mode;
    out->ring     = server->config.camera.wrap;
    out->hp       = server->config.physics.hp;
    out->opacity  = server->config.decor.tray_opacity;
}

void server_close_modes_menu(FwmServer *server) {
    if (!server->modes_buffer) return;
    /* The exact reverse of the open: same duration, same distance, the other
     * way. Ownership passes to the animation — hence clearing the pointer
     * immediately, which is also what stops the tick redrawing a buffer that is
     * on its way out. */
    cairo_overlay_animate_out(server->modes_buffer, MODES_MENU_ANIM_MS,
                              -MODES_MENU_RISE_PX, NULL, NULL);
    server->modes_buffer = NULL;
}

/* Teardown path: the scene is going away, so there is nothing to animate into
 * and no frames left to animate with. */
void server_kill_modes_menu(FwmServer *server) {
    if (server->modes_buffer) {
        cairo_overlay_destroy(server->modes_buffer);
        server->modes_buffer = NULL;
    }
}

void server_toggle_modes_menu(FwmServer *server) {
    if (server->modes_buffer) {
        server_close_modes_menu(server);
    } else {
        /* The menu hangs off the pill in the ACTIVE monitor's tray — that is
         * the strip the pointer is at, and the one the modes it shows belong
         * to. */
        FwmOutput *out = server_active_output(server);
        /* Ask the strip's node, not tray_hidden: the node is where BOTH reasons
         * a strip is off screen have already met — the global toggle and a real
         * fullscreen window, which is decided per monitor. The flag alone let
         * the keybind hang a menu off a pill that fullscreen had hidden, until
         * the next tick took it away again. */
        if (!out || !out->tray_buffer || !out->tray_buffer->node.enabled) return;
        /* The pill is dropped on a screen too narrow to hold it, and then there
         * is nothing to hang the menu off. The keybind lands here too, so this
         * is also what stops it opening a menu pointing at nothing. */
        if (!tray_modes_pill_hit(&out->tray_strip, tray_modes_pill_x(&out->tray_strip), 1.0))
            return;
        ModesState st;
        server_modes_state(server, &st);
        /* Both in layout coordinates: the strip's node is already on its own
         * monitor, and the menu is clamped to that monitor's box. */
        server->modes_buffer = modes_menu_show(
            server->layer_overlay, &out->box,
            out->tray_buffer->node.x + tray_modes_pill_x(&out->tray_strip), &st);
    }
    server_request_tray_redraw(server);
}

int server_modes_menu_click(FwmServer *server, int row, int seg) {
    int changed = 0;
    /* The desktop the user is looking at — which, while the strip is up, is
     * where the strip has panned to and not where camera_x is parked. */
    int d = expo_view_desktop(server);
    if (d < 0) d = server_active_desktop(server);
    if (d < 0) d = 0;
    if (d > 9) d = 9;

    switch (row) {
    case MODES_ROW_TILING:
        server_set_desktop_mode(server, d,
            server->desktop_mode[d] == DESKTOP_MODE_TILING
                ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_TILING);
        changed = 1;
        break;
    case MODES_ROW_FLOATING:
        server_set_desktop_mode(server, d,
            server->desktop_mode[d] == DESKTOP_MODE_FLOATING
                ? DESKTOP_MODE_PHYSICS : DESKTOP_MODE_FLOATING);
        changed = 1;
        break;
    case MODES_ROW_GRAVITY: {
        /* A switch is on or off, so this does not walk gravity_steps the way
         * cycle_gravity does — it goes to zero, or back to the heaviest step the
         * config offers, which is what someone flicking "gravity" expects. */
        if (server->physics.gravity_scale > 0.0) {
            server->physics.gravity_scale = 0.0;
        } else {
            const PhysicsConfig *pc = &server->config.physics;
            double g = 1.0;
            for (int i = 0; i < pc->gravity_step_count; i++)
                if (pc->gravity_steps[i] > g) g = pc->gravity_steps[i];
            server->physics.gravity_scale = g;
        }
        ipc_emit_gravity(server->ipc, server->physics.gravity_scale);
        changed = 1;
        break;
    }
    case MODES_ROW_MASS: {
        if (seg < 0) break;
        int want = seg == MODES_MASS_RAM ? PHYSICS_MASS_RAM : PHYSICS_MASS_SIZE;
        if (want != server->config.physics.mass_mode) {
            server->config.physics.mass_mode = want;
            /* server_mass_sync picks it up on the next tick: it compares the
             * mode against the one it last acted on, so switching either way
             * takes effect at once without this having to know how. */
            server_state_save_modes(server);
            changed = 1;
        }
        break;
    }
    case MODES_ROW_SOUND:
        /* server_sound_sync starts or stops the mixer on the next tick; nothing
         * here has to know that a thread and an audio device are involved. */
        server->config.sound.collisions = !server->config.sound.collisions;
        server_state_save_modes(server);
        changed = 1;
        break;
    case MODES_ROW_RING:
        server_dispatch_action(server, "toggle_wrap");
        changed = 1;
        break;
    case MODES_ROW_HP:
        /* Nothing to sync: server_consume_impacts reads the flag straight off
         * the config every frame, so switching it off mid-flight simply stops
         * the next collision from being worked out. Windows already asked to
         * close are not called back — that request is the client's now. */
        server->config.physics.hp = !server->config.physics.hp;
        server_state_save_modes(server);
        changed = 1;
        break;
    case MODES_ROW_CAVA: {
        if (seg < 0) break;
        /* The menu's "physical" is the config's "both": bars you can see AND
         * that push. Invisible pushing is a real mode but a strange thing to
         * land on by clicking, so it stays a file-only setting. */
        int want = seg == MODES_CAVA_OFF     ? CAVA_MODE_OFF
                 : seg == MODES_CAVA_VISUAL  ? CAVA_MODE_VISUAL
                                             : CAVA_MODE_BOTH;
        if (want != server->config.cava.mode) {
            server->config.cava.mode = want;
            changed = 1; /* server_cava_sync picks it up on the next tick */
        }
        break;
    }
    default:
        break;
    }

    /* The strip is showing photographs of that desktop, and the layout just
     * moved under them. */
    if (changed) expo_refresh_desktop(server, d);
    if (changed) {
        ModesState st;
        server_modes_state(server, &st);
        modes_menu_redraw(server->modes_buffer, &st);
        server_request_tray_redraw(server);
    }
    return changed;
}

/* ── stats menu ──────────────────────────────────────────────────────────
 * The same three verbs as the modes menu above, and deliberately the same
 * shape: two menus hanging off two neighbouring pills that were opened and
 * closed by different code would drift apart in exactly the ways a user
 * notices. */

void server_close_stats_menu(FwmServer *server) {
    if (!server->stats_buffer) return;
    cairo_overlay_animate_out(server->stats_buffer, STATS_MENU_ANIM_MS,
                              -STATS_MENU_RISE_PX, NULL, NULL);
    server->stats_buffer = NULL;
}

/* Teardown path: the scene is going away, so there is nothing to animate into
 * and no frames left to animate with. */
void server_kill_stats_menu(FwmServer *server) {
    if (server->stats_buffer) {
        cairo_overlay_destroy(server->stats_buffer);
        server->stats_buffer = NULL;
    }
}

void server_toggle_stats_menu(FwmServer *server) {
    if (server->stats_buffer) {
        server_close_stats_menu(server);
    } else {
        FwmOutput *out = server_active_output(server);
        /* Ask the strip's node rather than tray_hidden: the node is where both
         * reasons a strip is off screen have already met — the global toggle,
         * and a fullscreen window, which is decided per monitor. */
        if (!out || !out->tray_buffer || !out->tray_buffer->node.enabled) return;
        /* Dropped on a screen too narrow to hold it, and then there is nothing
         * to hang a menu off. */
        if (!tray_stats_pill_hit(&out->tray_strip, tray_stats_pill_x(&out->tray_strip), 1.0))
            return;
        server->stats_buffer = stats_menu_show(
            server->layer_overlay, &out->box,
            out->tray_buffer->node.x + tray_stats_pill_x(&out->tray_strip),
            out->tray_strip.stats.w, server->stats,
            server->config.decor.tray_opacity);
    }
    server_request_tray_redraw(server);
}

int server_stats_menu_click(FwmServer *server, int row) {
    const StatsItem *it = stats_item(server->stats, row);
    if (!it) return 0;
    /* A sensor the machine cannot answer is not a switch: turning it on would
     * put a name with no value in the pill. The row says "unavailable" and the
     * click does nothing, which is the honest pair. */
    if (!it->available) return 0;

    stats_set_enabled(server->stats, row, !it->enabled);
    stats_menu_redraw(server->stats_buffer, server->stats,
                      server->config.decor.tray_opacity);
    server_request_tray_redraw(server);
    return 1;
}
