#ifndef FWM_VIEW_H
#define FWM_VIEW_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland.h>
#include <stdbool.h>

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

    /* wlr-foreign-toplevel handle: this window as external panels see it
     * (see foreign.h). NULL while unmapped. */
    struct wlr_foreign_toplevel_handle_v1 *ftl;
    struct wl_listener ftl_request_activate;
    struct wl_listener ftl_request_close;
    struct wl_listener ftl_request_fullscreen;

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
void view_set_size(FwmView *view, int width, int height);
void view_send_close(FwmView *view);
void view_set_activated(FwmView *view, bool activated);
/* Tell the client it is (or is no longer) fullscreen. */
void view_set_fullscreen_hint(FwmView *view, bool fullscreen);
/* Push the current compositor-side position to the client. X11 clients place
 * their popups from it; no-op for xdg views (Wayland has no global coords). */
void view_sync_position(FwmView *view);

/* Border helpers (no-ops when borders are disabled or the view is unmapped). */
void view_update_border_geometry(FwmView *view);

/* Impact squash & stretch (see the squash_* fields above).
 * `nx`,`ny` is the contact normal pointing from the window toward whatever it
 * hit; `amount` is the peak deformation, 0..1. Starting one while another runs
 * restarts it. Safe to call when the view has no snapshot to deform — it is
 * simply ignored. */
void view_start_squash(FwmView *view, double nx, double ny, double amount);
void view_squash_tick(FwmView *view, double dt);
void view_stop_squash(FwmView *view);
void view_set_border_color(FwmView *view, const float color[4]);
void view_set_border_enabled(FwmView *view, int enabled);

/* Set opacity (0..1) on every surface buffer of the view (fade-in). */

#endif /* FWM_VIEW_H */
