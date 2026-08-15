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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* pipe2, setpgid on a strict libc */
#endif

#include "volume.h"
#include "server.h"
#include "ui/osd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

/* The system volume, as far as a compositor is entitled to know it.
 *
 * fwm does not own the audio session — PipeWire does — so this is not a mixer.
 * It is the two things a knob needs: a number to show, and a way to move it.
 * Both go through the shell commands in [volume], which default to wpctl's (or
 * pactl's), so a machine with an unusual setup writes three lines instead of
 * waiting for fwm to grow a backend for it.
 *
 * The turn must be answered on the frame it happened, and a command takes
 * milliseconds to fork, exec and print — long enough to see. So the value is
 * PREDICTED from what we last knew, shown immediately, and then confirmed:
 * `set` is handed an absolute percentage (never a step), and a read is started
 * behind it whose answer corrects the panel if the mixer disagreed — because
 * something else moved the volume, or because it clamped, or because the first
 * turn of the session had nothing to predict from.
 */

#define VOL_TIMEOUT_S 5.0    /* a reader that has not answered by now is gone */

struct Volume {
    struct FwmServer *server;

    double percent;   /* last known, 0..max; < 0 = not known yet */
    int    muted;     /* 0, 1, or -1 for unknown */

    /* The pending step, applied when the first read lands. Only ever set
     * before anything is known: after that a step is applied to `percent`
     * straight away. */
    double pending;
    bool   pending_set;

    /* The reader. One at a time: a knob spun hard would otherwise leave a
     * process per detent racing to answer about the same number. */
    pid_t  pid;
    int    fd;
    double age;
    char   buf[512];
    size_t len;
};

Volume *volume_create(struct FwmServer *server) {
    Volume *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->server  = server;
    v->percent = -1.0;
    v->muted   = -1;
    v->fd      = -1;
    return v;
}

static void reader_stop(Volume *v) {
    if (v->pid > 0) {
        kill(-v->pid, SIGTERM);
        waitpid(v->pid, NULL, WNOHANG);
        v->pid = 0;
    }
    if (v->fd >= 0) { close(v->fd); v->fd = -1; }
    v->len = 0;
    v->age = 0.0;
}

void volume_destroy(Volume *v) {
    if (!v) return;
    reader_stop(v);
    free(v);
}

/* ── talking to the mixer ────────────────────────────────────────────── */

static const VolumeConfig *conf(Volume *v) { return &v->server->config.volume; }

/* Fire and forget, like a `spawn:` bind: the double fork orphans the command
 * so nothing here ever waits on the mixer. */
static void run_detached(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    } else {
        wlr_log(WLR_ERROR, "volume: cannot fork for \"%s\"", cmd);
    }
}

/* [volume] set, with %v replaced by the percentage. Written by hand rather
 * than with printf because the command is the USER's string: a stray %d in it
 * would otherwise be a format-string bug with their config as the input. */
static void expand_set(const char *tmpl, double percent, char *out, size_t cap) {
    char num[16];
    snprintf(num, sizeof(num), "%d", (int)(percent + 0.5));
    size_t o = 0;
    for (size_t i = 0; tmpl[i] && o + 1 < cap; i++) {
        if (tmpl[i] == '%' && tmpl[i + 1] == 'v') {
            for (size_t k = 0; num[k] && o + 1 < cap; k++) out[o++] = num[k];
            i++;
        } else {
            out[o++] = tmpl[i];
        }
    }
    out[o] = '\0';
}

/* Start the reader. Modelled on the [stats] custom sensors, down to the
 * CLOEXEC pipe and the process group: same problem, same answer. */
static void reader_start(Volume *v) {
    const char *cmd = conf(v)->get;
    if (!cmd[0] || v->pid > 0) return;

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return; }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    setpgid(pid, pid);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    v->pid = pid;
    v->fd  = pipefd[0];
    v->age = 0.0;
    v->len = 0;
}

/* What the mixer said, out of whatever shape it said it in.
 *
 *   wpctl:  "Volume: 0.62" / "Volume: 0.62 [MUTED]"
 *   pactl:  "Volume: front-left: 40632 /  62% / -10.29 dB, ..." + "Mute: no"
 *
 * A percentage wins where there is one, and a bare fraction is read as one:
 * between them that covers both, and a command of the user's own only has to
 * print a number somewhere for this to work. */
static bool parse_reading(const char *s, double *percent, int *muted) {
    *muted = (strstr(s, "MUTED") || strstr(s, "Mute: yes")) ? 1 : 0;

    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        char *end;
        double n = strtod(p, &end);
        if (*end == '%') { *percent = n; return true; }
        /* A fraction — wpctl's form. Anything at or below 2 is read as one, so
         * "0.62" is 62% and a plain "62" is not mistaken for 6200%. */
        if (*end == '.' || n <= 2.0) {
            double frac = strtod(p, &end);
            if (frac <= 2.0) { *percent = frac * 100.0; return true; }
        }
        p = end - 1;
    }
    return false;
}

/* ── the panel ───────────────────────────────────────────────────────── */

static void show(Volume *v, bool at_end) {
    char value[32];
    if (v->muted == 1) snprintf(value, sizeof(value), "muted");
    else               snprintf(value, sizeof(value), "%d%%", (int)(v->percent + 0.5));

    double max = conf(v)->max > 0.0 ? conf(v)->max : 100.0;
    double frac = v->percent >= 0.0 ? v->percent / max : -1.0;
    osd_show(v->server->osd, "Volume", value, frac, at_end);
}

/* ── the action ──────────────────────────────────────────────────────── */

static void apply_percent(Volume *v, double want) {
    double max = conf(v)->max > 0.0 ? conf(v)->max : 100.0;
    bool at_end = false;
    if (want < 0.0)   { want = 0.0; at_end = true; }
    if (want > max)   { want = max; at_end = true; }
    v->percent = want;

    const char *tmpl = conf(v)->set;
    if (tmpl[0]) {
        char cmd[512];
        expand_set(tmpl, want, cmd, sizeof(cmd));
        run_detached(cmd);
    }
    show(v, at_end);
    /* Behind the change, ask what actually happened. */
    reader_start(v);
}

void volume_action(Volume *v, const char *arg) {
    if (!v || !arg) return;
    if (!conf(v)->get[0] && !conf(v)->set[0]) {
        wlr_log(WLR_ERROR, "volume:%s: no mixer — neither wpctl nor pactl is on "
                           "PATH, and [volume] names no commands of its own", arg);
        return;
    }

    if (strcmp(arg, "mute") == 0) {
        if (conf(v)->mute[0]) run_detached(conf(v)->mute);
        /* Predicted the same way a step is: flip it now, correct it when the
         * reader answers. Unknown stays unknown — there is nothing to flip. */
        if (v->muted >= 0) v->muted = !v->muted;
        show(v, false);
        reader_start(v);
        return;
    }

    char *end;
    double n = strtod(arg, &end);
    if (end == arg || *end) {
        wlr_log(WLR_ERROR, "volume:%s: expected +N, -N, a number, or \"mute\"", arg);
        return;
    }
    bool relative = (*arg == '+' || *arg == '-');

    if (!relative) {
        v->muted = v->muted == 1 ? 1 : v->muted;   /* setting a level un-mutes nothing by itself */
        apply_percent(v, n);
        return;
    }

    if (v->percent < 0.0) {
        /* The first turn of the session, with nothing to step from: read first
         * and apply the step to the answer. One detent of delay, once. */
        v->pending = n;
        v->pending_set = true;
        reader_start(v);
        return;
    }
    apply_percent(v, v->percent + n);
}

/* ── the reader's answer ─────────────────────────────────────────────── */

static void reader_finish(Volume *v) {
    v->buf[v->len < sizeof(v->buf) ? v->len : sizeof(v->buf) - 1] = '\0';

    double percent;
    int muted;
    bool ok = parse_reading(v->buf, &percent, &muted);

    reader_stop(v);
    if (!ok) {
        wlr_log(WLR_ERROR, "volume: cannot read a level out of \"%s\" — "
                           "check [volume] get", conf(v)->get);
        v->pending_set = false;
        return;
    }

    v->muted = muted;

    if (v->pending_set) {
        /* The step that was waiting for a number to step from. */
        v->pending_set = false;
        v->percent = percent;
        apply_percent(v, percent + v->pending);
        return;
    }

    /* Only worth redrawing if the mixer disagreed with the prediction — and
     * only while the panel is still up, so a stray reading long after the hand
     * stopped does not put it back on screen. */
    bool differs = v->percent < 0.0 || (int)(percent + 0.5) != (int)(v->percent + 0.5);
    v->percent = percent;
    if (differs && osd_busy(v->server->osd)) show(v, false);
}

void volume_tick(Volume *v, double dt) {
    if (!v || v->pid <= 0) return;
    v->age += dt;

    for (;;) {
        if (v->len >= sizeof(v->buf) - 1) { reader_finish(v); return; }
        ssize_t n = read(v->fd, v->buf + v->len, sizeof(v->buf) - 1 - v->len);
        if (n > 0) { v->len += (size_t)n; continue; }
        if (n == 0) { reader_finish(v); return; }          /* EOF: it is done */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* still thinking */
        reader_stop(v);                                    /* the pipe broke */
        return;
    }

    if (v->age > VOL_TIMEOUT_S) {
        wlr_log(WLR_INFO, "volume: \"%s\" did not answer in %.0fs — killed",
                conf(v)->get, VOL_TIMEOUT_S);
        reader_stop(v);
        v->pending_set = false;
    }
}

bool volume_busy(Volume *v) { return v && v->pid > 0; }
