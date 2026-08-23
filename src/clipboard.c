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

/* The selection: offering fwm's own bytes, and keeping a client's after the
 * client is gone. See clipboard.h for what the two have to do with each other.
 */

#include "clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

/* The names the same UTF-8 text goes by. Read in this order — the first one a
 * source offers is the one asked for — and offered in this order too.
 *
 * All five are the same bytes: the X11 names are here because XWayland clients
 * ask for them by those names, and a paste into an X11 application that only
 * knows UTF8_STRING is exactly the paste this feature exists for. */
static const char *const TEXT_MIMES[] = {
    "text/plain;charset=utf-8",
    "UTF8_STRING",
    "text/plain",
    "STRING",
    "TEXT",
};
#define TEXT_MIME_COUNT ((int)(sizeof(TEXT_MIMES) / sizeof(TEXT_MIMES[0])))

/* ── offering bytes ──────────────────────────────────────────────────── */

typedef struct {
    struct wlr_data_source base;
    struct wl_event_loop *loop;
    unsigned char *data;
    size_t len;
    FwmClipboard *cb;      /* NULL once the clipboard outlives this source */
} ClipSource;

typedef struct {
    int fd;
    unsigned char *data;
    size_t len, sent;
    struct wl_event_source *src;
} ClipXfer;

struct FwmClipboard {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_seat *seat;

    struct wl_listener set_selection;

    bool   persist;
    size_t max_bytes;

    /* The last text a client copied, kept for after that client is gone. */
    unsigned char *saved;
    size_t saved_len;

    /* A read of a client's selection, in flight. One at a time: a second copy
     * makes the first one pointless before it finishes. */
    struct ClipRead *read;

    ClipSource *ours;   /* the source we put on the seat, while it is there */
};

static void xfer_finish(ClipXfer *x) {
    if (x->src) wl_event_source_remove(x->src);
    close(x->fd);
    free(x->data);
    free(x);
}

/* write(2) to a pipe whose reader has gone raises SIGPIPE, and the default
 * disposition of that would take the display server down with it — the same
 * trap ipc.c avoids with MSG_NOSIGNAL, which a pipe has no equivalent for.
 * Block it for the length of the call and eat the one we caused, rather than
 * ignoring SIGPIPE process-wide: that disposition survives exec, and every
 * application fwm spawns would inherit it. */
static ssize_t write_nosigpipe(int fd, const void *buf, size_t len) {
    sigset_t pipe_only, prev;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_only, &prev);

    ssize_t n = write(fd, buf, len);
    int err = errno;
    if (n < 0 && err == EPIPE) {
        /* Ours, and pending on this thread: take it off before unblocking. */
        struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_only, NULL, &zero) >= 0) {}
    }

    pthread_sigmask(SIG_SETMASK, &prev, NULL);
    errno = err;
    return n;
}

static int xfer_writable(int fd, uint32_t mask, void *data) {
    ClipXfer *x = data;
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        xfer_finish(x);
        return 0;
    }
    while (x->sent < x->len) {
        ssize_t n = write_nosigpipe(fd, x->data + x->sent, x->len - x->sent);
        if (n > 0) {
            x->sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;  /* later */
        break;   /* the reader gave up mid-paste; nothing to be done about it */
    }
    xfer_finish(x);
    return 0;
}

static void source_send(struct wlr_data_source *source, const char *mime_type,
                        int32_t fd) {
    ClipSource *s = (ClipSource *)source;
    (void)mime_type;   /* every mime we offer is the same bytes; see TEXT_MIMES */

    ClipXfer *x = s->data ? calloc(1, sizeof(*x)) : NULL;
    if (!x) { close(fd); return; }

    x->data = malloc(s->len);
    if (!x->data) { free(x); close(fd); return; }
    memcpy(x->data, s->data, s->len);
    x->len = s->len;
    x->fd  = fd;

    /* Non-blocking, because the writer above is only allowed to write what
     * fits right now. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    x->src = wl_event_loop_add_fd(s->loop, fd, WL_EVENT_WRITABLE, xfer_writable, x);
    if (!x->src) { free(x->data); free(x); close(fd); }
}

static void source_destroy(struct wlr_data_source *source) {
    ClipSource *s = (ClipSource *)source;
    /* Transfers in flight are NOT touched: each owns its own copy of the bytes,
     * so a paste survives the next thing copied replacing the selection. */
    if (s->cb && s->cb->ours == s) s->cb->ours = NULL;
    free(s->data);
    free(s);
}

static const struct wlr_data_source_impl clip_source_impl = {
    .send = source_send,
    .destroy = source_destroy,
};

bool clipboard_offer(FwmClipboard *cb, const char *const *mimes, int mime_count,
                     unsigned char *data, size_t len) {
    if (!cb || !data || mime_count <= 0) { free(data); return false; }

    ClipSource *s = calloc(1, sizeof(*s));
    if (!s) { free(data); return false; }
    wlr_data_source_init(&s->base, &clip_source_impl);
    s->loop = cb->loop;
    s->data = data;
    s->len  = len;
    s->cb   = cb;

    for (int i = 0; i < mime_count; i++) {
        char **slot = wl_array_add(&s->base.mime_types, sizeof(char *));
        if (!slot || !(*slot = strdup(mimes[i]))) {
            wlr_data_source_destroy(&s->base);   /* frees the bytes with it */
            return false;
        }
    }

    cb->ours = s;
    wlr_seat_set_selection(cb->seat, &s->base,
                           wl_display_next_serial(cb->display));
    return true;
}

/* ── keeping a client's text ─────────────────────────────────────────── */

typedef struct ClipRead {
    FwmClipboard *cb;
    int fd;
    unsigned char *buf;
    size_t len, cap;
    struct wl_event_source *src;
} ClipRead;

static void read_cancel(FwmClipboard *cb) {
    ClipRead *r = cb->read;
    if (!r) return;
    cb->read = NULL;
    if (r->src) wl_event_source_remove(r->src);
    close(r->fd);
    free(r->buf);
    free(r);
}

static void saved_drop(FwmClipboard *cb) {
    free(cb->saved);
    cb->saved = NULL;
    cb->saved_len = 0;
}

static int read_readable(int fd, uint32_t mask, void *data) {
    ClipRead *r = data;
    FwmClipboard *cb = r->cb;

    for (;;) {
        if (r->len == r->cap) {
            size_t want = r->cap ? r->cap * 2 : 4096;
            if (want > cb->max_bytes) want = cb->max_bytes;
            if (want == r->cap) {
                /* Bigger than [clipboard] max_kb. Dropped rather than
                 * truncated: half a copied file is not a smaller copy of it,
                 * it is wrong text that looks right. The client is alive and
                 * still owns the selection — nothing about the paste changes
                 * until it dies, which is the only case this feature is for. */
                wlr_log(WLR_INFO, "clipboard: selection over max_kb — not kept");
                read_cancel(cb);
                return 0;
            }
            unsigned char *grown = realloc(r->buf, want);
            if (!grown) { read_cancel(cb); return 0; }
            r->buf = grown;
            r->cap = want;
        }

        ssize_t n = read(fd, r->buf + r->len, r->cap - r->len);
        if (n > 0) { r->len += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Nothing more yet. HANGUP without readable data is the end. */
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) break;
            return 0;
        }
        break;   /* EOF, or a pipe that broke: take what arrived */
    }

    /* Empty is not worth keeping: a client that offered text and sent none
     * leaves the clipboard exactly as useful as no clipboard. */
    if (r->len > 0) {
        saved_drop(cb);
        cb->saved = r->buf;
        cb->saved_len = r->len;
        r->buf = NULL;
        wlr_log(WLR_DEBUG, "clipboard: holding %zu bytes for after the window goes",
                cb->saved_len);
    }
    read_cancel(cb);
    return 0;
}

/* Ask `source` for its text and start reading it into memory.
 *
 * The read happens NOW, while the client is alive, not when it dies — by then
 * there is nobody left to answer. That is the whole shape of the feature: one
 * pipe per copy, drained by the event loop like everything else. */
static void read_start(FwmClipboard *cb, struct wlr_data_source *source,
                       const char *mime) {
    int fds[2];
    if (pipe(fds) < 0) return;

    /* Non-blocking on our end: the client writes when it feels like it. */
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    ClipRead *r = calloc(1, sizeof(*r));
    if (!r) { close(fds[0]); close(fds[1]); return; }
    r->cb = cb;
    r->fd = fds[0];

    /* wlr_data_source_send closes the write end for us. */
    wlr_data_source_send(source, mime, fds[1]);

    r->src = wl_event_loop_add_fd(cb->loop, r->fd, WL_EVENT_READABLE,
                                  read_readable, r);
    if (!r->src) { close(r->fd); free(r); return; }
    cb->read = r;
}

/* Does this source offer `mime`? */
static bool source_offers(struct wlr_data_source *source, const char *mime) {
    char **m;
    wl_array_for_each(m, &source->mime_types)
        if (strcmp(*m, mime) == 0) return true;
    return false;
}

/* Put the kept text back on the seat, under every name it goes by. */
static void restore(FwmClipboard *cb) {
    unsigned char *copy = malloc(cb->saved_len);
    if (!copy) return;
    memcpy(copy, cb->saved, cb->saved_len);
    clipboard_offer(cb, TEXT_MIMES, TEXT_MIME_COUNT, copy, cb->saved_len);
}

static void handle_set_selection(struct wl_listener *listener, void *data) {
    FwmClipboard *cb = wl_container_of(listener, cb, set_selection);
    (void)data;

    struct wlr_data_source *source = cb->seat->selection_source;

    /* Ours: either the screenshot's PNG or the text we just put back. Reading
     * our own bytes back out through a pipe would be pointless, and restoring
     * on top of a restore is how you write an infinite loop. */
    if (source && source->impl == &clip_source_impl) return;

    if (!cb->persist) return;

    if (source) {
        /* A new client owns the clipboard. Whatever was kept describes a
         * selection nobody can reach any more, so it goes now rather than
         * lingering as a stale paste waiting for the new owner to die. */
        read_cancel(cb);
        saved_drop(cb);

        for (int i = 0; i < TEXT_MIME_COUNT; i++) {
            if (!source_offers(source, TEXT_MIMES[i])) continue;
            read_start(cb, source, TEXT_MIMES[i]);
            break;
        }
        /* No text on offer (an image, a file drag): nothing kept, and the
         * selection behaves as it always did. */
        return;
    }

    /* The selection went empty, which on Wayland means one thing: the client
     * that owned it is gone. This is the moment the whole module exists for. */
    read_cancel(cb);
    if (cb->saved_len == 0) return;
    wlr_log(WLR_DEBUG, "clipboard: the window that copied is gone — offering its %zu bytes",
            cb->saved_len);
    restore(cb);
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

FwmClipboard *clipboard_create(struct wl_display *display, struct wlr_seat *seat) {
    if (!display || !seat) return NULL;
    FwmClipboard *cb = calloc(1, sizeof(*cb));
    if (!cb) return NULL;

    cb->display = display;
    cb->loop = wl_display_get_event_loop(display);
    cb->seat = seat;
    cb->persist = true;
    cb->max_bytes = 1024 * 1024;

    cb->set_selection.notify = handle_set_selection;
    wl_signal_add(&seat->events.set_selection, &cb->set_selection);
    return cb;
}

void clipboard_configure(FwmClipboard *cb, bool persist, size_t max_bytes) {
    if (!cb) return;
    cb->max_bytes = max_bytes;
    if (cb->persist == persist) return;
    cb->persist = persist;
    if (!persist) {
        read_cancel(cb);
        saved_drop(cb);
    }
}

void clipboard_destroy(FwmClipboard *cb) {
    if (!cb) return;
    wl_list_remove(&cb->set_selection.link);
    read_cancel(cb);
    saved_drop(cb);
    /* The source is the seat's to destroy, and it may outlive us by a moment
     * during shutdown — cut its way back here first. */
    if (cb->ours) cb->ours->cb = NULL;
    free(cb);
}
