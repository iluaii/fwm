#include "tray.h"

#include <stdio.h>
#include <string.h>
#include <X11/extensions/shape.h>
#include <X11/Xft/Xft.h>

static GC tray_gc;
static unsigned long bg_color;
static XftFont *font;
static XftDraw *xft_draw;
static XftColor text_primary;
static XftColor text_secondary;
static int tray_width_stored;

static void apply_hexagon_shape(Display *dpy, Window tray, int width, int height) {
    int cut = height / 2;

    Pixmap mask = XCreatePixmap(dpy, tray, width, height, 1);

    XGCValues gcv;
    GC mask_gc = XCreateGC(dpy, mask, 0, &gcv);

    XSetForeground(dpy, mask_gc, 1);
    XPoint points[6] = {
        { cut, 0 },
        { width - cut, 0 },
        { width, height / 2 },
        { width - cut, height },
        { cut, height },
        { 0, height / 2 },
    };
    XFillPolygon(dpy, mask, mask_gc, points, 6, Convex, CoordModeOrigin);

    XShapeCombineMask(dpy, tray, ShapeBounding, 0,0, mask, ShapeSet);

    XFreeGC(dpy, mask_gc);
    XFreePixmap(dpy, mask);
}

Window tray_init(Display *dpy, Window root, int screen_width) {
    int screen = DefaultScreen(dpy);

    int tray_width = screen_width - 40;
    int tray_x = (screen_width - tray_width) / 2;
    int tray_y = 8;

    Window tray = XCreateSimpleWindow(dpy, root, tray_x, tray_y, tray_width, TRAY_HEIGHT, 0,0, BlackPixel(dpy, screen));

    tray_gc = XCreateGC(dpy, tray, 0, NULL);


    Visual *visual = DefaultVisual(dpy, screen);
    Colormap cmap = XDefaultColormap(dpy, screen);

    font = XftFontOpenName(dpy, screen, "monospace:size=10");
    xft_draw = XftDrawCreate(dpy, tray, visual, cmap);

    XftColorAllocName(dpy, visual, cmap, "#eceff4", &text_primary);
    XftColorAllocName(dpy, visual, cmap, "#9099aa", &text_secondary);

    tray_width_stored = tray_width;

    XColor color;
    XParseColor(dpy, cmap, "#2e3440", &color);
    XAllocColor(dpy, cmap, &color);
    bg_color = color.pixel;

    XSetWindowBackground(dpy, tray, bg_color);

    apply_hexagon_shape(dpy, tray, tray_width, TRAY_HEIGHT);
    XMapRaised(dpy, tray);

    return tray;
}

void tray_redraw(Display *dpy, Window tray_win, const TrayData *data) {
    XRaiseWindow(dpy, tray_win);

    XSetForeground(dpy, tray_gc, bg_color);
    XFillRectangle(dpy, tray_win, tray_gc, 0, 0, tray_width_stored, TRAY_HEIGHT);

    if (!data) return;

    int text_y = (TRAY_HEIGHT + font->ascent - font->descent) / 2;
    int margin_x = TRAY_HEIGHT / 2 + 8;

    // 1. Draw 10 desktop indicators
    int dot_spacing = 20;
    for (int i = 0; i < 10; i++) {
        char buf[16];
        int count = data->desktop_window_counts[i];
        if (count > 0) {
            snprintf(buf, sizeof(buf), "%d", count);
        } else {
            strcpy(buf, "•");
        }

        XftColor *color = (i == data->active_desktop) ? &text_primary : &text_secondary;
        XftDrawStringUtf8(xft_draw, color, font,
                          margin_x + i * dot_spacing, text_y,
                          (const FcChar8 *)buf, strlen(buf));
    }

    int win_info_x = margin_x + 10 * dot_spacing + 15;
    if (data->win_name) {
        XftDrawStringUtf8(xft_draw, &text_primary, font,
                          win_info_x, text_y,
                          (const FcChar8 *)data->win_name,
                          strlen(data->win_name));

        char params[128];
        if (data->flying) {
            snprintf(params, sizeof(params),
                     "spd: %.0f  ang: %.0f°  mass: %.1f",
                     data->speed, data->angle, data->mass);
        } else {
            snprintf(params, sizeof(params),
                     "mass: %.1f  idle",
                     data->mass);
        }

        XGlyphInfo extents;
        XftTextExtentsUtf8(dpy, font,
                           (const FcChar8 *)data->win_name,
                           strlen(data->win_name), &extents);

        XftDrawStringUtf8(xft_draw, &text_secondary, font,
                          win_info_x + extents.xOff + 8, text_y,
                          (const FcChar8 *)params,
                          strlen(params));
    }
}