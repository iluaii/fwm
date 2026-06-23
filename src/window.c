#include "window.h"

#include <stdio.h>
#include <unistd.h>

int window_get_geometry(Display *dpy, Window win, WindowGeometry *geometry) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr)) {
        return 0;
    }

    geometry->x = attr.x;
    geometry->y = attr.y;
    geometry->width = attr.width;
    geometry->height = attr.height;
    return 1;
}

WindowGeometry window_center_geometry(int screen_width, int screen_height) {
    WindowGeometry geometry;
    geometry.width = screen_width / 2;
    geometry.height = screen_height / 2;
    geometry.x = (screen_width - geometry.width) / 2;
    geometry.y = (screen_height - geometry.height) / 2;
    return geometry;
}

void window_map_centered(Display *dpy, Window win, int screen_width, int screen_height,
                         WindowGeometry *geometry) {
    WindowGeometry centered = window_center_geometry(screen_width, screen_height);
    XMoveResizeWindow(dpy, win, centered.x, centered.y, centered.width, centered.height);
    XMapWindow(dpy, win);

    if (geometry) {
        *geometry = centered;
    }
}

void window_spawn(const char **cmd) {
    if (fork() == 0) {
        execvp(cmd[0], (char *const *)cmd);
        perror("fwm: execvp failed");
        _exit(1);
    }
}
