#include "errors.h"
#include "../theme.h"
#include "cairo_overlay.h"
#include "tray.h"
#include <stdio.h>
#include <string.h>

#define ERR_W        720
#define ERR_PAD_X    28
#define ERR_PAD_Y    22
#define ERR_LINE_H   20
#define ERR_TITLE_H  30
#define ERR_CUT      14.0  /* corner chevron cut, matching the hints panel */

struct ErrCtx {
    const FwmConfig *cfg;
    double opacity;
};

/* Same silhouette as the hints panel: moderate corner cut, not the tray's
 * full h/2 chevron. */
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

/* Wrapped line count for a message, so tall panels size themselves correctly. */
static int wrapped_lines(PangoLayout *layout, const char *text, int width) {
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_text(layout, text, -1);
    int n = pango_layout_get_line_count(layout);
    return n > 0 ? n : 1;
}

static int panel_height(const FwmConfig *cfg) {
    /* Measured off-screen with the same font/width the draw pass uses. */
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(s);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    int text_w = ERR_W - 2 * ERR_PAD_X - 18; /* 18: bullet column */
    int lines = 0;
    for (int i = 0; i < cfg->error_count; i++)
        lines += wrapped_lines(layout, cfg->errors[i].msg, text_w);
    if (cfg->error_total > cfg->error_count) lines++;
    lines += 2; /* source path + footer */

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return ERR_PAD_Y * 2 + ERR_TITLE_H + lines * ERR_LINE_H + ERR_LINE_H;
}

static void draw_errors_content(cairo_t *cr, int w, int h, void *user_data) {
    struct ErrCtx *ctx = user_data;
    const FwmConfig *cfg = ctx->cfg;

    const FwmTheme *thm = theme_get();
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], ctx->opacity);
    panel_path(cr, 0, 0, w, h, ERR_CUT);
    cairo_fill(cr);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    double y = ERR_PAD_Y;

    /* Title, amber like the pill that opened this panel. */
    {
        char title[96];
        snprintf(title, sizeof(title), "\xE2\x9A\xA0 %d config problem%s",
                 cfg->error_total, cfg->error_total == 1 ? "" : "s");
        PangoFontDescription *bold = pango_font_description_from_string("sans bold 11");
        pango_layout_set_font_description(layout, bold);
        pango_font_description_free(bold);
        pango_layout_set_width(layout, -1);
        pango_layout_set_text(layout, title, -1);
        cairo_set_source_rgb(cr, 0.98, 0.75, 0.27);
        cairo_move_to(cr, ERR_PAD_X, y);
        pango_cairo_show_layout(cr, layout);

        PangoFontDescription *plain = pango_font_description_from_string("sans 10");
        pango_layout_set_font_description(layout, plain);
        pango_font_description_free(plain);
        y += ERR_TITLE_H;
    }

    /* Source path, so the user knows which file to fix. Ellipsized in the
     * middle: a long path must not run past the panel edge. */
    pango_layout_set_width(layout, (w - 2 * ERR_PAD_X) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
    pango_layout_set_text(layout, cfg->source, -1);
    cairo_set_source_rgb(cr, 0.45, 0.48, 0.55);
    cairo_move_to(cr, ERR_PAD_X, y);
    pango_cairo_show_layout(cr, layout);
    y += ERR_LINE_H * 1.4;

    /* Messages wrap instead of being cut short, so drop the ellipsis the path
     * above set on this shared layout. */
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    int text_w = w - 2 * ERR_PAD_X - 18;
    for (int i = 0; i < cfg->error_count; i++) {
        cairo_set_source_rgb(cr, 0.98, 0.75, 0.27);
        cairo_arc(cr, ERR_PAD_X + 3, y + ERR_LINE_H / 2.0 - 1, 2.5, 0, 2 * 3.14159265358979);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.88, 0.90, 0.93);
        pango_layout_set_width(layout, text_w * PANGO_SCALE);
        pango_layout_set_text(layout, cfg->errors[i].msg, -1);
        cairo_move_to(cr, ERR_PAD_X + 18, y);
        pango_cairo_show_layout(cr, layout);
        y += pango_layout_get_line_count(layout) * ERR_LINE_H;
    }

    if (cfg->error_total > cfg->error_count) {
        char more[64];
        snprintf(more, sizeof(more), "\xE2\x80\xA6 and %d more",
                 cfg->error_total - cfg->error_count);
        pango_layout_set_width(layout, -1);
        pango_layout_set_text(layout, more, -1);
        cairo_set_source_rgb(cr, 0.56, 0.60, 0.67);
        cairo_move_to(cr, ERR_PAD_X + 18, y);
        pango_cairo_show_layout(cr, layout);
        y += ERR_LINE_H;
    }

    const char *footer = cfg->fallback_binds
        ? "Built-in keybindings are active. Fix the file, then Super+Shift+R to reload."
        : "Fix the file, then Super+Shift+R to reload.";
    pango_layout_set_width(layout, -1);
    pango_layout_set_text(layout, footer, -1);
    cairo_set_source_rgb(cr, 0.56, 0.60, 0.67);
    cairo_move_to(cr, ERR_PAD_X, y + ERR_LINE_H * 0.4);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
}

struct wlr_scene_buffer *errors_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                     const FwmConfig *cfg) {
    if (cfg->error_count <= 0) return NULL;

    struct ErrCtx ctx = { .cfg = cfg, .opacity = cfg->decor.tray_opacity };
    int panel_h = panel_height(cfg);
    if (panel_h > screen_h - 60) panel_h = screen_h - 60;

    /* Hangs just below the tray, left-aligned with the pill it belongs to. */
    int wx = 20;
    int wy = TRAY_HEIGHT + 16;

    struct wlr_scene_buffer *buf = cairo_overlay_create(parent, ERR_W, panel_h);
    if (buf) {
        wlr_scene_node_set_position(&buf->node, wx, wy);
        cairo_overlay_update(buf, draw_errors_content, &ctx);
        cairo_overlay_make_static(buf);
        /* Drops down from under the tray pill it belongs to. */
        cairo_overlay_animate_in(buf, 170.0, -14.0);
    }
    return buf;
}
