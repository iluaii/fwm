#include "tray.h"
#include "cairo_overlay.h"
#include <stdio.h>
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
#define PILL_ALPHA  0.92

static const double COL_PILL[3]  = {0.075, 0.082, 0.098}; /* near-black */
static const double COL_TEXT[3]  = {0.91, 0.92, 0.94};    /* primary */
static const double COL_MUTED[3] = {0.54, 0.57, 0.63};    /* secondary */
static const double COL_DIM[3]   = {0.32, 0.34, 0.40};    /* empty desktop dot */

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

static void draw_pill(cairo_t *cr, double x, double y, double w, double h) {
    pill_path(cr, x, y, w, h);
    cairo_set_source_rgba(cr, COL_PILL[0], COL_PILL[1], COL_PILL[2], PILL_ALPHA);
    cairo_fill(cr);
}

typedef struct {
    const TrayData *data;
} DrawTrayData;

static void draw_tray_content(cairo_t *cr, int w, int h, void *user_data) {
    DrawTrayData *draw_data = user_data;
    const TrayData *data = draw_data->data;
    if (!data) return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    int th;
    pango_layout_get_pixel_size(layout, NULL, &th);
    double text_y = (h - th) / 2.0;

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
        double pw = PILL_PAD + title_w + gap + params_w + PILL_PAD;
        draw_pill(cr, 0, 0, pw, h);

        cairo_set_source_rgb(cr, COL_TEXT[0], COL_TEXT[1], COL_TEXT[2]);
        pango_layout_set_text(layout, data->win_name, -1);
        cairo_move_to(cr, PILL_PAD, text_y);
        pango_cairo_show_layout(cr, layout);

        cairo_set_source_rgb(cr, COL_MUTED[0], COL_MUTED[1], COL_MUTED[2]);
        pango_layout_set_text(layout, params, -1);
        cairo_move_to(cr, PILL_PAD + title_w + gap, text_y);
        pango_cairo_show_layout(cr, layout);
    }

    /* ── center pill: desktop indicators ── */
    {
        double spacing = 18.0;
        double pw = PILL_PAD * 2 + spacing * 9 + 6;
        double px = (w - pw) / 2.0;
        draw_pill(cr, px, 0, pw, h);

        for (int i = 0; i < 10; i++) {
            double cx = px + PILL_PAD + 3 + i * spacing;
            int count = data->desktop_window_counts[i];
            int active = (i == data->active_desktop);

            if (count > 0) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", count);
                pango_layout_set_text(layout, buf, -1);
                int nw;
                pango_layout_get_pixel_size(layout, &nw, NULL);
                if (active) cairo_set_source_rgb(cr, COL_TEXT[0], COL_TEXT[1], COL_TEXT[2]);
                else        cairo_set_source_rgb(cr, COL_MUTED[0], COL_MUTED[1], COL_MUTED[2]);
                cairo_move_to(cr, cx - nw / 2.0, text_y);
                pango_cairo_show_layout(cr, layout);
            } else {
                double r = active ? 3.5 : 2.0;
                if (active) cairo_set_source_rgb(cr, COL_TEXT[0], COL_TEXT[1], COL_TEXT[2]);
                else        cairo_set_source_rgb(cr, COL_DIM[0], COL_DIM[1], COL_DIM[2]);
                cairo_arc(cr, cx, h / 2.0, r, 0, 2 * M_PI);
                cairo_fill(cr);
            }

            // Small underline marker for the active desktop, dot or number.
            if (active) {
                cairo_set_source_rgb(cr, COL_TEXT[0], COL_TEXT[1], COL_TEXT[2]);
                cairo_rectangle(cr, cx - 4, h - 6, 8, 2);
                cairo_fill(cr);
            }
        }
    }

    /* ── right pill: clock ── */
    {
        char clock[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(clock, sizeof(clock), "%H:%M \xE2\x80\xA2 %a, %d/%m", &tm);

        int cw;
        pango_layout_set_text(layout, clock, -1);
        pango_layout_get_pixel_size(layout, &cw, NULL);

        double pw = PILL_PAD * 2 + cw;
        double px = w - pw;
        draw_pill(cr, px, 0, pw, h);

        cairo_set_source_rgb(cr, COL_TEXT[0], COL_TEXT[1], COL_TEXT[2]);
        cairo_move_to(cr, px + PILL_PAD, text_y);
        pango_cairo_show_layout(cr, layout);
    }

    g_object_unref(layout);
}

struct wlr_scene_buffer *tray_init(struct wlr_scene_tree *parent, int screen_width) {
    int tray_width = screen_width - 40;
    int tray_x = (screen_width - tray_width) / 2;
    int tray_y = 8;

    struct wlr_scene_buffer *tray_buf = cairo_overlay_create(parent, tray_width, TRAY_HEIGHT);
    if (tray_buf) {
        wlr_scene_node_set_position(&tray_buf->node, tray_x, tray_y);
    }
    return tray_buf;
}

void tray_redraw(struct wlr_scene_buffer *tray_buf, const TrayData *data) {
    DrawTrayData draw_data = { .data = data };
    cairo_overlay_update(tray_buf, draw_tray_content, &draw_data);
}
