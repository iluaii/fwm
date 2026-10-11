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

#include "server_journal.h"
#include "journal.h"
#include "server.h"
#include "view.h"
#include "defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/util/log.h>

int64_t server_journal_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Where a window is, the way the history keeps it: the desktop it belongs to,
 * and its place on that desktop rather than on the whole strip. The desktop is
 * the body's, as `fwmctl windows` reports it — a window flying over the next
 * desktop still belongs to the one it was thrown from until it lands. */
static JournalGeom view_geom(FwmServer *server, FwmView *view) {
    PhysicsBody *b = physics_find_body(&server->physics, view->id);
    int sw = server->screen_width;
    int d = b ? b->desktop_id : (sw > 0 ? view->x / sw : 0);
    if (d < 0) d = 0;
    if (d >= FWM_DESKTOPS) d = FWM_DESKTOPS - 1;

    JournalGeom g = {
        .desktop = d,
        .x = view->x - d * sw, .y = view->y,
        .w = view->width, .h = view->height,
    };
    if (bsp_find(server->bsp_roots[d], view->id)) g.flags |= JOURNAL_F_TILED;
    if (view->fs_real || (b && b->fullscreen)) g.flags |= JOURNAL_F_FULLSCREEN;
    return g;
}

static bool geom_eq(const JournalGeom *a, const JournalGeom *b) {
    return a->desktop == b->desktop && a->x == b->x && a->y == b->y
        && a->w == b->w && a->h == b->h && a->flags == b->flags;
}

/* Far enough from `b` to be worth a point of the way: another desktop, another
 * state, or JOURNAL_PATH_PX in any of the four numbers. */
static bool geom_far(const JournalGeom *a, const JournalGeom *b) {
    if (a->desktop != b->desktop || a->flags != b->flags) return true;
    return abs(a->x - b->x) >= JOURNAL_PATH_PX || abs(a->y - b->y) >= JOURNAL_PATH_PX
        || abs(a->w - b->w) >= JOURNAL_PATH_PX || abs(a->h - b->h) >= JOURNAL_PATH_PX;
}

void server_journal_start(FwmServer *server, const char *socket) {
    /* $XDG_RUNTIME_DIR and nowhere else: it is the user's own and it goes at
     * reboot, which is how long this history is meant to last. Without it the
     * history is kept in memory — /tmp would let anyone on the machine read
     * the titles of everything opened. */
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    const char *p = NULL;
    if (dir && dir[0] && socket && socket[0]) {
        int n = snprintf(path, sizeof(path), "%s/fwm-journal-%s", dir, socket);
        if (n > 0 && (size_t)n < sizeof(path)) p = path;
    }
    server->journal = journal_open(p, server_journal_now());
    if (server->journal)
        wlr_log(WLR_INFO, "journal: %d events so far this boot%s%s",
                journal_count(server->journal), p ? ", kept in " : ", in memory only",
                p ? p : "");
}

void server_journal_finish(FwmServer *server) {
    journal_close(server->journal);
    server->journal = NULL;
}

void server_journal_map(FwmServer *server, FwmView *view) {
    if (!server->journal || view->journal_wid) return;
    int64_t now = server_journal_now();
    JournalGeom g = view_geom(server, view);
    view->journal_wid = journal_window_open(server->journal, now, view_app_id(view),
                                            view_title(view), &g);
    view->journal_seen = view->journal_kept = g;
    view->journal_changed_ms = view->journal_kept_ms = now;
    view->journal_moving = 0;
}

void server_journal_unmap(FwmServer *server, FwmView *view) {
    if (!server->journal || !view->journal_wid) return;
    journal_window_close(server->journal, server_journal_now(), view->journal_wid);
    view->journal_wid = 0;
}

void server_journal_title(FwmServer *server, FwmView *view) {
    if (!server->journal || !view->journal_wid) return;
    journal_window_title(server->journal, server_journal_now(), view->journal_wid,
                         view_title(view));
}

void server_journal_focus(FwmServer *server, FwmView *view) {
    if (!server->journal || !view || !view->journal_wid) return;
    journal_record(server->journal, server_journal_now(), view->journal_wid,
                   JOURNAL_FOCUS, NULL);
}

void server_journal_throw(FwmServer *server, FwmView *view) {
    if (!server->journal || !view || !view->journal_wid) return;
    JournalGeom g = view_geom(server, view);
    journal_record(server->journal, server_journal_now(), view->journal_wid,
                   JOURNAL_THROW, &g);
}

void server_journal_look(FwmServer *server) {
    if (!server->journal) return;
    int64_t now = server_journal_now();
    /* A clock set back would otherwise stop the looking until it caught up
     * with where it had been. */
    if (now >= server->journal_looked_ms
        && now - server->journal_looked_ms < JOURNAL_LOOK_MS) return;
    server->journal_looked_ms = now;

    FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->journal_wid) continue;
        JournalGeom g = view_geom(server, view);

        if (!geom_eq(&g, &view->journal_seen)) {
            view->journal_seen = g;
            view->journal_changed_ms = now;
            if (!view->journal_moving) view->journal_moving = 1;
        }
        if (!view->journal_moving) continue;

        if (now - view->journal_changed_ms >= JOURNAL_REST_MS) {
            /* Stopped. Where it stopped is worth a line of its own even when
             * it is where the last point already was: it says the way ended
             * there. A move that came back to exactly where it began, with no
             * point written on the way, is no move at all. */
            if (view->journal_moving == 2 || !geom_eq(&g, &view->journal_kept)) {
                journal_record(server->journal, now, view->journal_wid,
                               JOURNAL_REST, &g);
                view->journal_kept = g;
                view->journal_kept_ms = now;
            }
            view->journal_moving = 0;
        } else if (now - view->journal_kept_ms >= JOURNAL_PATH_MS
                   && geom_far(&g, &view->journal_kept)) {
            journal_record(server->journal, now, view->journal_wid,
                           JOURNAL_MOVE, &g);
            view->journal_kept = g;
            view->journal_kept_ms = now;
            view->journal_moving = 2;
        }
    }
}
