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

/* The liquid form. See liquid_form.h. */

#include "liquid_form.h"
#include "cairo_overlay.h"
#include "modes.h"
#include "../theme.h"
#include "../glass.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── geometry ──────────────────────────────────────────────────────────── */

#define FORM_W      344
#define PAD         14.0
#define LABEL_W     82.0
#define FIELD_X     (PAD + LABEL_W)
#define RIGHT       (FORM_W - PAD)
#define FIELD_H     24.0
#define HEADER_H    32.0
#define ROW_H       34.0
#define WHEEL_D     124.0
#define WHEEL_H     (WHEEL_D + 12.0)
#define SLIDER_ROW  46.0
#define ERROR_H     18.0
#define BUTTONS_H   36.0
#define NUM_W       72.0
#define BTN_W       88.0
#define BTN_H       26.0
#define BAR_W       16.0

#define Y_NAME    (PAD + HEADER_H)
#define Y_HEX     (Y_NAME + ROW_H)
#define Y_WHEEL   (Y_HEX + ROW_H)
#define Y_VISC    (Y_WHEEL + WHEEL_H)
#define Y_DENS    (Y_VISC + SLIDER_ROW)
#define Y_ERROR   (Y_DENS + SLIDER_ROW)
#define Y_BUTTONS (Y_ERROR + ERROR_H)
#define FORM_H    ((int)(Y_BUTTONS + BUTTONS_H + PAD))

#define WHEEL_CX  (FIELD_X + WHEEL_D / 2.0)
#define WHEEL_CY  (Y_WHEEL + 4.0 + WHEEL_D / 2.0)
#define BAR_X     (FIELD_X + WHEEL_D + 18.0)
#define BAR_Y     (Y_WHEEL + 4.0)
#define SLIDE_X0  (FIELD_X + NUM_W + 16.0)
#define SLIDE_X1  (RIGHT - 8.0)

enum { DRAG_NONE = 0, DRAG_WHEEL, DRAG_VALUE, DRAG_VISC, DRAG_DENS };

static const double COL_ERROR[3] = { 0.93, 0.42, 0.38 };

void liquid_form_size(int *w, int *h) {
    if (w) *w = FORM_W;
    if (h) *h = FORM_H;
}

/* ── values ────────────────────────────────────────────────────────────── */

/* Both sliders are logarithmic: water and honey are three orders of
 * magnitude apart, oil and mercury one and a half, and on a straight scale
 * everything anyone pours would be crammed into the first tenth of it. */
static double visc_to_t(double v) { return log10(v + 1.0) / log10(LIQUID_VISCOSITY_MAX + 1.0); }
static double visc_from_t(double t) { return pow(LIQUID_VISCOSITY_MAX + 1.0, t) - 1.0; }
static double dens_to_t(double d) {
    return log(d / LIQUID_DENSITY_MIN) / log(LIQUID_DENSITY_MAX / LIQUID_DENSITY_MIN);
}
static double dens_from_t(double t) {
    return LIQUID_DENSITY_MIN * pow(LIQUID_DENSITY_MAX / LIQUID_DENSITY_MIN, t);
}

/* Two significant figures: a slider dragged to 2873.4 means "about three
 * thousand", and that is the number to show and keep. */
static double round2(double v) {
    if (v <= 0.0) return 0.0;
    double p = pow(10.0, floor(log10(v)) - 1.0);
    return round(v / p) * p;
}

static void fmt_visc(char *out, size_t cap, double v) {
    if (v >= 10.0) snprintf(out, cap, "%.0f", v);
    else           snprintf(out, cap, "%.1f", v);
}

static void fmt_dens(char *out, size_t cap, double v) {
    snprintf(out, cap, v >= 10.0 ? "%.1f" : "%.2f", v);
}

static void hsv_to_rgb(double h, double s, double v, float rgb[3]) {
    double hh = fmod(h, 1.0) * 6.0;
    if (hh < 0.0) hh += 6.0;
    int i = (int)hh;
    double f = hh - i, p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
    double r, g, b;
    switch (i) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    rgb[0] = (float)r; rgb[1] = (float)g; rgb[2] = (float)b;
}

static void rgb_to_hsv(const float rgb[3], double *h, double *s, double *v) {
    double r = rgb[0], g = rgb[1], b = rgb[2];
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0.0 ? d / mx : 0.0;
    if (d <= 0.0) { *h = 0.0; return; }
    double hh;
    if (mx == r)      hh = fmod((g - b) / d, 6.0);
    else if (mx == g) hh = (b - r) / d + 2.0;
    else              hh = (r - g) / d + 4.0;
    hh /= 6.0;
    if (hh < 0.0) hh += 1.0;
    *h = hh;
}

static bool parse_hex(const char *s, float rgb[3]) {
    if (!s || s[0] != '#') return false;
    size_t len = strlen(s + 1);
    if (len != 6 && len != 8) return false;
    for (int i = 0; i < 3; i++) {
        char buf[3] = { s[1 + 2 * i], s[2 + 2 * i], 0 };
        char *end;
        unsigned v = (unsigned)strtoul(buf, &end, 16);
        if (*end) return false;
        rgb[i] = (float)v / 255.0f;
    }
    for (size_t i = 7; i <= len; i++)
        if (!strchr("0123456789abcdefABCDEF", s[i])) return false;
    return true;
}

void liquid_form_rgb(const LiquidForm *f, float rgb[3]) {
    hsv_to_rgb(f->hue, f->sat, f->val, rgb);
}

static void sync_hex(LiquidForm *f) {
    float rgb[3];
    liquid_form_rgb(f, rgb);
    snprintf(f->text[LIQUID_FORM_HEX], sizeof(f->text[0]), "#%02X%02X%02X",
             (unsigned)lroundf(rgb[0] * 255.0f), (unsigned)lroundf(rgb[1] * 255.0f),
             (unsigned)lroundf(rgb[2] * 255.0f));
}

/* A number as typed, with a comma taken for the decimal point it means. */
static bool parse_num(const char *s, double *out) {
    char buf[LIQUID_NAME_MAX];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = buf; *p; p++) if (*p == ',') *p = '.';
    char *end;
    double v = strtod(buf, &end);
    if (end == buf || *end || !isfinite(v)) return false;
    *out = v;
    return true;
}

void liquid_form_init(LiquidForm *f, const LiquidKind *from, bool editing) {
    memset(f, 0, sizeof(*f));
    f->editing = editing;
    f->focus = editing ? -1 : LIQUID_FORM_NAME;
    snprintf(f->text[LIQUID_FORM_NAME], sizeof(f->text[0]), "%s", editing ? from->name : "");
    rgb_to_hsv(from->color, &f->hue, &f->sat, &f->val);
    f->alpha = from->color[3];
    sync_hex(f);
    f->viscosity = from->viscosity;
    f->density = from->density;
    fmt_visc(f->text[LIQUID_FORM_VISC], sizeof(f->text[0]), f->viscosity);
    fmt_dens(f->text[LIQUID_FORM_DENS], sizeof(f->text[0]), f->density);
}

void liquid_form_result(const LiquidForm *f, LiquidKind *out) {
    memset(out, 0, sizeof(*out));
    /* Spaces at either end are not part of a name. */
    const char *s = f->text[LIQUID_FORM_NAME];
    while (*s == ' ') s++;
    snprintf(out->name, sizeof(out->name), "%s", s);
    size_t n = strlen(out->name);
    while (n > 0 && out->name[n - 1] == ' ') out->name[--n] = '\0';
    liquid_form_rgb(f, out->color);
    out->color[3] = f->alpha;
    out->viscosity = f->viscosity;
    out->density = f->density;
}

/* Everything a submit needs, said in the form if it is missing. */
static bool validate(LiquidForm *f) {
    LiquidKind k;
    liquid_form_result(f, &k);
    double v, d;
    float rgb[3];
    if (!k.name[0])
        snprintf(f->error, sizeof f->error, "Give it a name");
    else if (!parse_hex(f->text[LIQUID_FORM_HEX], rgb))
        snprintf(f->error, sizeof f->error, "Colour is #RRGGBB");
    else if (!parse_num(f->text[LIQUID_FORM_VISC], &v) || v < 0.0 || v > LIQUID_VISCOSITY_MAX)
        snprintf(f->error, sizeof f->error, "Viscosity is 0 to %.0f", LIQUID_VISCOSITY_MAX);
    else if (!parse_num(f->text[LIQUID_FORM_DENS], &d) || d < LIQUID_DENSITY_MIN || d > LIQUID_DENSITY_MAX)
        snprintf(f->error, sizeof f->error, "Density is %.1f to %.0f", LIQUID_DENSITY_MIN, LIQUID_DENSITY_MAX);
    else {
        f->error[0] = '\0';
        f->viscosity = v;
        f->density = d;
        return true;
    }
    return false;
}

/* ── drawing ───────────────────────────────────────────────────────────── */

static void set_font(PangoLayout *layout, const char *font) {
    PangoFontDescription *desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
}

static void text_at(cairo_t *cr, PangoLayout *layout, const char *s,
                    double x, double y, double h, int *w_out) {
    int tw, th;
    pango_layout_set_text(layout, s, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x, y + (h - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    /* show_layout leaves the move_to's point as the current point, and the
     * next arc drawn — the wheel's marker — was joined to it by a straight
     * line, a stripe from the hex field to wherever the colour was picked. */
    cairo_new_path(cr);
    if (w_out) *w_out = tw;
}

static void draw_field(cairo_t *cr, PangoLayout *layout, double x, double y, double w,
                       const char *text, bool focused, const char *placeholder) {
    const FwmTheme *thm = theme_get();
    cairo_set_source_rgba(cr, thm->dim[0], thm->dim[1], thm->dim[2], 0.35);
    cairo_rectangle(cr, x, y, w, FIELD_H);
    cairo_fill(cr);
    if (focused) {
        cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, x + 0.75, y + 0.75, w - 1.5, FIELD_H - 1.5);
        cairo_stroke(cr);
    }
    cairo_save(cr);
    cairo_rectangle(cr, x + 4, y, w - 8, FIELD_H);
    cairo_clip(cr);
    int tw = 0;
    if (text[0]) {
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        text_at(cr, layout, text, x + 8, y, FIELD_H, &tw);
    } else if (placeholder && !focused) {
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        text_at(cr, layout, placeholder, x + 8, y, FIELD_H, NULL);
    }
    if (focused) {
        cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
        cairo_rectangle(cr, x + 8 + tw + 1, y + 5, 1.5, FIELD_H - 10);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

/* The hue around the wheel and the saturation out from its centre, at the
 * brightness the bar beside it is set to. */
static void draw_wheel(cairo_t *cr, const LiquidForm *f) {
    int d = (int)WHEEL_D;
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, d, d);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) return;
    unsigned char *px = cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img);
    double r = d / 2.0;
    for (int y = 0; y < d; y++) {
        uint32_t *row = (uint32_t *)(px + y * stride);
        for (int x = 0; x < d; x++) {
            double dx = x + 0.5 - r, dy = y + 0.5 - r;
            double dist = sqrt(dx * dx + dy * dy);
            /* A pixel of antialiasing at the rim. */
            double a = fmin(1.0, fmax(0.0, r - dist));
            if (a <= 0.0) { row[x] = 0; continue; }
            double h = atan2(-dy, dx) / (2.0 * M_PI);
            if (h < 0.0) h += 1.0;
            float rgb[3];
            hsv_to_rgb(h, fmin(1.0, dist / r), f->val, rgb);
            unsigned A = (unsigned)lround(a * 255.0);
            row[x] = (A << 24)
                   | ((unsigned)lround(rgb[0] * a * 255.0) << 16)
                   | ((unsigned)lround(rgb[1] * a * 255.0) << 8)
                   |  (unsigned)lround(rgb[2] * a * 255.0);
        }
    }
    cairo_surface_mark_dirty(img);
    cairo_set_source_surface(cr, img, WHEEL_CX - r, WHEEL_CY - r);
    cairo_paint(cr);
    cairo_surface_destroy(img);

    cairo_new_path(cr);
    double ang = f->hue * 2.0 * M_PI;
    double mx = WHEEL_CX + cos(ang) * f->sat * r, my = WHEEL_CY - sin(ang) * f->sat * r;
    cairo_set_line_width(cr, 2.5);
    cairo_set_source_rgb(cr, 0.06, 0.07, 0.09);
    cairo_arc(cr, mx, my, 5.0, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_arc(cr, mx, my, 5.0, 0, 2 * M_PI);
    cairo_stroke(cr);

    /* The brightness, top bright to bottom black. */
    float top[3];
    hsv_to_rgb(f->hue, f->sat, 1.0, top);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, BAR_Y, 0, BAR_Y + WHEEL_D);
    cairo_pattern_add_color_stop_rgb(g, 0.0, top[0], top[1], top[2]);
    cairo_pattern_add_color_stop_rgb(g, 1.0, 0, 0, 0);
    cairo_set_source(cr, g);
    cairo_rectangle(cr, BAR_X, BAR_Y, BAR_W, WHEEL_D);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    double by = BAR_Y + (1.0 - f->val) * WHEEL_D;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, BAR_X - 3, by - 1.5, BAR_W + 6, 3);
    cairo_fill(cr);
}

static void draw_slider(cairo_t *cr, PangoLayout *layout, double y, double t,
                        const char *left, const char *right, double mark) {
    const FwmTheme *thm = theme_get();
    double cy = y + 2.0 + FIELD_H / 2.0;
    double x0 = SLIDE_X0, x1 = SLIDE_X1;
    cairo_set_source_rgba(cr, thm->dim[0], thm->dim[1], thm->dim[2], 0.6);
    cairo_rectangle(cr, x0, cy - 2, x1 - x0, 4);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
    cairo_rectangle(cr, x0, cy - 2, (x1 - x0) * t, 4);
    cairo_fill(cr);
    /* Water's place on the scale, so a liquid reads as lighter or heavier
     * than something everyone knows. */
    if (mark >= 0.0) {
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        cairo_rectangle(cr, x0 + (x1 - x0) * mark - 0.75, cy + 4, 1.5, 5);
        cairo_fill(cr);
    }
    /* The knob: the modes switch's diamond. */
    double kx = x0 + (x1 - x0) * t;
    cairo_save(cr);
    cairo_translate(cr, kx, cy);
    cairo_rotate(cr, M_PI / 4.0);
    cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
    cairo_rectangle(cr, -5, -5, 10, 10);
    cairo_fill(cr);
    cairo_restore(cr);

    set_font(layout, "sans 7");
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    int tw, th;
    pango_layout_set_text(layout, left, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x0, cy + 9);
    pango_cairo_show_layout(cr, layout);
    pango_layout_set_text(layout, right, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x1 - tw, cy + 9);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
    set_font(layout, "sans 10");
}

static void draw_button(cairo_t *cr, PangoLayout *layout, double x, double y,
                        const char *label, bool primary, bool hover) {
    const FwmTheme *thm = theme_get();
    modes_panel_path(cr, x, y, BTN_W, BTN_H, 5.0);
    if (primary) {
        cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2], hover ? 1.0 : 0.85);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, thm->pill[0], thm->pill[1], thm->pill[2]);
    } else {
        cairo_set_source_rgba(cr, thm->dim[0], thm->dim[1], thm->dim[2], hover ? 0.7 : 0.4);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
    }
    int tw, th;
    pango_layout_set_text(layout, label, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x + (BTN_W - tw) / 2.0, y + (BTN_H - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
}

static void draw_form(cairo_t *cr, int w, int h, void *user) {
    const LiquidForm *f = user;
    const FwmTheme *thm = theme_get();

    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], f->opacity);
    modes_panel_path(cr, 0, 0, w, h, MODES_MENU_CHAMFER);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);

    /* The drop at the top is the liquid as it stands: its colour is the one
     * being picked. */
    float rgb[3];
    liquid_form_rgb(f, rgb);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    modes_icon(cr, MODE_ICON_LIQUID, PAD, PAD + (HEADER_H - 20.0) / 2.0, 20.0);
    set_font(layout, "sans 8");
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    text_at(cr, layout, f->editing ? "EDIT LIQUID" : "NEW LIQUID", PAD + 30.0, PAD, HEADER_H, NULL);

    set_font(layout, "sans 10");
    static const char *const label[] = { "Name", "Colour", "Thickness", "Density" };
    static const double label_y[] = { Y_NAME, Y_HEX, Y_VISC, Y_DENS };
    for (int i = 0; i < 4; i++) {
        double ly = label_y[i] + (i < 2 ? (ROW_H - FIELD_H) / 2.0 : 2.0);
        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        text_at(cr, layout, label[i], PAD, ly, FIELD_H, NULL);
    }

    draw_field(cr, layout, FIELD_X, Y_NAME + (ROW_H - FIELD_H) / 2.0, RIGHT - FIELD_X,
               f->text[LIQUID_FORM_NAME], f->focus == LIQUID_FORM_NAME, "Honey, oil, lava…");
    draw_field(cr, layout, FIELD_X, Y_HEX + (ROW_H - FIELD_H) / 2.0, 96.0,
               f->text[LIQUID_FORM_HEX], f->focus == LIQUID_FORM_HEX, NULL);
    draw_wheel(cr, f);

    draw_field(cr, layout, FIELD_X, Y_VISC + 2.0, NUM_W, f->text[LIQUID_FORM_VISC],
               f->focus == LIQUID_FORM_VISC, NULL);
    draw_slider(cr, layout, Y_VISC, visc_to_t(f->viscosity), "water", "honey · tar",
                visc_to_t(60.0));
    draw_field(cr, layout, FIELD_X, Y_DENS + 2.0, NUM_W, f->text[LIQUID_FORM_DENS],
               f->focus == LIQUID_FORM_DENS, NULL);
    draw_slider(cr, layout, Y_DENS, dens_to_t(f->density), "lighter than water", "mercury",
                dens_to_t(1.0));

    if (f->error[0]) {
        set_font(layout, "sans 8");
        cairo_set_source_rgb(cr, COL_ERROR[0], COL_ERROR[1], COL_ERROR[2]);
        text_at(cr, layout, f->error, FIELD_X, Y_ERROR, ERROR_H, NULL);
        set_font(layout, "sans 10");
    }

    double by = Y_BUTTONS + (BUTTONS_H - BTN_H) / 2.0;
    draw_button(cr, layout, RIGHT - BTN_W, by, f->editing ? "Save" : "Create", true,
                f->hover_button == 2);
    draw_button(cr, layout, RIGHT - 2 * BTN_W - 10.0, by, "Cancel", false, f->hover_button == 1);

    g_object_unref(layout);
}

struct wlr_scene_buffer *liquid_form_show(struct wlr_scene_tree *parent, int x, int y,
                                          const LiquidForm *f) {
    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, FORM_W, FORM_H);
    if (buf) {
        wlr_scene_node_set_position(&buf->node, x, y);
        cairo_overlay_update(buf, draw_form, (void *)f);
        glass_attach(buf);
        cairo_overlay_animate_in(buf, 170.0, -14.0);
    }
    return buf;
}

void liquid_form_redraw(struct wlr_scene_buffer *buf, const LiquidForm *f) {
    if (buf) cairo_overlay_update(buf, draw_form, (void *)f);
}

/* ── input ─────────────────────────────────────────────────────────────── */

static bool in_rect(double x, double y, double rx, double ry, double rw, double rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int button_at(double x, double y) {
    double by = Y_BUTTONS + (BUTTONS_H - BTN_H) / 2.0;
    if (in_rect(x, y, RIGHT - BTN_W, by, BTN_W, BTN_H)) return 2;
    if (in_rect(x, y, RIGHT - 2 * BTN_W - 10.0, by, BTN_W, BTN_H)) return 1;
    return 0;
}

static void drag_to(LiquidForm *f, double x, double y) {
    switch (f->drag) {
    case DRAG_WHEEL: {
        double dx = x - WHEEL_CX, dy = y - WHEEL_CY, r = WHEEL_D / 2.0;
        double h = atan2(-dy, dx) / (2.0 * M_PI);
        f->hue = h < 0.0 ? h + 1.0 : h;
        f->sat = fmin(1.0, sqrt(dx * dx + dy * dy) / r);
        /* A colour picked off a black wheel is still black; lift it to where
         * the pick can be seen. */
        if (f->val < 0.15) f->val = 0.15;
        sync_hex(f);
        break;
    }
    case DRAG_VALUE:
        f->val = fmin(1.0, fmax(0.0, 1.0 - (y - BAR_Y) / WHEEL_D));
        sync_hex(f);
        break;
    case DRAG_VISC: {
        double t = fmin(1.0, fmax(0.0, (x - SLIDE_X0) / (SLIDE_X1 - SLIDE_X0)));
        f->viscosity = fmin(LIQUID_VISCOSITY_MAX, round2(visc_from_t(t)));
        fmt_visc(f->text[LIQUID_FORM_VISC], sizeof(f->text[0]), f->viscosity);
        break;
    }
    case DRAG_DENS: {
        double t = fmin(1.0, fmax(0.0, (x - SLIDE_X0) / (SLIDE_X1 - SLIDE_X0)));
        f->density = fmin(LIQUID_DENSITY_MAX, fmax(LIQUID_DENSITY_MIN, round2(dens_from_t(t))));
        fmt_dens(f->text[LIQUID_FORM_DENS], sizeof(f->text[0]), f->density);
        break;
    }
    default:
        break;
    }
}

LiquidFormResult liquid_form_press(LiquidForm *f, double x, double y) {
    if (x < 0 || y < 0 || x >= FORM_W || y >= FORM_H) return LIQUID_FORM_IGNORED;
    int b = button_at(x, y);
    if (b == 1) return LIQUID_FORM_CANCEL;
    if (b == 2) return validate(f) ? LIQUID_FORM_SUBMIT : LIQUID_FORM_CHANGED;

    double dx = x - WHEEL_CX, dy = y - WHEEL_CY, r = WHEEL_D / 2.0;
    if (dx * dx + dy * dy <= r * r) f->drag = DRAG_WHEEL;
    else if (in_rect(x, y, BAR_X - 4, BAR_Y, BAR_W + 8, WHEEL_D)) f->drag = DRAG_VALUE;
    else if (in_rect(x, y, SLIDE_X0 - 8, Y_VISC, SLIDE_X1 - SLIDE_X0 + 16, SLIDER_ROW)) f->drag = DRAG_VISC;
    else if (in_rect(x, y, SLIDE_X0 - 8, Y_DENS, SLIDE_X1 - SLIDE_X0 + 16, SLIDER_ROW)) f->drag = DRAG_DENS;
    if (f->drag) {
        drag_to(f, x, y);
        return LIQUID_FORM_CHANGED;
    }

    int focus = -1;
    if (in_rect(x, y, FIELD_X, Y_NAME, RIGHT - FIELD_X, ROW_H)) focus = LIQUID_FORM_NAME;
    else if (in_rect(x, y, FIELD_X, Y_HEX, 96.0, ROW_H)) focus = LIQUID_FORM_HEX;
    else if (in_rect(x, y, FIELD_X, Y_VISC, NUM_W, FIELD_H + 4)) focus = LIQUID_FORM_VISC;
    else if (in_rect(x, y, FIELD_X, Y_DENS, NUM_W, FIELD_H + 4)) focus = LIQUID_FORM_DENS;
    if (focus == f->focus) return LIQUID_FORM_IGNORED;
    f->focus = focus;
    return LIQUID_FORM_CHANGED;
}

LiquidFormResult liquid_form_motion(LiquidForm *f, double x, double y) {
    if (f->drag) {
        drag_to(f, x, y);
        return LIQUID_FORM_CHANGED;
    }
    int b = button_at(x, y);
    if (b == f->hover_button) return LIQUID_FORM_IGNORED;
    f->hover_button = b;
    return LIQUID_FORM_CHANGED;
}

LiquidFormResult liquid_form_release(LiquidForm *f) {
    if (!f->drag) return LIQUID_FORM_IGNORED;
    f->drag = DRAG_NONE;
    return LIQUID_FORM_CHANGED;
}

/* What a field takes: a name takes any text, the others only what they can
 * hold, so a slip of the hand does not turn a number into a word. */
static bool field_accepts(int field, const char *utf8) {
    if ((unsigned char)utf8[0] < 0x20 || utf8[0] == 0x7f) return false;
    if (field == LIQUID_FORM_NAME) return true;
    if (utf8[1]) return false;
    char c = utf8[0];
    if (field == LIQUID_FORM_HEX) return c == '#' || strchr("0123456789abcdefABCDEF", c);
    return (c >= '0' && c <= '9') || c == '.' || c == ',';
}

/* The field's text changed: carry it into the value it stands for, if it
 * stands for one yet. Half-typed is fine — the value waits. */
static void field_changed(LiquidForm *f, int field) {
    double v;
    float rgb[3];
    switch (field) {
    case LIQUID_FORM_HEX:
        if (parse_hex(f->text[field], rgb)) rgb_to_hsv(rgb, &f->hue, &f->sat, &f->val);
        break;
    case LIQUID_FORM_VISC:
        if (parse_num(f->text[field], &v) && v >= 0.0) f->viscosity = fmin(v, LIQUID_VISCOSITY_MAX);
        break;
    case LIQUID_FORM_DENS:
        if (parse_num(f->text[field], &v) && v > 0.0)
            f->density = fmin(LIQUID_DENSITY_MAX, fmax(LIQUID_DENSITY_MIN, v));
        break;
    default:
        break;
    }
}

LiquidFormResult liquid_form_key(LiquidForm *f, uint32_t sym, const char *utf8) {
    switch (sym) {
    case XKB_KEY_Escape:
        return LIQUID_FORM_CANCEL;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        return validate(f) ? LIQUID_FORM_SUBMIT : LIQUID_FORM_CHANGED;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        f->focus = (f->focus + 1) % LIQUID_FORM_FIELDS;
        return LIQUID_FORM_CHANGED;
    default:
        break;
    }
    if (f->focus < 0) return LIQUID_FORM_IGNORED;
    char *t = f->text[f->focus];
    size_t n = strlen(t);
    if (sym == XKB_KEY_BackSpace) {
        if (n == 0) return LIQUID_FORM_IGNORED;
        /* One character, which in UTF-8 may be several bytes. */
        do { n--; } while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80);
        t[n] = '\0';
        field_changed(f, f->focus);
        f->error[0] = '\0';
        return LIQUID_FORM_CHANGED;
    }
    if (!utf8 || !utf8[0] || !field_accepts(f->focus, utf8)) return LIQUID_FORM_IGNORED;
    size_t add = strlen(utf8);
    size_t cap = f->focus == LIQUID_FORM_NAME ? sizeof(f->text[0]) - 1
               : f->focus == LIQUID_FORM_HEX  ? 9 : 10;
    if (n + add > cap) return LIQUID_FORM_IGNORED;
    memcpy(t + n, utf8, add + 1);
    field_changed(f, f->focus);
    f->error[0] = '\0';
    return LIQUID_FORM_CHANGED;
}
