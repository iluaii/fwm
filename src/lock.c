#include <stdlib.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "lock.h"
#include "server.h"
#include "view.h"

/* One lock surface, i.e. one output's worth of lock screen. */
typedef struct FwmLockSurface {
    struct FwmServer *server;
    struct wlr_session_lock_surface_v1 *surface;
    struct wlr_scene_tree *tree;
    struct wl_listener map;
    struct wl_listener destroy;
    struct wl_list link; /* FwmServer.lock_surfaces */
} FwmLockSurface;

/* Everything the user could otherwise see or click. The lock tree itself stays
 * enabled — it is the only thing that may be visible while locked. */
static void set_normal_content_enabled(FwmServer *server, bool enabled) {
    struct wlr_scene_tree *trees[] = {
        server->layer_background, server->ls_background, server->ls_bottom,
        server->layer_windows, server->ls_top, server->layer_overlay,
        server->ls_overlay,
    };
    for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
        if (trees[i]) wlr_scene_node_set_enabled(&trees[i]->node, enabled);
    }
}

bool lock_is_active(FwmServer *server) {
    return server->locked;
}

/* Hand the keyboard to a lock surface. Done on every map and on unlock-failure
 * so the password field always has focus. */
static void lock_focus_surface(FwmServer *server, struct wlr_surface *surface) {
    struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
    if (kbd) {
        wlr_seat_keyboard_notify_enter(server->seat, surface,
            kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
    }
}

static void lock_surface_handle_map(struct wl_listener *listener, void *data) {
    FwmLockSurface *ls = wl_container_of(listener, ls, map);
    (void)data;
    lock_focus_surface(ls->server, ls->surface->surface);
}

static void lock_surface_handle_destroy(struct wl_listener *listener, void *data) {
    FwmLockSurface *ls = wl_container_of(listener, ls, destroy);
    (void)data;
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->link);
    free(ls);
}

static void handle_new_lock_surface(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_lock_surface);
    struct wlr_session_lock_surface_v1 *surface = data;

    FwmLockSurface *ls = calloc(1, sizeof(*ls));
    if (!ls) return;
    ls->server = server;
    ls->surface = surface;

    ls->tree = wlr_scene_subsurface_tree_create(server->layer_lock, surface->surface);
    if (!ls->tree) { free(ls); return; }
    wlr_scene_node_set_position(&ls->tree->node, 0, 0);

    /* The lock surface must cover its whole output, and the client cannot pick
     * its own size — we configure it. */
    struct wlr_output *output = surface->output;
    if (output) {
        wlr_session_lock_surface_v1_configure(surface, output->width, output->height);
    }

    ls->map.notify = lock_surface_handle_map;
    wl_signal_add(&surface->surface->events.map, &ls->map);
    ls->destroy.notify = lock_surface_handle_destroy;
    wl_signal_add(&surface->events.destroy, &ls->destroy);
    wl_list_insert(&server->lock_surfaces, &ls->link);
}

static void handle_lock_unlock(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, lock_unlock);
    (void)data;

    server->locked = 0;
    server->lock = NULL;
    set_normal_content_enabled(server, true);
    wlr_scene_node_set_enabled(&server->layer_lock->node, false);

    /* Give the keyboard back to whatever was focused before the lock. */
    if (server->focused_view) {
        struct wlr_surface *surface = view_surface(server->focused_view);
        if (surface) lock_focus_surface(server, surface);
    } else {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
    wlr_log(WLR_INFO, "session unlocked");
}

static void handle_lock_destroy(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, lock_destroy);
    (void)data;

    wl_list_remove(&server->lock_unlock.link);
    wl_list_remove(&server->new_lock_surface.link);
    wl_list_remove(&server->lock_destroy.link);
    server->lock = NULL;

    /* NOT an unlock. If the lock client crashed while the session was locked,
     * `locked` is still set and we deliberately stay locked with everything
     * hidden — a blank screen the user cannot get past. Treating a dead locker
     * as an unlock would make crashing swaylock a way in. */
    if (server->locked) {
        wlr_log(WLR_ERROR, "lock client died while locked — session stays locked "
                           "(blank screen); start another locker to unlock");
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
}

static void handle_new_lock(struct wl_listener *listener, void *data) {
    FwmServer *server = wl_container_of(listener, server, new_lock);
    struct wlr_session_lock_v1 *lock = data;

    /* One locker at a time. A second client asking to lock an already locked
     * session is refused rather than allowed to take over the screen. */
    if (server->lock) {
        wlr_log(WLR_ERROR, "refusing a second session lock");
        wlr_session_lock_v1_destroy(lock);
        return;
    }

    server->lock = lock;
    server->locked = 1;

    set_normal_content_enabled(server, false);
    wlr_scene_node_set_enabled(&server->layer_lock->node, true);

    /* Drop pointer focus immediately: nothing underneath may keep receiving
     * events, and the cursor must not still be hovering a client's button. */
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_seat_keyboard_notify_clear_focus(server->seat);

    server->new_lock_surface.notify = handle_new_lock_surface;
    wl_signal_add(&lock->events.new_surface, &server->new_lock_surface);
    server->lock_unlock.notify = handle_lock_unlock;
    wl_signal_add(&lock->events.unlock, &server->lock_unlock);
    server->lock_destroy.notify = handle_lock_destroy;
    wl_signal_add(&lock->events.destroy, &server->lock_destroy);

    wlr_session_lock_v1_send_locked(lock);
    wlr_log(WLR_INFO, "session locked");
}

void lock_init(FwmServer *server) {
    wl_list_init(&server->lock_surfaces);
    server->locked = 0;
    server->lock = NULL;

    server->lock_manager = wlr_session_lock_manager_v1_create(server->wl_display);
    if (!server->lock_manager) {
        wlr_log(WLR_ERROR, "failed to create session lock manager");
        return;
    }
    server->new_lock.notify = handle_new_lock;
    wl_signal_add(&server->lock_manager->events.new_lock, &server->new_lock);
}
