#ifndef FWM_VIEW_H
#define FWM_VIEW_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <stdbool.h>

struct FwmServer;

typedef struct FwmView {
    uint32_t id; /* Unique ID matching the ID in physics */
    struct wlr_xdg_toplevel *xdg_toplevel;
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

    /* Map fade-in: opacity ramps 0 -> 1 over decor.fade_in_ms. */
    int fade_anim;
    double fade_t;

    /* Last committed buffer, kept locked so view_unmap can leave a fading
     * close-animation snapshot (FwmGhost) after the client buffer is gone. */
    struct wlr_buffer *last_buffer;

    /* Real (whole-output) fullscreen: while such a view is on the active
     * desktop the tray hides — overlays outrank windows in the scene, so
     * this is the only way a fullscreen surface can cover everything. */
    int fs_real;
    
    struct FwmServer *server;
    struct wl_list link;
} FwmView;

FwmView *view_create(struct wlr_xdg_toplevel *toplevel, struct FwmServer *server);
void view_destroy(FwmView *view);
void view_map(FwmView *view);
void view_unmap(FwmView *view);

/* Border helpers (no-ops when borders are disabled or the view is unmapped). */
void view_update_border_geometry(FwmView *view);
void view_set_border_color(FwmView *view, const float color[4]);
void view_set_border_enabled(FwmView *view, int enabled);

/* Set opacity (0..1) on every surface buffer of the view (fade-in). */
void view_set_opacity(FwmView *view, double opacity);

#endif /* FWM_VIEW_H */
