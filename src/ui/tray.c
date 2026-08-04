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

#include <stdlib.h>
#include "tray.h"
#include "cairo_overlay.h"
#include "modes.h"
#include "../theme.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Minimal "islands" style with the WM's sharp identity: flat dark islands
 * whose ends taper to a point (chevron cut), on a transparent background.
 * No gradients, no shadows — depth comes from spacing alone. */

#define PILL_PAD    20.0  /* horizontal padding inside an island (covers the point) */
#define PILL_GAP     8.0  /* clearance kept between two islands */
#define DESK_SPACING 18.0 /* centre-to-centre of the desktop indicators */
#define TITLE_MIN   40.0  /* never squeeze the title below this before giving up */

/* Palette comes from the live theme (see src/theme.h) so the tray follows
 * [decor] color_source. Amber stays hardcoded: a warning must not blend into
 * whatever the wallpaper suggests. */
static const double COL_WARN[3]  = {0.98, 0.75, 0.27};    /* config-error amber */

/* The islands' rects are decided DURING the draw — the error pill's width
 * depends on its text, the desktop island's indicator spacing on its own width,
 * and the modes pill is dropped entirely when the screen is too narrow — so
 * they are recorded there, into the caller's TrayStrip. One per monitor: see
 * the header. */

static int rect_hit(const TrayRect *r, double x, double y) {
    return r->valid && x >= r->x && x <= r->x + r->w &&
                       y >= r->y && y <= r->y + r->h;
}

int tray_error_pill_hit(const TrayStrip *strip, double x, double y) {
    return strip && rect_hit(&strip->err, x, y);
}

int tray_desktop_island_hit(const TrayStrip *strip, double x, double y) {
    return strip && rect_hit(&strip->desk, x, y);
}

int tray_desktop_hit(const TrayStrip *strip, double x, double y) {
    if (!tray_desktop_island_hit(strip, x, y)) return -1;
    /* Snap to the nearest indicator rather than demanding a hit on the dot
     * itself: the dots are 4-7px across, which is not a clickable target. */
    int i = (int)lround((x - strip->desk_first_cx) / strip->desk_spacing);
    if (i < 0) i = 0;
    if (i > FWM_DESKTOPS - 1) i = FWM_DESKTOPS - 1;
    return i;
}

/* Modes pill: fixed width (see MODES_PILL_W), so the clock on its right and the
 * desktop island on its left keep the positions they had before it existed. */
int tray_modes_pill_hit(const TrayStrip *strip, double x, double y) {
    return strip && rect_hit(&strip->modes, x, y);
}

double tray_modes_pill_x(const TrayStrip *strip) {
    return strip ? strip->modes.x : 0.0;
}

/* Island with pointed (chevron) ends: same silhouette family as the old bar. */
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

static void draw_pill(cairo_t *cr, double x, double y, double w, double h, double alpha) {
    const FwmTheme *thm = theme_get();
    pill_path(cr, x, y, w, h);
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], alpha);
    cairo_fill(cr);
}

typedef struct {
    const TrayData *data;
    TrayStrip *strip;   /* geometry is written back here as it is drawn */
} DrawTrayData;

static void draw_tray_content(cairo_t *cr, int w, int h, void *user_data) {
    DrawTrayData *draw_data = user_data;
    const TrayData *data = draw_data->data;
    TrayStrip *strip = draw_data->strip;
    if (!data || !strip) return;

    const FwmTheme *thm = theme_get();

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    int th;
    pango_layout_get_pixel_size(layout, NULL, &th);
    double text_y = (h - th) / 2.0;

    /* The centre island has a fixed width and is centred, so its left edge is
     * a hard ceiling for everything drawn from the left. Worked out up here
     * because the title pill needs it before the centre block runs. */
    const double desk_pw = PILL_PAD * 2 + DESK_SPACING * 9 + 6;
    const double desk_px = (w - desk_pw) / 2.0;

    /* ── error pill: leftmost, only while the config has problems ── */
    double left_x = 0.0;
    strip->err.valid = 0;
    if (data->error_count > 0) {
        char warn[64];
        snprintf(warn, sizeof(warn), "\xE2\x9A\xA0 %d", data->error_count);
        int ww;
        pango_layout_set_text(layout, warn, -1);
        pango_layout_get_pixel_size(layout, &ww, NULL);

        double pw = PILL_PAD + ww + PILL_PAD;
        /* Expanded: amber fill, dark text — the pill reads as pressed. */
        pill_path(cr, 0, 0, pw, h);
        if (data->error_expanded)
            cairo_set_source_rgba(cr, COL_WARN[0], COL_WARN[1], COL_WARN[2], data->opacity);
        else
            cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], data->opacity);
        cairo_fill(cr);

        if (data->error_expanded) cairo_set_source_rgb(cr, thm->pill[0], thm->pill[1], thm->pill[2]);
        else                      cairo_set_source_rgb(cr, COL_WARN[0], COL_WARN[1], COL_WARN[2]);
        cairo_move_to(cr, PILL_PAD, text_y);
        pango_cairo_show_layout(cr, layout);

        strip->err.x = 0; strip->err.y = 0;
        strip->err.w = pw; strip->err.h = h; strip->err.valid = 1;
        left_x = pw + 8.0;
    }

    /* ── mode pill: next to the errors, only while a submap is open ──
     * Drawn in the accent colour rather than the island grey: a mode is a
     * state the keyboard is IN, and the tray is the only thing that says so. */
    if (data->mode_name && data->mode_name[0]) {
        char label[64];
        snprintf(label, sizeof(label), "\xE2\x8C\xA8 %s", data->mode_name);  /* ⌨ */
        int mw;
        pango_layout_set_text(layout, label, -1);
        pango_layout_get_pixel_size(layout, &mw, NULL);

        double pw = PILL_PAD + mw + PILL_PAD;
        pill_path(cr, left_x, 0, pw, h);
        cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2],
                              data->opacity);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, thm->pill[0], thm->pill[1], thm->pill[2]);
        cairo_move_to(cr, left_x + PILL_PAD, text_y);
        pango_cairo_show_layout(cr, layout);

        left_x += pw + 8.0;
    }

    /* ── left pill: focused window title + physics info ── */
    if (data->win_name) {
        char params[128];
        if (data->flying) {
            snprintf(params, sizeof(params), "spd %.0f  ang %.0f\xC2\xB0  m %.1f",
                     data->speed, data->angle, data->mass);
        } else {
            snprintf(params, sizeof(params), "m %.1f", data->mass);
        }

        int title_w, params_w;
        pango_layout_set_text(layout, data->win_name, -1);
        pango_layout_get_pixel_size(layout, &title_w, NULL);
        pango_layout_set_text(layout, params, -1);
        pango_layout_get_pixel_size(layout, &params_w, NULL);

        double gap = 10.0;

        /* Window titles are arbitrarily long, so an unclamped pill grew until
         * it ran under the centre island -- which, being drawn later, painted
         * over it. It showed up on narrow screens first: the centre island is
         * centred, so a 1366px panel leaves the title barely a third of the
         * room a 2560px one does.
         *
         * Only the title gives way; the physics readout is short, fixed and
         * the reason this pill exists at all. */
        double avail = desk_px - PILL_GAP - left_x
                       - (PILL_PAD + gap + params_w + PILL_PAD);
        if (avail < TITLE_MIN) avail = TITLE_MIN;
        int ellipsized = title_w > avail;
        if (ellipsized) {
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
            pango_layout_set_width(layout, (int)(avail * PANGO_SCALE));
            pango_layout_set_text(layout, data->win_name, -1);
            pango_layout_get_pixel_size(layout, &title_w, NULL);
        }

        double pw = PILL_PAD + title_w + gap + params_w + PILL_PAD;
        draw_pill(cr, left_x, 0, pw, h, data->opacity);

        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        pango_layout_set_text(layout, data->win_name, -1);
        cairo_move_to(cr, left_x + PILL_PAD, text_y);
        pango_cairo_show_layout(cr, layout);

        /* Hand the layout back unclamped: the params, the clock and the
         * desktop counters all reuse it and must not inherit the ellipsis. */
        if (ellipsized) {
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
            pango_layout_set_width(layout, -1);
        }

        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        pango_layout_set_text(layout, params, -1);
        cairo_move_to(cr, left_x + PILL_PAD + title_w + gap, text_y);
        pango_cairo_show_layout(cr, layout);
    }

    /* ── center pill: desktop indicators ── */
    {
        double spacing = DESK_SPACING;
        double pw = desk_pw;
        double px = desk_px;
        draw_pill(cr, px, 0, pw, h, data->opacity);

        strip->desk.x = px; strip->desk.y = 0;
        strip->desk.w = pw; strip->desk.h = h; strip->desk.valid = 1;
        strip->desk_first_cx = px + PILL_PAD + 3;
        strip->desk_spacing = spacing;

        for (int i = 0; i < FWM_DESKTOPS; i++) {
            double cx = px + PILL_PAD + 3 + i * spacing;
            int count = data->desktop_window_counts[i];
            int active = (i == data->active_desktop);

            if (count > 0) {
                /* Sized for any int, not for the count we expect: the compiler
                 * cannot know it is bounded by MAX_WINDOWS, and neither can a
                 * future caller feeding this from somewhere else. */
                char buf[12];
                snprintf(buf, sizeof(buf), "%d", count);
                pango_layout_set_text(layout, buf, -1);
                int nw;
                pango_layout_get_pixel_size(layout, &nw, NULL);
                if (active) cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
                else        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
                cairo_move_to(cr, cx - nw / 2.0, text_y);
                pango_cairo_show_layout(cr, layout);
            } else {
                double r = active ? 3.5 : 2.0;
                if (active) cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
                else        cairo_set_source_rgb(cr, thm->dim[0], thm->dim[1], thm->dim[2]);
                cairo_arc(cr, cx, h / 2.0, r, 0, 2 * M_PI);
                cairo_fill(cr);
            }

        }

        // Underline marker: drawn at the fractional camera position, so it
        // glides between indicators in sync with the desktop-switch slide.
        double ux = px + PILL_PAD + 3 + data->active_pos * spacing;
        cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
        cairo_rectangle(cr, ux - 4, h - 6, 8, 2);
        cairo_fill(cr);
    }

    /* ── right pill: clock (+ keyboard layout when several configured) ── */
    double clock_px;
    {
        char clock[80];
        char stamp[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%H:%M \xE2\x80\xA2 %a, %d/%m", &tm);
        if (data->kbd_layout[0]) {
            snprintf(clock, sizeof(clock), "%s \xE2\x80\xA2 %s", data->kbd_layout, stamp);
        } else {
            snprintf(clock, sizeof(clock), "%s", stamp);
        }

        int cw;
        pango_layout_set_text(layout, clock, -1);
        pango_layout_get_pixel_size(layout, &cw, NULL);

        double pw = PILL_PAD * 2 + cw;
        double px = w - pw;
        clock_px = px;
        draw_pill(cr, px, 0, pw, h, data->opacity);

        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        cairo_move_to(cr, px + PILL_PAD, text_y);
        pango_cairo_show_layout(cr, layout);
    }

    /* ── modes pill: between the desktop island and the clock ──
     * Fixed width, and DROPPED rather than squeezed when it does not fit: the
     * clock grows with the locale's date and the island is centred, so on a
     * narrow screen there is a width at which something has to give. Losing the
     * pill costs a shortcut that also exists as a keybind; overlapping the clock
     * would corrupt both, and the tray is drawn back-to-front so the damage
     * would be silent. */
    {
        double px = clock_px - PILL_GAP - MODES_PILL_W;
        double left_limit = desk_px + desk_pw + PILL_GAP;
        if (px >= left_limit) {
            int pressed = data->modes_open;
            pill_path(cr, px, 0, MODES_PILL_W, h);
            if (pressed)
                cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2],
                                      data->opacity);
            else
                cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2],
                                      data->opacity);
            cairo_fill(cr);

            strip->modes.x = px; strip->modes.y = 0;
            strip->modes.w = MODES_PILL_W; strip->modes.h = h;
            strip->modes.valid = 1;

            /* Four slots, always in the same order and always all four drawn:
             * an icon that vanished when its mode went off would make the
             * remaining ones move, and a row that reflows is unreadable at a
             * glance — which is the only way anyone reads a tray. */
            const int icons[4] = {
                MODE_ICON_TILING, MODE_ICON_FLOATING, MODE_ICON_GRAVITY, MODE_ICON_CAVA,
            };
            const int on[4] = {
                data->modes_tiling, data->modes_floating, data->modes_gravity,
                data->modes_cava != 0,
            };
            double is = 14.0;
            double gap = (MODES_PILL_W - PILL_PAD * 2 - is * 4) / 3.0;
            double iy = (h - is) / 2.0;
            for (int i = 0; i < 4; i++) {
                if (pressed) {
                    /* On the pressed pill the fill IS the accent, so an active
                     * icon has to invert instead of using it. */
                    if (on[i]) cairo_set_source_rgb(cr, thm->pill[0], thm->pill[1], thm->pill[2]);
                    else       cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], 0.4);
                } else {
                    if (on[i]) cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
                    else       cairo_set_source_rgb(cr, thm->dim[0], thm->dim[1], thm->dim[2]);
                }
                modes_icon(cr, icons[i], px + PILL_PAD + i * (is + gap), iy, is);
            }
        } else {
            strip->modes.valid = 0;
        }
    }

    g_object_unref(layout);
}

struct wlr_scene_buffer *tray_init(struct wlr_scene_tree *parent, int screen_width) {
    int tray_width = screen_width - 40;
    int tray_x = (screen_width - tray_width) / 2;
    int tray_y = TRAY_MARGIN;

    struct wlr_scene_buffer *tray_buf = cairo_overlay_create(parent, tray_width, TRAY_HEIGHT);
    if (tray_buf) {
        wlr_scene_node_set_position(&tray_buf->node, tray_x, tray_y);
    }
    return tray_buf;
}

/* Everything the tray renders, rounded to displayed precision. Redrawing a
 * full-width ARGB strip at 60 Hz when nothing changed is pure memory/GPU
 * churn, so tray_redraw compares against the last drawn signature first.
 *
 * The signature lives in the caller's TrayStrip, not in a static: with one per
 * process, two monitors compared each strip against the OTHER one's contents —
 * which repainted both every frame, and, on the frame where the two happened to
 * agree, left the second strip blank because its buffer had never been drawn at
 * all. */
void tray_redraw(struct wlr_scene_buffer *tray_buf, const TrayData *data,
                 TrayStrip *strip) {
    if (!tray_buf || !data || !strip) return;

    TrayStrip sig = *strip;
    memset(sig.sig_name, 0, sizeof(sig.sig_name));
    if (data->win_name) snprintf(sig.sig_name, sizeof(sig.sig_name), "%s", data->win_name);
    sig.sig_speed = (int)lround(data->speed);
    sig.sig_angle = (int)lround(data->angle);
    sig.sig_mass10 = (int)lround(data->mass * 10.0);
    sig.sig_flying = data->flying;
    memcpy(sig.sig_counts, data->desktop_window_counts, sizeof(sig.sig_counts));
    sig.sig_active_desktop = data->active_desktop;
    sig.sig_pos_mil = (int)lround(data->active_pos * 1000.0);
    sig.sig_opacity1000 = (int)lround(data->opacity * 1000.0);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    sig.sig_minute = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
    memcpy(sig.sig_kbd, data->kbd_layout, sizeof(sig.sig_kbd));
    sig.sig_errors = data->error_count;
    sig.sig_err_expanded = data->error_expanded;
    sig.sig_m_tiling   = data->modes_tiling;
    sig.sig_m_floating = data->modes_floating;
    sig.sig_m_gravity  = data->modes_gravity;
    sig.sig_m_cava     = data->modes_cava;
    sig.sig_m_open     = data->modes_open;
    sig.sig_theme_gen = theme_generation();

    /* Compare the signature fields only: the rects below them are an OUTPUT of
     * the draw, so folding them in would compare this frame's inputs against
     * last frame's results and never match. */
    size_t n = offsetof(TrayStrip, have_sig);
    if (strip->have_sig && memcmp(&sig, strip, n) == 0) return;
    memcpy(strip, &sig, n);
    strip->have_sig = 1;

    DrawTrayData draw_data = { .data = data, .strip = strip };
    cairo_overlay_update(tray_buf, draw_tray_content, &draw_data);
}
