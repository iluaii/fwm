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
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <math.h>

/* ── physics defaults (mirrors old defines.h) ────────────────────────── */

static const PhysicsConfig physics_defaults = {
    .friction               = 0.97,
    .mass_density           = 0.0005,
    .mass_mode              = PHYSICS_MASS_SIZE,
    .mass_ram_ref           = 300.0,
    .mass_ram_max           = 20.0,
    /* Off, and with the break speed left to follow max_throw_speed: a window
     * cannot then be destroyed by anything lighter than itself. */
    .hp                     = 0,
    .hp_break_speed         = 0.0,
    .throw_speed_multiplier = 0.65,
    .max_throw_speed        = 1800.0,
    .stop_speed_threshold   = 1.0,
    .restitution            = 0.75,
    .gravity                = 200.0,
    .tick_rate              = 60.0,
    /* Bars are scenery, not furniture: the world keeps the shape it always
     * had unless the config asks for the other one. */
    .solid_bars             = 0,
    /* No profiles, every desktop on the world's own values, and the gravity
     * ladder cycle_gravity has always climbed. Spelled out because this struct
     * is also assigned wholesale on the paths that never reach load_physics
     * (no config file, unreadable file, syntax error). */
    .profile_count      = 0,
    .desktop_profile    = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    .gravity_steps      = { 0.0, 0.15, 1.0 },
    .gravity_step_count = 3,
};

/* ── diagnostics ─────────────────────────────────────────────────────── */

/* Record a config problem. Never fatal: the caller carries on with defaults for
 * whatever it could not read, so a typo can never cost the user their session. */
void config_report_error(FwmConfig *cfg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[CONFIG_ERR_LEN];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "fwm config: %s\n", buf);
    cfg->error_total++;
    if (cfg->error_count < CONFIG_MAX_ERRORS) {
        snprintf(cfg->errors[cfg->error_count].msg, CONFIG_ERR_LEN, "%s", buf);
        cfg->error_count++;
    }
}

/* Actions understood by server_dispatch_action. Kept here so a typo in a bind
 * is reported at load time instead of silently doing nothing when pressed. */
int action_is_known(const char *a) {
    static const char *exact[] = {
        "killclient", "toggle_tiling", "toggle_split", "EXIT", "show_hints",
        "show_errors", "reload_config", "wallpaper_picker", "group_toggle", "group_next",
        "group_prev", "group_add", "cycle_gravity", "pin_window",
        "toggle_nocollide", "toggle_nocollide_all", "toggle_tiling_all",
        "toggle_floating", "toggle_floating_all",
        "calm_all", "fake_fullscreen", "real_fullscreen",
        "launcher", "toggle_tray", "spin_window", "spin_all", "terminal",
        "expo", "toggle_wrap", "modes_menu", "stats_menu",
        "screenshot", "screenshot_region",
        "output_off", "toggle_internal_output", "outputs_on", NULL
    };
    static const char *prefixes[] = {
        "spawn:", "view:", "move_camera:", "tile_focus:", "tile_move:",
        "move_to:", "move_to_view:", "global:", FWM_MODE_ACTION, NULL
    };
    for (int i = 0; exact[i]; i++)
        if (strcmp(a, exact[i]) == 0) return 1;
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(a, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    return 0;
}

/* ── mod key parsing ─────────────────────────────────────────────────── */

unsigned int parse_mod_token(const char *tok) {
    if (strcmp(tok, "super") == 0)  return FWM_MOD_LOGO;
    if (strcmp(tok, "alt")   == 0)  return FWM_MOD_ALT;
    if (strcmp(tok, "ctrl")  == 0)  return FWM_MOD_CTRL;
    if (strcmp(tok, "shift") == 0)  return FWM_MOD_SHIFT;
    return 0;
}

// Split string helper because strsep modifies the pointer and we want a clean implementation.
int parse_bind_key(const char *str, unsigned int *mod_out, xkb_keysym_t *key_out) {
    char buf[128];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    *mod_out = 0;
    *key_out = XKB_KEY_NoSymbol;

    char *tokens[8];
    int   n = 0;
    char *p = buf;
    char *tok;
    
    // Using standard strsep-like behavior or manual tokenization.
    // We can use strsep here since we make a local copy in buf.
    while ((tok = strsep(&p, "+")) != NULL && n < 8) {
        tokens[n++] = tok;
    }

    if (n == 0) return 0;

    for (int i = 0; i < n - 1; i++) {
        *mod_out |= parse_mod_token(tokens[i]);
    }

    // Convert keysym name to xkb_keysym_t.
    // If the key is e.g. "Return", xkb_keysym_from_name expects "Return".
    // Special key names in xkbcommon might be slightly different than X11 but mostly they align.
    // XStringToKeysym and xkb_keysym_from_name are highly compatible.
    // We need to map some common ones if they differ, but standard ones like "Return", "space", "q", "Escape" are identical.
    const char *keyname = tokens[n - 1];
    
    // In TOML, we had "Return", "space", "q", "t", "d", "f", "h", "l", "p", "n", "c", "g", "Escape", "question", etc.
    // X11 "Escape" -> xkbcommon "Escape".
    // X11 "Return" -> xkbcommon "Return".
    // X11 "space" -> xkbcommon "space".
    // X11 "question" -> xkbcommon "question".
    *key_out = xkb_keysym_from_name(keyname, XKB_KEYSYM_CASE_INSENSITIVE);
    if (*key_out == XKB_KEY_NoSymbol) {
        // Some fallback matches
        if (strcmp(keyname, "Return") == 0) *key_out = XKB_KEY_Return;
        else if (strcmp(keyname, "Escape") == 0) *key_out = XKB_KEY_Escape;
        else if (strcmp(keyname, "space") == 0) *key_out = XKB_KEY_space;
        else if (strcmp(keyname, "question") == 0) *key_out = XKB_KEY_question;
        else return 0; /* caller reports it with the full bind string */
    }
    return 1;
}

/* ── physics section ─────────────────────────────────────────────────── */

#define LOAD_DOUBLE(tbl, field, cfg_field) \
    do { \
        toml_datum_t _d = toml_double_in(tbl, field); \
        if (_d.ok) cfg_field = _d.u.d; \
    } while (0)

/* One [physics.<name>] profile: the world's values, with whatever the table
 * writes over the top, plus the desktops it claims. */
static void load_physics_profile(FwmConfig *cfg, toml_table_t *tbl, const char *name) {
    PhysicsConfig *p = &cfg->physics;
    if (p->profile_count >= CONFIG_MAX_PROFILES) {
        config_report_error(cfg, "[physics.%s]: too many profiles (max %d) — ignored",
                            name, CONFIG_MAX_PROFILES);
        return;
    }

    PhysicsProfileConfig *pr = &p->profiles[p->profile_count];
    snprintf(pr->name, sizeof(pr->name), "%s", name);
    pr->friction     = p->friction;
    pr->mass_density = p->mass_density;
    pr->restitution  = p->restitution;
    pr->gravity      = p->gravity;

    LOAD_DOUBLE(tbl, "friction",     pr->friction);
    LOAD_DOUBLE(tbl, "mass_density", pr->mass_density);
    LOAD_DOUBLE(tbl, "restitution",  pr->restitution);
    LOAD_DOUBLE(tbl, "gravity",      pr->gravity);

    /* A profile nothing points at is dead weight, and silence about it is the
     * kind of thing that costs an evening. */
    toml_array_t *desks = toml_array_in(tbl, "desktops");
    int claimed = 0;
    for (int i = 0; desks && i < toml_array_nelem(desks); i++) {
        toml_datum_t d = toml_int_at(desks, i);
        if (!d.ok) continue;
        if (d.u.i < 0 || d.u.i >= FWM_DESKTOPS) {
            config_report_error(cfg, "[physics.%s]: desktop %lld out of range 0..%d — ignored",
                                name, (long long)d.u.i, FWM_DESKTOPS - 1);
            continue;
        }
        p->desktop_profile[d.u.i] = p->profile_count;
        claimed++;
    }
    if (!claimed)
        config_report_error(cfg, "[physics.%s]: no desktops = [...] — profile unused", name);

    p->profile_count++;
}

static void load_physics(toml_table_t *root, FwmConfig *cfg) {
    PhysicsConfig *p = &cfg->physics;
    *p = physics_defaults;
    p->profile_count = 0;
    for (int i = 0; i < FWM_DESKTOPS; i++) p->desktop_profile[i] = -1;
    /* Zero-g, a lick of gravity, and earth — what cycle_gravity has always
     * walked, now merely the default list rather than the only one. */
    p->gravity_steps[0] = 0.0;
    p->gravity_steps[1] = 0.15;
    p->gravity_steps[2] = 1.0;
    p->gravity_step_count = 3;

    toml_table_t *tbl = toml_table_in(root, "physics");
    if (!tbl) return;

    LOAD_DOUBLE(tbl, "friction",               p->friction);
    LOAD_DOUBLE(tbl, "mass_density",           p->mass_density);
    LOAD_DOUBLE(tbl, "throw_speed_multiplier", p->throw_speed_multiplier);
    LOAD_DOUBLE(tbl, "max_throw_speed",        p->max_throw_speed);
    LOAD_DOUBLE(tbl, "stop_speed_threshold",   p->stop_speed_threshold);
    LOAD_DOUBLE(tbl, "restitution",            p->restitution);
    LOAD_DOUBLE(tbl, "gravity",                p->gravity);
    LOAD_DOUBLE(tbl, "tick_rate",              p->tick_rate);
    LOAD_DOUBLE(tbl, "mass_ram_ref",           p->mass_ram_ref);
    LOAD_DOUBLE(tbl, "mass_ram_max",           p->mass_ram_max);
    LOAD_DOUBLE(tbl, "hp_break_speed",         p->hp_break_speed);
    toml_datum_t sb = toml_bool_in(tbl, "solid_bars");
    if (sb.ok) p->solid_bars = sb.u.b ? 1 : 0;
    /* There is deliberately no "hp" key to go with it: breakable windows are
     * session state, turned on from the modes menu and never restored from
     * anywhere. See PhysicsConfig.hp. */

    toml_datum_t mm = toml_string_in(tbl, "mass");
    if (mm.ok) {
        if      (strcmp(mm.u.s, "size") == 0) p->mass_mode = PHYSICS_MASS_SIZE;
        else if (strcmp(mm.u.s, "area") == 0) p->mass_mode = PHYSICS_MASS_SIZE;
        else if (strcmp(mm.u.s, "ram")  == 0) p->mass_mode = PHYSICS_MASS_RAM;
        else if (strcmp(mm.u.s, "memory") == 0) p->mass_mode = PHYSICS_MASS_RAM;
        else config_report_error(cfg, "[physics] mass: unknown value \"%s\" (size | ram)",
                                 mm.u.s);
        free(mm.u.s);
    }

    /* A reference of zero divides every window's weight by nothing at all, and
     * a ceiling below 1 would make a memory hog LIGHTER than an idle one. */
    if (p->mass_ram_ref < 1.0)  p->mass_ram_ref = 1.0;
    if (p->mass_ram_max < 1.0)  p->mass_ram_max = 1.0;
    if (p->mass_ram_max > 200.0) p->mass_ram_max = 200.0;

    /* A break speed near zero divides by nothing and makes the gentlest touch
     * lethal, which is not a configuration anybody wants to arrive at by
     * typo. Zero and below keep their meaning ("use max_throw_speed"). */
    if (p->hp_break_speed > 0.0 && p->hp_break_speed < 50.0) {
        config_report_error(cfg, "[physics] hp_break_speed: %.0f is too low to survive — "
                                 "raised to 50", p->hp_break_speed);
        p->hp_break_speed = 50.0;
    }

    toml_array_t *steps = toml_array_in(tbl, "gravity_steps");
    if (steps) {
        int n = toml_array_nelem(steps);
        int count = 0;
        for (int i = 0; i < n && count < CONFIG_MAX_GRAVITY_STEPS; i++) {
            toml_datum_t d = toml_double_at(steps, i);
            if (!d.ok) {
                toml_datum_t di = toml_int_at(steps, i);   /* "1" is not "1.0" */
                if (!di.ok) continue;
                d.u.d = (double)di.u.i;
            }
            p->gravity_steps[count++] = d.u.d;
        }
        if (count > 0) p->gravity_step_count = count;
        else config_report_error(cfg, "[physics] gravity_steps: no usable numbers — "
                                      "keeping the built-in steps");
        if (n > CONFIG_MAX_GRAVITY_STEPS)
            config_report_error(cfg, "[physics] gravity_steps: only the first %d are used",
                                CONFIG_MAX_GRAVITY_STEPS);
    }

    /* Sub-tables of [physics] are profiles; the scalars above are not. Walked
     * after them so a profile inherits the world's final values. */
    for (int i = 0; ; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) break;
        toml_table_t *sub = toml_table_in(tbl, key);
        if (sub) load_physics_profile(cfg, sub, key);
    }
}

/* ── tiling section ──────────────────────────────────────────────────── */

static void load_tiling(toml_table_t *root, TilingConfig *t) {
    t->gaps_in    = 6;
    t->gaps_out   = 14;
    t->anim_speed = 12.0; /* ~250 ms glide */

    toml_table_t *tbl = toml_table_in(root, "tiling");
    if (!tbl) return;

    toml_datum_t d;
    d = toml_int_in(tbl, "gaps_in");
    if (d.ok) t->gaps_in = (int)d.u.i;
    d = toml_int_in(tbl, "gaps_out");
    if (d.ok) t->gaps_out = (int)d.u.i;
    LOAD_DOUBLE(tbl, "anim_speed", t->anim_speed);

    if (t->gaps_in < 0) t->gaps_in = 0;
    if (t->gaps_out < 0) t->gaps_out = 0;
}

/* ── camera section ──────────────────────────────────────────────────── */

static void load_camera(toml_table_t *root, CameraConfig *c) {
    c->anim_ms = 350.0;
    c->free_speed = 14.0;
    c->wrap = 0;

    toml_table_t *tbl = toml_table_in(root, "camera");
    if (!tbl) return;

    LOAD_DOUBLE(tbl, "anim_ms", c->anim_ms);
    LOAD_DOUBLE(tbl, "free_speed", c->free_speed);
    toml_datum_t w = toml_bool_in(tbl, "wrap");
    if (w.ok) c->wrap = w.u.b ? 1 : 0;
}

/* ── decor section ───────────────────────────────────────────────────── */

/* Parse "#RRGGBB" or "#RRGGBBAA" into RGBA floats. Returns 0 on bad input. */
int parse_hex_color(const char *s, float out[4]) {
    if (!s || s[0] != '#') return 0;
    size_t len = strlen(s + 1);
    if (len != 6 && len != 8) return 0;

    unsigned int v[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < len / 2; i++) {
        char buf[3] = { s[1 + i*2], s[2 + i*2], 0 };
        char *end;
        v[i] = (unsigned int)strtoul(buf, &end, 16);
        if (*end) return 0;
    }
    for (int i = 0; i < 4; i++) out[i] = v[i] / 255.0f;
    // wlr_scene_rect expects premultiplied alpha
    for (int i = 0; i < 3; i++) out[i] *= out[3];
    return 1;
}

static void load_decor(toml_table_t *root, FwmConfig *cfg) {
    DecorConfig *dc = &cfg->decor;
    dc->border_width = 2;
    parse_hex_color("#7aa2f7", dc->col_active);   /* soft blue */
    parse_hex_color("#3b4261", dc->col_inactive); /* muted slate */
    dc->fade_in_ms = 260.0;
    dc->wallpaper_fade_ms = 420.0;
    dc->tray_opacity = 0.92;
    dc->tray_yield = 0;
    dc->launcher_opacity = 0.92;
    dc->icon_theme[0] = '\0';
    dc->color_source = COLOR_SOURCE_CONFIG;
    dc->tint_strength = 0.4;

    toml_table_t *tbl = toml_table_in(root, "decor");
    if (!tbl) return;

    toml_datum_t d;
    d = toml_int_in(tbl, "border_width");
    if (d.ok) dc->border_width = (int)d.u.i;
    if (dc->border_width < 0) dc->border_width = 0;

    d = toml_string_in(tbl, "col_active");
    if (d.ok) {
        if (!parse_hex_color(d.u.s, dc->col_active))
            config_report_error(cfg, "[decor] col_active: \"%s\" is not #RRGGBB[AA]", d.u.s);
        free(d.u.s);
    }
    d = toml_string_in(tbl, "col_inactive");
    if (d.ok) {
        if (!parse_hex_color(d.u.s, dc->col_inactive))
            config_report_error(cfg, "[decor] col_inactive: \"%s\" is not #RRGGBB[AA]", d.u.s);
        free(d.u.s);
    }
    LOAD_DOUBLE(tbl, "fade_in_ms", dc->fade_in_ms);
    LOAD_DOUBLE(tbl, "wallpaper_fade_ms", dc->wallpaper_fade_ms);
    d = toml_string_in(tbl, "icon_theme");
    if (d.ok) { snprintf(dc->icon_theme, sizeof(dc->icon_theme), "%s", d.u.s); free(d.u.s); }
    d = toml_string_in(tbl, "color_source");
    if (d.ok) {
        if (strcmp(d.u.s, "wallpaper") == 0)   dc->color_source = COLOR_SOURCE_WALLPAPER;
        else if (strcmp(d.u.s, "config") == 0) dc->color_source = COLOR_SOURCE_CONFIG;
        else config_report_error(cfg, "[decor] color_source: unknown value \"%s\" "
                                      "(use \"config\" or \"wallpaper\")", d.u.s);
        free(d.u.s);
    }
    LOAD_DOUBLE(tbl, "tint_strength", dc->tint_strength);
    if (dc->tint_strength < 0.0) dc->tint_strength = 0.0;
    if (dc->tint_strength > 1.0) dc->tint_strength = 1.0;
    LOAD_DOUBLE(tbl, "tray_opacity", dc->tray_opacity);
    toml_datum_t ty = toml_bool_in(tbl, "tray_yield");
    if (ty.ok) dc->tray_yield = ty.u.b ? 1 : 0;
    LOAD_DOUBLE(tbl, "launcher_opacity", dc->launcher_opacity);
    if (dc->tray_opacity < 0.0) dc->tray_opacity = 0.0;
    if (dc->tray_opacity > 1.0) dc->tray_opacity = 1.0;
    if (dc->launcher_opacity < 0.0) dc->launcher_opacity = 0.0;
    if (dc->launcher_opacity > 1.0) dc->launcher_opacity = 1.0;
}

/* ── input section ───────────────────────────────────────────────────── */

/* A boolean the file may simply not mention. Leaves `*out` untouched then, so
 * the tri-state -1 (or tap's 1) survives. */
static void load_tristate(toml_table_t *tbl, const char *key, int *out) {
    toml_datum_t d = toml_bool_in(tbl, key);
    if (d.ok) *out = d.u.b ? 1 : 0;
}

/* One of a fixed set of words, copied verbatim so the applying code can map it
 * to libinput's enum — config.c has no business including libinput.h. */
static void load_enum(FwmConfig *cfg, toml_table_t *tbl, const char *key,
                      const char *const *allowed, char *out, size_t cap) {
    toml_datum_t d = toml_string_in(tbl, key);
    if (!d.ok) return;
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(d.u.s, allowed[i]) == 0) {
            snprintf(out, cap, "%s", d.u.s);
            free(d.u.s);
            return;
        }
    }
    /* Build the list for the message rather than repeating it at each call. */
    char list[128] = "";
    for (int i = 0; allowed[i]; i++) {
        if (i) strncat(list, ", ", sizeof(list) - strlen(list) - 1);
        strncat(list, allowed[i], sizeof(list) - strlen(list) - 1);
    }
    config_report_error(cfg, "[input] %s: unknown value \"%s\" (want %s) — ignored",
                        key, d.u.s, list);
    free(d.u.s);
}

static void load_input(toml_table_t *root, FwmConfig *cfg) {
    InputConfig *in = &cfg->input;
    in->kbd_layout[0]  = '\0';
    in->kbd_variant[0] = '\0';
    in->kbd_options[0] = '\0';
    in->repeat_rate  = 25;
    in->repeat_delay = 600;

    /* Everything libinput owns is left alone unless the file says otherwise —
     * except tap, which fwm turns on (see InputConfig). */
    in->tap              = 1;
    in->tap_drag         = -1;
    in->drag_lock        = -1;
    in->natural_scroll   = -1;
    in->dwt              = -1;
    in->middle_emulation = -1;
    in->left_handed      = -1;
    in->accel_speed      = INPUT_ACCEL_UNSET;
    in->accel_profile[0] = '\0';
    in->scroll_method[0] = '\0';
    in->click_method[0]  = '\0';

    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "input");
    if (!tbl) return;

    toml_datum_t d;
    d = toml_string_in(tbl, "kbd_layout");
    if (d.ok) { snprintf(in->kbd_layout, sizeof(in->kbd_layout), "%s", d.u.s); free(d.u.s); }
    d = toml_string_in(tbl, "kbd_variant");
    if (d.ok) { snprintf(in->kbd_variant, sizeof(in->kbd_variant), "%s", d.u.s); free(d.u.s); }
    d = toml_string_in(tbl, "kbd_options");
    if (d.ok) { snprintf(in->kbd_options, sizeof(in->kbd_options), "%s", d.u.s); free(d.u.s); }
    d = toml_int_in(tbl, "repeat_rate");
    if (d.ok && d.u.i > 0) in->repeat_rate = (int)d.u.i;
    d = toml_int_in(tbl, "repeat_delay");
    if (d.ok && d.u.i > 0) in->repeat_delay = (int)d.u.i;

    /* Touchpad. A device that cannot do one of these ignores it; see
     * pointer_apply_input_config. */
    load_tristate(tbl, "tap",              &in->tap);
    load_tristate(tbl, "tap_drag",         &in->tap_drag);
    load_tristate(tbl, "drag_lock",        &in->drag_lock);
    load_tristate(tbl, "natural_scroll",   &in->natural_scroll);
    load_tristate(tbl, "dwt",              &in->dwt);
    load_tristate(tbl, "middle_emulation", &in->middle_emulation);
    load_tristate(tbl, "left_handed",      &in->left_handed);

    d = toml_double_in(tbl, "accel_speed");
    if (d.ok) {
        if (d.u.d >= -1.0 && d.u.d <= 1.0) in->accel_speed = d.u.d;
        else config_report_error(cfg, "[input] accel_speed %.3f out of range "
                                 "-1..1 — ignored", d.u.d);
    }

    static const char *const profiles[] = { "adaptive", "flat", NULL };
    static const char *const scrolls[]  = { "two_finger", "edge", "button", "none", NULL };
    static const char *const clicks[]   = { "button_areas", "clickfinger", "none", NULL };
    load_enum(cfg, tbl, "accel_profile", profiles,
              in->accel_profile, sizeof(in->accel_profile));
    load_enum(cfg, tbl, "scroll_method", scrolls,
              in->scroll_method, sizeof(in->scroll_method));
    load_enum(cfg, tbl, "click_method", clicks,
              in->click_method, sizeof(in->click_method));
}

static void load_focus(toml_table_t *root, FocusConfig *f, FwmConfig *cfg) {
    /* Default keeps activation useful without ever yanking the view away from
     * what the user is looking at. */
    f->on_activate = FOCUS_ACTIVATE_SAME_DESKTOP;

    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "focus");
    if (!tbl) return;

    toml_datum_t d = toml_string_in(tbl, "on_activate");
    if (!d.ok) return;
    if      (strcmp(d.u.s, "never")        == 0) f->on_activate = FOCUS_ACTIVATE_NEVER;
    else if (strcmp(d.u.s, "same_desktop") == 0) f->on_activate = FOCUS_ACTIVATE_SAME_DESKTOP;
    else if (strcmp(d.u.s, "always")       == 0) f->on_activate = FOCUS_ACTIVATE_ALWAYS;
    else config_report_error(cfg, "[focus] on_activate: unknown value \"%s\" "
                                  "(never | same_desktop | always)", d.u.s);
    free(d.u.s);
}

static void load_effects(toml_table_t *root, EffectsConfig *e) {
    /* Shake is OFF by default: it moves the whole view on every hard landing,
     * which reads as intrusive during actual work. Opt in. */
    e->camera_shake = 0.0;
    e->squash = 1.0;
    e->jelly = 1.0;
    e->spin = 1.0;
    e->live = 1.0;
    e->shot_fly = 1.0;
    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "effects");
    if (!tbl) return;
    LOAD_DOUBLE(tbl, "camera_shake", e->camera_shake);
    if (e->camera_shake < 0.0) e->camera_shake = 0.0;
    if (e->camera_shake > 4.0) e->camera_shake = 4.0;  /* past this it is nausea */
    LOAD_DOUBLE(tbl, "squash", e->squash);
    if (e->squash < 0.0) e->squash = 0.0;
    if (e->squash > 2.0) e->squash = 2.0;
    LOAD_DOUBLE(tbl, "jelly", e->jelly);
    if (e->jelly < 0.0) e->jelly = 0.0;
    if (e->jelly > 2.0) e->jelly = 2.0;
    LOAD_DOUBLE(tbl, "live", e->live);
    if (e->live < 0.0) e->live = 0.0;
    if (e->live > 1.0) e->live = 1.0;
    LOAD_DOUBLE(tbl, "spin", e->spin);
    if (e->spin < 0.0) e->spin = 0.0;
    if (e->spin > 4.0) e->spin = 4.0;
    LOAD_DOUBLE(tbl, "shot_fly", e->shot_fly);
    if (e->shot_fly < 0.0) e->shot_fly = 0.0;
    if (e->shot_fly > 3.0) e->shot_fly = 3.0;
}

/* ── cava section ────────────────────────────────────────────────────── */

static void load_cava(toml_table_t *root, FwmConfig *cfg) {
    CavaConfig *c = &cfg->cava;

    /* Off by default. It opens an audio capture stream and puts bodies in the
     * physics world; neither is something a compositor should do because it
     * was merely installed. */
    c->mode        = CAVA_MODE_OFF;
    c->bars        = 48;
    c->height      = 160.0;
    c->gap         = 3.0;
    c->sensitivity = 1.0;
    c->smoothing   = 0.75;
    c->push        = 1.0;
    c->opacity     = 0.85;
    c->min_hz      = 40.0;
    c->max_hz      = 12000.0;

    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "cava");
    if (!tbl) return;

    toml_datum_t d = toml_string_in(tbl, "mode");
    if (d.ok) {
        if      (strcmp(d.u.s, "off")      == 0) c->mode = CAVA_MODE_OFF;
        else if (strcmp(d.u.s, "false")    == 0) c->mode = CAVA_MODE_OFF;
        else if (strcmp(d.u.s, "visual")   == 0) c->mode = CAVA_MODE_VISUAL;
        else if (strcmp(d.u.s, "physical") == 0) c->mode = CAVA_MODE_PHYSICAL;
        else if (strcmp(d.u.s, "both")     == 0) c->mode = CAVA_MODE_BOTH;
        else config_report_error(cfg, "[cava] mode: unknown value \"%s\" "
                                      "(off | visual | physical | both)", d.u.s);
        free(d.u.s);
    } else {
        /* `mode = false` is what a user writes when they mean "off", and TOML
         * hands it over as a bool rather than a string. Accept it instead of
         * reporting a type error for something perfectly clear. */
        toml_datum_t b = toml_bool_in(tbl, "mode");
        if (b.ok) c->mode = b.u.b ? CAVA_MODE_BOTH : CAVA_MODE_OFF;
    }

    toml_datum_t n = toml_int_in(tbl, "bars");
    if (n.ok) {
        if (n.u.i < 2 || n.u.i > CONFIG_MAX_BARS) {
            config_report_error(cfg, "[cava] bars: %lld out of range 2..%d — using %d",
                                (long long)n.u.i, CONFIG_MAX_BARS, c->bars);
        } else {
            c->bars = (int)n.u.i;
        }
    }

    LOAD_DOUBLE(tbl, "height",      c->height);
    LOAD_DOUBLE(tbl, "gap",         c->gap);
    LOAD_DOUBLE(tbl, "sensitivity", c->sensitivity);
    LOAD_DOUBLE(tbl, "smoothing",   c->smoothing);
    LOAD_DOUBLE(tbl, "push",        c->push);
    LOAD_DOUBLE(tbl, "opacity",     c->opacity);
    LOAD_DOUBLE(tbl, "min_hz",      c->min_hz);
    LOAD_DOUBLE(tbl, "max_hz",      c->max_hz);

    if (c->height < 8.0)   c->height = 8.0;
    if (c->gap < 0.0)      c->gap = 0.0;
    if (c->sensitivity < 0.0) c->sensitivity = 0.0;
    if (c->smoothing < 0.0) c->smoothing = 0.0;
    if (c->smoothing > 0.99) c->smoothing = 0.99;  /* 1.0 would never fall again */
    if (c->push < 0.0)     c->push = 0.0;
    if (c->push > 4.0)     c->push = 4.0;
    if (c->opacity < 0.0)  c->opacity = 0.0;
    if (c->opacity > 1.0)  c->opacity = 1.0;
    if (c->min_hz < 1.0)   c->min_hz = 1.0;
    /* An inverted or collapsed band range divides by zero when the log-spaced
     * edges are built, and a silent fallback beats a NaN row of bars. */
    if (c->max_hz <= c->min_hz * 1.1) {
        config_report_error(cfg, "[cava] max_hz must be well above min_hz — using 40..12000");
        c->min_hz = 40.0;
        c->max_hz = 12000.0;
    }
}

static void load_session(toml_table_t *root, SessionConfig *s, FwmConfig *cfg) {
    s->restore = SESSION_RESTORE_CRASH;
    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "session");
    if (!tbl) return;

    toml_datum_t d = toml_string_in(tbl, "restore");
    if (!d.ok) return;
    if      (strcmp(d.u.s, "crash")  == 0) s->restore = SESSION_RESTORE_CRASH;
    else if (strcmp(d.u.s, "always") == 0) s->restore = SESSION_RESTORE_ALWAYS;
    else if (strcmp(d.u.s, "never")  == 0) s->restore = SESSION_RESTORE_NEVER;
    else config_report_error(cfg, "[session] unknown restore \"%s\" — using \"crash\"", d.u.s);
    free(d.u.s);
}

/* ── public api ──────────────────────────────────────────────────────── */

/* Expand a leading "~/" — config paths are hand-written, and the shell that
 * would normally do this is not involved. */
static void expand_tilde(const char *in, char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0') && home)
        snprintf(out, cap, "%s%s", home, in + 1);
    else
        snprintf(out, cap, "%s", in);
}

static void load_wallpaper_picker(toml_table_t *root, FwmConfig *cfg) {
    expand_tilde("~/Pictures", cfg->wallpaper_dir, sizeof(cfg->wallpaper_dir));
    cfg->wallpaper_picker_fps = 0.0; /* 0 = clip's own rate */
    if (!root) return;

    toml_table_t *tbl = toml_table_in(root, "wallpaper_picker");
    if (!tbl) return;

    /* `dir` and `fps` are independent — parse both, so an fps with no dir (or a
     * dir with no fps) still takes effect. */
    toml_datum_t d = toml_string_in(tbl, "dir");
    if (d.ok) {
        expand_tilde(d.u.s, cfg->wallpaper_dir, sizeof(cfg->wallpaper_dir));
        free(d.u.s);
        if (access(cfg->wallpaper_dir, R_OK | X_OK) != 0)
            config_report_error(cfg, "[wallpaper_picker] dir: cannot read \"%s\"",
                                cfg->wallpaper_dir);
    }

    toml_datum_t f = toml_double_in(tbl, "fps");
    if (f.ok) cfg->wallpaper_picker_fps = f.u.d;
}

/* ── stats section ───────────────────────────────────────────────────── */

/* Names the compositor answers itself. Anything else in [stats] is a command. */
static int stats_is_builtin(const char *name) {
    return strcmp(name, "cpu") == 0 || strcmp(name, "ram") == 0 ||
           strcmp(name, "gpu") == 0;
}

static void load_stats(toml_table_t *root, FwmConfig *cfg) {
    StatsConfig *s = &cfg->stats;
    memset(s, 0, sizeof(*s));
    s->interval = 2.0;

    /* The default is the pill everybody would have written anyway. It is not
     * "nothing": an empty island in the tray teaches nobody that the feature
     * exists, and every one of these three costs a read of a file that is
     * already in memory. */
    snprintf(s->items[0], STATS_NAME_MAX, "cpu");
    snprintf(s->items[1], STATS_NAME_MAX, "ram");
    snprintf(s->items[2], STATS_NAME_MAX, "gpu");
    s->item_count = 3;

    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "stats");
    if (!tbl) return;

    LOAD_DOUBLE(tbl, "interval", s->interval);
    /* A command every tenth of a second is a fork bomb with a nice UI. The
     * floor is generous rather than tight: sensors are read, not watched. */
    if (s->interval < 0.5) s->interval = 0.5;

    /* Commands first, so `items` can be checked against them below whichever
     * order the two appear in the file. Every key that is not one of the
     * table's own settings, and not a built-in name, defines a sensor. */
    for (int i = 0; ; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) break;
        if (strcmp(key, "items") == 0 || strcmp(key, "interval") == 0) continue;

        toml_datum_t d = toml_string_in(tbl, key);
        if (!d.ok) continue;   /* not a string: not a command, so not ours */

        if (stats_is_builtin(key)) {
            config_report_error(cfg, "[stats] %s is a built-in sensor and cannot be "
                                     "given a command", key);
            free(d.u.s);
            continue;
        }
        if (strlen(key) >= STATS_NAME_MAX) {
            config_report_error(cfg, "[stats] sensor name \"%s\" is too long (max %d)",
                                key, STATS_NAME_MAX - 1);
            free(d.u.s);
            continue;
        }
        if (s->custom_count >= STATS_MAX_ITEMS) {
            config_report_error(cfg, "[stats] more than %d sensors — \"%s\" ignored",
                                STATS_MAX_ITEMS, key);
            free(d.u.s);
            continue;
        }
        StatsCustom *c = &s->custom[s->custom_count++];
        snprintf(c->name, sizeof(c->name), "%s", key);
        snprintf(c->cmd, sizeof(c->cmd), "%s", d.u.s);
        free(d.u.s);
    }

    toml_array_t *arr = toml_array_in(tbl, "items");
    if (!arr) return;

    s->item_count = 0;
    int n = toml_array_nelem(arr);
    for (int i = 0; i < n; i++) {
        toml_datum_t d = toml_string_at(arr, i);
        if (!d.ok) {
            config_report_error(cfg, "[stats] items: entry %d is not a name", i + 1);
            continue;
        }
        int known = stats_is_builtin(d.u.s);
        for (int c = 0; !known && c < s->custom_count; c++)
            known = strcmp(s->custom[c].name, d.u.s) == 0;

        if (!known) {
            /* Reported rather than dropped in silence: a misspelt sensor and a
             * sensor with nothing to say look identical in the tray. */
            config_report_error(cfg, "[stats] items: no sensor called \"%s\" — "
                                     "built-ins are cpu, ram, gpu; anything else "
                                     "needs a line of its own in [stats]", d.u.s);
        } else if (s->item_count >= STATS_MAX_ITEMS) {
            config_report_error(cfg, "[stats] items: more than %d shown — \"%s\" dropped",
                                STATS_MAX_ITEMS, d.u.s);
        } else {
            snprintf(s->items[s->item_count++], STATS_NAME_MAX, "%s", d.u.s);
        }
        free(d.u.s);
    }
}

/* ── sound section ───────────────────────────────────────────────────── */

static void load_sound(toml_table_t *root, FwmConfig *cfg) {
    SoundConfig *s = &cfg->sound;

    /* Off by default: a compositor that starts making noises because it was
     * installed is a compositor people uninstall. */
    s->collisions = 0;
    s->path[0]    = '\0';
    s->volume     = 0.6;
    s->min_speed  = 200.0;
    s->max_speed  = 2000.0;

    if (!root) return;
    toml_table_t *tbl = toml_table_in(root, "sound");
    if (!tbl) return;

    toml_datum_t b = toml_bool_in(tbl, "collisions");
    if (b.ok) s->collisions = b.u.b ? 1 : 0;

    toml_datum_t d = toml_string_in(tbl, "path");
    if (d.ok) {
        expand_tilde(d.u.s, s->path, sizeof(s->path));
        free(d.u.s);
        /* Reported here rather than at play time: the first collision is a bad
         * moment to find out, and the built-in click sounds enough like a
         * working feature to hide the typo completely. */
        if (s->path[0] && access(s->path, R_OK) != 0) {
            config_report_error(cfg, "[sound] path: cannot read \"%s\" — using the built-in click",
                                s->path);
            s->path[0] = '\0';
        }
    }

    LOAD_DOUBLE(tbl, "volume",    s->volume);
    LOAD_DOUBLE(tbl, "min_speed", s->min_speed);
    LOAD_DOUBLE(tbl, "max_speed", s->max_speed);

    if (s->volume < 0.0) s->volume = 0.0;
    if (s->volume > 1.0) s->volume = 1.0;
    if (s->min_speed < 0.0) s->min_speed = 0.0;
    /* Equal or inverted ends divide by zero (or by a negative) when a hit is
     * mapped onto them, and every collision would come out at one volume. */
    if (s->max_speed <= s->min_speed + 1.0) {
        config_report_error(cfg, "[sound] max_speed must be above min_speed — using %.0f..%.0f",
                            s->min_speed, s->min_speed + 1800.0);
        s->max_speed = s->min_speed + 1800.0;
    }
}

static void load_wallpaper(toml_table_t *root, FwmConfig *cfg) {
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;

    toml_array_t *arr = toml_array_in(root, "wallpaper");
    if (!arr) return;

    int n = toml_array_nelem(arr);
    if (n <= 0) return;

    cfg->wallpapers = calloc(n, sizeof(WallpaperLayer));
    if (!cfg->wallpapers) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        toml_table_t *tbl = toml_table_at(arr, i);
        if (!tbl) continue;

        toml_datum_t path = toml_string_in(tbl, "path");
        if (!path.ok) {
            config_report_error(cfg, "[[wallpaper]] #%d: missing or unquoted \"path\"", i + 1);
            continue;
        }
        if (access(path.u.s, R_OK) != 0) {
            config_report_error(cfg, "[[wallpaper]] #%d: cannot read \"%s\"", i + 1, path.u.s);
            free(path.u.s);
            continue;
        }

        strncpy(cfg->wallpapers[idx].path, path.u.s, sizeof(cfg->wallpapers[idx].path) - 1);
        free(path.u.s);

        toml_datum_t fit = toml_string_in(tbl, "fit");
        int mode = WALLPAPER_FIT_COVER;
        if (fit.ok) {
            if (strcmp(fit.u.s, "contain") == 0)   mode = WALLPAPER_FIT_CONTAIN;
            else if (strcmp(fit.u.s, "pan") == 0)  mode = WALLPAPER_FIT_PAN;
            else if (strcmp(fit.u.s, "video") == 0) mode = WALLPAPER_FIT_VIDEO;
            else if (strcmp(fit.u.s, "cover") != 0)
                config_report_error(cfg, "[[wallpaper]] #%d: unknown fit \"%s\" — using cover",
                        i + 1, fit.u.s);
            free(fit.u.s);
        }
        cfg->wallpapers[idx].fit = mode;

        toml_datum_t crop = toml_double_in(tbl, "pan_crop");
        double pc = crop.ok ? crop.u.d : 0.0;
        if (pc < 0.0) pc = 0.0;
        if (pc > 0.9) pc = 0.9;   /* past this nothing recognisable is left */
        cfg->wallpapers[idx].pan_crop = pc;

        toml_datum_t zoom = toml_double_in(tbl, "zoom");
        cfg->wallpapers[idx].zoom = zoom.ok ? zoom.u.d : 0.0; /* 0 = auto (native) */

        toml_datum_t fps = toml_double_in(tbl, "fps");
        cfg->wallpapers[idx].fps = fps.ok ? fps.u.d : 0.0; /* 0 = source rate */

        idx++;
    }
    cfg->wallpaper_count = idx;
}

/* ── window rules ────────────────────────────────────────────────────── */

/* Compile one matcher. Returns 1 if the pattern was present AND valid; a
 * broken regex is reported and the rule simply loses that matcher — but a rule
 * left with NO matchers is dropped entirely by the caller, because a matcher-
 * less rule would silently apply to every window. */
static int compile_matcher(FwmConfig *cfg, toml_table_t *tbl, const char *key,
                           int index, regex_t *re, char *pat, size_t patcap,
                           int *present) {
    toml_datum_t d = toml_string_in(tbl, key);
    if (!d.ok) return 0;
    *present = 1;

    int rc = regcomp(re, d.u.s, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char reason[128];
        regerror(rc, re, reason, sizeof(reason));
        config_report_error(cfg, "[[rule]] #%d: bad %s regex \"%s\" — %s",
                            index + 1, key, d.u.s, reason);
        free(d.u.s);
        return 0;
    }
    snprintf(pat, patcap, "%s", d.u.s);
    free(d.u.s);
    return 1;
}

/* Reads an optional boolean property into a tri-state: -1 keeps "unset", so a
 * later rule can leave an earlier rule's decision standing. */
static int rule_tristate(toml_table_t *tbl, const char *key) {
    toml_datum_t d = toml_bool_in(tbl, key);
    return d.ok ? (d.u.b ? 1 : 0) : -1;
}

/* The same idea for a number, where -1 is a value someone may well mean
 * (gravity = -1 is a window that falls upward). NAN is the "unset" state, and
 * an integer in the file is accepted as the double it obviously is — writing
 * `mass = 8` and getting silence would be a mean trick. */
static double rule_number(FwmConfig *cfg, toml_table_t *tbl, const char *key,
                          int index, double min, double max) {
    toml_datum_t d = toml_double_in(tbl, key);
    if (!d.ok) {
        toml_datum_t di = toml_int_in(tbl, key);
        if (!di.ok) return NAN;
        d.u.d = (double)di.u.i;
    }
    if (d.u.d < min || d.u.d > max) {
        config_report_error(cfg, "[[rule]] #%d: %s %g out of range %g..%g — ignored",
                            index + 1, key, d.u.d, min, max);
        return NAN;
    }
    return d.u.d;
}

/* ── mode and transform spellings ─────────────────────────────────────────
 *
 * Shared with `fwmctl output`, which is why they are public: the file and the
 * socket must accept exactly the same strings, or a mode that works in
 * config.toml would be rejected at runtime for no reason a user could see. */

bool config_parse_mode(const char *s, int *w, int *h, int *refresh_mhz) {
    if (!s) return false;

    char *end;
    long width = strtol(s, &end, 10);
    if (end == s || (*end != 'x' && *end != 'X')) return false;
    const char *p = end + 1;
    long height = strtol(p, &end, 10);
    if (end == p) return false;

    double hz = 0.0;
    if (*end == '@') {
        p = end + 1;
        hz = strtod(p, &end);
        if (end == p || hz <= 0.0 || hz > 1000.0) return false;
    }
    while (*end == ' ') end++;
    if (*end) return false;      /* trailing junk: a typo, not a mode */

    /* A monitor narrower than 64px or wider than 16K is a typo every time, and
     * the cap keeps a bad number away from the backend's own arithmetic. */
    if (width < 64 || width > 16384 || height < 64 || height > 16384) return false;

    if (w) *w = (int)width;
    if (h) *h = (int)height;
    /* +0.5 so 59.94 lands on 59940 rather than 59939. */
    if (refresh_mhz) *refresh_mhz = (int)(hz * 1000.0 + 0.5);
    return true;
}

/* Index is the wl_output_transform value; the names are wlr-randr's. */
static const char *const transform_names[8] = {
    "normal", "90", "180", "270",
    "flipped", "flipped-90", "flipped-180", "flipped-270",
};

int config_parse_transform(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "0") == 0) return 0;    /* "0" is how people write "normal" */
    for (int i = 0; i < 8; i++)
        if (strcmp(s, transform_names[i]) == 0) return i;
    return -1;
}

const char *config_transform_name(int transform) {
    return (transform >= 0 && transform < 8) ? transform_names[transform] : "?";
}

/* [[output]]: where each monitor sits and what it starts on. Everything is
 * optional except the name — an entry that names nothing cannot be matched to
 * a monitor, so it is reported rather than silently ignored. */
static void load_outputs(toml_table_t *root, FwmConfig *cfg) {
    cfg->output_count = 0;

    toml_array_t *arr = toml_array_in(root, "output");
    if (!arr) return;

    int n = toml_array_nelem(arr);
    if (n <= 0) return;
    if (n > CONFIG_MAX_OUTPUTS) {
        config_report_error(cfg, "too many [[output]] entries (%d) — only the first %d are used",
                            n, CONFIG_MAX_OUTPUTS);
        n = CONFIG_MAX_OUTPUTS;
    }

    for (int i = 0; i < n; i++) {
        toml_table_t *tbl = toml_table_at(arr, i);
        if (!tbl) continue;

        ConfigOutput *o = &cfg->outputs[cfg->output_count];
        memset(o, 0, sizeof(*o));
        o->desktop = -1;
        o->enabled = 1;
        o->transform = -1;

        toml_datum_t name = toml_string_in(tbl, "name");
        if (!name.ok) {
            config_report_error(cfg, "[[output]] #%d: no name — entry ignored", i + 1);
            continue;
        }
        snprintf(o->name, sizeof(o->name), "%s", name.u.s);
        free(name.u.s);

        toml_datum_t x = toml_int_in(tbl, "x");
        toml_datum_t y = toml_int_in(tbl, "y");
        if (x.ok != y.ok) {
            config_report_error(cfg, "[[output]] %s: x and y must be given together — position ignored",
                                o->name);
        } else if (x.ok) {
            /* Negative is legal: a monitor may sit left of the origin. The cap
             * only keeps a typo from putting a screen a million px away, where
             * nothing would ever be drawn on it. */
            if (x.u.i < -32768 || x.u.i > 32768 || y.u.i < -32768 || y.u.i > 32768) {
                config_report_error(cfg, "[[output]] %s: position %lld,%lld out of range — ignored",
                                    o->name, (long long)x.u.i, (long long)y.u.i);
            } else {
                o->have_pos = 1;
                o->x = (int)x.u.i;
                o->y = (int)y.u.i;
            }
        }

        toml_datum_t desk = toml_int_in(tbl, "desktop");
        if (desk.ok) {
            if (desk.u.i < 0 || desk.u.i >= 10)
                config_report_error(cfg, "[[output]] %s: desktop %lld out of range 0..9 — ignored",
                                    o->name, (long long)desk.u.i);
            else
                o->desktop = (int)desk.u.i;
        }

        toml_datum_t en = toml_bool_in(tbl, "enabled");
        if (en.ok) o->enabled = en.u.b ? 1 : 0;

        /* A mode the monitor cannot do is not knowable here — no monitor has
         * been seen yet — so only the spelling is checked; the compositor
         * reports one the hardware rejects when it tries to apply it. */
        toml_datum_t mode = toml_string_in(tbl, "mode");
        if (mode.ok) {
            if (config_parse_mode(mode.u.s, &o->mode_w, &o->mode_h, &o->mode_refresh))
                o->have_mode = 1;
            else
                config_report_error(cfg, "[[output]] %s: mode \"%s\" is not WIDTHxHEIGHT[@HZ] — ignored",
                                    o->name, mode.u.s);
            free(mode.u.s);
        }

        /* `scale = 2` is TOML for an integer, and toml_double_in will not take
         * it — the one place in this file where a whole number is the value
         * people reach for first, so it is read either way. */
        toml_datum_t scale = toml_double_in(tbl, "scale");
        if (!scale.ok) {
            toml_datum_t si = toml_int_in(tbl, "scale");
            if (si.ok) { scale.ok = 1; scale.u.d = (double)si.u.i; }
        }
        if (scale.ok) {
            /* wlroots refuses a scale of 0 or below with an abort, and past
             * ~10 a desktop holds a single button. */
            if (scale.u.d < 0.25 || scale.u.d > 10.0)
                config_report_error(cfg, "[[output]] %s: scale %g out of range 0.25..10 — ignored",
                                    o->name, scale.u.d);
            else
                o->scale = scale.u.d;
        }

        toml_datum_t tr = toml_string_in(tbl, "transform");
        if (tr.ok) {
            o->transform = config_parse_transform(tr.u.s);
            if (o->transform < 0)
                config_report_error(cfg, "[[output]] %s: transform \"%s\" is not one of "
                                         "normal, 90, 180, 270, flipped, flipped-90, "
                                         "flipped-180, flipped-270 — ignored",
                                    o->name, tr.u.s);
            free(tr.u.s);
        }

        cfg->output_count++;
    }
}

const ConfigOutput *config_find_output(const FwmConfig *cfg, const char *name) {
    if (!cfg || !name) return NULL;
    for (int i = 0; i < cfg->output_count; i++) {
        if (strcmp(cfg->outputs[i].name, name) == 0) return &cfg->outputs[i];
    }
    return NULL;
}

static void load_rules(toml_table_t *root, FwmConfig *cfg) {
    cfg->rules      = NULL;
    cfg->rule_count = 0;

    toml_array_t *arr = toml_array_in(root, "rule");
    if (!arr) return;

    int n = toml_array_nelem(arr);
    if (n <= 0) return;
    if (n > CONFIG_MAX_RULES) {
        config_report_error(cfg, "too many [[rule]] entries (%d) — only the first %d are used",
                            n, CONFIG_MAX_RULES);
        n = CONFIG_MAX_RULES;
    }

    cfg->rules = calloc(n, sizeof(ConfigRule));
    if (!cfg->rules) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        toml_table_t *tbl = toml_table_at(arr, i);
        if (!tbl) continue;

        ConfigRule *r = &cfg->rules[idx];
        int present = 0;
        r->has_app_id = compile_matcher(cfg, tbl, "app_id", i, &r->re_app_id,
                                        r->pat_app_id, sizeof(r->pat_app_id), &present);
        r->has_title  = compile_matcher(cfg, tbl, "title", i, &r->re_title,
                                        r->pat_title, sizeof(r->pat_title), &present);

        if (!r->has_app_id && !r->has_title) {
            /* Only complain about the absence when the user never wrote a
             * matcher. If one was written but would not compile, the regex
             * error above is the actionable message — saying it twice would
             * spend two of the tray pill's slots on a single typo. */
            if (!present)
                config_report_error(cfg, "[[rule]] #%d: no app_id/title matcher — rule ignored",
                                    i + 1);
            continue;   /* nothing compiled, so nothing to regfree */
        }

        r->nocollide = rule_tristate(tbl, "nocollide");
        r->pin       = rule_tristate(tbl, "pin");

        r->desktop = -1;
        toml_datum_t desk = toml_int_in(tbl, "desktop");
        if (desk.ok) {
            if (desk.u.i < 0 || desk.u.i > 9)
                config_report_error(cfg, "[[rule]] #%d: desktop %lld out of range 0..9 — ignored",
                                    i + 1, (long long)desk.u.i);
            else
                r->desktop = (int)desk.u.i;
        }

        /* Material. Ranges are wide on purpose — a window 100x heavier than
         * normal or one that falls upward is a thing someone may genuinely
         * want — and only bar the values that would break the simulation
         * outright (a massless body, restitution over 1 gaining energy on
         * every bounce, retention over 1 accelerating for free). */
        r->mass     = rule_number(cfg, tbl, "mass",     i, 0.001, 1000.0);
        r->gravity  = rule_number(cfg, tbl, "gravity",  i, -100.0, 100.0);
        r->bounce   = rule_number(cfg, tbl, "bounce",   i, 0.0, 1.0);
        r->friction = rule_number(cfg, tbl, "friction", i, 0.0, 1.0);
        /* Toughness may be 0 — a window made of glass, destroyed by any hit at
         * all — so the floor is 0 and not the 0.001 mass needs. */
        r->toughness = rule_number(cfg, tbl, "toughness", i, 0.0, 1000.0);
        r->hardness  = rule_number(cfg, tbl, "hardness",  i, 0.0, 1000.0);

        if (r->nocollide < 0 && r->pin < 0 && r->desktop < 0 &&
            isnan(r->mass) && isnan(r->gravity) && isnan(r->bounce) &&
            isnan(r->friction) && isnan(r->toughness) && isnan(r->hardness))
            config_report_error(cfg, "[[rule]] #%d: matches but sets nothing", i + 1);

        idx++;
    }
    cfg->rule_count = idx;
}

int config_match_rules(const FwmConfig *cfg, const char *app_id, const char *title,
                       ConfigRule *out) {
    out->nocollide = -1;
    out->pin       = -1;
    out->desktop   = -1;
    out->mass = out->gravity = out->bounce = out->friction = NAN;
    out->toughness = out->hardness = NAN;

    int matched = 0;
    for (int i = 0; i < cfg->rule_count; i++) {
        const ConfigRule *r = &cfg->rules[i];

        /* Every matcher the rule declares has to hit. A window with no app_id
         * (some X11 clients) can never satisfy an app_id matcher. */
        if (r->has_app_id) {
            if (!app_id || regexec(&r->re_app_id, app_id, 0, NULL, 0) != 0) continue;
        }
        if (r->has_title) {
            if (!title || regexec(&r->re_title, title, 0, NULL, 0) != 0) continue;
        }

        /* Later rules override earlier ones field by field. */
        if (r->nocollide >= 0) out->nocollide = r->nocollide;
        if (r->pin       >= 0) out->pin       = r->pin;
        if (r->desktop   >= 0) out->desktop   = r->desktop;
        if (!isnan(r->mass))     out->mass     = r->mass;
        if (!isnan(r->gravity))  out->gravity  = r->gravity;
        if (!isnan(r->bounce))   out->bounce   = r->bounce;
        if (!isnan(r->friction)) out->friction = r->friction;
        if (!isnan(r->toughness)) out->toughness = r->toughness;
        if (!isnan(r->hardness))  out->hardness  = r->hardness;
        matched = 1;
    }
    return matched;
}

void config_load(FwmConfig *cfg, const char *path) {
    cfg->physics         = physics_defaults;
    cfg->tiling          = (TilingConfig){ .gaps_in = 6, .gaps_out = 14, .anim_speed = 12.0 };
    cfg->camera          = (CameraConfig){ .anim_ms = 350.0, .free_speed = 14.0 };
    // Defaults for the no-config-file path; load_decor re-applies them anyway.
    cfg->decor.border_width = 2;
    parse_hex_color("#7aa2f7", cfg->decor.col_active);
    parse_hex_color("#3b4261", cfg->decor.col_inactive);
    cfg->decor.fade_in_ms = 260.0;
    cfg->decor.wallpaper_fade_ms = 420.0;
    cfg->decor.tray_opacity = 0.92;
    cfg->decor.tray_yield = 0;
    cfg->decor.launcher_opacity = 0.92;
    cfg->decor.icon_theme[0] = '\0';
    cfg->decor.color_source = COLOR_SOURCE_CONFIG;
    cfg->decor.tint_strength = 0.4;
    cfg->keys            = NULL;
    cfg->key_count       = 0;
    cfg->mode_count      = 0;
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;
    cfg->rules           = NULL;
    cfg->rule_count      = 0;
    cfg->error_count     = 0;
    cfg->error_total     = 0;
    cfg->fallback_binds  = 0;
    snprintf(cfg->source, sizeof(cfg->source), "%s", path ? path : "");
    load_input(NULL, cfg); /* defaults for the no-config-file path */
    load_focus(NULL, &cfg->focus, cfg);
    load_effects(NULL, &cfg->effects);
    load_session(NULL, &cfg->session, cfg);
    load_gestures(NULL, cfg);
    load_cava(NULL, cfg);
    load_sound(NULL, cfg);
    load_stats(NULL, cfg);
    load_mouse(NULL, cfg);   /* the built-in drag verbs, for every early-out below */

    FILE *f = fopen(path, "r");
    if (!f) {
        config_report_error(cfg, "cannot open %s — using defaults", path);
        apply_default_binds(cfg);
        load_wallpaper_picker(NULL, cfg);
        return;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);

    /* A syntax error used to abandon the whole load, leaving zero binds — a
     * running compositor the user could not control. Now the defaults stand in
     * and the tray reports the error. */
    if (!root) {
        config_report_error(cfg, "syntax error: %s", errbuf);
        config_report_error(cfg, "config ignored — using defaults and built-in keybindings");
        apply_default_binds(cfg);
        load_wallpaper_picker(NULL, cfg);
        return;
    }

    load_physics(root, cfg);
    load_tiling(root, &cfg->tiling);
    load_camera(root, &cfg->camera);
    load_decor(root, cfg);
    load_input(root, cfg);
    load_focus(root, &cfg->focus, cfg);
    load_effects(root, &cfg->effects);
    load_session(root, &cfg->session, cfg);
    load_binds(root, cfg);
    load_modes(root, cfg);   /* after [binds]: each mode's `enter` key joins the root map */
    load_mouse(root, cfg);
    load_gestures(root, cfg);
    load_cava(root, cfg);
    load_sound(root, cfg);
    load_stats(root, cfg);
    load_wallpaper(root, cfg);
    load_wallpaper_picker(root, cfg);
    load_rules(root, cfg);
    load_outputs(root, cfg);

    toml_free(root);
}

void config_free(FwmConfig *cfg) {
    free(cfg->keys);
    cfg->keys      = NULL;
    cfg->key_count = 0;
    for (int i = 0; i < cfg->mode_count; i++) free(cfg->modes[i].keys);
    cfg->mode_count = 0;
    free(cfg->wallpapers);
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;
    /* Compiled regexes own heap memory of their own: without regfree every
     * hot reload (super+shift+r) would leak one allocation per matcher. */
    for (int i = 0; i < cfg->rule_count; i++) {
        if (cfg->rules[i].has_app_id) regfree(&cfg->rules[i].re_app_id);
        if (cfg->rules[i].has_title)  regfree(&cfg->rules[i].re_title);
    }
    free(cfg->rules);
    cfg->rules      = NULL;
    cfg->rule_count = 0;
}

/* ── bind lookup ─────────────────────────────────────────────────────── */

const KeyBind *config_match_bind(const FwmConfig *cfg, xkb_keysym_t sym, unsigned int mods) {
    if (!cfg) return NULL;
    /* Compare case-insensitively: xkb_state_key_get_syms() reflects the live
     * CapsLock state, so with Caps Lock on a letter key resolves to its
     * uppercase keysym ('q' -> 'Q') while binds are parsed from lowercase
     * config strings. Without normalising, every letter bind silently stops
     * working the moment Caps Lock is on, while digit and Return binds — which
     * Caps Lock does not touch — keep working, which is a maddening way to
     * find out. */
    xkb_keysym_t want = xkb_keysym_to_lower(sym);
    for (int i = 0; i < cfg->key_count; i++) {
        const KeyBind *bind = &cfg->keys[i];
        if (xkb_keysym_to_lower(bind->key) == want && bind->mod == mods)
            return bind;
    }
    return NULL;
}

int config_action_is_repeatable(const char *action) {
    if (!action) return 0;
    return strncmp(action, "move_camera:", 12) == 0;
}

