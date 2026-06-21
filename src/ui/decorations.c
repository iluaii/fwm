#include "decorations.h"

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