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

#include "expo_time.h"
#include "tray.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "../glass.h"
#include "icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TIME_H         TRAY_HEIGHT  /* button and pill, the tray's own height */
#define TIME_PAD_X     16.0         /* inside the button, past the chevron */
#define TIME_ICON_R    6.0          /* the clock face */
#define TIME_ICON_GAP  8.0          /* between the clock and its label */
#define TIME_EDGE      22           /* px off the screen's bottom and left edge */
#define TAB_PAD_X      18.0         /* either side of a section's name */
#define TAB_INSET      3.0          /* the lit section, inside the pill */
#define PAGE_SIDE      40           /* px between the page and the screen's sides */
#define PAGE_GAP       20           /* between the page and the tray, and the pill */
#define PAGE_PAD       32.0         /* inside the page */
#define PAGE_CUT       12.0         /* corner chevron, matching the other panels */

static const char *const section_name[EXPO_TIME_SECTIONS] = {
    "log", "stats", "timelapse",
};

/* What each section will hold, said on the page until it holds it. */
static const char *const section_about[EXPO_TIME_SECTIONS] = {
    "Every window opened and closed since the computer was switched on, "
    "newest first.",
    "How many windows are open and how many have closed, how long fwm has "
    "been up, and where the time went.",
    "The session played back: the windows travelling the paths they took, "
    "fast enough to watch a day in a minute.",
};

/* Island with pointed ends, the tray's silhouette: these sit along the bottom
 * edge as the tray's islands sit along the top, and should read as the same
 * family. */
static void pill_path(cairo_t *cr, double x, double y, double w, double h) {
    double cut = h / 2.0;
    cairo_new_path(cr);
    cairo_move_to(cr, x + cut, y);
    cairo_line_to(cr, x + w - cut, y);
    cairo_line_to(cr, x + w, y + h / 2.0);
    cairo_line_to(cr, x + w - cut, y + h);
    cairo_line_to(cr, x + cut, y + h);
    cairo_line_to(cr, x, y + h / 2.0);
    cairo_close_path(cr);
}

static void panel_path(cairo_t *cr, double w, double h, double cut) {
    cairo_new_path(cr);
    cairo_move_to(cr, cut, 0);
    cairo_line_to(cr, w - cut, 0);
    cairo_line_to(cr, w, cut);
    cairo_line_to(cr, w, h - cut);
    cairo_line_to(cr, w - cut, h);
    cairo_line_to(cr, cut, h);
    cairo_line_to(cr, 0, h - cut);
    cairo_line_to(cr, 0, cut);
    cairo_close_path(cr);
}

static PangoLayout *make_layout(cairo_t *cr, const char *font) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *d = pango_font_description_from_string(font);
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);
    return l;
}

static void text_size(PangoLayout *l, const char *s, int *w, int *h) {
    pango_layout_set_text(l, s, -1);
    pango_layout_get_pixel_size(l, w, h);
}

/* Width of `s` in `font`, measured off-screen with the fonts the draw uses: the
 * button and the pill are only as wide as what they say. */
static double measure(const char *font, const char *s) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surf);
    PangoLayout *l = make_layout(cr, font);
    int w = 0, h = 0;
    text_size(l, s, &w, &h);
    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return w;
}

static void set_rgb(cairo_t *cr, const double c[3]) {
    cairo_set_source_rgb(cr, c[0], c[1], c[2]);
}

/* ── the button ───────────────────────────────────────────────────────── */

static struct {
    double w;
    bool hover, open;
} g_button;

/* A clock face rather than a letter: the button is the one way in to a view
 * about time, and a face with two hands says so at a glance. */
static void draw_clock(cairo_t *cr, double cx, double cy) {
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, TIME_ICON_R, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx, cy - TIME_ICON_R * 0.62);
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, cx + TIME_ICON_R * 0.48, cy + TIME_ICON_R * 0.22);
    cairo_stroke(cr);
}

static void draw_button(cairo_t *cr, int w, int h, void *data) {
    (void)data;
    const FwmTheme *thm = theme_get();

    /* Open, it is lit in the accent with the label in the island's colour —
     * the tray's own way of saying a pill is pressed. */
    if (g_button.open)
        cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
    else
        cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2],
                              glass_fill(NULL, 0.92));
    pill_path(cr, 0, 0, w, h);
    cairo_fill(cr);

    const double *ink = g_button.open ? thm->pill
                      : g_button.hover ? thm->text : thm->muted;
    set_rgb(cr, ink);
    draw_clock(cr, TIME_PAD_X + TIME_ICON_R, h / 2.0);

    PangoLayout *l = make_layout(cr, "sans 10");
    int tw, th;
    text_size(l, "time", &tw, &th);
    cairo_move_to(cr, TIME_PAD_X + 2 * TIME_ICON_R + TIME_ICON_GAP, (h - th) / 2.0);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static void button_xy(int ox, int oy, int sh, double reveal, int *x, int *y) {
    if (reveal < 0.0) reveal = 0.0;
    if (reveal > 1.0) reveal = 1.0;
    /* Hidden means entirely off the bottom, the way the key hints go. */
    double home = sh - TIME_H - TIME_EDGE;
    double gone = sh + 2.0;
    *x = ox + TIME_EDGE;
    *y = oy + (int)(gone + (home - gone) * reveal);
}

struct wlr_scene_buffer *expo_time_button_show(struct wlr_scene_tree *parent,
                                               int ox, int oy, int sw, int sh) {
    (void)sw;
    g_button.w = TIME_PAD_X + 2 * TIME_ICON_R + TIME_ICON_GAP
               + measure("sans 10", "time") + TIME_PAD_X;
    g_button.hover = false;
    g_button.open = false;

    struct wlr_scene_buffer *buf =
        cairo_overlay_create(parent, (int)ceil(g_button.w), TIME_H);
    if (!buf) return NULL;
    cairo_overlay_update(buf, draw_button, NULL);
    glass_attach(buf);
    expo_time_button_place(buf, ox, oy, sw, sh, 1.0);
    return buf;
}

void expo_time_button_place(struct wlr_scene_buffer *buf, int ox, int oy,
                            int sw, int sh, double reveal) {
    (void)sw;
    if (!buf) return;
    int x, y;
    button_xy(ox, oy, sh, reveal, &x, &y);
    wlr_scene_node_set_position(&buf->node, x, y);
}

void expo_time_button_set(struct wlr_scene_buffer *buf, bool hover, bool open) {
    if (!buf || (g_button.hover == hover && g_button.open == open)) return;
    g_button.hover = hover;
    g_button.open = open;
    cairo_overlay_update(buf, draw_button, NULL);
}

bool expo_time_button_hit(struct wlr_scene_buffer *buf, double lx, double ly) {
    if (!buf || !buf->node.enabled) return false;
    double x = lx - buf->node.x, y = ly - buf->node.y;
    return x >= 0 && x < g_button.w && y >= 0 && y < TIME_H;
}

/* ── the section pill ─────────────────────────────────────────────────── */

static struct {
    double seg_x[EXPO_TIME_SECTIONS], seg_w[EXPO_TIME_SECTIONS];
    double w;
    double pos;      /* where the lit section is, fractional while it travels */
    int hover;
} g_tabs;


static void draw_tabs(cairo_t *cr, int w, int h, void *data) {
    (void)data;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2],
                          glass_fill(NULL, 0.92));
    pill_path(cr, 0, 0, w, h);
    cairo_fill(cr);

    /* The lit section is one piece of accent that travels: between two
     * sections it is part way along and part way between their widths, so a
     * change of section is seen going from one to the other. */
    double pos = g_tabs.pos;
    if (pos < 0.0) pos = 0.0;
    if (pos > EXPO_TIME_SECTIONS - 1) pos = EXPO_TIME_SECTIONS - 1;
    int a = (int)floor(pos), b = a + 1 < EXPO_TIME_SECTIONS ? a + 1 : a;
    double f = pos - a;
    double lx = g_tabs.seg_x[a] + (g_tabs.seg_x[b] - g_tabs.seg_x[a]) * f;
    double lw = g_tabs.seg_w[a] + (g_tabs.seg_w[b] - g_tabs.seg_w[a]) * f;
    cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
    pill_path(cr, lx, TAB_INSET, lw, h - 2 * TAB_INSET);
    cairo_fill(cr);

    /* The names twice: in their own colours, then again in the island's
     * colour clipped to the accent. Where the accent is under a letter the
     * letter is dark and where it is not it is light, so a name the accent is
     * passing over is cut cleanly by its edge instead of going grey all over. */
    PangoLayout *l = make_layout(cr, "sans 10");
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            cairo_save(cr);
            pill_path(cr, lx, TAB_INSET, lw, h - 2 * TAB_INSET);
            cairo_clip(cr);
        }
        for (int s = 0; s < EXPO_TIME_SECTIONS; s++) {
            double x = g_tabs.seg_x[s], sw = g_tabs.seg_w[s];
            set_rgb(cr, pass ? thm->pill : s == g_tabs.hover ? thm->text : thm->muted);
            int tw, th;
            text_size(l, section_name[s], &tw, &th);
            cairo_move_to(cr, x + (sw - tw) / 2.0, (h - th) / 2.0);
            pango_cairo_show_layout(cr, l);
        }
        if (pass) cairo_restore(cr);
    }
    g_object_unref(l);
}

static void tabs_xy(int ox, int oy, int sw, int sh, int *x, int *y) {
    *x = ox + (int)((sw - g_tabs.w) / 2.0);
    *y = oy + sh - TIME_H - TIME_EDGE;
}

struct wlr_scene_buffer *expo_time_tabs_show(struct wlr_scene_tree *parent,
                                             int ox, int oy, int sw, int sh,
                                             int section) {
    /* Each section is its name plus room either side, and the pill's own
     * pointed ends are kept clear of the first and the last. */
    double x = TIME_H / 2.0;
    for (int s = 0; s < EXPO_TIME_SECTIONS; s++) {
        g_tabs.seg_x[s] = x;
        g_tabs.seg_w[s] = TAB_PAD_X + measure("sans 10", section_name[s]) + TAB_PAD_X;
        x += g_tabs.seg_w[s];
    }
    g_tabs.w = x + TIME_H / 2.0;
    g_tabs.pos = section;
    g_tabs.hover = EXPO_TIME_NONE;

    struct wlr_scene_buffer *buf =
        cairo_overlay_create(parent, (int)ceil(g_tabs.w), TIME_H);
    if (!buf) return NULL;
    cairo_overlay_update(buf, draw_tabs, NULL);
    glass_attach(buf);
    int px, py;
    tabs_xy(ox, oy, sw, sh, &px, &py);
    wlr_scene_node_set_position(&buf->node, px, py);
    return buf;
}

void expo_time_tabs_set(struct wlr_scene_buffer *buf, double pos, int hover) {
    if (!buf || (fabs(g_tabs.pos - pos) < 0.001 && g_tabs.hover == hover)) return;
    g_tabs.pos = pos;
    g_tabs.hover = hover;
    cairo_overlay_update(buf, draw_tabs, NULL);
}

int expo_time_tabs_hit(struct wlr_scene_buffer *buf, double lx, double ly) {
    if (!buf) return EXPO_TIME_NONE;
    double x = lx - buf->node.x, y = ly - buf->node.y;
    if (y < 0 || y >= TIME_H) return EXPO_TIME_NONE;
    /* The pointed ends belong to the section next to them: aiming at the tip
     * of the pill is aiming at the first or the last. */
    if (x < 0 || x >= g_tabs.w) return EXPO_TIME_NONE;
    for (int s = 0; s < EXPO_TIME_SECTIONS; s++)
        if (x < g_tabs.seg_x[s] + g_tabs.seg_w[s]) return s;
    return EXPO_TIME_SECTIONS - 1;
}

/* ── the page ─────────────────────────────────────────────────────────── */

/* Two layers: the frosted panel, drawn once, and over it the words of one
 * section on a transparent layer of their own. A change of section is then
 * the words moving and fading — a scene node's position and opacity, nothing
 * redrawn — while the glass under them stands still. */

/* The keys, in the strip's hint style: the key in the accent, what it does in
 * the muted colour. */
static double draw_key(cairo_t *cr, PangoLayout *key, PangoLayout *lab,
                       double x, double y, const char *k, const char *what) {
    const FwmTheme *thm = theme_get();
    int w, h;
    set_rgb(cr, thm->accent);
    text_size(key, k, &w, &h);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, key);
    x += w + 6.0;
    set_rgb(cr, thm->muted);
    text_size(lab, what, &w, &h);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, lab);
    return x + w + 14.0;
}

static void draw_page(cairo_t *cr, int w, int h, void *data) {
    (void)data;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2],
                          glass_fill(NULL, 0.92));
    panel_path(cr, w, h, PAGE_CUT);
    cairo_fill(cr);

    /* The keys belong to the page, not to a section: they stay put while the
     * sections slide past above them. */
    PangoLayout *key = make_layout(cr, "sans bold 9");
    PangoLayout *lab = make_layout(cr, "sans 9");
    int tw, kh;
    text_size(lab, "Ag", &tw, &kh);
    double ky = h - PAGE_PAD + 8.0 - kh;
    double kx = PAGE_PAD;
    kx = draw_key(cr, key, lab, kx, ky, "\xE2\x86\x90 \xE2\x86\x92", "section");  /* ← → */
    kx = draw_key(cr, key, lab, kx, ky, "1 2 3", "jump");
    kx = draw_key(cr, key, lab, kx, ky, "Esc", "back to the strip");
    (void)kx;
    g_object_unref(key);
    g_object_unref(lab);
}

/* ── the log ──────────────────────────────────────────────────────────── */

#define LIST_TOP      124.0   /* below the section's title and what it says */
#define LIST_BOTTOM   60.0    /* above the keys along the bottom of the page */
#define ROW_H         34.0
#define TIME_COL      96.0    /* "14:32", or "Mon 14:32" for an earlier day */
#define ICON_SZ       20
#define VERB_COL      70.0
#define STATE_COL     190.0

/* Icons by app_id, looked up once each: finding one walks the icon theme on
 * disk, and the list is redrawn on every turn of the wheel. Kept for the life
 * of the compositor — a few dozen small pictures. A miss is remembered too, so
 * an application with no icon is not searched for on every redraw. */
#define ICON_CACHE 48
static struct {
    char app[64];
    cairo_surface_t *surf;
} g_icons[ICON_CACHE];
static int g_icons_n, g_icons_next;

static cairo_surface_t *icon_try(const char *name, const char *theme) {
    if (!name || !name[0]) return NULL;
    cairo_surface_t *s = icon_load(name, theme, ICON_SZ);
    if (s) return s;
    /* Most applications name their icon after their app_id, but not always
     * in the same case, and reverse-DNS ids often name it after their last
     * part: org.gnome.Nautilus, but also plain nautilus. */
    char low[64];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(low); i++)
        low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
    low[i] = '\0';
    if (strcmp(low, name) != 0 && (s = icon_load(low, theme, ICON_SZ))) return s;
    const char *dot = strrchr(low, '.');
    if (dot && dot[1]) return icon_load(dot + 1, theme, ICON_SZ);
    return NULL;
}

static cairo_surface_t *icon_for(const char *app, const char *theme) {
    if (!app || !app[0]) return NULL;
    for (int i = 0; i < g_icons_n; i++)
        if (strcmp(g_icons[i].app, app) == 0) return g_icons[i].surf;
    int slot = g_icons_n < ICON_CACHE ? g_icons_n++ : g_icons_next;
    if (g_icons_n == ICON_CACHE) g_icons_next = (slot + 1) % ICON_CACHE;
    if (g_icons[slot].surf) cairo_surface_destroy(g_icons[slot].surf);
    snprintf(g_icons[slot].app, sizeof(g_icons[slot].app), "%s", app);
    g_icons[slot].surf = icon_try(app, theme);
    return g_icons[slot].surf;
}

/* No icon: the first letter of the application, on a disc. */
static void draw_letter(cairo_t *cr, PangoLayout *l, double x, double y,
                        const char *name) {
    const FwmTheme *thm = theme_get();
    cairo_new_path(cr);
    cairo_arc(cr, x + ICON_SZ / 2.0, y + ICON_SZ / 2.0, ICON_SZ / 2.0, 0, 2 * M_PI);
    set_rgb(cr, thm->sel);
    cairo_fill(cr);
    char letter[8] = "?";
    if (name && name[0]) {
        /* One character, whole: it may well be more than one byte. */
        size_t n = 1;
        while (name[n] && ((unsigned char)name[n] & 0xC0) == 0x80 && n < 4) n++;
        memcpy(letter, name, n);
        letter[n] = '\0';
        if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] = (char)(letter[0] - 'a' + 'A');
    }
    int tw, th;
    text_size(l, letter, &tw, &th);
    set_rgb(cr, thm->muted);
    cairo_move_to(cr, x + (ICON_SZ - tw) / 2.0, y + (ICON_SZ - th) / 2.0);
    pango_cairo_show_layout(cr, l);
}

static void format_clock(int64_t ms, int64_t now_ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000), now = (time_t)(now_ms / 1000);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    /* The day only when it is not today: a log of one afternoon should not
     * say "Saturday" twenty times. */
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year)
        strftime(out, cap, "%H:%M", &a);
    else
        strftime(out, cap, "%a %H:%M", &a);
}

static void format_span(int64_t ms, char *out, size_t cap) {
    long s = (long)(ms / 1000);
    if (s < 60)        snprintf(out, cap, "lived %lds", s);
    else if (s < 3600) snprintf(out, cap, "lived %ld min", s / 60);
    else               snprintf(out, cap, "lived %ldh %02ldm", s / 3600, (s / 60) % 60);
}

static void draw_log(cairo_t *cr, int w, int h, const ExpoTimeLog *log) {
    const FwmTheme *thm = theme_get();
    int tw, th;

    if (!log || log->total == 0) {
        PangoLayout *empty = make_layout(cr, "sans 12");
        set_rgb(cr, thm->dim);
        text_size(empty, "nothing recorded yet", &tw, &th);
        cairo_move_to(cr, (w - tw) / 2.0, (h - th) / 2.0);
        pango_cairo_show_layout(cr, empty);
        g_object_unref(empty);
        return;
    }

    PangoLayout *small = make_layout(cr, "sans 10");
    PangoLayout *name = make_layout(cr, "sans 11");
    PangoLayout *letter = make_layout(cr, "sans bold 9");
    double title_x = PAGE_PAD + TIME_COL + ICON_SZ + 12.0 + VERB_COL;
    double title_w = w - PAGE_PAD - STATE_COL - title_x;
    if (title_w < 40.0) title_w = 40.0;
    pango_layout_set_width(name, (int)(title_w * PANGO_SCALE));
    pango_layout_set_ellipsize(name, PANGO_ELLIPSIZE_END);

    for (int i = 0; i < log->n; i++) {
        const ExpoTimeRow *r = &log->rows[i];
        double y = LIST_TOP + i * ROW_H;
        double mid = y + ROW_H / 2.0;

        /* A hairline between rows, so a long list can be followed across. */
        if (i) {
            cairo_set_source_rgba(cr, thm->sel[0], thm->sel[1], thm->sel[2], 0.8);
            cairo_rectangle(cr, PAGE_PAD, y, w - 2 * PAGE_PAD, 1);
            cairo_fill(cr);
        }

        char buf[64];
        format_clock(r->ms, log->now_ms, buf, sizeof(buf));
        set_rgb(cr, thm->muted);
        text_size(small, buf, &tw, &th);
        cairo_move_to(cr, PAGE_PAD, mid - th / 2.0);
        pango_cairo_show_layout(cr, small);

        double x = PAGE_PAD + TIME_COL;
        cairo_surface_t *icon = icon_for(r->app_id, log->icon_theme);
        if (icon) {
            int iw = cairo_image_surface_get_width(icon);
            int ih = cairo_image_surface_get_height(icon);
            cairo_set_source_surface(cr, icon, x + (ICON_SZ - iw) / 2.0,
                                     mid - ih / 2.0);
            cairo_paint(cr);
        } else {
            draw_letter(cr, letter, x, mid - ICON_SZ / 2.0,
                        r->app_id && r->app_id[0] ? r->app_id : r->title);
        }
        x += ICON_SZ + 12.0;

        /* Opening in the accent and closing in the muted colour: the eye
         * reads the comings by colour and the goings by their absence. */
        set_rgb(cr, r->closed ? thm->muted : thm->accent);
        text_size(small, r->closed ? "closed" : "opened", &tw, &th);
        cairo_move_to(cr, x, mid - th / 2.0);
        pango_cairo_show_layout(cr, small);

        set_rgb(cr, thm->text);
        const char *label = r->title && r->title[0] ? r->title
                          : (r->app_id && r->app_id[0] ? r->app_id : "a window");
        text_size(name, label, &tw, &th);
        cairo_move_to(cr, title_x, mid - th / 2.0);
        pango_cairo_show_layout(cr, name);

        /* What became of it, right-aligned: open still, or how long it was. */
        if (r->closed) {
            format_span(r->lived_ms, buf, sizeof(buf));
            set_rgb(cr, thm->muted);
        } else if (r->still_open) {
            snprintf(buf, sizeof(buf), "open");
            set_rgb(cr, thm->accent);
        } else {
            snprintf(buf, sizeof(buf), "closed");
            set_rgb(cr, thm->dim);
        }
        text_size(small, buf, &tw, &th);
        double sx = w - PAGE_PAD - tw;
        cairo_move_to(cr, sx, mid - th / 2.0);
        pango_cairo_show_layout(cr, small);
        if (!r->closed && r->still_open) {
            cairo_new_path(cr);
            cairo_arc(cr, sx - 9.0, mid, 3.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    /* Where in the list this is, when there is more of it than fits. */
    if (log->total > log->n) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d\xE2\x80\x93%d of %d", log->first + 1,
                 log->first + log->n, log->total);
        set_rgb(cr, thm->muted);
        text_size(small, buf, &tw, &th);
        cairo_move_to(cr, w - PAGE_PAD - tw, h - PAGE_PAD + 8.0 - th);
        pango_cairo_show_layout(cr, small);
    }

    g_object_unref(small);
    g_object_unref(name);
    g_object_unref(letter);
}

int expo_time_log_fit(struct wlr_scene_buffer *page) {
    struct wlr_buffer *pix = page ? cairo_overlay_buffer(page) : NULL;
    if (!pix) return 0;
    int n = (int)((pix->height - LIST_TOP - LIST_BOTTOM) / ROW_H);
    return n > 0 ? n : 0;
}

struct ContentCtx {
    int section;
    const ExpoTimeLog *log;
};

static void draw_content(cairo_t *cr, int w, int h, void *data) {
    const FwmTheme *thm = theme_get();
    const struct ContentCtx *ctx = data;
    int s = ctx->section;
    if (s < 0 || s >= EXPO_TIME_SECTIONS) s = EXPO_TIME_LOG;

    int tw, th;
    PangoLayout *title = make_layout(cr, "sans bold 20");
    set_rgb(cr, thm->text);
    text_size(title, section_name[s], &tw, &th);
    cairo_move_to(cr, PAGE_PAD, PAGE_PAD);
    pango_cairo_show_layout(cr, title);
    double y = PAGE_PAD + th + 10.0;
    g_object_unref(title);

    PangoLayout *body = make_layout(cr, "sans 11");
    pango_layout_set_width(body, (int)((w - 2 * PAGE_PAD) * PANGO_SCALE));
    pango_layout_set_wrap(body, PANGO_WRAP_WORD);
    set_rgb(cr, thm->muted);
    text_size(body, section_about[s], &tw, &th);
    cairo_move_to(cr, PAGE_PAD, y);
    pango_cairo_show_layout(cr, body);
    g_object_unref(body);

    if (s == EXPO_TIME_LOG) {
        draw_log(cr, w, h, ctx->log);
        return;
    }

    /* Nothing is recorded for this one yet, and the middle of the page says so
     * rather than standing empty: an empty page reads as one that failed to
     * load. */
    PangoLayout *empty = make_layout(cr, "sans 12");
    set_rgb(cr, thm->dim);
    text_size(empty, "nothing recorded yet", &tw, &th);
    cairo_move_to(cr, (w - tw) / 2.0, (h - th) / 2.0);
    pango_cairo_show_layout(cr, empty);
    g_object_unref(empty);
}

struct wlr_scene_buffer *expo_time_page_show(struct wlr_scene_tree *parent,
                                             int ox, int oy, int sw, int sh) {
    /* Between the tray and the pill, so neither is covered: the tray goes on
     * working while the strip is up, and the pill is how the page is driven. */
    int top = TRAY_BOTTOM + PAGE_GAP;
    int bottom = TIME_EDGE + TIME_H + PAGE_GAP;
    int w = sw - 2 * PAGE_SIDE, h = sh - top - bottom;
    if (w < 200 || h < 120) return NULL;

    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, w, h);
    if (!buf) return NULL;
    cairo_overlay_update(buf, draw_page, NULL);
    glass_attach(buf);
    wlr_scene_node_set_position(&buf->node, ox + PAGE_SIDE, oy + top);
    return buf;
}

struct wlr_scene_buffer *expo_time_content_show(struct wlr_scene_tree *parent,
                                                struct wlr_scene_buffer *page,
                                                int section, const ExpoTimeLog *log) {
    struct wlr_buffer *pix = page ? cairo_overlay_buffer(page) : NULL;
    if (!pix) return NULL;
    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, pix->width, pix->height);
    if (!buf) return NULL;
    struct ContentCtx ctx = { .section = section, .log = log };
    cairo_overlay_update(buf, draw_content, &ctx);
    wlr_scene_node_set_position(&buf->node, page->node.x, page->node.y);
    return buf;
}

void expo_time_content_redraw(struct wlr_scene_buffer *buf, int section,
                              const ExpoTimeLog *log) {
    if (!buf) return;
    struct ContentCtx ctx = { .section = section, .log = log };
    cairo_overlay_update(buf, draw_content, &ctx);
}
