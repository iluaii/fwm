#include "wm.h"

#include <X11/keysym.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "window.h"
#include "ui/decorations.h"

static int xerror_handler(Display *dpy, XErrorEvent *event) {
    char msg[256];
    XGetErrorText(dpy, event->error_code, msg, sizeof(msg));
    fprintf(stderr, "X11 error: %s(%d)\n", msg, event->error_code);
    return 0;
}

static void handle_map_request(Fwm *wm, XMapRequestEvent *event) {
    WindowGeometry geometry;
    window_map_centered(wm->dpy, event->window, wm->screen_width, wm->screen_height, &geometry);
    physics_sync_body(&wm->physics, event->window, geometry.x, geometry.y,
                      geometry.width, geometry.height);

    XSelectInput(wm->dpy, event->window, EnterWindowMask);
}

static void handle_button_press(Fwm *wm, XButtonEvent *event) {
    if (event->subwindow == None) return;
    if (event->subwindow == wm->tray_win) return;
    
    WindowGeometry geometry;
    if (!window_get_geometry(wm->dpy, event->subwindow, &geometry)) {
        return;
    }

    if (wm->last_touched_win != None && wm->last_touched_win != event->subwindow) {
        decorations_draw_border(wm->dpy, wm->last_touched_win, 0);
    }

    wm->last_touched_win = event->subwindow;
    decorations_draw_border(wm->dpy, event->subwindow, 1);

    physics_sync_body(&wm->physics, event->subwindow, geometry.x, geometry.y,
                      geometry.width, geometry.height);
    physics_stop_body(&wm->physics, event->subwindow);

    if (event->button == Button1) {
        wm->drag.dragging = 1;
        wm->drag.hist_count = 0;
        wm->drag.win = event->subwindow;
        wm->drag.collision_disabled = (event->state & ShiftMask) ? 1 : 0;
        wm->drag.start_x = event->x_root;
        wm->drag.start_y = event->y_root;
        wm->drag.last_x = event->x_root;
        wm->drag.last_y = event->y_root;
        wm->drag.last_time = event->time;
        wm->drag.vx = 0;
        wm->drag.vy = 0;
        wm->drag.win_start_x = geometry.x;
        wm->drag.win_start_y = geometry.y;
        wm->drag.win_width = geometry.width;
        wm->drag.win_height = geometry.height;
        XRaiseWindow(wm->dpy, wm->drag.win);
    } else if (event->button == Button3) {
        wm->resize.resizing = 1;
        wm->resize.win = event->subwindow;
        wm->resize.start_x = event->x_root;
        wm->resize.start_y = event->y_root;
        wm->resize.win_x = geometry.x;
        wm->resize.win_y = geometry.y;
        wm->resize.win_start_width = geometry.width;
        wm->resize.win_start_height = geometry.height;
        XRaiseWindow(wm->dpy, wm->resize.win);
    }
}

static void handle_drag_motion(Fwm *wm, XMotionEvent *event) {
    if (!wm->drag.dragging) return;

    int dx = event->x_root - wm->drag.start_x;
    int dy = event->y_root - wm->drag.start_y;

    int new_x = wm->drag.win_start_x + dx;
    int new_y = wm->drag.win_start_y + dy;

    int min_x = -(wm->drag.win_width - DRAG_MARGIN);
    int max_x = wm->screen_width - DRAG_MARGIN - wm->drag.win_width;
    int min_y = -(wm->drag.win_height - DRAG_MARGIN);
    int max_y = wm->screen_height - DRAG_MARGIN - wm->drag.win_height;

    if (new_x < min_x) new_x = min_x;
    if (new_x > max_x) new_x = max_x;
    if (new_y < min_y) new_y = min_y;
    if (new_y > max_y) new_y = max_y;

    for (int i = 0; i < VELOCITY_HISTORY - 1; i++) {
        wm->drag.hist_x[i] = wm->drag.hist_x[i + 1];
        wm->drag.hist_y[i] = wm->drag.hist_y[i + 1];
        wm->drag.hist_time[i] = wm->drag.hist_time[i + 1];
    }
    wm->drag.hist_x[VELOCITY_HISTORY - 1] = event->x_root;
    wm->drag.hist_y[VELOCITY_HISTORY - 1] = event->y_root;
    wm->drag.hist_time[VELOCITY_HISTORY - 1] = event->time;
    if (wm->drag.hist_count < VELOCITY_HISTORY) wm->drag.hist_count++;

    if (wm->drag.hist_count >= 2) {
        int oldest = VELOCITY_HISTORY - wm->drag.hist_count;
        double dt = (double)(event->time - wm->drag.hist_time[oldest]) / 1000.0;
        if (dt > 0.001) {
            wm->drag.vx = (event->x_root - wm->drag.hist_x[oldest]) / dt;
            wm->drag.vy = (event->y_root - wm->drag.hist_y[oldest]) / dt;
        }
    }

    XMoveWindow(wm->dpy, wm->drag.win, new_x, new_y);
    decorations_draw_border(wm->dpy, wm->drag.win, 1);
    physics_sync_body(&wm->physics, wm->drag.win, new_x, new_y,
                      wm->drag.win_width, wm->drag.win_height);
}

static void handle_resize_motion(Fwm *wm, XMotionEvent *event) {
    if (!wm->resize.resizing) return;

    int dx = event->x_root - wm->resize.start_x;
    int dy = event->y_root - wm->resize.start_y;

    int new_w = wm->resize.win_start_width + dx;
    int new_h = wm->resize.win_start_height + dy;

    if (new_w < 50) new_w = 50;
    if (new_h < 50) new_h = 50;

    if (wm->resize.win_x + new_w > wm->screen_width) {
        new_w = wm->screen_width - wm->resize.win_x;
    }
    if (wm->resize.win_y + new_h > wm->screen_height) {
        new_h = wm->screen_height - wm->resize.win_y;
    }

    XResizeWindow(wm->dpy, wm->resize.win, new_w, new_h);
    physics_sync_body(&wm->physics, wm->resize.win, wm->resize.win_x, wm->resize.win_y,
                      new_w, new_h);
}

static void handle_button_release(Fwm *wm) {
    if (wm->drag.dragging) {
        WindowGeometry geometry;
        if (window_get_geometry(wm->dpy, wm->drag.win, &geometry)) {
            physics_sync_body(&wm->physics, wm->drag.win, geometry.x, geometry.y,
                              geometry.width, geometry.height);
            physics_throw_body(&wm->physics, wm->drag.win, wm->drag.vx, wm->drag.vy);
        }
    }

    wm->drag.dragging = 0;
    wm->resize.resizing = 0;
}

static void handle_key_press(Fwm *wm, XKeyEvent *event) {
    KeyCode return_key = XKeysymToKeycode(wm->dpy, XK_Return);
    if (event->keycode == return_key && (event->state & MOD_KEY)) {
        window_spawn_terminal();
    }
}

void fwm_init(Fwm *wm, Display *dpy) {
    memset(wm, 0, sizeof(*wm));
    wm->dpy = dpy;
    wm->root = XDefaultRootWindow(dpy);
    wm->screen_width = DisplayWidth(dpy, DefaultScreen(dpy));
    wm->screen_height = DisplayHeight(dpy, DefaultScreen(dpy));
    physics_init(&wm->physics);

    XSetErrorHandler(xerror_handler);
    XSelectInput(dpy, wm->root, SubstructureRedirectMask | StructureNotifyMask);

    KeyCode return_key = XKeysymToKeycode(dpy, XK_Return);
    XGrabKey(dpy, return_key, MOD_KEY, wm->root, True, GrabModeAsync, GrabModeAsync);

    XGrabButton(dpy, Button1, MOD_KEY, wm->root, True,
                ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);

    XGrabButton(dpy, Button1, MOD_KEY | ShiftMask, wm->root, True,
            ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
            GrabModeAsync, GrabModeAsync, None, None);

    XGrabButton(dpy, Button3, MOD_KEY, wm->root, True,
                ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);

    XSync(dpy, False);
}

static void handle_enter_notify(Fwm *wm, XCrossingEvent *event) {
    if (event->window == wm->tray_win) return;
    XSetInputFocus(wm->dpy, event->window, RevertToPointerRoot, CurrentTime);

    if (wm->last_touched_win != None && wm->last_touched_win != event->window) {
        decorations_draw_border(wm->dpy, wm->last_touched_win, 0);
    }
    wm->last_touched_win = event->window;
    decorations_draw_border(wm->dpy, event->window, 1);
}

void fwm_handle_event(Fwm *wm, XEvent *event) {
    switch (event->type) {
        case MapRequest:
            handle_map_request(wm, &event->xmaprequest);
            break;
        case ButtonPress:
            handle_button_press(wm, &event->xbutton);
            break;
        case MotionNotify:
            handle_drag_motion(wm, &event->xmotion);
            handle_resize_motion(wm, &event->xmotion);
            break;
        case ButtonRelease:
            handle_button_release(wm);
            break;
        case KeyPress:
            handle_key_press(wm, &event->xkey);
            break;
        case EnterNotify:
            handle_enter_notify(wm, &event->xcrossing);
            break;
        default:
            break;
    }
}

void fwm_tick(Fwm *wm, double dt) {
    Window drag_window = (wm->drag.dragging && wm->drag.collision_disabled) ? wm->drag.win : None;
    Window resize_window = wm->resize.resizing ? wm->resize.win : None;
    Window dragged_win = wm->drag.dragging ? wm->drag.win : None;

    if (wm->drag.dragging) {
        physics_set_velocity(&wm->physics, wm->drag.win, wm->drag.vx, wm->drag.vy);
    }

    physics_step(&wm->physics, wm->dpy, wm->screen_width, wm->screen_height,
                 drag_window, resize_window, dragged_win, dt);
    XFlush(wm->dpy);
}
