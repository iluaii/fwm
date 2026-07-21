/* Config and persisted state: where the config and state files live, applying
 * a loaded config to a running compositor, live reload, and the wallpaper the
 * picker remembers without ever rewriting the user's config. Split out of
 * server.c; see server_internal.h. */
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
void server_state_apply_wallpaper(FwmServer *server) {
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
