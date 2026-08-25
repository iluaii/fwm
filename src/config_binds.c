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
#include <strings.h>
#include <xkbcommon/xkbcommon.h>

/* ── binds section ───────────────────────────────────────────────────── */

/* Built-in binds, installed whenever the config file yielded no usable ones
 * (missing file, TOML syntax error, empty or entirely broken [binds]). Without
 * them a single typo — a forgotten quote — leaves a running compositor that
 * cannot spawn a terminal, switch desktops or exit. Mirrors the defaults in
 * config.toml.example; keep the two in sync. */
static const struct { const char *bind; const char *action; } default_binds[] = {
    { "super+Return",         "terminal"         },
    { "super+space",          "launcher"         },
    { "super+q",              "killclient"       },
    { "super+t",              "toggle_tiling"    },
    { "super+alt+space",      "toggle_floating"  },
    { "super+d",              "fake_fullscreen"  },
    { "super+f",              "real_fullscreen"  },
    { "super+h",              "move_camera:-50"  },
    { "super+l",              "move_camera:50"   },
    { "super+p",              "pin_window"       },
    { "super+n",              "toggle_nocollide" },
    { "super+g",              "cycle_gravity"    },
    { "super+j",              "toggle_tray"      },
    { "super+r",              "spin_window"      },
    { "super+s",              "toggle_split"     },
    { "super+w",              "group_toggle"     },
    { "super+Tab",            "group_next"       },
    { "super+shift+Tab",      "group_prev"       },
    { "super+shift+w",        "group_add"        },
    /* Send the focused window to a desktop. Tiling has no other way out: the
     * layout owns the window's geometry, so it cannot be dragged across. */
    { "super+shift+1",        "move_to:0"        },
    { "super+shift+2",        "move_to:1"        },
    { "super+shift+3",        "move_to:2"        },
    { "super+shift+4",        "move_to:3"        },
    { "super+shift+5",        "move_to:4"        },
    { "super+shift+6",        "move_to:5"        },
    { "super+shift+7",        "move_to:6"        },
    { "super+shift+8",        "move_to:7"        },
    { "super+shift+9",        "move_to:8"        },
    { "super+shift+0",        "move_to:9"        },
    { "super+a",              "expo"             },
    /* Print for the screen, where every other desktop puts it. The region one
     * is on super+shift+s instead of shift+Print: a laptop without a Print key
     * would otherwise have no way to reach it. */
    { "Print",                "screenshot"        },
    { "super+shift+s",        "screenshot_region" },
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
    /* One desktop over, without having to know which number it is. `next` and
     * `prev` are steps rather than destinations, so with the strip closed into
     * a ring they step off desktop 9 onto 0 and back — the same wrap super+h
     * and super+l pan through. */
    { "super+ctrl+Left",      "view:prev"        },
    { "super+ctrl+Right",     "view:next"        },
    /* And one monitor over. Numbers rather than a "next screen" step: on two
     * screens a step is the same key twice and you have to look up to see
     * where you landed, while ctrl+alt+1 is always the screen at the left of
     * the desk. Every number past the last monitor lands on the last one, so a
     * two-screen desk gets 2..9 all meaning "the other one" and a third screen
     * takes over 3 the moment it is plugged in. */
    { "ctrl+alt+1",           "focus_output:1"   },
    { "ctrl+alt+2",           "focus_output:2"   },
    { "ctrl+alt+3",           "focus_output:3"   },
    { "ctrl+alt+4",           "focus_output:4"   },
    { "ctrl+alt+5",           "focus_output:5"   },
    { "ctrl+alt+6",           "focus_output:6"   },
    { "ctrl+alt+7",           "focus_output:7"   },
    { "ctrl+alt+8",           "focus_output:8"   },
    { "ctrl+alt+9",           "focus_output:9"   },
};

void apply_default_binds(FwmConfig *cfg) {
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

void load_binds(toml_table_t *root, FwmConfig *cfg) {
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

/* ── modes (submaps) ─────────────────────────────────────────────────── */

int config_mode_find(const FwmConfig *cfg, const char *name) {
    if (!name) return -1;
    /* Callers hold either "mode:resize" or "resize"; accept both rather than
     * making every one of them skip the prefix itself. */
    if (strncmp(name, FWM_MODE_ACTION, strlen(FWM_MODE_ACTION)) == 0)
        name += strlen(FWM_MODE_ACTION);
    for (int i = 0; i < cfg->mode_count; i++)
        if (strcmp(cfg->modes[i].name, name) == 0) return i;
    return -1;
}

const KeyBind *config_match_mode_bind(const FwmConfig *cfg, int mode,
                                      xkb_keysym_t sym, unsigned int mods) {
    if (mode < 0 || mode >= cfg->mode_count) return NULL;
    const ConfigMode *m = &cfg->modes[mode];
    xkb_keysym_t lower = xkb_keysym_to_lower(sym);
    for (int i = 0; i < m->key_count; i++) {
        if (m->keys[i].mod != mods) continue;
        if (m->keys[i].key == sym || m->keys[i].key == lower) return &m->keys[i];
    }
    return NULL;
}

/* One [mode.<name>] table: its binds, plus the two words that are settings
 * rather than binds. */
static void load_mode(FwmConfig *cfg, toml_table_t *tbl, const char *name) {
    if (cfg->mode_count >= CONFIG_MAX_MODES) {
        config_report_error(cfg, "[mode.%s]: too many modes (max %d) — ignored",
                            name, CONFIG_MAX_MODES);
        return;
    }
    ConfigMode *m = &cfg->modes[cfg->mode_count];
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);

    toml_datum_t st = toml_bool_in(tbl, "sticky");
    if (st.ok) m->sticky = st.u.b ? 1 : 0;

    int n = toml_table_nkval(tbl);
    m->keys = calloc(n > 0 ? n : 1, sizeof(KeyBind));
    if (!m->keys) { perror("calloc"); return; }

    for (int i = 0; i < n; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) continue;
        if (strcmp(key, "sticky") == 0) continue;

        toml_datum_t val = toml_string_in(tbl, key);
        if (!val.ok) {
            config_report_error(cfg, "[mode.%s] \"%s\": value must be a quoted string",
                                name, key);
            continue;
        }

        /* `enter` is not a bind of the mode but a bind INTO it, so it goes into
         * the root map — appended after [binds] has been read, below. */
        if (strcmp(key, "enter") == 0) { free(val.u.s); continue; }

        unsigned int mod;
        xkb_keysym_t sym;
        if (!parse_bind_key(key, &mod, &sym)) {
            config_report_error(cfg, "[mode.%s] \"%s\": unknown key or modifier", name, key);
            free(val.u.s);
            continue;
        }
        if (!action_is_known(val.u.s)) {
            config_report_error(cfg, "[mode.%s] \"%s\": unknown action \"%s\"",
                                name, key, val.u.s);
            free(val.u.s);
            continue;
        }
        m->keys[m->key_count].mod = mod;
        m->keys[m->key_count].key = sym;
        snprintf(m->keys[m->key_count].action, sizeof(m->keys[m->key_count].action),
                 "%s", val.u.s);
        m->key_count++;
        free(val.u.s);
    }

    if (m->key_count == 0) {
        /* A mode with nothing in it can still be entered, and then only Escape
         * gets you out — a trap rather than a feature. */
        config_report_error(cfg, "[mode.%s]: no binds — mode ignored", name);
        free(m->keys);
        m->keys = NULL;
        return;
    }
    cfg->mode_count++;
}

/* Append one bind to the root map. Used for each mode's `enter` key, which is
 * written inside the mode but belongs to the map you press it from. */
static void add_root_bind(FwmConfig *cfg, unsigned int mod, xkb_keysym_t key,
                          const char *action) {
    KeyBind *grown = realloc(cfg->keys, (size_t)(cfg->key_count + 1) * sizeof(KeyBind));
    if (!grown) { perror("realloc"); return; }
    cfg->keys = grown;
    KeyBind *b = &cfg->keys[cfg->key_count++];
    memset(b, 0, sizeof(*b));
    b->mod = mod;
    b->key = key;
    snprintf(b->action, sizeof(b->action), "%s", action);
}

void load_modes(toml_table_t *root, FwmConfig *cfg) {
    cfg->mode_count = 0;
    toml_table_t *tbl = root ? toml_table_in(root, "mode") : NULL;
    if (!tbl) return;

    for (int i = 0; ; i++) {
        const char *name = toml_key_in(tbl, i);
        if (!name) break;
        toml_table_t *sub = toml_table_in(tbl, name);
        if (!sub) {
            config_report_error(cfg, "[mode] \"%s\": modes are tables, e.g. [mode.%s]",
                                name, name);
            continue;
        }
        load_mode(cfg, sub, name);
    }

    /* Now that every mode exists and [binds] has been read, wire up the keys
     * that step into them. Done second so `enter` can be reported against a
     * mode that was itself dropped for being empty. */
    for (int i = 0; ; i++) {
        const char *name = toml_key_in(tbl, i);
        if (!name) break;
        toml_table_t *sub = toml_table_in(tbl, name);
        if (!sub) continue;
        toml_datum_t e = toml_string_in(sub, "enter");
        if (!e.ok) continue;

        if (config_mode_find(cfg, name) < 0) {
            free(e.u.s);
            continue;   /* the mode was dropped; it already said why */
        }
        unsigned int mod;
        xkb_keysym_t sym;
        if (!parse_bind_key(e.u.s, &mod, &sym)) {
            config_report_error(cfg, "[mode.%s] enter = \"%s\": unknown key or modifier",
                                name, e.u.s);
            free(e.u.s);
            continue;
        }
        char action[64];
        snprintf(action, sizeof(action), FWM_MODE_ACTION "%s", name);
        add_root_bind(cfg, mod, sym, action);
        free(e.u.s);
    }
}

/* ── the radial menu ─────────────────────────────────────────────────── */

static void load_radial_items(FwmConfig *cfg, toml_array_t *arr, int menu,
                              const char *path);

/* One [[radial.item]]. Everything but the thing it does is optional: an item
 * with no picture and no glyph still draws its label, and an item with no
 * label still fires. What it cannot lack is somewhere to go — an `action`, or
 * a ring of items of its own written one level deeper.
 *
 * `path` is how this item is spelled in the file ("[[radial.item]]", then
 * "[[radial.item]] Power" one ring in), so a mistake three rings down can be
 * found by reading the message. */
static void load_radial_item(FwmConfig *cfg, toml_table_t *tbl, int menu,
                             int idx, const char *path) {
    RadialConfig *r = &cfg->radial;
    RadialMenu *m = &r->menus[menu];

    /* Read the face first: it names the item in every message below, and a
     * sub-ring borrows it for its hub. */
    RadialItem it;
    memset(&it, 0, sizeof(it));
    toml_datum_t lbl = toml_string_in(tbl, "label");
    if (lbl.ok) { snprintf(it.label, sizeof(it.label), "%s", lbl.u.s); free(lbl.u.s); }
    toml_datum_t ico = toml_string_in(tbl, "icon");
    if (ico.ok) { expand_tilde(ico.u.s, it.icon, sizeof(it.icon)); free(ico.u.s); }
    toml_datum_t txt = toml_string_in(tbl, "text");
    if (txt.ok) { snprintf(it.text, sizeof(it.text), "%s", txt.u.s); free(txt.u.s); }

    /* How this one item is named from here on down. */
    char here[160];
    if (it.label[0]) snprintf(here, sizeof(here), "%s %s", path, it.label);
    else             snprintf(here, sizeof(here), "%s #%d", path, idx + 1);

    toml_array_t *sub = toml_array_in(tbl, "item");
    toml_datum_t act = toml_string_in(tbl, "action");

    if (sub && act.ok) {
        /* Both is not a third behaviour — pressing a petal can either fire or
         * open, and the items are the more specific thing to have written. */
        config_report_error(cfg, "%s: has both an action and its own items — "
                            "the sub-ring wins, \"%s\" ignored", here, act.u.s);
        free(act.u.s);
        act.ok = 0;
    }

    if (sub) {
        if (r->menu_count >= CONFIG_MAX_RADIAL_MENUS) {
            config_report_error(cfg, "%s: too many rings (max %d) — item ignored",
                                here, CONFIG_MAX_RADIAL_MENUS);
            return;
        }
        /* Claimed before its items are read, because reading them may claim
         * more rings still. */
        int child = r->menu_count++;
        memset(&r->menus[child], 0, sizeof(r->menus[child]));
        /* The sub-ring's hub is the face of the petal that opens it — so the
         * middle of the ring says what was pressed to get here, which is also
         * what pressing it undoes. `center`/`center_text` on the same item
         * override that for a hub picture of its own. */
        snprintf(r->menus[child].center, sizeof(r->menus[child].center), "%s", it.icon);
        snprintf(r->menus[child].center_text, sizeof(r->menus[child].center_text),
                 "%s", it.text[0] ? it.text : it.label);
        toml_datum_t ctr = toml_string_in(tbl, "center");
        if (ctr.ok) {
            expand_tilde(ctr.u.s, r->menus[child].center, sizeof(r->menus[child].center));
            free(ctr.u.s);
        }
        toml_datum_t ctxt = toml_string_in(tbl, "center_text");
        if (ctxt.ok) {
            snprintf(r->menus[child].center_text, sizeof(r->menus[child].center_text),
                     "%s", ctxt.u.s);
            free(ctxt.u.s);
        }

        load_radial_items(cfg, sub, child, here);

        if (r->menus[child].item_count == 0) {
            /* Nothing usable underneath: a petal that opens an empty ring is a
             * dead end with a way back, which is worse than not being there.
             * The ring stays claimed — indexes handed out below it are already
             * spent — but nothing points at it. */
            config_report_error(cfg, "%s: no usable items in its ring — item ignored", here);
            return;
        }
        it.submenu = child;
    } else {
        if (!act.ok) {
            config_report_error(cfg, "%s: no action and no items — item ignored", here);
            return;
        }
        if (!action_is_known(act.u.s)) {
            config_report_error(cfg, "%s: unknown action \"%s\"", here, act.u.s);
            free(act.u.s);
            return;
        }
        snprintf(it.action, sizeof(it.action), "%s", act.u.s);
        /* The petal has to say something. Falling back to the action means a
         * half-written item is visibly half-written rather than a blank circle.
         * Copied from the parsed string rather than from it.action: those two
         * are members of one object, and gcc cannot see that they do not
         * overlap. */
        if (!it.label[0] && !it.icon[0] && !it.text[0])
            snprintf(it.label, sizeof(it.label), "%s", act.u.s);
        free(act.u.s);
    }

    m->items[m->item_count++] = it;
}

/* The items of one ring, in file order. */
static void load_radial_items(FwmConfig *cfg, toml_array_t *arr, int menu,
                              const char *path) {
    int n = toml_array_nelem(arr);
    for (int i = 0; i < n; i++) {
        if (cfg->radial.menus[menu].item_count >= CONFIG_MAX_RADIAL) {
            config_report_error(cfg, "%s: too many items (max %d) — the rest ignored",
                                path, CONFIG_MAX_RADIAL);
            break;
        }
        toml_table_t *sub = toml_table_at(arr, i);
        if (!sub) {
            config_report_error(cfg, "%s #%d: items are tables", path, i + 1);
            continue;
        }
        load_radial_item(cfg, sub, menu, i, path);
    }
}

void load_radial(toml_table_t *root, FwmConfig *cfg) {
    RadialConfig *r = &cfg->radial;
    memset(r, 0, sizeof(*r));
    r->radius = 190.0;
    r->menu_count = 1;   /* the root ring exists even when nothing fills it */
    if (!root) return;

    toml_table_t *tbl = toml_table_in(root, "radial");
    if (!tbl) return;

    toml_datum_t rad = toml_double_in(tbl, "radius");
    if (rad.ok) {
        if (rad.u.d >= 80.0 && rad.u.d <= 600.0) r->radius = rad.u.d;
        else config_report_error(cfg, "[radial] radius %.0f out of range 80..600 — using %.0f",
                                 rad.u.d, r->radius);
    }
    RadialMenu *rootm = &r->menus[0];
    toml_datum_t ctr = toml_string_in(tbl, "center");
    if (ctr.ok) { expand_tilde(ctr.u.s, rootm->center, sizeof(rootm->center)); free(ctr.u.s); }
    toml_datum_t ctxt = toml_string_in(tbl, "center_text");
    if (ctxt.ok) { snprintf(rootm->center_text, sizeof(rootm->center_text), "%s", ctxt.u.s); free(ctxt.u.s); }

    toml_array_t *arr = toml_array_in(tbl, "item");
    if (!arr) {
        config_report_error(cfg, "[radial]: no items — write them as [[radial.item]]");
        return;
    }
    load_radial_items(cfg, arr, 0, "[[radial.item]]");

    if (rootm->item_count == 0) {
        /* An empty ring can still be opened, and then there is nothing in it
         * but the way out. The errors above already said why. */
        return;
    }

    /* Like a mode's `enter`, this is a bind INTO the menu and belongs to the
     * root map — added here, after [binds] has been read. */
    toml_datum_t e = toml_string_in(tbl, "enter");
    if (!e.ok) return;
    unsigned int mod;
    xkb_keysym_t sym;
    if (!parse_bind_key(e.u.s, &mod, &sym)) {
        config_report_error(cfg, "[radial] enter = \"%s\": unknown key or modifier", e.u.s);
        free(e.u.s);
        return;
    }
    add_root_bind(cfg, mod, sym, FWM_RADIAL_ACTION);
    free(e.u.s);
}

/* ── mouse section ───────────────────────────────────────────────────── */

/* "super+shift+left" -> mods + FWM_BTN_*. Shares parse_mod_token with the
 * keyboard, so the modifier spelling is the same in both tables. */
static int parse_mouse_key(const char *str, unsigned int *mod_out, int *btn_out) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", str);

    char *tokens[8];
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "+", &save); t && n < 8; t = strtok_r(NULL, "+", &save))
        tokens[n++] = t;
    if (n == 0) return 0;

    *mod_out = 0;
    for (int i = 0; i < n - 1; i++) {
        unsigned int m = parse_mod_token(tokens[i]);
        if (!m) return 0;   /* an unknown word here is a typo, not a button */
        *mod_out |= m;
    }

    const char *b = tokens[n - 1];
    if      (strcmp(b, "left")   == 0) *btn_out = FWM_BTN_LEFT;
    else if (strcmp(b, "right")  == 0) *btn_out = FWM_BTN_RIGHT;
    else if (strcmp(b, "middle") == 0) *btn_out = FWM_BTN_MIDDLE;
    else if (strcmp(b, "side")   == 0) *btn_out = FWM_BTN_SIDE;
    else if (strcmp(b, "extra")  == 0) *btn_out = FWM_BTN_EXTRA;
    else return 0;
    return 1;
}

int config_action_is_drag(const char *action) {
    return strcmp(action, FWM_MOUSE_MOVE) == 0
        || strcmp(action, FWM_MOUSE_MOVE_NOCOLLIDE) == 0
        || strcmp(action, FWM_MOUSE_RESIZE) == 0
        || strcmp(action, FWM_MOUSE_SWAP) == 0
        || strcmp(action, FWM_MOUSE_TWIST) == 0;
}

static void add_mouse_bind(MouseConfig *mc, unsigned int mod, int button, const char *action) {
    if (mc->bind_count >= CONFIG_MAX_MOUSE) return;
    MouseBind *b = &mc->binds[mc->bind_count++];
    b->mod    = mod;
    b->button = button;
    snprintf(b->action, sizeof(b->action), "%s", action);
}

/* What the button handler did before any of this was configurable. Also what
 * you get back by deleting the [mouse] section. */
static const struct { const char *bind; const char *action; } default_mouse[] = {
    { "super+left",       FWM_MOUSE_MOVE },
    { "super+shift+left", FWM_MOUSE_MOVE_NOCOLLIDE },
    { "super+right",      FWM_MOUSE_RESIZE },
};

static void apply_default_mouse(FwmConfig *cfg) {
    cfg->mouse.bind_count = 0;
    for (size_t i = 0; i < sizeof(default_mouse) / sizeof(default_mouse[0]); i++) {
        unsigned int mod;
        int btn;
        if (!parse_mouse_key(default_mouse[i].bind, &mod, &btn)) continue;
        add_mouse_bind(&cfg->mouse, mod, btn, default_mouse[i].action);
    }
}

void load_mouse(toml_table_t *root, FwmConfig *cfg) {
    toml_table_t *tbl = root ? toml_table_in(root, "mouse") : NULL;
    if (!tbl) { apply_default_mouse(cfg); return; }

    cfg->mouse.bind_count = 0;
    int n = toml_table_nkval(tbl);
    for (int i = 0; i < n; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) continue;

        unsigned int mod;
        int btn;
        if (!parse_mouse_key(key, &mod, &btn)) {
            config_report_error(cfg, "[mouse] \"%s\": not a button "
                                "(want e.g. \"super+left\" or \"super+shift+right\")", key);
            continue;
        }
        toml_datum_t val = toml_string_in(tbl, key);
        if (!val.ok) {
            config_report_error(cfg, "[mouse] \"%s\": value must be a quoted string", key);
            continue;
        }
        if (!config_action_is_drag(val.u.s) && !action_is_known(val.u.s)) {
            config_report_error(cfg, "[mouse] \"%s\": unknown action \"%s\"", key, val.u.s);
            free(val.u.s);
            continue;
        }
        if (cfg->mouse.bind_count >= CONFIG_MAX_MOUSE) {
            config_report_error(cfg, "too many [mouse] entries — only the first %d are used",
                                CONFIG_MAX_MOUSE);
            free(val.u.s);
            break;
        }
        add_mouse_bind(&cfg->mouse, mod, btn, val.u.s);
        free(val.u.s);
    }

    /* An empty or wholly broken [mouse] table leaves a compositor whose windows
     * cannot be moved with the mouse at all. That is a legitimate thing to want
     * — someone may drive everything from the keyboard — so it is honoured
     * rather than overridden; the errors above say what went wrong. */
}

const MouseBind *config_match_mouse(const FwmConfig *cfg, int button, unsigned int mods) {
    for (int i = 0; i < cfg->mouse.bind_count; i++) {
        const MouseBind *b = &cfg->mouse.binds[i];
        if (b->button == button && b->mod == mods) return b;
    }
    return NULL;
}

/* ── gestures section ────────────────────────────────────────────────── */

/* The gesture vocabulary is [binds]' plus the one action that only a gesture
 * can express. */
static int gesture_action_is_known(const char *a) {
    return action_is_known(a) || strcmp(a, GESTURE_ACTION_PAN) == 0;
}

/* "swipe3+left", "pinch2+in". Returns 0 on anything else. */
static int parse_gesture_key(const char *str, int *fingers_out, int *dir_out) {
    int swipe;
    if      (strncmp(str, "swipe", 5) == 0) swipe = 1;
    else if (strncmp(str, "pinch", 5) == 0) swipe = 0;
    else return 0;

    char *end;
    long n = strtol(str + 5, &end, 10);
    if (end == str + 5 || *end != '+') return 0;
    /* libinput reports 2..5; a one-finger "swipe" is just pointer motion. */
    if (n < 2 || n > 5) return 0;

    const char *dir = end + 1;
    int d;
    if (swipe) {
        if      (strcmp(dir, "left")  == 0) d = GESTURE_SWIPE_LEFT;
        else if (strcmp(dir, "right") == 0) d = GESTURE_SWIPE_RIGHT;
        else if (strcmp(dir, "up")    == 0) d = GESTURE_SWIPE_UP;
        else if (strcmp(dir, "down")  == 0) d = GESTURE_SWIPE_DOWN;
        else return 0;
    } else {
        if      (strcmp(dir, "in")  == 0) d = GESTURE_PINCH_IN;
        else if (strcmp(dir, "out") == 0) d = GESTURE_PINCH_OUT;
        else return 0;
    }

    *fingers_out = (int)n;
    *dir_out     = d;
    return 1;
}

static void add_gesture(GesturesConfig *g, int fingers, int dir, const char *action) {
    if (g->bind_count >= CONFIG_MAX_GESTURES) return;
    GestureBind *b = &g->binds[g->bind_count++];
    b->fingers = fingers;
    b->dir     = dir;
    snprintf(b->action, sizeof(b->action), "%s", action);
}

void load_gestures(toml_table_t *root, FwmConfig *cfg) {
    GesturesConfig *g = &cfg->gestures;
    g->sensitivity = 1.0;
    g->natural     = 1;
    g->bind_count  = 0;

    /* No gestures unless the config asks for them, by name. Unlike [binds] —
     * where an empty table would leave a compositor nobody can drive, so the
     * built-ins step in — a gesture that nobody asked for is a surprise: it
     * takes a swipe away from the application under the cursor, and on hardware
     * that reports its fingers coarsely it can fire on a stray palm. The set
     * worth copying is written out (commented) in config.toml.example. */
    toml_table_t *tbl = root ? toml_table_in(root, "gestures") : NULL;
    if (!tbl) return;

    toml_datum_t s = toml_double_in(tbl, "sensitivity");
    if (s.ok) {
        if (s.u.d > 0.0 && s.u.d <= 10.0) g->sensitivity = s.u.d;
        else config_report_error(cfg, "[gestures] sensitivity %.3f out of range 0..10 — using 1.0", s.u.d);
    }
    toml_datum_t nat = toml_bool_in(tbl, "natural");
    if (nat.ok) g->natural = nat.u.b ? 1 : 0;

    int n = toml_table_nkval(tbl);
    for (int i = 0; i < n; i++) {
        const char *key = toml_key_in(tbl, i);
        if (!key) continue;
        /* The two scalars share the table with the binds. */
        if (strcmp(key, "sensitivity") == 0 || strcmp(key, "natural") == 0) continue;

        int fingers, dir;
        if (!parse_gesture_key(key, &fingers, &dir)) {
            config_report_error(cfg, "[gestures] \"%s\": not a gesture "
                                "(want e.g. \"swipe3+left\" or \"pinch2+in\")", key);
            continue;
        }
        toml_datum_t val = toml_string_in(tbl, key);
        if (!val.ok) {
            config_report_error(cfg, "[gestures] \"%s\": value must be a quoted string", key);
            continue;
        }
        if (!gesture_action_is_known(val.u.s)) {
            config_report_error(cfg, "[gestures] \"%s\": unknown action \"%s\"", key, val.u.s);
            free(val.u.s);
            continue;
        }
        if (strcmp(val.u.s, GESTURE_ACTION_PAN) == 0 &&
            dir != GESTURE_SWIPE_LEFT && dir != GESTURE_SWIPE_RIGHT) {
            /* The desktop strip runs left to right; there is nothing above it
             * to pan to. */
            config_report_error(cfg, "[gestures] \"%s\": " GESTURE_ACTION_PAN
                                " needs a horizontal swipe", key);
            free(val.u.s);
            continue;
        }
        if (g->bind_count >= CONFIG_MAX_GESTURES) {
            config_report_error(cfg, "too many [gestures] entries — only the first %d are used",
                                CONFIG_MAX_GESTURES);
            free(val.u.s);
            break;
        }
        add_gesture(g, fingers, dir, val.u.s);
        free(val.u.s);
    }
}

