#include "hints.h"

#include <stdio.h>
#include <string.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#define HINTS_W     480
#define HINTS_PAD_X 32
#define HINTS_PAD_Y 24
#define HINTS_LINE_H 22

#define HINTS_BG "#2e3440"

static const char *hints[] = {
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

#define HINTS_COUNT (int)(sizeof(hints)/sizeof(hints[0]))

void hints_show(Display *dpy, Window root, int screen_w, int screen_h) {
    int screen = DefaultScreen(dpy);
    Visual *visual = DefaultVisual(dpy, screen);
    Colormap cmap = XDefaultColormap(dpy, screen);

    XftFont *font = XftFontOpenName(dpy, screen, "monospace:size=10");

    int hints_h = HINTS_PAD_Y * 2 + HINTS_COUNT * HINTS_LINE_H;

    XColor bg_xcolor;
    XParseColor(dpy, cmap, HINTS_BG, &bg_xcolor);
    XAllocColor(dpy, cmap, &bg_xcolor);

    int wx = (screen_w - HINTS_W) / 2;
    int wy = (screen_h - hints_h) / 2;

    Window win = XCreateSimpleWindow(dpy, root,
                                     wx, wy, HINTS_W, hints_h,
                                     0, 0, bg_xcolor.pixel);

    XSetWindowBackground(dpy, win, bg_xcolor.pixel);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapRaised(dpy, win);
    XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);

    XftDraw *draw = XftDrawCreate(dpy, win, visual, cmap);

    XftColor col_primary, col_secondary, col_header;
    XftColorAllocName(dpy, visual, cmap, "#eceff4", &col_primary);
    XftColorAllocName(dpy, visual, cmap, "#9099aa", &col_secondary);
    XftColorAllocName(dpy, visual, cmap, "#88c0d0", &col_header);

    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == Expose) {
            GC gc = XCreateGC(dpy, win, 0, NULL);
            XSetForeground(dpy, gc, bg_xcolor.pixel);
            XFillRectangle(dpy, win, gc, 0, 0, HINTS_W, hints_h);
            XFreeGC(dpy, gc);

            int y = HINTS_PAD_Y + font->ascent;
            for (int i = 0; i < HINTS_COUNT; i++) {
                XftDrawStringUtf8(draw, &col_primary, font,
                                  HINTS_PAD_X, y,
                                  (const FcChar8 *)hints[i], strlen(hints[i]));
                y += HINTS_LINE_H;
            }

            const char *footer = "Press Escape or Enter to close";
            XftDrawStringUtf8(draw, &col_secondary, font,
                              HINTS_PAD_X, hints_h - HINTS_PAD_Y / 2,
                              (const FcChar8 *)footer, strlen(footer));
        }

        if (ev.type == KeyPress) {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            if (sym == XK_Return || sym == XK_Escape) {
                running = 0;
            }
        }
    }

    XftDrawDestroy(draw);
    XftColorFree(dpy, visual, cmap, &col_primary);
    XftColorFree(dpy, visual, cmap, &col_secondary);
    XftColorFree(dpy, visual, cmap, &col_header);
    XftFontClose(dpy, font);
    XDestroyWindow(dpy, win);
    XFlush(dpy);
}