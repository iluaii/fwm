#ifndef FWM_WINDOW_H
#define FWM_WINDOW_H

#include <X11/Xlib.h>

typedef struct {
    int x, y;
    int width, height;
} WindowGeometry;

int window_get_geometry(Display *dpy, Window win, WindowGeometry *geometry);
WindowGeometry window_center_geometry(int screen_width, int screen_height);
void window_map_centered(Display *dpy, Window win, int screen_width, int screen_height,
                         WindowGeometry *geometry);
void window_spawn(const char **cmd);

#endif /* FWM_WINDOW_H */
