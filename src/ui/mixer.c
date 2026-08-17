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

#include "mixer.h"
#include "cairo_overlay.h"
#include "icons.h"
#include "modes.h"
#include "../theme.h"
#include "../server.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

/* Everything that is playing, one row each, with the master at the top.
 *
 * Built for the same hand the radial menu is: turning the knob walks the list,
 * pressing it TAKES HOLD of the row it stopped on, and while a row is held the
 * knob is that row's volume knob. Press again and it lets go, and turning
 * walks the list once more. Two verbs on one control, which is the whole
 * reason the panel is a list and not another ring — a ring cannot hold a value
 * you turn, because turning is how you leave it.
 *
 * fwm does not own the audio session, so nothing here talks to PipeWire: the
 * rows come out of the shell commands in [mixer] (and, for the master row,
 * [volume]), exactly as the volume keys already work. See src/config.h.
 *
 * What is shown is PREDICTED and then confirmed, for volume.c's reason: a turn
 * has to answer on the frame it happened, and a command takes milliseconds to
 * fork and print. So a step moves the row straight away, the command goes out
 * behind it, and the next read corrects the row if the mixer disagreed — with
 * a short hold after every local change, so an answer about the value from
 * BEFORE the turn cannot snap the bar back under the hand.
 */

#define MIX_ROWS_MAX   24
#define MASTER_ID      (-1)          /* the row that is not a stream */

#define PANEL_W        470.0
#define ROW_H          46.0
#define PAD            12.0
#define HEAD_H         36.0
#define ROWS_VISIBLE   9             /* before the list starts scrolling */
#define ROW_R          9.0           /* row corner radius */
#define ICON_SZ        26
#define BAR_H          6.0
#define BAR_X          52.0          /* from the panel's left edge */
#define VAL_W          58.0          /* the percentage column on the right */

#define PANEL_ANIM_MS  150.0
#define PANEL_RISE_PX  12.0
#define ROW_STAGGER    0.028         /* seconds between rows arriving */
#define ROW_RISE       0.16          /* how long one row takes to arrive */
#define BAR_RATE       16.0          /* how fast a bar eases to its value */
#define HOLD_S         0.6           /* a local change owns its row this long */
#define POLL_S         1.2           /* how often the list is re-read */
#define READ_TIMEOUT_S 5.0

/* The line that separates the master's reading from the stream list. Both come
 * out of one shell so there is one process and one pipe to wait on, and this
 * is how the parser knows where the first ends. */
#define MIX_MARK "__fwm_mixer__"

typedef struct {
    int    id;                 /* stream id, or MASTER_ID */
    char   name[96];
    char   icon[96];           /* icon theme name; "" = draw the speaker */
    double percent;            /* what fwm believes it is, 0..max */
    double shown;              /* eased, the number the bar is drawn at */
    int    muted;
    double hold;               /* seconds a local change still owns the row */
    double appear;             /* 0..1, the row sliding in */
    cairo_surface_t *art;
    int    art_tried;
} MixRow;

struct Mixer {
    struct FwmServer *server;
    bool   open;
    bool   dirty;

    MixRow rows[MIX_ROWS_MAX];
    int    count;
    int    sel;
    int    top;                /* first row drawn, for a list that scrolls */
    bool   held;               /* the selected row has the knob */
    double anim;               /* seconds since the panel opened */

    int    visible;            /* rows that fit the monitor it opened on */

    struct wlr_scene_buffer *overlay;
    struct wlr_scene_buffer *closing;   /* see radial.c: the panel on its way out */
    int    w, h;               /* the buffer */
    int    px, py;
    double panel_h, panel_y;   /* the drawn panel inside that buffer */

    /* The reader, one at a time — the same shape as volume.c's, with a bigger
     * mouth: `pactl list sink-inputs` prints a page per stream. */
    pid_t  pid;
    int    fd;
    double age;
    double since_poll;
    char   buf[65536];
    size_t len;
};

static const MixerConfig  *mconf(Mixer *m) { return &m->server->config.mixer; }
static const VolumeConfig *vconf(Mixer *m) { return &m->server->config.volume; }

static double vol_max(Mixer *m) {
    double x = mconf(m)->max;
    return x > 0.0 ? x : 100.0;
}

/* ── talking to the mixer ────────────────────────────────────────────── */

/* Fire and forget, like a `spawn:` bind — volume.c's, for volume.c's reason:
 * nothing in the compositor may ever wait on the audio stack. */
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
        wlr_log(WLR_ERROR, "mixer: cannot fork for \"%s\"", cmd);
    }
}

/* %v -> the percentage, %i -> the stream. Written by hand rather than with
 * printf because the template is the USER's string, and a stray %d in it would
 * otherwise be a format-string bug with their config as the input. */
static void expand(const char *tmpl, double percent, int id, char *out, size_t cap) {
    char num[16], sid[16];
    snprintf(num, sizeof(num), "%d", (int)(percent + 0.5));
    snprintf(sid, sizeof(sid), "%d", id);
    size_t o = 0;
    for (size_t i = 0; tmpl[i] && o + 1 < cap; i++) {
        const char *sub = NULL;
        if (tmpl[i] == '%' && tmpl[i + 1] == 'v') sub = num;
        else if (tmpl[i] == '%' && tmpl[i + 1] == 'i') sub = sid;
        if (sub) {
            for (size_t k = 0; sub[k] && o + 1 < cap; k++) out[o++] = sub[k];
            i++;
        } else {
            out[o++] = tmpl[i];
        }
    }
    out[o] = '\0';
}

/* Master and streams in one process: [volume] get answers for the first row,
 * [mixer] list for the rest. */
static void reader_start(Mixer *m) {
    if (m->pid > 0) return;
    const char *get  = vconf(m)->get;
    const char *list = mconf(m)->list;
    if (!get[0] && !list[0]) return;

    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "%s; echo %s; %s",
             get[0] ? get : "true", MIX_MARK, list[0] ? list : "true");

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

    m->pid = pid;
    m->fd  = pipefd[0];
    m->age = 0.0;
    m->len = 0;
}

static void reader_stop(Mixer *m) {
    if (m->pid > 0) {
        /* Reaped for real, as the [stats] sensors are: the usual caller is the
         * one below, where the child has already exited and this returns at
         * once, and the other two have just told it to. A WNOHANG here would
         * leave a zombie behind every poll the panel closed on top of. */
        kill(-m->pid, SIGTERM);
        while (waitpid(m->pid, NULL, 0) < 0 && errno == EINTR) {}
        m->pid = 0;
    }
    if (m->fd >= 0) { close(m->fd); m->fd = -1; }
    m->len = 0;
    m->age = 0.0;
}

/* ── reading what the mixer said ─────────────────────────────────────── */

/* The master's line, in whichever shape the configured command prints it.
 * Lifted from volume.c and kept in step with it: wpctl's "Volume: 0.62",
 * pactl's "... /  62% / ...", and a bare number from a command of the user's
 * own all mean the same thing. */
static bool parse_master(const char *s, double *percent, int *muted) {
    *muted = (strstr(s, "MUTED") || strstr(s, "Mute: yes")) ? 1 : 0;

    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        char *end;
        double n = strtod(p, &end);
        if (*end == '%') { *percent = n; return true; }
        if (*end == '.' || n <= 2.0) {
            double frac = strtod(p, &end);
            if (frac <= 2.0) { *percent = frac * 100.0; return true; }
        }
        p = end - 1;
    }
    return false;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* `key = "value"` out of a pactl property line, unquoted, into `out`. */
static bool prop(const char *line, const char *key, char *out, size_t cap) {
    const char *p = skip_ws(line);
    size_t klen = strlen(key);
    if (strncmp(p, key, klen) != 0) return false;
    p = skip_ws(p + klen);
    if (*p != '=') return false;
    p = skip_ws(p + 1);
    bool quoted = (*p == '"');
    if (quoted) p++;
    size_t o = 0;
    while (*p && o + 1 < cap) {
        if (quoted && *p == '"') break;
        if (!quoted && (*p == '\n' || *p == '\r')) break;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out[0] != '\0';
}

/* The first percentage on a pactl Volume line. Both channels carry one and
 * they are the same number in every case a single bar could show anyway. */
static bool parse_percent_line(const char *line, double *out) {
    for (const char *p = line; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        char *end;
        double n = strtod(p, &end);
        if (*end == '%') { *out = n; return true; }
        p = end - 1;
    }
    return false;
}

static void row_defaults(MixRow *r, int id) {
    memset(r, 0, sizeof(*r));
    r->id      = id;
    r->percent = -1.0;
    r->shown   = -1.0;
    r->muted   = -1;
}

/* One stream per line, `id<TAB>percent<TAB>muted<TAB>name` — the escape hatch
 * for a machine pactl does not serve. Anything that does not look like it is
 * left to the block parser. */
static bool parse_tsv(const char *line, MixRow *r) {
    const char *p = skip_ws(line);
    if (*p < '0' || *p > '9') return false;
    char *end;
    long id = strtol(p, &end, 10);
    if (*end != '\t') return false;

    row_defaults(r, (int)id);
    p = end + 1;
    r->percent = strtod(p, &end);
    if (*end != '\t') return false;
    p = end + 1;
    r->muted = (*p == '1' || *p == 'y' || *p == 'Y' || *p == 't' || *p == 'T');
    const char *tab = strchr(p, '\t');
    snprintf(r->name, sizeof(r->name), "%s", tab ? tab + 1 : "stream");
    return true;
}

/* The stream list, into `out`. pactl's blocks, or the TSV above. */
static int parse_streams(Mixer *m, char *text, MixRow *out, int cap) {
    int n = 0;
    MixRow *cur = NULL;
    char weak[96] = "";        /* media.name, used only if nothing better came */

    char *line = text;
    while (line && *line && n <= cap) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        const char *p = skip_ws(line);
        if (strncmp(p, "Sink Input #", 12) == 0 || strncmp(p, "Sink Input#", 11) == 0) {
            if (cur && !cur->name[0])
                snprintf(cur->name, sizeof(cur->name), "%s", weak[0] ? weak : "stream");
            weak[0] = '\0';
            if (n >= cap) break;
            const char *hash = strchr(p, '#');
            cur = &out[n++];
            row_defaults(cur, hash ? atoi(hash + 1) : 0);
        } else if (cur) {
            double pc;
            if (strncmp(p, "Volume:", 7) == 0 && parse_percent_line(p, &pc)) {
                cur->percent = pc;
            } else if (strncmp(p, "Mute:", 5) == 0) {
                cur->muted = strstr(p, "yes") ? 1 : 0;
            } else if (prop(p, "application.name", cur->name, sizeof(cur->name))) {
                /* taken */
            } else if (prop(p, "application.icon_name", cur->icon, sizeof(cur->icon))) {
                /* taken */
            } else if (!cur->icon[0] &&
                       prop(p, "application.process.binary", cur->icon, sizeof(cur->icon))) {
                /* the binary is the icon name on nearly every desktop file */
            } else if (!weak[0]) {
                prop(p, "media.name", weak, sizeof(weak));
            }
        } else if (n < cap) {
            MixRow tsv;
            if (parse_tsv(line, &tsv)) out[n++] = tsv;
        }

        if (!nl) break;
        line = nl + 1;
    }
    if (cur && !cur->name[0])
        snprintf(cur->name, sizeof(cur->name), "%s", weak[0] ? weak : "stream");

    /* A stream with no icon of its own borrows its name: "Firefox" is the icon
     * "firefox" on every theme that has one, and no icon at all is the case
     * the speaker glyph already covers. */
    for (int i = 0; i < n; i++) {
        if (out[i].icon[0]) continue;
        snprintf(out[i].icon, sizeof(out[i].icon), "%s", out[i].name);
        for (char *c = out[i].icon; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
    }
    (void)m;
    return n;
}

/* Fold a fresh reading into the rows on screen.
 *
 * The list is rebuilt rather than patched — a stream can appear, end, or be
 * renamed between two reads — but everything that belongs to the PANEL rather
 * than to the mixer is carried across by stream id: the eased bar position,
 * the decoded icon, and any hold still running over a value the user just
 * moved. The selection and the held row are carried the same way, so a stream
 * ending three rows above the one in your hand does not move it. */
static void merge(Mixer *m, MixRow *fresh, int n) {
    int sel_id  = (m->sel >= 0 && m->sel < m->count) ? m->rows[m->sel].id : MASTER_ID;
    int held_id = m->held ? sel_id : 0;

    MixRow old[MIX_ROWS_MAX];
    int old_count = m->count;
    memcpy(old, m->rows, sizeof(old));

    if (n > MIX_ROWS_MAX - 1) n = MIX_ROWS_MAX - 1;
    for (int i = 0; i < n; i++) {
        MixRow *r = &m->rows[i + 1];
        MixRow *f = &fresh[i];
        MixRow *prev = NULL;
        for (int k = 0; k < old_count; k++)
            if (old[k].id == f->id) { prev = &old[k]; break; }

        double hold = prev ? prev->hold : 0.0;
        *r = *f;
        r->hold   = hold;
        r->appear = prev ? prev->appear : 0.0;
        r->shown  = prev ? prev->shown : f->percent;
        if (prev && hold > 0.0) {          /* the hand still owns this one */
            r->percent = prev->percent;
            r->muted   = prev->muted;
        }
        if (prev && prev->art && strcmp(prev->icon, f->icon) == 0) {
            r->art = prev->art;
            r->art_tried = prev->art_tried;
            prev->art = NULL;               /* moved, not copied */
        }
    }

    /* Icons of rows that are gone belong to nobody now. */
    for (int k = 0; k < old_count; k++)
        if (old[k].art && old[k].id != MASTER_ID) cairo_surface_destroy(old[k].art);

    m->count = n + 1;

    m->sel = 0;
    for (int i = 0; i < m->count; i++)
        if (m->rows[i].id == sel_id) { m->sel = i; break; }
    /* A row that ended while it was held cannot go on being held: the knob
     * would be turning a volume that no longer exists. */
    m->held = held_id != 0 && m->rows[m->sel].id == held_id;
    m->dirty = true;
}

static void reader_finish(Mixer *m) {
    m->buf[m->len < sizeof(m->buf) ? m->len : sizeof(m->buf) - 1] = '\0';

    char *mark = strstr(m->buf, MIX_MARK);
    char *streams = m->buf;
    if (mark) {
        *mark = '\0';
        streams = mark + strlen(MIX_MARK);

        double pc;
        int mu;
        MixRow *master = &m->rows[0];
        if (parse_master(m->buf, &pc, &mu) && master->hold <= 0.0) {
            master->percent = pc;
            master->muted   = mu;
        }
    }

    MixRow fresh[MIX_ROWS_MAX];
    int n = parse_streams(m, streams, fresh, MIX_ROWS_MAX - 1);
    reader_stop(m);
    merge(m, fresh, n);
}

/* ── moving a volume ─────────────────────────────────────────────────── */

static void row_apply(Mixer *m, MixRow *r, double want) {
    double max = vol_max(m);
    if (want < 0.0) want = 0.0;
    if (want > max) want = max;
    r->percent = want;
    r->hold    = HOLD_S;

    const char *tmpl = r->id == MASTER_ID ? vconf(m)->set : mconf(m)->set;
    if (tmpl[0]) {
        char cmd[600];
        expand(tmpl, want, r->id, cmd, sizeof(cmd));
        run_detached(cmd);
    }
    m->dirty = true;
}

static void row_step(Mixer *m, MixRow *r, double delta) {
    if (r->percent < 0.0) {
        /* Nothing read back yet — the first read is only a poll away, and a
         * step applied to a value we do not have would be a guess drawn as a
         * fact. */
        return;
    }
    row_apply(m, r, r->percent + delta);
}

static void row_mute(Mixer *m, MixRow *r) {
    const char *tmpl = r->id == MASTER_ID ? vconf(m)->mute : mconf(m)->mute;
    if (!tmpl[0]) return;
    char cmd[600];
    expand(tmpl, r->percent < 0.0 ? 0.0 : r->percent, r->id, cmd, sizeof(cmd));
    run_detached(cmd);
    /* Predicted like a step is, and unknown stays unknown: there is nothing to
     * flip until the first read lands. */
    if (r->muted >= 0) r->muted = !r->muted;
    r->hold = HOLD_S;
    m->dirty = true;
}

static MixRow *sel_row(Mixer *m) {
    if (m->sel < 0 || m->sel >= m->count) return NULL;
    return &m->rows[m->sel];
}

/* ── layout ──────────────────────────────────────────────────────────── */

static int rows_shown(Mixer *m) {
    return m->count < m->visible ? m->count : m->visible;
}

/* The panel is drawn inside a buffer sized for a FULL list and centred in it,
 * so a stream that ends does not have to rebuild the overlay to keep the panel
 * in the middle of the screen. */
static void relayout(Mixer *m) {
    m->panel_h = HEAD_H + PAD + rows_shown(m) * ROW_H + PAD;
    /* Room for the line that says nothing else is playing, on the panel that
     * has to say it. */
    if (m->count <= 1) m->panel_h += 22.0;
    m->panel_y = (m->h - m->panel_h) / 2.0;
    if (m->panel_y < 0.0) m->panel_y = 0.0;
}

/* Keep the selection inside the window of rows being drawn. */
static void scroll_to_sel(Mixer *m) {
    int vis = rows_shown(m);
    if (m->sel < m->top) m->top = m->sel;
    if (m->sel > m->top + vis - 1) m->top = m->sel - vis + 1;
    if (m->top > m->count - vis) m->top = m->count - vis;
    if (m->top < 0) m->top = 0;
}

static double row_y(Mixer *m, int i) {
    return m->panel_y + HEAD_H + PAD + (i - m->top) * ROW_H;
}

/* ── drawing ─────────────────────────────────────────────────────────── */

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

static void ensure_art(Mixer *m, MixRow *r) {
    if (r->art_tried || r->id == MASTER_ID) return;
    r->art_tried = 1;
    if (r->icon[0])
        r->art = icon_load(r->icon, m->server->config.decor.icon_theme, ICON_SZ);
}

static void draw_text(cairo_t *cr, PangoLayout *layout, const char *text,
                      double x, double y, double max_w) {
    pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_text(layout, text, -1);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    (void)tw;
    cairo_move_to(cr, x, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_layout_set_width(layout, -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
}

static void draw_right(cairo_t *cr, PangoLayout *layout, const char *text,
                       double right, double y) {
    int tw, th;
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, right - tw, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
}

static void draw_mixer(cairo_t *cr, int w, int h, void *data) {
    Mixer *m = data;
    (void)w; (void)h;

    const FwmTheme *thm = theme_get();
    double alpha = m->server->config.decor.launcher_opacity;
    double px = 0.0, py = m->panel_y;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    /* ── the panel ── */
    rounded_rect(cr, px, py, PANEL_W, m->panel_h, 14.0);
    cairo_set_source_rgba(cr, thm->pill[0], thm->pill[1], thm->pill[2], alpha);
    cairo_fill(cr);

    /* ── the head ── */
    cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
    modes_icon(cr, MODE_ICON_SOUND, px + PAD + 6.0, py + 11.0, 15.0);
    PangoFontDescription *bold = pango_font_description_from_string("sans bold 10");
    pango_layout_set_font_description(layout, bold);
    pango_font_description_free(bold);
    draw_text(cr, layout, "Sound", px + PAD + 30.0, py + 18.0, 200.0);

    /* What the knob does right now, said in the corner where the hand can find
     * it: this panel is the one place in fwm where turning means two different
     * things, and which one it means has to be readable at a glance. */
    PangoFontDescription *small = pango_font_description_from_string("sans 9");
    pango_layout_set_font_description(layout, small);
    pango_font_description_free(small);
    cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
    draw_right(cr, layout, m->held ? "turn: volume · press: let go"
                                   : "turn: pick · press: hold",
               px + PANEL_W - PAD - 6.0, py + 18.0);

    PangoFontDescription *base = pango_font_description_from_string("sans 10");
    pango_layout_set_font_description(layout, base);
    pango_font_description_free(base);

    /* ── the rows ── */
    int vis = rows_shown(m);
    for (int vi = 0; vi < vis; vi++) {
        int i = m->top + vi;
        if (i >= m->count) break;
        MixRow *r = &m->rows[i];

        double ease = r->appear;
        if (ease <= 0.001) continue;
        double slide = (1.0 - ease) * 14.0;      /* rows arrive from the right */
        double y  = row_y(m, i) + 0.0;
        double rx = px + PAD + slide;
        double rw = PANEL_W - 2.0 * PAD;
        double cy = y + ROW_H / 2.0;
        bool focused = (i == m->sel);
        bool held    = focused && m->held;

        cairo_save(cr);
        cairo_push_group(cr);

        if (focused) {
            rounded_rect(cr, rx, y + 2.0, rw, ROW_H - 4.0, ROW_R);
            cairo_set_source_rgba(cr, thm->sel[0], thm->sel[1], thm->sel[2], 1.0);
            cairo_fill_preserve(cr);
            /* Held is a ring, not a brighter fill: on a theme whose sel and
             * pill sit close together a fill alone says nothing, and this is
             * the state the whole panel turns on. */
            if (held) {
                cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
                cairo_set_line_width(cr, 2.0);
                cairo_stroke(cr);
            } else {
                cairo_new_path(cr);
            }
        }

        /* Icon. The master has no application to borrow one from, and a stream
         * whose icon the theme does not have is in the same position — both
         * get the speaker the modes menu already draws. */
        ensure_art(m, r);
        if (r->art) {
            int aw = cairo_image_surface_get_width(r->art);
            int ah = cairo_image_surface_get_height(r->art);
            cairo_set_source_surface(cr, r->art, rx + 10.0, cy - ah / 2.0);
            cairo_paint(cr);
            (void)aw;
        } else {
            if (r->id == MASTER_ID)
                cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
            else
                cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
            modes_icon(cr, MODE_ICON_SOUND, rx + 10.0, cy - 11.0, 22.0);
        }

        /* Name over its bar. */
        if (focused) cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        else         cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        double text_x = px + BAR_X + slide;
        double bar_w  = PANEL_W - PAD - VAL_W - BAR_X - 6.0;
        draw_text(cr, layout, r->id == MASTER_ID ? "Master" : r->name,
                  text_x, cy - 9.0, bar_w);

        /* The bar. A level fwm has not been told yet is drawn as an empty
         * track rather than as zero: those are different things, and one of
         * them would have the user turning a knob to fix a number that was
         * never real. */
        double frac = r->shown >= 0.0 ? r->shown / vol_max(m) : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        double by = cy + 8.0;
        rounded_rect(cr, text_x, by, bar_w, BAR_H, BAR_H / 2.0);
        cairo_set_source_rgba(cr, thm->dim[0], thm->dim[1], thm->dim[2], 0.85);
        cairo_fill(cr);
        if (frac > 0.001 && r->muted != 1) {
            rounded_rect(cr, text_x, by, bar_w * frac, BAR_H, BAR_H / 2.0);
            if (held || focused)
                cairo_set_source_rgb(cr, thm->accent[0], thm->accent[1], thm->accent[2]);
            else
                cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
            cairo_fill(cr);
        }

        /* The reading. */
        char val[16];
        if (r->muted == 1)          snprintf(val, sizeof(val), "muted");
        else if (r->percent < 0.0)  snprintf(val, sizeof(val), "—");
        else                        snprintf(val, sizeof(val), "%d%%", (int)(r->percent + 0.5));
        if (r->muted == 1 || r->percent < 0.0)
            cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        else if (focused)
            cairo_set_source_rgb(cr, thm->text[0], thm->text[1], thm->text[2]);
        else
            cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        draw_right(cr, layout, val, px + PANEL_W - PAD - 8.0 + slide, cy);

        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, ease);
        cairo_restore(cr);
    }

    /* Nothing but the master: say so, rather than leaving a panel that looks
     * like it failed to load. */
    if (m->count <= 1) {
        cairo_set_source_rgb(cr, thm->muted[0], thm->muted[1], thm->muted[2]);
        draw_text(cr, layout, "nothing else is playing",
                  px + BAR_X, m->panel_y + HEAD_H + PAD + ROW_H + 10.0, 300.0);
    } else if (m->top == 0) {
        /* A hairline under the master: it is the only row that is not an
         * application, and the one whose reach is the whole machine. */
        double ly = row_y(m, 0) + ROW_H;
        cairo_rectangle(cr, px + PAD + 10.0, ly - 0.5, PANEL_W - 2.0 * PAD - 20.0, 1.0);
        cairo_set_source_rgba(cr, thm->dim[0], thm->dim[1], thm->dim[2], 0.6);
        cairo_fill(cr);
    }

    /* More rows than fit: a thin thumb down the right edge, so a list that
     * scrolls looks like one. */
    if (m->count > vis) {
        double track_y = m->panel_y + HEAD_H + PAD;
        double track_h = vis * ROW_H;
        double th = track_h * ((double)vis / (double)m->count);
        double ty = track_y + track_h * ((double)m->top / (double)m->count);
        rounded_rect(cr, px + PANEL_W - 6.0, ty, 3.0, th, 1.5);
        cairo_set_source_rgba(cr, thm->accent[0], thm->accent[1], thm->accent[2], 0.7);
        cairo_fill(cr);
    }

    g_object_unref(layout);
}

/* ── open / close ────────────────────────────────────────────────────── */

static void closing_done(void *data) {
    Mixer *m = data;
    m->closing = NULL;
}

static void closing_cancel(Mixer *m) {
    if (!m->closing) return;
    struct wlr_scene_buffer *buf = m->closing;
    m->closing = NULL;
    cairo_overlay_destroy(buf);
}

static void art_free(Mixer *m) {
    for (int i = 0; i < m->count; i++) {
        if (m->rows[i].art) cairo_surface_destroy(m->rows[i].art);
        m->rows[i].art = NULL;
        m->rows[i].art_tried = 0;
    }
}

static void mixer_close_ex(Mixer *m, bool animate) {
    if (!m->open) return;
    m->open = false;      /* input goes back to the client now; pixels linger */
    if (m->overlay) {
        closing_cancel(m);
        if (animate) {
            m->closing = m->overlay;
            cairo_overlay_animate_out(m->overlay, PANEL_ANIM_MS, PANEL_RISE_PX,
                                      closing_done, m);
        } else {
            cairo_overlay_destroy(m->overlay);
        }
        m->overlay = NULL;
    }
    reader_stop(m);
    art_free(m);
    m->count = 0;
    m->held  = false;
    m->sel   = 0;
    m->top   = 0;
}

static void mixer_close(Mixer *m) { mixer_close_ex(m, true); }

static void mixer_open(Mixer *m) {
    if (m->open) return;
    FwmServer *server = m->server;

    if (!mconf(m)->list[0] && !vconf(m)->get[0]) {
        wlr_log(WLR_ERROR, "mixer: no mixer — pactl is not on PATH and [mixer] "
                           "names no commands of its own");
        return;
    }

    closing_cancel(m);

    struct wlr_box screen;
    server_active_output_box(server, &screen);

    /* How much list this monitor can hold. A panel taller than the screen is
     * worse than one that scrolls. */
    m->visible = (int)((screen.height - 80.0 - HEAD_H - 2.0 * PAD) / ROW_H);
    if (m->visible > ROWS_VISIBLE) m->visible = ROWS_VISIBLE;
    if (m->visible < 3) m->visible = 3;

    m->w = (int)PANEL_W;
    m->h = (int)(HEAD_H + PAD + m->visible * ROW_H + PAD);
    m->overlay = cairo_overlay_create(server->layer_overlay, m->w, m->h);
    if (!m->overlay) return;

    /* The master is row zero and is always there, read or not: it is the row
     * the hand lands on, and a panel whose first row appeared a second late
     * would move everything under the selection just as it was being used. */
    row_defaults(&m->rows[0], MASTER_ID);
    snprintf(m->rows[0].name, sizeof(m->rows[0].name), "Master");
    m->count = 1;
    m->sel   = 0;
    m->top   = 0;
    m->held  = false;
    m->anim  = 0.0;
    m->since_poll = POLL_S;      /* read immediately, not a poll from now */

    /* Centred on the monitor the user is at, like the ring. */
    m->px = screen.x + (screen.width  - m->w) / 2;
    m->py = screen.y + (screen.height - m->h) / 2;

    relayout(m);
    wlr_scene_node_set_position(&m->overlay->node, m->px, m->py);
    cairo_overlay_animate_in(m->overlay, PANEL_ANIM_MS, PANEL_RISE_PX);
    m->open  = true;
    m->dirty = true;
}

/* ── public api ──────────────────────────────────────────────────────── */

Mixer *mixer_create(struct FwmServer *server) {
    Mixer *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->server = server;
    m->fd = -1;
    return m;
}

void mixer_destroy(Mixer *m) {
    if (!m) return;
    mixer_close_ex(m, false);
    closing_cancel(m);
    reader_stop(m);
    art_free(m);
    free(m);
}

void mixer_toggle(Mixer *m) {
    if (!m) return;
    if (m->open) mixer_close(m);
    else         mixer_open(m);
}

bool mixer_is_open(Mixer *m) { return m && m->open; }

bool mixer_busy(Mixer *m) { return m && (m->open || m->pid > 0); }

static void step_sel(Mixer *m, int delta) {
    if (m->count <= 0) return;
    /* The list wraps, as the ring does: the knob it is driven by has no ends,
     * so a list with them is a list that stops answering the hand halfway
     * through a turn. Off the top is the last row, off the bottom the master. */
    m->sel = (m->sel + delta % m->count + m->count) % m->count;
    scroll_to_sel(m);
    m->dirty = true;
}

static void toggle_hold(Mixer *m) {
    MixRow *r = sel_row(m);
    if (!r) return;
    /* Nothing to take hold of on a row whose level has not been read yet, and
     * nothing to move if the config gave no way to move it. */
    const char *tmpl = r->id == MASTER_ID ? vconf(m)->set : mconf(m)->set;
    if (!m->held && !tmpl[0]) {
        wlr_log(WLR_ERROR, "mixer: nothing to set volume with — see [mixer] set");
        return;
    }
    m->held = !m->held;
    m->dirty = true;
}

bool mixer_handle_key(Mixer *m, xkb_keysym_t sym) {
    if (!m || !m->open) return false;
    m->dirty = true;
    double step = mconf(m)->step > 0.0 ? mconf(m)->step : 5.0;
    MixRow *r = sel_row(m);

    switch (sym) {
    /* One thing at a time on the way out: a held row is let go first, and only
     * a panel with nothing in its hand closes. Otherwise a mis-turn while
     * holding a row would cost the whole panel. */
    case XKB_KEY_Escape:
    case XKB_KEY_BackSpace:
        if (m->held) m->held = false;
        else         mixer_close(m);
        return true;

    /* The knob. Its three keys are the volume keys, which is exactly why the
     * panel is asked for them before [binds] is — see server_input.c. */
    case XKB_KEY_XF86AudioRaiseVolume:
        if (m->held && r) row_step(m, r, step * server_knob_step(m->server, +1));
        else              step_sel(m, +server_knob_step(m->server, +1));
        return true;
    case XKB_KEY_XF86AudioLowerVolume:
        if (m->held && r) row_step(m, r, -step * server_knob_step(m->server, -1));
        else              step_sel(m, -server_knob_step(m->server, -1));
        return true;
    case XKB_KEY_XF86AudioMute:
        toggle_hold(m);
        return true;

    /* The same panel from a keyboard with no knob: the arrows split the two
     * meanings the knob has to share, so holding a row is not needed to move
     * one — up and down walk, left and right move the volume of whatever the
     * selection is on. */
    case XKB_KEY_Down:
    case XKB_KEY_Tab:
        step_sel(m, +1);
        return true;
    case XKB_KEY_Up:
    case XKB_KEY_ISO_Left_Tab:
        step_sel(m, -1);
        return true;
    case XKB_KEY_Right:
        if (r) row_step(m, r, step);
        return true;
    case XKB_KEY_Left:
        if (r) row_step(m, r, -step);
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        toggle_hold(m);
        return true;
    case XKB_KEY_Home:
        m->sel = 0;
        scroll_to_sel(m);
        return true;
    case XKB_KEY_End:
        m->sel = m->count - 1;
        scroll_to_sel(m);
        return true;
    /* Mute needs a key of its own: the knob's press is spent on taking hold,
     * which is the one thing the knob cannot do any other way. */
    case XKB_KEY_m:
    case XKB_KEY_M:
        if (r) row_mute(m, r);
        return true;
    default:
        break;
    }

    /* Straight to a row by its place in the list, 1 being the master. */
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
        int idx = (int)(sym - XKB_KEY_1);
        if (idx < m->count) { m->sel = idx; scroll_to_sel(m); }
        return true;
    }

    return true;   /* swallow everything else while open */
}

/* Row under a panel-local point, or -1. */
static int row_at(Mixer *m, double x, double y) {
    if (x < PAD || x > PANEL_W - PAD) return -1;
    int vis = rows_shown(m);
    for (int vi = 0; vi < vis; vi++) {
        int i = m->top + vi;
        if (i >= m->count) break;
        double ry = row_y(m, i);
        if (y >= ry && y < ry + ROW_H) return i;
    }
    return -1;
}

void mixer_handle_motion(Mixer *m, double lx, double ly) {
    if (!m || !m->open) return;
    int hit = row_at(m, lx - m->px, ly - m->py);
    /* Hover moves the selection but never takes hold: holding is a decision,
     * and a pointer crossing the panel on its way somewhere else must not make
     * the wheel start moving somebody's volume. */
    if (hit >= 0 && hit != m->sel) {
        m->sel  = hit;
        m->held = false;
        m->dirty = true;
    }
}

bool mixer_handle_button(Mixer *m, double lx, double ly, bool pressed) {
    if (!m || !m->open) return false;
    if (!pressed) return true;         /* swallow releases too */

    double x = lx - m->px, y = ly - m->py;
    int hit = row_at(m, x, y);
    if (hit >= 0) {
        m->sel = hit;
        MixRow *r = &m->rows[hit];

        /* On the bar, the click IS the value — a mouse can aim, and asking it
         * to take hold and then spin a wheel would be the knob's workflow
         * imposed on a device that does not need it. */
        double bar_x = BAR_X;
        double bar_w = PANEL_W - PAD - VAL_W - BAR_X - 6.0;
        double by    = row_y(m, hit) + ROW_H / 2.0 + 8.0;
        if (x >= bar_x && x <= bar_x + bar_w && y >= by - 8.0 && y <= by + BAR_H + 8.0) {
            row_apply(m, r, ((x - bar_x) / bar_w) * vol_max(m));
            return true;
        }
        toggle_hold(m);
        return true;
    }
    /* Inside the panel but on no row — the head, or the margin — is not a way
     * out: only a click on what is behind the panel closes it. */
    if (x >= 0.0 && x <= PANEL_W && y >= m->panel_y && y <= m->panel_y + m->panel_h)
        return true;

    mixer_close(m);
    return true;
}

bool mixer_handle_axis(Mixer *m, double delta) {
    if (!m || !m->open || delta == 0.0) return false;
    int dir = delta > 0.0 ? -1 : +1;    /* wheel down walks down the list */
    MixRow *r = sel_row(m);
    if (m->held && r) {
        double step = mconf(m)->step > 0.0 ? mconf(m)->step : 5.0;
        row_step(m, r, dir > 0 ? step : -step);
    } else {
        step_sel(m, dir > 0 ? +1 : -1);
    }
    return true;
}

/* ── the tick ────────────────────────────────────────────────────────── */

/* Drain the reader's pipe, exactly as volume.c does. */
static void reader_pump(Mixer *m, double dt) {
    if (m->pid <= 0) return;
    m->age += dt;

    for (;;) {
        if (m->len >= sizeof(m->buf) - 1) { reader_finish(m); return; }
        ssize_t n = read(m->fd, m->buf + m->len, sizeof(m->buf) - 1 - m->len);
        if (n > 0) { m->len += (size_t)n; continue; }
        if (n == 0) { reader_finish(m); return; }          /* EOF: it is done */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* still thinking */
        reader_stop(m);                                    /* the pipe broke */
        return;
    }

    if (m->age > READ_TIMEOUT_S) {
        wlr_log(WLR_INFO, "mixer: \"%s\" did not answer in %.0fs — killed",
                mconf(m)->list, READ_TIMEOUT_S);
        reader_stop(m);
    }
}

void mixer_tick(Mixer *m, double dt) {
    if (!m) return;

    /* Nothing is out asking once the panel is closed — the close reaps it —
     * so this is a no-op there and the early return below costs nothing. */
    reader_pump(m, dt);
    if (!m->open) return;

    m->anim += dt;
    m->since_poll += dt;
    if (m->since_poll >= POLL_S && m->pid <= 0) {
        m->since_poll = 0.0;
        reader_start(m);
    }

    bool moving = false;
    double k = 1.0 - exp(-BAR_RATE * dt);
    for (int i = 0; i < m->count; i++) {
        MixRow *r = &m->rows[i];
        if (r->hold > 0.0) { r->hold -= dt; moving = true; }

        /* Rows arrive one after another, top first — the launcher's stagger,
         * for the launcher's reason: a list that appears all at once reads as
         * a picture, and one that arrives reads as a list. */
        double want_appear = 0.0;
        int vi = i - m->top;
        if (vi >= 0 && vi < rows_shown(m)) {
            double t = (m->anim - vi * ROW_STAGGER) / ROW_RISE;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            want_appear = t * t * (3.0 - 2.0 * t);
        }
        if (fabs(want_appear - r->appear) > 0.002) { r->appear = want_appear; moving = true; }
        else r->appear = want_appear;

        double want_bar = r->percent;
        if (r->shown < 0.0) r->shown = want_bar;    /* the first reading lands, it does not slide */
        else if (want_bar >= 0.0 && fabs(want_bar - r->shown) > 0.05) {
            r->shown += (want_bar - r->shown) * k;
            moving = true;
        } else if (want_bar >= 0.0) {
            r->shown = want_bar;
        }
    }

    relayout(m);
    scroll_to_sel(m);

    if (!moving && !m->dirty) return;
    m->dirty = false;
    cairo_overlay_update(m->overlay, draw_mixer, m);
}
