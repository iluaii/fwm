#include <math.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include "wm.h"
#include "ui/decorations.h"
#include "ui/tray.h"
#include "defines.h"

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
        TrayData tray_data = {0};

        for (int i = 0; i < 10; i++) {
            tray_data.desktop_window_counts[i] = 0;
        }
        for (int i = 0; i < wm.physics.body_count; i++) {
            PhysicsBody *body = &wm.physics.bodies[i];
            if (body->active) {
                int d = (int)((body->x + body->width / 2.0) / wm.screen_width);
                if (d >= 0 && d < 10) {
                    tray_data.desktop_window_counts[d]++;
                }
            }
        }

        tray_data.active_desktop = (wm.camera_x + wm.screen_width / 2) / wm.screen_width;
        if (tray_data.active_desktop < 0) tray_data.active_desktop = 0;
        if (tray_data.active_desktop >= 10) tray_data.active_desktop = 9;
        char *win_name = NULL;

        if (wm.last_touched_win != None) {
            PhysicsBody *b = physics_find_body(&wm.physics, wm.last_touched_win);
            if (b) {
                if (!XFetchName(dpy, wm.last_touched_win, &win_name) || !win_name) {
                    win_name = "Window";
                }

                tray_data.win_name = win_name;
                tray_data.speed = hypot(b->vx, b->vy);
                tray_data.angle = atan2(b->vy, b->vx) * 180.0 / M_PI;
                tray_data.mass = b->mass;
                tray_data.flying = b->flying;
            }
        }

        static TrayData prev_tray_data;
        static int tray_drawn = 0;
        if (!tray_drawn || memcmp(&tray_data, &prev_tray_data, sizeof(TrayData)) != 0) {
            tray_redraw(dpy, tray, &tray_data);
            prev_tray_data = tray_data;
            tray_drawn = 1;
        }

        if (win_name && strcmp(win_name, "Window") != 0) {
            XFree(win_name);
        }
    }

    XCloseDisplay(dpy);
    return 0;
}