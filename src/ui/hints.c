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

#include "hints.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "logo.h"
#include "../glass.h"
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define HINTS_FONT    "sans 10"
#define HINTS_PAD_X   36
#define HINTS_PAD_Y   26
#define HINTS_LINE_H  22
#define HINTS_LOGO_H  64
#define HINTS_LOGO_GAP 20
#define HINTS_CUT     14.0  /* corner chevron cut, px */
#define HINTS_KEY_GAP 18    /* px between a key cell and what it does */
#define HINTS_COL_GAP 44    /* px between one key+action pair and the next */
#define HINTS_MIN_CELL 60   /* no cell is squeezed narrower than this */
#define HINTS_MIN_W   460   /* the logo and the footer need this much anyway */
#define HINTS_MARGIN  24    /* px of screen left free on either side */
#define HINTS_MAX     160
#define HINTS_GROUPS  24

/* One rendered row: "Super+Q" -> "close window". */
struct HintRow {
    /* Wide enough for the longest thing written into it: a 31-char modifier
     * string followed by a 63-char key list. Sizing it to the common case
     * meant a crowded group silently lost its tail. */
    char key[96];
    char action[64];
};

struct HintsCtx {
    struct HintRow rows[HINTS_MAX];
    int count;
    double opacity;
    /* Worked out in hints_measure, from the text itself. */
    int cols;         /* key+action pairs across */
    int rows_per_col;
    int key_w;        /* px given to a key cell */
    int act_w;        /* px given to an action cell */
};

/* Same island silhouette as the tray pills, but with a moderate corner cut
 * instead of the full h/2 chevron — a big panel with h/2 points would look
 * like an arrow, not a card. */
static void panel_path(cairo_t *cr, double x, double y, double w, double h, double cut) {
    cairo_move_to(cr, x + cut, y);
    cairo_line_to(cr, x + w - cut, y);
    cairo_line_to(cr, x + w, y + cut);
    cairo_line_to(cr, x + w, y + h - cut);
    cairo_line_to(cr, x + w - cut, y + h);
    cairo_line_to(cr, x + cut, y + h);
    cairo_line_to(cr, x, y + h - cut);
    cairo_line_to(cr, x, y + cut);
    cairo_close_path(cr);
}

static void mods_string(unsigned int mod, char *out, size_t cap) {
    out[0] = '\0';
    if (mod & FWM_MOD_LOGO)  strncat(out, "Super+", cap - strlen(out) - 1);
    if (mod & FWM_MOD_CTRL)  strncat(out, "Ctrl+",  cap - strlen(out) - 1);
    if (mod & FWM_MOD_ALT)   strncat(out, "Alt+",   cap - strlen(out) - 1);
    if (mod & FWM_MOD_SHIFT) strncat(out, "Shift+", cap - strlen(out) - 1);
}

/* xkb keysym name -> compact display name.
 *
 * The media keys are here because their xkb names are the longest thing a
 * keyboard can produce: "XF86AudioRaiseVolume" is twenty characters for one
 * key, and three of them joined in a group ran off the side of the panel. What
 * is printed is the legend on the key rather than the symbol behind it. */
static const char *key_display(const char *name, char *buf, size_t cap) {
    if (!strcmp(name, "Left"))  return "\xe2\x86\x90";  /* ← */
    if (!strcmp(name, "Right")) return "\xe2\x86\x92";  /* → */
    if (!strcmp(name, "Up"))    return "\xe2\x86\x91";  /* ↑ */
    if (!strcmp(name, "Down"))  return "\xe2\x86\x93";  /* ↓ */
    if (!strcmp(name, "Return")) return "Enter";
    if (!strcmp(name, "space"))  return "Space";
    if (!strcmp(name, "Escape")) return "Esc";
    if (!strcmp(name, "question")) return "?";
    if (!strcmp(name, "slash"))    return "/";
    if (!strcmp(name, "XF86AudioRaiseVolume"))  return "Vol+";
    if (!strcmp(name, "XF86AudioLowerVolume"))  return "Vol\xe2\x88\x92"; /* − */
    if (!strcmp(name, "XF86AudioMute"))         return "Mute";
    if (!strcmp(name, "XF86AudioMicMute"))      return "MicMute";
    if (!strcmp(name, "XF86AudioPlay"))         return "Play";
    if (!strcmp(name, "XF86AudioPause"))        return "Pause";
    if (!strcmp(name, "XF86AudioNext"))         return "Next";
    if (!strcmp(name, "XF86AudioPrev"))         return "Prev";
    if (!strcmp(name, "XF86AudioStop"))         return "Stop";
    if (!strcmp(name, "XF86MonBrightnessUp"))   return "Bright+";
    if (!strcmp(name, "XF86MonBrightnessDown")) return "Bright\xe2\x88\x92";
    /* Anything else off the media block keeps its name minus the vendor
     * prefix, which says nothing and costs four characters on every one. */
    if (!strncmp(name, "XF86", 4)) {
        snprintf(buf, cap, "%s", name + 4);
        return buf;
    }
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') {
        buf[0] = (char)(name[0] - 'a' + 'A');
        buf[1] = '\0';
        return buf;
    }
    return name;
}

/* Action string -> human label. Returns NULL for actions that are collapsed
 * into a group row instead (those are emitted separately).
 *
 * Everything else gets a row, including an action this table has never heard
 * of: the sheet says it is generated from your binds, so a bind it cannot name
 * is printed as the action itself rather than dropped. A key that does
 * something and appears nowhere is worse than an ugly row. */
static const char *action_label(const char *a, char *buf, size_t cap) {
    static const struct { const char *action, *label; } map[] = {
        { "killclient",       "close window" },
        { "toggle_tiling",    "toggle tiling" },
        { "toggle_floating",  "toggle floating" },
        { "toggle_floating_all", "floating: all desktops" },
        { "fake_fullscreen",  "fake fullscreen" },
        { "real_fullscreen",  "fullscreen" },
        { "pin_window",       "pin window" },
        { "toggle_nocollide", "toggle no-collide" },
        { "toggle_nocollide_all", "no-collide: all windows" },
        { "toggle_tiling_all", "tiling: all desktops" },
        { "calm_all",         "calm all windows" },
        { "cycle_gravity",    "cycle gravity" },
        { "toggle_tray",      "show/hide tray" },
        { "spin_window",      "spin window (experimental)" },
        { "spin_all",         "spin: all windows" },
        { "toggle_split",     "flip split" },
        { "group_toggle",     "tab-stack on/off" },
        { "group_next",       "next tab" },
        { "group_prev",       "prev tab" },
        { "group_add",        "join stack under" },
        { "launcher",         "app launcher" },
        { "radial_menu",      "radial menu" },
        { "mixer",            "sound panel" },
        { "expo",             "desktop strip" },
        { "toggle_wrap",      "desktop strip: ring" },
        { "terminal",         "terminal" },
        { "show_hints",       "this help" },
        { "reload_config",    "reload config" },
        { "wallpaper_picker", "wallpaper picker" },
        { "screenshot",       "screenshot" },
        { "screenshot_region", "screenshot: region" },
        { "show_errors",      "config problems" },
        { "modes_menu",       "modes menu" },
        { "stats_menu",       "stats menu" },
        { "output_off",       "this monitor off" },
        { "toggle_internal_output", "laptop panel on/off" },
        { "outputs_on",       "all monitors on" },
        { "toggle_sun",       "shadows on/off" },
        { "sun_mode",         "sun: clock or hand" },
        { "star_spawn",       "light a star" },
        { "star_off",         "put the star out" },
        { "star_collapse",    "collapse the star" },
        { "EXIT",             "exit fwm" },
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (!strcmp(a, map[i].action)) return map[i].label;
    }
    if (!strncmp(a, "spawn:", 6)) {
        snprintf(buf, cap, "%s", a + 6);
        char *sp = strchr(buf, ' ');
        if (sp) *sp = '\0';       /* command name only, no args */
        return buf;
    }
    /* A key handed to somebody else entirely. Naming the client is the whole
     * point of the row: this key does nothing fwm can describe. */
    if (!strncmp(a, "global:", 7)) {
        char who[48];
        snprintf(who, sizeof(who), "%s", a + 7);
        char *colon = strchr(who, ':');
        if (colon) *colon = '\0';
        snprintf(buf, cap, "%s (external)", who);
        return buf;
    }
    /* set:sun.blur+2 — the option is what the key is about; the step is not. */
    if (!strncmp(a, "set:", 4)) {
        char opt[48];
        snprintf(opt, sizeof(opt), "%s", a + 4);
        size_t n = strcspn(opt, "+-=");
        opt[n] = '\0';
        snprintf(buf, cap, "set %s", opt);
        return buf;
    }
    return NULL; /* collapsed into a group, or named by the caller as-is */
}

/* ── collapsed groups ────────────────────────────────────────────────── */

/* Several binds shown as one row: "Super+1…0  switch desktop".
 *
 * A group is a prefix AND a modifier. The row prints one modifier in front of
 * every key it collected, so binds that share the action but not the modifier
 * must not share a row — grouping on the prefix alone printed the first
 * member's modifier over all of them, and a sheet that says Super over a key
 * bound to Alt is worse than no sheet. */
struct Group {
    const char *prefix;
    const char *label;
    unsigned int mod;
    char keys[64];
    char first[24], last[24];  /* ends of the range, when there is one */
    int single;                /* every key so far is one plain character */
    int seen;
    int dropped;               /* members the keys buffer had no room for */
};

static const struct { const char *prefix, *label; } group_kinds[] = {
    { "view:",          "switch desktop" },
    { "move_camera:",   "scroll camera" },
    { "tile_focus:",    "focus tile" },
    { "tile_move:",     "move tile" },
    { "move_to:",       "send window over" },
    { "move_to_view:",  "send window, follow" },
    { "volume:",        "volume" },
    { "sun_azimuth:",   "turn the sun" },
    { "sun_elevation:", "raise the sun" },
};

/* Returns true when the bind belonged to a group and has been folded in. */
static bool groups_add(struct Group *groups, int *ngroups, const KeyBind *kb,
                       const char *keyname) {
    const char *label = NULL;
    const char *prefix = NULL;
    for (size_t k = 0; k < sizeof(group_kinds)/sizeof(group_kinds[0]); k++) {
        if (!strncmp(kb->action, group_kinds[k].prefix, strlen(group_kinds[k].prefix))) {
            prefix = group_kinds[k].prefix;
            label  = group_kinds[k].label;
            break;
        }
    }
    if (!prefix) return false;

    struct Group *g = NULL;
    for (int i = 0; i < *ngroups; i++) {
        if (groups[i].prefix == prefix && groups[i].mod == kb->mod) {
            g = &groups[i];
            break;
        }
    }
    if (!g) {
        /* Out of group slots: hand the bind back so it gets an ordinary row of
         * its own. Swallowing it here would be the one way this sheet could
         * still hide a key, which is the whole thing it must not do. */
        if (*ngroups >= HINTS_GROUPS) return false;
        g = &groups[(*ngroups)++];
        memset(g, 0, sizeof(*g));
        g->prefix = prefix;
        g->label  = label;
        g->mod    = kb->mod;
        g->single = 1;
    }

    char tmp[32];
    const char *disp = key_display(keyname, tmp, sizeof(tmp));
    if (strlen(disp) != 1 || (disp[0] & 0x80)) g->single = 0;

    if (!g->seen) {
        snprintf(g->keys, sizeof(g->keys), "%s", disp);
        snprintf(g->first, sizeof(g->first), "%s", disp);
    } else if (strlen(g->keys) + strlen(disp) + 2 < sizeof(g->keys)) {
        /* arrows join tight (←→↑↓); everything else with a dot */
        if (disp[0] & 0x80) strcat(g->keys, disp);
        else { strcat(g->keys, "\xc2\xb7"); strcat(g->keys, disp); }
    } else {
        g->dropped++;
    }
    snprintf(g->last, sizeof(g->last), "%s", disp);
    g->seen++;
    return true;
}

/* ── mouse ───────────────────────────────────────────────────────────── */

/* The drag verbs are not [binds] actions and action_label knows nothing of
 * them, so they are named here. Everything else in a [mouse] value is an
 * ordinary action and goes through the usual labeller. */
static const char *mouse_label(const char *a, char *buf, size_t cap) {
    if (!strcmp(a, FWM_MOUSE_MOVE))           return "move window";
    if (!strcmp(a, FWM_MOUSE_MOVE_NOCOLLIDE)) return "move, no collisions";
    if (!strcmp(a, FWM_MOUSE_RESIZE))         return "resize window";
    if (!strcmp(a, FWM_MOUSE_SWAP))           return "swap tiles";
    if (!strcmp(a, FWM_MOUSE_TWIST))          return "turn window";
    return action_label(a, buf, cap);
}

static const char *button_name(int button) {
    switch (button) {
    case FWM_BTN_LEFT:   return "drag";        /* the button a drag is done with */
    case FWM_BTN_RIGHT:  return "right-drag";
    case FWM_BTN_MIDDLE: return "middle";
    case FWM_BTN_SIDE:   return "side";
    default:             return "extra";
    }
}

static void mouse_build(const FwmConfig *cfg, struct HintsCtx *ctx) {
    for (int i = 0; i < cfg->mouse.bind_count && ctx->count < HINTS_MAX; i++) {
        const MouseBind *mb = &cfg->mouse.binds[i];
        char labelbuf[64];
        const char *label = mouse_label(mb->action, labelbuf, sizeof(labelbuf));
        if (!label) {
            snprintf(labelbuf, sizeof(labelbuf), "%.*s",
                     (int)sizeof(labelbuf) - 1, mb->action);
            label = labelbuf;
        }

        struct HintRow *row = &ctx->rows[ctx->count++];
        char mods[32];
        mods_string(mb->mod, mods, sizeof(mods));
        snprintf(row->key, sizeof(row->key), "%s%s", mods, button_name(mb->button));
        snprintf(row->action, sizeof(row->action), "%s", label);
    }
}

/* ── gestures ────────────────────────────────────────────────────────── */

/* Gestures are the one part of the config nothing on screen ever hints at: no
 * key to hunt for, no menu item. Listing them here is the only place a user
 * finds out that three fingers do anything at all. */
static const char *gesture_label(const char *a, char *buf, size_t cap) {
    if (!strcmp(a, GESTURE_ACTION_PAN))     return "pan desktops";
    if (!strcmp(a, "move_to_view:next") ||
        !strcmp(a, "move_to_view:prev"))    return "take window across";
    if (!strncmp(a, "view:", 5))            return "switch desktop";
    return action_label(a, buf, cap);
}

/* "3 ←", and the arrow a second bind with the same meaning folds into. */
static const char *dir_arrow(int dir) {
    switch (dir) {
    case GESTURE_SWIPE_LEFT:  return "\xe2\x86\x90"; /* ← */
    case GESTURE_SWIPE_RIGHT: return "\xe2\x86\x92"; /* → */
    case GESTURE_SWIPE_UP:    return "\xe2\x86\x91"; /* ↑ */
    case GESTURE_SWIPE_DOWN:  return "\xe2\x86\x93"; /* ↓ */
    case GESTURE_PINCH_IN:    return "in";
    default:                  return "out";
    }
}

static void gestures_build(const FwmConfig *cfg, struct HintsCtx *ctx) {
    for (int i = 0; i < cfg->gestures.bind_count && ctx->count < HINTS_MAX; i++) {
        const GestureBind *gb = &cfg->gestures.binds[i];
        char labelbuf[64];
        const char *label = gesture_label(gb->action, labelbuf, sizeof(labelbuf));
        if (!label) {
            snprintf(labelbuf, sizeof(labelbuf), "%.*s",
                     (int)sizeof(labelbuf) - 1, gb->action);
            label = labelbuf;
        }

        int pinch = gb->dir == GESTURE_PINCH_IN || gb->dir == GESTURE_PINCH_OUT;
        char key[96];
        snprintf(key, sizeof(key), "%s%d %s", pinch ? "pinch" : "swipe",
                 gb->fingers, dir_arrow(gb->dir));

        /* One meaning, one row: the pan is bound left AND right, and it would
         * be odd to see it listed twice. */
        struct HintRow *row = NULL;
        for (int r = 0; r < ctx->count; r++) {
            if (!strcmp(ctx->rows[r].action, label) &&
                !strncmp(ctx->rows[r].key, key, strlen(key) - strlen(dir_arrow(gb->dir)))) {
                row = &ctx->rows[r];
                break;
            }
        }
        if (row) {
            strncat(row->key, dir_arrow(gb->dir),
                    sizeof(row->key) - strlen(row->key) - 1);
            continue;
        }
        row = &ctx->rows[ctx->count++];
        snprintf(row->key, sizeof(row->key), "%s", key);
        snprintf(row->action, sizeof(row->action), "%s", label);
    }
}

static void hints_build(const FwmConfig *cfg, struct HintsCtx *ctx) {
    struct Group groups[HINTS_GROUPS];
    int ngroups = 0;

    ctx->count = 0;
    for (int i = 0; i < cfg->key_count && ctx->count < HINTS_MAX; i++) {
        const KeyBind *kb = &cfg->keys[i];
        char keyname[64];
        if (xkb_keysym_get_name(kb->key, keyname, sizeof(keyname)) <= 0) continue;

        char labelbuf[64], keybuf[32];
        if (groups_add(groups, &ngroups, kb, keyname)) continue;

        const char *label = action_label(kb->action, labelbuf, sizeof(labelbuf));
        if (!label) {   /* unnamed, but never hidden */
            snprintf(labelbuf, sizeof(labelbuf), "%.*s",
                     (int)sizeof(labelbuf) - 1, kb->action);
            label = labelbuf;
        }

        struct HintRow *row = &ctx->rows[ctx->count++];
        char mods[32];
        mods_string(kb->mod, mods, sizeof(mods));
        snprintf(row->key, sizeof(row->key), "%s%s",
                 mods, key_display(keyname, keybuf, sizeof(keybuf)));
        snprintf(row->action, sizeof(row->action), "%s", label);
    }

    for (int g = 0; g < ngroups; g++) {
        if (!groups[g].seen || ctx->count >= HINTS_MAX) continue;
        struct HintRow *row = &ctx->rows[ctx->count++];
        char mods[32];
        mods_string(groups[g].mod, mods, sizeof(mods));

        /* Ten desktops read better as a range than as ten joined digits — but
         * only when every member really is one character, and the range is the
         * FIRST and LAST key collected rather than the first and last byte of
         * the joined string. Taking bytes turned a group that had picked up a
         * media key into "Super+1…e", the tail of "…Volume". */
        if (groups[g].seen >= 4 && groups[g].single) {
            snprintf(row->key, sizeof(row->key), "%s%s\xe2\x80\xa6%s",
                     mods, groups[g].first, groups[g].last);
        } else if (groups[g].dropped) {
            snprintf(row->key, sizeof(row->key), "%s%s\xe2\x80\xa6",
                     mods, groups[g].keys);
        } else {
            snprintf(row->key, sizeof(row->key), "%s%s", mods, groups[g].keys);
        }
        snprintf(row->action, sizeof(row->action), "%s", groups[g].label);
    }

    mouse_build(cfg, ctx);
    gestures_build(cfg, ctx);
}

/* ── layout ──────────────────────────────────────────────────────────── */

static int text_w(PangoLayout *l, const char *s) {
    pango_layout_set_text(l, s, -1);
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    (void)h;
    return w;
}

/* The panel is only as wide as what it says, and never wider than the screen.
 *
 * The fixed 190px key column this replaced was narrower than "Super+Shift+S"
 * with a media key in it, so the key ran straight through the label beside it,
 * and the fixed 760px panel then ran off the side of a small screen. Both cells
 * are measured here with the font the draw uses; if the total still does not
 * fit, the label gives up its width first — a clipped label still says which
 * key it belongs to, while a clipped key is a key you cannot press. */
static void hints_measure(struct HintsCtx *ctx, int screen_w, int screen_h) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(s);
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *d = pango_font_description_from_string(HINTS_FONT);
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);

    ctx->key_w = ctx->act_w = 0;
    for (int i = 0; i < ctx->count; i++) {
        int w = text_w(l, ctx->rows[i].key);
        if (w > ctx->key_w) ctx->key_w = w;
        w = text_w(l, ctx->rows[i].action);
        if (w > ctx->act_w) ctx->act_w = w;
    }

    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_destroy(s);

    /* Two columns is the shape the sheet is drawn in. A screen too short for
     * the binds gets a third and a fourth rather than a panel hanging off the
     * bottom edge, since there is nothing to scroll here. */
    int chrome = HINTS_PAD_Y * 2 + HINTS_LOGO_H + HINTS_LOGO_GAP + 2 * HINTS_LINE_H;
    int room = (screen_h - chrome) / HINTS_LINE_H;
    if (room < 1) room = 1;
    ctx->cols = 2;
    while (ctx->cols < 4 && (ctx->count + ctx->cols - 1) / ctx->cols > room)
        ctx->cols++;
    ctx->rows_per_col = (ctx->count + ctx->cols - 1) / ctx->cols;
    if (ctx->rows_per_col < 1) ctx->rows_per_col = 1;

    int avail = screen_w - 2 * HINTS_MARGIN - 2 * HINTS_PAD_X
              - (ctx->cols - 1) * HINTS_COL_GAP;
    int col_w = avail / ctx->cols;
    int want  = ctx->key_w + HINTS_KEY_GAP + ctx->act_w;
    if (want > col_w) {
        int over = want - col_w;
        int give = ctx->act_w - HINTS_MIN_CELL;
        if (give < 0) give = 0;
        int take = over < give ? over : give;
        ctx->act_w -= take;
        over -= take;
        if (over > 0) ctx->key_w -= over;
        if (ctx->key_w < HINTS_MIN_CELL) ctx->key_w = HINTS_MIN_CELL;
        if (ctx->act_w < HINTS_MIN_CELL) ctx->act_w = HINTS_MIN_CELL;
    }
}

static int hints_width(const struct HintsCtx *ctx) {
    int w = 2 * HINTS_PAD_X + (ctx->cols - 1) * HINTS_COL_GAP
          + ctx->cols * (ctx->key_w + HINTS_KEY_GAP + ctx->act_w);
    return w < HINTS_MIN_W ? HINTS_MIN_W : w;
}

static void draw_hints_content(cairo_t *cr, int w, int h, void *user_data) {
    struct HintsCtx *ctx = user_data;

    /* Same flat near-black as the tray islands, same opacity knob. */
    const FwmTheme *thm = theme_get();
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], ctx->opacity);
    panel_path(cr, 0, 0, w, h, HINTS_CUT);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(HINTS_FONT);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    double logo_w = HINTS_LOGO_H * FWM_LOGO_AR_BRACKETS;
    fwm_logo_draw(cr, (w - logo_w) / 2.0, HINTS_PAD_Y, HINTS_LOGO_H, FWM_LOGO_BRACKETS,
                  0.816, 0.659, 0.173, 1.0);

    int col_w = ctx->key_w + HINTS_KEY_GAP + ctx->act_w + HINTS_COL_GAP;
    int top = HINTS_PAD_Y + HINTS_LOGO_H + HINTS_LOGO_GAP;

    /* Cells are clipped by the layout rather than by the panel edge: a row that
     * outgrows its cell ends in an ellipsis instead of running over its
     * neighbour or off the card. */
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    for (int i = 0; i < ctx->count; i++) {
        int col = i / ctx->rows_per_col;
        int x = HINTS_PAD_X + col * col_w;
        int y = top + (i % ctx->rows_per_col) * HINTS_LINE_H;

        cairo_set_source_rgba(cr, 0.92, 0.94, 0.96, 1.0);
        pango_layout_set_width(layout, ctx->key_w * PANGO_SCALE);
        pango_layout_set_text(layout, ctx->rows[i].key, -1);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);

        cairo_set_source_rgba(cr, 0.56, 0.60, 0.67, 1.0);
        pango_layout_set_width(layout, ctx->act_w * PANGO_SCALE);
        pango_layout_set_text(layout, ctx->rows[i].action, -1);
        cairo_move_to(cr, x + ctx->key_w + HINTS_KEY_GAP, y);
        pango_cairo_show_layout(cr, layout);
    }

    const char *footer = "Esc / Enter \xe2\x80\x94 close";
    int fw;
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_width(layout, -1);
    pango_layout_set_text(layout, footer, -1);
    pango_layout_get_pixel_size(layout, &fw, NULL);
    cairo_set_source_rgba(cr, 0.56, 0.60, 0.67, 1.0);
    cairo_move_to(cr, (w - fw) / 2.0,
                  top + ctx->rows_per_col * HINTS_LINE_H + HINTS_LINE_H / 2);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

struct wlr_scene_buffer *hints_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                    const FwmConfig *cfg) {
    struct HintsCtx ctx;
    hints_build(cfg, &ctx);
    ctx.opacity = glass_fill(cfg, cfg->decor.tray_opacity);
    hints_measure(&ctx, screen_w, screen_h);

    int hints_w = hints_width(&ctx);
    int hints_h = HINTS_PAD_Y * 2 + HINTS_LOGO_H + HINTS_LOGO_GAP
                + (ctx.rows_per_col + 2) * HINTS_LINE_H; /* +2: gap + footer row */
    int wx = (screen_w - hints_w) / 2;
    int wy = (screen_h - hints_h) / 2;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;

    struct wlr_scene_buffer *hints_buf = cairo_overlay_create(parent, hints_w, hints_h);
    if (hints_buf) {
        wlr_scene_node_set_position(&hints_buf->node, wx, wy);
        cairo_overlay_update(hints_buf, draw_hints_content, &ctx);
        /* Before make_static, which is the last moment the sheet's own
         * pixels exist to be taken a copy of. */
        glass_attach(hints_buf);
        cairo_overlay_make_static(hints_buf);
        cairo_overlay_animate_in(hints_buf, 170.0, 14.0);
    }
    return hints_buf;
}
