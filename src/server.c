#include "server.h"
#include "view.h"
#include "physics.h"
#include "bsp.h"
#include "theme.h"
#include "layer.h"
#include "lock.h"
#include "foreign.h"
#include "ipc.h"
#include "session.h"
#include <signal.h>
#include "ui/tray.h"
#include "ui/hints.h"
#include "ui/errors.h"
#include "ui/welcome.h"
#include "ui/launcher.h"
#include "ui/cairo_overlay.h"
#include "wallpaper.h"
#include "group.h"
#include "server_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/render/color.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* SIGTERM/SIGINT arrive when the session ends or a dev run is killed. Without
 * these the process dies where it stands, server_destroy never runs, and the
 * control socket is left behind as a dead file. Terminating the display loop
 * instead makes the normal teardown path the only exit path. */
static int handle_signal(int signal, void *data) {
    FwmServer *server = data;
    wlr_log(WLR_INFO, "caught signal %d, shutting down", signal);
    server->running = 0;
    wl_display_terminate(server->wl_display);
    return 0;
}



struct FwmDecoration {
    struct wlr_xdg_toplevel_decoration_v1 *deco;
    struct wl_listener destroy;
    struct wl_listener commit;
};


/* ── Xwayland ─────────────────────────────────────────────────────────────
 * Managed X11 windows become regular FwmViews (view_xwl_create). Override-
 * redirect surfaces (menus, tooltips, DnD icons) position themselves in X11
 * root coordinates — which match our screen coordinates, because managed X
 * windows are always configured at screen coords (view_set_size). They get a
 * bare scene surface, no physics/borders/focus. */

struct FwmXwlUnmanaged {
    struct wlr_xwayland_surface *xs;
    FwmServer *server;
    struct wlr_scene_tree *tree;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener destroy;
    struct wl_listener set_geometry;
    struct wl_listener map;
    struct wl_listener unmap;
};


// xdg popups (context/dropdown menus). wlr_scene_xdg_surface_create on the
// toplevel does NOT cover its popups — each popup needs its own scene tree
// parented into the parent surface's tree, or the menu is invisible while
// its input still works (clicks land through view_at's scene lookup).
struct FwmPopup {
    struct wlr_xdg_popup *popup;
    FwmServer *server;
    struct wl_listener commit;
    struct wl_listener destroy;
};


/* Impacts only matter if the user can see them: a window landing on desktop 7
 * must not shake the view of someone working on desktop 0. */
#define SHAKE_MAX_PX      14.0
#define SHAKE_FULL_SPEED  2000.0  /* px/s that produces a full-strength shake */
#define SHAKE_DECAY       9.0     /* 1/s; ~0.3s until it dies out */

/* Peak deformation for a window taking `speed`. The shake wants gentle impacts
 * damped because it is intrusive; the squash wants them SEEN, so the curve
 * bends the other way — squaring it (measured) turned an ordinary landing at
 * ~600 px/s into a 3.7% dent.
 *
 * Linear was still too flat at the low end: window-on-window contacts in real
 * use land around 200-300 px/s (measured in a nested run: 0.053 and 0.067,
 * i.e. 5-7%, which reads as "there is no animation between windows"), while
 * only a long fall onto the floor reaches four figures. sqrt lifts the gentle
 * half of the range without touching the cap: 200 px/s -> 14%, 500 -> 22%,
 * 900+ -> the full 30%. */
#define SQUASH_FULL_SPEED 900.0
#define SQUASH_MAX_AMOUNT 0.24

static void server_squash_from_impact(FwmServer *server, uint32_t id,
                                      double nx, double ny, double speed) {
    double strength = server->config.effects.squash;
    if (strength <= 0.0 || id == 0) return;  /* id 0 is a wall, nothing to squash */
    FwmView *v = server_find_view(server, id);
    if (!v) return;

    double f = speed / SQUASH_FULL_SPEED;
    if (f > 1.0) f = 1.0;
    view_start_squash(v, nx, ny, strength * SQUASH_MAX_AMOUNT * sqrt(f));
}

static void server_consume_impacts(FwmServer *server) {
    double shake = server->config.effects.camera_shake;
    double squash = server->config.effects.squash;
    if (shake <= 0.0 && squash <= 0.0) { server->physics.impact_count = 0; return; }

    int visible_d = (server->camera_x + server->screen_width / 2) / server->screen_width;
    double want = 0.0;
    for (int i = 0; i < server->physics.impact_count; i++) {
        const PhysicsImpact *im = &server->physics.impacts[i];
        /* A contact point sits ON the surface it hit, so a wall impact lands
         * just OUTSIDE the play area: the right wall's inner face is at
         * 10*screen_width, which divides to desktop 10 — a desktop that does
         * not exist — and every hit against it was silently dropped. (The left
         * wall only escaped because C truncates -2/1920 toward zero.) Clamp
         * into the real range instead of trusting the division. */
        int impact_d = (int)(im->x / server->screen_width);
        if (impact_d < 0) impact_d = 0;
        if (impact_d > 9) impact_d = 9;
        if (impact_d != visible_d) continue;

        /* The normal points from A to B, so it faces the contact for A and
         * away from it for B — flip it for B. */
        server_squash_from_impact(server, im->id_a,  im->nx,  im->ny, im->speed);
        server_squash_from_impact(server, im->id_b, -im->nx, -im->ny, im->speed);

        double f = im->speed / SHAKE_FULL_SPEED;
        if (f > 1.0) f = 1.0;
        /* Squared so gentle bumps stay subtle and only real slams shake hard. */
        double mag = shake * SHAKE_MAX_PX * f * f;
        if (mag > want) want = mag;
    }
    if (shake <= 0.0) want = 0.0;
    /* Take the strongest impact of the frame rather than summing: three windows
     * landing together should not triple the shake. */
    if (want > server->shake_mag) {
        server->shake_mag = want;
        server->shake_t = 0.0;
    }
}

/* Advanced at FRAME time, like the other purely visual ramps (see
 * server_animate) — on the physics timer it would beat against vsync. */
void server_shake_tick(FwmServer *server, double dt) {
    if (server->shake_mag <= 0.01) {
        if (server->shake_mag != 0.0) {
            server->shake_mag = 0.0;
            if (server->layer_windows)
                wlr_scene_node_set_position(&server->layer_windows->node, 0, 0);
            if (server->layer_background)
                wlr_scene_node_set_position(&server->layer_background->node, 0, 0);
        }
        return;
    }
    server->shake_t += dt;
    server->shake_mag *= exp(-SHAKE_DECAY * dt);

    /* Two different frequencies, or the offset would travel a straight
     * diagonal instead of reading as a shake. */
    int ox = (int)lround(server->shake_mag * sin(server->shake_t * 38.0));
    int oy = (int)lround(server->shake_mag * sin(server->shake_t * 47.0 + 1.3));

    /* Only the world shakes. The tray and panels stay put: UI jittering under
     * the cursor reads as a glitch, not as impact. */
    if (server->layer_windows)
        wlr_scene_node_set_position(&server->layer_windows->node, ox, oy);
    if (server->layer_background)
        wlr_scene_node_set_position(&server->layer_background->node, ox, oy);
}

/* How long the compositor may sit without driving a frame itself. Not a
 * refresh rate: client redraws and scene node moves damage the scene and
 * wlr_scene schedules a frame off that damage, so this is only a heartbeat. */
#define TICK_IDLE_MS 200

/* Is anything actually moving? Everything listed here is driven by our timers
 * rather than by client damage, so the frame loop must keep running for it.
 * Err on the side of "yes": a false busy costs some idle wakeups, a false idle
 * freezes an animation halfway. */
static int server_is_busy(FwmServer *server) {
    if (server->interactive.action != FWM_ACTION_NONE) return 1;
    if (server->camera_x != server->target_camera_x || server->cam_anim) return 1;
    if (server->shake_mag > 0.0) return 1;
    if (server->wallpaper_prev) return 1;              /* wallpaper cross-fade */
    if (!wl_list_empty(&server->ghosts)) return 1;     /* close animations */
    if (launcher_is_open(server->launcher)) return 1;  /* spring tiles */
    if (cairo_overlay_animating()) return 1;

    for (int i = 0; i < server->physics.body_count; i++) {
        const PhysicsBody *b = &server->physics.bodies[i];
        if (b->active && b->flying) return 1;
    }
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->open_anim || v->tile_anim || v->squash_buf) return 1;
    }
    return 0;
}

/* Hand focus to something sensible without waiting for the pointer to move.
 * Prefers whatever sits under the cursor, which is what focus-follows-pointer
 * would have chosen anyway; otherwise takes a window on `desktop` so arriving
 * somewhere never leaves the keyboard pointing at nothing.
 *
 * `skip` is the view being unmapped: it is still in the list and may still own
 * scene nodes when this runs, so it must be excluded explicitly. */
void server_refocus(FwmServer *server, int desktop, struct FwmView *skip) {
    struct wlr_surface *surface = NULL;
    double sx, sy;
    FwmView *under = view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (under && under != skip) {
        server_focus_view(server, under);
        return;
    }

    /* Nothing under the pointer: take the most recently mapped window on the
     * desktop. Views are inserted at the head, so the first match is the
     * newest — closest to what the user last worked with. */
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v == skip) continue;
        PhysicsBody *b = physics_find_body(&server->physics, v->id);
        /* No body means a hidden tab-stack member: not a focus candidate. */
        if (!b || b->desktop_id != desktop) continue;
        server_focus_view(server, v);
        return;
    }

    /* Genuinely empty desktop: drop the keyboard rather than leave it pointed
     * at a window the user can no longer see. */
    server->focused_view = NULL;
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

void server_schedule_frames(FwmServer *server) {
    struct FwmOutput *output;
    wl_list_for_each(output, &server->outputs, link) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

static int physics_tick_cb(void *data) {
    FwmServer *server = data;
    
    // Tick intervals
    double dt = 1.0 / PHYSICS_TICK_RATE;
    
    uint32_t drag_win = (server->interactive.action == FWM_ACTION_MOVE && server->interactive.collision_disabled) ? server->interactive.view->id : 0;
    uint32_t dragged_win = (server->interactive.action == FWM_ACTION_MOVE) ? server->interactive.view->id : 0;
    // Freeze the window being resized into a static anchor (skip_b): it must not
    // sink under gravity, get shoved by neighbors, or jitter while its collision
    // box is rebuilt every motion event — the mouse owns it entirely.
    uint32_t resize_win = (server->interactive.action == FWM_ACTION_RESIZE && server->interactive.view) ? server->interactive.view->id : 0;

    if (resize_win) {
        PhysicsBody *rb = physics_find_body(&server->physics, resize_win);
        if (rb) { rb->flying = 0; rb->vx = 0; rb->vy = 0; }
    }
    
    if (server->interactive.action == FWM_ACTION_MOVE && server->interactive.view) {
        physics_set_velocity(&server->physics, server->interactive.view->id, server->interactive.vx, server->interactive.vy);
    }
    
    // Desktop-switch camera slide: fixed-duration ease-in-out instead of the
    // old exponential chase (fast jump + 1px/tick crawl tail). If the target
    // changes mid-flight, restart from the current position so it stays smooth.
    if (server->camera_x != server->target_camera_x || server->cam_anim) {
        // X11 clients place popups from their last-configured root coords;
        // tell them where they are once the camera comes to rest.
        int cam_settled = 0;

        if (server->cam_free) {
            // Continuous pan under a held bind: framerate-independent
            // exponential chase, same form as the tile glide. Unlike the slide
            // below it has no notion of "start over", so a target that moves
            // every 40ms costs nothing — the camera tracks it immediately and
            // coasts the last few px once the key is released.
            server->cam_anim = 0;
            int gap = server->target_camera_x - server->camera_x;
            if (gap != 0) {
                double speed = server->config.camera.free_speed;
                double k = speed > 0.0 ? 1.0 - exp(-speed * dt) : 1.0;
                int step = (int)lround(gap * k);
                if (step == 0) step = gap > 0 ? 1 : -1; // never stall sub-pixel
                server->camera_x += step;
                // Snap the last pixel: edge auto-scroll only ever fires while
                // camera_x == target_camera_x exactly.
                if (abs(server->target_camera_x - server->camera_x) <= 1) {
                    server->camera_x = server->target_camera_x;
                }
                cam_settled = server->camera_x == server->target_camera_x;
            }
        } else {
            if (!server->cam_anim || server->cam_anim_to != server->target_camera_x) {
                server->cam_anim = 1;
                server->cam_anim_from = server->camera_x;
                server->cam_anim_to = server->target_camera_x;
                server->cam_anim_t = 0.0;
            }
            double cam_ms = server->config.camera.anim_ms;
            server->cam_anim_t += cam_ms > 0.0 ? dt * 1000.0 / cam_ms : 1.0;
            double t = server->cam_anim_t;
            if (t >= 1.0) {
                server->camera_x = server->cam_anim_to;
                server->cam_anim = 0;
                cam_settled = 1;
            } else {
                // Cubic ease-in-out.
                double e = t < 0.5 ? 4.0 * t * t * t
                                   : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
                server->camera_x = server->cam_anim_from
                    + (int)lround((server->cam_anim_to - server->cam_anim_from) * e);
            }
        }

        if (cam_settled) {
            FwmView *xv;
            wl_list_for_each(xv, &server->views, link) view_sync_position(xv);

            /* Arriving on a desktop should hand the keyboard to something
             * there. Otherwise focus stays on the window you left behind and
             * typing goes to a desktop you can no longer see. Covers every way
             * of getting here — the view: binds, the tray, edge auto-scroll. */
            int arrived = server->camera_x / server->screen_width;
            if (arrived != server->focus_desktop) {
                server->focus_desktop = arrived;
                server_refocus(server, arrived, NULL);
                ipc_emit_desktop(server->ipc, arrived);
            }
        }
        
        // Sync non-dragged windows relative to camera
        FwmView *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->id != dragged_win && view->scene_tree) {
                PhysicsBody *body = physics_find_body(&server->physics, view->id);
                if (body) {
                    wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(body->x - server->camera_x), (int)lround(body->y));
                }
            }
        }
    }
    
    // Tile-glide animations: ease windows toward their tile slots (Hyprland-
    // style) instead of teleporting. Exponential approach is frame-rate
    // independent; the physics bridge sees these as external writes and keeps
    // the Box2D body glued to the glide.
    {
        double k = 1.0 - exp(-server->config.tiling.anim_speed * dt);
        FwmView *av;
        wl_list_for_each(av, &server->views, link) {
            if (!av->tile_anim) continue;
            PhysicsBody *pb = physics_find_body(&server->physics, av->id);
            if (!pb) { av->tile_anim = 0; continue; }
            double dx = av->tile_tx - pb->x;
            double dy = av->tile_ty - pb->y;
            if (fabs(dx) < 1.0 && fabs(dy) < 1.0) {
                pb->x = av->tile_tx;
                pb->y = av->tile_ty;
                av->tile_anim = 0;
            } else {
                pb->x += dx * k;
                pb->y += dy * k;
            }
            pb->vx = 0; pb->vy = 0; pb->flying = 0;
            av->x = pb->x;
            av->y = pb->y;
        }
    }

    // Tab bars follow their window's width (resize/tiling glides).
    group_tick(server);

    // Launcher: tile physics + overlay redraw while open.
    launcher_tick(server->launcher, dt);

    // Physics step
    physics_step(&server->physics, server->screen_width, server->screen_height,
                 drag_win, resize_win, dragged_win, dt);

    /* Impacts are only valid until the next step, so drain them here. */
    server_consume_impacts(server);

    // Synchronize scene tree nodes to physics coordinates
    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->scene_tree) {
            PhysicsBody *body = physics_find_body(&server->physics, view->id);
            if (body && !body->pinned && view->id != dragged_win) {
                view->x = body->x;
                view->y = body->y;
                wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(body->x - server->camera_x), (int)lround(body->y));
            }
        }
    }

    // Hide the tray while a real-fullscreen window occupies the active desktop
    // (overlays outrank windows in the scene, so the surface can't cover it).
    // Fake fullscreen keeps the tray — that's its point. Checking every tick
    // also covers desktop switches and the fullscreen window closing.
    if (server->tray_buffer) {
        int active_d = (server->camera_x + server->screen_width / 2) / server->screen_width;
        bool hide = false;
        FwmView *fsv;
        wl_list_for_each(fsv, &server->views, link) {
            if (!fsv->fs_real) continue;
            PhysicsBody *fb = physics_find_body(&server->physics, fsv->id);
            if (fb && fb->fullscreen && fb->desktop_id == active_d) {
                hide = true;
                break;
            }
        }
        wlr_scene_node_set_enabled(&server->tray_buffer->node, !hide);
    }

    // Redraw tray if data changed
    server_request_tray_redraw(server);

    idle_inhibit_refresh(server);

    // Parallax: shift each wallpaper layer by a fraction of the camera offset.
    wallpaper_update(server->wallpaper, server->camera_x);

    /* While anything is actually moving we drive the frame loop ourselves at
     * the full tick rate. Once everything settles we drop to a slow heartbeat
     * instead of spinning at 60Hz forever: client redraws and scene node moves
     * damage the scene, and wlr_scene schedules a frame off that damage on its
     * own, so nothing depends on us for those.
     *
     * The heartbeat is not just for the tray clock. It is the safety net that
     * the old unconditional schedule used to provide: should any path ever
     * change what is on screen without damaging the scene, the screen is stale
     * for at most one beat instead of forever. */
    int busy = server_is_busy(server);
    server_schedule_frames(server);

    /* physics_step always advances a FIXED 1/60 regardless of when we are
     * called, so slowing the timer cannot hand Box2D an enormous dt — idle
     * simply means nothing is moving for it to integrate. */
    server->tick_idle = !busy;
    wl_event_source_timer_update(server->physics_timer,
                                 busy ? (int)(1000.0 / PHYSICS_TICK_RATE) : TICK_IDLE_MS);
    return 0;
}


FwmView *server_find_view(FwmServer *server, uint32_t id) {
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->id == id) return v;
    }
    return NULL;
}


bool server_init(FwmServer *server) {
    memset(server, 0, sizeof(*server));
    server->wl_display = wl_display_create();
    if (!server->wl_display) {
        wlr_log(WLR_ERROR, "failed to create display");
        return false;
    }
    
    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);

    /* MUST happen before the backend/renderer, and thus before the GPU driver
     * spawns its worker threads. wl_event_loop_add_signal blocks the signal
     * with sigprocmask, and a thread inherits the mask AS IT IS AT CREATION —
     * threads that already exist keep accepting the signal. The kernel hands a
     * process-directed signal to any thread that does not block it, so with
     * this registered later the driver's threads took SIGTERM and the default
     * disposition killed us (exit 143) with server_destroy never running.
     * Verified by reading SigBlk from each thread's status under
     * /proc/<pid>/task: it was 0 on those threads. */
    wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, server);
    wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, server);

    server->wlr_backend = wlr_backend_autocreate(event_loop, &server->session);
    if (!server->wlr_backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return false;
    }
    
    server->wlr_renderer = wlr_renderer_autocreate(server->wlr_backend);
    if (!server->wlr_renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return false;
    }
    wlr_renderer_init_wl_display(server->wlr_renderer, server->wl_display);
    
    server->wlr_allocator = wlr_allocator_autocreate(server->wlr_backend, server->wlr_renderer);
    if (!server->wlr_allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return false;
    }

    server->compositor = wlr_compositor_create(server->wl_display, 5, server->wlr_renderer);
    wlr_subcompositor_create(server->wl_display);
    wlr_data_device_manager_create(server->wl_display);
    // Screen capture protocol: lets wf-recorder record and grim screenshot.
    wlr_screencopy_manager_v1_create(server->wl_display);
    // Standard client protocols, all self-contained in wlroots:
    // primary selection = middle-click paste; viewporter + fractional-scale +
    // single-pixel-buffer are expected by games/video players/toolkits;
    // presentation-time gives clients accurate frame timing (mpv, games).
    wlr_primary_selection_v1_device_manager_create(server->wl_display);
    wlr_viewporter_create(server->wl_display);
    wlr_fractional_scale_manager_v1_create(server->wl_display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server->wl_display);
    wlr_presentation_create(server->wl_display, server->wlr_backend, 2);

    server->output_layout = wlr_output_layout_create(server->wl_display);
    // xdg-output: exposes output geometry to clients (grim/wf-recorder need it).
    wlr_xdg_output_manager_v1_create(server->wl_display, server->output_layout);

    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

    // Scene layers, created bottom-to-top: parallax wallpaper sits below the
    // windows, and the tray/hints/welcome overlays sit above them so a raised
    // window can never cover them.
    server->layer_background = wlr_scene_tree_create(&server->scene->tree);
    server->layer_windows = wlr_scene_tree_create(&server->scene->tree);
    server->layer_overlay = wlr_scene_tree_create(&server->scene->tree);

    // Layer-shell trees are woven between ours. Creation order alone cannot
    // express this (new trees always land on top), so place them explicitly:
    // wallpaper < ls_background < ls_bottom < windows < ls_top < our overlays
    // < ls_overlay. A layer-shell overlay therefore outranks even the tray,
    // which is what clients like a screen locker or a menu expect.
    server->ls_background = wlr_scene_tree_create(&server->scene->tree);
    server->ls_bottom = wlr_scene_tree_create(&server->scene->tree);
    server->ls_top = wlr_scene_tree_create(&server->scene->tree);
    server->ls_overlay = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_above(&server->ls_background->node, &server->layer_background->node);
    wlr_scene_node_place_above(&server->ls_bottom->node, &server->ls_background->node);
    wlr_scene_node_place_above(&server->layer_windows->node, &server->ls_bottom->node);
    wlr_scene_node_place_above(&server->ls_top->node, &server->layer_windows->node);
    wlr_scene_node_place_above(&server->layer_overlay->node, &server->ls_top->node);
    wlr_scene_node_place_above(&server->ls_overlay->node, &server->layer_overlay->node);
    /* The lock screen outranks everything, including an external bar's overlay
     * layer — nothing may be drawn over a locked session. Disabled until a
     * lock actually engages. */
    server->layer_lock = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_place_above(&server->layer_lock->node, &server->ls_overlay->node);
    wlr_scene_node_set_enabled(&server->layer_lock->node, false);

    server->launcher = launcher_create(server);

    wl_list_init(&server->views);
    wl_list_init(&server->groups);
    wl_list_init(&server->ghosts);
    wl_list_init(&server->outputs);
    wl_list_init(&server->keyboards);
    
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3); // xdg-shell v3/v6 depending on wlroots version (v3 is standard in 0.17+)
    layer_shell_init(server);
    lock_init(server);
    foreign_init(server);

    server->cursor = wlr_cursor_create();
    server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    // Load the theme and show a default cursor image immediately. Without this
    // the pointer has no image until a client sets one, so it looks "gone" for
    // the first few seconds after startup (and over the empty background).
    wlr_xcursor_manager_load(server->cursor_mgr, 1);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    server->seat = wlr_seat_create(server->wl_display, "seat0");

    // Xwayland (lazy: the X server starts on the first X11 client). Managed
    // windows become FwmViews, override-redirect ones bare scene surfaces.
    server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, true);
    if (server->xwayland) {
        setenv("DISPLAY", server->xwayland->display_name, true);
        wlr_log(WLR_INFO, "Xwayland on DISPLAY=%s", server->xwayland->display_name);
    } else {
        wlr_log(WLR_ERROR, "Failed to start Xwayland; X11 apps won't work");
    }

    /* No listeners needed: the notifier is driven by server_notify_activity
     * from the input paths, and the inhibit manager keeps its own list, which
     * idle_inhibit_refresh polls each tick. */
    server->relative_pointer = wlr_relative_pointer_manager_v1_create(server->wl_display);
    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->wl_display);

    server->output_power = wlr_output_power_manager_v1_create(server->wl_display);
    server->gamma_control = wlr_gamma_control_manager_v1_create(server->wl_display);
    if (server->gamma_control) {
        /* The scene applies client ramps itself, including re-applying them
         * when an output comes back — far less to get wrong than committing
         * them by hand. We only watch the event to stand our own night light
         * down when a client takes the ramp over. */
        wlr_scene_set_gamma_control_manager_v1(server->scene, server->gamma_control);
    }
    server->cursor_shape = wlr_cursor_shape_manager_v1_create(server->wl_display, 1);

    server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
    server->idle_inhibit  = wlr_idle_inhibit_v1_create(server->wl_display);
    server->idle_inhibited = 0;

    server->xdg_activation = wlr_xdg_activation_v1_create(server->wl_display);

    /* Each module wires its own listeners. Nothing can fire while init runs —
     * the display socket is created below and the backend does not start until
     * server_run() — so doing this in one place, after every object exists, is
     * equivalent to interleaving it with construction, and it lets the handlers
     * stay private to the file that implements them. */
    server_shell_register(server);
    server_pointer_register(server);
    server_output_register(server);
    server_input_register(server);
    
    // Load config
    char path[512];
    server_config_path(path, sizeof(path));
    config_load(&server->config, path);
    server_state_apply_wallpaper(server);
    // Palette for every overlay and window border; may sample the wallpaper.
    theme_build(&server->config);

    // Init physics
    physics_init(&server->physics);
    server->physics.friction = server->config.physics.friction;
    server->physics.mass_density = server->config.physics.mass_density;
    server->physics.throw_speed_multiplier = server->config.physics.throw_speed_multiplier;
    server->physics.max_throw_speed = server->config.physics.max_throw_speed;
    server->physics.stop_speed_threshold = server->config.physics.stop_speed_threshold;
    server->physics.restitution = server->config.physics.restitution;
    server->physics.gravity = server->config.physics.gravity;
    
    server->camera_x = 0;
    server->target_camera_x = 0;
    
    // Setup physics timer callback (60Hz)
    server->physics_timer = wl_event_loop_add_timer(event_loop, physics_tick_cb, server);
    wl_event_source_timer_update(server->physics_timer, (int)(1000.0 / PHYSICS_TICK_RATE));

    // Held-key auto-repeat timer for repeatable binds (armed on demand).
    server->repeat_action = NULL;
    server->repeat_keycode = 0;

    // Must be created after the backend (which may itself connect to an
    // upstream WAYLAND_DISPLAY when nested) so we don't clobber that env var
    // with our own socket before the backend has a chance to read it.
    const char *socket = wl_display_add_socket_auto(server->wl_display);
    if (!socket) {
        wlr_log(WLR_ERROR, "failed to create socket");
        return false;
    }
    wlr_log(WLR_INFO, "Wayland socket: %s", socket);
    setenv("WAYLAND_DISPLAY", socket, 1);

    /* Control socket. Named after the Wayland display so several fwm
     * instances (a nested dev run inside a real session) never collide.
     * Failure is non-fatal: fwm works without it, just not scriptably. */
    server->ipc = ipc_create(server, socket);

    server->running = 1;
    return true;
}

void server_run(FwmServer *server) {
    if (!wlr_backend_start(server->wlr_backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return;
    }

    /* After the backend is up (so WAYLAND_DISPLAY is exported and the socket
     * accepts connections) but before the event loop blocks: the relaunched
     * clients connect into the loop we are about to enter. */
    session_restore(server);

    wl_display_run(server->wl_display);
}

/* Detach a listener that may never have been attached: server_init memsets the
 * whole struct, so an unused one still has a zeroed link and wl_list_remove
 * would walk a NULL pointer. */
static void server_remove_listener(struct wl_listener *l) {
    if (l->link.prev) wl_list_remove(&l->link);
}

void server_destroy(FwmServer *server) {
    /* Before anything else: stop accepting commands that would touch state we
     * are about to free, and take the socket file with us. */
    ipc_destroy(server->ipc);
    server->ipc = NULL;

    /* Order matters: the policy lives in the config, which is freed below. */
    session_clear_on_clean_exit(server);
    session_finish(server);
    config_free(&server->config);
    
    /* Clean overlays. Clearing each pointer is not tidiness: teardown below
     * takes the clients down, and an unmapping view still calls
     * server_request_tray_redraw(). That guards on a NULL tray_buffer — but a
     * freed pointer is not NULL, so it sailed through the guard and read the
     * released scene buffer. Same reasoning as the IPC handle above. */
    if (server->tray_buffer) cairo_overlay_destroy(server->tray_buffer);
    server->tray_buffer = NULL;
    if (server->hints_buffer) cairo_overlay_destroy(server->hints_buffer);
    server->hints_buffer = NULL;
    if (server->welcome_buffer) cairo_overlay_destroy(server->welcome_buffer);
    server->welcome_buffer = NULL;
    if (server->errors_buffer) cairo_overlay_destroy(server->errors_buffer);
    server->errors_buffer = NULL;
    if (server->wallpaper_prev) wallpaper_destroy(server->wallpaper_prev);
    server->wallpaper_prev = NULL;
    if (server->wallpaper) wallpaper_destroy(server->wallpaper);
    server->wallpaper = NULL;
    launcher_destroy(server->launcher);
    server->launcher = NULL;
    
    if (server->physics_timer) {
        wl_event_source_remove(server->physics_timer);
    }
    if (server->key_repeat_timer) {
        wl_event_source_remove(server->key_repeat_timer);
    }
    physics_destroy(&server->physics);

    FwmGhost *g, *g_tmp;
    wl_list_for_each_safe(g, g_tmp, &server->ghosts, link) {
        wlr_scene_node_destroy(&g->scene_buffer->node);
        wlr_buffer_unlock(g->buffer);
        wl_list_remove(&g->link);
        free(g);
    }

    // Xwayland must go down before the clients/display it hangs off.
    if (server->xwayland) {
        wl_list_remove(&server->xwl_ready.link);
        wl_list_remove(&server->xwl_new_surface.link);
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
    }

    /* Every wlroots global we listen to asserts on destroy that its signals
     * have no listeners left (xdg_shell was the one that caught us), so all of
     * ours have to come off before wl_display_destroy runs them down.
     *
     * None of this ever ran before: without a signal handler the process was
     * killed outright and server_destroy was dead code on the normal exit
     * path. server_init memsets the struct, so a listener that was never added
     * still has a NULL link and is skipped. */
    server_remove_listener(&server->new_xdg_toplevel);
    server_remove_listener(&server->new_xdg_popup);
    server_remove_listener(&server->new_toplevel_decoration);
    server_remove_listener(&server->new_input);
    server_remove_listener(&server->new_output);
    server_remove_listener(&server->cursor_motion);
    server_remove_listener(&server->cursor_motion_absolute);
    server_remove_listener(&server->cursor_button);
    server_remove_listener(&server->cursor_axis);
    server_remove_listener(&server->cursor_frame);
    server_remove_listener(&server->request_cursor);
    server_remove_listener(&server->seat_request_set_selection);
    server_remove_listener(&server->seat_request_set_primary_selection);
    server_remove_listener(&server->seat_request_start_drag);
    server_remove_listener(&server->seat_start_drag);
    server_remove_listener(&server->new_pointer_constraint);
    server_remove_listener(&server->constraint_destroy);
    server_remove_listener(&server->output_power_set_mode);
    server_remove_listener(&server->cursor_shape_request);
    server_remove_listener(&server->xdg_activation_request_activate);
    /* Owned by other modules but attached to globals just the same. */
    server_remove_listener(&server->new_layer_surface);
    server_remove_listener(&server->new_lock);

    wl_display_destroy_clients(server->wl_display);
    wl_display_destroy(server->wl_display);
}

void server_focus_view(FwmServer *server, struct FwmView *view) {
    if (server->focused_view == view) return;
    
    struct FwmView *prev_focus = server->focused_view;
    server->focused_view = view;
    
    if (view) {
        if (view->scene_tree) wlr_scene_node_raise_to_top(&view->scene_tree->node);
        
        struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
        struct wlr_surface *surface = view_surface(view);
        if (kbd && surface) {
            wlr_seat_keyboard_notify_enter(server->seat, surface,
                kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
        }
        view_set_activated(view, true);
        foreign_view_set_activated(view, true);
        
        PhysicsBody *pb = physics_find_body(&server->physics, view->id);
        if (pb) {
            pb->corner_mode = CORNER_ROUND;
        }
        view_set_border_color(view, theme_get()->border_active);
    } else {
        wlr_seat_keyboard_clear_focus(server->seat);
    }
    ipc_emit_window(server->ipc, FWM_EV_WINDOW_FOCUS, view);

    if (prev_focus) {
        view_set_activated(prev_focus, false);
        foreign_view_set_activated(prev_focus, false);
        PhysicsBody *pb = physics_find_body(&server->physics, prev_focus->id);
        if (pb) {
            int d = pb->desktop_id;
            pb->corner_mode = (server->desktop_mode[d] == DESKTOP_MODE_PHYSICS) ? CORNER_CHAMFER : CORNER_SHARP;
        }
        view_set_border_color(prev_focus, theme_get()->border_inactive);
    }
    
    server_request_tray_redraw(server);
}

/* The area tiles live in: the space layer-shell clients left us, minus our own
 * tray and the outer gap. Shared by the layout and the alignment pass so the
 * two cannot drift apart. */
static void tile_area(FwmServer *server, int desktop, int *x, int *y, int *w, int *h) {
    int gout = server->config.tiling.gaps_out;
    struct wlr_box work = server->usable_area;
    if (work.width <= 0 || work.height <= 0) {
        work = (struct wlr_box){ 0, 0, server->screen_width, server->screen_height };
    }
    if (work.y < TRAY_BOTTOM) {
        work.height -= TRAY_BOTTOM - work.y;
        work.y = TRAY_BOTTOM;
    }
    *x = desktop * server->screen_width + work.x + gout;
    *y = work.y + gout;
    *w = work.width  - gout * 2;
    *h = work.height - gout * 2;
}

/* Move one tile to where the alignment pass decided it goes. */
static void tile_move_to(FwmServer *server, FwmView *view, PhysicsBody *pb, int nx, int ny) {
    int animate = server->config.tiling.anim_speed > 0.0 &&
                  server->interactive.action != FWM_ACTION_BSP_RESIZE;
    if (!view) { pb->x = nx; pb->y = ny; return; }

    if (animate) {
        view->tile_anim = 1;
        view->tile_tx = nx;
        view->tile_ty = ny;
    } else {
        view->tile_anim = 0;
        pb->x = nx;
        pb->y = ny;
        view->x = pb->x;
        view->y = pb->y;
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node,
                                        (int)lround(view->x - server->camera_x),
                                        (int)lround(view->y));
        }
    }
}

/* Re-run tile positioning against the sizes clients actually committed.
 *
 * A tile is asked to be its slot's size, but a client may commit something
 * smaller and terminals routinely do — they round to whole character cells.
 * Anchored at the slot's top-left, that leftover ends up BETWEEN windows, so a
 * gap meant to be gaps_in reads as gaps_in plus a stray 6-16px. It is why the
 * layout looked right with two windows and wrong from three: two side-by-side
 * tiles are both full height, so there is no interior gap to get wrong yet.
 *
 * bsp_place_actual() does the arithmetic; this feeds it the committed sizes
 * and moves the windows to what it decided. No size is touched, so this cannot
 * provoke the commits that would call it again.
 */
void server_align_tiles(FwmServer *server, int desktop) {
    if (desktop < 0 || desktop >= 10) return;
    BspNode *root = server->bsp_roots[desktop];
    if (!root) return;

    BspNode *leaves[MAX_WINDOWS];
    int count = 0;
    bsp_collect_leaves(root, leaves, &count, MAX_WINDOWS);
    if (count == 0) return;

    BspActual actual[MAX_WINDOWS];
    int bar[MAX_WINDOWS];
    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        FwmView *view = server_find_view(server, n->id);
        int w = n->aw, h = n->ah;
        if (view) view_committed_size(view, &w, &h);
        /* A tab-stack's bar sits above the client but inside the slot, so it
         * counts toward the space this leaf occupies. */
        bar[i] = (view && view->group && n->ah > GROUP_TAB_H * 2) ? GROUP_TAB_H : 0;
        actual[i] = (BspActual){ .id = n->id, .w = w, .h = h + bar[i] };
    }

    int x, y, w, h;
    tile_area(server, desktop, &x, &y, &w, &h);
    bsp_place_actual(root, x, y, w, h, server->config.tiling.gaps_in, actual, count);

    for (int i = 0; i < count; i++) {
        BspNode *n = leaves[i];
        PhysicsBody *pb = physics_find_body(&server->physics, n->id);
        if (!pb) continue;
        pb->tiled = 1;
        pb->vx = 0;
        pb->vy = 0;
        pb->flying = 0;

        FwmView *view = server_find_view(server, n->id);
        pb->width  = n->aw;
        pb->height = n->ah - bar[i];
        if (view) {
            view->width  = pb->width;
            view->height = pb->height;
            view_set_size(view, view->width, view->height);
        }
        tile_move_to(server, view, pb, n->ax, n->ay + bar[i]);
    }
}

void server_apply_tiling(FwmServer *server, int desktop) {
    int x, y, usable_w, usable_h;
    tile_area(server, desktop, &x, &y, &usable_w, &usable_h);

    /* The slot grid. Sizes and positions no longer come from it —
     * server_align_tiles() derives those from what clients committed — but
     * bsp_find_border() hit-tests dragging against x/y/w/h, and the ratio each
     * split is recalculated from lives here. */
    bsp_recalc(server->bsp_roots[desktop], x, y, usable_w, usable_h,
               server->config.tiling.gaps_in);

    server_align_tiles(server, desktop);
}
void server_start_interactive_move(FwmServer *server, struct FwmView *view, uint32_t serial) {
    (void)serial;
    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    if (!pb || pb->pinned) return;
    
    int tiling = (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING);
    if (tiling) {
        // Swap drag in tiling mode requires shift key, handled in handle_cursor_button
        return;
    }
    
    server->interactive.action = FWM_ACTION_MOVE;
    server->interactive.view = view;
    server->interactive.start_x = server->cursor->x;
    server->interactive.start_y = server->cursor->y;
    server->interactive.view_start_x = view->x - server->camera_x;
    server->interactive.view_start_y = view->y;
    server->interactive.view_start_width = view->width;
    server->interactive.view_start_height = view->height;
    server->interactive.last_x = server->cursor->x;
    server->interactive.last_y = server->cursor->y;
    clock_gettime(CLOCK_MONOTONIC, &server->interactive.last_time);
    server->interactive.vx = 0;
    server->interactive.vy = 0;
    server->interactive.hist_count = 0;
    server->interactive.collision_disabled = 0;
    
    physics_stop_body(&server->physics, view->id);
}

void server_start_interactive_resize(FwmServer *server, struct FwmView *view, uint32_t edges, uint32_t serial) {
    (void)serial;
    (void)edges;
    PhysicsBody *pb = physics_find_body(&server->physics, view->id);
    if (!pb || pb->pinned) return;
    if (server->desktop_mode[pb->desktop_id] == DESKTOP_MODE_TILING) return;
    
    server->interactive.action = FWM_ACTION_RESIZE;
    server->interactive.view = view;
    server->interactive.start_x = server->cursor->x;
    server->interactive.start_y = server->cursor->y;
    server->interactive.view_start_x = view->x - server->camera_x;
    server->interactive.view_start_y = view->y;
    server->interactive.view_start_width = view->width;
    server->interactive.view_start_height = view->height;
    
    physics_stop_body(&server->physics, view->id);
}

void server_set_fullscreen(FwmServer *server, struct FwmView *view, bool fullscreen, bool real) {
    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    if (!b) return;
    
    int d = b->desktop_id;
    if (fullscreen) {
        if (!b->fullscreen) {
            b->sav_x = b->x; b->sav_y = b->y;
            b->sav_w = b->width; b->sav_h = b->height;
        }
        b->fullscreen = 1;
        b->flying = 0; b->vx = 0; b->vy = 0;
        
        // Real fullscreen covers the whole output; fake fullscreen fills the
        // work area below the status bar so the tray stays visible.
        /* Fake fullscreen fills the work area: below our tray and clear of
         * any layer-shell bar that reserved space. */
        struct wlr_box work = server->usable_area;
        if (work.width <= 0 || work.height <= 0) {
            work = (struct wlr_box){ 0, 0, server->screen_width, server->screen_height };
        }
        int top = real ? 0 : (work.y > TRAY_BOTTOM + 12 ? work.y : TRAY_BOTTOM + 12);
        view->x = d * server->screen_width + (real ? 0 : work.x);
        view->y = top;
        view->width = real ? server->screen_width : work.width;
        view->height = server->screen_height - top;
        
        // Keep the physics body in sync with the fullscreen geometry, otherwise
        // physics_tick_cb re-syncs the scene node back to the body's stale
        // position every tick and the window snaps out of fullscreen.
        b->x = view->x; b->y = view->y;
        b->width = view->width; b->height = view->height;
        
        view_set_size(view, view->width, view->height);
        view_set_fullscreen_hint(view, real);
        
        if (view->scene_tree) {
            wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(view->x - server->camera_x), (int)lround(view->y));
            wlr_scene_node_raise_to_top(&view->scene_tree->node);
        }
        view_set_border_enabled(view, 0); // borderless fullscreen
        view->fs_real = real ? 1 : 0;
    } else {
        if (b->fullscreen) {
            b->fullscreen = 0;
            b->x = b->sav_x; b->y = b->sav_y;
            b->width = b->sav_w; b->height = b->sav_h;
            
            view->x = b->x; view->y = b->y;
            view->width = b->width; view->height = b->height;
            view_set_size(view, view->width, view->height);
            view_set_fullscreen_hint(view, false);
            
            if (view->scene_tree) {
                wlr_scene_node_set_position(&view->scene_tree->node, (int)lround(view->x - server->camera_x), (int)lround(view->y));
            }
            view_set_border_enabled(view, 1);
            view->fs_real = 0;
            if (server->desktop_mode[d] == DESKTOP_MODE_TILING) {
                server_apply_tiling(server, d);
            }
        }
    }
    
    server_request_tray_redraw(server);
}

void server_request_tray_redraw(FwmServer *server) {
    if (!server->tray_buffer) return;
    
    TrayData data = {0};
    for (int i = 0; i < 10; i++) {
        data.desktop_window_counts[i] = 0;
    }
    
    // Count windows per desktop
    for (int i = 0; i < server->physics.body_count; i++) {
        PhysicsBody *body = &server->physics.bodies[i];
        if (body->active) {
            int d = (int)((body->x + body->width / 2.0) / server->screen_width);
            if (d >= 0 && d < 10) {
                data.desktop_window_counts[d]++;
            }
        }
    }
    
    // Active keyboard layout tag, shown only when several are configured.
    // Prefer the short name from [input] kbd_layout ("us,ru" -> "US"/"RU");
    // fall back to the xkb layout name's first letters.
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (kbd && kbd->keymap && xkb_keymap_num_layouts(kbd->keymap) > 1) {
        xkb_layout_index_t idx =
            xkb_state_serialize_layout(kbd->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
        const char *src = server->config.input.kbd_layout;
        int cur = 0;
        const char *p = src;
        while (*p && cur < (int)idx) {
            if (*p == ',') cur++;
            p++;
        }
        if (*p && cur == (int)idx) {
            int n = 0;
            while (p[n] && p[n] != ',' && p[n] != '(' && n < 2) n++;
            for (int i = 0; i < n; i++) {
                char c = p[i];
                data.kbd_layout[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }
            data.kbd_layout[n] = '\0';
        } else {
            const char *name = xkb_keymap_layout_get_name(kbd->keymap, idx);
            if (name) snprintf(data.kbd_layout, sizeof(data.kbd_layout), "%.2s", name);
        }
    }

    data.opacity = server->config.decor.tray_opacity;
    data.error_count = server->config.error_total;
    data.error_expanded = server->errors_buffer != NULL;
    data.active_pos = (double)server->camera_x / server->screen_width;
    if (data.active_pos < 0.0) data.active_pos = 0.0;
    if (data.active_pos > 9.0) data.active_pos = 9.0;
    data.active_desktop = (server->camera_x + server->screen_width / 2) / server->screen_width;
    if (data.active_desktop < 0) data.active_desktop = 0;
    if (data.active_desktop >= 10) data.active_desktop = 9;
    
    if (server->focused_view) {
        PhysicsBody *b = physics_find_body(&server->physics, server->focused_view->id);
        if (b) {
            data.win_name = view_title(server->focused_view);
            if (!data.win_name) data.win_name = "Window";
            data.speed = hypot(b->vx, b->vy);
            data.angle = atan2(b->vy, b->vx) * 180.0 / M_PI;
            data.mass = b->mass;
            data.flying = b->flying;
        }
    }
    
    tray_redraw(server->tray_buffer, &data);
}
