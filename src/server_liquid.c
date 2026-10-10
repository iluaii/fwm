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

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pixman.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "expo.h"
#include "liquid.h"
#include "liquid_draw.h"
#include "physics.h"
#include "server_internal.h"

/* The most simulated time one step may cover. A step that fell behind — the
 * worker was still busy at the last tick, or the tick itself was late — is
 * handed the time it owes up to this, and the rest is lost: the liquid runs
 * slow for a moment rather than being handed a second it would have to take
 * in sixteen pieces. Two frames, the same allowance the window physics has. */
#define LIQUID_MAX_DT (2.0 / 60.0)

struct FwmLiquidRun {
    FwmLiquid *sim;
    int desktop;

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
    FwmOutput *out = server_output_at(server, server->cursor->x, server->cursor->y);
    bool at_pointer = out && out->enabled;
    if (!at_pointer) out = server_active_output(server);
    if (!out || out->desktop < 0 || out->desktop >= FWM_DESKTOPS) {
        snprintf(err, errcap, "no screen to pour onto");
        return false;
    }
    int d = out->desktop;

    struct FwmLiquidRun *run = server->liquid;
    if (run && run->desktop != d) {
        /* Liquid that cannot be seen is not stepped, and a desktop's worth of
         * it waiting unseen elsewhere is one more desktop the CPU is spent
         * on the moment it comes back; until there are liquids on more than
         * one desktop at once, the one there is says where. An empty one
         * simply moves — and one with a clear still waiting for its tick is
         * as good as empty, since the clear goes in before the pour does. */
        bool empty = run->clear
                  || (run->pour == 0 && run->snap_n == 0 && run_idle(run)
                      && liquid_count(run->sim) == 0);
        if (!empty) {
            snprintf(err, errcap, "the liquid is on desktop %d — fwmctl liquid clear "
                                  "first (one desktop at a time, for now)", run->desktop);
            return false;
        }
        run->desktop = d;
    }
    if (!run) {
        run = server->liquid = run_create(server, d);
        if (!run) {
            snprintf(err, errcap, "the liquid could not be started");
            return false;
        }
        wlr_log(WLR_INFO, "liquid: on desktop %d", d);
    }

    if (count <= 0) count = cfg->particles / 4;
    if (count < 1) count = 1;
    run->pour += count;
    if (run->pour > cfg->particles) run->pour = cfg->particles;
    if (at_pointer) {
        run->pour_x = server->cursor->x - (out->box.x + (double)d * server->screen_width - out->camera_x);
        run->pour_y = server->cursor->y - out->box.y;
    } else {
        run->pour_x = out->box.width / 2.0;
        run->pour_y = out->box.height / 4.0;
    }
    if (desktop_out) *desktop_out = d;
    return true;
}

void server_liquid_clear(FwmServer *server) {
    struct FwmLiquidRun *run = server->liquid;
    if (!run) return;
    run->clear = true;
    run->pour = 0;
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
        liquid_set_material(run->sim, &(LiquidMaterial){
            .viscosity = cfg->viscosity, .tension = cfg->tension });

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
        if (run->paint)
            fresh = liquid_paint_draw(run->paint, run->snap, run->snap_n,
                                      liquid_spacing(run->sim), cfg->color, &run->damage);
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
    struct FwmLiquidRun *run = server->liquid;
    if (!run) return false;
    if (!run_idle(run) || run->stepped || run->dirty) return true;
    if (run->pour > 0 || run->clear) return true;
    if (run->asleep) return false;
    FwmOutput *mon = server_output_showing(server, run->desktop);
    return mon && !expo_active(server);
}
