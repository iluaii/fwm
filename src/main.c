#include <X11/Xlib.h>
#include <stdio.h>
#include <sys/select.h>
#include <time.h>
#include "config.h"
#include "wm.h"
#include "ui/decorations.h"
#include "ui/tray.h"

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec)
         + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Can't open display\n");
        return 1;
    }

    Fwm wm;
    fwm_init(&wm, dpy);
    decorations_init(dpy);
    Window tray = tray_init(dpy, wm.root, wm.screen_width);
    wm.tray_win = tray;

    int xfd = ConnectionNumber(dpy);
    fprintf(stderr, "fwm: init done, entering event loop\n");
    fflush(stderr);

    XEvent event;
    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = (long)(1000000.0 / PHYSICS_TICK_RATE);

        int ready = select(xfd + 1, &fds, NULL, NULL, &timeout);

        if (ready > 0) {
            while (XPending(dpy)) {
                XNextEvent(dpy, &event);
                fwm_handle_event(&wm, &event);
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        double dt = elapsed_seconds(last_tick, now);
        if (dt > 0.1) dt = 0.1;
        last_tick = now;

        fwm_tick(&wm, dt);
        tray_redraw(dpy, tray, wm.screen_width);
    }

    XCloseDisplay(dpy);
    return 0;
}