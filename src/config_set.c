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
    { "tiling.pickup",                  CFG_OPT_DOUBLE, offsetof(FwmConfig, tiling.pickup),                   0.0,     0.9,    "size a window leaves the layout at, fraction of the screen" },

    { "camera.anim_ms",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, camera.anim_ms),                  0.0, 10000.0,    "desktop-switch slide, ms" },
    { "camera.free_speed",              CFG_OPT_DOUBLE, offsetof(FwmConfig, camera.free_speed),               0.0,  1000.0,    "held move_camera chase rate, 1/s" },
    { "camera.wrap",                    CFG_OPT_INT,    offsetof(FwmConfig, camera.wrap),                     0.0,     1.0,    "the strip is a ring: stepping past the last desktop arrives on the first" },
    { "camera.back_and_forth",          CFG_OPT_INT,    offsetof(FwmConfig, camera.back_and_forth),           0.0,     1.0,    "the bind for the desktop you are on returns to the one you came from" },

    /* [idle]. Seconds, and the top of the range is a day: a machine that must
     * never blank says 0, and everything else is a coffee break. The locker's
     * command line is a string and therefore reload-only, like every other
     * string in this table. */
    { "clipboard.persist",              CFG_OPT_INT,    offsetof(FwmConfig, clipboard.persist),               0.0,     1.0,    "1 = keep the last copied text after its window closes" },
    { "clipboard.max_kb",               CFG_OPT_DOUBLE, offsetof(FwmConfig, clipboard.max_kb),                1.0, 65536.0,    "biggest selection kept, KB" },

    { "battery.low",                    CFG_OPT_INT,    offsetof(FwmConfig, battery.low),                     0.0,   100.0,    "percent at or below which the low-charge line shows; 0 = never" },
    { "battery.critical",               CFG_OPT_INT,    offsetof(FwmConfig, battery.critical),                0.0,   100.0,    "percent for the red one, and for [battery] command; 0 = never" },

    { "idle.blank_after",               CFG_OPT_DOUBLE, offsetof(FwmConfig, idle.blank_after),                0.0, 86400.0,    "seconds of no input before the screens go dark; 0 = never" },
    { "idle.lock_after",                CFG_OPT_DOUBLE, offsetof(FwmConfig, idle.lock_after),                 0.0, 86400.0,    "seconds before [idle] lock is run; 0 = never" },
    { "idle.audio_holds",               CFG_OPT_INT,    offsetof(FwmConfig, idle.audio_holds),                0.0,     1.0,    "1 = sound playing puts both timers back to the start" },

    { "decor.border_width",             CFG_OPT_INT,    offsetof(FwmConfig, decor.border_width),              0.0,    64.0,    "focus border, px; 0 disables" },
    { "decor.col_active",               CFG_OPT_COLOR,  offsetof(FwmConfig, decor.col_active),                0.0,     0.0,    "focused border colour" },
    { "decor.col_inactive",             CFG_OPT_COLOR,  offsetof(FwmConfig, decor.col_inactive),              0.0,     0.0,    "unfocused border colour" },
    { "decor.fade_in_ms",               CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.fade_in_ms),                0.0, 10000.0,    "window open animation, ms" },
    { "decor.wallpaper_fade_ms",        CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.wallpaper_fade_ms),         0.0, 10000.0,    "wallpaper cross-fade, ms" },
    { "decor.tray_opacity",             CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.tray_opacity),              0.0,     1.0,    "tray island fill alpha" },
    { "decor.tray_yield",               CFG_OPT_INT,    offsetof(FwmConfig, decor.tray_yield),                0.0,     1.0,    "hide the strip on a screen where a bar reserved the top" },
    { "decor.launcher_opacity",         CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.launcher_opacity),          0.0,     1.0,    "launcher island fill alpha" },
    { "decor.tint_strength",            CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.tint_strength),             0.0,     1.0,    "island tint toward the wallpaper hue" },
    { "decor.inactive_opacity",         CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.inactive_opacity),          0.0,     1.0,    "how much of itself an unfocused window keeps" },
    { "decor.dim_ms",                   CFG_OPT_DOUBLE, offsetof(FwmConfig, decor.dim_ms),                    0.0, 10000.0,    "how long the unfocused dim takes, ms" },

    /* The frost under fwm's own panels. `fill` takes over from the two decor
     * opacities above while the glass is on — see [glass] in config.h. */
    { "glass.enabled",                  CFG_OPT_INT,    offsetof(FwmConfig, glass.enabled),                   0.0,     1.0,    "1 = fwm's panels stand on frosted glass" },
    { "glass.radius",                   CFG_OPT_DOUBLE, offsetof(FwmConfig, glass.radius),                    0.0,   128.0,    "px of blur behind a panel" },
    { "glass.fill",                     CFG_OPT_DOUBLE, offsetof(FwmConfig, glass.fill),                      0.02,    1.0,    "flat colour left on the glass; low is more frost" },
    { "glass.tint",                     CFG_OPT_DOUBLE, offsetof(FwmConfig, glass.tint),                      0.0,     1.0,    "how far the frost is pulled toward tint_color" },
    { "glass.tint_color",               CFG_OPT_COLOR,  offsetof(FwmConfig, glass.tint_color),                0.0,     0.0,    "what the frost is tinted with" },
    { "glass.brightness",               CFG_OPT_DOUBLE, offsetof(FwmConfig, glass.brightness),                0.0,     2.0,    "multiplier on the blurred desktop" },
    { "glass.shadow",                   CFG_OPT_INT,    offsetof(FwmConfig, glass.shadow),                    0.0,     1.0,    "1 = panels cast a shadow, from [sun]" },
    { "glass.shadow_length",            CFG_OPT_DOUBLE, offsetof(FwmConfig, glass.shadow_length),             0.0,   128.0,    "px a panel's shadow is cast" },
    { "glass.shadow_blur",              CFG_OPT_DOUBLE, offsetof(FwmConfig, glass.shadow_blur),               0.0,   128.0,    "penumbra under a panel, px" },
    { "glass.shadow_opacity",           CFG_OPT_DOUBLE, offsetof(FwmConfig, glass.shadow_opacity),            0.0,     1.0,    "how dark a panel's shadow is" },

    /* Numeric like cava.mode and physics.mass, and for the same reason — the
     * table is typed. 0 = manual, 1 = clock. */
    { "sun.enabled",                    CFG_OPT_INT,    offsetof(FwmConfig, sun.enabled),                     0.0,     1.0,    "1 = windows cast shadows" },
    { "sun.mode",                       CFG_OPT_INT,    offsetof(FwmConfig, sun.mode),                        0.0,     1.0,    "0 the sun is where you put it, 1 it follows the clock" },
    { "sun.azimuth",                    CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.azimuth),                  -360.0,   360.0,    "where the light comes from, deg clockwise from the top" },
    { "sun.elevation",                  CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.elevation),                 -90.0,    89.0,    "how high it is, deg; at or below 0 it is night" },
    { "sun.sunrise",                    CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.sunrise),                     0.0,    24.0,    "clock mode: when the light comes up" },
    { "sun.sunset",                     CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.sunset),                      0.0,    24.0,    "clock mode: when it goes down" },
    { "sun.dawn_azimuth",               CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.dawn_azimuth),             -360.0,   360.0,    "clock mode: azimuth at sunrise" },
    { "sun.dusk_azimuth",               CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.dusk_azimuth),             -360.0,   360.0,    "clock mode: azimuth at sunset" },
    { "sun.noon_elevation",             CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.noon_elevation),              0.0,    89.0,    "clock mode: how high it gets at midday" },
    { "sun.length",                     CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.length),                      0.0,  1000.0,    "shadow length at 45 deg, px" },
    { "sun.length_max",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.length_max),                  0.0,  1000.0,    "longest a shadow may get, px" },
    { "sun.opacity",                    CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.opacity),                     0.0,     1.0,    "how dark a shadow is" },
    { "sun.blur",                       CFG_OPT_DOUBLE, offsetof(FwmConfig, sun.blur),                        0.0,    64.0,    "penumbra, px; 0 = a hard-edged shadow" },
    { "sun.under_window",               CFG_OPT_INT,    offsetof(FwmConfig, sun.under_window),                0.0,     1.0,    "1 = draw the shadow under the window too" },
    { "sun.color",                      CFG_OPT_COLOR,  offsetof(FwmConfig, sun.color),                       0.0,     0.0,    "shadow colour" },

    { "effects.camera_shake",           CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.camera_shake),            0.0,     4.0,    "impact shake; 0 disables" },
    { "effects.squash",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.squash),                  0.0,     4.0,    "impact squash & stretch; 0 disables" },
    { "effects.jelly",                  CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.jelly),                   0.0,     4.0,    "drag wobble; 0 disables" },
    { "effects.droplet",                CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.droplet),                 0.0,     1.0,    "a window carried off a tiling layout is a drop; 0 disables" },
    { "effects.rubber",                 CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.rubber),                  0.0,     1.0,    "resize follows the hand, not the client's steps; 0 disables" },
    { "effects.spin",              CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.spin),                    0.0,     4.0,    "free rotation kick (experimental); 0 disables" },
    { "effects.live",              CFG_OPT_DOUBLE, offsetof(FwmConfig, effects.live),                    0.0,     1.0,    "live content under spin/wobble; 0 = still frame" },

    { "input.repeat_rate",              CFG_OPT_INT,    offsetof(FwmConfig, input.repeat_rate),               0.0,   200.0,    "key repeat, chars/s" },
    { "input.repeat_delay",             CFG_OPT_INT,    offsetof(FwmConfig, input.repeat_delay),              0.0,  5000.0,    "ms before repeat starts" },
    { "input.caps_hold_ms",             CFG_OPT_INT,    offsetof(FwmConfig, input.caps_hold_ms),              0.0,  5000.0,    "ms CapsLock must be held to lock; 0 = lock on the press" },

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

    /* The whole patch is regrown when any of these moves — the strip is a
     * picture, not a row of nodes, so there is nothing cheaper to do and
     * nothing that has to stay put. `color` is absent only because the table
     * carries numbers; it is a reload away. */
    { "grass.enabled",                  CFG_OPT_INT,    offsetof(FwmConfig, grass.enabled),                   0.0,     1.0,    "1 = grass along the bottom of the screen" },
    { "grass.height",                   CFG_OPT_DOUBLE, offsetof(FwmConfig, grass.height),                    4.0,  2000.0,    "px the tallest blades reach" },
    { "grass.density",                  CFG_OPT_DOUBLE, offsetof(FwmConfig, grass.density),                   1.0,   200.0,    "blades per 100px of width" },
    { "grass.width",                    CFG_OPT_DOUBLE, offsetof(FwmConfig, grass.width),                     0.5,    40.0,    "px across the base of a blade" },
    { "grass.opacity",                  CFG_OPT_DOUBLE, offsetof(FwmConfig, grass.opacity),                   0.0,     1.0,    "blade alpha over the wallpaper" },
    { "grass.wind",                     CFG_OPT_DOUBLE, offsetof(FwmConfig, grass.wind),                      0.0,     2.0,    "how far a gust bends a blade; 0 = still" },
    { "grass.wind_speed",               CFG_OPT_DOUBLE, offsetof(FwmConfig, grass.wind_speed),                0.0,  4000.0,    "px/s the gusts travel" },
    { "grass.fps",                      CFG_OPT_DOUBLE, offsetof(FwmConfig, grass.fps),                       5.0,   144.0,    "repaint ceiling while it sways" },

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

/* Parse one value without storing it. `cfg` may be NULL, and is where the
 * result goes when it is not — which is what makes checking and setting the
 * same code rather than two readings of the same rules that drift apart. */
static int option_apply(FwmConfig *cfg, const ConfigOption *opt,
                        const char *value, char *err, size_t errcap) {
    if (!value || !*value) {
        snprintf(err, errcap, "%s needs a value", opt->name);
        return 0;
    }
    char *field = cfg ? (char *)cfg + opt->offset : NULL;

    if (opt->type == CFG_OPT_COLOR) {
        float rgba[4];
        if (!parse_hex_color(value, rgba)) {
            snprintf(err, errcap, "%s: expected #RRGGBB or #RRGGBBAA, got \"%s\"",
                     opt->name, value);
            return 0;
        }
        if (field) memcpy(field, rgba, sizeof(rgba));
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

    if (!field) return 1;
    if (opt->type == CFG_OPT_INT) *(int *)field = (int)v;
    else                          *(double *)field = v;
    return 1;
}

int config_option_set(FwmConfig *cfg, const ConfigOption *opt,
                      const char *value, char *err, size_t errcap) {
    return option_apply(cfg, opt, value, err, errcap);
}

int config_option_check(const ConfigOption *opt, const char *value,
                        char *err, size_t errcap) {
    return option_apply(NULL, opt, value, err, errcap);
}

int config_option_number(const FwmConfig *cfg, const ConfigOption *opt, double *out) {
    if (!cfg || !opt || opt->type == CFG_OPT_COLOR) return 0;
    const char *field = (const char *)cfg + opt->offset;
    *out = opt->type == CFG_OPT_INT ? (double)*(const int *)field
                                    : *(const double *)field;
    return 1;
}

/* CLAMPED, unlike every other writer in this file, and deliberately: a dial is
 * turned until something looks right, and a knob that refused the last click
 * before the end of its range — the way the socket refuses an out-of-range
 * value — would be a knob that stops working near the edges. `hit_end` says
 * the range answered instead of the hand, which is what the OSD draws. */
int config_option_nudge(FwmConfig *cfg, const ConfigOption *opt, double delta,
                        int *hit_end, char *err, size_t errcap) {
    if (hit_end) *hit_end = 0;
    if (!opt) return 0;
    if (opt->type == CFG_OPT_COLOR) {
        snprintf(err, errcap, "%s is a colour — a step up or down means nothing to it",
                 opt->name);
        return 0;
    }

    double v;
    if (!config_option_number(cfg, opt, &v)) {
        snprintf(err, errcap, "%s cannot be read as a number", opt->name);
        return 0;
    }
    v += delta;
    if (v < opt->min) { v = opt->min; if (hit_end) *hit_end = 1; }
    if (v > opt->max) { v = opt->max; if (hit_end) *hit_end = 1; }

    char *field = (char *)cfg + opt->offset;
    /* Rounded, not truncated. An int option cannot hold a fraction and this
     * keeps no remainder between turns, so the whole of the rule is: a step of
     * half a unit or more moves it one, and anything smaller moves it not at
     * all. Truncating instead would have made every step under a whole unit do
     * nothing, which is a dial that ignores you rather than one with a
     * coarsest setting. */
    if (opt->type == CFG_OPT_INT) *(int *)field = (int)(v < 0.0 ? v - 0.5 : v + 0.5);
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

