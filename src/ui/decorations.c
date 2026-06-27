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

void decorations_apply_chamfer(Display *dpy, Window win,
                               int width, int height, int cut) {
    if (width <= 0 || height <= 0) return;
    if (cut < 0) cut = 0;
    if (cut * 2 > width)  cut = width  / 2;
    if (cut * 2 > height) cut = height / 2;

    Pixmap mask = XCreatePixmap(dpy, win, width, height, 1);
    GC gc = XCreateGC(dpy, mask, 0, NULL);

    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, mask, gc, 0, 0, width, height);

    XSetForeground(dpy, gc, 1);
    XPoint points[8] = {
        { cut,         0          },
        { width - cut, 0          },
        { width,       cut        },
        { width,       height-cut },
        { width - cut, height     },
        { cut,         height     },
        { 0,           height-cut },
        { 0,           cut        },
    };
    XFillPolygon(dpy, mask, gc, points, 8, Convex, CoordModeOrigin);

    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, mask);
}

static void apply_rounded(Display *dpy, Window win,
                           int width, int height, int r) {
    if (width <= 0 || height <= 0) return;
    if (r < 0) r = 0;
    if (r * 2 > width)  r = width  / 2;
    if (r * 2 > height) r = height / 2;

    Pixmap mask = XCreatePixmap(dpy, win, width, height, 1);
    GC gc = XCreateGC(dpy, mask, 0, NULL);

    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, mask, gc, 0, 0, width, height);

    XSetForeground(dpy, gc, 1);
    XFillRectangle(dpy, mask, gc, r, 0, width - 2*r, height);
    XFillRectangle(dpy, mask, gc, 0, r, width,       height - 2*r);
    XFillArc(dpy, mask, gc, 0,           0,            2*r, 2*r, 90*64,  90*64);
    XFillArc(dpy, mask, gc, width - 2*r, 0,            2*r, 2*r, 0,      90*64);
    XFillArc(dpy, mask, gc, width - 2*r, height - 2*r, 2*r, 2*r, 270*64, 90*64);
    XFillArc(dpy, mask, gc, 0,           height - 2*r, 2*r, 2*r, 180*64, 90*64);

    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, mask);
}

void decorations_set_corner_mode(Display *dpy, Window win, CornerState *cs,
                                 int mode, int width, int height) {
    cs->mode = mode;
    switch (mode) {
        case CORNER_SHARP:
            XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, None, ShapeSet);
            break;
        case CORNER_CHAMFER:
            decorations_apply_chamfer(dpy, win, width, height, 10);
            break;
        case CORNER_ROUND:
            apply_rounded(dpy, win, width, height, 14);
            break;
    }
}