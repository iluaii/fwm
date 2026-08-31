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
#include "clipboard.h"
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
#include "stats.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "grass.h"
#include "group.h"

#include <dirent.h>
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

/* The state file a monitor's pick lives in: `wallpaper` for the un-named set,
 * `wallpaper.DP-1` for one screen. Output names come from the kernel and carry
 * no slashes, so they are their own file name. */
static void server_state_wallpaper_path(char *buf, size_t cap, const char *output) {
    if (output && output[0]) {
        char name[128];
        snprintf(name, sizeof(name), "wallpaper.%s", output);
        server_state_path(buf, cap, name);
    } else {
        server_state_path(buf, cap, "wallpaper");
    }
}

static void server_state_save_wallpaper(const char *output, const char *path) {
    char sp[512];
    server_state_wallpaper_path(sp, sizeof(sp), output);
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
    fprintf(f, "grass = %s\n", server->config.grass.enabled ? "on" : "off");
    /* The strength, not just on/off: switching the wind off and restarting must
     * not quietly forget a gale someone had tuned. */
    fprintf(f, "wind = %.3f\n", server->config.grass.wind);
    /* The ten desktop modes as ten digits, in strip order, and only while
     * [tiling] remember asks for them: without the line the next start reads
     * `default` from the file, which is what somebody who has just switched
     * remembering off is asking for. */
    if (server->config.tiling.remember) {
        char modes[11];
        for (int d = 0; d < 10; d++) modes[d] = (char)('0' + server->desktop_mode[d]);
        modes[10] = '\0';
        fprintf(f, "desktops = %s\n", modes);
    }
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
        } else if (strcmp(key, "grass") == 0) {
            if      (strcmp(val, "on")  == 0) server->config.grass.enabled = 1;
            else if (strcmp(val, "off") == 0) server->config.grass.enabled = 0;
        } else if (strcmp(key, "desktops") == 0) {
            /* Over `default`, and only when the config still asks to be
             * remembered — the file is the way back, so switching remember off
             * and reloading is enough to get it, without deleting anything.
             * Read into the config rather than into the live desktops because
             * that is where the starting modes live; only the start reads
             * them, so a reload cannot yank a desktop out from under you. */
            if (server->config.tiling.remember)
                for (int d = 0; d < 10 && val[d]; d++)
                    if (val[d] >= '0' && val[d] <= '2')
                        server->config.tiling.default_mode[d] = val[d] - '0';
        } else if (strcmp(key, "wind") == 0) {
            char *end;
            double w = strtod(val, &end);
            if (end != val && w >= 0.0 && w <= 2.0) server->config.grass.wind = w;
        }
        /* No "hp" key: see server_state_save_modes. A file left over from a
         * build that did write one is simply skipped, like any unknown key. */
    }
    fclose(f);
}

/* ── the settings overlay ────────────────────────────────────────────────
 *
 * `fwmctl set` changes a setting for this session and nothing else: the file
 * is the source of truth and a reload is the way back to it. That is the right
 * default and it is also, on its own, a dead end — everything found by trying
 * it (a shadow length that suits your wallpaper, a gravity you actually like)
 * is lost at the next reload unless you go and edit the file by hand.
 *
 * So `save` writes it HERE instead: one `name = value` per line, applied over
 * the config after every load. It is the same contract the wallpaper and the
 * modes files already carry, for the same reason — this file is fwm's to
 * rewrite whenever the user asks, and the moment a config parser touches
 * config.toml somebody's hand-written comments are one bug away from being
 * reformatted. config.toml is still never written.
 *
 * Order: this goes on FIRST and the modes file second, so the two switches the
 * modes menu owns (mass, sound) still answer to the menu. Saving one of them
 * here is legal and will be overruled by a later click, which is the honest
 * outcome — the pill on screen shows what the menu chose, and a saved value
 * that quietly beat it would make the pill a liar.
 *
 * An unknown key is skipped in silence: a file written by a newer fwm must not
 * stop an older one from starting. A key that exists with a value it will not
 * accept is a config problem and goes to the tray's ⚠ pill like any other,
 * because that one IS the user's mistake and silence about it presents as the
 * setting having been forgotten. */

#define SETTINGS_LINE 256

/* What each option was worth with the config file alone — captured after the
 * file is parsed and before this overlay goes on. `save --all` is the whole
 * reason it exists: "everything I have changed" is a question about the file,
 * and once the overlay has been applied nothing can tell the two apart.
 *
 * One string per option, allocated once and reused across reloads. */
static char (*g_file_value)[64];
static int   g_file_count;

void server_settings_baseline(FwmServer *server) {
    int n = 0;
    const ConfigOption *opts = config_options(&n);
    if (n <= 0) return;
    if (g_file_count != n) {
        free(g_file_value);
        g_file_value = calloc((size_t)n, sizeof(*g_file_value));
        g_file_count = g_file_value ? n : 0;
    }
    if (!g_file_value) return;
    for (int i = 0; i < n; i++)
        config_option_get(&server->config, &opts[i], g_file_value[i], sizeof(g_file_value[i]));
}

/* What each option was worth the last time anyone was told about it.
 *
 * The event has to be emitted from ONE place or it will not be emitted from
 * all of them: an option changes through the socket, through a keybind, and
 * through the modes menu, and three call sites is two chances to add a fourth
 * route and forget. So nobody emits a `setting` by hand — the value is simply
 * compared against what was last announced, wherever the compositor happens to
 * settle after a change. */
static char (*g_live_value)[64];
static int   g_live_count;

void server_settings_notify(FwmServer *server) {
    int n = 0;
    const ConfigOption *opts = config_options(&n);
    if (n <= 0) return;

    if (g_live_count != n) {
        free(g_live_value);
        g_live_value = calloc((size_t)n, sizeof(*g_live_value));
        g_live_count = g_live_value ? n : 0;
        if (!g_live_value) return;
        /* First call of the session: this is what things ARE, not news. */
        for (int i = 0; i < n; i++)
            config_option_get(&server->config, &opts[i], g_live_value[i],
                              sizeof(g_live_value[i]));
        return;
    }

    /* Which of them are written down, read once and only when something
     * actually moved — the answer is part of the event, and a script watching
     * a slider wants to know whether what it is seeing will outlive a reload. */
    char names[SETTINGS_MAX][64], values[SETTINGS_MAX][64];
    int saved_n = -1;

    for (int i = 0; i < n; i++) {
        char now[64];
        config_option_get(&server->config, &opts[i], now, sizeof(now));
        if (strcmp(now, g_live_value[i]) == 0) continue;
        snprintf(g_live_value[i], sizeof(g_live_value[i]), "%s", now);

        if (saved_n < 0) saved_n = server_settings_read(names, values, SETTINGS_MAX);
        bool saved = false;
        for (int k = 0; k < saved_n; k++)
            if (strcmp(names[k], opts[i].name) == 0) { saved = true; break; }

        ipc_emit_setting(server->ipc, opts[i].name, now, saved);
    }
}

int server_settings_file_value(const ConfigOption *opt, char *out, size_t cap) {
    int n = 0;
    const ConfigOption *opts = config_options(&n);
    if (!g_file_value || g_file_count != n) return 0;
    for (int i = 0; i < n; i++) {
        if (&opts[i] != opt) continue;
        snprintf(out, cap, "%s", g_file_value[i]);
        return 1;
    }
    return 0;
}

void server_settings_finish(void) {
    free(g_file_value);
    g_file_value = NULL;
    g_file_count = 0;
    free(g_live_value);
    g_live_value = NULL;
    g_live_count = 0;
}

/* Every saved pair, in file order. Returns how many were written to the
 * arrays; a missing file is simply none of them. */
int server_settings_read(char (*names)[64], char (*values)[64], int max) {
    char sp[512];
    server_state_path(sp, sizeof(sp), "settings");
    FILE *f = fopen(sp, "r");
    if (!f) return 0;

    int n = 0;
    char line[SETTINGS_LINE];
    while (n < max && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = '\0';
        while (*val == ' ' || *val == '\t') val++;
        if (!*key || !*val) continue;
        snprintf(names[n], sizeof(names[n]), "%s", key);
        snprintf(values[n], sizeof(values[n]), "%s", val);
        n++;
    }
    fclose(f);
    return n;
}

/* Rewrite the file from a list of pairs. Through a temporary and a rename, so
 * a full disk or a crash mid-write leaves the previous settings rather than
 * half of them — this file is read at every start, and half a line of it is a
 * config problem reported at a login the user has no idea why. */
static int settings_write_all(char (*names)[64], char (*values)[64], int n) {
    char sp[512], tmp[544];
    server_state_path(sp, sizeof(sp), "settings");
    server_state_mkdir_parents(sp);
    snprintf(tmp, sizeof(tmp), "%s.new", sp);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        wlr_log(WLR_ERROR, "cannot save settings to %s", sp);
        return 0;
    }
    for (int i = 0; i < n; i++) fprintf(f, "%s = %s\n", names[i], values[i]);
    int ok = fflush(f) == 0;
    fclose(f);
    if (!ok || rename(tmp, sp) != 0) {
        wlr_log(WLR_ERROR, "cannot save settings to %s", sp);
        unlink(tmp);
        return 0;
    }
    return 1;
}

int server_settings_write(const char *name, const char *value) {
    char names[SETTINGS_MAX][64], values[SETTINGS_MAX][64];
    int n = server_settings_read(names, values, SETTINGS_MAX);

    int at = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) { at = i; break; }

    if (!value) {                      /* forget it */
        if (at < 0) return 1;          /* not saved: already the wanted state */
        for (int i = at; i + 1 < n; i++) {
            memcpy(names[i], names[i + 1], sizeof(names[i]));
            memcpy(values[i], values[i + 1], sizeof(values[i]));
        }
        n--;
    } else if (at >= 0) {
        snprintf(values[at], sizeof(values[at]), "%s", value);
    } else {
        if (n >= SETTINGS_MAX) return 0;
        snprintf(names[n], sizeof(names[n]), "%s", name);
        snprintf(values[n], sizeof(values[n]), "%s", value);
        n++;
    }
    return settings_write_all(names, values, n);
}

int server_settings_save_all(FwmServer *server) {
    int n = 0;
    const ConfigOption *opts = config_options(&n);
    if (!g_file_value || g_file_count != n) return -1;

    char names[SETTINGS_MAX][64], values[SETTINGS_MAX][64];
    int saved = 0;
    for (int i = 0; i < n && saved < SETTINGS_MAX; i++) {
        char now[64];
        config_option_get(&server->config, &opts[i], now, sizeof(now));
        if (strcmp(now, g_file_value[i]) == 0) continue;
        snprintf(names[saved], sizeof(names[saved]), "%s", opts[i].name);
        snprintf(values[saved], sizeof(values[saved]), "%s", now);
        saved++;
    }
    /* The whole overlay is replaced rather than added to: "save everything
     * that differs from the file" is a complete statement about the session,
     * and a leftover line from an earlier save that the session has since put
     * back to its configured value is not part of it. */
    if (!settings_write_all(names, values, saved)) return -1;
    return saved;
}

void server_state_apply_settings(FwmServer *server) {
    char names[SETTINGS_MAX][64], values[SETTINGS_MAX][64];
    int n = server_settings_read(names, values, SETTINGS_MAX);

    for (int i = 0; i < n; i++) {
        const ConfigOption *opt = config_option_find(names[i]);
        if (!opt) continue;   /* written by a newer fwm; not our business */
        char err[192];
        if (!config_option_set(&server->config, opt, values[i], err, sizeof(err)))
            config_report_error(&server->config, "saved setting %s", err);
    }
}

/* Read one remembered pick and put it over the configured layers. A stale
 * entry (image deleted since) must not blank the wallpaper: it is dropped and
 * whatever the config says stands. */
static void state_apply_wallpaper_file(FwmServer *server, const char *output,
                                       const char *file) {
    FILE *f = fopen(file, "r");
    if (!f) return;

    char line[512];
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && access(line, R_OK) == 0) {
            WallpaperLayer *w = config_wallpaper_set_path(&server->config, output, line);
            /* Remembered picks are picker choices, so honour the picker's base
             * fps here too — a restart must not revert to source rate. */
            if (w && server->config.wallpaper_picker_fps > 0.0)
                w->fps = server->config.wallpaper_picker_fps;
        }
    }
    fclose(f);
}

/* Apply the remembered wallpapers over the configured ones. Called after every
 * config load, so a reload keeps the picked images rather than snapping back.
 *
 * The per-monitor files are found by reading the state directory rather than
 * by asking the outputs: this runs while the config is being loaded, before
 * anyone knows which screens are plugged in, and a pick made on a monitor that
 * is currently dark must still be there when it comes back. */
void server_state_apply_wallpaper(FwmServer *server) {
    char sp[512];
    server_state_wallpaper_path(sp, sizeof(sp), NULL);
    state_apply_wallpaper_file(server, "", sp);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", sp);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "wallpaper.", 10) != 0) continue;
        const char *output = e->d_name + 10;
        if (!output[0]) continue;
        char file[600];
        snprintf(file, sizeof(file), "%s/%s", dir, e->d_name);
        state_apply_wallpaper_file(server, output, file);
    }
    closedir(d);
}

/* Rebuild the wallpaper of every monitor that shows `layer`, cross-fading from
 * what it had. The monitors that do not show it are left alone: re-decoding
 * another screen's image to fade it into itself is work that buys nothing. */
static void wallpaper_rebuild_showing(FwmServer *server, const WallpaperLayer *layer) {
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (layer && !config_wallpaper_on_output(&server->config, layer,
                                                 out->wlr_output->name))
            continue;

        /* A swap still in flight is finished immediately, so rapid picking
         * cannot pile up wallpapers. */
        if (out->wallpaper_prev) {
            wallpaper_destroy(out->wallpaper_prev);
            out->wallpaper_prev = NULL;
        }
        out->wallpaper_prev = out->wallpaper;
        out->wallpaper = wallpaper_create(server->layer_background, &server->config,
                                          out->wlr_output->name,
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
        /* The new set went in above everything already in the background
         * layer, grass included. */
        if (out->grass) grass_raise(out->grass);
    }
}

/* Which monitor a pick is for, and therefore which layer and which state file
 * it is written to. */
static const char *wallpaper_pick_target(FwmServer *server) {
    FwmOutput *out = server_active_output(server);
    if (!out) return "";
    const char *name = out->wlr_output->name;

    /* A screen with a layer of its own is written by name whatever else is
     * plugged in: it shows only what names it, so a pick written to the
     * un-named set would land on a layer this monitor never draws — the picker
     * would look like it had done nothing. */
    for (int i = 0; i < server->config.wallpaper_count; i++)
        if (strcmp(server->config.wallpapers[i].output, name) == 0) return name;

    /* Otherwise: with one screen there is nothing to choose and the un-named
     * set is written, so a single-monitor session keeps exactly the state file
     * it always had. With two, the pick lands on the screen you are looking at
     * and the other one keeps its own image. */
    return wl_list_length(&server->outputs) < 2 ? "" : name;
}

void server_set_wallpaper(FwmServer *server, const char *path) {
    if (!path || !path[0]) return;
    if (access(path, R_OK) != 0) {
        wlr_log(WLR_ERROR, "wallpaper '%s' is not readable", path);
        return;
    }

    const char *output = wallpaper_pick_target(server);

    /* Only the path changes: fit and zoom stay as configured, so a "pan" setup
     * keeps panning with the new image. A monitor with no layer of its own
     * gets one, as "cover". */
    WallpaperLayer *layer = config_wallpaper_set_path(&server->config, output, path);
    if (!layer) return;

    /* A video chosen through the picker takes the picker's base fps cap, so the
     * user has a single knob for everything they pick. Only when one is set
     * (>0), so an explicit per-layer [[wallpaper]] fps is not clobbered. */
    if (server->config.wallpaper_picker_fps > 0.0)
        layer->fps = server->config.wallpaper_picker_fps;

    /* Cross-fade rather than cut: the outgoing set stays underneath (the new
     * one is created later, so the scene draws it on top) until the fade
     * ends. */
    wallpaper_rebuild_showing(server, layer);

    /* Start (or stop) driving video frames for whatever was just built. */
    server_video_sync(server);

    /* The palette may be derived from the image that just changed, and it is
     * the screen the user is looking at that it should come from. */
    snprintf(server->config.palette_output, sizeof(server->config.palette_output),
             "%s", output);
    theme_build(&server->config);
    ipc_emit_palette(server->ipc);
    server_request_tray_redraw(server);

    server_state_save_wallpaper(output, path);
    if (output[0]) wlr_log(WLR_INFO, "wallpaper on %s set to %s", output, path);
    else           wlr_log(WLR_INFO, "wallpaper set to %s", path);
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

/* Which monitor the un-tied palette should be taken from: the one the user is
 * on. Empty with a single screen (there is nothing to choose, and the un-named
 * [[wallpaper]] set is what a lone monitor shows) or while no monitor is up
 * yet — then the first layer answers, as it always did. */
static const char *palette_target(FwmServer *server) {
    FwmOutput *active = server_active_output(server);
    return active && wl_list_length(&server->outputs) > 1
               ? active->wlr_output->name : "";
}

void server_palette_sync(FwmServer *server) {
    const char *name = palette_target(server);
    if (strcmp(server->config.palette_output, name) == 0) return;
    snprintf(server->config.palette_output, sizeof(server->config.palette_output),
             "%s", name);
    /* The per-monitor palettes are already built, so this is a lookup. Two
     * screens whose images derive the same colours change nothing on screen,
     * and then nothing is repainted and nobody is told. */
    if (!theme_set_active_output(name)) return;
    ipc_emit_palette(server->ipc);
    server_request_tray_redraw(server);
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
    /* A reload re-reads the file, which knows nothing about which screen the
     * palette came from; point it back at the active one before the colours
     * are computed. */
    snprintf(server->config.palette_output, sizeof(server->config.palette_output),
             "%s", palette_target(server));

    /* Before anything reads colours: a new wallpaper or color_source repaints
     * the whole system. */
    theme_build(&server->config);
    ipc_emit_palette(server->ipc);

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
        view_refresh_border_color(view);
    }

    /* The sun: a new [sun] may have turned the shadows on, off, or moved them,
     * and this is also what gives every window its nodes the first time it is
     * switched on at runtime. */
    server_sun_apply(server);

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
                                                  out->wlr_output->name,
                                                  out->box.width, out->box.height);
                if (out->wallpaper) {
                    wallpaper_set_origin(out->wallpaper, out->box.x, out->box.y);
                    wallpaper_update(out->wallpaper, out->camera_x);
                }
            }
            if (out->grass) grass_raise(out->grass);
        }
        /* A reload that drops a video wallpaper releases the same hundreds of
         * MB as picking a new one, and takes the same cut-not-fade path. */
        server_reclaim_memory();
    }

    /* Monitors may have moved, been turned off, or been pointed at another
     * desktop. Before the tiling below, which is sized to a screen. */
    server_outputs_apply_config(server);

    /* The shape of the next split — [tiling] split_ratio and force_split.
     * Existing splits keep the ratio they were made with, the same as they keep
     * one dragged by hand: this says how a split is BORN, and re-cutting the
     * layout under a reload would throw away every divider ever placed. */
    bsp_configure((float)server->config.tiling.split_ratio,
                  server->config.tiling.force_split);

    /* New gaps / anim settings take effect on tiled desktops. */
    for (int d = 0; d < 10; d++) {
        if (server->desktop_mode[d] == DESKTOP_MODE_TILING) server_apply_tiling(server, d);
    }

    /* The clipboard's two settings, live. Here rather than in
     * server_reload_config because `fwmctl set clipboard.persist=0` never
     * reaches that function — a socket set writes the value and calls this one,
     * so anything that must follow a set belongs on this path. */
    clipboard_configure(server->clipboard, server->config.clipboard.persist != 0,
                        (size_t)(server->config.clipboard.max_kb * 1024.0));

    server_request_tray_redraw(server);

    /* Last: whatever this changed, the subscribers hear about it here and
     * nowhere else. */
    server_settings_notify(server);
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
    /* Between the file and everything that goes over it: what save --all
     * later means by "changed". */
    server_settings_baseline(server);
    server_state_apply_settings(server);
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

    /* The sensors are rebuilt against the new [stats]: names that survived keep
     * their value and their on/off, so a reload does not blank the pill for an
     * interval nor undo what the menu was used to choose. */
    stats_reconfigure(server->stats, &server->config.stats);

    /* An open menu is a list of the OLD sensors, and the click that would arrive
     * next is indexed into it. */
    server_close_stats_menu(server);

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
