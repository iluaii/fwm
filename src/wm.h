#ifndef FWM_WM_H
#define FWM_WM_H
#define VELOCITY_HISTORY 4
#define BLOCKEDWINDOWS 1

#include <X11/Xlib.h>
#include "physics.h"

typedef struct {
    int dragging;
    Window win;
    int start_x, start_y;
    int win_start_x, win_start_y;
    int win_width, win_height;
    int last_x, last_y;
    Time last_time;
    double vx, vy;
    int hist_x[VELOCITY_HISTORY];
    int hist_y[VELOCITY_HISTORY];
    Time hist_time[VELOCITY_HISTORY];
    int hist_count;
    int collision_disabled;
} DragState;

typedef struct {
    int resizing;
    Window win;
    int start_x, start_y;
    int win_x, win_y;
    int win_start_width, win_start_height;
} ResizeState;

typedef struct {
    Display *dpy;
    Window root;
    int screen_width;
    int screen_height;
    DragState drag;
    ResizeState resize;
    PhysicsWorld physics;
    Window last_touched_win;
    Window tray_win;
} Fwm;

void fwm_init(Fwm *wm, Display *dpy);
void fwm_handle_event(Fwm *wm, XEvent *ev);
void fwm_tick(Fwm *wm, double dt);


#endif /* FWM_WM_H */
