#include "welcome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#define WELCOME_W 670
#define WELCOME_H 180
#define WELCOME_BG "#2e3440"
#define WELCOME_PAD_X 32
#define WELCOME_PAD_Y 28
#define WELCOME_LINE_H 24

static const char *lines[] = {
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

static void create_welcomed_flag(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.fwm_welcomed", home);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

void welcome_show(Display *dpy, Window root, int screen_w, int screen_h) {
    if (welcomed_flag_exists()) return;

    int screen = DefaultScreen(dpy);
    Visual *visual = DefaultVisual(dpy, screen);
    Colormap cmap = XDefaultColormap(dpy, screen);

    XColor bg_xcolor;
    XParseColor(dpy, cmap, WELCOME_BG, &bg_xcolor);
    XAllocColor(dpy, cmap, &bg_xcolor);

    int wx = (screen_w - WELCOME_W) / 2;
    int wy = (screen_h - WELCOME_H) / 2;

    Window win = XCreateSimpleWindow(dpy, root,
                                     wx, wy, WELCOME_W, WELCOME_H,
                                     0, 0, bg_xcolor.pixel);

    XSetWindowBackground(dpy, win, bg_xcolor.pixel);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapRaised(dpy, win);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    XftFont *font_primary   = XftFontOpenName(dpy, screen, "monospace:size=10");
    XftFont *font_secondary = XftFontOpenName(dpy, screen, "monospace:size=10");

    XftDraw *draw = XftDrawCreate(dpy, win, visual, cmap);

    XftColor col_primary, col_secondary;
    XftColorAllocName(dpy, visual, cmap, "#eceff4", &col_primary);
    XftColorAllocName(dpy, visual, cmap, "#9099aa", &col_secondary);

    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == Expose) {
            XftDrawRect(draw, &(XftColor){0}, 0, 0, WELCOME_W, WELCOME_H);

            GC gc = XCreateGC(dpy, win, 0, NULL);
            XSetForeground(dpy, gc, bg_xcolor.pixel);
            XFillRectangle(dpy, win, gc, 0, 0, WELCOME_W, WELCOME_H);
            XFreeGC(dpy, gc);

            int y = WELCOME_PAD_Y + font_primary->ascent;
            for (size_t i = 0; i < sizeof(lines)/sizeof(lines[0]); i++) {
                if (lines[i] == NULL) {
                    y += WELCOME_LINE_H;
                    continue;
                }
                XftColor *col = (i < 2) ? &col_primary : &col_secondary;
                XftDrawStringUtf8(draw, col, font_primary,
                                  WELCOME_PAD_X, y,
                                  (const FcChar8 *)lines[i], strlen(lines[i]));
                y += WELCOME_LINE_H;
            }
        }

        if (ev.type == KeyPress) {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            if (sym == XK_Return) {
                running = 0;
            }
        }
    }

    create_welcomed_flag();

    XftDrawDestroy(draw);
    XftColorFree(dpy, visual, cmap, &col_primary);
    XftColorFree(dpy, visual, cmap, &col_secondary);
    XftFontClose(dpy, font_primary);
    XftFontClose(dpy, font_secondary);
    XDestroyWindow(dpy, win);
    XFlush(dpy);
}