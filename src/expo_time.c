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

#include "expo_internal.h"
#include "ui/cairo_overlay.h"
#include "ui/expo_time.h"
#include "journal.h"
#include "server_journal.h"
#include <math.h>
#include <stdlib.h>
#include <linux/input-event-codes.h>
#include <wlr/util/log.h>

static const char *const section_log_name[EXPO_TIME_SECTIONS] = {
    "log", "stats", "timelapse",
};

/* The section the view last showed, for the next time it opens: whoever went
 * to the stats once is likely to want the stats again. Kept for the session,
 * across closing the strip. */
static int g_section = EXPO_TIME_LOG;

/* Cubic ease-out, the curve the tray's menus come up on. Played on a progress
 * that runs back down when the view closes, it is the opening seen in reverse
 * — and a close caught half way simply turns round, from wherever it got to. */
static double ease(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return 1.0 - pow(1.0 - t, 3.0);
}

static double clamp01(double t) {
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

/* ── the log ──────────────────────────────────────────────────────────── */

/* Never more on screen than this, whatever the monitor. */
#define LOG_ROWS_MAX 64
static ExpoTimeRow g_rows[LOG_ROWS_MAX];

/* Every opening and every closing the history still knows a window for, to be
 * put in order: two per window at most. */
#define LOG_ENTRIES_MAX 2048
typedef struct {
    int64_t ms;
    const JournalWindow *w;
    bool closed;
} LogEntry;
static LogEntry g_entries[LOG_ENTRIES_MAX];

/* Newest first; a closing before the opening it shares a millisecond with,
 * since it cannot have come first. */
static int entry_cmp(const void *pa, const void *pb) {
    const LogEntry *a = pa, *b = pb;
    if (a->ms != b->ms) return a->ms < b->ms ? 1 : -1;
    return (int)b->closed - (int)a->closed;
}

/* The part of the log the page has room for, newest first, from however far
 * down it has been scrolled. Built from the windows rather than from the
 * events: the events are a ring that a window dragged about all afternoon can
 * fill, and the list must not lose its openings and closings to that. The
 * names point into the history itself, so the result is good until the
 * history next changes — for one draw. */
static ExpoTimeLog log_view(FwmExpo *e) {
    FwmServer *server = e->server;
    FwmJournal *j = server->journal;
    ExpoTimeLog log = {
        .rows = g_rows,
        .now_ms = server_journal_now(),
        .icon_theme = server->config.decor.icon_theme,
    };
    int fit = expo_time_log_fit(e->time_page);
    if (fit > LOG_ROWS_MAX) fit = LOG_ROWS_MAX;

    int n = 0;
    int wins = journal_window_count(j);
    for (int i = 0; i < wins && n + 2 <= LOG_ENTRIES_MAX; i++) {
        const JournalWindow *w = journal_window_at(j, i);
        g_entries[n++] = (LogEntry){ .ms = w->opened_ms, .w = w, .closed = false };
        if (w->closed_ms)
            g_entries[n++] = (LogEntry){ .ms = w->closed_ms, .w = w, .closed = true };
    }
    qsort(g_entries, (size_t)n, sizeof(g_entries[0]), entry_cmp);
    log.total = n;

    int most = n - fit;
    if (most < 0) most = 0;
    if (e->time_scroll > most) e->time_scroll = most;
    if (e->time_scroll < 0) e->time_scroll = 0;
    log.first = e->time_scroll;

    for (int i = log.first; i < n && log.n < fit; i++) {
        const LogEntry *en = &g_entries[i];
        g_rows[log.n++] = (ExpoTimeRow){
            .closed = en->closed,
            .ms = en->ms,
            .app_id = en->w->app_id,
            .title = en->w->title,
            .still_open = en->w->closed_ms == 0,
            .lived_ms = en->closed && en->ms > en->w->opened_ms
                            ? en->ms - en->w->opened_ms : 0,
        };
    }
    e->time_log_version = journal_list_version(j);
    return log;
}

static struct wlr_scene_buffer *content_show(FwmExpo *e, int section) {
    if (section != EXPO_TIME_LOG)
        return expo_time_content_show(e->server->layer_overlay, e->time_page,
                                      section, NULL);
    ExpoTimeLog log = log_view(e);
    return expo_time_content_show(e->server->layer_overlay, e->time_page,
                                  section, &log);
}

static void log_redraw(FwmExpo *e) {
    if (!e->time_content || e->time_section != EXPO_TIME_LOG) return;
    ExpoTimeLog log = log_view(e);
    expo_time_content_redraw(e->time_content, EXPO_TIME_LOG, &log);
}

void expo_time_button_create(FwmExpo *e) {
    FwmServer *server = e->server;
    e->time_button = expo_time_button_show(server->layer_overlay,
                                           e->out->box.x, e->out->box.y,
                                           e->out->box.width, e->out->box.height);
    e->time_reveal = e->time_reveal_target = 1.0;
}

/* Everything the view has on screen, put where the animations say: the page
 * rising into place as it appears, the pill a beat behind it, the words of the
 * section riding on the page and sliding across it when the section changes,
 * and the words of the section before them sliding out the other way. */
static void expo_time_apply(FwmExpo *e) {
    double show = ease(e->time_show);
    double lift = EXPO_TIME_RISE_PX * (1.0 - show);
    double slide = ease(e->time_slide);

    if (e->time_page) {
        wlr_scene_node_set_position(&e->time_page->node, e->time_page_x,
                                    e->time_page_y + (int)lround(lift));
        wlr_scene_buffer_set_opacity(e->time_page, (float)show);
    }
    if (e->time_tabs) {
        double t = ease(clamp01((e->time_show - EXPO_TIME_TABS_LAG)
                                / (1.0 - EXPO_TIME_TABS_LAG)));
        wlr_scene_node_set_position(&e->time_tabs->node, e->time_tabs_x,
                                    e->time_tabs_y
                                    + (int)lround(EXPO_TIME_RISE_PX * (1.0 - t)));
        wlr_scene_buffer_set_opacity(e->time_tabs, (float)t);
        double pos = e->time_tab_from + (e->time_section - e->time_tab_from) * slide;
        expo_time_tabs_set(e->time_tabs, pos, e->time_tab_hover);
    }
    if (e->time_content) {
        double dx = e->time_slide_dir * EXPO_TIME_SLIDE_PX * (1.0 - slide);
        wlr_scene_node_set_position(&e->time_content->node,
                                    e->time_page_x + (int)lround(dx),
                                    e->time_page_y + (int)lround(lift));
        wlr_scene_buffer_set_opacity(e->time_content, (float)(show * slide));
    }
    if (e->time_content_old) {
        double to = -e->time_slide_dir * EXPO_TIME_SLIDE_PX;
        double dx = e->time_old_x0 + (to - e->time_old_x0) * slide;
        wlr_scene_node_set_position(&e->time_content_old->node,
                                    e->time_page_x + (int)lround(dx),
                                    e->time_page_y + (int)lround(lift));
        wlr_scene_buffer_set_opacity(e->time_content_old,
                                     (float)(show * e->time_old_a0 * (1.0 - slide)));
    }
}

/* The section's words, as they stand right now: how far off their place they
 * are and how much of them is showing. What a section change sliding out
 * starts from, so that one caught half way leaves from where it was. */
static void content_now(FwmExpo *e, double *dx, double *alpha) {
    double slide = ease(e->time_slide);
    *dx = e->time_slide_dir * EXPO_TIME_SLIDE_PX * (1.0 - slide);
    *alpha = slide;
}

/* `dir` is which way the sections were stepped: +1 toward the right of the
 * pill, -1 toward the left. The new words come in from that side. */
static void expo_time_set_section(FwmExpo *e, int s, int dir) {
    if (s < 0) s = EXPO_TIME_SECTIONS - 1;
    if (s >= EXPO_TIME_SECTIONS) s = 0;
    if (s == e->time_section) return;

    /* The accent leaves from wherever it is, which mid-change is between two
     * sections rather than on either. */
    double slide = ease(e->time_slide);
    e->time_tab_from = e->time_tab_from + (e->time_section - e->time_tab_from) * slide;

    if (e->time_content_old) cairo_overlay_destroy(e->time_content_old);
    e->time_content_old = e->time_content;
    content_now(e, &e->time_old_x0, &e->time_old_a0);

    e->time_section = s;
    g_section = s;
    e->time_slide_dir = dir;
    e->time_slide = 0.0;
    e->time_content = content_show(e, s);
    expo_time_apply(e);
    wlr_log(WLR_INFO, "expo: time view on %s", section_log_name[s]);
}

void expo_time_open(FwmExpo *e) {
    if (e->time_open || e->leaving || e->drag) return;
    FwmServer *server = e->server;

    /* A page over the strip, so nothing the strip was in the middle of may
     * stay half-done under it: the menu goes, the hovered card lets go, and a
     * flight in progress lands. */
    expo_menu_close(e);
    e->hover = NULL;
    e->orbiting = 0;
    e->time_open = 1;

    /* The pill stands where the strip's key hints stand, and none of those
     * keys mean anything on the page. */
    e->hints_reveal_target = 0.0;
    e->hints_timer = 0.0;
    expo_time_button_set(e->time_button, true, true);
    expo_layout(e);

    /* Still on its way out: it turns round from where it is, rather than a
     * second page appearing over the first. */
    if (e->time_page || e->time_tabs) {
        wlr_log(WLR_INFO, "expo: time view open again on %s",
                section_log_name[e->time_section]);
        return;
    }

    e->time_section = g_section;
    e->time_tab_from = g_section;
    e->time_tab_hover = EXPO_TIME_NONE;
    e->time_slide = 1.0;
    e->time_slide_dir = 1;
    e->time_show = 0.0;
    e->time_page = expo_time_page_show(server->layer_overlay,
                                       e->out->box.x, e->out->box.y,
                                       e->out->box.width, e->out->box.height);
    e->time_tabs = expo_time_tabs_show(server->layer_overlay,
                                       e->out->box.x, e->out->box.y,
                                       e->out->box.width, e->out->box.height,
                                       e->time_section);
    if (e->time_page) {
        e->time_page_x = e->time_page->node.x;
        e->time_page_y = e->time_page->node.y;
    }
    if (e->time_tabs) {
        e->time_tabs_x = e->time_tabs->node.x;
        e->time_tabs_y = e->time_tabs->node.y;
    }
    /* Opened again, it starts from the newest: what has just happened is the
     * likeliest thing to have come looking for. */
    e->time_scroll = 0;
    e->time_content = content_show(e, e->time_section);
    /* Placed before the first frame can show them, or they would flash up
     * whole for a frame and then start to appear. */
    expo_time_apply(e);
    wlr_log(WLR_INFO, "expo: time view open on %s", section_log_name[e->time_section]);
}

static void expo_time_drop(FwmExpo *e) {
    if (e->time_content_old) cairo_overlay_destroy(e->time_content_old);
    if (e->time_content) cairo_overlay_destroy(e->time_content);
    if (e->time_page) cairo_overlay_destroy(e->time_page);
    if (e->time_tabs) cairo_overlay_destroy(e->time_tabs);
    e->time_content_old = NULL;
    e->time_content = NULL;
    e->time_page = NULL;
    e->time_tabs = NULL;
}

void expo_time_close(FwmExpo *e) {
    if (!e->time_open) return;
    /* Shut at once for input — the strip has the pointer and the keys back
     * from this moment — while the pictures go on to play the opening
     * backwards, and are taken down by the tick when it lands. */
    e->time_open = 0;
    e->time_tab_hover = EXPO_TIME_NONE;
    expo_time_button_set(e->time_button,
                         expo_time_button_hit(e->time_button, e->server->cursor->x,
                                              e->server->cursor->y),
                         false);
    wlr_log(WLR_INFO, "expo: time view closed");
}

void expo_time_toggle(FwmExpo *e) {
    if (e->time_open) expo_time_close(e);
    else              expo_time_open(e);
}

void expo_time_teardown(FwmExpo *e) {
    expo_time_drop(e);
    if (e->time_button) cairo_overlay_destroy(e->time_button);
    e->time_button = NULL;
    e->time_open = 0;
}

bool expo_time_animating(FwmExpo *e) {
    return fabs(e->time_reveal - e->time_reveal_target) > 0.002
        || (e->time_open ? e->time_show < 1.0 : e->time_show > 0.0)
        || e->time_slide < 1.0;
}

bool expo_time_handle_key(FwmExpo *e, xkb_keysym_t sym) {
    if (!e->time_open) {
        if (sym != XKB_KEY_t && sym != XKB_KEY_T) return false;
        expo_time_open(e);
        return true;
    }

    switch (sym) {
    /* Back to the strip, not out of it: the page was opened from the strip,
     * and one Escape too many still only closes the strip, as it always did. */
    case XKB_KEY_Escape:
    case XKB_KEY_t:
    case XKB_KEY_T:
        expo_time_close(e);
        return true;
    case XKB_KEY_Left:
    case XKB_KEY_ISO_Left_Tab:
        expo_time_set_section(e, e->time_section - 1, -1);
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_Tab:
        expo_time_set_section(e, e->time_section + 1, 1);
        return true;
    case XKB_KEY_1:
    case XKB_KEY_2:
    case XKB_KEY_3: {
        int s = (int)(sym - XKB_KEY_1);
        expo_time_set_section(e, s, s > e->time_section ? 1 : -1);
        return true;
    }
    default:
        /* Everything else is the strip's, and the strip is behind glass: it
         * is not acted on, and the caller is told so, which is what lets it
         * try the same key in the first layout (the `t` of a Cyrillic one). */
        return false;
    }
}

bool expo_time_handle_motion(FwmExpo *e, double lx, double ly) {
    bool on_button = expo_time_button_hit(e->time_button, lx, ly);
    expo_time_button_set(e->time_button, on_button, e->time_open);

    if (e->time_open) {
        e->time_tab_hover = expo_time_tabs_hit(e->time_tabs, lx, ly);
        expo_time_apply(e);
        return true;
    }
    if (on_button && e->hover) {
        /* Over the button is over nothing on the strip. */
        e->hover = NULL;
        expo_layout(e);
    }
    return on_button;
}

bool expo_time_handle_button(FwmExpo *e, uint32_t button, bool pressed,
                             double lx, double ly) {
    /* A flight ends on whatever release comes next, wherever it lands — the
     * strip's rule, and the strip's to apply. */
    if (!pressed && e->orbiting) return false;
    bool on_button = expo_time_button_hit(e->time_button, lx, ly);
    if (on_button) {
        if (pressed && button == BTN_LEFT) expo_time_toggle(e);
        return true;
    }
    if (!e->time_open) return false;

    /* The page is modal over the strip: a click on it, or past it on the glass
     * between the panels, never falls through to a card it cannot see. */
    if (pressed && button == BTN_LEFT) {
        int tab = expo_time_tabs_hit(e->time_tabs, lx, ly);
        if (tab != EXPO_TIME_NONE)
            expo_time_set_section(e, tab, tab > e->time_section ? 1 : -1);
    }
    return true;
}

bool expo_time_handle_axis(FwmExpo *e, double delta) {
    if (!e->time_open || e->time_section != EXPO_TIME_LOG || delta == 0.0) return true;
    /* A notch of the wheel is about fifteen units: three rows. */
    int rows = (int)lround(delta / 5.0);
    if (rows == 0) rows = delta > 0.0 ? 1 : -1;
    int was = e->time_scroll;
    e->time_scroll += rows;
    if (e->time_scroll < 0) e->time_scroll = 0;
    if (e->time_scroll != was) log_redraw(e);
    return true;
}

void expo_time_tick(FwmExpo *e, double dt) {
    if (e->time_button) {
        /* Down with the strip as it leaves, the way the key hints go. */
        e->time_reveal_target = e->leaving ? 0.0 : 1.0;
        double gap = e->time_reveal_target - e->time_reveal;
        if (fabs(gap) > 0.002) e->time_reveal += gap * (1.0 - exp(-EXPO_HINT_SLIDE * dt));
        else                   e->time_reveal = e->time_reveal_target;
        expo_time_button_place(e->time_button, e->out->box.x, e->out->box.y,
                               e->out->box.width, e->out->box.height,
                               e->time_reveal);
    }

    if (!e->time_page && !e->time_tabs) return;

    if (e->time_open)
        e->time_show = clamp01(e->time_show + dt * 1000.0 / EXPO_TIME_OPEN_MS);
    else
        e->time_show = clamp01(e->time_show - dt * 1000.0 / EXPO_TIME_CLOSE_MS);

    if (e->time_slide < 1.0) {
        e->time_slide = clamp01(e->time_slide + dt * 1000.0 / EXPO_TIME_SLIDE_MS);
        if (e->time_slide >= 1.0) {
            /* Landed: the section it came from is gone, and the accent is
             * wholly on the new one, so the next change starts from there. */
            if (e->time_content_old) cairo_overlay_destroy(e->time_content_old);
            e->time_content_old = NULL;
            e->time_tab_from = e->time_section;
        }
    }

    if (!e->time_open && e->time_show <= 0.0) {
        expo_time_drop(e);
        return;
    }
    /* A window opened or closed while the page is up: the log says so now,
     * not the next time it is opened. */
    if (e->time_open && e->time_section == EXPO_TIME_LOG
        && journal_list_version(e->server->journal) != e->time_log_version)
        log_redraw(e);
    expo_time_apply(e);
}
