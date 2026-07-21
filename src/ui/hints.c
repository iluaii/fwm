#include "hints.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "logo.h"
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define HINTS_W       760
#define HINTS_PAD_X   36
#define HINTS_PAD_Y   26
#define HINTS_LINE_H  22
#define HINTS_LOGO_H  64
#define HINTS_LOGO_GAP 20
#define HINTS_CUT     14.0  /* corner chevron cut, px */
#define HINTS_KEY_COL 190   /* key column width inside each half */
#define HINTS_MAX     64

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

/* xkb keysym name -> compact display name. */
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
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') {
        buf[0] = (char)(name[0] - 'a' + 'A');
        buf[1] = '\0';
        return buf;
    }
    return name;
}

/* Action string -> human label. Returns NULL for actions that should not get
 * their own row (collapsed groups are emitted separately). */
static const char *action_label(const char *a, char *buf, size_t cap) {
    static const struct { const char *action, *label; } map[] = {
        { "killclient",       "close window" },
        { "toggle_tiling",    "toggle tiling" },
        { "fake_fullscreen",  "fake fullscreen" },
        { "real_fullscreen",  "fullscreen" },
        { "pin_window",       "pin window" },
        { "toggle_nocollide", "toggle no-collide" },
        { "toggle_nocollide_all", "no-collide: all windows" },
        { "toggle_tiling_all", "tiling: all desktops" },
        { "calm_all",         "calm all windows" },
        { "cycle_gravity",    "cycle gravity" },
        { "toggle_split",     "flip split" },
        { "group_toggle",     "tab-stack on/off" },
        { "group_next",       "next tab" },
        { "group_prev",       "prev tab" },
        { "group_add",        "join stack under" },
        { "launcher",         "app launcher" },
        { "show_hints",       "this help" },
        { "reload_config",    "reload config" },
        { "wallpaper_picker", "wallpaper picker" },
        { "show_errors",      "config problems" },
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
    return NULL; /* view:, tile_*, move_camera: handled as groups; unknown hidden */
}

/* Collapsed groups: several binds shown as one row. `prefix` matches the
 * action, the key cell is the joined key names of every member. */
struct Group {
    const char *prefix;
    const char *label;
    unsigned int mod;
    char keys[64];
    int seen;
};

static void groups_add(struct Group *groups, int ngroups, const KeyBind *kb,
                       const char *keyname) {
    for (int g = 0; g < ngroups; g++) {
        if (strncmp(kb->action, groups[g].prefix, strlen(groups[g].prefix)) != 0)
            continue;
        char tmp[16];
        const char *disp = key_display(keyname, tmp, sizeof(tmp));
        if (!groups[g].seen) {
            groups[g].mod = kb->mod;
            snprintf(groups[g].keys, sizeof(groups[g].keys), "%s", disp);
        } else if (strlen(groups[g].keys) + strlen(disp) + 2 < sizeof(groups[g].keys)) {
            /* arrows join tight (←→↑↓); everything else with a dot */
            if (disp[0] & 0x80) strcat(groups[g].keys, disp);
            else { strcat(groups[g].keys, "\xc2\xb7"); strcat(groups[g].keys, disp); }
        }
        groups[g].seen++;
        return;
    }
}

static void hints_build(const FwmConfig *cfg, struct HintsCtx *ctx) {
    struct Group groups[] = {
        { "view:",        "switch desktop", 0, "", 0 },
        { "move_camera:", "scroll camera",  0, "", 0 },
        { "tile_focus:",  "focus tile",     0, "", 0 },
        { "tile_move:",   "move tile",      0, "", 0 },
    };
    const int ngroups = (int)(sizeof(groups)/sizeof(groups[0]));

    ctx->count = 0;
    for (int i = 0; i < cfg->key_count && ctx->count < HINTS_MAX; i++) {
        const KeyBind *kb = &cfg->keys[i];
        char keyname[32];
        if (xkb_keysym_get_name(kb->key, keyname, sizeof(keyname)) <= 0) continue;

        char labelbuf[64], keybuf[16];
        const char *label = action_label(kb->action, labelbuf, sizeof(labelbuf));
        if (!label) {
            groups_add(groups, ngroups, kb, keyname);
            continue;
        }
        struct HintRow *row = &ctx->rows[ctx->count++];
        char mods[32];
        mods_string(kb->mod, mods, sizeof(mods));
        snprintf(row->key, sizeof(row->key), "%s%s",
                 mods, key_display(keyname, keybuf, sizeof(keybuf)));
        snprintf(row->action, sizeof(row->action), "%s", label);
    }

    /* "view:" reads better as a range than as ten joined digits. */
    for (int g = 0; g < ngroups; g++) {
        if (!groups[g].seen || ctx->count >= HINTS_MAX) continue;
        struct HintRow *row = &ctx->rows[ctx->count++];
        char mods[32];
        mods_string(groups[g].mod, mods, sizeof(mods));
        if (!strcmp(groups[g].prefix, "view:") && groups[g].seen >= 4) {
            char first = groups[g].keys[0];
            char last = groups[g].keys[strlen(groups[g].keys) - 1];
            snprintf(row->key, sizeof(row->key), "%s%c\xe2\x80\xa6%c", mods, first, last);
        } else {
            snprintf(row->key, sizeof(row->key), "%s%s", mods, groups[g].keys);
        }
        snprintf(row->action, sizeof(row->action), "%s", groups[g].label);
    }
}

static void draw_hints_content(cairo_t *cr, int w, int h, void *user_data) {
    struct HintsCtx *ctx = user_data;

    /* Same flat near-black as the tray islands, same opacity knob. */
    const FwmTheme *thm = theme_get();
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], ctx->opacity);
    panel_path(cr, 0, 0, w, h, HINTS_CUT);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    double logo_w = HINTS_LOGO_H * FWM_LOGO_AR_BRACKETS;
    fwm_logo_draw(cr, (w - logo_w) / 2.0, HINTS_PAD_Y, HINTS_LOGO_H, FWM_LOGO_BRACKETS,
                  0.816, 0.659, 0.173, 1.0);

    int rows_per_col = (ctx->count + 1) / 2;
    int col_w = (w - 2 * HINTS_PAD_X) / 2;
    int top = HINTS_PAD_Y + HINTS_LOGO_H + HINTS_LOGO_GAP;

    for (int i = 0; i < ctx->count; i++) {
        int col = i / rows_per_col;
        int x = HINTS_PAD_X + col * col_w;
        int y = top + (i % rows_per_col) * HINTS_LINE_H;

        cairo_set_source_rgba(cr, 0.92, 0.94, 0.96, 1.0);
        pango_layout_set_text(layout, ctx->rows[i].key, -1);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);

        cairo_set_source_rgba(cr, 0.56, 0.60, 0.67, 1.0);
        pango_layout_set_text(layout, ctx->rows[i].action, -1);
        cairo_move_to(cr, x + HINTS_KEY_COL, y);
        pango_cairo_show_layout(cr, layout);
    }

    const char *footer = "Esc / Enter \xe2\x80\x94 close";
    int fw;
    pango_layout_set_text(layout, footer, -1);
    pango_layout_get_pixel_size(layout, &fw, NULL);
    cairo_set_source_rgba(cr, 0.56, 0.60, 0.67, 1.0);
    cairo_move_to(cr, (w - fw) / 2.0, top + rows_per_col * HINTS_LINE_H + HINTS_LINE_H / 2);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

struct wlr_scene_buffer *hints_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                    const FwmConfig *cfg) {
    struct HintsCtx ctx;
    hints_build(cfg, &ctx);
    ctx.opacity = cfg->decor.tray_opacity;

    int rows_per_col = (ctx.count + 1) / 2;
    int hints_h = HINTS_PAD_Y * 2 + HINTS_LOGO_H + HINTS_LOGO_GAP
                + (rows_per_col + 2) * HINTS_LINE_H; /* +2: gap + footer row */
    int wx = (screen_w - HINTS_W) / 2;
    int wy = (screen_h - hints_h) / 2;

    struct wlr_scene_buffer *hints_buf = cairo_overlay_create(parent, HINTS_W, hints_h);
    if (hints_buf) {
        wlr_scene_node_set_position(&hints_buf->node, wx, wy);
        cairo_overlay_update(hints_buf, draw_hints_content, &ctx);
        cairo_overlay_make_static(hints_buf);
        cairo_overlay_animate_in(hints_buf, 170.0, 14.0);
    }
    return hints_buf;
}
