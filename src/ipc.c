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

/* accept4() and the SOCK_* creation flags are GNU extensions; the project
 * builds with -std=c11, which hides them without this. */
#define _GNU_SOURCE

#include "ipc.h"
#include "server.h"
#include "view.h"
#include "physics.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

/* One request line in, one JSON reply out, then the server closes — unless the
 * request was `subscribe`, which keeps the connection open and streams events
 * down it instead.
 *
 * What makes both safe to bolt onto the compositor's own event loop is that
 * nothing here ever blocks on a client. Writes go through an outbound queue
 * drained on WL_EVENT_WRITABLE, and a subscriber that stops reading until the
 * queue passes IPC_MAX_BACKLOG is dropped rather than allowed to stall the
 * compositor: a wedged client can never hold compositor state hostage. */

#define IPC_MAX_REQUEST  4096          /* a request longer than this is malformed */
#define IPC_MAX_CLIENTS  16            /* cheap ceiling; replies are immediate */
#define IPC_MAX_BACKLOG  (256 * 1024)  /* unread bytes before a subscriber is dropped */

struct FwmIpc {
    struct FwmServer *server;
    int fd;
    char path[108];             /* sun_path is 108 bytes on Linux */
    struct wl_event_source *source;
    struct wl_list clients;
    uint32_t subscribed;        /* union of every client's mask; 0 = emit nothing */

    /* The client whose command is currently running, if any. A `dispatch` sent
     * down a subscription emits events while that client is still on the
     * stack, and if its own backlog is what overflows, freeing it here would
     * pull the ground out from under the caller. It is marked instead and
     * reaped by the read path once the command has returned. */
    struct IpcClient *current;
};

struct IpcClient {
    struct wl_list link;
    FwmIpc *ipc;
    int fd;
    struct wl_event_source *source;
    char buf[IPC_MAX_REQUEST];
    size_t len;

    uint32_t events;            /* subscribed event mask; 0 = not a subscriber */
    bool closing;               /* reply written, close once the queue drains */
    bool dead;                  /* doomed; freed as soon as it is safe to */
    char *out;                  /* queued outbound bytes */
    size_t out_len, out_cap;
};

/* ── reply builder ────────────────────────────────────────────────────── */

struct Buf {
    char *data;
    size_t len, cap;
};

static void buf_append(struct Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return;         /* out of memory: reply is truncated, not fatal */
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_puts(struct Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

static void buf_printf(struct Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_append(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* Window titles are arbitrary client-controlled text and go straight into the
 * reply — anything that would break the JSON (or a consumer's parser) has to
 * be escaped here, control characters included. */
static void buf_json_string(struct Buf *b, const char *s) {
    buf_puts(b, "\"");
    if (!s) { buf_puts(b, "\""); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n");  break;
        case '\r': buf_puts(b, "\\r");  break;
        case '\t': buf_puts(b, "\\t");  break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else           buf_append(b, (const char *)p, 1);
        }
    }
    buf_puts(b, "\"");
}

static void reply_error(struct Buf *b, const char *msg) {
    buf_puts(b, "{\"ok\":false,\"error\":");
    buf_json_string(b, msg);
    buf_puts(b, "}\n");
}

static void reply_ok(struct Buf *b) { buf_puts(b, "{\"ok\":true}\n"); }

/* ── outbound queue ───────────────────────────────────────────────────── */

/* Declared here because the write path and the command handlers are mutually
 * recursive through `subscribe`: it replies, then keeps the client. */
struct IpcClient;
static void ipc_client_destroy(struct IpcClient *c);

/* Push whatever the socket will take right now. Returns false if the
 * connection is dead and the caller should drop the client. */
static bool ipc_client_flush(struct IpcClient *c) {
    while (c->out_len > 0) {
        /* MSG_NOSIGNAL: a client that hung up mid-write must not kill the
         * compositor with SIGPIPE. MSG_DONTWAIT: nor may it stall it. */
        ssize_t n = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            c->out_len -= (size_t)n;
            memmove(c->out, c->out + n, c->out_len);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return false;
    }

    /* Only ask for writability while there is something to write; otherwise
     * an idle subscriber would spin the event loop. */
    uint32_t mask = WL_EVENT_READABLE | (c->out_len ? WL_EVENT_WRITABLE : 0);
    if (c->source) wl_event_source_fd_update(c->source, mask);
    return true;
}

/* Queue `len` bytes, flushing as much as goes immediately. Returns false when
 * the client should be dropped: dead connection, or a subscriber so far behind
 * that keeping its backlog is no longer the compositor's problem to carry. */
static bool ipc_client_send(struct IpcClient *c, const char *data, size_t len) {
    if (len == 0) return true;

    if (c->out_len + len > IPC_MAX_BACKLOG) {
        wlr_log(WLR_INFO, "ipc: dropping client %d bytes behind", (int)c->out_len);
        return false;
    }
    if (c->out_len + len > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < c->out_len + len) cap *= 2;
        char *d = realloc(c->out, cap);
        if (!d) return false;   /* cannot queue it: dropping beats lying */
        c->out = d;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;

    return ipc_client_flush(c);
}

/* ── commands ─────────────────────────────────────────────────────────── */

/* Shared by cmd_state and the desktop/mode events, which must agree on how a
 * mode is spelled or a subscriber would see two vocabularies for one thing. */
static const char *mode_name(int m) {
    static const char *const names[] = { "physics", "tiling", "floating" };
    return (m >= 0 && m <= 2) ? names[m] : "?";
}

static void cmd_windows(FwmServer *server, struct Buf *b) {
    buf_puts(b, "{\"ok\":true,\"windows\":[");
    FwmView *view;
    bool first = true;
    wl_list_for_each(view, &server->views, link) {
        if (!first) buf_puts(b, ",");
        first = false;
        buf_printf(b, "{\"id\":%u,\"title\":", view->id);
        buf_json_string(b, view_title(view));
        buf_puts(b, ",\"app_id\":");
        buf_json_string(b, view_app_id(view));
        buf_printf(b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d",
                   view->x, view->y, view->width, view->height);
        buf_printf(b, ",\"desktop\":%d", server->screen_width > 0
                                         ? view->x / server->screen_width : 0);
        buf_printf(b, ",\"focused\":%s", view == server->focused_view ? "true" : "false");
        /* Physics flags: the only way to see whether a [[rule]] (or a manual
         * toggle) actually took hold on this window. */
        PhysicsBody *body = physics_find_body(&server->physics, view->id);
        buf_printf(b, ",\"pinned\":%s", body && body->pinned ? "true" : "false");
        buf_printf(b, ",\"nocollide\":%s", body && body->no_collide ? "true" : "false");
        buf_printf(b, ",\"xwayland\":%s}", view->type == FWM_VIEW_XDG ? "false" : "true");
    }
    buf_puts(b, "]}\n");
}

static void cmd_state(FwmServer *server, struct Buf *b) {
    int count = wl_list_length(&server->views);
    int desktop = server->screen_width > 0
                  ? (server->camera_x + server->screen_width / 2) / server->screen_width : 0;
    buf_puts(b, "{\"ok\":true,");
    buf_printf(b, "\"desktop\":%d,\"camera_x\":%d,\"windows\":%d,",
               desktop, server->camera_x, count);
    buf_printf(b, "\"screen_width\":%d,\"screen_height\":%d,",
               server->screen_width, server->screen_height);
    buf_printf(b, "\"gravity\":%.3f,\"locked\":%s,",
               server->physics.gravity_scale, server->locked ? "true" : "false");

    /* Per-desktop mode: nothing else exposes it, and with three modes "which
     * one am I in" stops being guessable from how the windows look. */
    buf_puts(b, "\"mode\":");
    buf_json_string(b, mode_name(server->desktop_mode[desktop]));
    buf_puts(b, ",\"modes\":[");
    for (int d = 0; d < 10; d++) {
        if (d) buf_puts(b, ",");
        buf_json_string(b, mode_name(server->desktop_mode[d]));
    }
    buf_puts(b, "],");
    buf_puts(b, "\"focused\":");
    buf_json_string(b, server->focused_view ? view_title(server->focused_view) : "");
    buf_puts(b, "}\n");
}

/* Every runtime-settable option with its current value, so `fwmctl config`
 * documents itself rather than needing the list kept in sync by hand. */
static void cmd_config(FwmServer *server, struct Buf *b) {
    int n;
    const ConfigOption *opts = config_options(&n);

    buf_puts(b, "{\"ok\":true,\"options\":[");
    for (int i = 0; i < n; i++) {
        char value[64];
        config_option_get(&server->config, &opts[i], value, sizeof(value));

        if (i) buf_puts(b, ",");
        buf_puts(b, "{\"name\":");
        buf_json_string(b, opts[i].name);
        buf_puts(b, ",\"value\":");
        buf_json_string(b, value);
        buf_puts(b, ",\"help\":");
        buf_json_string(b, opts[i].help);
        if (opts[i].type != CFG_OPT_COLOR)
            buf_printf(b, ",\"min\":%g,\"max\":%g", opts[i].min, opts[i].max);
        buf_puts(b, "}");
    }
    buf_puts(b, "]}\n");
}

static void cmd_get(FwmServer *server, const char *name, struct Buf *b) {
    const ConfigOption *opt = config_option_find(name);
    if (!opt) { reply_error(b, "unknown option (try: config)"); return; }

    char value[64];
    config_option_get(&server->config, opt, value, sizeof(value));
    buf_puts(b, "{\"ok\":true,\"name\":");
    buf_json_string(b, opt->name);
    buf_puts(b, ",\"value\":");
    buf_json_string(b, value);
    buf_puts(b, "}\n");
}

/* `set <name> <value>` — runtime only. config.toml is never rewritten, so
 * reload_config (super+shift+r) is the documented way back to the file. */
static void cmd_set(FwmServer *server, const char *arg, struct Buf *b) {
    const char *sep = arg ? strchr(arg, ' ') : NULL;
    if (!sep) { reply_error(b, "set needs <name> <value>"); return; }

    char name[64];
    size_t namelen = (size_t)(sep - arg);
    if (namelen >= sizeof(name)) { reply_error(b, "option name too long"); return; }
    memcpy(name, arg, namelen);
    name[namelen] = '\0';

    while (*sep == ' ') sep++;

    const ConfigOption *opt = config_option_find(name);
    if (!opt) { reply_error(b, "unknown option (try: config)"); return; }

    char err[192];
    if (!config_option_set(&server->config, opt, sep, err, sizeof(err))) {
        reply_error(b, err);
        return;
    }
    /* Same re-apply path a reload uses, minus the wallpaper image decode. */
    server_apply_config(server, 0);

    char value[64];
    config_option_get(&server->config, opt, value, sizeof(value));
    buf_puts(b, "{\"ok\":true,\"name\":");
    buf_json_string(b, opt->name);
    buf_puts(b, ",\"value\":");
    buf_json_string(b, value);
    buf_puts(b, "}\n");
}

/* Recomputed from the live clients rather than accumulated, so unsubscribing
 * by disconnecting actually stops the work of building those events. */
static void ipc_refresh_subscriptions(FwmIpc *ipc) {
    uint32_t mask = 0;
    struct IpcClient *c;
    wl_list_for_each(c, &ipc->clients, link) mask |= c->events;
    ipc->subscribed = mask;
}

/* `subscribe [events]` — everything, or a comma/space separated subset. The
 * reply names what was actually subscribed, so a client can log it instead of
 * assuming its request was understood. */
static void cmd_subscribe(struct IpcClient *c, const char *arg, struct Buf *b) {
    uint32_t mask = 0;
    char err[320];
    if (!fwm_ipc_events_parse(arg, &mask, err, sizeof err)) {
        reply_error(b, err);
        return;
    }

    /* A second subscribe widens the set rather than replacing it: the two
     * readings differ only for a client that asked twice, and adding is the
     * one that never silently unsubscribes it from the first request. */
    c->events |= mask;
    ipc_refresh_subscriptions(c->ipc);

    buf_puts(b, "{\"ok\":true,\"events\":[");
    bool first = true;
    for (int i = 0; i < FWM_EV_COUNT; i++) {
        uint32_t bit = 1u << i;
        if (!(c->events & bit)) continue;
        if (!first) buf_puts(b, ",");
        first = false;
        buf_json_string(b, fwm_ipc_event_name(bit));
    }
    buf_puts(b, "]}\n");
}

static void ipc_handle_command(struct IpcClient *client, const char *line, struct Buf *out) {
    FwmIpc *ipc = client->ipc;
    FwmServer *server = ipc->server;

    /* Split off the first word. */
    const char *arg = strchr(line, ' ');
    size_t cmdlen = arg ? (size_t)(arg - line) : strlen(line);
    while (arg && *arg == ' ') arg++;
    if (arg && !*arg) arg = NULL;

    #define IS(name) (cmdlen == strlen(name) && strncmp(line, name, cmdlen) == 0)

    if (IS("version")) {
        buf_puts(out, "{\"ok\":true,\"version\":\"fwm ipc 1\"}\n");
        return;
    }
    if (IS("state"))   { cmd_state(server, out);   return; }
    if (IS("windows")) { cmd_windows(server, out); return; }
    if (IS("config"))  { cmd_config(server, out);  return; }
    if (IS("get")) {
        if (!arg) { reply_error(out, "get needs an option name"); return; }
        cmd_get(server, arg, out);
        return;
    }
    /* Read-only, so it sits with the commands above the lock check: a
     * subscriber learns nothing a `windows` poll would not already tell it. */
    if (IS("subscribe")) { cmd_subscribe(client, arg, out); return; }

    /* Everything below CHANGES state. The lock screen's whole promise is that
     * a locked session accepts no input — routing binds through a socket
     * instead of the keyboard must not become the hole in it. */
    if (server->locked) {
        reply_error(out, "session is locked");
        return;
    }

    if (IS("dispatch")) {
        if (!arg) { reply_error(out, "dispatch needs an action"); return; }
        server_dispatch_action_external(server, arg);
        reply_ok(out);
        return;
    }
    if (IS("set")) {
        cmd_set(server, arg, out);
        return;
    }
    if (IS("reload")) {
        server_reload_config(server);
        reply_ok(out);
        return;
    }

    reply_error(out, "unknown command (try: version, state, windows, config, get, set, "
                     "dispatch, reload, subscribe)");
    #undef IS
}

/* ── event emission ───────────────────────────────────────────────────── */

/* Fan one built line out to everyone who asked for that event.
 *
 * A client that cannot take it is condemned rather than freed on the spot:
 * this can run underneath that very client's own `dispatch`, and ipc->current
 * is the one whose memory the caller is still standing on. Clearing `events`
 * takes it out of every later broadcast, so a doomed client costs nothing
 * while it waits to be reaped. */
static void ipc_broadcast(FwmIpc *ipc, uint32_t event, struct Buf *b) {
    if (!b->data) return;

    struct IpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &ipc->clients, link) {
        if (c->dead || !(c->events & event)) continue;
        if (ipc_client_send(c, b->data, b->len)) continue;

        c->dead = true;
        c->events = 0;
        if (c != ipc->current) ipc_client_destroy(c);
    }
    ipc_refresh_subscriptions(ipc);
}

/* True when the event is worth building at all. */
static bool ipc_wants(FwmIpc *ipc, uint32_t event) {
    return ipc && (ipc->subscribed & event);
}

void ipc_emit_window(FwmIpc *ipc, uint32_t event, struct FwmView *view) {
    if (!ipc_wants(ipc, event)) return;

    FwmServer *server = ipc->server;
    struct Buf b = {0};

    buf_puts(&b, "{\"event\":");
    buf_json_string(&b, fwm_ipc_event_name(event));

    /* Focus genuinely goes nowhere when the last window on a desktop closes,
     * and a subscriber has to be able to tell that from "window 0". */
    if (!view) {
        buf_puts(&b, ",\"id\":null}\n");
    } else {
        buf_printf(&b, ",\"id\":%u,\"title\":", view->id);
        buf_json_string(&b, view_title(view));
        buf_puts(&b, ",\"app_id\":");
        buf_json_string(&b, view_app_id(view));
        buf_printf(&b, ",\"desktop\":%d",
                   server->screen_width > 0 ? view->x / server->screen_width : 0);
        buf_puts(&b, "}\n");
    }

    ipc_broadcast(ipc, event, &b);
    free(b.data);
}

void ipc_emit_desktop(FwmIpc *ipc, int desktop) {
    if (!ipc_wants(ipc, FWM_EV_DESKTOP)) return;

    struct Buf b = {0};
    buf_printf(&b, "{\"event\":\"desktop\",\"desktop\":%d,\"mode\":", desktop);
    buf_json_string(&b, mode_name(desktop >= 0 && desktop < 10
                                  ? ipc->server->desktop_mode[desktop] : -1));
    buf_puts(&b, "}\n");
    ipc_broadcast(ipc, FWM_EV_DESKTOP, &b);
    free(b.data);
}

void ipc_emit_mode(FwmIpc *ipc, int desktop, int mode) {
    if (!ipc_wants(ipc, FWM_EV_MODE)) return;

    struct Buf b = {0};
    buf_printf(&b, "{\"event\":\"mode\",\"desktop\":%d,\"mode\":", desktop);
    buf_json_string(&b, mode_name(mode));
    buf_puts(&b, "}\n");
    ipc_broadcast(ipc, FWM_EV_MODE, &b);
    free(b.data);
}

void ipc_emit_gravity(FwmIpc *ipc, double gravity_scale) {
    if (!ipc_wants(ipc, FWM_EV_GRAVITY)) return;

    struct Buf b = {0};
    buf_printf(&b, "{\"event\":\"gravity\",\"gravity\":%.3f}\n", gravity_scale);
    ipc_broadcast(ipc, FWM_EV_GRAVITY, &b);
    free(b.data);
}

void ipc_emit_config_reload(FwmIpc *ipc) {
    if (!ipc_wants(ipc, FWM_EV_CONFIG_RELOAD)) return;

    struct Buf b = {0};
    buf_puts(&b, "{\"event\":\"config_reload\"}\n");
    ipc_broadcast(ipc, FWM_EV_CONFIG_RELOAD, &b);
    free(b.data);
}

/* ── connection handling ──────────────────────────────────────────────── */

static void ipc_client_destroy(struct IpcClient *c) {
    wl_list_remove(&c->link);
    if (c->source) wl_event_source_remove(c->source);
    close(c->fd);
    free(c->out);
    free(c);
}

/* Run one complete request line and queue its reply. Returns false when the
 * client is finished with and should be destroyed. */
static bool ipc_client_line(struct IpcClient *c, char *line) {
    /* Tolerate CRLF so `echo` from any shell works. */
    size_t l = strlen(line);
    if (l && line[l - 1] == '\r') line[l - 1] = '\0';

    struct Buf out = {0};

    /* The command may emit events — including to this very client — so the
     * broadcast path has to know not to free what we are standing on. */
    c->ipc->current = c;
    ipc_handle_command(c, line, &out);
    c->ipc->current = NULL;

    bool alive = !c->dead;
    if (alive && out.data) alive = ipc_client_send(c, out.data, out.len);
    free(out.data);
    if (!alive) return false;

    /* A subscriber stays for the stream; everyone else gets the one reply and
     * goes. The close waits for the queue to drain, because a large reply
     * (`config`, `windows` with many windows) can exceed what the socket takes
     * in one go and closing on top of that would truncate it. */
    if (c->events) return true;
    c->closing = true;
    return c->out_len > 0;
}

static int ipc_client_event(int fd, uint32_t mask, void *data) {
    struct IpcClient *c = data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        ipc_client_destroy(c);
        return 0;
    }

    if (mask & WL_EVENT_WRITABLE) {
        if (!ipc_client_flush(c) || (c->closing && c->out_len == 0)) {
            ipc_client_destroy(c);
            return 0;
        }
        ipc_refresh_subscriptions(c->ipc);
    }

    if (!(mask & WL_EVENT_READABLE)) return 0;

    /* Already answered and just waiting to drain: nothing more to read. */
    if (c->closing) return 0;

    ssize_t n = read(fd, c->buf + c->len, sizeof(c->buf) - c->len - 1);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        ipc_client_destroy(c);
        return 0;
    }
    c->len += (size_t)n;
    c->buf[c->len] = '\0';

    /* A subscriber may keep issuing commands down the same connection, so run
     * every complete line the read produced, not just the first. */
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (!nl) {
            /* No line yet. A request that fills the buffer without one is junk. */
            if (c->len >= sizeof(c->buf) - 1) ipc_client_destroy(c);
            return 0;
        }
        *nl = '\0';

        /* Consume the line before running it: the handler can queue output, and
         * on the error paths below `c` is gone and must not be touched again. */
        size_t used = (size_t)(nl - c->buf) + 1;
        char line[IPC_MAX_REQUEST];
        memcpy(line, c->buf, used);           /* includes the NUL we just wrote */
        c->len -= used;
        memmove(c->buf, c->buf + used, c->len);
        c->buf[c->len] = '\0';

        if (!ipc_client_line(c, line)) {
            ipc_client_destroy(c);
            return 0;
        }
        ipc_refresh_subscriptions(c->ipc);
        if (c->closing) return 0;             /* draining; ignore the rest */
    }
}

static int ipc_listen_readable(int fd, uint32_t mask, void *data) {
    FwmIpc *ipc = data;
    (void)mask;

    int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) return 0;

    if (wl_list_length(&ipc->clients) >= IPC_MAX_CLIENTS) {
        close(cfd);
        return 0;
    }

    struct IpcClient *c = calloc(1, sizeof(*c));
    if (!c) { close(cfd); return 0; }
    c->ipc = ipc;
    c->fd = cfd;
    wl_list_insert(&ipc->clients, &c->link);

    struct wl_event_loop *el = wl_display_get_event_loop(ipc->server->wl_display);
    c->source = wl_event_loop_add_fd(el, cfd, WL_EVENT_READABLE, ipc_client_event, c);
    if (!c->source) ipc_client_destroy(c);
    return 0;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

FwmIpc *ipc_create(struct FwmServer *server, const char *wl_socket) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) dir = "/tmp";

    FwmIpc *ipc = calloc(1, sizeof(*ipc));
    if (!ipc) return NULL;
    ipc->server = server;
    ipc->fd = -1;
    wl_list_init(&ipc->clients);

    int n = snprintf(ipc->path, sizeof(ipc->path), "%s/fwm-%s.sock", dir, wl_socket);
    if (n < 0 || (size_t)n >= sizeof(ipc->path)) {
        wlr_log(WLR_ERROR, "ipc: socket path too long, control socket disabled");
        free(ipc);
        return NULL;
    }

    ipc->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ipc->fd < 0) {
        wlr_log(WLR_ERROR, "ipc: socket() failed: %s", strerror(errno));
        free(ipc);
        return NULL;
    }

    /* A socket left behind by a crashed run would make bind() fail. Removing
     * it is safe: the path is per Wayland display, and that display name is
     * ours for as long as we run. */
    unlink(ipc->path);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, ipc->path, strlen(ipc->path) + 1);
    if (bind(ipc->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(ipc->fd, IPC_MAX_CLIENTS) < 0) {
        wlr_log(WLR_ERROR, "ipc: bind/listen on %s failed: %s", ipc->path, strerror(errno));
        close(ipc->fd);
        free(ipc);
        return NULL;
    }

    struct wl_event_loop *el = wl_display_get_event_loop(server->wl_display);
    ipc->source = wl_event_loop_add_fd(el, ipc->fd, WL_EVENT_READABLE,
                                       ipc_listen_readable, ipc);
    if (!ipc->source) {
        wlr_log(WLR_ERROR, "ipc: could not watch the control socket");
        close(ipc->fd);
        unlink(ipc->path);
        free(ipc);
        return NULL;
    }

    /* Children inherit this, so a script spawned from a bind can talk back to
     * the compositor that started it without guessing the path. */
    setenv("FWM_SOCKET", ipc->path, 1);
    wlr_log(WLR_INFO, "ipc: listening on %s", ipc->path);
    return ipc;
}

void ipc_destroy(FwmIpc *ipc) {
    if (!ipc) return;
    struct IpcClient *c, *tmp;
    wl_list_for_each_safe(c, tmp, &ipc->clients, link) ipc_client_destroy(c);
    if (ipc->source) wl_event_source_remove(ipc->source);
    if (ipc->fd >= 0) close(ipc->fd);
    unlink(ipc->path);
    free(ipc);
}
