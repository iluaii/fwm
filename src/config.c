#include "config.h"
#include "toml.h"

#include <X11/keysym.h>
#include <X11/Xlib.h>
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
    if (strcmp(tok, "super") == 0)  return Mod4Mask;
    if (strcmp(tok, "alt")   == 0)  return Mod1Mask;
    if (strcmp(tok, "ctrl")  == 0)  return ControlMask;
    if (strcmp(tok, "shift") == 0)  return ShiftMask;
    return 0;
}

static int parse_bind_key(const char *str, unsigned int *mod_out, KeySym *key_out) {
    char buf[128];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    *mod_out = 0;
    *key_out = NoSymbol;

    char *tokens[8];
    int   n = 0;
    char *p = buf;
    char *tok;
    while ((tok = strsep(&p, "+")) != NULL && n < 8)
        tokens[n++] = tok;

    if (n == 0) return 0;

    for (int i = 0; i < n - 1; i++)
        *mod_out |= parse_mod_token(tokens[i]);

    *key_out = XStringToKeysym(tokens[n - 1]);
    if (*key_out == NoSymbol) {
        fprintf(stderr, "fwm config: unknown keysym '%s'\n", tokens[n - 1]);
        return 0;
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
        KeySym key;
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

void config_load(FwmConfig *cfg, const char *path) {
    cfg->physics   = physics_defaults;
    cfg->keys      = NULL;
    cfg->key_count = 0;

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
    load_binds(root, cfg);

    toml_free(root);
}

void config_free(FwmConfig *cfg) {
    free(cfg->keys);
    cfg->keys      = NULL;
    cfg->key_count = 0;
}