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
#include "theme.h"
#include "view.h"
#include "physics.h"
#include "group.h"
#include "urgent.h"
/* For the settings overlay and the window verbs: acting on one window by id is
 * the same work a keybind does, and it goes through the same functions. */
#include "server_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
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

    /* The palette as it stood when the `palette` event last went out. Every
     * `fwmctl set` re-applies the config and so rebuilds the theme; comparing
     * the result is what keeps a knob being dragged through a range from
     * flooding subscribers with an event that says nothing changed. */
    FwmTheme palette;
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

/* Which desktop a window is ON, for every report that names one.
 *
 * The body's answer, not the picture's: a window sent to another desktop is
 * put there at once and then shown crossing the strip (server_desktop.c), and
 * for the third of a second that takes, dividing its x by the screen width
 * would report the desktop it is flying OVER — so a script that asked for the
 * move would be told it had not happened. `x` still says where the window is
 * being drawn, which is the other half of the truth. */
static int view_desktop(FwmServer *server, FwmView *view) {
    PhysicsBody *body = physics_find_body(&server->physics, view->id);
    if (body) return body->desktop_id;
    return server->screen_width > 0 ? view->x / server->screen_width : 0;
}

/* One window, in full. Shared by `windows` and by the reply to a `window`
 * change, so a script can see the result of what it just asked for without a
 * round trip — the same bargain `output` and `outputs` make. */
static void buf_window(struct Buf *b, FwmServer *server, FwmView *view) {
    buf_printf(b, "{\"id\":%u,\"title\":", view->id);
    buf_json_string(b, view_title(view));
    buf_puts(b, ",\"app_id\":");
    buf_json_string(b, view_app_id(view));
    buf_printf(b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d",
               view->x, view->y, view->width, view->height);
    buf_printf(b, ",\"desktop\":%d", view_desktop(server, view));
    buf_printf(b, ",\"focused\":%s", view == server->focused_view ? "true" : "false");
    /* Physics flags: the only way to see whether a [[rule]] (or a manual
     * toggle) actually took hold on this window. */
    PhysicsBody *body = physics_find_body(&server->physics, view->id);
    buf_printf(b, ",\"pinned\":%s", body && body->pinned ? "true" : "false");
    buf_printf(b, ",\"nocollide\":%s", body && body->no_collide ? "true" : "false");
    buf_printf(b, ",\"xwayland\":%s}", view->type == FWM_VIEW_XDG ? "false" : "true");
}

static void cmd_windows(FwmServer *server, struct Buf *b) {
    buf_puts(b, "{\"ok\":true,\"windows\":[");
    FwmView *view;
    bool first = true;
    wl_list_for_each(view, &server->views, link) {
        if (!first) buf_puts(b, ",");
        first = false;
        buf_window(b, server, view);
    }
    buf_puts(b, "]}\n");
}

static void cmd_state(FwmServer *server, struct Buf *b) {
    int count = wl_list_length(&server->views);
    int desktop = server->screen_width > 0
                  ? server_active_desktop(server) : 0;
    buf_puts(b, "{\"ok\":true,");
    buf_printf(b, "\"desktop\":%d,\"camera_x\":%d,\"windows\":%d,",
               desktop, server_active_output(server)
                            ? server_active_output(server)->camera_x : 0, count);
    buf_printf(b, "\"screen_width\":%d,\"screen_height\":%d,",
               server->screen_width, server->screen_height);

    /* One entry per monitor: which desktop it is showing and where it sits.
     * With independent screens "which desktop am I on" has more than one
     * answer, and this is the only place that gives all of them. */
    buf_puts(b, "\"outputs\":[");
    {
        FwmOutput *o;
        int first = 1;
        wl_list_for_each(o, &server->outputs, link) {
            if (!first) buf_puts(b, ",");
            first = 0;
            buf_puts(b, "{\"name\":");
            buf_json_string(b, o->wlr_output->name);
            buf_printf(b, ",\"desktop\":%d,\"camera_x\":%d,\"target_camera_x\":%d,"
                          "\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
                       o->desktop, o->camera_x, o->target_camera_x,
                       o->box.x, o->box.y, o->box.width, o->box.height);
        }
    }
    buf_puts(b, "],");
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

    /* Which desktops are asking to be looked at. The list, not a count: a bar
     * drawing the strip has to colour a particular digit, and `subscribe
     * urgent` only tells it what changed since it asked. */
    buf_puts(b, "\"urgent\":[");
    {
        int first = 1;
        for (int d = 0; d < FWM_DESKTOPS; d++) {
            if (!urgent_get(server, d)) continue;
            buf_printf(b, "%s%d", first ? "" : ",", d);
            first = 0;
        }
    }
    buf_puts(b, "],");

    buf_puts(b, "\"focused\":");
    buf_json_string(b, server->focused_view ? view_title(server->focused_view) : "");
    buf_puts(b, "}\n");
}

/* The compositor's own memory, split into the parts that mean different things.
 *
 * `top` shows one number for fwm — RES, well over a hundred megabytes — and it
 * reads as a compositor with a leak. Almost none of it is fwm's. The split is
 * the whole answer, so this reports the parts rather than the total:
 *
 *   anon   the heap and stacks: what fwm actually allocated and the only figure
 *          that can grow because of a bug here.
 *   file   mapped executables and libraries — mesa, ffmpeg, pango, cairo. Shared
 *          with every other process that maps them, so it is counted again in
 *          each of their totals; freeing it is not something a compositor can do.
 *   shmem  client buffers the compositor has mapped to composite them. Charged
 *          to fwm as well as to the client that owns them, and it grows with the
 *          number and size of open windows, not with anything fwm keeps.
 *
 * Straight out of /proc/self/status, which already maintains this breakdown —
 * walking smaps for a PSS figure would cost far more and answer a question
 * nobody asked. A kernel that omits a field leaves it at zero rather than
 * failing the command. */
static void cmd_memory(struct Buf *b) {
    long rss = 0, anon = 0, file = 0, shmem = 0;

    FILE *f = fopen("/proc/self/status", "r");
    if (!f) { reply_error(b, "cannot read /proc/self/status"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long v;
        if      (sscanf(line, "VmRSS: %ld kB", &v)    == 1) rss   = v;
        else if (sscanf(line, "RssAnon: %ld kB", &v)  == 1) anon  = v;
        else if (sscanf(line, "RssFile: %ld kB", &v)  == 1) file  = v;
        else if (sscanf(line, "RssShmem: %ld kB", &v) == 1) shmem = v;
    }
    fclose(f);

    buf_puts(b, "{\"ok\":true");
    buf_printf(b, ",\"rss_mb\":%.1f",   rss   / 1024.0);
    buf_printf(b, ",\"anon_mb\":%.1f",  anon  / 1024.0);
    buf_printf(b, ",\"file_mb\":%.1f",  file  / 1024.0);
    buf_printf(b, ",\"shmem_mb\":%.1f", shmem / 1024.0);
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

/* ── theme ────────────────────────────────────────────────────────────────
 *
 * The palette the overlays draw with, in the one form a shell script can use
 * without a parser: hex, exactly as config.toml writes a colour. With
 * color_source = "wallpaper" it is derived from the image and changes when the
 * image does, which is what makes it worth asking for — a generator like
 * matugen or pywal subscribes to `palette` and dresses GTK, the terminal and
 * everything else in the colours fwm is already using. */

/* unsigned char, not int: it is what a colour channel is, and it also tells
 * the compiler the %02x below can never widen past two digits. */
static unsigned char hex_byte(double v) {
    if (!(v > 0)) return 0;   /* the negated test also catches NaN */
    if (v > 1) v = 1;
    return (unsigned char)(v * 255.0 + 0.5);
}

static void hex_rgb(char out[10], const double rgb[3]) {
    snprintf(out, 10, "#%02x%02x%02x",
             hex_byte(rgb[0]), hex_byte(rgb[1]), hex_byte(rgb[2]));
}

/* Border colours are stored premultiplied for wlr_scene_rect; divide the alpha
 * back out so what comes over the socket is the colour that was written rather
 * than the colour as it happens to be blended. Opaque ones print as #RRGGBB so
 * the common case reads like every other entry. */
static void hex_premul(char out[10], const float c[4]) {
    double a = c[3], straight[3] = {0, 0, 0};
    if (a > 0)
        for (int i = 0; i < 3; i++) straight[i] = c[i] / a;
    if (a >= 1.0) { hex_rgb(out, straight); return; }
    snprintf(out, 10, "#%02x%02x%02x%02x", hex_byte(straight[0]),
             hex_byte(straight[1]), hex_byte(straight[2]), hex_byte(a));
}

/* The palette's fields, each with its leading comma, shared by the `theme`
 * reply and the `palette` event so the two can never drift apart. */
static void palette_fields(FwmServer *server, struct Buf *b) {
    const FwmTheme *t = theme_get();
    const DecorConfig *dc = &server->config.decor;
    bool from_wallpaper = dc->color_source == COLOR_SOURCE_WALLPAPER;
    char hex[10];

    buf_puts(b, ",\"source\":");
    buf_json_string(b, from_wallpaper ? "wallpaper" : "config");

    /* The image as well as the colours: matugen and pywal both make a better
     * scheme from the picture than from one colour lifted out of it, and the
     * event is the only notice they get that it changed. */
    const WallpaperLayer *pal = from_wallpaper
        ? config_wallpaper_first(&server->config, server->config.palette_output)
        : NULL;
    if (pal) {
        buf_puts(b, ",\"wallpaper\":");
        buf_json_string(b, pal->path);
    }

    buf_printf(b, ",\"generation\":%u,\"colors\":{", theme_generation());
    const struct { const char *name; const double *rgb; } cols[] = {
        { "pill",   t->pill   },
        { "sel",    t->sel    },
        { "text",   t->text   },
        { "muted",  t->muted  },
        { "dim",    t->dim    },
        { "accent", t->accent },
    };
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        hex_rgb(hex, cols[i].rgb);
        buf_printf(b, "%s\"%s\":\"%s\"", i ? "," : "", cols[i].name, hex);
    }
    hex_premul(hex, t->border_active);
    buf_printf(b, ",\"border_active\":\"%s\"", hex);
    hex_premul(hex, t->border_inactive);
    buf_printf(b, ",\"border_inactive\":\"%s\"}", hex);
}

static void cmd_theme(FwmServer *server, struct Buf *b) {
    buf_puts(b, "{\"ok\":true");
    palette_fields(server, b);
    buf_puts(b, "}\n");
}

/* ── settings ─────────────────────────────────────────────────────────────
 *
 * `set` is this session only; `save` is the same change written to the
 * overlay in ~/.local/state/fwm/settings, which is applied over config.toml
 * after every load. config.toml itself is never rewritten either way: the file
 * is the user's, and a compositor that reformats it once has broken a promise
 * it cannot make again. See server_config.c for the overlay's contract. */

#define IPC_MAX_PAIRS 32

struct SettingPair {
    const ConfigOption *opt;
    const char *value;   /* into the caller's copy of the request line */
};

/* Split `name=value name=value ...`, or the older `name value`, into pairs and
 * check every one of them against the option table. Nothing is applied here:
 * a request that carries several settings must be answered before any of them
 * lands, because a typo in the third one leaving the first two in place is the
 * failure a script cannot see and cannot undo.
 *
 * `line` is written into (the values are pointers into it). Returns how many
 * pairs were found, or -1 with the reason in `err`. */
static int parse_settings(char *line, struct SettingPair *out, int max,
                          char *err, size_t errcap) {
    /* The old form, kept because it is what every script and every page of the
     * docs has been saying for as long as there has been a socket. One name,
     * one value, and the value may be anything short of a space. */
    if (!strchr(line, '=')) {
        char *sep = strchr(line, ' ');
        if (!sep) { snprintf(err, errcap, "set needs <name> <value>"); return -1; }
        *sep++ = '\0';
        while (*sep == ' ') sep++;
        const ConfigOption *opt = config_option_find(line);
        if (!opt) { snprintf(err, errcap, "unknown option \"%s\" (try: config)", line); return -1; }
        if (!config_option_check(opt, sep, err, errcap)) return -1;
        out[0].opt = opt;
        out[0].value = sep;
        return 1;
    }

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (n >= max) { snprintf(err, errcap, "at most %d settings at once", max); return -1; }
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok) {
            snprintf(err, errcap, "\"%s\" is not name=value", tok);
            return -1;
        }
        *eq = '\0';
        const ConfigOption *opt = config_option_find(tok);
        if (!opt) { snprintf(err, errcap, "unknown option \"%s\" (try: config)", tok); return -1; }
        if (!config_option_check(opt, eq + 1, err, errcap)) return -1;
        out[n].opt = opt;
        out[n].value = eq + 1;
        n++;
    }
    if (n == 0) { snprintf(err, errcap, "set needs <name>=<value>"); return -1; }
    return n;
}

/* The reply every settings command sends: the options it touched and what they
 * are worth now. `name`/`value` are repeated at the top level for a single
 * option, because that is the shape every script that already speaks to this
 * socket is reading. */
static void reply_settings(FwmServer *server, struct SettingPair *pairs, int n,
                           const char *array, struct Buf *b) {
    buf_puts(b, "{\"ok\":true");
    if (n == 1) {
        char value[64];
        config_option_get(&server->config, pairs[0].opt, value, sizeof(value));
        buf_puts(b, ",\"name\":");
        buf_json_string(b, pairs[0].opt->name);
        buf_puts(b, ",\"value\":");
        buf_json_string(b, value);
    }
    buf_printf(b, ",\"%s\":[", array);
    for (int i = 0; i < n; i++) {
        char value[64];
        config_option_get(&server->config, pairs[i].opt, value, sizeof(value));
        if (i) buf_puts(b, ",");
        buf_puts(b, "{\"name\":");
        buf_json_string(b, pairs[i].opt->name);
        buf_puts(b, ",\"value\":");
        buf_json_string(b, value);
        buf_puts(b, "}");
    }
    buf_puts(b, "]}\n");
}

/* Apply the checked pairs. One re-apply for the lot: a script changing three
 * related settings (a sun's angle, height and length) should not produce two
 * frames of the half-changed world.
 *
 * Nothing is emitted here. server_apply_config ends by announcing whatever
 * moved, which is the only way an option changed by a KEY gets announced too —
 * see server_settings_notify. */
static void settings_commit(FwmServer *server, struct SettingPair *pairs, int n) {
    for (int i = 0; i < n; i++) {
        char err[192];
        config_option_set(&server->config, pairs[i].opt, pairs[i].value, err, sizeof(err));
    }
    /* Same re-apply path a reload uses, minus the wallpaper image decode. */
    server_apply_config(server, 0);
}

/* `set <name> <value>` or `set <name>=<value> ...` — runtime only.
 * reload (super+shift+r) is the documented way back to the file. */
static void cmd_set(FwmServer *server, const char *arg, struct Buf *b) {
    if (!arg) { reply_error(b, "set needs <name> <value>"); return; }

    char line[1024];
    if (snprintf(line, sizeof line, "%s", arg) >= (int)sizeof line) {
        reply_error(b, "set command too long");
        return;
    }

    struct SettingPair pairs[IPC_MAX_PAIRS];
    char err[256];
    int n = parse_settings(line, pairs, IPC_MAX_PAIRS, err, sizeof err);
    if (n < 0) { reply_error(b, err); return; }

    settings_commit(server, pairs, n);
    reply_settings(server, pairs, n, "set", b);
}

/* `save` — the same change, and remembered.
 *
 *   save <name> <value>        set it and write it down
 *   save <name>=<v> ...        several, together
 *   save <name>                write down whatever it is worth right now,
 *                              which is how a session tuned with `set` is kept
 *   save --all                 everything this session has changed
 */
static void cmd_save(FwmServer *server, const char *arg, struct Buf *b) {
    if (!arg) { reply_error(b, "save needs <name> [value], or --all"); return; }

    if (strcmp(arg, "--all") == 0 || strcmp(arg, "-a") == 0) {
        int n = server_settings_save_all(server);
        if (n < 0) { reply_error(b, "cannot write the settings file"); return; }
        buf_printf(b, "{\"ok\":true,\"count\":%d}\n", n);
        return;
    }

    char line[1024];
    if (snprintf(line, sizeof line, "%s", arg) >= (int)sizeof line) {
        reply_error(b, "save command too long");
        return;
    }

    /* A bare name saves what the option is worth now. Deliberately the same
     * verb: turning a knob with `set` until it looks right and then keeping it
     * is the way this gets used, and it should not need a second word for the
     * value you have just been looking at. */
    struct SettingPair pairs[IPC_MAX_PAIRS];
    char err[256];
    int n;
    char current[64];
    const ConfigOption *bare = strchr(line, ' ') || strchr(line, '=')
                               ? NULL : config_option_find(line);
    if (bare) {
        config_option_get(&server->config, bare, current, sizeof(current));
        pairs[0].opt = bare;
        pairs[0].value = current;
        n = 1;
    } else {
        n = parse_settings(line, pairs, IPC_MAX_PAIRS, err, sizeof err);
        if (n < 0) { reply_error(b, err); return; }
    }

    /* Written down BEFORE it is applied, so that the `setting` event the apply
     * produces already says saved:true. A subscriber seeing the change and the
     * word "saved" in two different events would have to guess whether a third
     * was coming. */
    for (int i = 0; i < n; i++) {
        if (!server_settings_write(pairs[i].opt->name, pairs[i].value)) {
            reply_error(b, "cannot write the settings file");
            return;
        }
    }
    if (!bare) settings_commit(server, pairs, n);
    reply_settings(server, pairs, n, "saved", b);
}

/* `unsave <name> ...` / `unsave --all` — take it out of the overlay AND put
 * the configured value back now. The alternative would be to ask for a reload,
 * which also throws away every other `set` the session is standing on: a heavy
 * price for taking back one line. */
static void cmd_unsave(FwmServer *server, const char *arg, struct Buf *b) {
    if (!arg) { reply_error(b, "unsave needs <name>, or --all"); return; }

    bool all = strcmp(arg, "--all") == 0 || strcmp(arg, "-a") == 0;

    struct SettingPair pairs[IPC_MAX_PAIRS];
    char values[IPC_MAX_PAIRS][64];   /* pairs[].value points in here */
    int n = 0;

    if (all) {
        char names[SETTINGS_MAX][64], vals[SETTINGS_MAX][64];
        int have = server_settings_read(names, vals, SETTINGS_MAX);
        for (int i = 0; i < have && n < IPC_MAX_PAIRS; i++) {
            const ConfigOption *opt = config_option_find(names[i]);
            if (!opt) continue;
            if (!server_settings_file_value(opt, values[n], sizeof(values[n]))) continue;
            pairs[n].opt = opt;
            pairs[n].value = values[n];
            n++;
        }
    } else {
        char line[1024];
        if (snprintf(line, sizeof line, "%s", arg) >= (int)sizeof line) {
            reply_error(b, "unsave command too long");
            return;
        }
        char *save = NULL;
        for (char *tok = strtok_r(line, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            if (n >= IPC_MAX_PAIRS) break;
            const ConfigOption *opt = config_option_find(tok);
            if (!opt) {
                char err[128];
                snprintf(err, sizeof err, "unknown option \"%s\" (try: config)", tok);
                reply_error(b, err);
                return;
            }
            if (!server_settings_file_value(opt, values[n], sizeof(values[n]))) continue;
            pairs[n].opt = opt;
            pairs[n].value = values[n];
            n++;
        }
    }

    for (int i = 0; i < n; i++) {
        if (!server_settings_write(pairs[i].opt->name, NULL)) {
            reply_error(b, "cannot write the settings file");
            return;
        }
    }
    if (n > 0) settings_commit(server, pairs, n);
    reply_settings(server, pairs, n, "unsaved", b);
}

/* `saved` — what is in the overlay, and what each of those options is worth
 * right now. The two differ whenever a `set` has moved one since, which is
 * exactly the state somebody asking this question is trying to see. */
static void cmd_saved(FwmServer *server, struct Buf *b) {
    char names[SETTINGS_MAX][64], values[SETTINGS_MAX][64];
    int n = server_settings_read(names, values, SETTINGS_MAX);

    buf_puts(b, "{\"ok\":true,\"saved\":[");
    for (int i = 0; i < n; i++) {
        if (i) buf_puts(b, ",");
        buf_puts(b, "{\"name\":");
        buf_json_string(b, names[i]);
        buf_puts(b, ",\"value\":");
        buf_json_string(b, values[i]);
        const ConfigOption *opt = config_option_find(names[i]);
        if (opt) {
            char now[64];
            config_option_get(&server->config, opt, now, sizeof(now));
            buf_puts(b, ",\"live\":");
            buf_json_string(b, now);
        } else {
            /* A name this build does not have: written by a newer fwm, kept
             * rather than dropped, and reported so it does not look lost. */
            buf_puts(b, ",\"live\":null,\"known\":false");
        }
        buf_puts(b, "}");
    }
    buf_puts(b, "]}\n");
}

/* ── one window ───────────────────────────────────────────────────────────
 *
 * `dispatch` acts on whatever has the focus, because that is what a keybind
 * means. A script has no hands and no focus to speak of: it has an id out of
 * `windows` and something it wants done to that window, and every way of
 * expressing that through `dispatch` goes through focusing the window first —
 * which is itself a visible change to the session nobody asked for.
 *
 * So: `window <id> key=value ...`, in the same shape as `output`, parsed in
 * full before anything is applied. */

/* on|off|true|false|1|0|toggle. Returns the new value, or -1 if the word is
 * not one of those. `now` is what makes toggle mean anything. */
static int parse_switch(const char *val, int now) {
    if (strcmp(val, "toggle") == 0) return !now;
    if (strcmp(val, "on") == 0 || strcmp(val, "true") == 0 || strcmp(val, "1") == 0) return 1;
    if (strcmp(val, "off") == 0 || strcmp(val, "false") == 0 || strcmp(val, "0") == 0) return 0;
    return -1;
}

static void cmd_window(FwmServer *server, const char *arg, struct Buf *b) {
    if (!arg) {
        reply_error(b, "window needs <id> <key>=<value>... "
                       "(desktop, x, y, pin, nocollide, focus, close)");
        return;
    }

    char args[512];
    if (snprintf(args, sizeof args, "%s", arg) >= (int)sizeof args) {
        reply_error(b, "window command too long");
        return;
    }

    char *save = NULL;
    char *id_tok = strtok_r(args, " ", &save);
    if (!id_tok) { reply_error(b, "window needs an id"); return; }

    char *end;
    unsigned long id = strtoul(id_tok, &end, 10);
    if (end == id_tok || *end) { reply_error(b, "window id must be a number (try: windows)"); return; }

    FwmView *view = server_find_view(server, (uint32_t)id);
    if (!view) {
        char err[96];
        snprintf(err, sizeof err, "no window with id %lu (try: windows)", id);
        reply_error(b, err);
        return;
    }
    PhysicsBody *body = physics_find_body(&server->physics, view->id);

    /* Everything is decided here and applied below, so a typo in the third
     * setting cannot leave the first two standing. */
    int want_desktop = -1, want_pin = -1, want_nocollide = -1;
    int want_focus = 0, want_close = 0;
    int want_x = 0, want_y = 0;
    bool have_x = false, have_y = false;
    char err[256];

    for (char *tok = strtok_r(NULL, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok) {
            snprintf(err, sizeof err, "\"%s\" is not key=value", tok);
            reply_error(b, err);
            return;
        }
        *eq = '\0';
        const char *key = tok, *val = eq + 1;

        if (strcmp(key, "desktop") == 0) {
            long d = strtol(val, &end, 10);
            if (end == val || *end || d < 0 || d >= FWM_DESKTOPS) {
                snprintf(err, sizeof err, "desktop must be 0..%d", FWM_DESKTOPS - 1);
                reply_error(b, err);
                return;
            }
            want_desktop = (int)d;
        } else if (strcmp(key, "x") == 0 || strcmp(key, "y") == 0) {
            long v = strtol(val, &end, 10);
            if (end == val || *end) { reply_error(b, "x and y must be whole numbers"); return; }
            if (*key == 'x') { want_x = (int)v; have_x = true; }
            else             { want_y = (int)v; have_y = true; }
        } else if (strcmp(key, "pin") == 0) {
            want_pin = parse_switch(val, body && body->pinned);
            if (want_pin < 0) { reply_error(b, "pin must be on, off or toggle"); return; }
        } else if (strcmp(key, "nocollide") == 0) {
            want_nocollide = parse_switch(val, body && body->no_collide);
            if (want_nocollide < 0) { reply_error(b, "nocollide must be on, off or toggle"); return; }
        } else if (strcmp(key, "focus") == 0) {
            int on = parse_switch(val, view == server->focused_view);
            if (on < 0) { reply_error(b, "focus must be on or off"); return; }
            if (on) want_focus = 1;
        } else if (strcmp(key, "close") == 0) {
            int on = parse_switch(val, 0);
            if (on < 0) { reply_error(b, "close must be on"); return; }
            want_close = on;
        } else {
            snprintf(err, sizeof err, "unknown key \"%s\" "
                     "(desktop, x, y, pin, nocollide, focus, close)", key);
            reply_error(b, err);
            return;
        }
    }

    /* A tiled window's geometry belongs to the layout, which would put it
     * straight back — refusing says so instead of appearing to work for one
     * frame. Moving it to another desktop is a different thing and stays
     * allowed: that is how a window leaves a layout. */
    if ((have_x || have_y) && body &&
        server->desktop_mode[body->desktop_id] == DESKTOP_MODE_TILING) {
        reply_error(b, "this window is tiled: its position belongs to the layout "
                       "(move it off the desktop, or turn tiling off)");
        return;
    }

    /* A view outlives the unmap of its surface and stays on the list until the
     * client is gone, so `windows` lists windows with nothing on screen — and
     * focusing one handed the keyboard to a window nobody could see or get
     * back out of. Refusing says so. */
    if (want_focus && !view->scene_tree) {
        reply_error(b, "this window has no surface on screen to focus");
        return;
    }

    if (want_desktop >= 0)
        server_move_view_to_desktop(server, view, want_desktop, 0);
    if (body && want_pin >= 0) {
        body->pinned = want_pin;
        body->vx = body->vy = 0;
        body->flying = 0;
    }
    if (body && want_nocollide >= 0) body->no_collide = want_nocollide;
    if (have_x || have_y) {
        /* Straight into the mirror: physics_step notices a position that no
         * longer matches its shadow and pushes it into Box2D, which is the
         * same path a drag takes. Dropped rather than carried, so a window put
         * somewhere does not then slide off under the momentum it had. */
        if (body) {
            if (have_x) body->x = want_x;
            if (have_y) body->y = want_y;
            body->vx = body->vy = 0;
            body->flying = 0;
        }
        /* Put HERE beats on the way somewhere else: a desktop= in the same
         * request has just armed a flight across the strip, and leaving it
         * armed would carry the window off the spot this asked for. */
        view->tile_anim = TILE_ANIM_NONE;
        /* And the window itself, rather than waiting for the tick to notice.
         * Not merely for a truthful reply: the tick does not carry a PINNED
         * body back to its view at all — being pinned is what "the simulation
         * does not move this" means — so a pinned window put somewhere by a
         * script would never arrive. */
        if (have_x) view->x = want_x;
        if (have_y) view->y = want_y;
        if (view->scene_tree)
            server_place_view(server, view, view->x, view->y);
    }
    if (want_focus) {
        /* A window hidden inside a tab-stack comes up first. Its scene node is
         * disabled and its physics body belongs to whichever tab is in front,
         * so focusing it where it stands would put the keyboard in a window
         * that is not drawn — `focus` on a tab means the same thing clicking
         * that tab means. */
        if (view->group) {
            for (int i = 0; i < view->group->count; i++) {
                if (view->group->members[i] == view) {
                    group_set_active(server, view->group, i);
                    break;
                }
            }
        }
        server_focus_view(server, view);
    }
    if (want_close) {
        view_send_close(view);
        /* The window is being ASKED to close and may decline, so it is still
         * here to be reported — which is the honest answer either way. */
    }

    buf_puts(b, "{\"ok\":true,\"window\":");
    buf_window(b, server, view);
    buf_puts(b, "}\n");
}

/* ── monitors ─────────────────────────────────────────────────────────────
 *
 * `outputs` says what the screens are and what they could be; `output` changes
 * one. Together they are the whole of display configuration, because there is
 * no fwm GUI for it and there is not going to be one: this is what wlr-randr
 * would give you, spoken in the compositor's own vocabulary (desktops
 * included) and applied through the same code the config file uses. */

/* One monitor, in full. Shared by `outputs` and by the reply to a change, so
 * a script can see the result of what it just asked for without a round trip. */
static void buf_output(struct Buf *b, FwmOutput *o) {
    struct wlr_output *wlr = o->wlr_output;

    buf_puts(b, "{\"name\":");
    buf_json_string(b, wlr->name);
    buf_puts(b, ",\"description\":");
    buf_json_string(b, wlr->description ? wlr->description : "");
    buf_puts(b, ",\"make\":");
    buf_json_string(b, wlr->make ? wlr->make : "");
    buf_puts(b, ",\"model\":");
    buf_json_string(b, wlr->model ? wlr->model : "");
    buf_printf(b, ",\"enabled\":%s", o->enabled ? "true" : "false");
    buf_printf(b, ",\"desktop\":%d", o->desktop);
    /* The layout box: the size a DESKTOP is on this screen, which is the mode
     * divided by the scale and turned by the transform — not the mode itself,
     * and the difference is the whole point of setting either. */
    buf_printf(b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d",
               o->box.x, o->box.y, o->box.width, o->box.height);
    buf_printf(b, ",\"scale\":%.3f", (double)wlr->scale);
    buf_puts(b, ",\"transform\":");
    buf_json_string(b, config_transform_name(wlr->transform));

    if (wlr->width > 0 && wlr->height > 0) {
        /* Spelled so it can be handed straight back to `mode=`. The nested and
         * headless backends report no refresh at all, and "@0.00" there would
         * be a number that means nothing. */
        char mode[64];
        if (wlr->refresh > 0)
            snprintf(mode, sizeof mode, "%dx%d@%.2f", wlr->width, wlr->height,
                     wlr->refresh / 1000.0);
        else
            snprintf(mode, sizeof mode, "%dx%d", wlr->width, wlr->height);
        buf_puts(b, ",\"mode\":");
        buf_json_string(b, mode);
    } else {
        buf_puts(b, ",\"mode\":\"\"");
    }

    /* Everything this monitor advertises — the list `mode=` picks from, and
     * empty for the nested and headless backends, which take any size. */
    buf_puts(b, ",\"modes\":[");
    struct wlr_output_mode *m;
    bool first = true;
    wl_list_for_each(m, &wlr->modes, link) {
        if (!first) buf_puts(b, ",");
        first = false;
        buf_printf(b, "{\"width\":%d,\"height\":%d,\"refresh\":%.3f,"
                      "\"preferred\":%s,\"current\":%s}",
                   m->width, m->height, m->refresh / 1000.0,
                   m->preferred ? "true" : "false",
                   m == wlr->current_mode ? "true" : "false");
    }
    buf_puts(b, "]}");
}

static void cmd_outputs(FwmServer *server, struct Buf *b) {
    buf_puts(b, "{\"ok\":true,\"outputs\":[");
    FwmOutput *o;
    bool first = true;
    wl_list_for_each(o, &server->outputs, link) {
        if (!first) buf_puts(b, ",");
        first = false;
        buf_output(b, o);
    }
    buf_puts(b, "]}\n");
}

/* `output <name> key=value ...` — resolution, scale, rotation, position, the
 * desktop it shows, and whether it is lit at all.
 *
 * Every token is parsed before any of them is applied. A command with a typo
 * in its third setting must not leave the screen halfway through the other
 * two: the failure a user is most likely to hit here is also the one they can
 * least afford, since a monitor mid-change may be showing nothing readable. */
static void cmd_output(FwmServer *server, const char *arg, struct Buf *b) {
    if (!arg) {
        reply_error(b, "output needs <name> <key>=<value>... "
                       "(mode, scale, transform, position, desktop, enabled)");
        return;
    }

    char args[512];
    if (snprintf(args, sizeof args, "%s", arg) >= (int)sizeof args) {
        reply_error(b, "output command too long");
        return;
    }

    char *save = NULL;
    char *name = strtok_r(args, " ", &save);
    if (!name) { reply_error(b, "output needs a monitor name"); return; }

    FwmOutput *out = server_output_find(server, name);
    if (!out) {
        char err[128];
        snprintf(err, sizeof err, "no monitor named %s (try: outputs)", name);
        reply_error(b, err);
        return;
    }

    FwmOutputSetup setup = {0};
    int want_enabled = -1, want_desktop = -1, changes = 0;
    char err[256];

    for (char *tok = strtok_r(NULL, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok) {
            snprintf(err, sizeof err, "\"%s\" is not key=value", tok);
            reply_error(b, err);
            return;
        }
        *eq = '\0';
        const char *key = tok, *val = eq + 1;
        changes++;

        if (strcmp(key, "mode") == 0) {
            if (!config_parse_mode(val, &setup.mode_w, &setup.mode_h, &setup.mode_refresh)) {
                snprintf(err, sizeof err, "mode \"%s\" is not WIDTHxHEIGHT[@HZ]", val);
                reply_error(b, err);
                return;
            }
            setup.have_mode = 1;
        } else if (strcmp(key, "scale") == 0) {
            char *end;
            double v = strtod(val, &end);
            if (end == val || *end || v < 0.25 || v > 10.0) {
                reply_error(b, "scale must be a number in 0.25..10");
                return;
            }
            setup.have_scale = 1;
            setup.scale = v;
        } else if (strcmp(key, "transform") == 0) {
            int t = config_parse_transform(val);
            if (t < 0) {
                reply_error(b, "transform must be normal, 90, 180, 270, flipped, "
                               "flipped-90, flipped-180 or flipped-270");
                return;
            }
            setup.have_transform = 1;
            setup.transform = t;
        } else if (strcmp(key, "position") == 0 || strcmp(key, "pos") == 0) {
            char *end;
            long x = strtol(val, &end, 10);
            if (end == val || *end != ',') { reply_error(b, "position must be X,Y"); return; }
            const char *p = end + 1;
            long y = strtol(p, &end, 10);
            if (end == p || *end || x < -32768 || x > 32768 || y < -32768 || y > 32768) {
                reply_error(b, "position must be X,Y, each within ±32768");
                return;
            }
            setup.have_pos = 1;
            setup.x = (int)x;
            setup.y = (int)y;
        } else if (strcmp(key, "desktop") == 0) {
            char *end;
            long d = strtol(val, &end, 10);
            if (end == val || *end || d < 0 || d >= FWM_DESKTOPS) {
                snprintf(err, sizeof err, "desktop must be 0..%d", FWM_DESKTOPS - 1);
                reply_error(b, err);
                return;
            }
            want_desktop = (int)d;
        } else if (strcmp(key, "enabled") == 0) {
            if (strcmp(val, "true") == 0 || strcmp(val, "on") == 0 || strcmp(val, "1") == 0)
                want_enabled = 1;
            else if (strcmp(val, "false") == 0 || strcmp(val, "off") == 0 || strcmp(val, "0") == 0)
                want_enabled = 0;
            else { reply_error(b, "enabled must be on or off"); return; }
        } else {
            snprintf(err, sizeof err, "unknown key \"%s\" (mode, scale, transform, "
                                      "position, desktop, enabled)", key);
            reply_error(b, err);
            return;
        }
    }

    if (!changes) { reply_error(b, "output needs at least one key=value"); return; }

    /* Lighting a screen comes first — a mode cannot be set on a dark one — and
     * putting one out comes last, so `enabled=off desktop=2` still means
     * something sane and nothing is applied to a screen that is already gone. */
    if (want_enabled == 1) server_output_set_enabled(server, out, 1);

    if ((setup.have_mode || setup.have_scale || setup.have_transform || setup.have_pos) &&
        !server_output_apply_setup(server, out, &setup, err, sizeof err)) {
        reply_error(b, err);
        return;
    }

    if (want_desktop >= 0 && out->enabled)
        server_output_show_desktop(server, out, want_desktop, 0);

    if (want_enabled == 0) {
        server_output_set_enabled(server, out, 0);
        /* The one request it can refuse outright, and silence would look like
         * success to a script. Asking a screen that is ALREADY off to go off
         * is not that: it is simply already how it was asked to be. */
        if (out->enabled) {
            reply_error(b, "refusing to turn off the last lit screen");
            return;
        }
    }

    buf_puts(b, "{\"ok\":true,\"output\":");
    buf_output(b, out);
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

/* `urgent <desktop> [on|off]` — light a desktop's number red, or put it out.
 *
 * This is how a notification daemon reaches the tray: dunst or mako already
 * knows a message arrived and which application sent it, fwm knows which
 * desktop that application's window is on, and one line of shell joins them.
 * Nothing here decides what deserves attention, which is exactly why it is a
 * command and not a protocol.
 *
 * The desktop is numbered from 0, like every other desktop in the IPC (`state`,
 * `windows`, `window <id> desktop=`) and unlike the keys, where super+1 reaches
 * desktop 0.
 *
 * Raising it on a desktop that is on a screen does nothing, and the reply says
 * so with `urgent: false` rather than failing — "you are already looking at it"
 * is an answer, not the script's mistake. */
static void cmd_urgent(FwmServer *server, const char *arg, struct Buf *b) {
    char err[96];
    if (!arg) {
        snprintf(err, sizeof err, "urgent needs a desktop, 0..%d", FWM_DESKTOPS - 1);
        reply_error(b, err);
        return;
    }

    char *end = NULL;
    long n = strtol(arg, &end, 10);
    if (end == arg || n < 0 || n >= FWM_DESKTOPS) {
        snprintf(err, sizeof err, "urgent: desktop must be 0..%d", FWM_DESKTOPS - 1);
        reply_error(b, err);
        return;
    }
    int d = (int)n;

    while (*end == ' ') end++;
    bool on = true;
    if (*end) {
        if      (strcmp(end, "on")  == 0) on = true;
        else if (strcmp(end, "off") == 0) on = false;
        else {
            snprintf(err, sizeof err, "urgent: expected on|off, got \"%.32s\"", end);
            reply_error(b, err);
            return;
        }
    }

    if (on) urgent_raise(server, d);
    else    urgent_drop(server, d);

    buf_printf(b, "{\"ok\":true,\"desktop\":%d,\"urgent\":%s}\n",
               d, urgent_get(server, d) ? "true" : "false");
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
        /* Which binary is answering, and when it was built.
         *
         * Read off the running executable itself rather than stamped in at
         * compile time, because the question this exists to settle is exactly
         * the one a compile-time stamp cannot: a compositor is started from a
         * TTY and outlives the shell that started it, there is normally an
         * installed fwm on PATH as well as whatever is in build/, and every
         * "it does not work" begins with finding out which of them is running.
         * /proc/self/exe and its mtime cannot disagree with reality. */
        char exe[256];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n < 0) n = 0;
        exe[n] = '\0';

        char built[64] = "?";
        struct stat st;
        if (n > 0 && stat(exe, &st) == 0) {
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            strftime(built, sizeof(built), "%Y-%m-%d %H:%M:%S", &tm);
        }

        /* `fwm` is the release; `version` stays the IPC's own, which scripts
         * branch on and which moves on its own schedule. Both, because "which
         * fwm is this" and "what may I ask it" are different questions. */
        buf_puts(out, "{\"ok\":true,\"fwm\":\"" FWM_VERSION
                      "\",\"version\":\"fwm ipc 1\",\"binary\":");
        buf_json_string(out, n > 0 ? exe : "?");
        buf_puts(out, ",\"built\":");
        buf_json_string(out, built);
        buf_puts(out, ",\"pid\":");
        char pid[32];
        snprintf(pid, sizeof(pid), "%d", (int)getpid());
        buf_puts(out, pid);
        buf_puts(out, "}\n");
        return;
    }
    if (IS("state"))   { cmd_state(server, out);   return; }
    if (IS("windows")) { cmd_windows(server, out); return; }
    if (IS("config"))  { cmd_config(server, out);  return; }
    if (IS("outputs")) { cmd_outputs(server, out); return; }
    if (IS("memory"))  { cmd_memory(out);          return; }
    if (IS("theme"))   { cmd_theme(server, out);   return; }
    if (IS("get")) {
        if (!arg) { reply_error(out, "get needs an option name"); return; }
        cmd_get(server, arg, out);
        return;
    }
    if (IS("saved")) { cmd_saved(server, out); return; }
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
    if (IS("save")) {
        cmd_save(server, arg, out);
        return;
    }
    if (IS("unsave")) {
        cmd_unsave(server, arg, out);
        return;
    }
    if (IS("window")) {
        cmd_window(server, arg, out);
        return;
    }
    if (IS("output")) {
        cmd_output(server, arg, out);
        return;
    }
    if (IS("urgent")) {
        cmd_urgent(server, arg, out);
        return;
    }
    if (IS("reload")) {
        server_reload_config(server);
        reply_ok(out);
        return;
    }

    reply_error(out, "unknown command (try: version, state, windows, window, outputs, "
                     "output, config, theme, get, set, save, unsave, saved, "
                     "dispatch, urgent, reload, subscribe)");
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
        buf_printf(&b, ",\"desktop\":%d", view_desktop(server, view));
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

void ipc_emit_urgent(FwmIpc *ipc, int desktop, bool urgent) {
    if (!ipc_wants(ipc, FWM_EV_URGENT)) return;

    struct Buf b = {0};
    buf_printf(&b, "{\"event\":\"urgent\",\"desktop\":%d,\"urgent\":%s}\n",
               desktop, urgent ? "true" : "false");
    ipc_broadcast(ipc, FWM_EV_URGENT, &b);
    free(b.data);
}

void ipc_emit_gravity(FwmIpc *ipc, double gravity_scale) {
    if (!ipc_wants(ipc, FWM_EV_GRAVITY)) return;

    struct Buf b = {0};
    buf_printf(&b, "{\"event\":\"gravity\",\"gravity\":%.3f}\n", gravity_scale);
    ipc_broadcast(ipc, FWM_EV_GRAVITY, &b);
    free(b.data);
}

/* A piece of fwm's own interface opened or closed. These are not windows and
 * never will be, so nothing else in the stream reports them: without this a
 * subscriber cannot tell that the launcher is up. */
void ipc_emit_ui(FwmIpc *ipc, const char *what, bool open) {
    if (!ipc_wants(ipc, FWM_EV_UI)) return;

    struct Buf b = {0};
    buf_puts(&b, "{\"event\":\"ui\",\"what\":");
    buf_json_string(&b, what);
    buf_printf(&b, ",\"open\":%s}\n", open ? "true" : "false");
    ipc_broadcast(ipc, FWM_EV_UI, &b);
    free(b.data);
}

/* One option changed, by whatever route — a key, the socket, or the modes
 * menu underneath. `saved` says whether it also went into the overlay, which
 * is the difference between a bar redrawing itself and a bar that should also
 * expect the change to survive the next reload. */
void ipc_emit_setting(FwmIpc *ipc, const char *name, const char *value, bool saved) {
    if (!ipc_wants(ipc, FWM_EV_SETTING)) return;

    struct Buf b = {0};
    buf_puts(&b, "{\"event\":\"setting\",\"name\":");
    buf_json_string(&b, name);
    buf_puts(&b, ",\"value\":");
    buf_json_string(&b, value);
    buf_printf(&b, ",\"saved\":%s}\n", saved ? "true" : "false");
    ipc_broadcast(ipc, FWM_EV_SETTING, &b);
    free(b.data);
}

/* The palette moved: a wallpaper swap, a reload, or a `set` that touched
 * color_source or tint_strength. Callers fire this after every theme_build()
 * without checking anything — the comparison below is what decides whether
 * there is news. Identical inputs rebuild to identical bits, so a false
 * "changed" would take a rebuild that genuinely differs somewhere invisible,
 * and would cost a subscriber one redundant event. */
void ipc_emit_palette(FwmIpc *ipc) {
    if (!ipc) return;

    const FwmTheme *t = theme_get();
    bool changed = memcmp(&ipc->palette, t, sizeof(*t)) != 0;
    ipc->palette = *t;
    if (!changed || !ipc_wants(ipc, FWM_EV_PALETTE)) return;

    struct Buf b = {0};
    buf_puts(&b, "{\"event\":\"palette\"");
    palette_fields(ipc->server, &b);
    buf_puts(&b, "}\n");
    ipc_broadcast(ipc, FWM_EV_PALETTE, &b);
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
    if (!dir || !*dir) {
        dir = "/tmp";
        wlr_log(WLR_INFO, "ipc: no XDG_RUNTIME_DIR — the control socket goes in "
                          "/tmp, where only its own mode keeps it private");
    }

    FwmIpc *ipc = calloc(1, sizeof(*ipc));
    if (!ipc) return NULL;
    ipc->server = server;
    ipc->fd = -1;
    ipc->palette = *theme_get();
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
    if (bind(ipc->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        wlr_log(WLR_ERROR, "ipc: bind on %s failed: %s", ipc->path, strerror(errno));
        close(ipc->fd);
        free(ipc);
        return NULL;
    }

    /* Owner only. `dispatch spawn:…` runs a command as the user who is logged
     * in, so anyone who can connect to this socket can run anything they like
     * as them — and bind() takes its mode from the umask, which is 0022 on a
     * normal login and world-readable-and-writable on some. XDG_RUNTIME_DIR is
     * 0700 and hides the whole question, but the fallback above is /tmp, which
     * hides nothing.
     *
     * Ordered bind → chmod → listen on purpose, and not bind → listen → chmod:
     * connect() to a socket that is bound but not yet listening is refused
     * outright, so there is no window in which a permissive mode is also a
     * reachable one. */
    if (chmod(ipc->path, S_IRUSR | S_IWUSR) < 0) {
        wlr_log(WLR_ERROR, "ipc: could not restrict %s to its owner: %s",
                ipc->path, strerror(errno));
        close(ipc->fd);
        unlink(ipc->path);
        free(ipc);
        return NULL;
    }

    if (listen(ipc->fd, IPC_MAX_CLIENTS) < 0) {
        wlr_log(WLR_ERROR, "ipc: listen on %s failed: %s", ipc->path, strerror(errno));
        close(ipc->fd);
        unlink(ipc->path);
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
