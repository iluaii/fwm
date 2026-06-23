#include "decorations.h"

#include <X11/extensions/shape.h>

static GC border_gc;

static unsigned long color_focused;
static unsigned long color_unfocused;

void decorations_init(Display *dpy) {
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    border_gc = XCreateGC(dpy, root, 0, NULL);

    Colormap cmap = XDefaultColormap(dpy, screen);
    XColor color;

    XParseColor(dpy, cmap, "#5e81ac", &color);
    XAllocColor(dpy, cmap, &color);
    color_focused = color.pixel;

    XParseColor(dpy, cmap, "#4c566a", &color);
    XAllocColor(dpy, cmap, &color);
    color_unfocused = color.pixel;
}

void decorations_draw_border(Display *dpy, Window win, int focused) {
    unsigned long color = focused ? color_focused : color_unfocused;
    XSetWindowBorder(dpy, win, color);
    XSetWindowBorderWidth(dpy, win, BORDER_WIDTH);
}

void decorations_apply_chamfer(Display *dpy, Window win, int width, int height, int cut) {
    if (width <= 0 || height <= 0) return;
    if (cut < 0) cut = 0;
    if (cut * 2 > width) cut = width / 2;
    if (cut * 2 > height) cut = height / 2;

    Pixmap mask = XCreatePixmap(dpy, win, width, height, 1);
    GC gc = XCreateGC(dpy, mask, 0, NULL);

    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, mask, gc, 0, 0, width, height);

    XSetForeground(dpy, gc, 1);
    XPoint points[8] = {
        { cut, 0 },
        { width - cut, 0 },
        { width, cut },
        { width, height - cut },
        { width - cut, height },
        { cut, height },
        { 0, height - cut },
        { 0, cut },
    };
    XFillPolygon(dpy, mask, gc, points, 8, Convex, CoordModeOrigin);

    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, mask);
}