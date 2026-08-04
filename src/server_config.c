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

/* Config and persisted state: where the config and state files live, applying
 * a loaded config to a running compositor, live reload, and the choices the UI
 * remembers (the picked wallpaper, the modes menu) without ever rewriting the
 * user's config. Split out of server.c; see server_internal.h. */
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
#include "sound.h"
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
#include "server_internal.h"

void server_config_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (home) snprintf(buf, cap, "%s%s", home, FWM_CONFIG_PATH);
    else      snprintf(buf, cap, ".config/fwm/config.toml");
}

/* Close the error panel if it is open; the caller decides whether to reopen it
 * against the new config. */
void server_close_errors_panel(FwmServer *server) {
    if (server->errors_buffer) {
        cairo_overlay_destroy(server->errors_buffer);
        server->errors_buffer = NULL;
    }
}

/* ~/.local/state/fwm/<name> — choices made through the UI, kept out of
 * config.toml so the user's file (comments, formatting) is never rewritten by
 * us. `wallpaper` is the picker's image; `modes` is what the modes menu was
 * last left set to. */
static void server_state_path(char *buf, size_t cap, const char *name) {
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state && state[0]) snprintf(buf, cap, "%s/fwm/%s", state, name);
    else if (home)         snprintf(buf, cap, "%s/.local/state/fwm/%s", home, name);
    else                   snprintf(buf, cap, ".fwm-%s", name);
}

/* mkdir -p of a file's parent, one component at a time. */
static void server_state_mkdir_parents(const char *file) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
    }
    mkdir(dir, 0755);
}

static void server_state_save_wallpaper(const char *path) {
    char sp[512];
    server_state_path(sp, sizeof(sp), "wallpaper");
    server_state_mkdir_parents(sp);

    FILE *f = fopen(sp, "w");
    if (!f) {
        wlr_log(WLR_ERROR, "cannot save wallpaper choice to %s", sp);
        return;
    }
    fprintf(f, "%s\n", path);
    fclose(f);
}

/* ── remembered modes ────────────────────────────────────────────────────
 *
 * A setting flipped in the modes menu has to survive a restart, or the menu is
 * a toy: nobody wants to re-pick "windows weigh what they eat" every login.
 *
 * Written as `key = value` lines rather than as TOML fragments the config
 * loader also reads, deliberately: this file is OURS to rewrite whenever the
 * user clicks something, and the moment a config parser touches it somebody's
 * hand-written config.toml is one bug away from being reformatted. Unknown
 * keys are skipped, so a file written by a newer fwm never stops an older one
 * from starting. */
void server_state_save_modes(FwmServer *server) {
    char sp[512];
    server_state_path(sp, sizeof(sp), "modes");
    server_state_mkdir_parents(sp);

    FILE *f = fopen(sp, "w");
    if (!f) {
        wlr_log(WLR_ERROR, "cannot save mode choices to %s", sp);
        return;
    }
    fprintf(f, "mass = %s\n",
            server->config.physics.mass_mode == PHYSICS_MASS_RAM ? "ram" : "size");
    fprintf(f, "sound = %s\n", server->config.sound.collisions ? "on" : "off");
    /* hp is deliberately NOT written. It is the one mode that can destroy
     * unsaved work, and a setting that survives a restart is one you can be
     * living under without having chosen it today. Every session starts with
     * windows unbreakable; turning it on is always a deliberate act. */
    fclose(f);
}

/* Apply the remembered modes over the loaded config. Called after every config
 * load, so a reload keeps what the menu was set to rather than snapping back —
 * the same contract server_state_apply_wallpaper has. The remembered choice
 * wins over the file because it is the more recent of the two: it exists only
 * because the user clicked it. Deleting the state file goes back to the
 * config. */
void server_state_apply_modes(FwmServer *server) {
    char sp[512];
    server_state_path(sp, sizeof(sp), "modes");
    FILE *f = fopen(sp, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        /* Trim the spaces either side of the '='. */
        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--)
            e[-1] = '\0';
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strcmp(key, "mass") == 0) {
            if      (strcmp(val, "ram")  == 0) server->config.physics.mass_mode = PHYSICS_MASS_RAM;
            else if (strcmp(val, "size") == 0) server->config.physics.mass_mode = PHYSICS_MASS_SIZE;
        } else if (strcmp(key, "sound") == 0) {
            if      (strcmp(val, "on")  == 0) server->config.sound.collisions = 1;
            else if (strcmp(val, "off") == 0) server->config.sound.collisions = 0;
        }
        /* No "hp" key: see server_state_save_modes. A file left over from a
         * build that did write one is simply skipped, like any unknown key. */
    }
    fclose(f);
}

/* Apply the remembered wallpaper over the configured one. Called after every
 * config load, so a reload keeps the picked image rather than snapping back. */
void server_state_apply_wallpaper(FwmServer *server) {
    char sp[512];
    server_state_path(sp, sizeof(sp), "wallpaper");
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
            if (server->config.wallpaper_count > 0) {
                snprintf(server->config.wallpapers[0].path,
                         sizeof(server->config.wallpapers[0].path), "%s", line);
                /* Remembered picks are picker choices, so honour the picker's
                 * base fps here too — a restart must not revert to source rate. */
                if (server->config.wallpaper_picker_fps > 0.0)
                    server->config.wallpapers[0].fps = server->config.wallpaper_picker_fps;
            }
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

    /* A video chosen through the picker takes the picker's base fps cap, so the
     * user has a single knob for everything they pick. Only when one is set
     * (>0), so an explicit per-layer [[wallpaper]] fps is not clobbered. */
    if (server->config.wallpaper_picker_fps > 0.0)
        server->config.wallpapers[0].fps = server->config.wallpaper_picker_fps;

    /* Cross-fade rather than cut: the outgoing set stays underneath (the new
     * one is created later, so the scene draws it on top) until the fade ends.
     * A swap still in flight is finished immediately, so rapid picking cannot
     * pile up wallpapers. */
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->wallpaper_prev) {
            wallpaper_destroy(out->wallpaper_prev);
            out->wallpaper_prev = NULL;
        }
        out->wallpaper_prev = out->wallpaper;
        out->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                          out->box.width, out->box.height);
        if (out->wallpaper) {
            wallpaper_set_origin(out->wallpaper, out->box.x, out->box.y);
            wallpaper_update(out->wallpaper, out->camera_x);
            if (out->wallpaper_prev)
                wallpaper_fade_in(out->wallpaper, server->config.decor.wallpaper_fade_ms);
        }
        if (!out->wallpaper || server->config.decor.wallpaper_fade_ms <= 0.0) {
            if (out->wallpaper_prev) {
                wallpaper_destroy(out->wallpaper_prev);
                out->wallpaper_prev = NULL;
                /* Instant cut: the outgoing set is gone right here, so reclaim
                 * now. With a fade it happens in server_animate instead, once
                 * the new set is opaque. */
                server_reclaim_memory();
            }
        }
    }

    /* Start (or stop) driving video frames for whatever was just built. */
    server_video_sync(server);

    /* The palette may be derived from the image that just changed. */
    theme_build(&server->config);
    server_request_tray_redraw(server);

    server_state_save_wallpaper(path);
    wlr_log(WLR_INFO, "wallpaper set to %s", path);
}

void server_apply_physics_config(FwmServer *server) {
    const PhysicsConfig *pc = &server->config.physics;

    /* Physics knobs are plain scalars on the live world. */
    server->physics.friction               = pc->friction;
    server->physics.mass_density           = pc->mass_density;
    server->physics.throw_speed_multiplier = pc->throw_speed_multiplier;
    server->physics.max_throw_speed        = pc->max_throw_speed;
    server->physics.stop_speed_threshold   = pc->stop_speed_threshold;
    server->physics.restitution            = pc->restitution;
    server->physics.gravity                = pc->gravity;

    /* Every desktop starts on those, then takes its profile if it has one.
     * Order matters: reset reads the scalars just written. */
    physics_reset_profiles(&server->physics);
    for (int d = 0; d < FWM_DESKTOPS; d++) {
        int idx = pc->desktop_profile[d];
        if (idx < 0 || idx >= pc->profile_count) continue;
        const PhysicsProfileConfig *pr = &pc->profiles[idx];
        server->physics.desktops[d].friction     = pr->friction;
        server->physics.desktops[d].mass_density = pr->mass_density;
        server->physics.desktops[d].restitution  = pr->restitution;
        server->physics.desktops[d].gravity      = pr->gravity;
    }
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

    server_apply_physics_config(server);

    /* Keyboards: layout/variant/options/repeat may all have changed. */
    struct FwmKeyboard *kb;
    wl_list_for_each(kb, &server->keyboards, link) {
        keyboard_apply_input_config(server, kb->wlr_keyboard);
    }

    /* Touchpads: tap, scrolling, acceleration. */
    struct FwmPointer *pt;
    wl_list_for_each(pt, &server->pointers, link) {
        pointer_apply_input_config(server, pt->device);
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
        FwmOutput *out;
        wl_list_for_each(out, &server->outputs, link) {
            if (out->wallpaper_prev) {
                wallpaper_destroy(out->wallpaper_prev);
                out->wallpaper_prev = NULL;
            }
            if (out->wallpaper) {
                wallpaper_destroy(out->wallpaper);
                out->wallpaper = NULL;
            }
            if (server->config.wallpaper_count > 0) {
                out->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                                  out->box.width, out->box.height);
                if (out->wallpaper) {
                    wallpaper_set_origin(out->wallpaper, out->box.x, out->box.y);
                    wallpaper_update(out->wallpaper, out->camera_x);
                }
            }
        }
        /* A reload that drops a video wallpaper releases the same hundreds of
         * MB as picking a new one, and takes the same cut-not-fade path. */
        server_reclaim_memory();
    }

    /* Monitors may have moved, been turned off, or been pointed at another
     * desktop. Before the tiling below, which is sized to a screen. */
    server_outputs_apply_config(server);

    /* New gaps / anim settings take effect on tiled desktops. */
    for (int d = 0; d < FWM_DESKTOPS; d++) {
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

    /* Modes are rebuilt from scratch, so an index into the old ones would point
     * at whatever happens to sit there now. Back to the root map. */
    server->key_mode = -1;

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
    server_state_apply_modes(server);

    /* The collision sample is loaded once and then read by the mixer thread
     * without a lock, so a new [sound] path is a rebuild rather than an update.
     * Dropping the mixer here is the whole of it: the next tick sees the
     * feature on with nothing running and starts it again, with the new file.
     * Cheap, because a mixer that is not playing holds no audio device. */
    if (server->sound) {
        sound_destroy(server->sound);
        server->sound = NULL;
    }
    server->sound_applied = 0;

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

    /* Anything a subscriber cached from `config` or `get` is now stale. */
    ipc_emit_config_reload(server->ipc);
}
