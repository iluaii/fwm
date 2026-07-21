#include "session.h"
#include "server.h"
#include "view.h"
#include "physics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include <wlr/util/log.h>

#define SESSION_MAX_ENTRIES 64
#define SESSION_LINE_MAX    2048
/* One write every few seconds at most. The file is tiny, but the point is to
 * avoid touching the disk on every frame while a window is being dragged. */
#define SESSION_SAVE_PERIOD_SEC 3.0

struct PendingEntry {
    char argv_key[SESSION_LINE_MAX]; /* tab-joined argv, as written to the file */
    int  desktop;
    int  claimed;
};

struct FwmSessionState {
    struct PendingEntry pending[SESSION_MAX_ENTRIES];
    int   pending_count;
    char  last_written[SESSION_MAX_ENTRIES * 128];
    double last_save_time;
};

/* ── helpers ─────────────────────────────────────────────────────────── */

static void session_path(char *buf, size_t cap) {
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (state && state[0]) snprintf(buf, cap, "%s/fwm/session", state);
    else if (home)         snprintf(buf, cap, "%s/.local/state/fwm/session", home);
    else                   snprintf(buf, cap, ".fwm-session");
}

static void mkdir_parents(const char *file) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
    }
    mkdir(dir, 0755);
}

/* The pid behind a view's client. xdg clients are asked through the Wayland
 * connection; XWayland surfaces carry the pid themselves. */
static pid_t view_pid(struct FwmView *view) {
    if (view->type == FWM_VIEW_XDG) {
        if (!view->xdg_toplevel || !view->xdg_toplevel->resource) return 0;
        struct wl_client *client = wl_resource_get_client(view->xdg_toplevel->resource);
        if (!client) return 0;
        pid_t pid = 0;
        wl_client_get_credentials(client, &pid, NULL, NULL);
        return pid;
    }
    return view->xwl_surface ? (pid_t)view->xwl_surface->pid : 0;
}

/* /proc/<pid>/cmdline is a NUL-separated argv. Join it with tabs, which is
 * what the state file stores and what pending entries are matched on. Returns
 * 0 when the process is gone or the cmdline is unusable. */
static int pid_argv_key(pid_t pid, char *out, size_t cap) {
    if (pid <= 0) return 0;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char raw[SESSION_LINE_MAX];
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    raw[n] = '\0';

    size_t used = 0;
    for (size_t i = 0; i < n; ) {
        size_t len = strlen(raw + i);
        if (len == 0) { i++; continue; }
        /* A tab inside an argument would corrupt the line on the way back in;
         * such an argument is vanishingly rare and not worth a quoting
         * scheme, so the entry is simply dropped. */
        if (strchr(raw + i, '\t')) return 0;
        int w = snprintf(out + used, cap - used, "%s%s", used ? "\t" : "", raw + i);
        if (w < 0 || (size_t)w >= cap - used) return 0;
        used += (size_t)w;
        i += len + 1;
    }
    return used > 0;
}

static struct FwmSessionState *state_of(struct FwmServer *server) {
    if (!server->session_state) {
        server->session_state = calloc(1, sizeof(struct FwmSessionState));
    }
    return (struct FwmSessionState *)server->session_state;
}

/* ── saving ──────────────────────────────────────────────────────────── */

/* Build the whole file contents. One line per distinct application: several
 * windows of one process (a browser, a terminal with two windows) must not
 * relaunch it several times, so entries are deduplicated by pid. */
static void build_snapshot(struct FwmServer *server, char *out, size_t cap) {
    out[0] = '\0';
    size_t used = 0;

    pid_t seen[SESSION_MAX_ENTRIES];
    int seen_count = 0;

    struct FwmView *view;
    wl_list_for_each(view, &server->views, link) {
        pid_t pid = view_pid(view);
        if (pid <= 0) continue;

        int dup = 0;
        for (int i = 0; i < seen_count; i++) if (seen[i] == pid) { dup = 1; break; }
        if (dup) continue;
        if (seen_count >= SESSION_MAX_ENTRIES) break;
        seen[seen_count++] = pid;

        char key[SESSION_LINE_MAX];
        if (!pid_argv_key(pid, key, sizeof(key))) continue;

        PhysicsBody *b = physics_find_body(&server->physics, view->id);
        int desktop = b ? b->desktop_id
                        : (server->screen_width > 0 ? view->x / server->screen_width : 0);
        if (desktop < 0) desktop = 0;
        if (desktop > 9) desktop = 9;

        int w = snprintf(out + used, cap - used, "%d\t%s\n", desktop, key);
        if (w < 0 || (size_t)w >= cap - used) break;
        used += (size_t)w;
    }
}

void session_maybe_save(struct FwmServer *server) {
    if (server->config.session.restore == SESSION_RESTORE_NEVER) return;

    struct FwmSessionState *st = state_of(server);
    if (!st) return;

    /* Rate-limit against the frame clock rather than a timer of its own. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double t = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
    if (st->last_save_time != 0.0 && t - st->last_save_time < SESSION_SAVE_PERIOD_SEC) return;
    st->last_save_time = t;

    char snapshot[sizeof(st->last_written)];
    build_snapshot(server, snapshot, sizeof(snapshot));

    /* Comparing against the last write is what makes dragging a window between
     * desktops get picked up without needing every caller to flag it dirty. */
    if (strcmp(snapshot, st->last_written) == 0) return;

    char sp[512];
    session_path(sp, sizeof(sp));
    mkdir_parents(sp);

    FILE *f = fopen(sp, "w");
    if (!f) {
        wlr_log(WLR_ERROR, "cannot write session state to %s", sp);
        return;
    }
    fputs(snapshot, f);
    fclose(f);
    snprintf(st->last_written, sizeof(st->last_written), "%s", snapshot);
}

/* ── restoring ───────────────────────────────────────────────────────── */

/* Split a tab-joined argv key and exec it. No shell: the arguments came out of
 * /proc verbatim and must go back in verbatim. */
static void spawn_argv_key(const char *key) {
    char buf[SESSION_LINE_MAX];
    snprintf(buf, sizeof(buf), "%s", key);

    char *argv[64];
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "\t", &save);
         tok && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1;
         tok = strtok_r(NULL, "\t", &save)) {
        argv[argc++] = tok;
    }
    argv[argc] = NULL;
    if (argc == 0) return;

    if (fork() == 0) {
        setsid();
        execvp(argv[0], argv);
        _exit(1);
    }
}

void session_restore(struct FwmServer *server) {
    if (server->config.session.restore == SESSION_RESTORE_NEVER) return;

    struct FwmSessionState *st = state_of(server);
    if (!st) return;

    char sp[512];
    session_path(sp, sizeof(sp));
    FILE *f = fopen(sp, "r");
    if (!f) return;

    char line[SESSION_LINE_MAX];
    while (fgets(line, sizeof(line), f) && st->pending_count < SESSION_MAX_ENTRIES) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (!len) continue;

        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';

        int desktop = atoi(line);
        if (desktop < 0) desktop = 0;
        if (desktop > 9) desktop = 9;

        struct PendingEntry *e = &st->pending[st->pending_count++];
        e->desktop = desktop;
        e->claimed = 0;
        snprintf(e->argv_key, sizeof(e->argv_key), "%s", tab + 1);

        spawn_argv_key(e->argv_key);
    }
    fclose(f);

    if (st->pending_count > 0)
        wlr_log(WLR_INFO, "session: relaunching %d application(s)", st->pending_count);
}

int session_claim_desktop(struct FwmServer *server, struct FwmView *view) {
    struct FwmSessionState *st = (struct FwmSessionState *)server->session_state;
    if (!st || st->pending_count == 0) return -1;

    char key[SESSION_LINE_MAX];
    if (!pid_argv_key(view_pid(view), key, sizeof(key))) return -1;

    for (int i = 0; i < st->pending_count; i++) {
        struct PendingEntry *e = &st->pending[i];
        if (e->claimed) continue;
        if (strcmp(e->argv_key, key) != 0) continue;
        e->claimed = 1;
        return e->desktop;
    }
    return -1;
}

/* Called only on the way out of a NORMAL shutdown — a crash never reaches
 * server_destroy. Removing the file here is what lets the next start tell the
 * two apart: a state file that survived means the last run died. */
void session_clear_on_clean_exit(struct FwmServer *server) {
    if (server->config.session.restore == SESSION_RESTORE_ALWAYS) return;

    char sp[512];
    session_path(sp, sizeof(sp));
    unlink(sp);
}

void session_finish(struct FwmServer *server) {
    free(server->session_state);
    server->session_state = NULL;
}
