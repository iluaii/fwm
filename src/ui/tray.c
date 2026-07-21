#include <stdlib.h>
#include "tray.h"
#include "cairo_overlay.h"
#include "../theme.h"
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
#define PILL_GAP     8.0  /* clearance kept between two islands */
#define DESK_SPACING 18.0 /* centre-to-centre of the desktop indicators */
#define TITLE_MIN   40.0  /* never squeeze the title below this before giving up */

/* Palette comes from the live theme (see src/theme.h) so the tray follows
 * [decor] color_source. Amber stays hardcoded: a warning must not blend into
 * whatever the wallpaper suggests. */
static const double COL_WARN[3]  = {0.98, 0.75, 0.27};    /* config-error amber */

/* Screen rect of the error pill, in tray-buffer coordinates, published for
 * hit-testing. Width depends on the rendered text, so it is recorded during
 * the draw rather than recomputed by the caller. */
static struct { double x, y, w, h; int valid; } g_err_pill;

int tray_error_pill_hit(double x, double y) {
    return g_err_pill.valid &&
           x >= g_err_pill.x && x <= g_err_pill.x + g_err_pill.w &&
           y >= g_err_pill.y && y <= g_err_pill.y + g_err_pill.h;
}

/* Same idea for the desktop island: the indicator geometry is decided during
 * the draw (it depends on the pill width), so it is recorded there. */
static struct {
    double x, y, w, h;   /* the whole island */
    double first_cx;     /* centre of indicator 0 */
    double spacing;
    int valid;
} g_desk;

int tray_desktop_island_hit(double x, double y) {
    return g_desk.valid &&
           x >= g_desk.x && x <= g_desk.x + g_desk.w &&
           y >= g_desk.y && y <= g_desk.y + g_desk.h;
}

int tray_desktop_hit(double x, double y) {
    if (!tray_desktop_island_hit(x, y)) return -1;
    /* Snap to the nearest indicator rather than demanding a hit on the dot
     * itself: the dots are 4-7px across, which is not a clickable target. */
    int i = (int)lround((x - g_desk.first_cx) / g_desk.spacing);
    if (i < 0) i = 0;
    if (i > 9) i = 9;
    return i;
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
} DrawTrayData;

static void draw_tray_content(cairo_t *cr, int w, int h, void *user_data) {
    DrawTrayData *draw_data = user_data;
    const TrayData *data = draw_data->data;
    if (!data) return;

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
    g_err_pill.valid = 0;
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

        g_err_pill.x = 0; g_err_pill.y = 0;
        g_err_pill.w = pw; g_err_pill.h = h; g_err_pill.valid = 1;
        left_x = pw + 8.0;
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

        g_desk.x = px; g_desk.y = 0; g_desk.w = pw; g_desk.h = h;
        g_desk.first_cx = px + PILL_PAD + 3;
        g_desk.spacing = spacing;
        g_desk.valid = 1;

        for (int i = 0; i < 10; i++) {
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
        draw_pill(cr, px, 0, pw, h, data->opacity);

        cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        cairo_move_to(cr, px + PILL_PAD, text_y);
        pango_cairo_show_layout(cr, layout);
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
 * churn, so tray_redraw compares against the last drawn signature first. */
typedef struct {
    char name[128];
    int  speed, angle;   /* displayed as %.0f */
    int  mass10;         /* displayed as %.1f */
    int  flying;
    int  counts[10];
    int  active_desktop;
    int  pos_mil; /* active_pos in thousandths — redraw while it glides */
    int  opacity1000;
    int  minute;         /* clock shows minutes at most */
    char kbd[8];
    int  errors, err_expanded;
    unsigned theme_gen;
} TraySig;

void tray_redraw(struct wlr_scene_buffer *tray_buf, const TrayData *data) {
    static TraySig last;
    static int have_last = 0;

    TraySig sig = {0};
    if (data->win_name) snprintf(sig.name, sizeof(sig.name), "%s", data->win_name);
    sig.speed = (int)lround(data->speed);
    sig.angle = (int)lround(data->angle);
    sig.mass10 = (int)lround(data->mass * 10.0);
    sig.flying = data->flying;
    memcpy(sig.counts, data->desktop_window_counts, sizeof(sig.counts));
    sig.active_desktop = data->active_desktop;
    sig.pos_mil = (int)lround(data->active_pos * 1000.0);
    sig.opacity1000 = (int)lround(data->opacity * 1000.0);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    sig.minute = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
    memcpy(sig.kbd, data->kbd_layout, sizeof(sig.kbd));
    sig.errors = data->error_count;
    sig.err_expanded = data->error_expanded;
    sig.theme_gen = theme_generation();

    if (have_last && memcmp(&sig, &last, sizeof(sig)) == 0) return;
    last = sig;
    have_last = 1;

    DrawTrayData draw_data = { .data = data };
    cairo_overlay_update(tray_buf, draw_tray_content, &draw_data);
}
