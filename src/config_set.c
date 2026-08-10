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


#include "config_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ── runtime-settable options ────────────────────────────────────────── */

/* physics.tick_rate is deliberately absent: the tick timer is armed once at
 * startup, so accepting a new value here would report success and change
 * nothing. Same reasoning excludes the string options (icon_theme, kbd_*) —
 * they are re-read only by a full reload. */
static const ConfigOption config_option_table[] = {
    { "physics.friction",               CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.friction),               0.0,     1.0,     "velocity retained per tick" },
    { "physics.mass_density",           CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.mass_density),            0.0,     1.0,     "mass per pixel of window area" },
    /* Numeric for the same reason cava.mode is: the table is typed. 0 = size,
     * 1 = ram, the two the enum spells out. */
    { "physics.mass",                   CFG_OPT_INT,    offsetof(FwmConfig, physics.mass_mode),               0.0,     1.0,     "0 mass from window size, 1 from RAM use" },
    { "physics.mass_ram_ref",           CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.mass_ram_ref),            1.0, 1000000.0,   "MB that weighs a normal window" },
    { "physics.mass_ram_max",           CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.mass_ram_max),            1.0,   200.0,     "heaviest a window may get, x normal" },
    { "physics.throw_speed_multiplier", CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.throw_speed_multiplier),  0.0,    10.0,     "how hard a drag throws" },
    { "physics.max_throw_speed",        CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.max_throw_speed),         0.0, 100000.0,    "throw speed cap, px/s" },
    { "physics.stop_speed_threshold",   CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.stop_speed_threshold),    0.0,  1000.0,    "below this a body is put to sleep" },
    { "physics.restitution",            CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.restitution),             0.0,     1.0,     "bounciness, 0 = dead stop" },
    { "physics.gravity",                CFG_OPT_DOUBLE, offsetof(FwmConfig, physics.gravity),           -100000.0, 100000.0,    "px/s^2; 981 = earth at 100px/m" },

    { "tiling.gaps_in",                 CFG_OPT_INT,    offsetof(FwmConfig, tiling.gaps_in),                  0.0,   500.0,    "gap between tiles, px" },
    { "tiling.gaps_out",                CFG_OPT_INT,    offsetof(FwmConfig, tiling.gaps_out),                 0.0,   500.0,    "gap to the screen edge, px" },
    { "tiling.anim_speed",              CFG_OPT_DOUBLE, offsetof(FwmConfig, tiling.anim_speed),               0.0,  1000.0,    "tile-glide rate, 1/s; 0 disables" },

    { "camera.anim_ms",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, camera.anim_ms),                  0.0, 10000.0,    "desktop-switch slide, ms" },
    { "camera.free_speed",              CFG_OPT_DOUBLE, offsetof(FwmConfig, camera.free_speed),               0.0,  1000.0,    "held move_camera chase rate, 1/s" },
    { "camera.wrap",                    CFG_OPT_INT,    offsetof(FwmConfig, camera.wrap),                     0.0,     1.0,    "the strip is a ring: stepping past the last desktop arrives on the first" },

    { "decor.border_width",             CFG_OPT_INT,    offsetof(FwmConfig, decor.border_width),              0.0,    64.0,    "focus border, px; 0 disables" },
    { "decor.col_active",               CFG_OPT_COLOR,  offsetof(FwmConfig, decor.col_active),                0.0,     0.0,    "focused border colour" },
    { "decor.col_inactive",             CFG_OPT_COLOR,  offsetof(FwmConfig, decor.col_inactive),              0.0,     0.0,    "unfocused border colour" },
    { "decor.fade_in_ms",               CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.fade_in_ms),                0.0, 10000.0,    "window open animation, ms" },
    { "decor.wallpaper_fade_ms",        CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.wallpaper_fade_ms),         0.0, 10000.0,    "wallpaper cross-fade, ms" },
    { "decor.tray_opacity",             CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.tray_opacity),              0.0,     1.0,    "tray island fill alpha" },
    { "decor.tray_yield",               CFG_OPT_INT,    offsetof(FwmConfig, decor.tray_yield),                0.0,     1.0,    "hide the strip on a screen where a bar reserved the top" },
    { "decor.launcher_opacity",         CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.launcher_opacity),          0.0,     1.0,    "launcher island fill alpha" },
    { "decor.tint_strength",            CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.tint_strength),             0.0,     1.0,    "island tint toward the wallpaper hue" },

    { "effects.camera_shake",           CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.camera_shake),            0.0,     4.0,    "impact shake; 0 disables" },
    { "effects.squash",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.squash),                  0.0,     4.0,    "impact squash & stretch; 0 disables" },
    { "effects.jelly",                  CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.jelly),                   0.0,     4.0,    "drag wobble; 0 disables" },
    { "effects.spin",              CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.spin),                    0.0,     4.0,    "free rotation kick (experimental); 0 disables" },
    { "effects.live",              CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.live),                    0.0,     1.0,    "live content under spin/wobble; 0 = still frame" },

    { "input.repeat_rate",              CFG_OPT_INT,    offsetof(FwmConfig, input.repeat_rate),               0.0,   200.0,    "key repeat, chars/s" },
    { "input.repeat_delay",             CFG_OPT_INT,    offsetof(FwmConfig, input.repeat_delay),              0.0,  5000.0,    "ms before repeat starts" },

    { "gestures.sensitivity",           CFG_OPT_DOUBLE, offsetof(FwmConfig, gestures.sensitivity),            0.1,    10.0,    "camera px per finger px" },
    { "gestures.natural",               CFG_OPT_INT,    offsetof(FwmConfig, gestures.natural),                0.0,     1.0,    "1 = the strip follows the fingers" },

    /* `mode` is settable as a number because the option table is typed, not
     * because anyone should enjoy writing it: 0 off, 1 visual, 2 physical,
     * 3 both — the same bits the enum spells out. `bars` is deliberately NOT
     * here: changing it rebuilds every scene rect and every kinematic body,
     * which is a config-reload job, not a live knob. */
    { "cava.mode",                      CFG_OPT_INT,    offsetof(FwmConfig, cava.mode),                       0.0,     3.0,    "0 off, 1 visual, 2 physical, 3 both" },
    { "cava.height",                    CFG_OPT_DOUBLE, offsetof(FwmConfig, cava.height),                     8.0,  2000.0,    "px the loudest band reaches" },
    { "cava.sensitivity",               CFG_OPT_DOUBLE, offsetof(FwmConfig, cava.sensitivity),                0.0,    20.0,    "spectrum gain" },
    { "cava.smoothing",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, cava.smoothing),                  0.0,    0.99,    "bar fall inertia; 0 = instant" },
    { "cava.push",                      CFG_OPT_DOUBLE, offsetof(FwmConfig, cava.push),                       0.0,     4.0,    "physical bar height vs. drawn" },
    { "cava.opacity",                   CFG_OPT_DOUBLE, offsetof(FwmConfig, cava.opacity),                    0.0,     1.0,    "drawn bar alpha" },

    /* `path` is absent for the same reason cava.bars is: a new sample means
     * reloading it, which is a config-reload job. Everything else here is felt
     * on the next collision. */
    { "sound.collisions",               CFG_OPT_INT,    offsetof(FwmConfig, sound.collisions),                0.0,     1.0,    "1 = windows knock when they collide" },
    { "sound.volume",                    CFG_OPT_DOUBLE, offsetof(FwmConfig, sound.volume),                   0.0,     1.0,    "collision sound volume" },
    { "sound.min_speed",                CFG_OPT_DOUBLE, offsetof(FwmConfig, sound.min_speed),                 0.0, 100000.0,   "px/s below which a hit is silent" },
    { "sound.max_speed",                CFG_OPT_DOUBLE, offsetof(FwmConfig, sound.max_speed),                 1.0, 100000.0,   "px/s at which a hit is full volume" },
};

const ConfigOption *config_options(int *count) {
    if (count) *count = (int)(sizeof(config_option_table) / sizeof(config_option_table[0]));
    return config_option_table;
}

const ConfigOption *config_option_find(const char *name) {
    if (!name) return NULL;
    int n;
    const ConfigOption *tbl = config_options(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(tbl[i].name, name) == 0) return &tbl[i];
    return NULL;
}

int config_option_set(FwmConfig *cfg, const ConfigOption *opt,
                      const char *value, char *err, size_t errcap) {
    if (!value || !*value) {
        snprintf(err, errcap, "%s needs a value", opt->name);
        return 0;
    }
    char *field = (char *)cfg + opt->offset;

    if (opt->type == CFG_OPT_COLOR) {
        float rgba[4];
        if (!parse_hex_color(value, rgba)) {
            snprintf(err, errcap, "%s: expected #RRGGBB or #RRGGBBAA, got \"%s\"",
                     opt->name, value);
            return 0;
        }
        memcpy(field, rgba, sizeof(rgba));
        return 1;
    }

    char *end;
    double v = strtod(value, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (end == value || *end) {
        snprintf(err, errcap, "%s: expected a number, got \"%s\"", opt->name, value);
        return 0;
    }
    /* Rejected rather than clamped: over a socket a silent clamp is
     * indistinguishable from the value having been accepted. */
    if (v < opt->min || v > opt->max) {
        snprintf(err, errcap, "%s: %g is outside %g..%g", opt->name, v, opt->min, opt->max);
        return 0;
    }

    if (opt->type == CFG_OPT_INT) *(int *)field = (int)v;
    else                          *(double *)field = v;
    return 1;
}

void config_option_get(const FwmConfig *cfg, const ConfigOption *opt,
                       char *out, size_t cap) {
    const char *field = (const char *)cfg + opt->offset;

    switch (opt->type) {
    case CFG_OPT_INT:
        snprintf(out, cap, "%d", *(const int *)field);
        break;
    case CFG_OPT_COLOR: {
        /* parse_hex_color stores premultiplied alpha for wlr_scene_rect, so
         * undo that on the way out or every colour reads back darkened. */
        const float *c = (const float *)field;
        float a = c[3];
        float r = a > 0.0f ? c[0] / a : 0.0f;
        float g = a > 0.0f ? c[1] / a : 0.0f;
        float b = a > 0.0f ? c[2] / a : 0.0f;
        snprintf(out, cap, "#%02X%02X%02X%02X",
                 (unsigned)(r * 255.0f + 0.5f), (unsigned)(g * 255.0f + 0.5f),
                 (unsigned)(b * 255.0f + 0.5f), (unsigned)(a * 255.0f + 0.5f));
        break;
    }
    default:
        snprintf(out, cap, "%g", *(const double *)field);
        break;
    }
}

