#include "welcome.h"
#include "cairo_overlay.h"
#include "logo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WELCOME_W 670
#define WELCOME_H 300
#define WELCOME_LOGO_H 88
#define WELCOME_PAD_X 32
#define WELCOME_PAD_Y 28
#define WELCOME_LINE_H 24

static const char *welcome_lines[] = {
    "fwm — physics-based window manager with real mass and inertia.",
    "Drag windows and throw them; they bounce, collide, and push each other.",
    "Press Super+? to see all keybindings.",
    NULL,
    "Press Enter to close this window.",
};

static int welcomed_flag_exists(void) {
    const char *home = getenv("HOME");
    if (!home) return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/.fwm_welcomed", home);
    return access(path, F_OK) == 0;
}

void welcome_set_welcomed(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.fwm_welcomed", home);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

#define WELCOME_CUT 14.0 /* corner chevron cut, px — matches hints.c */

// Tray-island silhouette with a moderate corner cut (see panel_path in hints.c).
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

static void draw_welcome_content(cairo_t *cr, int w, int h, void *user_data) {
    double opacity = *(double *)user_data;

    // Same flat near-black as the tray islands, same opacity knob.
    cairo_set_source_rgba(cr, 0.075, 0.082, 0.098, opacity);
    panel_path(cr, 0, 0, w, h, WELCOME_CUT);
    cairo_fill(cr);
    
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    
    // Logo badge centered at the top (#d0a82c brand gold), text below.
    double logo_w = WELCOME_LOGO_H * FWM_LOGO_AR_BRACKETS;
    fwm_logo_draw(cr, (w - logo_w) / 2.0, WELCOME_PAD_Y, WELCOME_LOGO_H, FWM_LOGO_BRACKETS,
                  0.816, 0.659, 0.173, 1.0);

    int y = WELCOME_PAD_Y + WELCOME_LOGO_H + 24;

    for (size_t i = 0; i < sizeof(welcome_lines)/sizeof(welcome_lines[0]); i++) {
        if (welcome_lines[i] == NULL) {
            y += WELCOME_LINE_H;
            continue;
        }
        
        pango_layout_set_text(layout, welcome_lines[i], -1);
        
        // First two lines: #eceff4, others: #9099aa
        if (i < 2) {
            cairo_set_source_rgba(cr, 0.92, 0.94, 0.96, 1.0); // #eceff4
        } else {
            cairo_set_source_rgba(cr, 0.56, 0.60, 0.67, 1.0); // #9099aa
        }
        
        cairo_move_to(cr, WELCOME_PAD_X, y);
        pango_cairo_show_layout(cr, layout);
        y += WELCOME_LINE_H;
    }
    
    g_object_unref(layout);
}

struct wlr_scene_buffer *welcome_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                      const FwmConfig *cfg) {
    if (welcomed_flag_exists()) return NULL;

    int wx = (screen_w - WELCOME_W) / 2;
    int wy = (screen_h - WELCOME_H) / 2;

    struct wlr_scene_buffer *welcome_buf = cairo_overlay_create(parent, WELCOME_W, WELCOME_H);
    if (welcome_buf) {
        double opacity = cfg->decor.tray_opacity;
        wlr_scene_node_set_position(&welcome_buf->node, wx, wy);
        cairo_overlay_update(welcome_buf, draw_welcome_content, &opacity);
        cairo_overlay_make_static(welcome_buf);
    }
    return welcome_buf;
}
