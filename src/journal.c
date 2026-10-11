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

#include "journal.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A day of steady work is a few thousand events — a window dragged across the
 * screen is a dozen, a throw a few more — so the ring holds several days of it,
 * in half a megabyte. A thousand windows is more than anyone opens between two
 * reboots without noticing. The file is let grow to about what the two rings
 * hold written out, and then rewritten from them. */
#define JOURNAL_EVENTS    16384
#define JOURNAL_WINDOWS   1024
#define JOURNAL_FILE_MAX  (4u << 20)

#define LINE_MAX_BYTES    512

struct FwmJournal {
    JournalEvent *ev;
    int ev_cap, ev_start, ev_n;
    JournalWindow *win;
    int win_cap, win_start, win_n;
    uint32_t next_wid;
    uint64_t version, list_version;

    char *path;
    int fd;
    size_t file_bytes, file_max;
};

static const char kind_char[JOURNAL_KINDS] = {
    [JOURNAL_START] = 'S', [JOURNAL_OPEN] = 'O', [JOURNAL_CLOSE] = 'C',
    [JOURNAL_TITLE] = 'T', [JOURNAL_FOCUS] = 'F', [JOURNAL_MOVE] = 'M',
    [JOURNAL_REST] = 'R', [JOURNAL_THROW] = 'H',
};

static int kind_of(char c) {
    for (int k = 0; k < JOURNAL_KINDS; k++)
        if (kind_char[k] == c) return k;
    return -1;
}

/* A name as the file can hold it: the fields are separated by tabs and the
 * events by newlines, so neither may appear inside one. */
static void clean_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) {
        char c = src[i];
        dst[i] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[i] = '\0';
    /* Cut where a character ends: a title chopped inside a UTF-8 sequence
     * draws as a box, or not at all. Only the last character can be cut, and
     * it is dropped only if it was. */
    if (src && src[i]) {
        size_t k = i;
        while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0) {
            unsigned char lead = (unsigned char)dst[k - 1];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (i - (k - 1) < need) dst[k - 1] = '\0';
        }
    }
}

/* ── the rings ────────────────────────────────────────────────────────── */

static JournalWindow *window_find(FwmJournal *j, uint32_t wid) {
    for (int i = j->win_n - 1; i >= 0; i--) {
        JournalWindow *w = &j->win[(j->win_start + i) % j->win_cap];
        if (w->wid == wid) return w;
    }
    return NULL;
}

static JournalWindow *window_add(FwmJournal *j, uint32_t wid) {
    JournalWindow *w;
    if (j->win_n < j->win_cap) {
        w = &j->win[(j->win_start + j->win_n) % j->win_cap];
        j->win_n++;
    } else {
        w = &j->win[j->win_start];
        j->win_start = (j->win_start + 1) % j->win_cap;
    }
    memset(w, 0, sizeof(*w));
    w->wid = wid;
    if (wid >= j->next_wid) j->next_wid = wid + 1;
    return w;
}

static void event_push(FwmJournal *j, const JournalEvent *e) {
    if (j->ev_n < j->ev_cap) {
        j->ev[(j->ev_start + j->ev_n) % j->ev_cap] = *e;
        j->ev_n++;
    } else {
        j->ev[j->ev_start] = *e;
        j->ev_start = (j->ev_start + 1) % j->ev_cap;
    }
    j->version++;
    if (e->kind == JOURNAL_OPEN || e->kind == JOURNAL_CLOSE
        || e->kind == JOURNAL_TITLE || e->kind == JOURNAL_START)
        j->list_version++;
}

static JournalEvent make_event(int64_t ms, uint32_t wid, JournalKind kind,
                               const JournalGeom *g) {
    JournalEvent e = { .ms = ms, .wid = wid, .kind = (uint8_t)kind };
    if (g) {
        e.desktop = (int8_t)g->desktop;
        e.x = g->x; e.y = g->y; e.w = g->w; e.h = g->h;
        e.flags = (uint8_t)g->flags;
    }
    return e;
}

/* ── the file ─────────────────────────────────────────────────────────── */

static int format_event(const FwmJournal *j, const JournalEvent *e,
                        char *buf, size_t cap) {
    char k = kind_char[e->kind];
    const JournalWindow *w = journal_window(j, e->wid);
    switch (e->kind) {
    case JOURNAL_START:
        return snprintf(buf, cap, "%c\t%" PRId64 "\n", k, e->ms);
    case JOURNAL_CLOSE:
    case JOURNAL_FOCUS:
        return snprintf(buf, cap, "%c\t%" PRId64 "\t%u\n", k, e->ms, e->wid);
    case JOURNAL_TITLE:
        return snprintf(buf, cap, "%c\t%" PRId64 "\t%u\t%s\n", k, e->ms, e->wid,
                        w ? w->title : "");
    case JOURNAL_OPEN:
        return snprintf(buf, cap, "%c\t%" PRId64 "\t%u\t%d\t%d\t%d\t%d\t%d\t%u\t%s\t%s\n",
                        k, e->ms, e->wid, e->desktop, e->x, e->y, e->w, e->h,
                        e->flags, w ? w->app_id : "", w ? w->title : "");
    default:
        return snprintf(buf, cap, "%c\t%" PRId64 "\t%u\t%d\t%d\t%d\t%d\t%d\t%u\n",
                        k, e->ms, e->wid, e->desktop, e->x, e->y, e->w, e->h,
                        e->flags);
    }
}

static void file_write(FwmJournal *j, const char *buf, int len) {
    if (j->fd < 0 || len <= 0) return;
    ssize_t n = write(j->fd, buf, (size_t)len);
    if (n > 0) j->file_bytes += (size_t)n;
}

static void file_rewrite(FwmJournal *j);

static void event_write(FwmJournal *j, const JournalEvent *e) {
    if (j->fd < 0) return;
    char buf[LINE_MAX_BYTES];
    int len = format_event(j, e, buf, sizeof(buf));
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    file_write(j, buf, len);
    if (j->file_bytes > j->file_max) file_rewrite(j);
}

/* What memory holds, written out afresh: every window first, so the names are
 * there for the events that follow, then the events. The file is written
 * beside the old one and moved over it, so a crash half way leaves the old
 * history rather than half of the new one. */
static void file_rewrite(FwmJournal *j) {
    if (!j->path) return;
    size_t plen = strlen(j->path);
    char *tmp = malloc(plen + 5);
    if (!tmp) return;
    memcpy(tmp, j->path, plen);
    memcpy(tmp + plen, ".new", 5);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { free(tmp); return; }

    size_t bytes = 0;
    char buf[LINE_MAX_BYTES];
    bool ok = true;
    for (int i = 0; i < j->win_n && ok; i++) {
        const JournalWindow *w = &j->win[(j->win_start + i) % j->win_cap];
        int len = snprintf(buf, sizeof(buf), "W\t%u\t%" PRId64 "\t%" PRId64 "\t%s\t%s\n",
                           w->wid, w->opened_ms, w->closed_ms, w->app_id, w->title);
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        ok = write(fd, buf, (size_t)len) == len;
        bytes += (size_t)len;
    }
    for (int i = 0; i < j->ev_n && ok; i++) {
        int len = format_event(j, journal_at(j, i), buf, sizeof(buf));
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        ok = write(fd, buf, (size_t)len) == len;
        bytes += (size_t)len;
    }
    close(fd);

    if (!ok || rename(tmp, j->path) != 0) {
        unlink(tmp);
        free(tmp);
        /* Could not rewrite it: stop growing it instead. Memory still has the
         * history; only the copy that would outlive a crash stops keeping up. */
        if (j->fd >= 0) close(j->fd);
        j->fd = -1;
        return;
    }
    free(tmp);
    if (j->fd >= 0) close(j->fd);
    j->fd = open(j->path, O_WRONLY | O_APPEND | O_CLOEXEC);
    j->file_bytes = bytes;
}

/* ── reading it back ──────────────────────────────────────────────────── */

/* Fields of one line, split on tabs in place. */
static int split(char *line, char **f, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = '\0';
        p = t + 1;
    }
    return n;
}

/* The windows an earlier run had open when it went, closed at the last moment
 * it was heard from. Only those opened before `before`: windows that came
 * after belong to a later run, and are not this run's to close. */
static void close_orphans(FwmJournal *j, int64_t before, int64_t last_ms) {
    for (int i = 0; i < j->win_n; i++) {
        JournalWindow *w = &j->win[(j->win_start + i) % j->win_cap];
        if (w->closed_ms || w->opened_ms > before) continue;
        w->closed_ms = last_ms > w->opened_ms ? last_ms : w->opened_ms;
        /* Into memory only. The file says the run started again, and that is
         * the whole of what it knows; working out the rest is this function's
         * job every time the history is read back. */
        JournalEvent e = make_event(w->closed_ms, w->wid, JOURNAL_CLOSE, NULL);
        event_push(j, &e);
    }
}

static void replay_line(FwmJournal *j, char *line, int64_t *last_ms) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    char *f[12];
    int n = split(line, f, 12);
    if (n < 2 || strlen(f[0]) != 1) return;

    if (f[0][0] == 'W') {
        if (n < 6) return;
        uint32_t wid = (uint32_t)strtoul(f[1], NULL, 10);
        if (!wid) return;
        JournalWindow *w = window_find(j, wid);
        if (!w) w = window_add(j, wid);
        w->opened_ms = strtoll(f[2], NULL, 10);
        w->closed_ms = strtoll(f[3], NULL, 10);
        clean_copy(w->app_id, sizeof(w->app_id), f[4]);
        clean_copy(w->title, sizeof(w->title), f[5]);
        return;
    }

    int kind = kind_of(f[0][0]);
    if (kind < 0) return;
    int64_t ms = strtoll(f[1], NULL, 10);

    if (kind == JOURNAL_START) {
        close_orphans(j, ms, *last_ms);
        JournalEvent e = make_event(ms, 0, JOURNAL_START, NULL);
        event_push(j, &e);
        *last_ms = ms;
        return;
    }
    if (n < 3) return;
    uint32_t wid = (uint32_t)strtoul(f[2], NULL, 10);
    if (!wid) return;

    JournalGeom g = {0};
    bool has_geom = n >= 9;
    if (has_geom) {
        g.desktop = atoi(f[3]);
        g.x = atoi(f[4]); g.y = atoi(f[5]); g.w = atoi(f[6]); g.h = atoi(f[7]);
        g.flags = (unsigned)strtoul(f[8], NULL, 10);
    }

    JournalWindow *w = window_find(j, wid);
    switch (kind) {
    case JOURNAL_OPEN:
        if (!has_geom || n < 11) return;
        if (!w) w = window_add(j, wid);
        w->opened_ms = ms;
        w->closed_ms = 0;
        clean_copy(w->app_id, sizeof(w->app_id), f[9]);
        clean_copy(w->title, sizeof(w->title), f[10]);
        break;
    case JOURNAL_CLOSE:
        if (w) w->closed_ms = ms;
        break;
    case JOURNAL_TITLE:
        if (w && n >= 4) clean_copy(w->title, sizeof(w->title), f[3]);
        break;
    default:
        break;
    }
    JournalEvent e = make_event(ms, wid, (JournalKind)kind, has_geom ? &g : NULL);
    event_push(j, &e);
    if (ms > *last_ms) *last_ms = ms;
}

static void replay_file(FwmJournal *j) {
    FILE *f = fopen(j->path, "r");
    if (!f) return;
    char line[LINE_MAX_BYTES * 2];
    int64_t last_ms = 0;
    while (fgets(line, sizeof(line), f)) {
        /* A line longer than the buffer is not one this file writes: skip the
         * rest of it rather than reading its tail as a line of its own. */
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        replay_line(j, line, &last_ms);
    }
    fclose(f);
    /* Whatever is still open was open when the last run ended. */
    close_orphans(j, INT64_MAX, last_ms);
}

/* ── public ───────────────────────────────────────────────────────────── */

FwmJournal *journal_open_sized(const char *path, int64_t now_ms,
                               int events, int windows, size_t file_max) {
    FwmJournal *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->ev_cap = events > 0 ? events : 1;
    j->win_cap = windows > 0 ? windows : 1;
    j->ev = calloc((size_t)j->ev_cap, sizeof(*j->ev));
    j->win = calloc((size_t)j->win_cap, sizeof(*j->win));
    j->next_wid = 1;
    j->fd = -1;
    j->file_max = file_max;
    if (!j->ev || !j->win) {
        free(j->ev);
        free(j->win);
        free(j);
        return NULL;
    }

    if (path && path[0]) {
        j->path = strdup(path);
        if (j->path) {
            replay_file(j);
            j->fd = open(j->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
            struct stat st;
            if (j->fd >= 0 && fstat(j->fd, &st) == 0) j->file_bytes = (size_t)st.st_size;
        }
    }

    JournalEvent e = make_event(now_ms, 0, JOURNAL_START, NULL);
    event_push(j, &e);
    event_write(j, &e);
    return j;
}

FwmJournal *journal_open(const char *path, int64_t now_ms) {
    return journal_open_sized(path, now_ms, JOURNAL_EVENTS, JOURNAL_WINDOWS,
                              JOURNAL_FILE_MAX);
}

void journal_close(FwmJournal *j) {
    if (!j) return;
    if (j->fd >= 0) close(j->fd);
    free(j->path);
    free(j->ev);
    free(j->win);
    free(j);
}

uint32_t journal_window_open(FwmJournal *j, int64_t ms, const char *app_id,
                             const char *title, const JournalGeom *g) {
    if (!j) return 0;
    JournalWindow *w = window_add(j, j->next_wid);
    w->opened_ms = ms;
    clean_copy(w->app_id, sizeof(w->app_id), app_id);
    clean_copy(w->title, sizeof(w->title), title);
    JournalEvent e = make_event(ms, w->wid, JOURNAL_OPEN, g);
    event_push(j, &e);
    event_write(j, &e);
    return w->wid;
}

void journal_window_close(FwmJournal *j, int64_t ms, uint32_t wid) {
    if (!j || !wid) return;
    JournalWindow *w = window_find(j, wid);
    if (w) w->closed_ms = ms;
    JournalEvent e = make_event(ms, wid, JOURNAL_CLOSE, NULL);
    event_push(j, &e);
    event_write(j, &e);
}

void journal_window_title(FwmJournal *j, int64_t ms, uint32_t wid, const char *title) {
    if (!j || !wid) return;
    JournalWindow *w = window_find(j, wid);
    if (!w) return;
    char clean[JOURNAL_TITLE_MAX];
    clean_copy(clean, sizeof(clean), title);
    if (strcmp(clean, w->title) == 0) return;   /* said again, not changed */
    memcpy(w->title, clean, sizeof(clean));
    JournalEvent e = make_event(ms, wid, JOURNAL_TITLE, NULL);
    event_push(j, &e);
    event_write(j, &e);
}

void journal_record(FwmJournal *j, int64_t ms, uint32_t wid, JournalKind kind,
                    const JournalGeom *g) {
    if (!j || !wid) return;
    if (kind != JOURNAL_FOCUS && kind != JOURNAL_MOVE && kind != JOURNAL_REST
        && kind != JOURNAL_THROW) return;
    JournalEvent e = make_event(ms, wid, kind, g);
    event_push(j, &e);
    event_write(j, &e);
}

int journal_count(const FwmJournal *j) {
    return j ? j->ev_n : 0;
}

const JournalEvent *journal_at(const FwmJournal *j, int i) {
    if (!j || i < 0 || i >= j->ev_n) return NULL;
    return &j->ev[(j->ev_start + i) % j->ev_cap];
}

const JournalWindow *journal_window(const FwmJournal *j, uint32_t wid) {
    if (!j || !wid) return NULL;
    return window_find((FwmJournal *)j, wid);
}

int journal_window_count(const FwmJournal *j) {
    return j ? j->win_n : 0;
}

const JournalWindow *journal_window_at(const FwmJournal *j, int i) {
    if (!j || i < 0 || i >= j->win_n) return NULL;
    return &j->win[(j->win_start + i) % j->win_cap];
}

uint64_t journal_version(const FwmJournal *j) {
    return j ? j->version : 0;
}

uint64_t journal_list_version(const FwmJournal *j) {
    return j ? j->list_version : 0;
}

size_t journal_file_bytes(const FwmJournal *j) {
    return j ? j->file_bytes : 0;
}
