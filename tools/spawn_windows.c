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

/* Opens N xdg_toplevel windows from a single process, to exercise the paths
 * that only run past MAX_WINDOWS (256).
 *
 * A real terminal per window would need tens of gigabytes at that count; these
 * are 1x1 shm surfaces sharing one connection, so 300 of them cost almost
 * nothing and map about as fast as the compositor will configure them.
 *
 * Build and run against a nested fwm:
 *   cc tools/spawn_windows.c build/xdg-shell-protocol.c -Ibuild \
 *      $(pkg-config --cflags --libs wayland-client) -o build/spawn_windows
 *   WAYLAND_DISPLAY=<inner socket> build/spawn_windows 300
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;

struct win {
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    int configured;
};

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(iface, wl_compositor_interface.name))
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    }
}
static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

/* One shared 1x1 buffer for every window: the contents are irrelevant, only
 * the commit that turns a configured xdg_surface into a mapped window is. */
static struct wl_buffer *make_buffer(void) {
    int stride = 4, size = 4;
    int fd = memfd_create("spawn", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) return NULL;
    void *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return NULL; }
    memset(px, 0xff, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(px, size);
    close(fd);
    return buf;
}

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    struct win *w = data;
    xdg_surface_ack_configure(s, serial);
    if (!w->configured) {
        w->configured = 1;
        wl_surface_attach(w->surface, w->buffer, 0, 0);
        wl_surface_commit(w->surface);
    }
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t w, int32_t h, struct wl_array *states) {
    (void)data; (void)t; (void)w; (void)h; (void)states;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
    (void)data; (void)t;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
};

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 300;
    if (count < 1) count = 1;

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "spawn_windows: cannot connect to WAYLAND_DISPLAY\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "spawn_windows: compositor is missing wl_compositor/wl_shm/xdg_wm_base\n");
        return 1;
    }

    struct wl_buffer *buffer = make_buffer();
    if (!buffer) {
        fprintf(stderr, "spawn_windows: cannot allocate shm buffer\n");
        return 1;
    }

    struct win *wins = calloc(count, sizeof(*wins));
    for (int i = 0; i < count; i++) {
        struct win *w = &wins[i];
        /* argv[2] overrides the title: the tray clamps long titles, and that
         * path needs a title long enough to actually hit the clamp. */
        char title[512];
        if (argc > 2) snprintf(title, sizeof(title), "%s", argv[2]);
        else          snprintf(title, sizeof(title), "spawn %d", i + 1);

        w->buffer = buffer;
        w->surface = wl_compositor_create_surface(compositor);
        w->xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, w->surface);
        xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, w);
        w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
        xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
        xdg_toplevel_set_title(w->toplevel, title);
        xdg_toplevel_set_app_id(w->toplevel, "fwm-spawn-test");
        wl_surface_commit(w->surface);

        /* Round-trip per window: the compositor must finish mapping this one
         * (and retile) before the next arrives, which is what a real burst of
         * window openings looks like. Batching them all would instead test
         * the request queue. */
        if (wl_display_roundtrip(display) < 0) {
            fprintf(stderr, "spawn_windows: connection lost after %d windows\n", i);
            return 2;
        }
        /* stderr, unbuffered: with stdout piped, progress would otherwise sit
         * in the buffer and a hung run would look identical to a silent one. */
        fprintf(stderr, "mapped %d/%d\n", i + 1, count);
    }
    fprintf(stderr, "spawn_windows: all %d windows mapped\n", count);

    /* Exit by default rather than hold the windows open: the map-time paths
     * are what the stress test exercises, and a spawner that never returns
     * just hangs the harness. "hold" is for screenshots, where the windows
     * must still exist when the shot is taken. */
    if (argc > 3 && !strcmp(argv[3], "hold")) {
        fprintf(stderr, "spawn_windows: holding open\n");
        while (wl_display_dispatch(display) != -1) { }
        return 2;
    }
    wl_display_roundtrip(display);
    return 0;
}
