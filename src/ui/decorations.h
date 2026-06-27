#ifndef CODEPROJECTS_DECORATIONS_H
#define CODEPROJECTS_DECORATIONS_H

#include <X11/Xlib.h>

#define BORDER_WIDTH 3

#define CORNER_SHARP   0
#define CORNER_CHAMFER 1
#define CORNER_ROUND   2

typedef struct {
    int mode;
} CornerState;

void decorations_init(Display *dpy);
void decorations_draw_border(Display *dpy, Window win, int focused);
void decorations_apply_chamfer(Display *dpy, Window win, int width, int height, int cut);
void decorations_set_corner_mode(Display *dpy, Window win, CornerState *cs,
                                 int mode, int width, int height);

#endif