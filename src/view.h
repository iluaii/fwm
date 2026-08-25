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

#include "droplet.h"
#include "wobble.h"
#include "sun.h"

struct FwmServer;
struct FwmGroup;

typedef enum {
    FWM_VIEW_XDG,      /* native Wayland xdg-shell toplevel */
    FWM_VIEW_XWAYLAND, /* X11 window under Xwayland */
} FwmViewType;

/* What a window's tile_anim is doing (see FwmView below). */
typedef enum {
    TILE_ANIM_NONE = 0,
    TILE_ANIM_GLIDE,   /* eased toward a slot on its own desktop */
    TILE_ANIM_FLIGHT,  /* sent across the strip to another desktop */
} FwmTileAnim;

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
    struct wl_listener xwl_set_override_redirect;
    
    /* Saved geometry (local coordinates in desktop) */
    int x, y;
    int width, height;

    /* Tile-glide animation: when tile_anim is set, the physics tick eases the
     * window toward (tile_tx, tile_ty) instead of snapping it there.
     *
     * GLIDE is a move within a desktop — a re-tile, a window pulled back onto
     * a screen — and chases the target exponentially, which is right for a
     * short hop of unpredictable length.
     *
     * FLIGHT is a window SENT to another desktop, which is a screen or more of
     * travel and needs the other curve: an exponential chase over that
     * distance spends nearly all of it in the first two frames, so a window
     * sent six desktops away left the screen within a frame and was a teleport
     * again. A flight is a timed cubic ease-in-out from (tile_fx, tile_fy) over
     * [camera] anim_ms — the camera's own curve and the camera's own duration,
     * so a window that travels WITH the camera (move_to_view:) sits still on
     * the screen while the world slides under it, and one sent without you
     * leaves at the speed a desktop switch moves at. `tile_t` is how far
     * through it is, 0..1. */
    int tile_anim;
    double tile_tx, tile_ty;
    double tile_fx, tile_fy;
    double tile_t;

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

    /* The shadow this window casts from wherever the sun is, as nodes at the
     * bottom of the same tree and for the same reason. NULL when [sun] is off
     * at map time, or when the nodes could not be created. */
    struct FwmShadow *shadow;

    /* This window is hanging off the edge of the screen it is drawn on, and the
     * part that is over the border has been cut away (server_views_clip). `cut`
     * is whether a crop is currently applied and `cut_box` the one that is, in
     * the surface's own coordinates, so a window travelling across the screen
     * only pays for a new crop when the crop actually changes. */
    int cut;
    struct wlr_box cut_box;

    /* The screen that drew this window last frame.
     *
     * Only consulted while no monitor owns its desktop — the few hundred ms
     * after a switch, when the desktop is being left behind. Two monitors
     * panning over the same strip can both have part of that column in view,
     * and picking whichever of them shows more of the window flips mid-flight:
     * the window vanishes off one screen and appears near the edge of the
     * other, a whole screen away. It leaves by the screen it was on. */
    struct FwmOutput *drawn_on;

    /* Unfocused windows fall back a step ([decor] inactive_opacity). `dim` is
     * what is on screen and `dim_target` what focus says it should be; the
     * tick walks one to the other so that clicking between windows is a fade
     * rather than a flick.
     *
     * Zero-initialised to 0, which is not a valid opacity — view_map sets both
     * to the right value once the window knows whether it has the focus.
     *
     * `dim_from` and `dim_t` are the fade itself: where it started and how far
     * through [decor] dim_ms it is, 0..1. A timed ease rather than a chase
     * toward the target, because a chase is front-loaded — it covered most of
     * the distance in the first frame or two and then crawled, which is a flick
     * followed by a tail and not a fade. */
    double dim, dim_target;
    double dim_from, dim_t;

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
    /* The light this window's shadow was last drawn for, and the size it was
     * drawn at. A star's light depends on WHERE the window is, so unlike the
     * sun there is no one answer to compare against for the whole screen —
     * without this the tick moved nine scene nodes per window per frame, and
     * damaged the screen under every shadow, for a light that had not
     * perceptibly changed. */
    FwmSunLight shadow_light;
    int shadow_w, shadow_h;
    bool shadow_drawn;

    /* Inside the star's reach right now. A window feeds the star when it
     * ARRIVES, once per pass — without this a window resting near it would
     * hand over mass every frame for as long as it sat there. */
    bool star_near;
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

    /* Rubber resize: the window drawn at the size the HAND is asking for while
     * the client is still catching up with it.
     *
     * A resize is a conversation — we ask, the client redraws, and only then is
     * there a picture of the new size. So the window moves in the client's
     * steps, not the hand's: a terminal jumps a whole character cell at a time
     * and everything lags a frame or three behind the cursor. Stretching the
     * picture we already have to the box being asked for hides all of it; the
     * client's real content arrives underneath and takes over at the release.
     * It is the same trade every compositor that feels smooth here makes — the
     * content is slightly scaled for as long as the drag lasts.
     *
     * The picture is taken once, at the grab, and held for the whole drag. The
     * client's newer frames are deliberately not swapped in: each answer it
     * sends is a different size, so a picture kept up to date was being
     * squeezed a few percent one way and then the other several times a second
     * on top of the stretch, and the window shuddered. `rub_live` marks the
     * cheap way of taking it — a single-surface window is stretched straight
     * from the client's own buffer and nothing is copied at all; anything with
     * a subsurface or an open menu is composited into `rub_lock` instead. */
    struct wlr_scene_buffer *rub_buf;
    struct wlr_buffer *rub_lock;      /* the composited picture, when copied */
    int rub_live;                     /* the client's own buffer, no copy */
    int rub_w, rub_h;                 /* size the picture holds */
    double rub_frame_t;               /* since the last frame callback, s */
    /* The moment after the hand comes off. The client is still answering the
     * last size it was asked for, and every answer changes the window — so the
     * edges that were NOT being dragged are held where the grab left them
     * until it stops, and the stretched picture stays up until the client's
     * next frame rather than snapping to a size it has already left behind.
     * `rs_x1`/`rs_y1` are where the held right and bottom edges stand. */
    int rub_settling;
    int rs_pin_r, rs_pin_b;
    int rs_x1, rs_y1;
    double rs_t;                      /* grace left, s; 0 = nothing pending */

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

    /* The drop (droplet.h): a window carried off a tiling layout.
     *
     * It is not a third thing that owns the picture — it is a shape the wobble
     * above is drawn in, which is why it lives among the jelly fields and not
     * beside them. `drop_round` is eased toward `drop_want` so a window rounds
     * off into a drop over a few frames instead of popping into one, and both
     * are 0 for every window that is not being carried out of a tree.
     *
     * `drop_fill` replaces the springs entirely for the length of the landing:
     * once it is armed the lattice comes from droplet_fill_points and the
     * wobble is not stepped at all. Two shapes, one at a time. */
    double drop_round;                /* how round the sheet is drawn, 0..1 */
    double drop_want;                 /* what it is easing toward */
    int drop_filling;                 /* the landing owns the lattice */
    DropletFill drop_fill;

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
/* Take on an X11 surface that already has its wlr_surface, and may already be
 * mapped — the state a window is in when it stops being override-redirect
 * mid-life and has to change hands. A freshly created view gets there through
 * the associate and map events instead; this is the same arrival, said late. */
void view_xwl_adopt(FwmView *view);
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

/* ── rubber resize (see rub_* above) ──────────────────────────────────────
 *
 * begin: put the stretched picture up and hide the live window behind it.
 * to:    draw it at this size — the box the hand is asking the client for.
 * tick:  keep the picture (and the client) alive for another frame.
 * end:   live content back, whatever size the client actually settled on.
 *
 * Begin is false when there is nothing to stretch (no picture yet) or another
 * effect already owns the window's picture; the resize then simply behaves as
 * it always did. */
bool view_rubber_begin(FwmView *view);
void view_rubber_to(FwmView *view, int w, int h);
void view_rubber_tick(FwmView *view, double dt);
void view_rubber_end(FwmView *view);
/* The hand has come off a resize. `pin_r`/`pin_b` say which far edges were
 * standing still — the ones opposite the corner that was dragged — and x1/y1
 * where they stand, in world pixels. */
void view_resize_settle(FwmView *view, int pin_r, int pin_b, int x1, int y1);
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

/* The window's world coordinates just changed by (dx, dy) for a reason that is
 * NOT the hand moving it: the camera slid a whole desktop under a drag, or the
 * window was sent to another desktop, both of which move it a screen's width in
 * world space while it sits still on screen.
 *
 * The sheet is driven by how far the window travelled, so without this those
 * teleports arrive as a 1920px shove in one tick and the wobble snaps to its
 * limit and rings. Re-base instead: the window is where it now is, and it got
 * there without moving. A no-op when nothing is wobbling. */
void view_jelly_carry(FwmView *view, double dx, double dy);

/* The drop (see the drop_* fields above and droplet.h).
 *
 * view_droplet_begin says this window has just been taken out of a tiling
 * layout and should round off into one; it arms the mesh itself if the wobble
 * has not already, so the drop does not need [effects] jelly to be on.
 * view_droplet_fill says it has been put back into one, `lx`/`ly` being where
 * the cursor let go in the SLOT's frame — the fill spreads out from there and
 * hands the live window back when the slot is full. view_droplet_clear drops
 * the shape without ending the wobble, for a window that left the tree and is
 * not going back into one.
 *
 * All three are no-ops at [effects] droplet 0, which leaves the pickup resize
 * and nothing else — the window simply becomes small in your hand. */
void view_droplet_begin(FwmView *view, double grab_lx, double grab_ly);
void view_droplet_fill(FwmView *view, double lx, double ly, double drop_w, double drop_h);
void view_droplet_clear(FwmView *view);
/* Is this window a drop right now? Asked at the end of a drag, where it is the
 * difference between a window that came out of a layout and one that was
 * carried in from a physics desktop and never was one. */
bool view_is_droplet(FwmView *view);

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

/* ── the unfocused dim ────────────────────────────────────────────────────
 *
 * view_dim_set says where the dim should end up — focus moved, or the config
 * was reloaded — and view_dim_tick walks it there, returning true while it is
 * still on the way so the compositor knows to keep the frames coming. The
 * apply is separate because a client that adds a subsurface hands us a scene
 * buffer at full strength that has to be brought down to the rest.
 *
 * suspend/restore put the window back to full strength for the length of a
 * snapshot: what a picture of the window is taken FOR (the spin, expo's cards)
 * is the window, not the state of the keyboard focus while it was taken. */
void view_dim_set(FwmView *view, double target, bool immediate);
bool view_dim_tick(FwmView *view, double dt);
void view_dim_apply(FwmView *view);
void view_dim_suspend(FwmView *view);
void view_dim_restore(FwmView *view);

/* Put the shadow where the light says, at the window's current size. Called on
 * every geometry change and whenever the sun has moved far enough to matter. */
void view_shadow_update(FwmView *view);

#endif /* FWM_VIEW_H */
