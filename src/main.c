#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include "config.h"

#define MOD_KEY Mod4Mask

static Display *dpy;
static Window root;
static int screen_width, screen_height;

static struct {
    int dragging;
    Window win;
    int start_y, start_x;
    int win_start_y, win_start_x;
    int win_width, win_height;
    int last_x, last_y;
    double vx, vy;
} drag = {0};

static struct {
    int resizing;
    Window win;
    int start_x, start_y;
    int win_x, win_y;
    int win_start_width, win_start_height;
} resize = {0};

static struct {
    int flying;
    Window win;
    double vx, vy;
} physics = {0};

static int xerror_handler(Display *d, XErrorEvent *e) {
    char msg[256];
    XGetErrorText(d, e->error_code, msg, sizeof(msg));
    fprintf(stderr, "X11 error: %s(%d)\n", msg, e->error_code);
    return 0;
}

static void wm_init() {
    root = XDefaultRootWindow(dpy);

    screen_width = DisplayWidth(dpy, DefaultScreen(dpy));
    screen_height = DisplayHeight(dpy, DefaultScreen(dpy));

    XSetErrorHandler(xerror_handler);
    XSelectInput(dpy, root, SubstructureRedirectMask | StructureNotifyMask);

    KeyCode return_key = XKeysymToKeycode(dpy, XK_Return);
    XGrabKey(dpy, return_key, MOD_KEY, root, True, GrabModeAsync, GrabModeAsync);

    XGrabButton(dpy, Button1, MOD_KEY, root, True, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, GrabModeAsync, GrabModeAsync, None, None);

    XGrabButton(dpy, Button3, MOD_KEY, root, True, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, GrabModeAsync, GrabModeAsync, None, None);

    XSync(dpy, False);
}

static void handle_map_request(XMapRequestEvent *ev) {
    int w = screen_width / 2;
    int h = screen_height / 2;
    int x = (screen_width - w) / 2;
    int y = (screen_height - h) / 2;

    XMoveResizeWindow(dpy, ev->window, x, y, w, h);
    XMapWindow(dpy, ev->window);
}

static void handle_button_press(XButtonEvent *ev) {
    if (ev->subwindow == None) return;

    XWindowAttributes attr;
    XGetWindowAttributes(dpy, ev->subwindow, &attr);

    if (ev->button == Button1) {
        /* Если окно ещё "летело" по инерции от предыдущего броска,
         * хватание его руками должно немедленно остановить физику —
         * иначе handle_motion и physics_step будут одновременно
         * двигать одно и то же окно, споря друг с другом. */
        if (physics.flying && physics.win == ev->subwindow) {
            physics.flying = 0;
        }

        drag.dragging = 1;
        drag.win = ev->subwindow;
        drag.start_x = ev->x_root;
        drag.start_y = ev->y_root;
        drag.last_x = ev->x_root;
        drag.last_y = ev->y_root;
        drag.win_start_x = attr.x;
        drag.win_start_y = attr.y;
        drag.win_width = attr.width;
        drag.win_height = attr.height;

        XRaiseWindow(dpy, drag.win);
    } else if (ev->button == Button3) {
        resize.resizing = 1;
        resize.win = ev->subwindow;
        resize.start_x = ev->x_root;
        resize.start_y = ev->y_root;
        resize.win_x = attr.x;
        resize.win_y = attr.y;
        resize.win_start_width = attr.width;
        resize.win_start_height = attr.height;

        XRaiseWindow(dpy, resize.win);
    }
}

static void handle_motion(XMotionEvent *ev) {
    if (!drag.dragging) return;

    int dx = ev->x_root - drag.start_x;
    int dy = ev->y_root - drag.start_y;

    int new_x = drag.win_start_x + dx;
    int new_y = drag.win_start_y + dy;

    int margin = DRAG_MARGIN;
    int min_x = -(drag.win_width - margin);
    int max_x = screen_width - margin - drag.win_width;
    int min_y = -(drag.win_height - margin);
    int max_y = screen_height - margin - drag.win_height;

    if (new_x < min_x) new_x = min_x;
    if (new_x > max_x) new_x = max_x;
    if (new_y < min_y) new_y = min_y;
    if (new_y > max_y) new_y = max_y;

    double dt = 1.0 / PHYSICS_TICK_RATE;
    drag.vx = (ev->x_root - drag.last_x) / dt;
    drag.vy = (ev->y_root - drag.last_y) / dt;

    drag.last_x = ev->x_root;
    drag.last_y = ev->y_root;

    XMoveWindow(dpy, drag.win, new_x, new_y);
}

static void handle_resize_motion(XMotionEvent *ev) {
    if (!resize.resizing) return;

    int dx = ev->x_root - resize.start_x;
    int dy = ev->y_root - resize.start_y;

    int new_w = resize.win_start_width + dx;
    int new_h = resize.win_start_height + dy;

    int min_size = 50;
    if (new_w < min_size) new_w = min_size;
    if (new_h < min_size) new_h = min_size;

    if (resize.win_x + new_w > screen_width) {
        new_w = screen_width - resize.win_x;
    }
    if (resize.win_y + new_h > screen_height) {
        new_h = screen_height - resize.win_y;
    }

    XResizeWindow(dpy, resize.win, new_w, new_h);
}

static void handle_button_release(void) {
    if (drag.dragging) {
        physics.flying = 1;
        physics.win = drag.win;
        physics.vx = drag.vx * THROW_SPEED_MULTIPLIER;
        physics.vy = drag.vy * THROW_SPEED_MULTIPLIER;
    }

    drag.dragging = 0;
    resize.resizing = 0;
}

static void handle_key_press(XKeyEvent *ev) {
    KeyCode return_key = XKeysymToKeycode(dpy, XK_Return);
    if (ev->keycode == return_key && (ev->state & MOD_KEY)) {
        if (fork() == 0) {
            execlp("kitty", "kitty","-o", "background_opacity=1.0", NULL);
            perror("fwm: execlp failed");
            _exit(1);
        }
    }
}

static void physics_step(double dt) {
    if (!physics.flying) return;

    XWindowAttributes attr;
    XGetWindowAttributes(dpy, physics.win, &attr);

    int new_x = attr.x + (int)(physics.vx * dt);
    int new_y = attr.y + (int)(physics.vy * dt);

    physics.vx *= FRICTION;
    physics.vy *= FRICTION;

    double speed = physics.vx * physics.vx + physics.vy * physics.vy;
    if (speed < STOP_SPEED_THRESHOLD) {
        physics.flying = 0;
    }

    int min_x = -(attr.width - PHYSICS_MARGIN);
    int max_x = screen_width - PHYSICS_MARGIN - attr.width;
    int min_y = -(attr.height - PHYSICS_MARGIN);
    int max_y = screen_height - PHYSICS_MARGIN - attr.height;

    if (new_x < min_x) { new_x = min_x; physics.vx = -physics.vx * RESTITUTION; }
    if (new_x > max_x) { new_x = max_x; physics.vx = -physics.vx * RESTITUTION; }
    if (new_y < min_y) { new_y = min_y; physics.vy = -physics.vy * RESTITUTION; }
    if (new_y > max_y) { new_y = max_y; physics.vy = -physics.vy * RESTITUTION; }

    XMoveWindow(dpy, physics.win, new_x, new_y);
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Can't open display\n");
        return 1;
    }

    wm_init();
    int xfd = ConnectionNumber(dpy);
    fprintf(stderr, "fwm: init done, entering event loop\n");
    fflush(stderr);

    XEvent ev;
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
                XNextEvent(dpy, &ev);
                switch (ev.type) {
                    case MapRequest:
                        handle_map_request(&ev.xmaprequest);
                        break;
                    case ButtonPress:
                        handle_button_press(&ev.xbutton);
                        break;
                    case MotionNotify:
                        handle_motion(&ev.xmotion);
                        handle_resize_motion(&ev.xmotion);
                        break;
                    case ButtonRelease:
                        handle_button_release();
                        break;
                    case KeyPress:
                        handle_key_press(&ev.xkey);
                        break;
                    default:
                        break;
                }
            }
        } else if (ready == 0) {
            physics_step(1.0 / PHYSICS_TICK_RATE);
            fflush(stderr);
        }
    }

    XCloseDisplay(dpy);
    return 0;
}