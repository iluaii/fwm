#include "logo.h"
#include <stddef.h>

/* Same geometry as assets/logo*.svg: F/W/M ligature in 110x100 design units,
 * hexagon frame in 240x132. Stroke 7 units, miter joins, butt caps. */

static const double MONO[][12] = {
    /* count*2, then x,y pairs */
    {6, 6,96, 6,10, 76,10},
    {4, 6,46, 34,46},
    {10, 34,46, 46,96, 54,62, 64,96, 76,10},
    {8, 76,10, 90,54, 104,14, 104,96},
};

static const double HEX[14] = {66,0, 174,0, 240,66, 174,132, 66,132, 0,66};
static const double BRACKET_L[6] = {-14,16, -40,53, -14,90};
static const double BRACKET_R[6] = {124,16, 150,53, 124,90};

static void draw_polyline(cairo_t *cr, const double *pts, int n, int close) {
    cairo_move_to(cr, pts[0], pts[1]);
    for (int i = 1; i < n; i++) {
        cairo_line_to(cr, pts[i * 2], pts[i * 2 + 1]);
    }
    if (close) cairo_close_path(cr);
    cairo_stroke(cr);
}

void fwm_logo_draw(cairo_t *cr, double x, double y, double height, int variant,
                   double r, double g, double b, double a) {
    double design_h = variant == FWM_LOGO_FRAMED ? 132.0 : 100.0;
    double s = height / design_h;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);

    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, 7.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);

    switch (variant) {
    case FWM_LOGO_FRAMED:
        draw_polyline(cr, HEX, 6, 1);
        cairo_translate(cr, 64.0, 13.0); /* center monogram in the hexagon */
        break;
    case FWM_LOGO_BRACKETS:
        cairo_translate(cr, 43.5, -6.5); /* leftmost bracket edge -> x = 0 */
        draw_polyline(cr, BRACKET_L, 3, 0);
        draw_polyline(cr, BRACKET_R, 3, 0);
        break;
    default: /* FWM_LOGO_MONO */
        cairo_translate(cr, -6.0, -10.0); /* monogram content starts at (6,10) */
        break;
    }
    for (size_t i = 0; i < sizeof(MONO) / sizeof(MONO[0]); i++) {
        draw_polyline(cr, &MONO[i][1], (int)(MONO[i][0] / 2.0), 0);
    }
    cairo_restore(cr);
}
