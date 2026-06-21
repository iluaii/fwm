
#ifndef CODEPROJECTS_TRAY_H
#define CODEPROJECTS_TRAY_H
#include <X11/Xlib.h>

#define TRAY_HEIGHT 28

Window tray_init(Display *dpy, Window root, int screen_width);

void tray_redraw(Display *dpy, Window tray_win, int screen_width);
#endif //CODEPROJECTS_TRAY_H
