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

/* The text column on the wallpaper. See sidebar.h for the two halves. */

#include "sidebar.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <pango/pango.h>

#include "stats.h"
#include "theme.h"
#include "ui/cairo_overlay.h"
#include "ui/tray.h"

/* How dark the shadow under the text is, at full text opacity. Enough to
 * keep white letters legible on a sky; on a dark wallpaper it is invisible. */
#define SHADOW_ALPHA 0.6

/* ── the feed ────────────────────────────────────────────────────────── */

struct FwmSidebarFeed {
    FwmStats     *stats;
    /* The lines as configured. A copy, not a pointer into the config: the
     * config is rewritten in place by a reload, before reconfigure is told. */
    char          lines[SIDEBAR_MAX_LINES][SIDEBAR_LINE_MAX];
    int           line_count;
    GString      *markup;
};

/* The value of readout `name`, or "" while it has nothing to say. */
static const char *readout(const FwmSidebarFeed *f, const char *name, size_t len) {
    for (int i = 0; i < stats_count(f->stats); i++) {
        const StatsItem *it = stats_item(f->stats, i);
        if (strlen(it->name) == len && memcmp(it->name, name, len) == 0)
            return it->available ? it->value : "";
    }
    return "";
}

/* One configured line with its readouts filled in, appended to `out` as
 * Pango markup. `escape_text` escapes the line's own words as well as the
 * readouts — the spelling for a line whose markup does not parse. */
static void expand_line(const FwmSidebarFeed *f, const char *line, bool escape_text,
                        GString *out) {
    for (const char *p = line; *p; p++) {
        const char *v = NULL;
        size_t n = 1;
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            n = 2;
        } else if (*p == '{' && strchr(p + 1, '}')) {
            const char *end = strchr(p + 1, '}');
            v = readout(f, p + 1, (size_t)(end - p - 1));
            n = (size_t)(end - p) + 1;
        }
        if (v) {
            /* A command's output is data: an `&` or a `<` in it must not
             * take the line's markup down with it. */
            char *esc = g_markup_escape_text(v, -1);
            g_string_append(out, esc);
            g_free(esc);
        } else if (escape_text) {
            char *esc = g_markup_escape_text(p, 1);
            g_string_append(out, esc);
            g_free(esc);
        } else {
            g_string_append_c(out, *p);
        }
        p += n - 1;
    }
}

/* Lay the lines out again with the readouts as they stand. Returns true when
 * the result differs from what was there. */
static bool compose(FwmSidebarFeed *f) {
    GString *markup = g_string_new(NULL);
    GString *line = g_string_new(NULL);

    for (int l = 0; l < f->line_count; l++) {
        g_string_truncate(line, 0);
        expand_line(f, f->lines[l], false, line);
        /* Checked a line at a time, so "a < b" written as plain words costs
         * that line its markup and not the whole column. */
        if (!pango_parse_markup(line->str, -1, 0, NULL, NULL, NULL, NULL)) {
            g_string_truncate(line, 0);
            expand_line(f, f->lines[l], true, line);
        }
        if (l) g_string_append_c(markup, '\n');
        g_string_append_len(markup, line->str, line->len);
    }
    g_string_free(line, TRUE);

    bool changed = !f->markup || !g_string_equal(f->markup, markup);
    if (f->markup) g_string_free(f->markup, TRUE);
    f->markup = markup;
    return changed;
}

/* Every readout the lines ask for, once each, for the stats engine to sample.
 * Only those: a built-in nobody wrote into a line is not read, and a command
 * nobody uses is not run. */
static void feed_stats_config(const SidebarConfig *cfg, StatsConfig *sc) {
    memset(sc, 0, sizeof(*sc));
    sc->interval = cfg->interval;
    memcpy(sc->custom, cfg->custom, sizeof(sc->custom));
    sc->custom_count = cfg->custom_count;

    for (int l = 0; l < cfg->line_count; l++) {
        for (const char *p = cfg->lines[l]; *p; p++) {
            if (p[0] == '{' && p[1] == '{') { p++; continue; }
            if (*p != '{') continue;
            const char *end = strchr(p + 1, '}');
            if (!end) break;
            size_t n = (size_t)(end - p - 1);
            p = end;
            if (n == 0 || n >= STATS_NAME_MAX) continue;

            int seen = 0;
            for (int i = 0; !seen && i < sc->item_count; i++)
                seen = strlen(sc->items[i]) == n && memcmp(sc->items[i], end - n, n) == 0;
            if (seen || sc->item_count >= STATS_MAX_ITEMS) continue;
            memcpy(sc->items[sc->item_count], end - n, n);
            sc->items[sc->item_count][n] = '\0';
            sc->item_count++;
        }
    }
}

void sidebar_feed_reconfigure(FwmSidebarFeed *f, const SidebarConfig *cfg) {
    if (!f || !cfg) return;
    memcpy(f->lines, cfg->lines, sizeof(f->lines));
    f->line_count = cfg->line_count;

    StatsConfig sc;
    feed_stats_config(cfg, &sc);
    stats_reconfigure(f->stats, &sc);
    compose(f);
}

FwmSidebarFeed *sidebar_feed_create(const SidebarConfig *cfg) {
    FwmSidebarFeed *f = calloc(1, sizeof(*f));
    if (!f) return NULL;

    StatsConfig sc;
    feed_stats_config(cfg, &sc);
    f->stats = stats_create(&sc);
    if (!f->stats) {
        free(f);
        return NULL;
    }
    stats_keep_lines(f->stats);
    sidebar_feed_reconfigure(f, cfg);
    return f;
}

void sidebar_feed_destroy(FwmSidebarFeed *f) {
    if (!f) return;
    stats_destroy(f->stats);
    if (f->markup) g_string_free(f->markup, TRUE);
    free(f);
}

bool sidebar_feed_tick(FwmSidebarFeed *f, double dt) {
    if (!f) return false;
    if (!stats_tick(f->stats, dt)) return false;
    return compose(f);
}

const char *sidebar_feed_markup(const FwmSidebarFeed *f) {
    return f && f->markup ? f->markup->str : "";
}

/* ── a monitor's panel ───────────────────────────────────────────────── */

/* Everything the pixels depend on besides the text, so a panel can tell that
 * what is on screen is already what it would paint. Zeroed before it is
 * filled, so two of them compare with memcmp. */
typedef struct {
    char     font[SIDEBAR_FONT_MAX];
    double   rgba[4];   /* straight, resolved from the config or the palette */
    double   margin;
    int      align;
    int      shadow;
    unsigned theme_gen;
} Look;

struct FwmSidebar {
    struct wlr_scene_tree   *parent;   /* to rebuild the buffer wider */
    struct wlr_scene_buffer *buf;
    /* `w` is the column as it stands; `base_w` the [sidebar] width share of
     * this screen, which it never goes below. In between is what the text
     * needed — see fit_width. */
    int    w, h, base_w;
    int    screen_w, screen_h;
    int    x, y;                       /* the monitor's corner, layout px */
    char  *drawn;       /* the markup on screen, NULL before the first paint */
    Look   drawn_look;
};

static int column_width(const SidebarConfig *cfg, int screen_w) {
    int w = (int)(screen_w * cfg->width + 0.5);
    return w < 1 ? 1 : w;
}

FwmSidebar *sidebar_create(struct wlr_scene_tree *parent, const SidebarConfig *cfg,
                           int screen_w, int screen_h) {
    if (!parent || !cfg || screen_w <= 0 || screen_h <= 0) return NULL;
    FwmSidebar *sb = calloc(1, sizeof(*sb));
    if (!sb) return NULL;
    sb->parent = parent;
    sb->screen_w = screen_w;
    sb->screen_h = screen_h;
    sb->base_w = column_width(cfg, screen_w);
    sb->w = sb->base_w;
    sb->h = screen_h;
    sb->buf = cairo_overlay_create(parent, sb->w, sb->h);
    if (!sb->buf) {
        free(sb);
        return NULL;
    }
    return sb;
}

void sidebar_set_origin(FwmSidebar *sb, int x, int y) {
    if (!sb) return;
    sb->x = x;
    sb->y = y;
    wlr_scene_node_set_position(&sb->buf->node, x + sb->screen_w - sb->w, y);
}

bool sidebar_stale(const FwmSidebar *sb, const SidebarConfig *cfg,
                   int screen_w, int screen_h) {
    if (!sb) return false;
    return sb->screen_w != screen_w || sb->screen_h != screen_h ||
           sb->base_w != column_width(cfg, screen_w);
}

static PangoLayout *make_layout(cairo_t *cr, const Look *lk, const char *markup) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(lk->font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_alignment(layout, lk->align ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    /* Always valid: compose() escaped every line that was not. */
    pango_layout_set_markup(layout, markup, -1);
    return layout;
}

/* How wide the column has to be for its longest line not to wrap.
 *
 * The share alone is a promise only one screen can keep: ten percent of 1920
 * holds a line of `load 2.07 2.33 1.46`, ten percent of 1366 does not, and the
 * same config then reads as a wall on one monitor and as a heap of broken
 * lines on the other. So the share is the column's floor, and a line wider
 * than it widens the column — up to half the screen, past which it wraps. */
static int fit_width(const FwmSidebar *sb, const Look *lk, const char *markup) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = make_layout(cr, lk, markup);
    int tw = 0;
    pango_layout_get_pixel_size(layout, &tw, NULL);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    /* +1 for the shadow, which sits a pixel to the right of the letters. */
    int need = tw + (int)(2.0 * lk->margin + 0.5) + 1;
    int cap = sb->screen_w / 2;
    if (need > cap) need = cap;
    return need > sb->base_w ? need : sb->base_w;
}

typedef struct {
    const Look *look;
    const char *markup;
} Paint;

static void sidebar_paint(cairo_t *cr, int w, int h, void *data) {
    (void)h;
    const Paint *p = data;
    const Look *lk = p->look;

    PangoLayout *layout = make_layout(cr, lk, p->markup);
    /* Only what is still too wide at half the screen wraps (fit_width). */
    double inner = w - 2.0 * lk->margin - 1.0;
    if (inner < 1.0) inner = 1.0;
    pango_layout_set_width(layout, (int)(inner * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    /* Below the tray's islands, which are drawn over this corner. */
    double x = lk->margin;
    double y = TRAY_BOTTOM + lk->margin;

    if (lk->shadow) {
        /* As a path, not a second show_layout: a <span color> would otherwise
         * paint its shadow in its own colour. */
        cairo_move_to(cr, x + 1.0, y + 1.0);
        pango_cairo_layout_path(cr, layout);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, SHADOW_ALPHA * lk->rgba[3]);
        cairo_fill(cr);
    }

    cairo_move_to(cr, x, y);
    cairo_set_source_rgba(cr, lk->rgba[0], lk->rgba[1], lk->rgba[2], lk->rgba[3]);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

void sidebar_draw(FwmSidebar *sb, const SidebarConfig *cfg,
                  const FwmSidebarFeed *feed, const char *output) {
    if (!sb || !cfg) return;

    Look lk;
    memset(&lk, 0, sizeof(lk));
    snprintf(lk.font, sizeof(lk.font), "%s", cfg->font);
    lk.margin = cfg->margin;
    lk.align  = cfg->align;
    lk.shadow = cfg->shadow;
    if (cfg->has_color) {
        /* parse_hex_color premultiplies; cairo wants it straight. */
        double a = cfg->color[3];
        for (int i = 0; i < 3; i++) lk.rgba[i] = a > 0.0 ? cfg->color[i] / a : 0.0;
        lk.rgba[3] = a;
    } else {
        /* The palette's own text colour follows the wallpaper, so it is
         * folded in by generation: a new palette is a repaint. */
        const FwmTheme *thm = theme_get_output(output);
        for (int i = 0; i < 3; i++) lk.rgba[i] = thm->text[i];
        lk.rgba[3] = 1.0;
        lk.theme_gen = theme_generation();
    }

    const char *markup = sidebar_feed_markup(feed);
    if (sb->drawn && strcmp(sb->drawn, markup) == 0 &&
        memcmp(&sb->drawn_look, &lk, sizeof(lk)) == 0)
        return;

    /* Wider when a line needs it, never narrower while the look holds: a
     * readout that loses a digit would otherwise pull the column's left edge
     * — where every line starts — back and forth. A new font or margin
     * starts over from the share. */
    bool same_look = sb->drawn && memcmp(&sb->drawn_look, &lk, sizeof(lk)) == 0;
    int want = fit_width(sb, &lk, markup);
    if (same_look && want < sb->w) want = sb->w;
    if (want != sb->w) {
        struct wlr_scene_buffer *wider = cairo_overlay_create(sb->parent, want, sb->h);
        if (wider) {
            cairo_overlay_destroy(sb->buf);
            sb->buf = wider;
            sb->w = want;
            sidebar_set_origin(sb, sb->x, sb->y);
        }
    }

    Paint p = { &lk, markup };
    cairo_overlay_update(sb->buf, sidebar_paint, &p);

    free(sb->drawn);
    sb->drawn = strdup(markup);
    sb->drawn_look = lk;
}

void sidebar_destroy(FwmSidebar *sb) {
    if (!sb) return;
    cairo_overlay_destroy(sb->buf);
    free(sb->drawn);
    free(sb);
}
