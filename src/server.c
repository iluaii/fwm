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

/* What is left of the compositor object once the rest of it moved into its own
 * files: focus, and the window-level operations that belong to no single
 * subsystem — the interactive grabs a client can ask for, fullscreen, and the
 * tray's redraw request. The heartbeat is server_tick.c, startup and teardown
 * server_lifecycle.c, the layout server_tiling.c; see server_internal.h for the
 * whole map. */
#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "theme.h"
#include "lock.h"
#include "foreign.h"
#include "ipc.h"
#include "expo.h"
#include <signal.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "ui/tray.h"
#include "ui/modes.h"
#include "ui/stats_menu.h"
#include "stats.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "server_internal.h"

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
#include <wlr/types/wlr_pointer_gestures_v1.h>
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
     * at a window the user can no longer see. Through server_focus_view, not by
     * clearing the seat here — the window being left has to be told it lost the
     * focus (deactivated, unhighlighted, and for an X client an actual
     * FocusOut), and only that path does it. Setting focused_view straight to
     * NULL left the old window believing it was still in front: a game kept its
     * grab of the pointer instead of pausing to its menu, and the cursor stayed
     * frozen for the whole session. */
    server_focus_view(server, NULL);
}



FwmView *server_find_view(FwmServer *server, uint32_t id) {
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->id == id) return v;
    }
    return NULL;
}



void server_focus_view(FwmServer *server, struct FwmView *view) {
    if (server->focused_view == view) return;
    
    struct FwmView *prev_focus = server->focused_view;
    server->focused_view = view;

    /* Before anything else: whatever was holding the pointer is not what the
     * user is on any more, and a held lock would freeze the cursor everywhere. */
    constraints_drop_unless(server, view ? view_surface(view) : NULL);

    if (view) {
        if (view->scene_tree) wlr_scene_node_raise_to_top(&view->scene_tree->node);
        
        server_keyboard_enter(server, view_surface(view));
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
    ipc_emit_window(server->ipc, FWM_EV_WINDOW_FOCUS, view);

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
    server->interactive.view_start_x = view->x;   /* world, not screen */
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
    /* Seeded on the first tick, from whichever monitor the hand is over. Left
     * unseeded here so a drag that starts with the cursor off every monitor is
     * simply not following a camera yet, rather than following a stale one. */
    server->interactive.cam_have = 0;
    server->interactive.cam_output = NULL;

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
    server->interactive.view_start_x = view->x;   /* world, not screen */
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
        /* A tile glide still in the air would carry the window off the screen
         * it is being handed: the animation moves it every tick toward a slot
         * that stopped applying the moment it went fullscreen. Windows that
         * open fullscreen on a tiling desktop are laid out and then immediately
         * fullscreened, so this is not a corner case, it is the normal path. */
        view->tile_anim = 0;

        /* Real fullscreen covers the whole output; fake fullscreen fills the
         * work area — below our tray and clear of any layer-shell bar that
         * reserved space — so the tray stays visible.
         *
         * "The output" is the monitor the window is standing on, not the
         * world: on two monitors a fullscreen video must not straddle the
         * bezel. A single-output setup gets the box it always did. */
        /* A desktop is one screen, so fullscreen is that screen. */
        if (real) {
            view->x = d * server->screen_width;
            view->y = 0;
            view->width = server->screen_width;
            view->height = server->screen_height;
        } else {
            /* Fake fullscreen is "as large as a window is allowed to be", which
             * is the same question the tiling layout answers — so it is the same
             * function, gaps and all. A window filling the screen this way sits
             * where a single tile would, rather than butting against the edges
             * while every tiled window keeps its margin. */
            server_work_area(server, d, &view->x, &view->y, &view->width, &view->height);
        }


        // Keep the physics body in sync with the fullscreen geometry, otherwise
        // physics_tick_cb re-syncs the scene node back to the body's stale
        // position every tick and the window snaps out of fullscreen.
        b->x = view->x; b->y = view->y;
        b->width = view->width; b->height = view->height;
        
        view_set_size(view, view->width, view->height);
        view_set_fullscreen_hint(view, real);
        
        if (view->scene_tree) {
            server_place_node(server, &view->scene_tree->node, view->x, view->y);
            wlr_scene_node_raise_to_top(&view->scene_tree->node);
        }
        view_set_border_enabled(view, 0); // borderless fullscreen
        view->fs_real = real ? 1 : 0;

        /* On a tiling desktop the window must leave the layout while
         * fullscreen: otherwise its own fullscreen commit trips
         * server_align_tiles (view.c) and shrinks it straight back into its
         * tile slot. Pull it from the tree and re-tile what remains. */
        if (server->desktop_mode[d] == DESKTOP_MODE_TILING) {
            b->tiled = 0;
            if (bsp_find(server->bsp_roots[d], view->id)) {
                bsp_remove(&server->bsp_roots[d], view->id);
                server_apply_tiling(server, d);
            }
        }
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
                server_place_node(server, &view->scene_tree->node, view->x, view->y);
            }
            view_set_border_enabled(view, 1);
            view->fs_real = 0;
            /* Rejoin the tiling layout. The window may never have been a tile
             * (fullscreened under physics, then tiling switched on), so insert
             * it if the tree does not know it yet before re-applying. */
            if (server->desktop_mode[d] == DESKTOP_MODE_TILING) {
                if (!bsp_find(server->bsp_roots[d], view->id)) {
                    bsp_insert(&server->bsp_roots[d],
                               server->focused_view ? server->focused_view->id : 0,
                               view->id);
                }
                server_apply_tiling(server, d);
            }
        }
    }
    
    /* External panels track this window's size state; it just changed by a
     * bind, a client request or their own button. */
    foreign_view_sync_fullscreen(view);

    server_request_tray_redraw(server);
}

void server_request_tray_redraw(FwmServer *server) {
    /* Nothing to draw into an invisible node — and this runs every physics
     * tick, so skipping it is the point of hiding the tray, not a micro-opt. */
    if (server->tray_hidden) return;
    if (wl_list_empty(&server->outputs)) return;
    
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
    data.mode_name = (server->key_mode >= 0 && server->key_mode < server->config.mode_count)
                         ? server->config.modes[server->key_mode].name : NULL;
    /* Modes pill. Gravity, cava and the ring are global; the per-desktop half
     * is filled in per monitor below. Read live rather than tracked, which is
     * what lets a keybind, `fwmctl set` and the menu all show up here without
     * any of them knowing the pill exists. */
    {
        data.modes_gravity  = server->physics.gravity_scale > 0.0;
        data.modes_cava     = server->config.cava.mode;
        data.modes_ring     = server->config.camera.wrap;
        data.modes_open     = server->modes_buffer != NULL;
    }
    /* The stats pill's whole content, formatted by the sensor engine — the tray
     * draws the line and knows nothing about what is in it. */
    char stats_line[160];
    stats_format(server->stats, stats_line, sizeof(stats_line));
    data.stats_text = stats_line;
    data.stats_open = server->stats_buffer != NULL;
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
    
    /* Each monitor's strip reports ITS OWN desktop: the marker, the layout
     * pill and the window name all belong to the screen they are drawn on. */
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (!out->tray_buffer) continue;
        TrayData d2 = data;

        /* While the desktop strip is up, the camera is parked and the strip's
         * own pan is what moved — so ask it instead, or the marker would sit on
         * the desktop the strip was entered from however far it travelled. */
        if (!expo_view_position(server, &d2.active_pos))
            d2.active_pos = (double)out->camera_x / server->screen_width;
        if (d2.active_pos < 0.0) d2.active_pos = 0.0;
        if (d2.active_pos > 9.0) d2.active_pos = 9.0;
        d2.active_desktop = (int)lround(d2.active_pos);
        if (d2.active_desktop < 0) d2.active_desktop = 0;
        if (d2.active_desktop > 9) d2.active_desktop = 9;

        /* Layout is per-desktop, so the pill reports this monitor's desktop;
         * gravity and cava are global. */
        d2.modes_tiling   = server->desktop_mode[d2.active_desktop] == DESKTOP_MODE_TILING;
        d2.modes_floating = server->desktop_mode[d2.active_desktop] == DESKTOP_MODE_FLOATING;

        /* The focused window belongs to whichever monitor is showing its
         * desktop; the others have nothing to say about it. */
        if (server->focused_view) {
            PhysicsBody *fb = physics_find_body(&server->physics, server->focused_view->id);
            if (!fb || fb->desktop_id != out->desktop) {
                d2.win_name = NULL;
                d2.speed = d2.angle = d2.mass = 0.0;
                d2.flying = 0;
            }
        }

        tray_redraw(out->tray_buffer, &d2, &out->tray_strip);
    }
}
