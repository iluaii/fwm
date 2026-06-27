#include "wm.h"

#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "window.h"
#include "ui/decorations.h"
#include "ui/tray.h"
#include <X11/Xatom.h>
#include "config.h"
#include "ui/hints.h"

#define LOCK_MASKS (Mod2Mask | LockMask | Mod5Mask)

static FwmConfig cfg;

static void move_camera(Fwm *wm, const Arg *arg) {
    int new_target = wm->target_camera_x + arg->i;
    if (new_target < 0) new_target = 0;
    if (new_target > 9 * wm->screen_width) new_target = 9 * wm->screen_width;
    wm->target_camera_x = new_target;
}

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

static void show_hints(Fwm *wm, const Arg *arg) {
    (void)arg;
    hints_show(wm->dpy, wm->root, wm->screen_width, wm->screen_height);
}

static void EXIT(Fwm *wm, const Arg *arg) {
    (void)arg;
    (void)wm;
    running = 0;
}

static void cycle_gravity(Fwm *wm, const Arg *arg) {
    (void)arg;
    double g = wm->physics.gravity_scale;
    if (g == 0.0)       wm->physics.gravity_scale = 0.15;
    else if (g == 0.15) wm->physics.gravity_scale = 1.0;
    else                wm->physics.gravity_scale = 0.0;
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
    if (body) {
        int mode = (wm->desktop_mode[body->desktop_id] == DESKTOP_MODE_PHYSICS)
                   ? CORNER_CHAMFER : CORNER_SHARP;
        decorations_set_corner_mode(wm->dpy, body->win, &body->corner, mode, body->width, body->height);
    }
    XMapWindow(wm->dpy, event->window);
    XSelectInput(wm->dpy, event->window, EnterWindowMask | ButtonPressMask);

    if (body && wm->desktop_mode[body->desktop_id] == DESKTOP_MODE_PHYSICS)
        physics_push_overlapping(&wm->physics, event->window, 300.0);

    if (body && wm->desktop_mode[body->desktop_id] == DESKTOP_MODE_TILING) {
        bsp_insert(&wm->bsp_roots[body->desktop_id], wm->focused_win, event->window);
        apply_tiling(wm, body->desktop_id);
    }

    if (wm->tray_win != None) XRaiseWindow(wm->dpy, wm->tray_win);
}

static void pin_window(Fwm *wm, const Arg *arg) {
    (void)arg;
    if (wm->focused_win == None) return;
    PhysicsBody *pb = physics_find_body(&wm->physics, wm->focused_win);
    if (!pb) return;
    pb->pinned ^= 1;
    pb->vx = 0; pb->vy = 0; pb->flying = 0;
}

static void toggle_nocollide(Fwm *wm, const Arg *arg) {
    (void)arg;
    if (wm->focused_win == None) return;
    PhysicsBody *pb = physics_find_body(&wm->physics, wm->focused_win);
    if (!pb) return;
    pb->no_collide ^= 1;
}

static void calm_all(Fwm *wm, const Arg *arg) {
    (void)arg;
    for (int i = 0; i < wm->physics.body_count; i++) {
        PhysicsBody *b = &wm->physics.bodies[i];
        if (!b->active) continue;
        b->vx = 0; b->vy = 0; b->flying = 0;
    }
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

static void view(Fwm *wm, const Arg *arg) {
    if (arg->i < 0 || arg->i >= wm->total_desktops) return;
    wm->target_camera_x = arg->i * wm->screen_width;
}

static void toggle_tiling(Fwm *wm, const Arg *arg) {
    (void)arg;
    int d = wm->target_camera_x / wm->screen_width;
    if (wm->desktop_mode[d] == DESKTOP_MODE_PHYSICS) {
        wm->desktop_mode[d] = DESKTOP_MODE_TILING;

        for (int i = 0; i < wm->physics.body_count; i++) {
            PhysicsBody *b = &wm->physics.bodies[i];
            if (!b->active || b->desktop_id != d) continue;
            if (!b->tiling_saved) {
                b->sav_x = b->x; b->sav_y = b->y;
                b->sav_w = b->width; b->sav_h = b->height;
                b->tiling_saved = 1;
            }
        }

        bsp_free(wm->bsp_roots[d]);
        wm->bsp_roots[d] = NULL;
        for (int i = 0; i < wm->physics.body_count; i++) {
            PhysicsBody *b = &wm->physics.bodies[i];
            if (b->active && !b->shaped && !b->fullscreen && b->desktop_id == d)
                bsp_insert(&wm->bsp_roots[d], None, b->win);
        }
        apply_tiling(wm, d);
        for (int i = 0; i < wm->physics.body_count; i++) {
            PhysicsBody *b = &wm->physics.bodies[i];
            if (!b->active || b->desktop_id != d || b->shaped) continue;
            int mode = (wm->desktop_mode[d] == DESKTOP_MODE_PHYSICS)
                       ? CORNER_CHAMFER : CORNER_SHARP;
            decorations_set_corner_mode(wm->dpy, b->win, &b->corner, mode, b->width, b->height);
        }
    } else {
        wm->desktop_mode[d] = DESKTOP_MODE_PHYSICS;

        for (int i = 0; i < wm->physics.body_count; i++) {
            PhysicsBody *b = &wm->physics.bodies[i];
            if (!b->active || b->desktop_id != d) continue;
            if (b->tiling_saved) {
                b->x = b->sav_x; b->y = b->sav_y;
                b->width = b->sav_w; b->height = b->sav_h;
                b->tiling_saved = 0;
            } else {
                b->width = 800; b->height = 600;
                b->x = d * wm->screen_width + (wm->screen_width - b->width) / 2;
                b->y = (wm->screen_height - b->height) / 2;
            }
            b->vx = 0; b->vy = 0; b->flying = 0;
            XMoveResizeWindow(wm->dpy, b->win,
                              (int)lround(b->x - wm->camera_x), (int)lround(b->y),
                              b->width, b->height);
        }

        for (int i = 0; i < wm->physics.body_count; i++) {
            PhysicsBody *b = &wm->physics.bodies[i];
            if (!b->active || b->desktop_id != d) continue;
            double angle = ((double)(b->win % 628)) / 100.0;
            b->vx = cos(angle) * 200.0;
            b->vy = sin(angle) * 200.0;
            b->flying = 1;
        }
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

static void handle_button_press(Fwm *wm, XButtonEvent *event) {
    if (event->button == Button4) {
        int d = wm->target_camera_x / wm->screen_width;
        if (d > 0) wm->target_camera_x = (d - 1) * wm->screen_width;
        return;
    }
    if (event->button == Button5) {
        int d = wm->target_camera_x / wm->screen_width;
        if (d < 9) wm->target_camera_x = (d + 1) * wm->screen_width;
        return;
    }
    XAllowEvents(wm->dpy, ReplayPointer, event->time);
    Window target = event->subwindow != None ? event->subwindow : event->window;
    if (target == None) return;
    if (target == wm->tray_win) return;
    if (target == wm->root) return;

    WindowGeometry geometry;
    if (!window_get_geometry(wm->dpy, target, &geometry)) {
        return;
    }

    wm->last_touched_win = target;

    XRaiseWindow(wm->dpy, target);
    XSetInputFocus(wm->dpy, target, RevertToPointerRoot, CurrentTime);

    physics_sync_body(&wm->physics, target, geometry.x + wm->camera_x, geometry.y,
                      geometry.width, geometry.height, wm->screen_width);
    physics_stop_body(&wm->physics, target);

    if (event->button == Button1) {
        PhysicsBody *pb = physics_find_body(&wm->physics, target);
        if (pb && wm->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) {
            if (event->state & ShiftMask) {
                wm->swap_drag.active = 1;
                wm->swap_drag.win = target;
                wm->swap_drag.start_x = event->x_root;
                wm->swap_drag.start_y = event->y_root;
            }
            return;
        }

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
        if (pb && wm->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) {
            int d = pb->desktop_id;
            BspNode *node = bsp_find_border(wm->bsp_roots[d],
                                            event->x_root + wm->camera_x,
                                            event->y_root, 40);
            if (node) {
                wm->bsp_resize.active = 1;
                wm->bsp_resize.node = node;
                wm->bsp_resize.start_ratio = node->ratio;
                wm->bsp_resize.start_x = event->x_root;
                wm->bsp_resize.start_y = event->y_root;
            }
            return;
        }
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
    if (wm->swap_drag.active) {
        wm->swap_drag.cur_x = event->x_root;
        wm->swap_drag.cur_y = event->y_root;
        return;
    }
    if (wm->bsp_resize.active) {
        BspNode *n = wm->bsp_resize.node;
        float delta;
        if (!n->split_h)
            delta = (float)(event->x_root - wm->bsp_resize.start_x) / n->w;
        else
            delta = (float)(event->y_root - wm->bsp_resize.start_y) / n->h;
        n->ratio = wm->bsp_resize.start_ratio + delta;
        if (n->ratio < 0.1f) n->ratio = 0.1f;
        if (n->ratio > 0.9f) n->ratio = 0.9f;
        int d = wm->target_camera_x / wm->screen_width;
        apply_tiling(wm, d);
        return;
    }
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
                physics_push_overlapping(&wm->physics, wm->drag.win, 280.0);
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

    if (wm->swap_drag.active) {
        int root_x = wm->swap_drag.cur_x;
        int root_y = wm->swap_drag.cur_y;

        Window target_win = None;
        PhysicsBody *src_pb = physics_find_body(&wm->physics, wm->swap_drag.win);
        if (src_pb) {
            int d = src_pb->desktop_id;
            BspNode *leaves[MAX_WINDOWS];
            int count = 0;
            bsp_collect_leaves(wm->bsp_roots[d], leaves, &count);
            for (int i = 0; i < count; i++) {
                BspNode *n = leaves[i];
                int sx = n->x - wm->camera_x;
                if (root_x >= sx && root_x <= sx + n->w &&
                    root_y >= n->y && root_y <= n->y + n->h) {
                    target_win = n->win;
                    break;
                }
            }
        }

        if (target_win != None && target_win != wm->swap_drag.win && src_pb) {
            bsp_swap(wm->bsp_roots[src_pb->desktop_id], wm->swap_drag.win, target_win);
            apply_tiling(wm, src_pb->desktop_id);
        }

        wm->swap_drag.active = 0;
        wm->swap_drag.win = None;
    }

    wm->drag.dragging = 0;
    wm->resize.resizing = 0;
    wm->bsp_resize.active = 0;
    wm->bsp_resize.node = NULL;
}

static void forget_window(Fwm *wm, Window win) {
    PhysicsBody *pb = physics_find_body(&wm->physics, win);
    int desktop = -1;
    int was_tiling = 0;

    if (pb) {
        desktop = pb->desktop_id;
        was_tiling = (wm->desktop_mode[desktop] == DESKTOP_MODE_TILING);
        physics_remove_body(&wm->physics, win);
    } else {
        for (int i = 0; i < wm->total_desktops; i++) {
            if (bsp_find(wm->bsp_roots[i], win)) {
                desktop = i;
                was_tiling = (wm->desktop_mode[i] == DESKTOP_MODE_TILING);
                break;
            }
        }
    }

    if (win == wm->focused_win) wm->focused_win = None;
    if (win == wm->last_touched_win) wm->last_touched_win = None;

    for (int i = 0; i < wm->total_desktops; i++)
        bsp_remove(&wm->bsp_roots[i], win);

    if (was_tiling && desktop >= 0) {
        bsp_free(wm->bsp_roots[desktop]);
        wm->bsp_roots[desktop] = NULL;
        for (int i = 0; i < wm->physics.body_count; i++) {
            PhysicsBody *b = &wm->physics.bodies[i];
            if (b->active && !b->shaped && !b->fullscreen && b->desktop_id == desktop)
                bsp_insert(&wm->bsp_roots[desktop], None, b->win);
        }
        apply_tiling(wm, desktop);
    }
}

static void apply_tiling(Fwm *wm, int desktop) {
    int cx = desktop * wm->screen_width;
    int gap = 6;
    int top = TRAY_HEIGHT + gap * 2;
    int usable_h = wm->screen_height - top - gap;
    int usable_w = wm->screen_width - gap * 2;

    bsp_recalc(wm->bsp_roots[desktop], wm->dpy, wm->camera_x,
               cx + gap, top, usable_w, usable_h);

    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(wm->bsp_roots[desktop], leaves, &count);

    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        PhysicsBody *pb = physics_find_body(&wm->physics, n->win);
        if (!pb) continue;
        pb->x = n->x;
        pb->y = n->y;
        pb->width = n->w;
        pb->height = n->h;
        pb->vx = 0;
        pb->vy = 0;
        pb->flying = 0;
    }
}

static void dispatch_action(Fwm *wm, const char *action) {
    if (strcmp(action, "killclient") == 0) {
        killclient(wm, NULL);
    } else if (strcmp(action, "toggle_tiling") == 0) {
        toggle_tiling(wm, NULL);
    } else if (strcmp(action, "EXIT") == 0) {
        EXIT(wm, NULL);
    } else if (strcmp(action, "show_hints") == 0) {
        show_hints(wm, NULL);
    } else if (strcmp(action, "cycle_gravity") == 0) {
        cycle_gravity(wm, NULL);
    } else if (strcmp(action, "pin_window") == 0) {
        pin_window(wm, NULL);
    } else if (strcmp(action, "toggle_nocollide") == 0) {
        toggle_nocollide(wm, NULL);
    } else if (strcmp(action, "calm_all") == 0) {
        calm_all(wm, NULL);
    } else if (strcmp(action, "fake_fullscreen") == 0) {
        fake_fullscreen(wm, NULL);
    } else if (strcmp(action, "real_fullscreen") == 0) {
        real_fullscreen(wm, NULL);
    } else if (strncmp(action, "spawn:", 6) == 0) {
        const char *cmd = action + 6;
        const char *args[] = { "sh", "-c", cmd, NULL };
        Arg a = { .v = args };
        spawn(wm, &a);
    } else if (strncmp(action, "move_camera:", 12) == 0) {
        Arg a = { .i = atoi(action + 12) };
        move_camera(wm, &a);
    } else if (strncmp(action, "view:", 5) == 0) {
        Arg a = { .i = atoi(action + 5) };
        view(wm, &a);
    }
}

static void handle_key_press(Fwm *wm, XKeyEvent *event) {
    unsigned int state = event->state & ~LOCK_MASKS;

    if (wm->focused_win == None && event->subwindow != None)
        wm->focused_win = event->subwindow;

    for (int i = 0; i < cfg.key_count; i++) {
        KeyCode code = XKeysymToKeycode(wm->dpy, cfg.keys[i].key);
        if (event->keycode == code && state == cfg.keys[i].mod) {
            dispatch_action(wm, cfg.keys[i].action);
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

    XSetErrorHandler(xerror_handler);
    XSelectInput(dpy, wm->root,
             SubstructureRedirectMask | SubstructureNotifyMask |
             StructureNotifyMask | EnterWindowMask);

    char path[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path), "%s%s", home, FWM_CONFIG_PATH);
    } else {
        snprintf(path, sizeof(path), ".config/fwm/config.toml");
    }
    config_load(&cfg, path);

    wm->physics.friction = cfg.physics.friction;
    wm->physics.mass_density = cfg.physics.mass_density;
    wm->physics.throw_speed_multiplier = cfg.physics.throw_speed_multiplier;
    wm->physics.max_throw_speed = cfg.physics.max_throw_speed;
    wm->physics.stop_speed_threshold = cfg.physics.stop_speed_threshold;
    wm->physics.restitution = cfg.physics.restitution;
    wm->physics.gravity = cfg.physics.gravity;

    for (int i = 0; i < cfg.key_count; i++) {
        KeyCode code = XKeysymToKeycode(dpy, cfg.keys[i].key);
        grab_key(dpy, wm->root, code, cfg.keys[i].mod);
    }

    unsigned int mouse_mod = Mod4Mask;
    for (int i = 0; i < cfg.key_count; i++) {
        if (cfg.keys[i].mod & Mod4Mask) { mouse_mod = Mod4Mask; break; }
        if (cfg.keys[i].mod & Mod1Mask) { mouse_mod = Mod1Mask; break; }
    }

    grab_button(dpy, wm->root, Button1, mouse_mod);
    grab_button(dpy, wm->root, Button1, mouse_mod | ShiftMask);
    grab_button(dpy, wm->root, Button3, mouse_mod);
    grab_button(dpy, wm->root, Button4, mouse_mod);
    grab_button(dpy, wm->root, Button5, mouse_mod);

    XSync(dpy, False);
}

static void handle_enter_notify(Fwm *wm, XCrossingEvent *event) {
    Window target = event->window;
    if (target == wm->root) target = event->subwindow;
    if (target == None || target == wm->root) return;
    if (target == wm->tray_win) return;
    if (event->mode != NotifyNormal || event->detail == NotifyInferior) return;
    if (wm->drag.dragging || wm->resize.resizing) return;

    XWindowAttributes attrs;
    if (XGetWindowAttributes(wm->dpy, target, &attrs)) {
        if (attrs.override_redirect) return;
        if (attrs.class == InputOnly) return;
    }

    if (!physics_find_body(&wm->physics, target)) return;

    Window prev_focused = wm->focused_win;
    wm->focused_win = target;
    XSetInputFocus(wm->dpy, target, RevertToPointerRoot, CurrentTime);
    PhysicsBody *pb = physics_find_body(&wm->physics, target);
    if (pb) {
        decorations_set_corner_mode(wm->dpy, pb->win, &pb->corner, CORNER_ROUND, pb->width, pb->height);
        if (prev_focused != None && prev_focused != target) {
            PhysicsBody *old = physics_find_body(&wm->physics, prev_focused);
            if (old) {
                int mode = (wm->desktop_mode[old->desktop_id] == DESKTOP_MODE_PHYSICS)
                           ? CORNER_CHAMFER : CORNER_SHARP;
                decorations_set_corner_mode(wm->dpy, old->win, &old->corner, mode, old->width, old->height);
            }
        }
    }
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

        if (pb->shaped) {
            decorations_apply_chamfer(wm->dpy, event->window, pb->width, pb->height,
                                      chamfer_cut(pb->width, pb->height));
        } else {
            decorations_set_corner_mode(wm->dpy, event->window, &pb->corner,
                                        pb->corner.mode, pb->width, pb->height);
        }
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
    Window dragged_win = wm->drag.dragging ? wm->drag.win : None;
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
        if (body->active && body->win != dragged_win && !body->pinned) {
            XMoveWindow(wm->dpy, body->win, (int)lround(body->x - wm->camera_x), (int)lround(body->y));
        }
    }

    if (!wm->drag.dragging && !wm->resize.resizing) {
        Window root_ret, child;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        XQueryPointer(wm->dpy, wm->root, &root_ret, &child,
                      &root_x, &root_y, &win_x, &win_y, &mask);
        if (child != None && child != wm->tray_win && child != wm->focused_win) {
            if (physics_find_body(&wm->physics, child)) {
                wm->focused_win = child;
                XSetInputFocus(wm->dpy, child, RevertToPointerRoot, CurrentTime);
            }
        }
    }

    {
        char keys_down[32];
        XQueryKeymap(wm->dpy, keys_down);

        KeyCode left  = XKeysymToKeycode(wm->dpy, XK_Left);
        KeyCode right = XKeysymToKeycode(wm->dpy, XK_Right);
        KeyCode super = XKeysymToKeycode(wm->dpy, XK_Super_L);

        int mod_down = keys_down[super / 8] & (1 << (super % 8));

        if (mod_down) {
            if (keys_down[left / 8] & (1 << (left % 8))) {
                int n = wm->target_camera_x - 20;
                if (n < 0) n = 0;
                wm->target_camera_x = n;
            }
            if (keys_down[right / 8] & (1 << (right % 8))) {
                int n = wm->target_camera_x + 20;
                if (n > 9 * wm->screen_width) n = 9 * wm->screen_width;
                wm->target_camera_x = n;
            }
        }
    }

    if (!wm->drag.dragging && !wm->resize.resizing) {
        Window root_ret, child;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        XQueryPointer(wm->dpy, wm->root, &root_ret, &child,
                      &root_x, &root_y, &win_x, &win_y, &mask);
        if (child != None && child != wm->tray_win && child != wm->focused_win) {
            PhysicsBody *nb = physics_find_body(&wm->physics, child);
            if (nb) {
                Window prev = wm->focused_win;
                wm->focused_win = child;
                XSetInputFocus(wm->dpy, child, RevertToPointerRoot, CurrentTime);
                decorations_set_corner_mode(wm->dpy, nb->win, &nb->corner, CORNER_ROUND, nb->width, nb->height);
                if (prev != None) {
                    PhysicsBody *ob = physics_find_body(&wm->physics, prev);
                    if (ob && !ob->shaped) {
                        int mode = (wm->desktop_mode[ob->desktop_id] == DESKTOP_MODE_PHYSICS)
                                   ? CORNER_CHAMFER : CORNER_SHARP;
                        decorations_set_corner_mode(wm->dpy, ob->win, &ob->corner, mode, ob->width, ob->height);
                    }
                }
            }
        }
    }
    XFlush(wm->dpy);
}