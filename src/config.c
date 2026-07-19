#include "config.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        else {
            fprintf(stderr, "fwm config: unknown keysym '%s'\n", keyname);
            return 0;
        }
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

    toml_table_t *tbl = toml_table_in(root, "camera");
    if (!tbl) return;

    LOAD_DOUBLE(tbl, "anim_ms", c->anim_ms);
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

static void load_decor(toml_table_t *root, DecorConfig *dc) {
    dc->border_width = 2;
    parse_hex_color("#7aa2f7", dc->col_active);   /* soft blue */
    parse_hex_color("#3b4261", dc->col_inactive); /* muted slate */
    dc->fade_in_ms = 150.0;

    toml_table_t *tbl = toml_table_in(root, "decor");
    if (!tbl) return;

    toml_datum_t d;
    d = toml_int_in(tbl, "border_width");
    if (d.ok) dc->border_width = (int)d.u.i;
    if (dc->border_width < 0) dc->border_width = 0;

    d = toml_string_in(tbl, "col_active");
    if (d.ok) { parse_hex_color(d.u.s, dc->col_active); free(d.u.s); }
    d = toml_string_in(tbl, "col_inactive");
    if (d.ok) { parse_hex_color(d.u.s, dc->col_inactive); free(d.u.s); }
    LOAD_DOUBLE(tbl, "fade_in_ms", dc->fade_in_ms);
}

/* ── binds section ───────────────────────────────────────────────────── */

static void load_binds(toml_table_t *root, FwmConfig *cfg) {
    cfg->keys      = NULL;
    cfg->key_count = 0;

    toml_table_t *tbl = toml_table_in(root, "binds");
    if (!tbl) return;

    int n = toml_table_nkval(tbl);
    if (n <= 0) return;

    cfg->keys = calloc(n, sizeof(KeyBind));
    if (!cfg->keys) { perror("calloc"); return; }

    int idx = 0;
    for (int i = 0; i < n; i++) {
        const char *bind_str = toml_key_in(tbl, i);
        if (!bind_str) continue;

        toml_datum_t val = toml_string_in(tbl, bind_str);
        if (!val.ok) continue;

        unsigned int mod;
        xkb_keysym_t key;
        if (!parse_bind_key(bind_str, &mod, &key)) {
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
}

/* ── public api ──────────────────────────────────────────────────────── */

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
        if (!path.ok) continue;

        strncpy(cfg->wallpapers[idx].path, path.u.s, sizeof(cfg->wallpapers[idx].path) - 1);
        free(path.u.s);

        toml_datum_t fit = toml_string_in(tbl, "fit");
        int mode = WALLPAPER_FIT_COVER;
        if (fit.ok) {
            if (strcmp(fit.u.s, "contain") == 0)   mode = WALLPAPER_FIT_CONTAIN;
            else if (strcmp(fit.u.s, "pan") == 0)  mode = WALLPAPER_FIT_PAN;
            free(fit.u.s);
        }
        cfg->wallpapers[idx].fit = mode;

        toml_datum_t zoom = toml_double_in(tbl, "zoom");
        cfg->wallpapers[idx].zoom = zoom.ok ? zoom.u.d : 0.0; /* 0 = auto (native) */

        idx++;
    }
    cfg->wallpaper_count = idx;
}

void config_load(FwmConfig *cfg, const char *path) {
    cfg->physics         = physics_defaults;
    cfg->tiling          = (TilingConfig){ .gaps_in = 6, .gaps_out = 12, .anim_speed = 12.0 };
    cfg->camera          = (CameraConfig){ .anim_ms = 350.0 };
    // Defaults for the no-config-file path; load_decor re-applies them anyway.
    cfg->decor.border_width = 2;
    parse_hex_color("#7aa2f7", cfg->decor.col_active);
    parse_hex_color("#3b4261", cfg->decor.col_inactive);
    cfg->decor.fade_in_ms = 150.0;
    cfg->keys            = NULL;
    cfg->key_count       = 0;
    cfg->wallpapers      = NULL;
    cfg->wallpaper_count = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "fwm config: cannot open '%s', using defaults\n", path);
        return;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, sizeof(errbuf));
    fclose(f);

    if (!root) {
        fprintf(stderr, "fwm config: parse error: %s\n", errbuf);
        return;
    }

    load_physics(root, &cfg->physics);
    load_tiling(root, &cfg->tiling);
    load_camera(root, &cfg->camera);
    load_decor(root, &cfg->decor);
    load_binds(root, cfg);
    load_wallpaper(root, cfg);

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
