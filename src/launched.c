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

/* Where a window's application was started from. See launched.h. */

#include "launched.h"
#include "server.h"
#include "view.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LAUNCHED_MAX  32   /* launches remembered at once */
#define LAUNCHED_HOPS 32   /* how far up the process tree to look */

struct LaunchEntry {
    pid_t pid;
    unsigned long long start;   /* its start time, so a reused pid is not it */
    int desktop;
    double at;      /* CLOCK_MONOTONIC seconds */
    char cmd[LAUNCHED_CMD_MAX];   /* "" when not known */
};

struct FwmLaunched {
    struct LaunchEntry e[LAUNCHED_MAX];
    int count;
};

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The parent of `pid` and when it started (in clock ticks since boot), from
 * /proc/<pid>/stat. False when there is no such process.
 *
 * The file holds the command name in parentheses, and a command may contain
 * spaces and parentheses of its own — so the fields are counted from the LAST
 * ')' rather than split from the front, which is the one way to read this file
 * that a process called ")  (" cannot break. */
static bool proc_stat(pid_t pid, pid_t *ppid, unsigned long long *start) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    char *close_paren = strrchr(buf, ')');
    if (!close_paren) return false;
    int pp = 0;
    unsigned long long st = 0;
    /* state, ppid, then fifteen fields to starttime (field 22). */
    if (sscanf(close_paren + 1,
               " %*c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
               &pp, &st) != 2) return false;
    if (ppid) *ppid = pp > 0 ? (pid_t)pp : 0;
    if (start) *start = st;
    return true;
}

static pid_t parent_pid(pid_t pid) {
    pid_t ppid = 0;
    return proc_stat(pid, &ppid, NULL) ? ppid : 0;
}

/* The process a launch started is still the one running under its pid. */
static bool launch_alive(const struct LaunchEntry *e) {
    unsigned long long start = 0;
    return proc_stat(e->pid, NULL, &start) && start == e->start;
}

static bool launch_young(const struct LaunchEntry *e, double now) {
    return now - e->at <= LAUNCHED_TTL;
}

/* Forget what is no use any more, closing the gaps. Called on both ends so the
 * table cannot fill with launches that never produced a window. Past the TTL a
 * launch no longer says where a window goes, but one with a command still says
 * how to start its application again, for as long as that application runs:
 * Steam can take minutes to show its window, and closes it to the tray and
 * opens a new one hours later. */
static void expire(struct FwmLaunched *t, double now) {
    int keep = 0;
    for (int i = 0; i < t->count; i++) {
        const struct LaunchEntry *e = &t->e[i];
        if (launch_young(e, now) || (e->cmd[0] && launch_alive(e)))
            t->e[keep++] = *e;
    }
    t->count = keep;
}

void launched_note(struct FwmServer *server, pid_t pid, int desktop, const char *cmd) {
    if (!server || pid <= 0) return;
    if (desktop < 0 || desktop >= FWM_DESKTOPS) return;

    struct FwmLaunched *t = server->launched;
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t) return;
        server->launched = t;
    }

    double now = now_sec();
    expire(t, now);
    /* Full: drop the oldest, which is the one least likely to still be waiting
     * for a window — and of those, first one only kept for its command, since
     * a young one still has a window to place. */
    if (t->count == LAUNCHED_MAX) {
        int drop = 0;
        for (int i = 0; i < t->count; i++)
            if (!launch_young(&t->e[i], now)) { drop = i; break; }
        memmove(&t->e[drop], &t->e[drop + 1],
                (size_t)(LAUNCHED_MAX - 1 - drop) * sizeof(t->e[0]));
        t->count--;
    }
    t->e[t->count].pid = pid;
    t->e[t->count].start = 0;
    proc_stat(pid, NULL, &t->e[t->count].start);
    t->e[t->count].desktop = desktop;
    t->e[t->count].at = now;
    /* Too long to keep whole is not kept at all: half a command relaunches
     * something else. */
    if (cmd && strlen(cmd) < sizeof(t->e[0].cmd))
        snprintf(t->e[t->count].cmd, sizeof(t->e[0].cmd), "%s", cmd);
    else
        t->e[t->count].cmd[0] = '\0';
    t->count++;
}

/* The launch this window descends from, or NULL. `young`: only one still inside
 * the TTL, which is all that may place a window. */
static struct LaunchEntry *find_launch(struct FwmServer *server, struct FwmView *view,
                                       bool young) {
    if (!server || !view) return NULL;
    struct FwmLaunched *t = server->launched;
    if (!t || t->count == 0) return NULL;

    pid_t pid = view_pid(view);
    if (pid <= 0) return NULL;

    double now = now_sec();
    expire(t, now);

    /* Up from the window's own process one parent at a time. The hop limit is
     * against a /proc that lies rather than against a deep tree: a pid whose
     * parent chain loops would otherwise be walked forever. */
    for (int hop = 0; hop < LAUNCHED_HOPS && pid > 1; hop++) {
        for (int i = t->count - 1; i >= 0; i--) {
            if (t->e[i].pid == pid && (!young || launch_young(&t->e[i], now)))
                return &t->e[i];
        }
        pid = parent_pid(pid);
        if (pid <= 0) break;
    }
    return NULL;
}

int launched_desktop(struct FwmServer *server, struct FwmView *view) {
    struct LaunchEntry *e = find_launch(server, view, true);
    return e ? e->desktop : -1;
}

bool launched_command(struct FwmServer *server, struct FwmView *view,
                      pid_t *pid, char *out, size_t cap) {
    struct LaunchEntry *e = find_launch(server, view, false);
    if (!e || !e->cmd[0] || strlen(e->cmd) >= cap) return false;

    if (view_pid(view) != e->pid) {
        FwmView *other;
        wl_list_for_each(other, &server->views, link) {
            if (other != view && view_pid(other) == e->pid) return false;
        }
    }
    snprintf(out, cap, "%s", e->cmd);
    *pid = e->pid;
    return true;
}

void launched_finish(struct FwmServer *server) {
    if (!server) return;
    free(server->launched);
    server->launched = NULL;
}
