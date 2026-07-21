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

/* One request line in, one JSON reply out, then the server closes. Keeping the
 * connection single-shot means no per-client state machine beyond reading a
 * line, which is what makes this safe to bolt onto the compositor's own event
 * loop: a wedged client can never hold compositor state hostage. */

#define IPC_MAX_REQUEST  4096   /* a request longer than this is malformed */
#define IPC_MAX_CLIENTS  16     /* cheap ceiling; replies are immediate */

struct FwmIpc {
    struct FwmServer *server;
    int fd;
    char path[108];             /* sun_path is 108 bytes on Linux */
    struct wl_event_source *source;
    struct wl_list clients;
};

struct IpcClient {
    struct wl_list link;
    FwmIpc *ipc;
    int fd;
    struct wl_event_source *source;
    char buf[IPC_MAX_REQUEST];
    size_t len;
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

/* ── commands ─────────────────────────────────────────────────────────── */

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
    static const char *mode_name[] = { "physics", "tiling", "floating" };
    int m = server->desktop_mode[desktop];
    buf_puts(b, "\"mode\":");
    buf_json_string(b, (m >= 0 && m <= 2) ? mode_name[m] : "?");
    buf_puts(b, ",\"modes\":[");
    for (int d = 0; d < 10; d++) {
        int dm = server->desktop_mode[d];
        if (d) buf_puts(b, ",");
        buf_json_string(b, (dm >= 0 && dm <= 2) ? mode_name[dm] : "?");
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

static void ipc_handle_command(FwmIpc *ipc, const char *line, struct Buf *out) {
    FwmServer *server = ipc->server;

    /* Split off the first word. */
    const char *arg = strchr(line, ' ');
    size_t cmdlen = arg ? (size_t)(arg - line) : strlen(line);
    while (arg && *arg == ' ') arg++;
    if (arg && !*arg) arg = NULL;

    #define IS(name) (cmdlen == strlen(name) && strncmp(line, name, cmdlen) == 0)

    if (IS("version")) {
        buf_puts(out, "{\"ok\":true,\"version\":\"fwm-wayland ipc 1\"}\n");
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

    reply_error(out, "unknown command (try: version, state, windows, config, get, set, dispatch, reload)");
    #undef IS
}

/* ── connection handling ──────────────────────────────────────────────── */

static void ipc_client_destroy(struct IpcClient *c) {
    wl_list_remove(&c->link);
    if (c->source) wl_event_source_remove(c->source);
    close(c->fd);
    free(c);
}

static int ipc_client_readable(int fd, uint32_t mask, void *data) {
    struct IpcClient *c = data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        ipc_client_destroy(c);
        return 0;
    }

    ssize_t n = read(fd, c->buf + c->len, sizeof(c->buf) - c->len - 1);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        ipc_client_destroy(c);
        return 0;
    }
    c->len += (size_t)n;
    c->buf[c->len] = '\0';

    char *nl = strchr(c->buf, '\n');
    if (!nl) {
        /* No line yet. A request that fills the buffer without one is junk. */
        if (c->len >= sizeof(c->buf) - 1) ipc_client_destroy(c);
        return 0;
    }
    *nl = '\0';
    /* Tolerate CRLF so `echo` from any shell works. */
    size_t l = strlen(c->buf);
    if (l && c->buf[l - 1] == '\r') c->buf[l - 1] = '\0';

    struct Buf out = {0};
    ipc_handle_command(c->ipc, c->buf, &out);
    if (out.data) {
        /* MSG_NOSIGNAL: a client that hung up mid-reply must not kill the
         * compositor with SIGPIPE. Partial writes are not retried — the reply
         * is small and the connection is single-shot. */
        ssize_t unused = send(fd, out.data, out.len, MSG_NOSIGNAL);
        (void)unused;
        free(out.data);
    }
    ipc_client_destroy(c);
    return 0;
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
    c->source = wl_event_loop_add_fd(el, cfd, WL_EVENT_READABLE, ipc_client_readable, c);
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
