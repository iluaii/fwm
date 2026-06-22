
#ifndef CODEPROJECTS_TRAY_H
#define CODEPROJECTS_TRAY_H
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#define TRAY_HEIGHT 28


typedef struct {
    const char *win_name;
    double speed;
    double angle;
    double mass;
    int flying;
    int desktop_window_counts[10];
    int active_desktop;
} TrayData;

Window tray_init(Display *dpy, Window root, int screen_width);
void tray_redraw(Display *dpy, Window tray_win, const TrayData *data);

#endif //CODEPROJECTS_TRAY_H
