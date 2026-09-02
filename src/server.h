/*
 * fwm — a Wayland compositor
 * Copyright (C) 2026 Ilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef FWM_SERVER_H
#define FWM_SERVER_H

#include <time.h>
#include <wlr/util/box.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output.h>
#include <xkbcommon/xkbcommon.h>

#include "physics.h"
#include "bsp.h"
#include "config.h"
#include "battery.h"
#include "star.h"
#include "sun.h"
#include "gestures.h"
/* For TrayStrip: each monitor owns the geometry of the status strip drawn on
 * it, so a click is answered by the strip it landed on. */
#include "ui/tray.h"

#define DESKTOP_MODE_PHYSICS 0
#define DESKTOP_MODE_TILING  1
/* "Normal desktop environment": windows keep the position you drop them at and
 * overlap freely — no gravity, no shoving, no layout. Mechanically it is the
 * per-window pinned + no_collide pair raised to the whole desktop. */
#define DESKTOP_MODE_FLOATING 2

typedef enum {
    FWM_ACTION_NONE,
    FWM_ACTION_MOVE,
    FWM_ACTION_RESIZE,
    FWM_ACTION_SWAP,
    FWM_ACTION_BSP_RESIZE,
    /* Turning a window with the mouse: the cursor's angle around the window's
     * centre is the window's angle, and letting go hands whatever rate the hand
     * was turning at to the simulation. The window does not move while this
     * runs — it is an anchor, like a resize. */
    FWM_ACTION_TWIST
} FwmInteractiveAction;

struct FwmView;
struct Launcher;
struct Radial;
struct FwmShotPicker;
struct FwmServer;

/* One monitor.
 *
 * The world is a strip of FWM_DESKTOPS columns, each the size of the LARGEST
 * monitor. A monitor is a WINDOW ONTO that strip: it shows one column, the one
 * its `desktop` names, and `camera_x` is where its left edge sits in world
 * coordinates. Two monitors are two independent windows onto the same strip —
 * which is what makes them independent screens rather than one wide desktop.
 *
 * A desktop is shown by at most one monitor at a time. Windows on a desktop
 * that no monitor is showing are parked off the layout (see server_place_node);
 * nothing draws them and nothing has to remember they are hidden. */
typedef struct FwmOutput {
    struct wl_list link;
    struct FwmServer *server;
    struct wlr_output *wlr_output;
    struct wlr_box box;      /* position and size in layout coordinates */
    int enabled;             /* 0 while this screen is dark */
    /* Dark because the session went idle, which is NOT the same as `enabled`:
     * everything about the monitor stays exactly as it was and only the light
     * is off. Remembered per screen so waking lights the ones idle put out and
     * leaves alone the ones the lid or `output_off` did. */
    int idle_blanked;
    /* The blanking commit was refused by the driver. Without this the timer
     * asks again on every tick for as long as the session stays idle — a DRM
     * atomic commit per beat, all night, on a screen that will not take it. */
    int idle_blank_failed;
    /* Turned off at RUNTIME (the lid, `output_off`) rather than by the config.
     * A reload re-applies the file to every monitor, and without this it would
     * light the panel inside a closed laptop back up. */
    int forced_off;

    /* Where `fwmctl output position=` put this screen. Remembered because
     * leaving the layout (the lid, `output_off`) forgets everything, and a
     * screen coming back where the layout feels like putting it would undo an
     * arrangement nobody re-typed. A [[output]] x/y still wins over it. */
    int manual_pos;
    int manual_x, manual_y;

    int desktop;             /* which of the ten columns this monitor shows */
    /* The column it showed before that, or -1 until it has moved: what
     * view:back and [camera] back_and_forth return to. Per monitor, because on
     * two screens "where I came from" was never the same place. */
    int prev_desktop;
    int camera_x;            /* world x of this monitor's left edge */
    int target_camera_x;
    /* Desktop-switch slide: timed ease-in-out from cam_anim_from to
     * cam_anim_to; retargets smoothly if target_camera_x changes mid-flight. */
    int cam_anim;
    int cam_anim_from, cam_anim_to;
    double cam_anim_t;
    /* Continuous free pan (a held move_camera: bind). Must NOT use the slide
     * above: that animator restarts its fixed-duration ease every time the
     * target moves, and a held bind moves it every 40ms, so the camera only
     * ever completed the slowest ~1% of an ease-in-out and then caught up in
     * one jump on release. Free pan chases the target exponentially instead. */
    int cam_free;

    /* This monitor's own wallpaper, fitted to its own size, panned by its own
     * camera. */
    struct FwmWallpaper *wallpaper;
    struct FwmWallpaper *wallpaper_prev;  /* outgoing set, alive during a fade */
    /* The grass along the bottom of THIS monitor: its own width, and rooted on
     * its own bottom edge, which is where the floor of the desktop it shows
     * is. NULL whenever [grass] is off. */
    struct FwmGrass *grass;
    struct FwmStarDraw *star_draw;
    /* Its own status strip, at the top of this monitor, and where that strip's
     * islands landed — hit-testing and the modes menu's anchor both read the
     * geometry of the strip they were aimed at, never another monitor's. */
    struct wlr_scene_buffer *tray_buffer;
    TrayStrip tray_strip;
    struct wlr_box usable_area;           /* this monitor minus exclusive zones */
    /* An external bar has reserved space along the TOP of this monitor, where
     * our own status strip lives. A FACT about the screen, not a decision:
     * whether the strip actually stands down for it is [decor] tray_yield,
     * read where this is used. Keeping the two apart is what lets the setting
     * be changed live — the fact only moves when the bars do, and
     * layer_arrange is the one place that recomputes it. */
    int top_reserved;
    /* This monitor as ext-workspace-v1 shows it: a group, holding whichever
     * desktop it is currently displaying (see workspace.h). */
    struct wlr_ext_workspace_group_handle_v1 *ws_group;

    /* Impact shake, and the slide across the ring's join. Both move what this
     * ONE monitor draws without moving its camera — the camera must not move,
     * or edge auto-scroll and the active-desktop test would see the offset —
     * so they are added on the way to the scene (server_place_node) instead of
     * by shifting a tree every monitor shares. */
    double shake_mag;   /* px; decays to 0 */
    double shake_t;     /* seconds since the last impact, drives the oscillation */
    double wrap_slide;  /* px still to travel; 0 when nothing is sliding */
    int wrap_dir;       /* +1 travelling right, -1 left */
    struct wlr_scene_buffer *wrap_ghost;   /* the desktop being left */
    struct wlr_buffer *wrap_ghost_buf;
    int render_dx, render_dy;              /* the two of them, resolved */
    /* The monitor this one is trading desktops with, for as long as the two
     * cameras are still travelling.
     *
     * A trade is the one move where a desktop changes SCREENS: each monitor
     * takes the other's, so its windows are drawn by their new monitor from the
     * moment it is asked for, while that monitor's camera is still standing
     * over on the far side. The whole animation is therefore the two desktops
     * crossing the gap between the screens — and it is the one case where a
     * window over the neighbour's box belongs there and must not be cut off
     * (see server_views_clip). NULL at every other moment. */
    struct FwmOutput *swap_with;
    /* And the jump that crossing would otherwise be.
     *
     * A monitor draws a window at `wx - camera_x + box.x`, `wy + box.y`: the
     * frame is the SCREEN's, so the instant a desktop changes hands its windows
     * are drawn in the new screen's frame — a screen that sits somewhere else in
     * the layout and whose camera is still on the far side. They teleported to
     * where they were about to fly out FROM and only then travelled, which is
     * not a crossing, it is a cut with a slide after it. (Horizontally the
     * camera happens to cancel it for neighbouring desktops; vertically there is
     * no camera at all, so a screen mounted lower than its neighbour jumped
     * every window by the difference.)
     *
     * So the difference between the two frames is taken at the moment of the
     * trade and eased away over exactly the camera's own ride: at t=0 the window
     * is drawn precisely where it already was, at t=1 the offset is gone and the
     * screen's frame is the plain truth again. */
    int swap_dx0, swap_dy0;   /* the whole jump, in layout pixels */
    double swap_dx, swap_dy;  /* what is left of it */
    /* The desktop strip has taken this screen over: its windows are parked off
     * the layout until the strip closes. Per monitor, so opening the strip on
     * one screen leaves the other one working. */
    int hide_world;

    struct wl_listener frame;
    struct wl_listener destroy;
} FwmOutput;

typedef struct {
    FwmInteractiveAction action;
    struct FwmView *view;
    double start_x, start_y;
    int view_start_x, view_start_y;
    int view_start_width, view_start_height;
    
    /* Throw speed history */
    double last_x, last_y;
    struct timespec last_time;
    double vx, vy;
    double hist_x[4];
    double hist_y[4];
    struct timespec hist_time[4];
    int hist_count;
    int collision_disabled;
    /* A tiled window is held, but the hand has not yet moved far enough to mean
     * it. Until it does the window stays in the layout and nothing has
     * happened, so letting go is a plain click (see TILE_TEAR_PX). */
    int tile_grab;

    /* Which monitor the hand was last over, so a drag notices when it crosses
     * to another screen (drag_cross_screens). The camera reading itself lives
     * in cam_offset below: the drag anchors its window with a screen delta, so
     * anything that moves the world underneath it — edge auto-scroll, above
     * all — has to be added back in, and that is done in one place, by the
     * placement that reads it (drag_place). */
    FwmOutput *cam_output;
    int cam_have;
    /* Where the world sits under the screen the hand was last on: camera_x
     * minus box.x, which is what turns a layout coordinate into a world one.
     * Kept as a NUMBER rather than read back off cam_output, because a monitor
     * can be unplugged in the middle of a drag and that pointer is then a
     * pointer to a freed output — comparing it is one thing, following it to
     * read two fields is another. */
    int cam_offset;
    /* And the same for the other axis: -box.y, because a column has no camera
     * vertically (server_world_to_screen adds box.y and nothing else). Two
     * monitors of different height are almost never aligned along their tops —
     * the layout stands them on a shared centre or a shared bottom — so this
     * is the difference that made a window jump as the hand crossed. */
    int cam_offset_y;

    /* Swirl: how the cursor's direction of travel is turning, which is what
     * winds a spinning window up mid-drag (see server_pointer.c). `dir` is the
     * angle of the velocity vector at the last sample; `acc`/`abs`/`span` are
     * a leaky integral of how far it has turned, how much it turned either
     * way, and over how long — so the rate is read from a fifth of a second of
     * hand movement, and a wobble that nets out to nothing is told apart from
     * a circle that does not. */
    double swirl_dir;
    struct timespec swirl_time;
    double swirl_acc, swirl_abs, swirl_span;
    int swirl_have;   /* a previous sample exists to compare against */

    /* Where the drag took hold of the window, in the window's OWN frame
     * (unrotated, relative to its center). A spinning window hangs from this
     * point: grab it by a corner and it swings like a real object held there
     * (server.c, server_drag_swing). Fixed for the length of the drag — the
     * hand does not slide along the window. */
    double grab_lx, grab_ly;
    /* The grab point in world coordinates, and how fast it is moving, as of
     * the last physics tick. The swing is driven by this point's
     * ACCELERATION, so both are needed. */
    double pivot_x, pivot_y;
    double pivot_vx, pivot_vy;
    /* ... and the smoothed acceleration itself, which is state rather than a
     * local because it is filtered across ticks (SWING_ACC_TAU). */
    double pivot_ax, pivot_ay;
    int pivot_have;
    
    /* Twist (FWM_ACTION_TWIST). `twist_base` is the angle the window is being
     * held at — seeded from its own angle at the grab, then moved by however
     * far the cursor turns around its centre, so the window never jumps to meet
     * the hand. `twist_last` is the previous cursor angle, for unwrapping.
     * `twist_vel` is how fast the hand is turning, smoothed, because the
     * release hands that rate to the simulation and one stuttering frame must
     * not be what decides it. */
    double twist_base, twist_last;
    double twist_vel;
    struct timespec twist_time;

    /* BSP resize. The node is borrowed from a tree that is free to rebuild
     * itself under the hand — a window opening, closing or leaving the desktop
     * does exactly that — so it is only ever touched after bsp_contains says it
     * is still there, and bsp_desktop remembers whose tree to ask. That desktop
     * is also the one the drag lays out again: the monitor the hand is on can
     * be showing another one. */
    /* Two of them, one per axis: the resize grabs the window's nearest corner,
     * so the hand usually moves a vertical divider and a horizontal one at the
     * same time. Either may be NULL — a window against the edge of the screen
     * has no divider on that side, and then only the other axis moves. */
    BspNode *bsp_node;      /* the vertical line, splitting left from right */
    BspNode *bsp_node_v;    /* the horizontal one, splitting top from bottom */
    int bsp_desktop;
    float bsp_start_ratio, bsp_start_ratio_v;
    
    /* Which corner of the window the hand took hold of, for a resize: the two
     * edges nearest the grab point are the ones that move, exactly as a tiled
     * window's are. Anchoring the top-left unconditionally meant a window
     * grabbed at its left edge grew to the RIGHT, away from the hand, and the
     * only way to move a left edge at all was to move the whole window first.
     * A client asking for the resize itself (xdg_toplevel.resize) names its
     * edges, and those win over where the cursor happens to be. */
    int resize_left, resize_top;
    /* The last size a resize actually asked the client for. Pointer motion
     * arrives far faster than any client redraws, and asking again for a size
     * it is already working on only lengthens the queue it is behind. */
    int sent_w, sent_h;

    /* Swap drag */
    double cur_x, cur_y;
} FwmInteractiveState;

/* How a ghost leaves. An ordinary close mirrors the map fade; a window
 * destroyed by a collision ([physics] hp) collapses to a point instead, so the
 * two read as different events rather than as the same one twice.
 *
 * An enum rather than a flag because this is the obvious place to hang the
 * rest: shattering into pieces, cracking first, whatever else a death is worth.
 * Everything below GHOST_FADE animates over the same clock, so a new kind is a
 * case in the tick and nothing else. */
enum {
    GHOST_FADE = 0,   /* closed: opacity 1 -> 0, what it has always done */
    GHOST_IMPLODE,    /* destroyed: shrinks to nothing about its own centre */
};

/* Snapshot of a closed window's last frame, animating out (close animation).
 * Owns one lock on `buffer`; released when the animation ends. */
typedef struct FwmGhost {
    struct wlr_scene_buffer *scene_buffer;
    struct wlr_buffer *buffer;
    double x, y; /* world coordinates (camera-independent) */
    int w, h;    /* size at full scale: an implode has to shrink from something */
    int kind;    /* GHOST_* */
    double t;    /* progress 0 -> 1 */
    /* Where the animation has walked the ghost away from (x, y) this frame — an
     * implode shrinks about its centre, so the origin moves inward. Kept here
     * rather than folded into x/y because it is recomputed from t every frame,
     * and because server_views_place has to be able to reproduce it: a desktop
     * switch mid-collapse otherwise snaps the ghost back to its full-size
     * corner for one frame. */
    double draw_dx, draw_dy;
    struct wl_list link;
} FwmGhost;

typedef struct FwmServer {
    struct wl_display *wl_display;
    struct wlr_backend *wlr_backend;
    struct wlr_session *session; /* NULL on nested backends; used for VT switching */
    struct wlr_renderer *wlr_renderer;
    struct wlr_allocator *wlr_allocator;
    /* wlroots raises renderer->events.lost when the driver reports a GPU reset
     * — a hung client (a game on a shader pack that runs the card out of VRAM
     * is the usual one) taking the whole context down with it. Everything
     * drawn on that context is garbage from then on, on EVERY monitor, because
     * one renderer serves them all. server_lifecycle.c rebuilds it. */
    struct wl_listener renderer_lost;
    bool renderer_recovering;   /* guards re-entry while the swap is under way */
    struct wlr_scene *scene;
    struct wlr_scene_tree *layer_background;
    struct wlr_scene_tree *layer_windows;
    struct wlr_scene_tree *layer_overlay;
    /* wlr-layer-shell trees, interleaved with ours (bottom to top):
     * wallpaper < ls_background < ls_bottom < windows < ls_top < our overlays
     * < ls_overlay. See src/layer.h. */
    struct wlr_scene_tree *ls_background;
    struct wlr_scene_tree *ls_bottom;
    struct wlr_scene_tree *ls_top;
    struct wlr_scene_tree *ls_overlay;
    /* Audio spectrum bars along the bottom of the screen. NULL whenever [cava]
     * is off, fwm was built without PipeWire, or no capture stream could be
     * opened — all three are ordinary, and every use site must expect NULL. */
    struct FwmCava *cava;
    /* The mode server_cava_sync last ACTED on, which is not the same as the
     * mode `cava` is in: a build that fails (no sound server) leaves cava NULL,
     * and without remembering the attempt the tick would retry — and log — sixty
     * times a second forever. Zero-initialised to CAVA_MODE_OFF, which is also
     * the correct starting state: off has nothing to build. */
    int cava_applied;
    /* When the row may next be attempted, on the monotonic clock. A mode that
     * is on with no sound server to capture is retried on this timer instead of
     * being abandoned — see server_cava_sync. */
    double cava_retry_at;
    int cava_reported;   /* the "no sound server" line has been logged once */

    /* The wind strength the modes menu last switched OFF, so switching it back
     * on restores what was there instead of the built-in default. Zero until
     * the switch has been used at all. */
    double grass_wind_saved;

    /* The knock windows make when they collide ([sound] collisions). NULL
     * whenever the feature is off or the mixer thread could not be started, and
     * every use site must expect that. `sound_applied` is the setting the sync
     * last acted on, so a toggle is noticed without anyone telling it. */
    struct FwmSound *sound;
    int sound_applied;

    /* What the tray's stats pill shows. NULL only if the allocation failed;
     * an empty [stats] is a live handle with no sensors in it, because the
     * menu still has to open and say so. */
    struct FwmStats *stats;

    /* [physics] mass = "ram": the mode server_mass_sync last acted on, and when
     * it may next walk /proc. Zero-initialised to PHYSICS_MASS_SIZE, which is
     * both the default and a state with nothing to do — so a compositor nobody
     * asked for this never reads /proc at all. */
    int mass_applied;
    double mass_sample_at;

    /* Hit points have to be frozen against the mass that mode produces, and
     * server_mass_sync only sets the SCALE — the mass itself is not written
     * until physics_step resolves the material. So the freeze is deferred by
     * one step rather than reading a figure that is about to change. */
    int hp_freeze_pending;

    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wl_list layer_surfaces;             /* FwmLayerSurface.link */
    struct FwmLayerSurface *focused_layer;     /* owns the keyboard, if any */
    struct wlr_box usable_area;                /* screen minus exclusive zones */
    struct wlr_compositor *compositor;
    struct wlr_xwayland *xwayland;
    
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_seat *seat;
    
    struct wl_list views;
    /* Override-redirect X11 surfaces (FwmXwlUnmanaged.link, server_shell.c).
     * Not views and never in `views`, but they are on a desktop and they move
     * with it, so the camera and the focus have to be able to find them. */
    struct wl_list xwl_unmanaged;
    struct wl_list groups; /* FwmGroup tab-stacks */
    struct wl_list ghosts; /* FwmGhost close-animation snapshots */ /* FwmView list */
    struct FwmView *focused_view;

    /* Where the light is this frame, and when the clock was last asked. One
     * light for the whole compositor: every window's shadow is cut from it, so
     * they all agree about where the sun is without any of them working it out
     * for itself.
     *
     * In clock mode the position is recomputed on a timer rather than every
     * tick — a day of sun moves about four thousandths of a degree per frame,
     * and trigonometry sixty times a second to find that out is the definition
     * of work nobody asked for. */
    FwmSunLight sun_light;
    double sun_checked_at;

    /* The star, if [star] put one on a desktop. One of these for the session —
     * it is an object in the world, not a property of a screen — while the
     * PICTURE of it belongs to each monitor that can see its desktop
     * (FwmOutput.star_draw). Its light is not cached the way the sun's is:
     * every window gets a different answer, so there is nothing to cache. */
    FwmStar star;
    bool star_running;
    /* The pointer has hold of it. Kept here rather than in the interactive
     * struct because a star is not a view and none of that machinery — the
     * layout, the tiling drop, the snap — means anything for it. */
    int star_drag;
    double star_drag_x, star_drag_y;   /* last pointer position, world px */
    double star_drag_vx, star_drag_vy; /* what it will be thrown with */

    /* An override-redirect X11 surface currently holding the keyboard. It has
     * no view — that is what unmanaged means — so it cannot be focused_view,
     * yet it is where the keys are going: a Wine game's own fullscreen window,
     * or a menu that asked for input focus. Remembered so the keyboard can be
     * handed back to focused_view when it goes away, and so any focus change
     * elsewhere knows to take it back (server_focus_view). NULL the rest of
     * the time, which is nearly always. */
    struct wlr_xwayland_surface *focused_unmanaged;
    struct FwmView *last_touched_view;
    
    struct wl_list outputs;
    
    /* Inputs */
    struct wl_listener new_output;
    /* Fires after an output joins, leaves or changes mode — the single place
     * the world is resized to fit every monitor. */
    struct wl_listener output_layout_change;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_cursor;
    struct wl_listener seat_request_set_selection;
    struct wl_listener seat_request_set_primary_selection;

    /* Touchpad gestures. The state machine is gestures.c; the wiring, and what
     * each resolved gesture does, is server_gestures.c. A gesture fwm has
     * nothing bound to is forwarded to the client through pointer_gestures
     * instead, so in-app pinch-zoom keeps working. */
    struct wlr_pointer_gestures_v1 *pointer_gestures;
    GestureState gesture;
    int gesture_base_camera; /* camera the live desktop pan started from */
    struct wl_listener cursor_swipe_begin;
    struct wl_listener cursor_swipe_update;
    struct wl_listener cursor_swipe_end;
    struct wl_listener cursor_pinch_begin;
    struct wl_listener cursor_pinch_update;
    struct wl_listener cursor_pinch_end;
    struct wl_listener cursor_hold_begin;
    struct wl_listener cursor_hold_end;

    /* Drag and drop. The data transfer itself is handled entirely by
     * wlr_data_device; all we own is the icon drawn under the cursor. */
    struct wl_listener seat_request_start_drag;
    struct wl_listener seat_start_drag;
    struct wlr_scene_tree *drag_icon;      /* NULL when no drag is running */
    struct wl_listener drag_icon_destroy;

    /* xdg-activation: apps asking to be raised/focused (a link opening in an
     * already-running browser, a chat client jumping to a message). */
    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wl_listener xdg_activation_request_activate;

    /* Idle: ext-idle-notify tells idle daemons (swayidle) when the user goes
     * quiet; idle-inhibit lets a client (a video player) suppress that. */
    /* Window list for external panels (waybar taskbar); see foreign.h. */
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel;

    /* Pointer capture: games and 3D viewports lock the cursor and steer from
     * raw deltas instead of its absolute position. */
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wl_listener new_pointer_constraint;
    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct wlr_pointer_constraint_v1 *active_constraint; /* NULL when free */
    struct wl_listener constraint_destroy;

    /* The implicit pointer grab: while a button is down the events keep going
     * to the surface it was pressed on, wherever the cursor wanders. Recorded
     * on every motion that is NOT holding a button, so a press always finds
     * the surface under it already measured.
     *
     * ptr_surface is only ever COMPARED against the seat's focused surface,
     * never dereferenced: when the client goes away wlroots clears the seat's
     * side and the comparison simply stops matching. */
    struct wlr_surface *ptr_surface;      /* what the pointer was last over */
    struct FwmView *ptr_view;             /* its window, NULL for unmanaged X */
    double ptr_ox, ptr_oy;                /* that surface's origin, in layout */
    int ptr_node_have;                    /* was the window's node placed? */
    int ptr_node_x, ptr_node_y;           /* and where, so a window that moves
                                           * under the grab carries it along */

    /* Display power (swayidle turning the screen off), gamma (wlsunset night
     * light) and client-requested cursor shapes. */
    struct wlr_output_power_manager_v1 *output_power;
    struct wl_listener output_power_set_mode;
    struct wlr_gamma_control_manager_v1 *gamma_control;
    struct wlr_cursor_shape_manager_v1 *cursor_shape;
    struct wl_listener cursor_shape_request;

    /* Which clients arrived through a sandbox, and thus never see the
     * privileged globals; see sandbox.h. */
    struct wlr_security_context_manager_v1 *security_context;

    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
    int idle_inhibited;                    /* last state pushed to the notifier */

    /* fwm's own idle timers ([idle]) — see server_idle.c. `idle_secs` is time
     * since the last input, frozen while an inhibitor is up; `idle_blanked` is
     * "the screens are dark and only input will light them"; `idle_locked`
     * keeps the locker from being started twice for one stretch of idleness;
     * `idle_woke` marks the one press that lit the screens, so it is not also
     * typed into the window under it. `idle_audio` is the last thing the sound
     * card said and `idle_audio_wait` how long until it is asked again — a poll
     * that only runs in the seconds around a threshold. */
    /* The charge watcher's memory: when the next read is due and which
     * warnings this discharge has already used up. See battery.h. */
    BatteryWatch battery;

    double idle_secs;
    int idle_blanked;
    int idle_locked;
    int idle_woke;
    double idle_audio_wait;
    int idle_audio;

    /* ext-session-lock-v1. `locked` stays set if the lock client dies, which
     * is what keeps a crashed locker from becoming an unlock — see src/lock.h. */
    /* Control socket (src/ipc.h). NULL if it could not be created. */
    struct FwmIpc *ipc;

    /* Session save/restore bookkeeping (src/session.c); opaque here. Not to be
     * confused with `session` above, which is the libseat/VT session. */
    void *session_state;

    /* Which desktop each application we started was started FROM
     * (src/launched.h), so a window that takes its time to appear still opens
     * where it was asked for. NULL until something is launched. */
    struct FwmLaunched *launched;

    /* MAX_WINDOWS overflow is reported once per session, not once per window:
     * the tray pill stores a capped 24 messages and would otherwise fill with
     * copies of the same one. */
    int warned_window_limit;

    /* The seat's selection: fwm's own offers (a screenshot), and the text a
     * dead client left behind ([clipboard] persist). See clipboard.h. */
    struct FwmClipboard *clipboard;

    struct wlr_session_lock_manager_v1 *lock_manager;
    struct wlr_session_lock_v1 *lock;      /* NULL once the client is gone */
    int locked;
    struct wlr_scene_tree *layer_lock;     /* above everything, incl. ls_overlay */
    struct wl_list lock_surfaces;          /* FwmLockSurface.link */
    struct wl_listener new_lock;
    struct wl_listener new_lock_surface;
    struct wl_listener lock_unlock;
    struct wl_listener lock_destroy;

    /* hyprland-global-shortcuts-v1: keybinds an external shell has claimed
     * (see shortcuts.h). Empty unless some client registered one. */
    struct wl_list shortcuts;

    /* ext-workspace-v1: the ten desktops as external bars see them (see
     * workspace.h). One handle each, for the compositor's whole life, plus the
     * state each was last told so an unchanged tick says nothing. */
    struct wlr_ext_workspace_manager_v1 *workspace_manager;
    struct wl_listener workspace_commit;
    struct wl_listener workspace_destroy;
    struct wlr_ext_workspace_handle_v1 *workspace[FWM_DESKTOPS];
    struct wlr_ext_workspace_group_handle_v1 *workspace_group[FWM_DESKTOPS];
    bool workspace_active[FWM_DESKTOPS];


    /* Keyboard input */
    struct wl_list keyboards;
    struct wl_list pointers;   /* struct FwmPointer; see server_internal.h */
    struct wl_list switches;   /* struct FwmSwitch: the laptop lid */
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_toplevel_decoration;
    struct wl_listener xwl_ready;
    struct wl_listener xwl_new_surface;

    /* Held-key auto-repeat for repeatable binds (e.g. move_camera) */
    struct wl_event_source *key_repeat_timer;
    const char *repeat_action;
    /* Launcher key auto-repeat (arrows/backspace/typing while it is open). */
    int repeat_l_active;
    xkb_keysym_t repeat_l_sym;
    char repeat_l_utf8[16];   /* points into config.keys[].action; NULL when idle */
    uint32_t repeat_keycode;     /* raw event keycode currently repeating */
    unsigned char key_consumed[768]; /* per-keycode: press was eaten by a bind,
                                        swallow the matching release too */
    int click_consumed; /* a press was eaten (a tab-bar click, the click that
                         * woke a blanked screen); swallow its release too, or
                         * the client sees a release it never saw a press for */

    /* When the last physics step finished. The tick timer and the display's
     * vsync are different clocks — the timer is armed in whole milliseconds, so
     * 60Hz becomes 16ms and runs at 62.5Hz — and anything the simulation
     * integrates therefore advances in steps that do not line up with the
     * frames it is drawn on. Rendering interpolates from this; see
     * server_render_angle. */
    struct timespec last_tick;
    /* When the tick callback last ran, and the real time it has taken in but
     * not yet spent on a whole step. Together these say how far behind the
     * clock the simulation is at any instant. */
    struct timespec tick_real_prev;
    double sim_accum;

    /* Effect frame diagnostics, on only when FWM_DEBUG_EFFECTS is set in the
     * environment. Judder is a timing complaint, and timing cannot be argued
     * about from the code — this counts what actually reached the screen while
     * a window was spinning or wobbling, and says so once a second.
     * `fx_snaps` counts composited re-photographs, `fx_moved` the frames where
     * the picture actually changed. */
    int fx_debug;
    struct timespec fx_since;
    int fx_frames, fx_snaps, fx_moved;
    double fx_dt_min, fx_dt_max;
    /* How evenly a rotation actually reaches the screen.
     *
     * Not the angle step itself: over a second that conflates judder with a
     * spin honestly slowing down, and it reads a wrap past +-pi as a 355-degree
     * jump. What matters is the drawn angular SPEED — step over the real time
     * the frame took — and how much it changes from one frame to the next.
     * Smooth motion holds it steady whatever the frame times are; judder is
     * precisely this number jumping. Reported as the worst frame-to-frame
     * change, in percent. */
    double fx_omega_prev;
    double fx_omega_jump;   /* worst |domega|/omega seen this second, 0..1 */
    int    fx_omega_have;
    double fx_snap_us;   /* time spent flattening subtrees this second */

    /* The [mode.<name>] submap the keyboard is currently in, as an index into
     * config.modes, or -1 for the root map. While a mode is active it owns
     * every key: its binds fire, Escape leaves, and nothing else reaches the
     * focused client. Reset on config reload, since the modes are rebuilt from
     * scratch and the index would otherwise point at a different one. */
    int key_mode;
    
    /* Physics and desktop coordinates */
    PhysicsWorld physics;
    /* Impact shake. Deliberately a RENDER-ONLY offset applied to the world
     * layer trees: camera_x must not move, because edge auto-scroll and the
     * active-desktop test compare it against target_camera_x exactly. */
    /* FWM_TEST_ACTION debug hook: fires one action shortly after startup. */
    char *test_action;
    struct wl_event_source *test_action_timer;
    int focus_desktop;  /* desktop server_refocus last homed the keyboard on */
    int tick_idle;      /* physics timer is on the slow heartbeat */

    /* The size of ONE desktop, taken from the largest monitor — so that no
     * screen is ever bigger than the column it shows, and every screen shows a
     * whole desktop. The world is a strip of FWM_DESKTOPS columns this wide, and each monitor shows one of
     * them through its own camera — see FwmOutput and server_output.c.
     *
     * This is the strip's shape, not any particular screen's. Anything that
     * has to line up with the GLASS — where a window comes to rest, where a
     * fullscreen video ends — asks the monitor showing that desktop instead
     * (server_output_showing), because on a mixed-resolution setup the two
     * answers differ. PhysicsWorld.desktop_h is that rule for the floor. */
    int screen_width;
    int screen_height;

    BspNode *bsp_roots[FWM_DESKTOPS];
    int desktop_mode[FWM_DESKTOPS];
    /* Desktops asking to be looked at — the red digit in the tray. Per desktop
     * rather than per window on purpose; see src/urgent.h for why, and for the
     * three things that raise one. */
    bool desktop_urgent[FWM_DESKTOPS];
    
    FwmConfig config;
    FwmInteractiveState interactive;
    
    struct wl_event_source *physics_timer;
    /* Drives frames at a playing video wallpaper's own fps, so video does not
     * pin the whole compositor to 60 Hz. Armed only while a video plays and is
     * not covered; see server_video_sync. */
    struct wl_event_source *video_timer;
    int video_timer_on;
    struct timespec last_anim; /* frame-time clock for visual animations */
    
    /* UI scene nodes */
    /* Tray hidden by the user (toggle_tray). Unlike the automatic hide under a
     * real-fullscreen window, this also gives the strip back: tile_area and
     * fake fullscreen stop reserving TRAY_BOTTOM, so windows grow into it. */
    int tray_hidden;
    struct wlr_scene_buffer *hints_buffer;
    struct wlr_scene_buffer *welcome_buffer;
    struct wlr_scene_buffer *errors_buffer; /* config-error detail panel */
    /* Modes menu, opened from the tray pill. NULL when closed, and that NULL is
     * also what the pill reads to draw itself pressed. */
    struct wlr_scene_buffer *modes_buffer;
    /* The stats pill's own menu, on exactly the same terms and the same
     * button: two neighbouring islands that opened on different clicks would
     * teach the hand nothing it could reuse. */
    struct wlr_scene_buffer *stats_buffer;
    struct Launcher *launcher;
    /* The radial menu, on the launcher's terms: it owns the keyboard and
     * the pointer while it is up. Never NULL after startup — being open is
     * its own flag, not this pointer (see radial_is_open). */
    struct Radial *radial;
    /* The per-application volume panel, on the ring's terms — keyboard and
     * pointer while it is up. Never NULL after startup (see mixer_is_open). */
    struct Mixer *mixer;
    /* The dial readout: what a `set:` bind is worth now, low on the screen for
     * a second after the turning stops. Never NULL after startup; being up is
     * its own flag (see osd_busy). */
    struct Osd *osd;
    /* The system volume, for the `volume:` action: what fwm last knew it to
     * be, and the reader that keeps that honest. Never NULL after startup. */
    struct Volume *volume;
    /* The knob's recent history, for server_knob_step: when the last detent
     * arrived, which way it went, and how many have arrived in a row without
     * the hand pausing. */
    uint32_t knob_last_ms;
    int      knob_dir;
    int      knob_run;
    /* The desktop strip (expo). NULL when closed — that NULL is the mode flag
     * every input path tests, so there is no second copy of "is it open". */
    struct FwmExpo *expo;
    /* The screenshot region selector, on the same terms: NULL when it is not
     * up, and that NULL is what the input paths test (see src/screenshot.h). */
    struct FwmShotPicker *shot_picker;
    /* The still the selector is aimed at: a copy of the monitor's last frame,
     * held over that monitor for as long as the rectangle is being dragged, so
     * the picture cannot move out from under the hand choosing it. NULL when
     * nothing is frozen. It outlives the selector by one commit, because the
     * shot is read back from the frame that still has it on screen. */
    struct wlr_scene_buffer *shot_freeze;

    /* `fwm -debug`: a scratch session, started beside the one you are living
     * in. It shares a HOME with that session, so it stays away from everything
     * of the real one's that is kept there — the saved session above all — and
     * brings up its own two windows instead. session_debug_desktop has the
     * whole of what it does. */
    int debug;

    int running;
} FwmServer;

bool server_init(FwmServer *server, bool debug);
void server_run(FwmServer *server);
void server_destroy(FwmServer *server);

/* Ask every output for a frame. The scene schedules frames off its own damage,
 * so this is only for changes that damage nothing — a colour transform, or the
 * idle heartbeat. */
void server_schedule_frames(FwmServer *server);

/* Re-home the keyboard after the focused window disappears or the camera lands
 * on another desktop, instead of waiting for the next pointer motion.
 * `skip` excludes a view that is unmapping but still listed; NULL otherwise. */
void server_refocus(FwmServer *server, int desktop, struct FwmView *skip);
void server_focus_view(FwmServer *server, struct FwmView *view);
/* Give the keyboard to a surface. Use this rather than
 * wlr_seat_keyboard_notify_enter: it leaves out the keys a bind has swallowed,
 * whose release the new client will never be told about. */
void server_keyboard_enter(FwmServer *server, struct wlr_surface *surface);
/* Take the keyboard away with nothing to give it to. The other half of
 * server_keyboard_enter, and guarded like it: a locked session and a layer
 * surface holding the keyboard exclusively are not to be taken from. */
void server_keyboard_clear(FwmServer *server);
/* The surface the keyboard belongs to right now — a layer surface that asked
 * for it, else focused_unmanaged if one holds it, else the focused window's.
 * NULL when there is none of the three. */
struct wlr_surface *server_keyboard_target(FwmServer *server);
/* Tell the client under the cursor that it has the cursor, with no motion
 * event to carry the news. Call after anything that moves the world under a
 * stationary pointer — a camera crossing to another desktop, a warp — or the
 * next click goes to whatever the pointer was over before. */
void server_pointer_resync(FwmServer *server);
/* Let go of whatever is holding the pointer, if anything is: a game that
 * locked it for mouse-look, a window confining it to a region. For the
 * overlays that aim with the cursor — the screenshot selector — since a locked
 * pointer does not move at all and the selector would have nothing to drag a
 * rectangle with. The constraint comes back on its own when the pointer next
 * enters the surface that asked for it. */
void server_pointer_release_constraint(FwmServer *server);
/* Move the pointer into a window that was just given the keyboard, so
 * focus-follows-pointer does not take the focus straight back. A no-op when
 * the pointer is already inside it. */
void server_warp_to_view(FwmServer *server, struct FwmView *view);
/* Take an override-redirect X11 surface as an unmanaged one: a bare scene
 * surface, no body, no borders. Public because a window can stop being a
 * managed one while it is alive (see xwl_handle_set_override_redirect), and
 * the surface then has to be handed over here. */
void server_xwl_unmanaged_create(FwmServer *server, struct wlr_xwayland_surface *xs);
/* Every mapped unmanaged surface put back where its desktop is on screen, or
 * parked if that desktop is not being shown. The unmanaged half of
 * server_views_place, and called from it. */
void server_xwl_unmanaged_place(FwmServer *server);
/* And every one of them raised back over the window layer. */
void server_xwl_unmanaged_raise(FwmServer *server);
/* A real fullscreen window put back on top of the ordinary windows sharing its
 * desktop, with its own dialogs kept above it. Called whenever something else
 * has been raised — a focus, a window opening — since the stack is otherwise
 * "whatever was raised last wins" and a fullscreen window would sink under the
 * next thing to open. */
void server_restack_fullscreen(FwmServer *server);
/* Give the keyboard to an unmanaged surface on `desktop` that wants it, if
 * there is one. Arriving on the desktop a fullscreen X11 game is on has to put
 * the keys back in the game; nothing else would, since it is not a view and so
 * never a candidate in the scan server_refocus does. */
bool server_xwl_unmanaged_refocus(FwmServer *server, int desktop);
/* ── monitors ─────────────────────────────────────────────────────────────
 * The world is a strip of FWM_DESKTOPS columns of screen_width; each monitor
 * shows one column. These are how the rest of the compositor asks "which
 * screen?" — see FwmOutput above and server_output.c. */

/* The monitor at the layout origin. NULL only before the first one arrives. */
FwmOutput *server_primary_output(FwmServer *server);
/* The monitor a LAYOUT point (the cursor, a scene node) is on, or NULL. */
FwmOutput *server_output_at(FwmServer *server, double lx, double ly);
/* The monitor the user is working on: the one under the pointer, else the
 * primary. What a bind means when it says "this desktop". */
FwmOutput *server_active_output(FwmServer *server);
/* The monitor showing desktop `d`, or NULL when none is. */
FwmOutput *server_output_showing(FwmServer *server, int d);
/* The monitor that draws window `v`, whose left edge is at `wx` and which is
 * `span` wide, and how far that monitor's own animations shift it (either
 * pointer may be NULL). Everything that draws, cuts or measures a window asks
 * THIS — a window in the hand is deliberately not on the screen a plain
 * "who shows this column" lookup would name. See server_output.c. */
FwmOutput *server_view_frame(FwmServer *server, struct FwmView *v, double wx,
                             double span, double *shift_x, double *shift_y);
/* Trade two desktops: what stood on `a` stands on `b` and the other way round,
 * windows, layout tree and mode together. The places themselves do not move —
 * the camera stays where it is and each monitor goes on showing the desktop it
 * was showing, with different windows on it. */
void server_swap_desktops(FwmServer *server, int a, int b);
/* Everything on desktop `d` re-measured against whichever monitor is showing
 * it now: windows out on a strip of world the glass does not reach are brought
 * back, a fullscreen window is refitted, and a tiling layout is re-split. */
void server_desktop_refit(FwmServer *server, int d);
/* The nth monitor as a person counts them — left to right, then top to bottom,
 * one-based, the primary always 1. A number past the last screen gives the last
 * screen, so a bind for a monitor you have not plugged in yet still lands
 * somewhere sensible. NULL only when nothing is lit. */
FwmOutput *server_output_nth(FwmServer *server, int n);
/* Move the session to that monitor: the pointer goes to the middle of it and
 * the keyboard follows to whatever is there. */
void server_focus_output(FwmServer *server, FwmOutput *out);
/* Light a monitor or put it out at runtime — a keybind, `fwmctl dispatch`, the
 * laptop lid. A dark monitor leaves the layout entirely, so it holds no desktop
 * and no window can be placed on it; lighting it again gives it a free desktop,
 * a wallpaper and a strip, exactly like one just plugged in. Returns 1 if
 * anything changed; refuses to put out the last lit screen. */
int server_output_set_enabled(FwmServer *server, FwmOutput *out, int on);
/* The built-in laptop panel, or NULL. Name-based (eDP/LVDS/DSI), which is what
 * every other compositor does with it. */
FwmOutput *server_internal_output(FwmServer *server);
/* The monitor with this connector name ("HDMI-A-1"), or NULL. */
FwmOutput *server_output_find(FwmServer *server, const char *name);
/* The monitor wrapping this wlr_output, or NULL. What the protocol handlers
 * need: they are handed a wlr_output and have to get back to our own. */
FwmOutput *server_output_for(FwmServer *server, struct wlr_output *wlr_output);

/* How one monitor should be driven: what `[[output]]` and `fwmctl output` both
 * end up saying. Every field is optional, and the have_* flags are what make
 * "put this screen at 1920,0" leave its resolution alone — a request carries
 * only what was asked for, never a full state that would overwrite the rest. */
typedef struct {
    int    have_mode;
    int    mode_w, mode_h;
    int    mode_refresh;    /* mHz; 0 = whatever refresh that size comes in */
    int    have_scale;
    double scale;
    int    have_transform;
    int    transform;       /* enum wl_output_transform */
    int    have_pos;
    int    x, y;            /* top-left in layout coordinates */
} FwmOutputSetup;

/* Apply a setup to one monitor, atomically: the whole thing is tested against
 * the hardware first, so a refused mode leaves the screen exactly as it was
 * rather than half-changed or black. Returns true on success; on failure it
 * writes a one-line reason into `err` (which may be NULL).
 *
 * Anything this changes resizes or moves the monitor's box, and the layout's
 * change event does the rest — wallpaper, strip, cameras, tiling. */
bool server_output_apply_setup(FwmServer *server, FwmOutput *out,
                               const FwmOutputSetup *setup, char *err, size_t err_len);
/* The setup a `[[output]]` entry asks for. Only the fields the file actually
 * set come back flagged. */
FwmOutputSetup server_output_setup_from_config(const ConfigOutput *cfg);
/* The lid opening or closing, from the switch device in server_input.c. */
void server_lid_changed(FwmServer *server, int closed);

/* The desktop of the active monitor, and the desktop a world x falls on. */
int server_active_desktop(FwmServer *server);
int server_desktop_at_x(FwmServer *server, double wx);

/* World coordinates to layout coordinates, through the monitor showing that
 * point's desktop — or, while no monitor owns it, through one whose camera is
 * standing over it mid-slide, so the desktop being left goes on being drawn
 * until it has travelled off the screen. False when neither applies. */
bool server_world_to_screen(FwmServer *server, double wx, double wy, double span,
                            double *sx, double *sy);
/* Place a scene node at a world position. A window on a desktop that nobody is
 * showing is parked far off the layout: no monitor covers that area, so it is
 * not drawn and no visibility flag has to be tracked and put back.
 *
 * `span` is how wide the thing is, and it decides WHICH monitor draws it while
 * no monitor owns its desktop: the one showing most of its body. Pass 0 for a
 * point, which only lands on a screen whose view contains it. */
void server_place_node(FwmServer *server, struct wlr_scene_node *node,
                       double wx, double wy, double span);
/* The same for a window, which is every caller that has one: it knows its own
 * width, and it remembers which screen drew it so that a desktop being left
 * does not change screens halfway out. */
void server_place_view(FwmServer *server, struct FwmView *view,
                       double wx, double wy);
/* Move a freshly opened centred panel onto the monitor the user is at. */
void server_panel_to_active_output(FwmServer *server, struct wlr_scene_buffer *panel);
/* The box that monitor covers, which is what a panel must centre itself in —
 * the desktop column is the PRIMARY monitor's size and is the wrong screen
 * everywhere else. Falls back to the column when there is no monitor yet. */
void server_active_output_box(FwmServer *server, struct wlr_box *box);

/* Every window and ghost put back where it belongs. Call after anything that
 * changes which monitor shows which desktop. */
void server_views_place(FwmServer *server);

/* Every window cut to the edge of the screen drawing it. Purely visual and
 * cheap when nothing overhangs, so it runs once per frame, after the placing
 * and before the scene is committed — see the definition for why a compositor
 * with two monitors cannot do without it. */
void server_views_clip(FwmServer *server);

/* Layout coordinates back to world, through the monitor at that point — the
 * inverse of server_world_to_screen. False when the point is off every
 * monitor. The pointer is the caller that matters: a click means "this world
 * position on THIS monitor's desktop". */
bool server_screen_to_world(FwmServer *server, double lx, double ly,
                            double *wx, double *wy);
/* The cursor in world coordinates. Falls back to the active monitor's camera
 * when the pointer is somehow off every screen, so callers always get a
 * usable answer. */
void server_cursor_world(FwmServer *server, double *wx, double *wy);

/* Show desktop `d` on monitor `out`. If another monitor is already showing it
 * the two trade places, so a desktop is never on two screens at once. `seam`
 * marks a step across the ring's join, which is jumped rather than slid. */
void server_output_show_desktop(FwmServer *server, FwmOutput *out, int d, int seam);

void server_apply_tiling(FwmServer *server, int desktop);
/* Which edge of a tile a point is nearest, as a fraction of that tile's own
 * shape: the side a window dropped there — or opened there — is put down on. */
BspSide server_tile_side_at(const BspNode *leaf, double wx, double wy);
/* Put a new window into a desktop's tree. With [tiling] spawn_cursor set that
 * is beside the window under the pointer, on the edge of it the pointer is
 * nearest — the drop rule, for a window that arrived by itself; otherwise, and
 * whenever the pointer is over no tile of this desktop, beside the focused
 * window as bsp_insert splits it. Does not lay the desktop out: call
 * server_apply_tiling after. */
void server_tile_insert(FwmServer *server, int desktop, uint32_t id);
/* Re-run tile positioning against the sizes clients actually committed. Called
 * when a tiled window commits a size different from the one it was asked for. */
void server_align_tiles(FwmServer *server, int desktop);
void server_tile_ratio_limits(FwmServer *server, int desktop, BspNode *node,
                              float *lo, float *hi);
/* The resize rubber, for the windows a divider drag is about to resize: every
 * leaf under `a` or `b`, which are the two splits the hand has hold of (either
 * may be NULL). A tiled window has the same problem a floating one does — it
 * is asked for a size and answers in its own units, a character cell at a time
 * — and this is the same answer: the window is drawn at the size the LAYOUT
 * says, from the picture it already had, and the client catches up behind it.
 * Only the windows the drag actually resizes, so a video in a tile the divider
 * does not touch is not frozen for the length of it. */
void server_tile_rubber_begin(FwmServer *server, BspNode *a, BspNode *b);
/* ...and the hand off it: every picture still up is held until its client has
 * answered the last size it was asked for, then dropped (view_resize_settle).
 * Swept over every view rather than the desktop's leaves, so a window that
 * left the tree mid-drag cannot be left frozen behind a picture of itself. */
void server_tile_rubber_settle(FwmServer *server);
/* Move one desktop to DESKTOP_MODE_*, running the leave/enter work for both
 * the old and the new mode. No-op when the desktop is already in that mode. */
void server_set_desktop_mode(FwmServer *server, int d, int mode);
void server_start_interactive_move(FwmServer *server, struct FwmView *view, uint32_t serial);
void server_start_interactive_resize(FwmServer *server, struct FwmView *view, uint32_t edges, uint32_t serial);
/* real=true: true fullscreen over the whole output (client is told it is
 * fullscreen). real=false: "fake" fullscreen filling the work area below the
 * tray, with the client left in its normal windowed state. Ignored when
 * fullscreen=false. */
void server_set_fullscreen(FwmServer *server, struct FwmView *view, bool fullscreen, bool real);
void server_request_tray_redraw(FwmServer *server);
/* Re-read the config file and re-apply everything that can change at runtime
 * (physics, decor, tiling, keymap, wallpaper, binds). Errors are reported
 * through the tray pill, never by failing. */
void server_reload_config(FwmServer *server);
/* Push the in-memory config onto the live compositor without touching the
 * file. Used by server_reload_config (rebuild_wallpaper = 1) and by
 * `fwmctl set`, which must not pay for an image decode per keystroke (0). */
void server_apply_config(FwmServer *server, int rebuild_wallpaper);
/* Point the un-tied palette at the monitor the user is on. Everything that
 * lives on a screen — the tray, a window's frame — is drawn from that screen's
 * own wallpaper (theme_get_output); everything that opens where the hand is —
 * the launcher, the ring, the OSD — is drawn from this one. Called whenever
 * the cursor may have crossed the join; nothing is repainted unless the two
 * monitors' images actually derive different colours. */
void server_palette_sync(FwmServer *server);
/* Copy the config's physics onto the live world: the scalars, and the
 * per-desktop profiles built out of them. Split out because startup and reload
 * both need exactly this and nothing else. */
void server_apply_physics_config(FwmServer *server);
/* Run a keybind action. The one entry point, so an action never behaves
 * differently depending on what triggered it — a key, a petal of the radial
 * menu, or a gesture. The _external variant is the socket's door onto the same
 * function, and only exists to log that the socket was the hand (src/ipc.c). */
void server_dispatch_action(FwmServer *server, const char *action);
void server_dispatch_action_external(FwmServer *server, const char *action);
/* How many steps this detent of the knob is worth, 1 or more: a spun knob
 * carries further than a clicked one. `dir` is -1 or +1 and only decides
 * whether the turn is a continuation of the last one — the caller multiplies
 * its own step by the answer. Every menu the knob drives asks this, so the
 * feel is one thing rather than three; capped by [input] knob_accel. */
int server_knob_step(FwmServer *server, int dir);
/* Swap the wallpaper at runtime: rebuilds the layers, recomputes the palette
 * when [decor] color_source = "wallpaper", and remembers the choice in the
 * state file so it survives a restart. Replaces the FIRST [[wallpaper]] layer;
 * further parallax layers keep their images. */
void server_set_wallpaper(FwmServer *server, const char *path);

/* Run `cmd` detached, through a shell, and return the pid of the process that
 * will actually run it (-1 if it could not be started). The pid is what
 * launched_note() needs to put the resulting window where it was asked for; a
 * caller with no window to place may ignore it. */
pid_t server_spawn(const char *cmd);

/* What the `terminal` bind runs: $TERMINAL, or the first emulator on PATH out
 * of a list of the usual ones. NULL when there is none, having said so once
 * through the tray's error pill. */
const char *server_terminal_command(FwmServer *server);

#endif /* FWM_SERVER_H */
