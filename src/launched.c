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
    int desktop;
    double at;      /* CLOCK_MONOTONIC seconds */
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

/* Forget everything older than the TTL, closing the gaps. Called on both ends
 * so the table cannot fill with launches that never produced a window. */
static void expire(struct FwmLaunched *t, double now) {
    int keep = 0;
    for (int i = 0; i < t->count; i++) {
        if (now - t->e[i].at <= LAUNCHED_TTL) t->e[keep++] = t->e[i];
    }
    t->count = keep;
}

/* The parent of `pid`, or 0.
 *
 * /proc/<pid>/stat holds the command name in parentheses, and a command may
 * contain spaces and parentheses of its own — so the fields are counted from
 * the LAST ')' rather than split from the front, which is the one way to read
 * this file that a process called ")  (" cannot break. */
static pid_t parent_pid(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char *close_paren = strrchr(buf, ')');
    if (!close_paren) return 0;
    int ppid = 0;
    if (sscanf(close_paren + 1, " %*c %d", &ppid) != 1) return 0;
    return ppid > 0 ? (pid_t)ppid : 0;
}

void launched_note(struct FwmServer *server, pid_t pid, int desktop) {
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
    /* Full of launches that are all still young: drop the oldest, which is the
     * one least likely to still be waiting for a window. */
    if (t->count == LAUNCHED_MAX) {
        memmove(&t->e[0], &t->e[1], (LAUNCHED_MAX - 1) * sizeof(t->e[0]));
        t->count--;
    }
    t->e[t->count].pid = pid;
    t->e[t->count].desktop = desktop;
    t->e[t->count].at = now;
    t->count++;
}

int launched_desktop(struct FwmServer *server, struct FwmView *view) {
    if (!server || !view) return -1;
    struct FwmLaunched *t = server->launched;
    if (!t || t->count == 0) return -1;

    pid_t pid = view_pid(view);
    if (pid <= 0) return -1;

    expire(t, now_sec());

    /* Up from the window's own process one parent at a time. The hop limit is
     * against a /proc that lies rather than against a deep tree: a pid whose
     * parent chain loops would otherwise be walked forever. */
    for (int hop = 0; hop < LAUNCHED_HOPS && pid > 1; hop++) {
        for (int i = t->count - 1; i >= 0; i--) {
            if (t->e[i].pid == pid) return t->e[i].desktop;
        }
        pid = parent_pid(pid);
        if (pid <= 0) break;
    }
    return -1;
}

void launched_finish(struct FwmServer *server) {
    if (!server) return;
    free(server->launched);
    server->launched = NULL;
}
