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

#ifndef FWM_VIEW_H
#define FWM_VIEW_H

#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland.h>
#include <stdbool.h>

#include "wobble.h"

struct FwmServer;
struct FwmGroup;

typedef enum {
    FWM_VIEW_XDG,      /* native Wayland xdg-shell toplevel */
    FWM_VIEW_XWAYLAND, /* X11 window under Xwayland */
} FwmViewType;

typedef struct FwmView {
    uint32_t id; /* Unique ID matching the ID in physics */
    FwmViewType type;
    struct wlr_xdg_toplevel *xdg_toplevel;       /* NULL for X11 views */
    struct wlr_xwayland_surface *xwl_surface;    /* NULL for xdg views */
    struct wlr_scene_tree *scene_tree;
    
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener set_title;
    /* X11-only listeners */
    struct wl_listener xwl_associate;
    struct wl_listener xwl_dissociate;
    struct wl_listener xwl_request_configure;
    
    /* Saved geometry (local coordinates in desktop) */
    int x, y;
    int width, height;

    /* Tile-glide animation: when tile_anim is set, the physics tick eases the
     * window toward (tile_tx, tile_ty) instead of snapping it there. */
    int tile_anim;
    double tile_tx, tile_ty;

    /* Size this view was last aligned against. The layout re-runs when a
     * client commits a different one; without this it would re-run on every
     * commit, cursor blink included. */
    int aligned_w, aligned_h;

    /* The smallest this window has been seen to accept, learned from the sizes
     * it commits against the sizes it is offered (see tile_actuals). The tiling
     * layout keeps this much room for it; 0 until it refuses something.
     *
     * The `over` pair is the oversized commit currently being timed, and when
     * it was first seen: a client is always a frame or two behind the size it
     * was asked for, so being too big has to LAST before it means anything. */
    int tile_floor_w, tile_floor_h;
    int tile_over_w, tile_over_h;
    double tile_over_tw, tile_over_th;

    /* Last size an X client asked for and was refused (fullscreen owns its
     * geometry). Refusing means answering with the geometry it already has, and
     * a client that asks again for the same thing must be met with silence
     * instead: it has been told once, and answering every repeat turns two
     * stubborn parties into a configure loop as fast as the socket allows. */
    int cfg_denied_w, cfg_denied_h;

    /* Focus border: 4 rects (top, bottom, left, right) parented to scene_tree,
     * so they move with the window for free. NULL when borders are disabled. */
    struct wlr_scene_rect *border[4];

    /* Open animation.
     *
     * The client's surface is NEVER blended at partial opacity: ramping it was
     * tried and kept producing a visible flash at the start, because a client's
     * first frames (blank, white, half-drawn) get composited mid-ramp. Instead:
     *
     *   1. the whole scene tree is DISABLED at map, so nothing shows at all;
     *   2. `open_hold` counts commits until the client has painted real
     *      content (commit #1 is the mapping commit itself);
     *   3. the tree is then enabled fully opaque, and a solid cover rect that
     *      we draw ourselves fades out over it while the window rises into
     *      place. Everything that blends is ours, so a client frame can never
     *      appear half-transparent. */
    int open_anim;
    double open_t;
    int open_hold;
    double open_hold_ms;
    struct wlr_scene_rect *open_cover;

    /* Last committed buffer, kept locked so view_unmap can leave a fading
     * close-animation snapshot (FwmGhost) after the client buffer is gone. */
    struct wlr_buffer *last_buffer;

    /* Destroyed by a collision ([physics] hp), close already sent. Two things
     * hang off it: a window crushed between two others must not be sent a
     * second close in the same frame, and the ghost it leaves behind collapses
     * to a point instead of fading, so being broken does not look like being
     * closed.
     *
     * This is a polite close, not a kill, so the client may decline it and put
     * up a "save your work?" dialog instead. `dying_at` is when the request
     * went out: past the grace period the window can be destroyed again, or a
     * client that says no once would be immortal for the rest of the session.
     * Keep being hit and you keep being asked to close, which is the right
     * answer for a compositor where being hit is the entire idea.
     *
     * A client that never handles the request at all is therefore INDESTRUCTIBLE,
     * and that is the accepted trade. Escalating to wl_client_destroy would take
     * down every other window the application owns, and no collision is worth
     * that. It is also not a special case: the same request is what `killclient`
     * sends, so a window that survives this is one the user could not have
     * closed by hand either. */
    int dying;
    double dying_at;

    /* Set on every client commit, cleared when an effect re-photographs the
     * window. The spin and the wobble redraw off THIS rather than off a timer:
     * new content is exactly when a new picture is worth taking, and a window
     * that is not drawing costs them nothing. */
    int content_dirty;

    /* Impact squash & stretch. Deforms a SNAPSHOT of the last committed frame,
     * never the live surface: wlroots' scene resets a surface buffer's
     * dest_size on every client commit, so a live deformation would be wiped
     * out the moment the client redraws. The real content is hidden for the
     * ~250ms this runs; the window is effectively a still frame, which is
     * imperceptible at impact speed and is the same trade the close ghost
     * already makes. */
    struct wlr_scene_buffer *squash_buf;
    struct wlr_buffer *squash_lock;   /* our own lock on the snapshot */
    double squash_t;                  /* seconds since the impact */
    double squash_amount;             /* peak deformation, 0..1 */
    double squash_nx, squash_ny;      /* impact normal, points at the contact */

    /* Wobble ("jelly") while a window is dragged: KDE's effect, a sheet of
     * springs that bends rather than a rectangle that is scaled. The model is
     * in wobble.h and knows nothing about any of this; here is only what it
     * takes to draw it.
     *
     * Which is the same machinery the spin below uses, for the same reason —
     * the scene graph cannot express a bent window any more than a tilted one,
     * so the subtree is flattened into `jelly_src` and warp_blit draws it
     * through the lattice into one of the `jelly_dst` pair. The destination is
     * bigger than the window by `jelly_margin` on every side, because a
     * wobbling sheet overshoots its own box.
     *
     * The wobble, the impact squash and the spin all replace the window with a
     * picture of it, so at most one of the three can be running. */
    int jelly;                        /* the wobble owns the picture */
    int jelly_settling;               /* let go: come to rest, then give the
                                       * live window back */
    Wobble jelly_wob;
    /* As the spin's: when the window is a single surface the sheet is warped
     * straight from the client's texture, so a dragged window bends around live
     * content instead of a still frame. jelly_src/jelly_tex are then unused. */
    int jelly_live;
    struct wlr_scene_buffer *jelly_buf;
    struct wlr_buffer *jelly_src;     /* flattened window content, w x h */
    struct wlr_texture *jelly_tex;    /* ... imported once, reused every frame */
    struct wlr_buffer *jelly_dst[2];  /* warped output, window + 2*margin */
    int jelly_flip;                   /* which of the two to draw into next */
    int jelly_border;                 /* were the borders shown before it */
    int jelly_w, jelly_h;             /* window size the snapshot was taken at */
    int jelly_margin;                 /* slack around the window, px */
    double jelly_snap_t;              /* since the last COMPOSITED refresh, s */
    double jelly_px, jelly_py;        /* window position at the last tick */

    /* Free rotation (experimental; see PhysicsBody.spin).
     *
     * Same snapshot trick as the squash, for the same reason and one step
     * further: wlr_scene is axis-aligned to its bones — node positions are
     * ints and a scene buffer's only transform is one of the eight
     * wl_output ones — so a genuinely tilted window cannot be expressed in
     * the scene graph at all. What CAN be is an upright buffer whose CONTENT
     * is a rotated picture, which is what this is: the window's subtree is
     * flattened into `spin_src`, that gets drawn rotated into one of the two
     * `spin_dst` buffers, and the scene shows the result.
     *
     * The flattening is only needed for windows that are genuinely several
     * buffers, though. A window that is ONE surface — most of them — is rotated
     * straight from the client's own texture and stays live as it turns (see
     * view_live_texture); spin_src and spin_tex are unused then. Either way
     * damage, compositing and scanout keep working exactly as they always did. */
    struct wlr_scene_buffer *spin_buf;
    struct wlr_buffer *spin_src;      /* flattened window content, w x h */
    struct wlr_texture *spin_tex;     /* ... imported once, reused every frame */
    /* Set when the window is a single surface and the rotation draws straight
     * from the client's own texture (view_live_texture): live content, and
     * spin_src/spin_tex are then unused. spin_seen is the texture last drawn,
     * for spotting that the client has committed a new frame. */
    int spin_live;
    struct wlr_texture *spin_seen;    /* borrowed, never owned */
    struct wlr_buffer *spin_dst[2];   /* rotated output, square, diagonal-sized */
    int spin_flip;                    /* which of the two to draw into next */
    int spin_border;                  /* were the borders shown before the spin */
    int spin_w, spin_h;               /* window size the snapshot was taken at */
    int spin_size;                    /* side of the spin_dst squares */
    double spin_snap_t;               /* since the last COMPOSITED refresh, s */
    double spin_angle;                /* angle currently on screen, radians */

    /* wlr-foreign-toplevel handle: this window as external panels see it
     * (see foreign.h). NULL while unmapped. */
    struct wlr_foreign_toplevel_handle_v1 *ftl;
    struct wl_listener ftl_request_activate;
    struct wl_listener ftl_request_close;
    struct wl_listener ftl_request_fullscreen;
    struct wl_listener ftl_request_maximize;
    /* The monitor the handle currently says this window is on, so enter/leave
     * are only sent when the answer changes. NULL while it is on none. */
    struct wlr_output *ftl_output;

    /* Tab-stack membership; NULL when not grouped (see group.h). */
    struct FwmGroup *group;

    /* Real (whole-output) fullscreen: while such a view is on the active
     * desktop the tray hides — overlays outrank windows in the scene, so
     * this is the only way a fullscreen surface can cover everything. */
    int fs_real;
    
    struct FwmServer *server;
    struct wl_list link;
} FwmView;

FwmView *view_create(struct wlr_xdg_toplevel *toplevel, struct FwmServer *server);
FwmView *view_xwl_create(struct wlr_xwayland_surface *xsurface, struct FwmServer *server);
void view_destroy(FwmView *view);
void view_map(FwmView *view);
void view_unmap(FwmView *view);

/* Shell-agnostic accessors: everything outside view.c must go through these
 * instead of poking xdg_toplevel directly (X11 views have no xdg_toplevel). */
struct wlr_surface *view_surface(FwmView *view);
const char *view_title(FwmView *view);
const char *view_app_id(FwmView *view);
/* The client's process, for anything that has to look the application up in
 * /proc (session restore, mass-from-RAM). 0 when it cannot be determined. */
pid_t view_pid(FwmView *view);
void view_set_size(FwmView *view, int width, int height);
void view_send_close(FwmView *view);
void view_set_activated(FwmView *view, bool activated);
/* Tell the client it is (or is no longer) fullscreen. */
void view_set_fullscreen_hint(FwmView *view, bool fullscreen);
/* Push the current compositor-side position to the client. X11 clients place
 * their popups from it; no-op for xdg views (Wayland has no global coords). */
void view_sync_position(FwmView *view);

/* Border helpers (no-ops when borders are disabled or the view is unmapped). */
/* The size the client actually committed, which is not always the size it was
 * asked for — see server_align_tiles(). Falls back to the requested size
 * before the first commit. */
void view_committed_size(FwmView *view, int *w, int *h);
void view_min_size(FwmView *view, int *w, int *h);

void view_update_border_geometry(FwmView *view);

/* Impact squash & stretch (see the squash_* fields above).
 * `nx`,`ny` is the contact normal pointing from the window toward whatever it
 * hit; `amount` is the peak deformation, 0..1. Starting one while another runs
 * restarts it. Safe to call when the view has no snapshot to deform — it is
 * simply ignored. */
void view_start_squash(FwmView *view, double nx, double ny, double amount);
void view_squash_tick(FwmView *view, double dt);
void view_stop_squash(FwmView *view);

/* Drag wobble (see the jelly_* fields above).
 *
 * view_jelly_begin arms it when a drag starts, told where in the window's own
 * frame it was taken hold of — that point keeps up with the cursor exactly and
 * the rest of the sheet trails behind it. view_jelly_tick runs the lattice and
 * draws it, every frame. view_jelly_release says the hand is off, after which
 * the wobble rings out on its own and hands the live window back;
 * view_jelly_stop ends it immediately.
 *
 * `strength` scales how far from its rest shape the sheet is drawn (config
 * effects.jelly), leaving its timing alone; at 0 begin does nothing. Neither
 * does it on a spinning window, or on a renderer with no GLES2 path — the
 * wobble is a mesh, and there is no lesser version of it worth showing. */
void view_jelly_begin(FwmView *view, double strength, double grab_lx, double grab_ly);
void view_jelly_tick(FwmView *view, double strength, double dt);
void view_jelly_release(FwmView *view);
void view_jelly_stop(FwmView *view);

/* Free rotation (see the spin_* fields above). view_spin_tick shows the window
 * at `angle` radians, creating the snapshot machinery on the first call and
 * refreshing it as it runs; view_stop_spin puts the live window back. Calling
 * the tick every frame while the body spins is the whole contract — there is
 * no separate "start". */
void view_spin_tick(FwmView *view, double angle, double dt);
void view_stop_spin(FwmView *view);
/* Is this window currently showing a rotated snapshot? */
bool view_is_spinning(FwmView *view);
void view_set_border_color(FwmView *view, const float color[4]);
void view_set_border_enabled(FwmView *view, int enabled);

/* Set opacity (0..1) on every surface buffer of the view (fade-in). */

#endif /* FWM_VIEW_H */
