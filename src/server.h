#ifndef FWM_SERVER_H
#define FWM_SERVER_H

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

#define DESKTOP_MODE_PHYSICS 0
#define DESKTOP_MODE_TILING  1

typedef enum {
    FWM_ACTION_NONE,
    FWM_ACTION_MOVE,
    FWM_ACTION_RESIZE,
    FWM_ACTION_SWAP,
    FWM_ACTION_BSP_RESIZE
} FwmInteractiveAction;

struct FwmView;

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
    
    /* BSP resize */
    BspNode *bsp_node;
    float bsp_start_ratio;
    
    /* Swap drag */
    double cur_x, cur_y;
} FwmInteractiveState;

typedef struct FwmServer {
    struct wl_display *wl_display;
    struct wlr_backend *wlr_backend;
    struct wlr_session *session; /* NULL on nested backends; used for VT switching */
    struct wlr_renderer *wlr_renderer;
    struct wlr_allocator *wlr_allocator;
    struct wlr_scene *scene;
    struct wlr_scene_tree *layer_background;
    struct wlr_scene_tree *layer_windows;
    struct wlr_scene_tree *layer_overlay;
    struct FwmWallpaper *wallpaper;
    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_xdg_shell *xdg_shell;
    
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_seat *seat;
    
    struct wl_list views; /* FwmView list */
    struct FwmView *focused_view;
    struct FwmView *last_touched_view;
    
    struct wl_list outputs;
    
    /* Inputs */
    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_cursor;
    struct wl_listener seat_request_set_selection;
    
    /* Keyboard input */
    struct wl_list keyboards;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_toplevel_decoration;

    /* Held-key auto-repeat for repeatable binds (e.g. move_camera) */
    struct wl_event_source *key_repeat_timer;
    const char *repeat_action;   /* points into config.keys[].action; NULL when idle */
    uint32_t repeat_keycode;     /* raw event keycode currently repeating */
    
    /* Physics and desktop coordinates */
    PhysicsWorld physics;
    int camera_x;
    int target_camera_x;
    int screen_width;
    int screen_height;
    
    BspNode *bsp_roots[10];
    int desktop_mode[10];
    
    FwmConfig config;
    FwmInteractiveState interactive;
    
    struct wl_event_source *physics_timer;
    
    /* UI scene nodes */
    struct wlr_scene_buffer *tray_buffer;
    struct wlr_scene_buffer *hints_buffer;
    struct wlr_scene_buffer *welcome_buffer;
    
    int running;
} FwmServer;

bool server_init(FwmServer *server);
void server_run(FwmServer *server);
void server_destroy(FwmServer *server);

void server_focus_view(FwmServer *server, struct FwmView *view);
void server_apply_tiling(FwmServer *server, int desktop);
void server_start_interactive_move(FwmServer *server, struct FwmView *view, uint32_t serial);
void server_start_interactive_resize(FwmServer *server, struct FwmView *view, uint32_t edges, uint32_t serial);
/* real=true: true fullscreen over the whole output (client is told it is
 * fullscreen). real=false: "fake" fullscreen filling the work area below the
 * tray, with the client left in its normal windowed state. Ignored when
 * fullscreen=false. */
void server_set_fullscreen(FwmServer *server, struct FwmView *view, bool fullscreen, bool real);
void server_request_tray_redraw(FwmServer *server);

#endif /* FWM_SERVER_H */
