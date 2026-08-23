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

/* The tray's readouts. See stats.h for the shape of the thing and why custom
 * sensors are shell commands. */

/* pipe2() is a GNU extension, and this has to be said before the first header
 * pulls in features.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "stats.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wlr/util/log.h>

/* The built-ins are reads out of memory-backed files, so they are taken on
 * their own cadence rather than the (user-set, deliberately slow) one the
 * commands run on. A second is what a load figure means anyway: faster and the
 * number is noise the eye cannot follow. */
#define BUILTIN_PERIOD_S  1.0

/* The charge, on its own much slower clock — see the sampling switch. */
#define BAT_PERIOD_S     10.0

/* A command that has not answered in this long is not going to. Killed rather
 * than left running: sensors run again every interval, and a hung one would
 * otherwise stack up a process per interval for the rest of the session. */
#define STATS_CMD_TIMEOUT_S 5.0

#define GPU_BUSY_GLOB "/sys/class/drm/card%d/device/gpu_busy_percent"
#define POWER_SUPPLY_DIR "/sys/class/power_supply"
#define NET_DEV_PATH     "/proc/net/dev"

/* Long enough for a machine with a wall of veth interfaces: /proc/net/dev is
 * about 200 bytes a line, so this is some eighty of them. A machine with more
 * than that has the tail of its list cut off rather than a wrong total — the
 * interfaces that matter are at the top of the file, not the end of it. */
#define NET_DEV_BUF      16384

typedef struct {
    StatsItem pub;                 /* what callers see */
    char      cmd[STATS_CMD_MAX];  /* custom only */

    double    due;                 /* seconds until the next sample */
    /* A custom sample in flight. `pid` is 0 when nothing is running, which is
     * also the only state in which a new one may be started: one child per
     * sensor, however slow it is, so a command slower than its interval simply
     * reports less often instead of forking forever. */
    pid_t     pid;
    int       fd;                  /* read end of the child's stdout */
    double    age;                 /* how long the child has been running */
    char      buf[128];
    size_t    len;
} Item;

struct FwmStats {
    Item   items[STATS_MAX_ITEMS];
    int    count;
    double interval;

    /* /proc/stat's previous totals, for the difference that IS the load. */
    unsigned long long cpu_busy, cpu_total;
    int    cpu_primed;   /* one sample is a total, not a load: skip the first */

    char   gpu_path[64]; /* "" when no card exposes a busy percentage */

    char   bat_path[288]; /* "" when the machine has no battery of its own */

    /* The counters behind the network rate, and the clock they were read on.
     * Like the CPU load this is a difference between two reads, so the first
     * one produces nothing — and unlike the CPU it is a difference per SECOND,
     * which is why the time is kept rather than assumed: a sensor that was
     * switched off in the menu for a minute comes back with a minute's worth
     * of bytes, and dividing those by the sample period would report a burst
     * that never happened. */
    unsigned long long net_rx, net_tx;
    int    net_primed;
    struct timespec net_at;
};

/* ── built-in sensors ────────────────────────────────────────────────── */

/* Read a whole small file. /proc and /sys files are generated on read, so they
 * report a size of 0 and cannot be sized first — hence the fixed buffer. */
static int read_small(const char *path, char *out, size_t out_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, out_size - 1);
    close(fd);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
}

/* Percentage of the last period that every core spent doing something.
 *
 * The first line of /proc/stat is a running total since boot, so a single read
 * says what the machine has been doing since it was switched on — which is not
 * a number anybody wants in a tray. The load is the difference between two
 * reads, which is why the first one produces nothing. */
static int sample_cpu(FwmStats *s, char *out, size_t out_size) {
    char buf[256];
    if (!read_small("/proc/stat", buf, sizeof(buf))) return 0;

    unsigned long long v[10] = {0};
    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) < 4)
        return 0;

    unsigned long long total = 0;
    for (int i = 0; i < 10; i++) total += v[i];
    /* Idle and iowait (fields 4 and 5) are the two the CPU was not working. */
    unsigned long long idle = v[3] + v[4];
    unsigned long long busy = total - idle;

    unsigned long long dt = total - s->cpu_total;
    unsigned long long db = busy - s->cpu_busy;
    s->cpu_total = total;
    s->cpu_busy  = busy;

    if (!s->cpu_primed) { s->cpu_primed = 1; return 0; }
    if (dt == 0) return 0;   /* two reads inside one jiffy */

    double pct = 100.0 * (double)db / (double)dt;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    snprintf(out, out_size, "%.0f%%", pct);
    return 1;
}

/* Memory in use, the way a person means it: total minus what the kernel says is
 * available. NOT total minus free — free excludes the page cache, so it reports
 * a machine with a warm cache as nearly full, which is the classic way to make
 * a memory readout useless. */
static int sample_ram(char *out, size_t out_size) {
    char buf[2048];
    if (!read_small("/proc/meminfo", buf, sizeof(buf))) return 0;

    unsigned long long total = 0, avail = 0;
    const char *p = buf;
    while (*p) {
        if (sscanf(p, "MemTotal: %llu kB", &total) == 1) {}
        else if (sscanf(p, "MemAvailable: %llu kB", &avail) == 1) {}
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    if (total == 0 || avail > total) return 0;

    double used_gb = (double)(total - avail) / (1024.0 * 1024.0);
    snprintf(out, out_size, "%.1fG", used_gb);
    return 1;
}

/* The first card that will say how busy it is. amdgpu and i915 both export
 * gpu_busy_percent; a card that does not (nvidia's proprietary stack) leaves
 * the sensor unavailable, and the menu greys it out rather than showing a
 * readout that never moves. */
static void find_gpu(FwmStats *s) {
    s->gpu_path[0] = '\0';
    for (int card = 0; card < 4; card++) {
        char path[64];
        snprintf(path, sizeof(path), GPU_BUSY_GLOB, card);
        if (access(path, R_OK) == 0) {
            snprintf(s->gpu_path, sizeof(s->gpu_path), "%s", path);
            return;
        }
    }
}

static int sample_gpu(FwmStats *s, char *out, size_t out_size) {
    if (!s->gpu_path[0]) return 0;
    char buf[32];
    if (!read_small(s->gpu_path, buf, sizeof(buf))) return 0;
    int pct = atoi(buf);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(out, out_size, "%d%%", pct);
    return 1;
}

/* ── the battery ─────────────────────────────────────────────────────
 *
 * The one readout on this list that is not about load, and the one a laptop
 * actually wants: a compositor that draws a status strip and leaves the charge
 * out of it sends its user to install a second bar for one number.
 *
 * `scope = Device` is what keeps a wireless mouse out of the tray. Peripherals
 * report themselves as batteries here too — hidpp_battery_0, a headset, a
 * stylus — and they are batteries, just not the one the machine runs on. A
 * supply with no `capacity` at all is skipped for the same reason: nothing to
 * show but a name. */
static void find_battery(FwmStats *s) {
    s->bat_path[0] = '\0';

    DIR *d = opendir(POWER_SUPPLY_DIR);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        char path[384], buf[64];
        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/type", e->d_name);
        if (!read_small(path, buf, sizeof(buf))) continue;
        if (strncmp(buf, "Battery", 7) != 0) continue;

        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/scope", e->d_name);
        if (read_small(path, buf, sizeof(buf)) && strncmp(buf, "Device", 6) == 0)
            continue;   /* a mouse, not the machine */

        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/capacity", e->d_name);
        if (access(path, R_OK) != 0) continue;

        snprintf(s->bat_path, sizeof(s->bat_path), POWER_SUPPLY_DIR "/%s", e->d_name);
        break;
    }
    closedir(d);
}

/* "87%" on the way down, "87%+" on the way up. A trailing plus rather than a
 * bolt or an arrow: the tray already gambles on two glyphs (see ui/modes.h)
 * and a charge readout is not the place to add a third — everything here has
 * to be legible in whatever "sans" resolves to on the machine. */
static int sample_bat(FwmStats *s, char *out, size_t out_size) {
    if (!s->bat_path[0]) return 0;

    char path[sizeof(s->bat_path) + 16], buf[64];
    snprintf(path, sizeof(path), "%s/capacity", s->bat_path);
    if (!read_small(path, buf, sizeof(buf))) return 0;
    int pct = atoi(buf);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* "Full" is deliberately NOT a plus: a battery that has finished charging
     * is not filling, and a plug left in overnight should read the same as one
     * that was never in. */
    snprintf(path, sizeof(path), "%s/status", s->bat_path);
    int charging = read_small(path, buf, sizeof(buf)) && strncmp(buf, "Charging", 8) == 0;

    snprintf(out, out_size, "%d%%%s", pct, charging ? "+" : "");
    return 1;
}

/* ── the network ─────────────────────────────────────────────────────
 *
 * What is moving right now, both ways, not what has moved since boot: a total
 * since boot is the same number all day and tells nobody whether the download
 * they are waiting on is still going.
 *
 * Only interfaces with a device behind them are counted. /sys/class/net/<if>/
 * device is a link to real hardware, so it is present for ethernet and wifi
 * and absent for lo, docker0, veth pairs, bridges and tunnels — and a bridge
 * counts every byte its members already counted, which on a machine running
 * containers doubles or triples the reading. A machine where nothing has a
 * device (a container, a VM with a virtio interface the driver does not
 * expose) falls back to everything except lo rather than reporting zero
 * forever. */
static int net_has_device(const char *ifname) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
    return access(path, F_OK) == 0;
}

static void fmt_rate(double bytes_per_s, char *out, size_t out_size) {
    if (bytes_per_s >= 1024.0 * 1024.0)
        snprintf(out, out_size, "%.1fM", bytes_per_s / (1024.0 * 1024.0));
    else if (bytes_per_s >= 1024.0)
        snprintf(out, out_size, "%.0fK", bytes_per_s / 1024.0);
    else
        snprintf(out, out_size, "0");
}

static int sample_net(FwmStats *s, char *out, size_t out_size) {
    char buf[NET_DEV_BUF];
    if (!read_small(NET_DEV_PATH, buf, sizeof(buf))) return 0;

    unsigned long long phys_rx = 0, phys_tx = 0, any_rx = 0, any_tx = 0;
    int phys_seen = 0;

    /* Two header lines, then one per interface:
     *   "  eth0: <rx_bytes> <rx_packets> ... <tx_bytes> <tx_packets> ..." */
    const char *p = buf;
    for (int line = 0; *p; line++) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        if (line >= 2) {
            const char *colon = memchr(p, ':', len);
            if (colon) {
                char name[32];
                const char *n = p;
                while (n < colon && (*n == ' ' || *n == '\t')) n++;
                size_t nlen = (size_t)(colon - n);
                if (nlen < sizeof(name)) {
                    memcpy(name, n, nlen);
                    name[nlen] = '\0';

                    unsigned long long v[9] = {0};
                    if (strcmp(name, "lo") != 0 &&
                        sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) == 9) {
                        any_rx += v[0];
                        any_tx += v[8];
                        if (net_has_device(name)) {
                            phys_rx += v[0];
                            phys_tx += v[8];
                            phys_seen = 1;
                        }
                    }
                }
            }
        }

        if (!nl) break;
        p = nl + 1;
    }

    unsigned long long rx = phys_seen ? phys_rx : any_rx;
    unsigned long long tx = phys_seen ? phys_tx : any_tx;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double secs = (double)(now.tv_sec - s->net_at.tv_sec) +
                  (double)(now.tv_nsec - s->net_at.tv_nsec) / 1e9;

    unsigned long long prev_rx = s->net_rx, prev_tx = s->net_tx;
    s->net_rx = rx;
    s->net_tx = tx;
    s->net_at = now;

    if (!s->net_primed) { s->net_primed = 1; return 0; }
    if (secs <= 0.0) return 0;
    /* Switched off in the menu and back on, or the machine was suspended: the
     * counters kept running while nobody was reading them, and the difference
     * now spans minutes. Averaging it over those minutes would be true and
     * useless — "what is moving right now" is the question — so take this read
     * as the new baseline and answer one period later. */
    if (secs > 5.0) return 0;

    /* An interface that was unplugged, renamed or restarted takes its counter
     * with it, so the sum can go DOWN. Reporting that as a negative rate — or
     * as the enormous positive one unsigned arithmetic would produce — is
     * worse than reporting nothing for one period. */
    double down = rx >= prev_rx ? (double)(rx - prev_rx) / secs : 0.0;
    double up   = tx >= prev_tx ? (double)(tx - prev_tx) / secs : 0.0;

    char d[10], u[10];
    fmt_rate(down, d, sizeof(d));
    fmt_rate(up, u, sizeof(u));
    /* U+2193 / U+2191, the two arrows every "sans" has had since the DejaVu
     * days. Down first: it is the one being waited on. */
    snprintf(out, out_size, "\xE2\x86\x93%s \xE2\x86\x91%s", d, u);
    return 1;
}

/* ── custom sensors ──────────────────────────────────────────────────── */

/* Collect a finished child. Returns 1 when `it->pub.value` changed. */
static int reap(Item *it) {
    if (it->fd >= 0) { close(it->fd); it->fd = -1; }
    if (it->pid > 0) {
        /* The wait runs on the compositor's own thread, so it has to be
         * bounded — and the pipe closing does not bound it. EOF says the write
         * end is gone, not that the process is: `cmd = "echo 50; exec >/dev/null;
         * sleep 300"` reaches here with the shell alive for another five
         * minutes, and waiting for it would stop the compositor dead for that
         * long. Killing the group first makes the wait what the comment used
         * to claim it already was.
         *
         * Safe against a recycled pgid precisely because the child has not been
         * reaped yet: its pid is still ours and cannot name another process. */
        kill(-it->pid, SIGKILL);
        while (waitpid(it->pid, NULL, 0) < 0 && errno == EINTR) {}
        it->pid = 0;
    }

    /* First line only, whitespace off both ends: `pactl` and friends answer
     * with a trailing newline, and a tray is one line high. */
    char *nl = memchr(it->buf, '\n', it->len);
    size_t n = nl ? (size_t)(nl - it->buf) : it->len;
    while (n > 0 && (it->buf[n - 1] == ' ' || it->buf[n - 1] == '\t' ||
                     it->buf[n - 1] == '\r')) n--;
    size_t start = 0;
    while (start < n && (it->buf[start] == ' ' || it->buf[start] == '\t')) start++;

    char value[sizeof(it->pub.value)];
    snprintf(value, sizeof(value), "%.*s", (int)(n - start), it->buf + start);
    it->len = 0;

    if (strcmp(value, it->pub.value) == 0) return 0;
    snprintf(it->pub.value, sizeof(it->pub.value), "%s", value);
    return 1;
}

static void start_command(Item *it) {
    /* CLOEXEC on both ends: everything else fwm spawns — the launcher's
     * applications, a keybind's script — must not inherit a sensor's pipe. The
     * child's own dup2 onto stdout clears it for the one end that needs it. */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        /* Child. Only async-signal-safe calls from here to exec. stdout goes
         * down the pipe; stderr is left alone so a broken command still says so
         * in the session's log rather than vanishing. */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        /* Its own process group, so the kill on timeout takes the whole
         * pipeline and not just the shell that is waiting on it. */
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", it->cmd, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    /* Set from BOTH sides, as the race demands: the kill on timeout aims at the
     * group, and until the child's own setpgid has run there is no group to aim
     * at. Whichever call lands first wins and the other fails harmlessly. */
    setpgid(pid, pid);
    /* Non-blocking, because the read happens on the compositor's thread: a
     * command that prints nothing for a second must cost this loop nothing. */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    it->pid = pid;
    it->fd  = pipefd[0];
    it->age = 0.0;
    it->len = 0;
}

/* Drain whatever the child has written. Returns 1 when the value changed, which
 * only happens on the read that ends in EOF. */
static int poll_command(Item *it, double dt) {
    if (it->pid <= 0) return 0;
    it->age += dt;

    for (;;) {
        if (it->len >= sizeof(it->buf) - 1) {
            /* More than a tray line's worth. Everything after the first line is
             * discarded anyway, so stop reading and take what we have — reap
             * kills the group, so a command still printing is not left to it. */
            return reap(it);
        }
        ssize_t n = read(it->fd, it->buf + it->len, sizeof(it->buf) - 1 - it->len);
        if (n > 0) { it->len += (size_t)n; continue; }
        if (n == 0) return reap(it);                      /* EOF: it is done */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* still thinking */
        return reap(it);                                   /* pipe broke */
    }

    if (it->age > STATS_CMD_TIMEOUT_S) {
        wlr_log(WLR_INFO, "stats: \"%s\" did not answer in %.0fs — killed",
                it->pub.name, STATS_CMD_TIMEOUT_S);
        return reap(it);
    }
    return 0;
}

/* ── the list ────────────────────────────────────────────────────────── */

static int source_of(const char *name) {
    if (strcmp(name, "cpu") == 0) return STATS_SRC_CPU;
    if (strcmp(name, "ram") == 0) return STATS_SRC_RAM;
    if (strcmp(name, "gpu") == 0) return STATS_SRC_GPU;
    if (strcmp(name, "bat") == 0) return STATS_SRC_BAT;
    if (strcmp(name, "net") == 0) return STATS_SRC_NET;
    return STATS_SRC_CUSTOM;
}

static void item_stop(Item *it) {
    if (it->pid > 0) {
        kill(-it->pid, SIGKILL);
        while (waitpid(it->pid, NULL, 0) < 0 && errno == EINTR) {}
        it->pid = 0;
    }
    if (it->fd >= 0) { close(it->fd); it->fd = -1; }
    it->len = 0;
}

void stats_reconfigure(FwmStats *s, const StatsConfig *cfg) {
    if (!s || !cfg) return;

    Item old[STATS_MAX_ITEMS];
    int old_count = s->count;
    memcpy(old, s->items, sizeof(old));

    memset(s->items, 0, sizeof(s->items));
    s->count = 0;
    s->interval = cfg->interval;

    for (int i = 0; i < cfg->item_count && s->count < STATS_MAX_ITEMS; i++) {
        const char *name = cfg->items[i];
        Item *it = &s->items[s->count];
        snprintf(it->pub.name, sizeof(it->pub.name), "%s", name);
        it->pub.source = source_of(name);
        it->pub.enabled = 1;
        it->pub.available = 1;
        it->fd = -1;
        /* Spread the first samples out rather than firing every command on the
         * same frame the config loaded: a reload is already the busiest moment
         * in the session. */
        it->due = 0.05 * s->count;

        if (it->pub.source == STATS_SRC_CUSTOM) {
            for (int c = 0; c < cfg->custom_count; c++) {
                if (strcmp(cfg->custom[c].name, name) != 0) continue;
                snprintf(it->cmd, sizeof(it->cmd), "%s", cfg->custom[c].cmd);
                break;
            }
            /* config.c has already reported the name that has no command; this
             * is only what keeps it from being a sensor that runs "". */
            if (!it->cmd[0]) continue;
        }
        if (it->pub.source == STATS_SRC_GPU && !s->gpu_path[0]) it->pub.available = 0;
        /* A desktop has no battery, and the menu greys the row out rather than
         * leaving a readout that never fills in. Looked for once per reload,
         * not once per sample: a machine does not grow one. */
        if (it->pub.source == STATS_SRC_BAT && !s->bat_path[0]) it->pub.available = 0;
        if (it->pub.source == STATS_SRC_NET && access(NET_DEV_PATH, R_OK) != 0)
            it->pub.available = 0;

        /* An item that was already here keeps what it had: its value, so the
         * pill does not blank for an interval, and its on/off, so a reload does
         * not undo what the menu was used to choose. */
        for (int o = 0; o < old_count; o++) {
            if (strcmp(old[o].pub.name, it->pub.name) != 0) continue;
            if (old[o].pub.source != it->pub.source) continue;
            snprintf(it->pub.value, sizeof(it->pub.value), "%s", old[o].pub.value);
            it->pub.enabled = old[o].pub.enabled;
            break;
        }
        s->count++;
    }

    /* Children belonging to sensors that are gone (or whose command changed)
     * are not left running: nothing would ever read their output. */
    for (int o = 0; o < old_count; o++) item_stop(&old[o]);
}

FwmStats *stats_create(const StatsConfig *cfg) {
    FwmStats *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    find_gpu(s);
    find_battery(s);
    stats_reconfigure(s, cfg);
    return s;
}

void stats_destroy(FwmStats *s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) item_stop(&s->items[i]);
    free(s);
}

bool stats_tick(FwmStats *s, double dt) {
    if (!s) return false;
    int changed = 0;

    for (int i = 0; i < s->count; i++) {
        Item *it = &s->items[i];
        if (!it->pub.available) continue;

        /* A sensor nobody is looking at is not sampled. It keeps its last
         * value, so switching it back on in the menu shows something at once
         * and then corrects itself on the next tick. */
        if (!it->pub.enabled) continue;

        changed |= poll_command(it, dt);

        it->due -= dt;
        if (it->due > 0.0) continue;

        char value[sizeof(it->pub.value)] = "";
        int have = 0;
        switch (it->pub.source) {
        case STATS_SRC_CPU:
            it->due = BUILTIN_PERIOD_S;
            have = sample_cpu(s, value, sizeof(value));
            break;
        case STATS_SRC_RAM:
            it->due = BUILTIN_PERIOD_S;
            have = sample_ram(value, sizeof(value));
            break;
        case STATS_SRC_GPU:
            it->due = BUILTIN_PERIOD_S;
            have = sample_gpu(s, value, sizeof(value));
            break;
        case STATS_SRC_BAT:
            /* Charge moves in minutes, not seconds. Sampling it as often as
             * the load sensors would be two file reads a second to watch a
             * number that changes forty times a day — and the tray is only
             * repainted when a value actually changes, so a slower sensor is
             * also a quieter one. */
            it->due = BAT_PERIOD_S;
            have = sample_bat(s, value, sizeof(value));
            break;
        case STATS_SRC_NET:
            it->due = BUILTIN_PERIOD_S;
            have = sample_net(s, value, sizeof(value));
            break;
        default:
            it->due = s->interval;
            /* Only if the last run has finished; see Item.pid. */
            if (it->pid <= 0) start_command(it);
            continue;
        }

        if (have && strcmp(value, it->pub.value) != 0) {
            snprintf(it->pub.value, sizeof(it->pub.value), "%s", value);
            changed = 1;
        }
    }
    return changed != 0;
}

int stats_count(const FwmStats *s) { return s ? s->count : 0; }

const StatsItem *stats_item(const FwmStats *s, int i) {
    if (!s || i < 0 || i >= s->count) return NULL;
    return &s->items[i].pub;
}

void stats_set_enabled(FwmStats *s, int i, int on) {
    if (!s || i < 0 || i >= s->count) return;
    s->items[i].pub.enabled = on ? 1 : 0;
    /* Switched back on: sample at the next tick rather than at the end of the
     * interval, or a five-second sensor spends five seconds looking broken. */
    if (on) s->items[i].due = 0.0;
}

void stats_format(const FwmStats *s, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!s) return;

    size_t len = 0;
    for (int i = 0; i < s->count; i++) {
        const StatsItem *it = &s->items[i].pub;
        if (!it->enabled || !it->available || !it->value[0]) continue;

        /* NAME value, joined by the same bullet the clock uses. The name is
         * upper-cased on the way out rather than stored that way: it is the
         * config key everywhere else, and matching what the user typed is what
         * makes the menu and the file read as the same list. */
        char up[STATS_NAME_MAX];
        size_t j = 0;
        for (; it->name[j] && j < sizeof(up) - 1; j++) {
            char c = it->name[j];
            up[j] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        up[j] = '\0';

        int n = snprintf(out + len, out_size - len, "%s%s %s",
                         len ? " \xE2\x80\xA2 " : "", up, it->value);
        if (n < 0 || (size_t)n >= out_size - len) break;   /* full; stop cleanly */
        len += (size_t)n;
    }
}
