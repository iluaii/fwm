#ifndef FWM_WM_H
#define FWM_WM_H
#define VELOCITY_HISTORY 4
#define BLOCKEDWINDOWS 1

#include <X11/Xlib.h>

#include "bsp.h"
#include "physics.h"

extern int running;

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

typedef enum {
    DESKTOP_MODE_PHYSICS,
    DESKTOP_MODE_TILING,
    DESKTOP_MODE_NORMAL,
} DesktopMode;

typedef struct BspNode {
    struct BspNode *parent;
    struct BspNode *left;
    struct BspNode *right;
    Window win;
    int x, y, w, h;
    int split_h;
    float ratio;
} BspNode;

typedef struct {
    BspNode *node;
    float start_ratio;
    int start_x, start_y;
    int active;
} BspDragState;

typedef struct {
    int active;
    Window win;
    int start_x, start_y;
    int cur_x, cur_y;
} SwapDragState;

typedef struct {
    int active;
    BspNode *node;
    float start_ratio;
    int start_x, start_y;
} BspResizeState;

typedef struct {
    Display *dpy;
    Window root;
    int screen_width;
    int screen_height;
    DragState drag;
    ResizeState resize;
    PhysicsWorld physics;
    Window last_touched_win;
    Window focused_win;
    Window tray_win;
    int camera_x;
    int target_camera_x;
    int total_desktops;
    DesktopMode desktop_mode[10];
    Atom net_wm_state;
    Atom net_wm_state_fullscreen;
    BspNode *bsp_roots[10];
    BspDragState bsp_drag;
    SwapDragState swap_drag;
    BspResizeState bsp_resize;
} Fwm;


void fwm_init(Fwm *wm, Display *dpy);
void fwm_handle_event(Fwm *wm, XEvent *ev);
void fwm_tick(Fwm *wm, double dt);


#endif /* FWM_WM_H */
