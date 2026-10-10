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

/* The liquid, in the compositor: where it stands, when it moves, what drives
 * it and how it reaches the screen. liquid.c is the liquid itself and knows
 * none of this; liquid_draw.c turns its particles into a picture.
 *
 * ON A THREAD OF ITS OWN. A step of twelve thousand particles is five
 * milliseconds on a calm frame and twenty on the frame a splash lands, and
 * nothing may block the event loop — so the step runs on a worker, one frame
 * behind. Each tick the compositor looks whether the last step has finished;
 * if it has, it copies the particles out for the picture, hands over whatever
 * was asked for since (a pour, a clear, a new box), and starts the next step.
 * If it has not, the tick goes on without it and the liquid simply shows the
 * picture it already had. The worker touches nothing but the FwmLiquid, and
 * the compositor touches the FwmLiquid only while the worker is waiting;
 * `busy` is the whole of the handover.
 *
 * ONE DESKTOP. The liquid stands on the desktop it was first poured on, until
 * it is cleared. It is stepped only while a monitor is showing that desktop
 * and nothing covers it — a fullscreen window over it, or the desktop strip —
 * which is the rule the video wallpaper and the grass already keep: what
 * nobody can see does not cost anything. Its walls are the edges of the
 * monitor showing it, and its floor is the floor the windows stand on.
 *
 * Its gravity is that desktop's, exactly as a window there feels it — the
 * [physics] profile and the gravity switch — handed over as a vector each
 * step. The liquid does not know what the switch is; it only finds the vector
 * changed. */

#include "server.h"
#include "view.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/input-event-codes.h>
#include <pixman.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "expo.h"
#include "glass.h"
#include "liquid.h"
#include "liquid_draw.h"
#include "physics.h"
#include "server_internal.h"
#include "theme.h"
#include "ui/cairo_overlay.h"
#include "ui/liquid_form.h"
#include "ui/liquids_menu.h"
#include "ui/modes.h"
#include "ui/toast.h"
#include "ui/tray.h"

/* The most simulated time one step may cover. A step that fell behind — the
 * worker was still busy at the last tick, or the tick itself was late — is
 * handed the time it owes up to this, and the rest is lost: the liquid runs
 * slow for a moment rather than being handed a second it would have to take
 * in sixteen pieces. Two frames, the same allowance the window physics has. */
#define LIQUID_MAX_DT (2.0 / 60.0)

enum { FULL_BUDGET = 1, FULL_BRIM = 2 };

struct FwmLiquidRun {
    FwmLiquid *sim;
    int desktop;
    int kind;               /* which liquid it is: an id in server->liquid_kinds */

    /* The worker. `job`, `quit` and the job's inputs are under `lock`; `busy`
     * is set by the compositor when it hands a step over and cleared by the
     * worker when the step is done, and while it is set the FwmLiquid is the
     * worker's. */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool job, quit;
    double job_dt, job_gx, job_gy;
    atomic_bool busy;
    double job_ms;          /* how long the last step took; written before busy clears */

    /* Everything below is the compositor's alone. */
    bool stepped;           /* a step was handed over and has not been looked at */
    double owed;            /* seconds of simulation not yet handed over */
    bool have_g;
    double last_gx, last_gy;
    double box_w, box_h;

    int pour;               /* particles asked for and not yet poured */
    double pour_x, pour_y;
    bool clear;
    /* What the bucket in the hand has poured and drained since the last step,
     * handed over together between two steps, at where the hand last was. The
     * stream leaves the bucket at the hand's own speed: swing it and the
     * water flies. */
    int stream;
    double stream_x, stream_y, stream_vx, stream_vy;
    int drain;
    double drain_x, drain_y;
    /* The last stream found no room: FULL_BUDGET, the budget spent, or
     * FULL_BRIM, the desktop full to the top; 0 while there is room. */
    int full;

    float *snap;            /* the particles as (x, y) pairs, for the picture */
    int snap_n, snap_cap;
    bool asleep;            /* as of the last copy */
    int substeps, iters;
    double vmax;
    double step_ms;         /* job_ms, read while the worker was waiting */

    FwmLiquidPaint *paint;
    bool dirty;             /* the snapshot is newer than the picture */
    struct wlr_box damage;  /* what the picture last changed */
    bool told_no_gl;

    bool dbg;
    double dbg_at, dbg_ms, dbg_worst;
    int dbg_steps;
};

static void hand_drop(FwmServer *server, bool quiet);

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ── the worker ──────────────────────────────────────────────────────── */

static void *liquid_worker(void *data) {
    struct FwmLiquidRun *run = data;
    pthread_mutex_lock(&run->lock);
    for (;;) {
        while (!run->job && !run->quit) pthread_cond_wait(&run->cond, &run->lock);
        if (run->quit) break;
        run->job = false;
        double dt = run->job_dt, gx = run->job_gx, gy = run->job_gy;
        pthread_mutex_unlock(&run->lock);

        double t0 = now_s();
        liquid_step(run->sim, dt, gx, gy);
        run->job_ms = (now_s() - t0) * 1000.0;
        atomic_store_explicit(&run->busy, false, memory_order_release);

        pthread_mutex_lock(&run->lock);
    }
    pthread_mutex_unlock(&run->lock);
    return NULL;
}

static bool run_idle(const struct FwmLiquidRun *run) {
    return !atomic_load_explicit(&run->busy, memory_order_acquire);
}

static void run_submit(struct FwmLiquidRun *run, double dt, double gx, double gy) {
    pthread_mutex_lock(&run->lock);
    run->job_dt = dt;
    run->job_gx = gx;
    run->job_gy = gy;
    run->job = true;
    atomic_store_explicit(&run->busy, true, memory_order_relaxed);
    pthread_cond_signal(&run->cond);
    pthread_mutex_unlock(&run->lock);
    run->stepped = true;
}

/* ── lifetime ────────────────────────────────────────────────────────── */

static void nodes_destroy(FwmServer *server) {
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->liquid_node) wlr_scene_node_destroy(&out->liquid_node->node);
        out->liquid_node = NULL;
    }
}

static struct FwmLiquidRun *run_create(FwmServer *server, int desktop) {
    const LiquidsConfig *cfg = &server->config.liquids;
    struct FwmLiquidRun *run = calloc(1, sizeof(*run));
    if (!run) return NULL;
    run->sim = liquid_create(cfg->spacing, cfg->particles);
    if (!run->sim) { free(run); return NULL; }
    run->desktop = desktop;
    atomic_init(&run->busy, false);
    pthread_mutex_init(&run->lock, NULL);
    pthread_cond_init(&run->cond, NULL);
    if (pthread_create(&run->thread, NULL, liquid_worker, run) != 0) {
        pthread_mutex_destroy(&run->lock);
        pthread_cond_destroy(&run->cond);
        liquid_destroy(run->sim);
        free(run);
        return NULL;
    }
    const char *dbg = getenv("FWM_DEBUG_LIQUID");
    run->dbg = dbg && *dbg && *dbg != '0';
    run->asleep = true;
    return run;
}

/* Joined, unlike the audio threads: a step is a bounded piece of arithmetic
 * that never waits on anything outside itself, so the longest this can stand
 * still is one step. */
static void run_destroy(FwmServer *server) {
    struct FwmLiquidRun *run = server->liquid;
    if (!run) return;
    pthread_mutex_lock(&run->lock);
    run->quit = true;
    pthread_cond_signal(&run->cond);
    pthread_mutex_unlock(&run->lock);
    pthread_join(run->thread, NULL);
    pthread_mutex_destroy(&run->lock);
    pthread_cond_destroy(&run->cond);

    nodes_destroy(server);
    liquid_paint_destroy(run->paint);
    liquid_destroy(run->sim);
    free(run->snap);
    free(run);
    server->liquid = NULL;
}

void server_liquid_finish(FwmServer *server) {
    hand_drop(server, true);
    server_kill_liquids_menu(server);
    run_destroy(server);
    liquid_paint_finish();
}

/* The renderer is about to be replaced. The picture and its buffers belong to
 * it; the liquid does not, and goes on exactly where it was — the next tick
 * paints it again from scratch on the new one. */
void server_liquid_gpu_release(FwmServer *server) {
    struct FwmLiquidRun *run = server->liquid;
    liquid_paint_finish();
    if (!run) return;
    nodes_destroy(server);
    liquid_paint_destroy(run->paint);
    run->paint = NULL;
    run->dirty = true;
}

void server_liquid_output_gone(FwmServer *server, FwmOutput *out) {
    (void)server;
    if (out->liquid_node) wlr_scene_node_destroy(&out->liquid_node->node);
    out->liquid_node = NULL;
}

void server_liquid_raise(FwmOutput *out) {
    if (out && out->liquid_node) wlr_scene_node_raise_to_top(&out->liquid_node->node);
}

/* ── what the socket asks for ────────────────────────────────────────── */

/* Where the pointer is pouring: the desktop under it, and the point on that
 * desktop, in the liquid's own box. False when the pointer is on no screen. */
static bool pointer_target(FwmServer *server, int *d, double *x, double *y) {
    FwmOutput *out = server_output_at(server, server->cursor->x, server->cursor->y);
    if (!out || !out->enabled || out->desktop < 0 || out->desktop >= FWM_DESKTOPS)
        return false;
    *d = out->desktop;
    *x = server->cursor->x - (out->box.x + (double)*d * server->screen_width - out->camera_x);
    *y = server->cursor->y - out->box.y;
    return true;
}

/* The liquid, standing on desktop `d` and being liquid `kind` — started there
 * if there is none yet, moved and changed if what there is holds nothing.
 * NULL when it holds something else or somewhere else: that desktop goes in
 * `*elsewhere` and that liquid in `*other` (-1 both when it could not be
 * started at all), so each caller can say so in its own words.
 *
 * Liquid that cannot be seen is not stepped, and a desktop's worth of it
 * waiting unseen elsewhere is one more desktop the CPU is spent on the moment
 * it comes back; until there are liquids on more than one desktop at once,
 * the one there is says where. And one liquid at a time: two in one pool is
 * a pressure solve with two densities in it, which is the layers this is not
 * yet. A clear still waiting for its tick counts as empty, since the clear
 * goes in before anything poured after it. */
static struct FwmLiquidRun *claim_desktop(FwmServer *server, int d, int kind,
                                          int *elsewhere, int *other) {
    struct FwmLiquidRun *run = server->liquid;
    *elsewhere = -1;
    *other = -1;
    if (run && (run->desktop != d || run->kind != kind)) {
        bool empty = run->clear
                  || (run->pour == 0 && run->stream == 0 && run->snap_n == 0
                      && run_idle(run) && liquid_count(run->sim) == 0);
        if (!empty) {
            *elsewhere = run->desktop;
            *other = run->kind;
            return NULL;
        }
        run->desktop = d;
        run->kind = kind;
    }
    if (!run) {
        run = server->liquid = run_create(server, d);
        if (!run) return NULL;
        run->kind = kind;
        wlr_log(WLR_INFO, "liquid: on desktop %d", d);
    }
    return run;
}

/* The liquid by id, water standing in for one that has gone. */
static const LiquidKind *kind_of(FwmServer *server, int id) {
    const LiquidKind *k = liquid_kinds_find(&server->liquid_kinds, id);
    return k ? k : &server->liquid_kinds.kind[0];
}

/* Water's colour and viscosity are [liquids] keys, which `fwmctl set` changes
 * under the registry's feet; this brings the registry's copy up to them. */
static void kinds_sync_water(FwmServer *server) {
    LiquidKind *w = &server->liquid_kinds.kind[0];
    const float *c = server->config.liquids.color;
    float k = c[3] > 0.0f ? 1.0f / c[3] : 0.0f;
    for (int i = 0; i < 3; i++) w->color[i] = c[i] * k;
    w->color[3] = c[3];
    w->viscosity = server->config.liquids.viscosity;
}

bool server_liquid_pour(FwmServer *server, int count, int *desktop_out,
                        char *err, size_t errcap) {
    const LiquidsConfig *cfg = &server->config.liquids;
    if (!cfg->enabled) {
        snprintf(err, errcap, "liquids are off — [liquids] enabled = true, "
                              "or fwmctl set liquids.enabled 1");
        return false;
    }
    if (cfg->particles <= 0) {
        snprintf(err, errcap, "[liquids] particles is 0 — there is no liquid to pour");
        return false;
    }

    /* Where: under the pointer, which is also where a bind that pours would
     * mean. A pointer on no screen pours from the top of the desktop the
     * user is working on. */
    int d;
    double x, y;
    if (!pointer_target(server, &d, &x, &y)) {
        FwmOutput *out = server_active_output(server);
        if (!out || out->desktop < 0 || out->desktop >= FWM_DESKTOPS) {
            snprintf(err, errcap, "no screen to pour onto");
            return false;
        }
        d = out->desktop;
        x = out->box.width / 2.0;
        y = out->box.height / 4.0;
    }
    int elsewhere, other;
    int kind = server->liquid ? server->liquid->kind : 0;
    struct FwmLiquidRun *run = claim_desktop(server, d, kind, &elsewhere, &other);
    if (!run) {
        if (elsewhere >= 0)
            snprintf(err, errcap, "the liquid is on desktop %d — fwmctl liquid clear "
                                  "first (one desktop at a time, for now)", elsewhere);
        else
            snprintf(err, errcap, "the liquid could not be started");
        return false;
    }

    if (count <= 0) count = cfg->particles / 4;
    if (count < 1) count = 1;
    run->pour += count;
    if (run->pour > cfg->particles) run->pour = cfg->particles;
    run->pour_x = x;
    run->pour_y = y;
    if (desktop_out) *desktop_out = d;
    return true;
}

void server_liquid_clear(FwmServer *server) {
    struct FwmLiquidRun *run = server->liquid;
    if (!run) return;
    run->clear = true;
    run->pour = run->stream = run->drain = 0;
}

void server_liquid_state(FwmServer *server, FwmLiquidState *st) {
    memset(st, 0, sizeof(*st));
    st->enabled = server->config.liquids.enabled;
    st->budget = server->config.liquids.particles;
    st->desktop = -1;
    struct FwmLiquidRun *run = server->liquid;
    if (!run) { st->asleep = true; return; }
    st->desktop = run->desktop;
    st->particles = run->snap_n;
    st->pending = run->pour;
    st->asleep = run->asleep && run->pour == 0 && !run->clear;
    st->substeps = run->substeps;
    st->step_ms = run->step_ms;
    st->drawn = run->paint && liquid_paint_current(run->paint);
    snprintf(st->kind, sizeof(st->kind), "%s", kind_of(server, run->kind)->name);
}

/* ── the bucket ──────────────────────────────────────────────────────────
 *
 * Picked up from the liquids menu: a bucket of one liquid in the hand, the
 * pointer turned into it, the left button pouring a stream from its lip and
 * the right one draining what is under it, slowly. Escape puts it down.
 *
 * The bucket takes the pointer — clicks pour instead of reaching windows, and
 * no window hears the pointer move — and leaves the keyboard alone, because
 * the gravity switch is a key, and turning gravity off under a stream in mid
 * pour is the whole point of having both. */

/* How fast a bucket pours, as a share of the desktop's budget a second: a
 * bucket emptied in five seconds of holding the button, whatever the budget
 * is. And how fast the right button drains, which is slower on purpose —
 * it is the one that cannot be taken back. */
#define BUCKET_POUR_SHARE 0.2
#define BUCKET_DRAIN_RATE 600.0   /* particles a second */
/* The hand's speed is smoothed over about this long before the stream takes
 * it, or a stream poured from a still hand flickers with every jitter of the
 * mouse. And capped, because a flick across the screen is thousands of px/s
 * for one frame, and water thrown that hard leaves through the far wall's
 * clamp rather than splashing against it. */
#define BUCKET_HAND_SMOOTH 0.06
#define BUCKET_THROW_MAX 2500.0

#define BUCKET_PX 44

struct FwmLiquidHand {
    int kind;                 /* the liquid in it: an id in server->liquid_kinds */
    bool pouring, draining;
    double pour_owed, drain_owed;
    double px, py, vx, vy;    /* the hand, layout px and px/s */
    bool have_pos;
    bool told;                /* said why this press is not pouring */
    struct wlr_buffer *picture;
    int hot_x, hot_y;         /* the lip, in the picture */
};

/* The bucket: a pail tipped to pour, its mouth up and to the left, with the
 * liquid showing in the mouth and a stream running off the low end of the
 * lip. The point of the cursor is where the stream lands, because that is
 * where the liquid appears. Outlined twice — dark, then light — so it stands
 * out on any wallpaper, the way a cursor has to. */
struct bucket_paint { float color[3]; int hot_x, hot_y; };

static void bucket_outline(cairo_t *cr, cairo_path_t *const *paths, int n) {
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) cairo_set_source_rgba(cr, 0.06, 0.07, 0.09, 0.85);
        else           cairo_set_source_rgb(cr, 0.93, 0.94, 0.96);
        cairo_set_line_width(cr, pass == 0 ? 3.6 : 1.5);
        for (int i = 0; i < n; i++) {
            cairo_new_path(cr);
            cairo_append_path(cr, paths[i]);
            cairo_stroke(cr);
        }
    }
}

static void draw_bucket(cairo_t *cr, int w, int h, void *data) {
    struct bucket_paint *bp = data;
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, w * 0.63, h * 0.43);
    cairo_matrix_rotate(&m, -48.0 * M_PI / 180.0);
    cairo_set_matrix(cr, &m);

    /* Upright, the mouth is an ellipse at y = -8 and the base is at y = 9. */
    cairo_new_path(cr);
    cairo_move_to(cr, -9.5, -8.0);
    cairo_line_to(cr, -7.0, 9.0);
    cairo_curve_to(cr, -4.0, 11.0, 4.0, 11.0, 7.0, 9.0);
    cairo_line_to(cr, 9.5, -8.0);
    cairo_path_t *body = cairo_copy_path(cr);

    cairo_set_source_rgb(cr, 0.55, 0.58, 0.63);
    cairo_append_path(cr, body);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_save(cr);
    cairo_translate(cr, 0.0, -8.0);
    cairo_scale(cr, 9.5, 3.2);
    cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * M_PI);
    cairo_restore(cr);
    cairo_path_t *mouth = cairo_copy_path(cr);
    /* The liquid in it. */
    cairo_set_source_rgb(cr, bp->color[0], bp->color[1], bp->color[2]);
    cairo_fill(cr);

    /* The handle, a bail swung up over the mouth. */
    cairo_new_path(cr);
    cairo_arc(cr, 0.0, -6.0, 10.5, M_PI * 1.08, M_PI * 1.92);
    cairo_path_t *handle = cairo_copy_path(cr);

    cairo_path_t *const parts[] = { body, mouth, handle };
    bucket_outline(cr, parts, 3);
    cairo_path_destroy(body);
    cairo_path_destroy(mouth);
    cairo_path_destroy(handle);

    /* The lip: the end of the mouth that hangs lowest once tipped. */
    double ax = -9.5, ay = -8.0, bx = 9.5, by = -8.0;
    cairo_matrix_transform_point(&m, &ax, &ay);
    cairo_matrix_transform_point(&m, &bx, &by);
    double lx = ay > by ? ax : bx, ly = ay > by ? ay : by;

    /* The stream, from the lip down to the point of the cursor. */
    cairo_identity_matrix(cr);
    double hx = lx - 3.0, hy = h - 4.0;
    cairo_new_path(cr);
    cairo_move_to(cr, lx, ly);
    cairo_curve_to(cr, lx - 3.0, ly + 4.0, hx, hy - 8.0, hx, hy - 2.0);
    cairo_path_t *stream = cairo_copy_path(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 0.06, 0.07, 0.09, 0.85);
    cairo_set_line_width(cr, 5.0);
    cairo_stroke(cr);
    cairo_append_path(cr, stream);
    cairo_set_source_rgb(cr, bp->color[0], bp->color[1], bp->color[2]);
    cairo_set_line_width(cr, 2.6);
    cairo_stroke(cr);
    cairo_path_destroy(stream);
    cairo_arc(cr, hx, hy, 2.4, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    bp->hot_x = (int)lround(hx);
    bp->hot_y = (int)lround(hy);
}

bool server_liquid_hand_held(FwmServer *server) {
    return server->liquid_hand != NULL;
}

/* `quiet` at teardown, where there is nobody left to give the pointer back to
 * and no tray to redraw. */
static void hand_drop(FwmServer *server, bool quiet) {
    struct FwmLiquidHand *hand = server->liquid_hand;
    if (!hand) return;
    server->liquid_hand = NULL;
    /* The cursor lets go of the picture before we do. */
    if (server->cursor && server->cursor_mgr)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    if (hand->picture) wlr_buffer_unlock(hand->picture);
    free(hand);
    if (quiet) return;
    /* The window under the pointer is told it has it again. */
    server_pointer_resync(server);
    server_request_tray_redraw(server);
}

void server_liquid_put_down(FwmServer *server) {
    hand_drop(server, false);
}

static void liquids_menu_refresh(FwmServer *server);

/* The bucket's picture, in the colour of what is in it. */
static void hand_paint(FwmServer *server, struct FwmLiquidHand *hand) {
    struct bucket_paint bp;
    const LiquidKind *k = kind_of(server, hand->kind);
    memcpy(bp.color, k->color, sizeof(bp.color));
    struct wlr_buffer *pic = cairo_picture_create(BUCKET_PX, BUCKET_PX, draw_bucket, &bp);
    if (pic)
        wlr_cursor_set_buffer(server->cursor, pic, bp.hot_x, bp.hot_y, 1.0f);
    else
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "crosshair");
    /* The cursor holds its own lock on whatever it was given; ours is let go
     * of only after it has a new picture to hold instead. */
    if (hand->picture) wlr_buffer_unlock(hand->picture);
    hand->picture = pic;
    hand->hot_x = bp.hot_x;
    hand->hot_y = bp.hot_y;
}

void server_liquid_take(FwmServer *server, int kind) {
    kinds_sync_water(server);
    const LiquidKind *k = liquid_kinds_find(&server->liquid_kinds, kind);
    if (!k) return;
    server_liquid_put_down(server);

    /* Picking up a bucket is how liquids are switched on from the screen; the
     * config's switch is for whether there is any at startup. */
    if (!server->config.liquids.enabled) {
        server->config.liquids.enabled = 1;
        server_settings_notify(server);
    }

    struct FwmLiquidHand *hand = calloc(1, sizeof(*hand));
    if (!hand) return;
    hand->kind = kind;
    server->liquid_hand = hand;

    server_close_modes_menu(server);
    server_close_liquids_menu(server);
    server_kill_liquid_form(server);
    /* Whatever had the pointer lets go of it, and a game that had it locked
     * for mouse-look with it — the same as the screenshot selector does. */
    wlr_seat_pointer_clear_focus(server->seat);
    server_pointer_release_constraint(server);
    hand_paint(server, hand);
    toast_show(server, false, "%s in hand — left pours, right drains, Esc puts it down",
               k->name);
    server_request_tray_redraw(server);
}

bool server_liquid_hand_button(FwmServer *server, uint32_t button, bool pressed) {
    struct FwmLiquidHand *hand = server->liquid_hand;
    if (!hand) return false;
    if (button == BTN_LEFT) hand->pouring = pressed;
    else if (button == BTN_RIGHT) hand->draining = pressed;
    if (pressed) hand->told = false;
    /* Every button: with a bucket in the hand nothing under it is clicked. */
    return true;
}

bool server_liquid_escape(FwmServer *server) {
    if (server->liquid_hand) {
        server_liquid_put_down(server);
        return true;
    }
    if (server->liquids_buffer) {
        server_close_liquids_menu(server);
        return true;
    }
    return false;
}

/* Once a tick: follow the hand, and hand over what it poured and drained. */
static void hand_tick(FwmServer *server, double elapsed) {
    struct FwmLiquidHand *hand = server->liquid_hand;
    if (!hand) return;
    const LiquidsConfig *cfg = &server->config.liquids;
    /* Put down by anything that takes the screen away from it. */
    if (!cfg->enabled || expo_active(server) || server->locked) {
        server_liquid_put_down(server);
        return;
    }

    double x = server->cursor->x, y = server->cursor->y;
    if (hand->have_pos && elapsed > 0.0) {
        double k = 1.0 - exp(-elapsed / BUCKET_HAND_SMOOTH);
        hand->vx += ((x - hand->px) / elapsed - hand->vx) * k;
        hand->vy += ((y - hand->py) / elapsed - hand->vy) * k;
    }
    hand->px = x;
    hand->py = y;
    hand->have_pos = true;
    if (!hand->pouring && !hand->draining) {
        hand->pour_owed = hand->drain_owed = 0.0;
        return;
    }

    int d;
    double lx, ly;
    if (!pointer_target(server, &d, &lx, &ly)) return;

    /* Draining takes whatever is there, whichever bucket is in the hand. */
    struct FwmLiquidRun *run = server->liquid;
    if (hand->draining && run && run->desktop == d) {
        hand->drain_owed += BUCKET_DRAIN_RATE * elapsed;
        int n = (int)hand->drain_owed;
        if (n > 0) {
            hand->drain_owed -= n;
            run->drain += n;
            run->drain_x = lx;
            run->drain_y = ly;
        }
    }
    if (!hand->pouring) return;

    int elsewhere, other;
    run = claim_desktop(server, d, hand->kind, &elsewhere, &other);
    if (!run) {
        if (!hand->told && other >= 0) {
            const char *there = kind_of(server, other)->name;
            if (elsewhere != d)
                toast_show(server, true, "%s is on desktop %d — drain it there first",
                           there, elsewhere + 1);
            else
                toast_show(server, true, "There is %s here — drain it first; "
                           "liquids do not mix yet", there);
        }
        hand->told = true;
        return;
    }

    double rate = cfg->particles * BUCKET_POUR_SHARE;
    if (rate < 200.0) rate = 200.0;
    hand->pour_owed += rate * elapsed;
    int n = (int)hand->pour_owed;
    if (n > 0) {
        hand->pour_owed -= n;
        run->stream += n;
        run->stream_x = lx;
        run->stream_y = ly;
        double v = sqrt(hand->vx * hand->vx + hand->vy * hand->vy);
        double s = v > BUCKET_THROW_MAX ? BUCKET_THROW_MAX / v : 1.0;
        run->stream_vx = hand->vx * s;
        run->stream_vy = hand->vy * s;
    }
    if (run->full && !hand->told) {
        toast_show(server, false, run->full == FULL_BRIM
                   ? "The desktop is full to the top"
                   : "That is all this desktop holds — [liquids] particles");
        hand->told = true;
    }
}

/* ── the liquids menu ──────────────────────────────────────────────────── */

static void liquids_menu_fill(FwmServer *server, LiquidsMenuState *st) {
    memset(st, 0, sizeof(*st));
    kinds_sync_water(server);
    const LiquidKinds *ks = &server->liquid_kinds;
    struct FwmLiquidRun *run = server->liquid;
    bool there = run && (run->snap_n > 0 || run->pour > 0 || run->stream > 0) && !run->clear;
    st->count = ks->count < LIQUIDS_MENU_MAX ? ks->count : LIQUIDS_MENU_MAX;
    st->in_hand = -1;
    for (int i = 0; i < st->count; i++) {
        const LiquidKind *k = &ks->kind[i];
        LiquidsMenuItem *it = &st->item[i];
        snprintf(it->name, sizeof(it->name), "%s", k->name);
        memcpy(it->color, k->color, sizeof(it->color));
        liquid_kind_describe(k, it->desc, sizeof(it->desc));
        it->desktop = there && run->kind == k->id ? run->desktop : -1;
        it->removable = k->id != 0;
        if (server->liquid_hand && server->liquid_hand->kind == k->id) st->in_hand = i;
    }
    st->hover = server->liquids_buffer ? server->liquids_hover : -1;
    st->hover_part = server->liquids_buffer ? server->liquids_hover_part : LIQUIDS_PART_NONE;
    st->full = ks->count >= LIQUID_KINDS_MAX;
    st->opacity = glass_fill(&server->config, server->config.decor.tray_opacity);
}

bool server_liquid_lit(FwmServer *server) {
    struct FwmLiquidRun *run = server->liquid;
    return server->liquid_hand
        || (run && (run->snap_n > 0 || run->pour > 0) && !run->clear);
}

void server_open_liquids_menu(FwmServer *server) {
    if (server->liquids_buffer) return;
    FwmOutput *out = server_active_output(server);
    if (!out || !out->tray_buffer || !out->tray_buffer->node.enabled) return;
    /* Hung where the modes menu hangs — right-aligned with the modes pill —
     * since it is that menu's next page. A screen too narrow for the pill
     * hangs it at the right edge instead. */
    double right = out->box.x + out->box.width - 8;
    if (tray_modes_pill_hit(&out->tray_strip, tray_modes_pill_x(&out->tray_strip), 1.0))
        right = out->tray_buffer->node.x + tray_modes_pill_x(&out->tray_strip) + MODES_PILL_W;
    server->liquids_hover = -1;
    server->liquids_hover_part = LIQUIDS_PART_NONE;
    LiquidsMenuState st;
    liquids_menu_fill(server, &st);
    server->liquids_buffer = liquids_menu_show(server->layer_overlay, &out->box, right, &st);
}

void server_close_liquids_menu(FwmServer *server) {
    server_kill_liquid_form(server);
    if (!server->liquids_buffer) return;
    cairo_overlay_animate_out(server->liquids_buffer, LIQUIDS_MENU_ANIM_MS,
                              -LIQUIDS_MENU_RISE_PX, NULL, NULL);
    server->liquids_buffer = NULL;
}

void server_toggle_liquids_menu(FwmServer *server) {
    if (server->liquids_buffer) server_close_liquids_menu(server);
    else server_open_liquids_menu(server);
}

static void liquids_menu_refresh(FwmServer *server) {
    if (!server->liquids_buffer) return;
    LiquidsMenuState st;
    liquids_menu_fill(server, &st);
    server->liquids_buffer = liquids_menu_refit(server->liquids_buffer, &st);
}

static void liquid_form_open(FwmServer *server, int edit_id);

/* A liquid removed from the list goes from the world with it: off the
 * desktop, and out of the hand. */
static void remove_kind(FwmServer *server, int id) {
    if (!liquid_kinds_remove(&server->liquid_kinds, id)) return;
    struct FwmLiquidRun *run = server->liquid;
    if (run && run->kind == id) {
        server_liquid_clear(server);
        run->kind = 0;
    }
    if (server->liquid_hand && server->liquid_hand->kind == id) server_liquid_put_down(server);
    server_state_save_liquids(server);
    server->liquids_hover = -1;
    server->liquids_hover_part = LIQUIDS_PART_NONE;
    liquids_menu_refresh(server);
    server_request_tray_redraw(server);
}

bool server_liquid_menu_click(FwmServer *server) {
    if (!server->liquids_buffer) return false;
    double mx = server->cursor->x - server->liquids_buffer->node.x;
    double my = server->cursor->y - server->liquids_buffer->node.y;
    LiquidsMenuState st;
    liquids_menu_fill(server, &st);
    int mw, mh;
    liquids_menu_size(st.count, &mw, &mh);
    if (mx < 0 || mx >= mw || my < 0 || my >= mh) {
        /* Outside: dismiss, and let the click carry on to whatever it was
         * aimed at, the way the modes menu does. */
        server_close_liquids_menu(server);
        return false;
    }
    int part = LIQUIDS_PART_NONE;
    int row = liquids_menu_hit(st.count, mx, my, &part);
    if (part == LIQUIDS_PART_PLUS) {
        if (!st.full) liquid_form_open(server, -1);
        return true;
    }
    if (row < 0) return true;
    int id = server->liquid_kinds.kind[row].id;
    switch (part) {
    case LIQUIDS_PART_GEAR:
        liquid_form_open(server, id);
        break;
    case LIQUIDS_PART_BIN:
        if (st.item[row].removable) remove_kind(server, id);
        break;
    default:
        server_liquid_take(server, id);
        break;
    }
    return true;
}

void server_liquid_menu_motion(FwmServer *server) {
    if (!server->liquids_buffer) return;
    double mx = server->cursor->x - server->liquids_buffer->node.x;
    double my = server->cursor->y - server->liquids_buffer->node.y;
    LiquidsMenuState st;
    liquids_menu_fill(server, &st);
    int part = LIQUIDS_PART_NONE;
    int row = liquids_menu_hit(st.count, mx, my, &part);
    if (row == server->liquids_hover && part == server->liquids_hover_part) return;
    server->liquids_hover = row;
    server->liquids_hover_part = part;
    liquids_menu_refresh(server);
}

void server_kill_liquids_menu(FwmServer *server) {
    server_kill_liquid_form(server);
    if (server->liquids_buffer) {
        cairo_overlay_destroy(server->liquids_buffer);
        server->liquids_buffer = NULL;
    }
}

/* ── the liquid form ───────────────────────────────────────────────────── */

struct FwmLiquidFormRun {
    LiquidForm form;
    struct wlr_scene_buffer *buf;
    int edit_id;              /* the liquid being changed, -1 for a new one */
};

bool server_liquid_form_open(FwmServer *server) {
    return server->liquid_form != NULL;
}

void server_kill_liquid_form(FwmServer *server) {
    struct FwmLiquidFormRun *fr = server->liquid_form;
    if (!fr) return;
    server->liquid_form = NULL;
    if (fr->buf) cairo_overlay_animate_out(fr->buf, LIQUIDS_MENU_ANIM_MS,
                                           -LIQUIDS_MENU_RISE_PX, NULL, NULL);
    free(fr);
}

/* Opened beside the liquids menu, to its left, as the next step out from it. */
static void liquid_form_open(FwmServer *server, int edit_id) {
    server_kill_liquid_form(server);
    kinds_sync_water(server);
    struct FwmLiquidFormRun *fr = calloc(1, sizeof(*fr));
    if (!fr) return;
    fr->edit_id = edit_id;
    const LiquidKind *from = edit_id >= 0 ? liquid_kinds_find(&server->liquid_kinds, edit_id) : NULL;
    if (from) {
        liquid_form_init(&fr->form, from, true);
    } else {
        /* A new liquid starts as water does, in amber, so it is already
         * something to pour and visibly not water. */
        LiquidKind fresh = server->liquid_kinds.kind[0];
        fresh.color[0] = 0.88f; fresh.color[1] = 0.62f; fresh.color[2] = 0.18f;
        fresh.density = 1.0;
        liquid_form_init(&fr->form, &fresh, false);
        fr->edit_id = -1;
    }
    fr->form.opacity = glass_fill(&server->config, server->config.decor.tray_opacity);

    FwmOutput *out = server_active_output(server);
    int fw, fh;
    liquid_form_size(&fw, &fh);
    int x, y;
    if (server->liquids_buffer) {
        x = server->liquids_buffer->node.x - fw - 8;
        y = server->liquids_buffer->node.y;
    } else {
        x = out ? out->box.x + out->box.width - fw - 8 : 8;
        y = out ? out->box.y + TRAY_BOTTOM + 6 : 8;
    }
    if (out && x < out->box.x + 8) x = out->box.x + 8;
    fr->buf = liquid_form_show(server->layer_overlay, x, y, &fr->form);
    if (!fr->buf) { free(fr); return; }
    server->liquid_form = fr;
}

static void form_submit(FwmServer *server) {
    struct FwmLiquidFormRun *fr = server->liquid_form;
    LiquidKind k;
    liquid_form_result(&fr->form, &k);
    int id = fr->edit_id;
    if (id == 0) {
        /* Water's colour and viscosity live in [liquids]; its name and density
         * live in the registry. */
        LiquidsConfig *cfg = &server->config.liquids;
        for (int i = 0; i < 3; i++) cfg->color[i] = k.color[i] * k.color[3];
        cfg->color[3] = k.color[3];
        cfg->viscosity = k.viscosity;
        liquid_kinds_update(&server->liquid_kinds, 0, &k);
        server_settings_notify(server);
    } else if (id > 0) {
        liquid_kinds_update(&server->liquid_kinds, id, &k);
    } else {
        id = liquid_kinds_add(&server->liquid_kinds, &k);
        if (id < 0) {
            toast_show(server, true, "There is no room for another liquid");
            return;
        }
    }
    server_state_save_liquids(server);
    /* Changing the liquid in the hand repaints the bucket; changing the
     * liquid on the desktop reaches it on the next step and the next
     * picture, since both are read from the registry. */
    if (server->liquid_hand && server->liquid_hand->kind == id)
        hand_paint(server, server->liquid_hand);
    if (server->liquid && server->liquid->kind == id) server->liquid->dirty = true;
    server_kill_liquid_form(server);
    liquids_menu_refresh(server);
}

static void form_result(FwmServer *server, LiquidFormResult r) {
    struct FwmLiquidFormRun *fr = server->liquid_form;
    if (!fr) return;
    switch (r) {
    case LIQUID_FORM_CHANGED: liquid_form_redraw(fr->buf, &fr->form); break;
    case LIQUID_FORM_SUBMIT:  form_submit(server); break;
    case LIQUID_FORM_CANCEL:  server_kill_liquid_form(server); break;
    default: break;
    }
}

bool server_liquid_form_button(FwmServer *server, uint32_t button, bool pressed) {
    struct FwmLiquidFormRun *fr = server->liquid_form;
    if (!fr) return false;
    double mx = server->cursor->x - fr->buf->node.x;
    double my = server->cursor->y - fr->buf->node.y;
    int fw, fh;
    liquid_form_size(&fw, &fh);
    bool inside = mx >= 0 && my >= 0 && mx < fw && my < fh;
    if (!pressed) {
        /* A drag ends wherever the button comes up — over a window, too, and
         * that window never saw the press, so the release is the form's. */
        if (fr->form.drag) {
            form_result(server, liquid_form_release(&fr->form));
            server->click_consumed = 0;
            return true;
        }
        return false;
    }
    if (!inside) {
        server_kill_liquid_form(server);
        return false;
    }
    /* Taken, and so is the release that goes with it — whether or not the
     * form is still there to hear it, which after Create it is not. */
    server->click_consumed = 1;
    if (button == BTN_LEFT) form_result(server, liquid_form_press(&fr->form, mx, my));
    return true;
}

void server_liquid_form_motion(FwmServer *server) {
    struct FwmLiquidFormRun *fr = server->liquid_form;
    if (!fr) return;
    form_result(server, liquid_form_motion(&fr->form, server->cursor->x - fr->buf->node.x,
                                           server->cursor->y - fr->buf->node.y));
}

void server_liquid_form_key(FwmServer *server, uint32_t sym, const char *utf8) {
    struct FwmLiquidFormRun *fr = server->liquid_form;
    if (!fr) return;
    form_result(server, liquid_form_key(&fr->form, sym, utf8));
}

/* ── the tick ────────────────────────────────────────────────────────── */

/* Something covers the whole desktop: a fullscreen window on it, real or
 * fake — the same test that pauses the wallpaper behind one. */
static bool desktop_covered(FwmServer *server, int d) {
    FwmView *v;
    wl_list_for_each(v, &server->views, link) {
        PhysicsBody *b = physics_find_body(&server->physics, v->id);
        if (b && b->fullscreen && b->desktop_id == d) return true;
    }
    return false;
}

static void snapshot_take(struct FwmLiquidRun *run) {
    int n = liquid_count(run->sim), stride;
    const float *p = liquid_positions(run->sim, &stride);
    if (n > run->snap_cap) {
        float *s = realloc(run->snap, sizeof(float) * 2 * (size_t)n);
        if (!s) n = run->snap_cap;
        else { run->snap = s; run->snap_cap = n; }
    }
    for (int i = 0; i < n; i++) {
        run->snap[2 * i]     = p[i * stride];
        run->snap[2 * i + 1] = p[i * stride + 1];
    }
    run->snap_n = n;
    run->asleep = liquid_asleep(run->sim);
    run->substeps = liquid_last_substeps(run->sim);
    run->iters = liquid_last_iterations(run->sim);
    run->vmax = liquid_max_speed(run->sim);
    run->step_ms = run->job_ms;
    run->dirty = true;
}

static bool liquid_declines_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    (void)buffer; (void)sx; (void)sy;
    return false;
}

/* Put the picture on every monitor that can see some of its desktop, cut to
 * that monitor: a desktop sliding off one screen during a switch must not
 * reappear on the screen beside it. */
static void place_nodes(FwmServer *server, struct FwmLiquidRun *run, bool show,
                        struct wlr_buffer *fresh) {
    struct wlr_buffer *buf = run->paint ? liquid_paint_current(run->paint) : NULL;
    int w = run->paint ? liquid_paint_width(run->paint) : 0;
    int h = run->paint ? liquid_paint_height(run->paint) : 0;
    FwmOutput *out;
    wl_list_for_each(out, &server->outputs, link) {
        bool on = show && buf && out->enabled && run->snap_n > 0;
        int x0 = 0, x1 = 0, nx = 0;
        if (on) {
            nx = out->box.x + run->desktop * server->screen_width - out->camera_x;
            x0 = nx > out->box.x ? nx : out->box.x;
            x1 = nx + w < out->box.x + out->box.width ? nx + w : out->box.x + out->box.width;
            on = x1 > x0;
        }
        if (!on) {
            if (out->liquid_node) wlr_scene_node_set_enabled(&out->liquid_node->node, false);
            continue;
        }
        if (!out->liquid_node) {
            out->liquid_node = wlr_scene_buffer_create(server->layer_background, buf);
            if (!out->liquid_node) continue;
            out->liquid_node->point_accepts_input = liquid_declines_input;
        } else if (fresh) {
            pixman_region32_t dmg;
            pixman_region32_init_rect(&dmg, run->damage.x, run->damage.y,
                                      (unsigned)run->damage.width, (unsigned)run->damage.height);
            wlr_scene_buffer_set_buffer_with_damage(out->liquid_node, fresh, &dmg);
            pixman_region32_fini(&dmg);
        }
        wlr_scene_buffer_set_source_box(out->liquid_node, &(struct wlr_fbox){
            .x = x0 - nx, .y = 0, .width = x1 - x0, .height = h });
        wlr_scene_buffer_set_dest_size(out->liquid_node, x1 - x0, h);
        wlr_scene_node_set_position(&out->liquid_node->node, x0, out->box.y);
        wlr_scene_node_set_enabled(&out->liquid_node->node, true);
    }
}

void server_liquid_sync(FwmServer *server, double elapsed) {
    const LiquidsConfig *cfg = &server->config.liquids;

    hand_tick(server, elapsed);
    if (server->liquids_buffer) {
        LiquidsMenuState st;
        liquids_menu_fill(server, &st);
        liquids_menu_tick(server->liquids_buffer, &st, elapsed);
    }

    if (!cfg->enabled) {
        if (server->liquid) {
            run_destroy(server);
            wlr_log(WLR_INFO, "liquid: switched off, emptied");
        }
        return;
    }
    struct FwmLiquidRun *run = server->liquid;
    if (!run) return;

    int d = run->desktop;
    FwmOutput *mon = server_output_showing(server, d);
    bool show = mon && !expo_active(server) && !desktop_covered(server, d);

    if (run_idle(run)) {
        if (run->dbg && run->stepped) {
            run->dbg_ms += run->job_ms;
            if (run->job_ms > run->dbg_worst) run->dbg_worst = run->job_ms;
            run->dbg_steps++;
        }
        bool changed = run->stepped;
        run->stepped = false;

        /* A new spacing is a new liquid: particles of one size do not become
         * particles of another. */
        if (cfg->spacing != liquid_spacing(run->sim)) {
            FwmLiquid *fresh = liquid_create(cfg->spacing, cfg->particles);
            if (fresh) {
                liquid_destroy(run->sim);
                run->sim = fresh;
                run->box_w = run->box_h = 0.0;
                changed = true;
                wlr_log(WLR_INFO, "liquid: spacing is now %.1f px — emptied", cfg->spacing);
            }
        }
        liquid_set_budget(run->sim, cfg->particles);
        /* The liquid's own thickness; the surface is still [liquids]' for
         * every liquid, until there is a form row for it. */
        kinds_sync_water(server);
        liquid_set_material(run->sim, &(LiquidMaterial){
            .viscosity = kind_of(server, run->kind)->viscosity, .tension = cfg->tension });

        /* The walls are the monitor showing the desktop, and the floor is the
         * one the windows stand on — above a bar that reserved the bottom of
         * the screen, when [physics] solid_bars says bars are solid. */
        if (mon) {
            double bw = mon->box.width;
            double bh = mon->box.height - server->physics.inset_bottom;
            if (bh < 1.0) bh = mon->box.height;
            if (bw != run->box_w || bh != run->box_h) {
                liquid_set_box(run->sim, bw, bh);
                run->box_w = bw;
                run->box_h = bh;
                changed = true;
            }
        }

        if (run->clear) {
            liquid_clear(run->sim);
            run->clear = false;
            changed = true;
        }
        if (run->pour > 0 && run->box_w > 0.0) {
            int got = liquid_pour(run->sim, run->pour_x, run->pour_y, run->pour, 0.0, 0.0);
            wlr_log(WLR_DEBUG, "liquid: poured %d of %d", got, run->pour);
            run->pour = 0;
            changed = true;
        }
        if (run->stream > 0 && run->box_w > 0.0) {
            int got = liquid_pour(run->sim, run->stream_x, run->stream_y, run->stream,
                                  run->stream_vx, run->stream_vy);
            run->full = got > 0 || liquid_room(run->sim) > 0 ? 0
                      : liquid_count(run->sim) >= liquid_budget(run->sim) ? FULL_BUDGET
                      : FULL_BRIM;
            run->stream = 0;
            changed = true;
        }
        if (run->drain > 0) {
            /* A drain the width of a few particles: a spout, not a sponge —
             * the liquid around it has to run into it. */
            if (liquid_drain(run->sim, run->drain_x, run->drain_y,
                             6.0 * liquid_spacing(run->sim), run->drain))
                changed = true;
            run->drain = 0;
        }
        if (changed) snapshot_take(run);

        /* Step it if anyone can see it and it is either moving or about to
         * be: a change of gravity is the one thing that wakes a settled
         * liquid without anything having been done to it, and the liquid can
         * only find out by being stepped. */
        if (show) {
            double gx = 0.0;
            double gy = server->physics.desktops[d].gravity * server->physics.gravity_scale;
            bool g_new = !run->have_g || gx != run->last_gx || gy != run->last_gy;
            run->owed += elapsed;
            if (!liquid_asleep(run->sim) || g_new) {
                double dt = run->owed < LIQUID_MAX_DT ? run->owed : LIQUID_MAX_DT;
                run->owed = 0.0;
                if (dt > 0.0) {
                    run_submit(run, dt, gx, gy);
                    run->have_g = true;
                    run->last_gx = gx;
                    run->last_gy = gy;
                }
            } else {
                run->owed = 0.0;
            }
        } else {
            run->owed = 0.0;
        }
    } else if (show) {
        /* Still stepping from last tick. The time keeps being owed, and is
         * paid — up to LIQUID_MAX_DT — when the worker is free again. */
        run->owed += elapsed;
    }

    /* The picture, from the newest particles there are. */
    struct wlr_buffer *fresh = NULL;
    if (run->dirty && show) {
        int w = mon->box.width, h = (int)run->box_h;
        if (run->paint && (liquid_paint_width(run->paint) != w
                           || liquid_paint_height(run->paint) != h)) {
            nodes_destroy(server);
            liquid_paint_destroy(run->paint);
            run->paint = NULL;
        }
        if (!run->paint && liquid_paint_supported(server->wlr_renderer) && h > 0)
            run->paint = liquid_paint_create(server, w, h);
        if (!run->paint && !run->told_no_gl) {
            wlr_log(WLR_INFO, "liquid: nothing to draw it with — it needs the GLES2 "
                              "renderer; the liquid still runs, unseen");
            run->told_no_gl = true;
        }
        if (run->paint) {
            kinds_sync_water(server);
            const LiquidKind *k = kind_of(server, run->kind);
            const float color[4] = { k->color[0] * k->color[3], k->color[1] * k->color[3],
                                     k->color[2] * k->color[3], k->color[3] };
            fresh = liquid_paint_draw(run->paint, run->snap, run->snap_n,
                                      liquid_spacing(run->sim), color, &run->damage);
        }
        run->dirty = false;
    }
    place_nodes(server, run, show, fresh);

    if (run->dbg) {
        double t = now_s();
        if (t - run->dbg_at >= 1.0) {
            wlr_log(WLR_INFO, "liquid: %d particles, %s, %d steps/s at %.2f ms avg "
                    "%.2f worst, %d substeps, %d iterations, fastest %.0f px/s",
                    run->snap_n, run->asleep ? "asleep" : "moving", run->dbg_steps,
                    run->dbg_steps ? run->dbg_ms / run->dbg_steps : 0.0, run->dbg_worst,
                    run->substeps, run->iters, run->vmax);
            run->dbg_at = t;
            run->dbg_ms = run->dbg_worst = 0.0;
            run->dbg_steps = 0;
        }
    }
}

/* The frame loop must run at the full rate while the liquid moves: it is
 * driven by the worker, not by any client, and on the 200ms heartbeat it
 * would advance in jumps of a fifth of a second. Settled, or out of sight, it
 * costs nothing. */
bool server_liquid_busy(FwmServer *server) {
    if (server->liquids_buffer && liquids_menu_animating()) return true;
    /* A bucket pouring or draining is fed at the tick, and on the heartbeat it
     * would pour in lumps five times a second. */
    if (server->liquid_hand && (server->liquid_hand->pouring || server->liquid_hand->draining))
        return true;
    struct FwmLiquidRun *run = server->liquid;
    if (!run) return false;
    if (!run_idle(run) || run->stepped || run->dirty) return true;
    if (run->pour > 0 || run->stream > 0 || run->drain > 0 || run->clear) return true;
    if (run->asleep) return false;
    FwmOutput *mon = server_output_showing(server, run->desktop);
    return mon && !expo_active(server);
}
