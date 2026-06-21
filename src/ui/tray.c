#include "tray.h"
#include <X11/extensions/shape.h>

static GC tray_gc;
static unsigned long bg_color;

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

    Colormap cmap = XDefaultColormap(dpy, screen);
    XColor color;
    XParseColor(dpy, cmap, "#2e3440", &color);
    XAllocColor(dpy, cmap, &color);
    bg_color = color.pixel;

    XSetWindowBackground(dpy, tray, bg_color);

    apply_hexagon_shape(dpy, tray, tray_width, TRAY_HEIGHT);
    XMapRaised(dpy, tray);

    return tray;
}

void tray_redraw(Display *dpy, Window tray_win, int screen_width) {
    XRaiseWindow(dpy, tray_win);

    XSetForeground(dpy, tray_gc, bg_color);
    XFillRectangle(dpy, tray_win, tray_gc, 0, 0, screen_width, TRAY_HEIGHT);
}