#include "hints.h"
#include "cairo_overlay.h"
#include "logo.h"
#include <string.h>

#define HINTS_W     480
#define HINTS_PAD_X 32
#define HINTS_PAD_Y 24
#define HINTS_LINE_H 22
#define HINTS_LOGO_H 64
#define HINTS_LOGO_GAP 20

static const char *hints_list[] = {
    "Super + Enter        terminal",
    "Super + Space        app launcher",
    "Super + Q            close window",
    "Super + T            toggle tiling",
    "Super + D            fake fullscreen",
    "Super + F            real fullscreen",
    "Super + H / L        scroll camera",
    "Super + P            pin window",
    "Super + N            toggle no-collide",
    "Super + Shift + C    calm all windows",
    "Super + G            cycle gravity",
    "Super + Shift + Esc  exit fwm",
    "Super + 1-0          switch desktop",
};

#define HINTS_COUNT (int)(sizeof(hints_list)/sizeof(hints_list[0]))

static void draw_hints_content(cairo_t *cr, int w, int h, void *user_data) {
    (void)user_data;
    
    // Draw background (#2e3440)
    cairo_set_source_rgba(cr, 0.18, 0.20, 0.25, 1.0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("monospace 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    
    int text_height;
    pango_layout_get_pixel_size(layout, NULL, &text_height);

    // Logo badge, centered above the list (#d0a82c brand gold).
    double logo_w = HINTS_LOGO_H * FWM_LOGO_AR_BRACKETS;
    fwm_logo_draw(cr, (w - logo_w) / 2.0, HINTS_PAD_Y, HINTS_LOGO_H, FWM_LOGO_BRACKETS,
                  0.816, 0.659, 0.173, 1.0);

    int y = HINTS_PAD_Y + HINTS_LOGO_H + HINTS_LOGO_GAP;

    // Draw hints
    cairo_set_source_rgba(cr, 0.92, 0.94, 0.96, 1.0); // #eceff4
    for (int i = 0; i < HINTS_COUNT; i++) {
        pango_layout_set_text(layout, hints_list[i], -1);
        cairo_move_to(cr, HINTS_PAD_X, y);
        pango_cairo_show_layout(cr, layout);
        y += HINTS_LINE_H;
    }
    
    // Draw footer
    const char *footer = "Press Escape or Enter to close";
    pango_layout_set_text(layout, footer, -1);
    cairo_set_source_rgba(cr, 0.56, 0.60, 0.67, 1.0); // #9099aa
    cairo_move_to(cr, HINTS_PAD_X, h - HINTS_PAD_Y - text_height/2);
    pango_cairo_show_layout(cr, layout);
    
    g_object_unref(layout);
}

struct wlr_scene_buffer *hints_show(struct wlr_scene_tree *parent, int screen_w, int screen_h) {
    int hints_h = HINTS_PAD_Y * 2 + HINTS_LOGO_H + HINTS_LOGO_GAP
                + (HINTS_COUNT + 1) * HINTS_LINE_H; /* +1: footer row */
    int wx = (screen_w - HINTS_W) / 2;
    int wy = (screen_h - hints_h) / 2;

    struct wlr_scene_buffer *hints_buf = cairo_overlay_create(parent, HINTS_W, hints_h);
    if (hints_buf) {
        wlr_scene_node_set_position(&hints_buf->node, wx, wy);
        cairo_overlay_update(hints_buf, draw_hints_content, NULL);
        cairo_overlay_make_static(hints_buf);
    }
    return hints_buf;
}
