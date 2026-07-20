#include "config.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

/* ── physics defaults (mirrors old defines.h) ────────────────────────── */

static const PhysicsConfig physics_defaults = {
    .friction               = 0.97,
    .mass_density           = 0.0005,
    .throw_speed_multiplier = 0.65,
    .max_throw_speed        = 1800.0,
    .stop_speed_threshold   = 1.0,
    .restitution            = 0.75,
    .gravity                = 200.0,
    .tick_rate              = 60.0,
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
static int action_is_known(const char *a) {
    static const char *exact[] = {
        "killclient", "toggle_tiling", "toggle_split", "EXIT", "show_hints",
        "show_errors", "reload_config", "wallpaper_picker", "group_toggle", "group_next",
        "group_prev", "group_add", "cycle_gravity", "pin_window",
        "toggle_nocollide", "calm_all", "fake_fullscreen", "real_fullscreen",
        "launcher", NULL
    };
    static const char *prefixes[] = {
        "spawn:", "view:", "move_camera:", "tile_focus:", "tile_move:", NULL
    };
    for (int i = 0; exact[i]; i++)
        if (strcmp(a, exact[i]) == 0) return 1;
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(a, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    return 0;
}

/* ── mod key parsing ─────────────────────────────────────────────────── */

static unsigned int parse_mod_token(const char *tok) {
    if (strcmp(tok, "super") == 0)  return FWM_MOD_LOGO;
    if (strcmp(tok, "alt")   == 0)  return FWM_MOD_ALT;
    if (strcmp(tok, "ctrl")  == 0)  return FWM_MOD_CTRL;
    if (strcmp(tok, "shift") == 0)  return FWM_MOD_SHIFT;
    return 0;
}

// Split string helper because strsep modifies the pointer and we want a clean implementation.
static int parse_bind_key(const char *str, unsigned int *mod_out, xkb_keysym_t *key_out) {
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

static void load_physics(toml_table_t *root, PhysicsConfig *p) {
    *p = physics_defaults;

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
}

/* ── tiling section ──────────────────────────────────────────────────── */

static void load_tiling(toml_table_t *root, TilingConfig *t) {
    t->gaps_in    = 6;
    t->gaps_out   = 12;
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

    toml_table_t *tbl = toml_table_in(root, "camera");
    if (!tbl) return;

    LOAD_DOUBLE(tbl, "anim_ms", c->anim_ms);
    LOAD_DOUBLE(tbl, "free_speed", c->free_speed);
}

/* ── decor section ───────────────────────────────────────────────────── */

/* Parse "#RRGGBB" or "#RRGGBBAA" into RGBA floats. Returns 0 on bad input. */
static int parse_hex_color(const char *s, float out[4]) {
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
    LOAD_DOUBLE(tbl, "launcher_opacity", dc->launcher_opacity);
    if (dc->tray_opacity < 0.0) dc->tray_opacity = 0.0;
    if (dc->tray_opacity > 1.0) dc->tray_opacity = 1.0;
    if (dc->launcher_opacity < 0.0) dc->launcher_opacity = 0.0;
    if (dc->launcher_opacity > 1.0) dc->launcher_opacity = 1.0;
}

/* ── input section ───────────────────────────────────────────────────── */

static void load_input(toml_table_t *root, InputConfig *in) {
    in->kbd_layout[0]  = '\0';
    in->kbd_variant[0] = '\0';
    in->kbd_options[0] = '\0';
    in->repeat_rate  = 25;
    in->repeat_delay = 600;

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

/* ── binds section ───────────────────────────────────────────────────── */

/* Built-in binds, installed whenever the config file yielded no usable ones
 * (missing file, TOML syntax error, empty or entirely broken [binds]). Without
 * them a single typo — a forgotten quote — leaves a running compositor that
 * cannot spawn a terminal, switch desktops or exit. Mirrors the defaults in
 * config.toml.example; keep the two in sync. */
static const struct { const char *bind; const char *action; } default_binds[] = {
    { "super+Return",         "spawn:kitty"      },
    { "super+space",          "launcher"         },
    { "super+q",              "killclient"       },
    { "super+t",              "toggle_tiling"    },
    { "super+d",              "fake_fullscreen"  },
    { "super+f",              "real_fullscreen"  },
    { "super+h",              "move_camera:-50"  },
    { "super+l",              "move_camera:50"   },
    { "super+p",              "pin_window"       },
    { "super+n",              "toggle_nocollide" },
    { "super+g",              "cycle_gravity"    },
    { "super+s",              "toggle_split"     },
    { "super+w",              "group_toggle"     },
    { "super+Tab",            "group_next"       },
    { "super+shift+Tab",      "group_prev"       },
    { "super+shift+w",        "group_add"        },
    { "super+shift+c",        "calm_all"         },
    { "super+shift+r",        "reload_config"    },
    { "super+shift+p",        "wallpaper_picker" },
    { "super+shift+question", "show_hints"       },
    { "super+shift+Escape",   "EXIT"             },
    { "super+Left",           "tile_focus:l"     },
    { "super+Right",          "tile_focus:r"     },
    { "super+Up",             "tile_focus:u"     },
    { "super+Down",           "tile_focus:d"     },
    { "super+shift+Left",     "tile_move:l"      },
    { "super+shift+Right",    "tile_move:r"      },
    { "super+shift+Up",       "tile_move:u"      },
    { "super+shift+Down",     "tile_move:d"      },
    { "super+1",              "view:0"           },
    { "super+2",              "view:1"           },
    { "super+3",              "view:2"           },
    { "super+4",              "view:3"           },
    { "super+5",              "view:4"           },
    { "super+6",              "view:5"           },
    { "super+7",              "view:6"           },
    { "super+8",              "view:7"           },
    { "super+9",              "view:8"           },
    { "super+0",              "view:9"           },
};

static void apply_default_binds(FwmConfig *cfg) {
    int n = (int)(sizeof(default_binds) / sizeof(default_binds[0]));
    free(cfg->keys);
    cfg->keys = calloc(n, sizeof(KeyBind));
    cfg->key_count = 0;
    if (!cfg->keys) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        unsigned int mod;
        xkb_keysym_t key;
        if (!parse_bind_key(default_binds[i].bind, &mod, &key)) continue;
        cfg->keys[idx].mod = mod;
        cfg->keys[idx].key = key;
        snprintf(cfg->keys[idx].action, sizeof(cfg->keys[idx].action), "%s",
                 default_binds[i].action);
        idx++;
    }
    cfg->key_count     = idx;
    cfg->fallback_binds = 1;
}

static void load_binds(toml_table_t *root, FwmConfig *cfg) {
    cfg->keys      = NULL;
    cfg->key_count = 0;

    toml_table_t *tbl = toml_table_in(root, "binds");
    if (!tbl) {
        config_report_error(cfg, "no [binds] section — using built-in keybindings");
        apply_default_binds(cfg);
        return;
    }

    int n = toml_table_nkval(tbl);
    if (n <= 0) {
        config_report_error(cfg, "[binds] is empty — using built-in keybindings");
        apply_default_binds(cfg);
        return;
    }

    cfg->keys = calloc(n, sizeof(KeyBind));
    if (!cfg->keys) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        const char *bind_str = toml_key_in(tbl, i);
        if (!bind_str) continue;

        toml_datum_t val = toml_string_in(tbl, bind_str);
        if (!val.ok) {
            config_report_error(cfg, "[binds] \"%s\": value must be a quoted string", bind_str);
            continue;
        }

        unsigned int mod;
        xkb_keysym_t key;
        if (!parse_bind_key(bind_str, &mod, &key)) {
            config_report_error(cfg, "[binds] \"%s\": unknown key or modifier", bind_str);
            free(val.u.s);
            continue;
        }
        if (!action_is_known(val.u.s)) {
            config_report_error(cfg, "[binds] \"%s\": unknown action \"%s\"", bind_str, val.u.s);
            free(val.u.s);
            continue;
        }

        cfg->keys[idx].mod = mod;
        cfg->keys[idx].key = key;
        strncpy(cfg->keys[idx].action, val.u.s, sizeof(cfg->keys[idx].action) - 1);
        free(val.u.s);
        idx++;
    }
    cfg->key_count = idx;

    /* Every line was broken: fall back rather than hand the user a compositor
     * with no way to open a terminal or quit. */
    if (idx == 0) {
        config_report_error(cfg, "no usable binds in [binds] — using built-in keybindings");
        apply_default_binds(cfg);
    }
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
    if (!root) return;

    toml_table_t *tbl = toml_table_in(root, "wallpaper_picker");
    if (!tbl) return;

    toml_datum_t d = toml_string_in(tbl, "dir");
    if (!d.ok) return;
    expand_tilde(d.u.s, cfg->wallpaper_dir, sizeof(cfg->wallpaper_dir));
    free(d.u.s);

    if (access(cfg->wallpaper_dir, R_OK | X_OK) != 0)
        config_report_error(cfg, "[wallpaper_picker] dir: cannot read \"%s\"",
                            cfg->wallpaper_dir);
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

        idx++;
    }
    cfg->wallpaper_count = idx;
}

void config_load(FwmConfig *cfg, const char *path) {
    cfg->physics         = physics_defaults;
    cfg->tiling          = (TilingConfig){ .gaps_in = 6, .gaps_out = 12, .anim_speed = 12.0 };
    cfg->camera          = (CameraConfig){ .anim_ms = 350.0, .free_speed = 14.0 };
    // Defaults for the no-config-file path; load_decor re-applies them anyway.
    cfg->decor.border_width = 2;
    parse_hex_color("#7aa2f7", cfg->decor.col_active);
    parse_hex_color("#3b4261", cfg->decor.col_inactive);
    cfg->decor.fade_in_ms = 260.0;
    cfg->decor.wallpaper_fade_ms = 420.0;
    cfg->decor.tray_opacity = 0.92;
    cfg->decor.launcher_opacity = 0.92;
    cfg->decor.icon_theme[0] = '\0';
    cfg->decor.color_source = COLOR_SOURCE_CONFIG;
    cfg->decor.tint_strength = 0.4;
    cfg->keys            = NULL;
    cfg->key_count       = 0;
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;
    cfg->error_count     = 0;
    cfg->error_total     = 0;
    cfg->fallback_binds  = 0;
    snprintf(cfg->source, sizeof(cfg->source), "%s", path ? path : "");
    load_input(NULL, &cfg->input); /* defaults for the no-config-file path */
    load_focus(NULL, &cfg->focus, cfg);

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

    load_physics(root, &cfg->physics);
    load_tiling(root, &cfg->tiling);
    load_camera(root, &cfg->camera);
    load_decor(root, cfg);
    load_input(root, &cfg->input);
    load_focus(root, &cfg->focus, cfg);
    load_binds(root, cfg);
    load_wallpaper(root, cfg);
    load_wallpaper_picker(root, cfg);

    toml_free(root);
}

void config_free(FwmConfig *cfg) {
    free(cfg->keys);
    cfg->keys      = NULL;
    cfg->key_count = 0;
    free(cfg->wallpapers);
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;
}
