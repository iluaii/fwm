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

/* security-context-v1 and the global filter it feeds; see sandbox.h. */
#include "sandbox.h"
#include "server.h"

#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/util/log.h>

/* The globals a sandboxed client is not shown.
 *
 * Matched by interface name rather than by pointer, so a protocol fwm does not
 * serve today is covered on the day it does, and a protocol served by wlroots
 * needs nothing from us but its name. The virtual-input and output-management
 * entries are exactly that: fwm has no such global yet, and if it ever grows
 * one it must not arrive unguarded.
 *
 * Everything not on this list is a client talking about its own surfaces, and
 * a sandbox has no reason to be denied that. */
static const char *const privileged[] = {
    /* The clipboard without focus: a manager's whole job, and a keylogger's
     * dream. Both spellings, since both are served. */
    "zwlr_data_control_manager_v1",
    "ext_data_control_manager_v1",
    /* Screen capture, old and new. */
    "zwlr_screencopy_manager_v1",
    "zwlr_export_dmabuf_manager_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    /* Panels, bars and lock screens: surfaces that place themselves over
     * everyone else's, and the one that takes the session. */
    "zwlr_layer_shell_v1",
    "ext_session_lock_manager_v1",
    /* The session read back and driven: every window, every desktop, every
     * key pressed anywhere. */
    "zwlr_foreign_toplevel_manager_v1",
    "ext_workspace_manager_v1",
    "hyprland_global_shortcuts_manager_v1",
    /* The screens themselves — backlight, colour, layout. */
    "zwlr_output_power_manager_v1",
    "zwlr_gamma_control_manager_v1",
    "zwlr_output_manager_v1",
    /* Synthetic input, and a seat conjured to carry it. Not served today. */
    "zwp_virtual_keyboard_manager_v1",
    "zwlr_virtual_pointer_manager_v1",
    "ext_transient_seat_manager_v1",
    /* The shell Xwayland uses to tie X windows to Wayland surfaces: wlroots
     * only serves it to the Xwayland it started, but a sandbox has no business
     * even seeing it. */
    "xwayland_shell_v1",
    /* This protocol itself: a sandbox that can declare sandboxes can declare
     * its own escape. */
    "wp_security_context_manager_v1",
};

static bool global_filter(const struct wl_client *client,
                          const struct wl_global *global, void *data) {
    FwmServer *server = data;
    if (!server->security_context) return true;

    /* No context on this client means it came from the session, not from a
     * sandbox: the ordinary case, and the cheap one. */
    if (!wlr_security_context_manager_v1_lookup_client(server->security_context,
                                                       client))
        return true;

    const char *name = wl_global_get_interface(global)->name;
    for (size_t i = 0; i < sizeof(privileged) / sizeof(privileged[0]); i++) {
        if (strcmp(name, privileged[i]) == 0) return false;
    }
    return true;
}

void sandbox_init(FwmServer *server) {
    server->security_context =
        wlr_security_context_manager_v1_create(server->wl_display);
    if (!server->security_context) {
        wlr_log(WLR_ERROR, "security-context-v1 unavailable: "
                           "sandboxed clients will see every global");
        return;
    }
    /* One filter per display, installed before the socket exists, so no client
     * can have been sent a registry under the old rules. */
    wl_display_set_global_filter(server->wl_display, global_filter, server);
}
