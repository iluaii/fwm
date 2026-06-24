#include "wm.h"

#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "window.h"
#include "ui/decorations.h"
#include "ui/tray.h"
#include "config.h"
#include <X11/Xatom.h>

#define LOCK_MASKS (Mod2Mask | LockMask | Mod5Mask)

static int xerror_handler(Display *dpy, XErrorEvent *event) {
    char msg[256];
    XGetErrorText(dpy, event->error_code, msg, sizeof(msg));
    fprintf(stderr, "X11 error: %s(%d)\n", msg, event->error_code);
    return 0;
}

static void grab_key(Display *dpy, Window root, KeyCode code, unsigned int mod) {
    static const unsigned int locks[] = { 0, Mod2Mask, LockMask, Mod5Mask,
                                          Mod2Mask|LockMask, Mod2Mask|Mod5Mask,
                                          LockMask|Mod5Mask, Mod2Mask|LockMask|Mod5Mask };
    for (size_t i = 0; i < sizeof(locks)/sizeof(locks[0]); i++)
        XGrabKey(dpy, code, mod | locks[i], root, False, GrabModeAsync, GrabModeAsync);
}

static void grab_button(Display *dpy, Window root, unsigned int button, unsigned int mod) {
    static const unsigned int locks[] = { 0, Mod2Mask, LockMask, Mod5Mask,
                                          Mod2Mask|LockMask, Mod2Mask|Mod5Mask,
                                          LockMask|Mod5Mask, Mod2Mask|LockMask|Mod5Mask };
    for (size_t i = 0; i < sizeof(locks)/sizeof(locks[0]); i++)
        XGrabButton(dpy, button, mod | locks[i], root, True,
                    ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                    GrabModeAsync, GrabModeAsync, None, None);
}

static void apply_tiling(Fwm *wm, int desktop);

static int window_is_rofi(Display *dpy, Window win) {
    XClassHint ch = { NULL, NULL };
    int rofi = 0;
    if (XGetClassHint(dpy, win, &ch)) {
        if ((ch.res_class && strcmp(ch.res_class, "Rofi") == 0) ||
            (ch.res_name && strcmp(ch.res_name, "rofi") == 0))
            rofi = 1;
        if (ch.res_class) XFree(ch.res_class);
        if (ch.res_name) XFree(ch.res_name);
    }
    return rofi;
}

static int chamfer_cut(int width, int height) {
    int smaller = width < height ? width : height;
    return smaller / 6;
}

static void handle_map_request(Fwm *wm, XMapRequestEvent *event) {
    if (window_is_rofi(wm->dpy, event->window)) {
        WindowGeometry g;
        if (!window_get_geometry(wm->dpy, event->window, &g)) return;

        int x = (wm->screen_width - g.width) / 2;
        int y = (wm->screen_height - g.height) / 2;
        XMoveWindow(wm->dpy, event->window, x, y);
        XMapWindow(wm->dpy, event->window);

        PhysicsBody *body = physics_sync_body(&wm->physics, event->window,
                                              x + wm->camera_x, y,
                                              g.width, g.height, wm->screen_width);
        XSelectInput(wm->dpy, event->window, EnterWindowMask | ButtonPressMask);
        if (body) body->shaped = 1;

        decorations_apply_chamfer(wm->dpy, event->window, g.width, g.height,
                                  chamfer_cut(g.width, g.height));
        if (wm->tray_win != None) XRaiseWindow(wm->dpy, wm->tray_win);
        return;
    }

    WindowGeometry geometry;
    window_map_centered(wm->dpy, event->window, wm->screen_width, wm->screen_height, &geometry);
    PhysicsBody *body = physics_sync_body(&wm->physics, event->window,
                                          geometry.x + wm->camera_x, geometry.y,
                                          geometry.width, geometry.height, wm->screen_width);
    XSelectInput(wm->dpy, event->window, EnterWindowMask | ButtonPressMask);

    if (body && wm->desktop_mode[body->desktop_id] == DESKTOP_MODE_TILING)
        apply_tiling(wm, body->desktop_id);

    if (wm->tray_win != None) XRaiseWindow(wm->dpy, wm->tray_win);
}

static void handle_button_press(Fwm *wm, XButtonEvent *event) {
    Window target = event->subwindow != None ? event->subwindow : event->window;
    if (target == None) return;
    if (target == wm->tray_win) return;
    if (target == wm->root) return;

    WindowGeometry geometry;
    if (!window_get_geometry(wm->dpy, target, &geometry)) {
        return;
    }

    if (wm->last_touched_win != None && wm->last_touched_win != target) {
        decorations_draw_border(wm->dpy, wm->last_touched_win, 0);
    }

    wm->last_touched_win = target;
    //decorations_draw_border(wm->dpy, target, 1);

    XRaiseWindow(wm->dpy, target);

    XSetInputFocus(wm->dpy, target, RevertToPointerRoot, CurrentTime);

    physics_sync_body(&wm->physics, target, geometry.x + wm->camera_x, geometry.y,
                      geometry.width, geometry.height, wm->screen_width);
    physics_stop_body(&wm->physics, target);

    if (event->button == Button1) {
        PhysicsBody *pb = physics_find_body(&wm->physics, target);
        if (pb && wm->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) return;

        wm->drag.dragging = 1;
        wm->drag.hist_count = 0;
        wm->drag.win = target;
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
    } else if (event->button == Button3) {
        PhysicsBody *pb = physics_find_body(&wm->physics, target);
        if (pb && wm->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) return;

        wm->resize.resizing = 1;
        wm->resize.win = target;
        wm->resize.start_x = event->x_root;
        wm->resize.start_y = event->y_root;
        wm->resize.win_x = geometry.x;
        wm->resize.win_y = geometry.y;
        wm->resize.win_start_width = geometry.width;
        wm->resize.win_start_height = geometry.height;
    }

    if (wm->tray_win != None) XRaiseWindow(wm->dpy, wm->tray_win);
}

static void handle_drag_motion(Fwm *wm, XMotionEvent *event) {
    if (!wm->drag.dragging) return;

    if (wm->camera_x == wm->target_camera_x) {
        int current_d = wm->target_camera_x / wm->screen_width;
        if (event->x_root >= wm->screen_width - 10 && current_d < 9) {
            wm->target_camera_x = (current_d + 1) * wm->screen_width;
        } else if (event->x_root <= 10 && current_d > 0) {
            wm->target_camera_x = (current_d - 1) * wm->screen_width;
        }
    }

    int dx = event->x_root - wm->drag.start_x;
    int dy = event->y_root - wm->drag.start_y;

    int min_world_x = -(wm->drag.win_width - DRAG_MARGIN);
    int max_world_x = 10 * wm->screen_width - DRAG_MARGIN - wm->drag.win_width;

    int target_world_x = wm->drag.win_start_x + wm->camera_x + dx;
    int target_world_y = wm->drag.win_start_y + dy;

    if (target_world_x < min_world_x) target_world_x = min_world_x;
    if (target_world_x > max_world_x) target_world_x = max_world_x;

    int min_y = -(wm->drag.win_height - DRAG_MARGIN);
    int max_y = wm->screen_height - DRAG_MARGIN - wm->drag.win_height;
    if (target_world_y < min_y) target_world_y = min_y;
    if (target_world_y > max_y) target_world_y = max_y;

    int new_x = target_world_x - wm->camera_x;
    int new_y = target_world_y;

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
    XFlush(wm->dpy);
    physics_sync_body(&wm->physics, wm->drag.win, target_world_x, new_y,
                      wm->drag.win_width, wm->drag.win_height, wm->screen_width);
}

static void handle_resize_motion(Fwm *wm, XMotionEvent *event) {
    if (!wm->resize.resizing) return;

    int dx = event->x_root - wm->resize.start_x;
    int dy = event->y_root - wm->resize.start_y;

    int new_w = wm->resize.win_start_width + dx;
    int new_h = wm->resize.win_start_height + dy;
    int max_w = wm->screen_width - wm->resize.win_x;
    int max_h = wm->screen_height - wm->resize.win_y;

    if (new_w < 50) new_w = 50;
    if (new_h < 50) new_h = 50;
    if (max_w < 50) max_w = 50;
    if (max_h < 50) max_h = 50;

    if (new_w > max_w) new_w = max_w;
    if (new_h > max_h) new_h = max_h;

    XResizeWindow(wm->dpy, wm->resize.win, new_w, new_h);
    XFlush(wm->dpy);
    physics_sync_body(&wm->physics, wm->resize.win,
                      wm->resize.win_x + wm->camera_x, wm->resize.win_y,
                      new_w, new_h, wm->screen_width);
}
static void handle_button_release(Fwm *wm) {
    if (wm->drag.dragging) {
        WindowGeometry geometry;
        if (window_get_geometry(wm->dpy, wm->drag.win, &geometry)) {
            physics_sync_body(&wm->physics, wm->drag.win, geometry.x + wm->camera_x, geometry.y,
                              geometry.width, geometry.height, wm->screen_width);
            PhysicsBody *pb = physics_find_body(&wm->physics, wm->drag.win);
            if (pb && wm->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) {
                apply_tiling(wm, pb->desktop_id);
            } else {
                physics_throw_body(&wm->physics, wm->drag.win, wm->drag.vx, wm->drag.vy);
            }
        }
    }

    if (wm->resize.resizing) {
        WindowGeometry geometry;
        if (window_get_geometry(wm->dpy, wm->resize.win, &geometry)) {
            physics_sync_body(&wm->physics, wm->resize.win,
                              geometry.x + wm->camera_x, geometry.y,
                              geometry.width, geometry.height, wm->screen_width);
        }
    }

    wm->drag.dragging = 0;
    wm->resize.resizing = 0;
}

static void spawn(Fwm *wm, const Arg *arg) {
    (void)wm;
    window_spawn(arg->v);
}

static void killclient(Fwm *wm, const Arg *arg) {
    (void)arg;
    if (wm->focused_win == None) return;
    PhysicsBody *pb = physics_find_body(&wm->physics, wm->focused_win);
    int desktop = pb ? pb->desktop_id : -1;
    physics_remove_body(&wm->physics, wm->focused_win);
    XKillClient(wm->dpy, wm->focused_win);
    wm->focused_win = None;
    if (desktop >= 0 && wm->desktop_mode[desktop] == DESKTOP_MODE_TILING)
        apply_tiling(wm, desktop);
}

static void forget_window(Fwm *wm, Window win) {
    PhysicsBody *pb = physics_find_body(&wm->physics, win);
    if (!pb) return;
    int desktop = pb->desktop_id;
    int was_tiling = (wm->desktop_mode[desktop] == DESKTOP_MODE_TILING);
    physics_remove_body(&wm->physics, win);
    if (win == wm->focused_win) wm->focused_win = None;
    if (win == wm->last_touched_win) wm->last_touched_win = None;
    if (was_tiling) apply_tiling(wm, desktop);
}

static void apply_tiling(Fwm *wm, int desktop) {
    int cx = desktop * wm->screen_width;
    int gap = 6;
    int top = TRAY_HEIGHT + gap * 2;
    int usable_h = wm->screen_height - top - gap;
    int usable_w = wm->screen_width - gap * 2;

    PhysicsBody *tiles[MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < wm->physics.body_count; i++) {
        PhysicsBody *b = &wm->physics.bodies[i];
        if (b->active && !b->fullscreen && !b->shaped && b->desktop_id == desktop)
            tiles[n++] = b;
    }
    if (n == 0) return;

    if (n == 1) {
        int x = cx + gap, y = top, w = usable_w, h = usable_h;
        tiles[0]->x = x; tiles[0]->y = y;
        tiles[0]->width = w; tiles[0]->height = h;
        tiles[0]->vx = 0; tiles[0]->vy = 0; tiles[0]->flying = 0;
        XMoveResizeWindow(wm->dpy, tiles[0]->win,
                          x - wm->camera_x, y, w, h);
    } else {
        int master_w = (int)(usable_w * wm->master_ratio) - gap / 2;
        int stack_w  = usable_w - master_w - gap;
        int stack_x  = cx + gap + master_w + gap;
        int stack_count = n - 1;
        int slot_h = (usable_h - gap * (stack_count - 1)) / stack_count;
        if (slot_h < 50) slot_h = 50;

        tiles[0]->x = cx + gap; tiles[0]->y = top;
        tiles[0]->width = master_w; tiles[0]->height = usable_h;
        tiles[0]->vx = 0; tiles[0]->vy = 0; tiles[0]->flying = 0;
        XMoveResizeWindow(wm->dpy, tiles[0]->win,
                          (cx + gap) - wm->camera_x, top, master_w, usable_h);

        for (int i = 1; i < n; i++) {
            int sy = top + (i - 1) * (slot_h + gap);
            tiles[i]->x = stack_x; tiles[i]->y = sy;
            tiles[i]->width = stack_w; tiles[i]->height = slot_h;
            tiles[i]->vx = 0; tiles[i]->vy = 0; tiles[i]->flying = 0;
            XMoveResizeWindow(wm->dpy, tiles[i]->win,
                              stack_x - wm->camera_x, sy, stack_w, slot_h);
        }
    }
}

static void setmaster(Fwm *wm, const Arg *arg) {
    int d = wm->target_camera_x / wm->screen_width;
    wm->master_ratio += arg->f;
    if (wm->master_ratio > 0.85) wm->master_ratio = 0.85;
    if (wm->master_ratio < 0.15) wm->master_ratio = 0.15;
    if (wm->desktop_mode[d] == DESKTOP_MODE_TILING) apply_tiling(wm, d);
}

static void view(Fwm *wm, const Arg *arg) {
    if (arg->i < 0 || arg->i >= wm->total_desktops) return;
    wm->target_camera_x = arg->i * wm->screen_width;
}

static void toggle_tiling(Fwm *wm, const Arg *arg) {
    (void)arg;
    int d = wm->target_camera_x / wm->screen_width;
    if (wm->desktop_mode[d] == DESKTOP_MODE_PHYSICS) {
        wm->desktop_mode[d] = DESKTOP_MODE_TILING;
        apply_tiling(wm, d);
    } else {
        wm->desktop_mode[d] = DESKTOP_MODE_PHYSICS;
    }
}

static void apply_fullscreen(Fwm *wm, int mode) {
    PhysicsBody *b = physics_find_body(&wm->physics, wm->focused_win);
    if (!b) return;
    int d = b->desktop_id;

    if (b->fullscreen == mode) {
        b->x = b->sav_x; b->y = b->sav_y;
        b->width = b->sav_w; b->height = b->sav_h;
        b->fullscreen = 0;

        XDeleteProperty(wm->dpy, b->win, wm->net_wm_state);

        XMoveResizeWindow(wm->dpy, b->win,
                          (int)lround(b->x - wm->camera_x), (int)lround(b->y),
                          b->width, b->height);
        if (wm->desktop_mode[d] == DESKTOP_MODE_TILING) apply_tiling(wm, d);
        if (wm->tray_win != None) XRaiseWindow(wm->dpy, wm->tray_win);
        return;
    }

    if (b->fullscreen == 0) {
        b->sav_x = b->x; b->sav_y = b->y;
        b->sav_w = b->width; b->sav_h = b->height;
    }

    b->flying = 0; b->vx = 0; b->vy = 0;
    b->fullscreen = mode;

    XSizeHints hints = {0};
    hints.flags = PMinSize | PResizeInc | PBaseSize;
    hints.min_width = 1; hints.min_height = 1;
    hints.width_inc = 1; hints.height_inc = 1;
    XSetNormalHints(wm->dpy, b->win, &hints);

    XChangeProperty(wm->dpy, b->win,
                    wm->net_wm_state, XA_ATOM, 32,
                    PropModeReplace,
                    (unsigned char *)&wm->net_wm_state_fullscreen, 1);

    int cx = d * wm->screen_width;
    if (mode == 1) {
        b->x = cx; b->y = TRAY_HEIGHT + 20;
        b->width = wm->screen_width;
        b->height = wm->screen_height - TRAY_HEIGHT;
    } else {
        b->x = cx; b->y = 0;
        b->width = wm->screen_width;
        b->height = wm->screen_height;
    }

    XMoveResizeWindow(wm->dpy, b->win,
                      (int)lround(b->x - wm->camera_x), (int)lround(b->y),
                      b->width, b->height);
    XRaiseWindow(wm->dpy, b->win);
    if (mode == 1 && wm->tray_win != None) XRaiseWindow(wm->dpy, wm->tray_win);
}

static void fake_fullscreen(Fwm *wm, const Arg *arg) {
    (void)arg;
    apply_fullscreen(wm, 1);
}

static void real_fullscreen(Fwm *wm, const Arg *arg) {
    (void)arg;
    apply_fullscreen(wm, 2);
}

static void handle_key_press(Fwm *wm, XKeyEvent *event) {
    unsigned int state = event->state & ~LOCK_MASKS;

    if (wm->focused_win == None && event->subwindow != None)
        wm->focused_win = event->subwindow;

    for (size_t i = 0; i < LENGTH(keys); i++) {
        KeyCode code = XKeysymToKeycode(wm->dpy, keys[i].key);
        if (event->keycode == code && state == keys[i].mod) {
            keys[i].func(wm, &keys[i].arg);
            return;
        }
    }
}

void fwm_init(Fwm *wm, Display *dpy) {
    memset(wm, 0, sizeof(*wm));
    wm->dpy = dpy;
    wm->root = XDefaultRootWindow(dpy);
    wm->screen_width = DisplayWidth(dpy, DefaultScreen(dpy));
    wm->screen_height = DisplayHeight(dpy, DefaultScreen(dpy));
    wm->net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    wm->net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    physics_init(&wm->physics);

    wm->camera_x = 0;
    wm->target_camera_x = 0;
    wm->total_desktops = 10;
    wm->master_ratio = 0.5;

    XSetErrorHandler(xerror_handler);
    XSelectInput(dpy, wm->root,
                 SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask);

    for (size_t i = 0; i < LENGTH(keys); i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].key);
        grab_key(dpy, wm->root, code, keys[i].mod);
    }

    grab_button(dpy, wm->root, Button1, MOD_KEY);
    grab_button(dpy, wm->root, Button1, MOD_KEY | ShiftMask);
    grab_button(dpy, wm->root, Button3, MOD_KEY);

    XSync(dpy, False);
}

static void handle_enter_notify(Fwm *wm, XCrossingEvent *event) {
    if (event->window == wm->tray_win) return;
    if (event->mode != NotifyNormal || event->detail == NotifyInferior) return;
    if (wm->drag.dragging || wm->resize.resizing) return;

    wm->focused_win = event->window;
    XSetInputFocus(wm->dpy, event->window, RevertToPointerRoot, CurrentTime);
}

static void handle_unmap_notify(Fwm *wm, XUnmapEvent *event) {
    forget_window(wm, event->window);
}

static void handle_destroy_notify(Fwm *wm, XDestroyWindowEvent *event) {
    forget_window(wm, event->window);
}

static void handle_configure_request(Fwm *wm, XConfigureRequestEvent *event) {
    PhysicsBody *pb = physics_find_body(&wm->physics, event->window);

    if (pb && wm->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) {
        XConfigureEvent ce = {0};
        ce.type = ConfigureNotify;
        ce.display = wm->dpy;
        ce.event = event->window;
        ce.window = event->window;
        ce.x = (int)lround(pb->x - wm->camera_x);
        ce.y = (int)lround(pb->y);
        ce.width = pb->width;
        ce.height = pb->height;
        ce.border_width = 0;
        ce.above = None;
        ce.override_redirect = False;
        XSendEvent(wm->dpy, event->window, False, StructureNotifyMask, (XEvent *)&ce);
        return;
    }

    XWindowChanges changes;
    changes.x = event->x;
    changes.y = event->y;
    changes.width = event->width;
    changes.height = event->height;
    changes.border_width = event->border_width;
    changes.sibling = event->above;
    changes.stack_mode = event->detail;
    XConfigureWindow(wm->dpy, event->window, event->value_mask, &changes);

    if (pb) {
        if (event->value_mask & CWWidth) pb->width = event->width;
        if (event->value_mask & CWHeight) pb->height = event->height;
        if (pb->shaped)
            decorations_apply_chamfer(wm->dpy, event->window, pb->width, pb->height,
                                      chamfer_cut(pb->width, pb->height));
    }
}
static void handle_client_message(Fwm *wm, XClientMessageEvent *event) {
    if (event->message_type != wm->net_wm_state) return;

    int action = event->data.l[0];
    Atom prop1 = (Atom)event->data.l[1];
    Atom prop2 = (Atom)event->data.l[2];

    if (prop1 != wm->net_wm_state_fullscreen &&
        prop2 != wm->net_wm_state_fullscreen) return;

    wm->focused_win = event->window;
    PhysicsBody *b = physics_find_body(&wm->physics, event->window);
    if (!b) return;

    int is_fs = (b->fullscreen != 0);
    int want_fs;
    if (action == 1) want_fs = 1;
    else if (action == 0) want_fs = 0;
    else want_fs = !is_fs;

    if (want_fs && !is_fs)
        apply_fullscreen(wm, 2);
    else if (!want_fs && is_fs)
        apply_fullscreen(wm, b->fullscreen);
}
void fwm_handle_event(Fwm *wm, XEvent *event) {
    switch (event->type) {
        case MapRequest:
            handle_map_request(wm, &event->xmaprequest);
            break;
        case ConfigureRequest:
            handle_configure_request(wm, &event->xconfigurerequest);
            break;
        case UnmapNotify:
            handle_unmap_notify(wm, &event->xunmap);
            break;
        case DestroyNotify:
            handle_destroy_notify(wm, &event->xdestroywindow);
            break;
        case ButtonPress:
            handle_button_press(wm, &event->xbutton);
            break;
        case MotionNotify:
            while (XPending(wm->dpy)) {
                XEvent next;
                XPeekEvent(wm->dpy, &next);
                if (next.type != MotionNotify) break;
                XNextEvent(wm->dpy, event);
            }
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
        case ClientMessage:
            handle_client_message(wm, &event->xclient);
            break;
        default:
            break;
    }
}

void fwm_tick(Fwm *wm, double dt) {
    Window drag_window = (wm->drag.dragging && wm->drag.collision_disabled) ? wm->drag.win : None;
    //Window resize_window = wm->resize.resizing ? wm->resize.win : None;
    Window dragged_win = wm->drag.dragging ? wm->drag.win : None;

    /*if (wm->resize.resizing) {
        XFlush(wm->dpy);
        return;
    }*/

    if (wm->resize.resizing) {
        PhysicsBody *rb = physics_find_body(&wm->physics, wm->resize.win);
        if (rb) { rb->flying = 0; rb->vx = 0; rb->vy = 0; }
    }

    if (wm->drag.dragging) {
        physics_set_velocity(&wm->physics, wm->drag.win, wm->drag.vx, wm->drag.vy);
    }

    if (wm->camera_x != wm->target_camera_x) {
        double diff = wm->target_camera_x - wm->camera_x;
        int step = (int)lround(diff * 10.0 * dt);
        if (step == 0) {
            step = diff > 0 ? 1 : -1;
        }
        if (abs(step) >= fabs(diff)) {
            wm->camera_x = wm->target_camera_x;
        } else {
            wm->camera_x += step;
        }

        for (int i = 0; i < wm->physics.body_count; i++) {
            PhysicsBody *body = &wm->physics.bodies[i];
            if (body->active && body->win != dragged_win) {
                XMoveWindow(wm->dpy, body->win, (int)lround(body->x - wm->camera_x), (int)lround(body->y));
            }
        }
    }

    physics_step(&wm->physics, wm->dpy, wm->screen_width, wm->screen_height,
             wm->camera_x,
             drag_window, None, dragged_win, dt);

    for (int i = 0; i < wm->physics.body_count; i++) {
        PhysicsBody *body = &wm->physics.bodies[i];
        if (body->active && wm->desktop_mode[body->desktop_id] == DESKTOP_MODE_TILING) {
            body->vx = 0;
            body->vy = 0;
            body->flying = 0;
        }
    }

    XFlush(wm->dpy);
}
