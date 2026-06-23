

#ifndef CODEPROJECTS_DECORATIONS_H
#define CODEPROJECTS_DECORATIONS_H
#include <X11/Xlib.h>

#define BORDER_WIDTH 3

void decorations_init(Display *dpy);
void decorations_draw_border(Display *dpy, Window win, int focused);
void decorations_apply_chamfer(Display *dpy, Window win, int width, int height, int cut);
#endif //CODEPROJECTS_DECORATIONS_H
