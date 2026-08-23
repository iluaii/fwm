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

/* What an unattended session does with itself: the screens go dark after
 * [idle] blank_after, and [idle] lock runs after lock_after. See IdleConfig in
 * config.h for why this is in the compositor at all rather than left to
 * swayidle — which still works, on its own timers, through ext-idle-notify.
 *
 * The clock is the tick's, not a timer of its own. The physics tick never
 * stops — it drops to a 200ms heartbeat and keeps beating — so an idle session
 * is already being counted; a fourth wl_event_source to count the same
 * seconds would only be a second place for the two to disagree.
 *
 * BLANKING IS NOT output_off. That action takes a monitor out of the layout,
 * hands its desktop back to the pool and moves the pointer off it, because it
 * means "I am not using this screen". Idle means the opposite: everything is
 * exactly where it was and only the light is off, so the commit here changes
 * `enabled` on the wlr_output and touches nothing fwm knows about the monitor.
 * Windows do not move, the desktop stays where it was, and the pointer is
 * still standing where it was left.
 */

#include "server.h"
#include "view.h"

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "server_internal.h"

/* How many screens the idle timer is currently holding dark. */
static int idle_blanked_count(FwmServer *server) {
    int n = 0;
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link)
        if (o->idle_blanked) n++;
    return n;
}

/* Light every screen we put out, or put out every screen that is lit.
 *
 * Only screens fwm believes are on: a laptop panel that is dark because the
 * lid is shut, or because `output_off` was pressed, must not be lit by a
 * keystroke ten minutes later. `idle_blanked` on the output is what remembers
 * which ones were ours to give back. */
static void idle_blank(FwmServer *server, int on) {
    /* Real monitors only. On a nested backend an "output" is a window on
     * somebody else's desktop, and blanking it there means a window that
     * vanishes while its compositor is being worked on — no session of ours to
     * darken, and nothing a developer asked for. */
    if (!server->session) return;

    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (on ? (!o->enabled || o->idle_blanked) : !o->idle_blanked) continue;

        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, on ? false : true);
        if (wlr_output_commit_state(o->wlr_output, &state))
            o->idle_blanked = on ? 1 : 0;
        wlr_output_state_finish(&state);

        /* Coming back needs a frame asked for by hand: nothing on screen
         * changed while the monitor was dark, so there is no damage waiting to
         * schedule one, and the scene would sit there composited but unshown. */
        if (!on && !o->idle_blanked) wlr_output_schedule_frame(o->wlr_output);
    }

    /* Counted rather than assumed. A session whose screens are ALL off already
     * — the lid shut on a laptop with nothing plugged in, every monitor turned
     * off by hand — blanks nothing, and must not come out of this believing it
     * did: the flag is what makes the next keystroke count as a wake-up, and
     * swallowing that keystroke would be a key press lost to a screen that was
     * never dark on our account. */
    int held = idle_blanked_count(server);
    if (server->idle_blanked == (held > 0)) return;
    server->idle_blanked = held > 0;
    wlr_log(WLR_INFO, "idle: screens %s", on ? "off" : "on");
}

/* Input arrived. Called from server_notify_activity, so every path that counts
 * as activity for ext-idle-notify counts as activity here too — one definition
 * of "the user is here", not two. */
void server_idle_activity(FwmServer *server) {
    server->idle_secs = 0.0;
    /* The locker may run again next time the session is left alone, but not
     * twice for one stretch of it. */
    server->idle_locked = 0;

    if (!server->idle_blanked) return;
    idle_blank(server, 0);
    /* The key or the button that did this is spent on it; see below. */
    server->idle_woke = 1;
}

/* Was this event the one that lit the screens back up?
 *
 * A press that wakes a dark screen should not also reach what is under it: the
 * space that brought the desktop back would otherwise be typed into whatever
 * had focus ten minutes ago, and the click would land on a button nobody could
 * see. Reading it clears it, so the NEXT press is an ordinary press.
 *
 * Pointer motion calls this and ignores the answer, which is the whole rule in
 * one line: moving the mouse wakes the screen and swallows nothing, because
 * there is nothing to swallow — a motion event is not an action. */
int server_idle_consume_wake(FwmServer *server) {
    int woke = server->idle_woke;
    server->idle_woke = 0;
    return woke;
}

/* Something OUTSIDE this file decided a monitor's power: swayidle through
 * wlr-output-power-management, `output_off`, the lid, a config reload.
 *
 * Without this the two would fight quietly. A screen we blanked and somebody
 * else lit is one this file still believes is dark, so the frame loop keeps
 * skipping it (server_schedule_frames) and it stays black with the monitor
 * switched on — and a session whose screens were all lit from outside would
 * never blank again, because the compositor thought it had already done it.
 *
 * `blanked` is only ever 1 for the external idle path. A screen turned off as
 * a screen — not as a light — is not the idle timer's to light again, so
 * everything else passes 0 and the next keystroke leaves it dark. */
void server_idle_set_blanked(FwmServer *server, FwmOutput *out, int blanked) {
    if (!out) return;
    out->idle_blanked = blanked ? 1 : 0;
    server->idle_blanked = idle_blanked_count(server) > 0;
}

void server_idle_tick(FwmServer *server, double dt) {
    const IdleConfig *cfg = &server->config.idle;

    /* The wake mark lasts for the event that set it and no longer.
     *
     * A key or a click reads it in the same breath, so it is gone before this
     * runs. Everything else that counts as activity does NOT read it — a
     * touchpad swipe, a scroll, a tablet — and a mark left standing by one of
     * those would be spent on whatever was pressed next, which could be
     * minutes later: swipe to wake, start typing, lose the first letter. One
     * tick is the whole life of it. */
    server->idle_woke = 0;

    /* An idle inhibitor freezes the clock where it stands rather than resetting
     * it. A film that runs for two hours holds the screen for two hours and the
     * session is dark a minute after it ends, which is what "inhibit" means —
     * resetting instead would hand the user a fresh ten minutes for having
     * watched something. idle_inhibit_refresh already decides which inhibitors
     * count (theirs must be on a desktop somebody is looking at). */
    if (server->idle_inhibited) return;

    server->idle_secs += dt;

    if (cfg->blank_after > 0.0 && !server->idle_blanked &&
        server->idle_secs >= cfg->blank_after)
        idle_blank(server, 1);

    /* `fwmctl set idle.blank_after=0` with the screens already dark: the dial
     * that put them out is the dial that brings them back, or turning blanking
     * off would be a setting you cannot see the effect of until you touch the
     * mouse — which would have lit them anyway. */
    if (cfg->blank_after <= 0.0 && server->idle_blanked)
        idle_blank(server, 0);

    /* Locking is a command, not a state fwm holds: it starts the locker and the
     * session-lock protocol takes it from there. Once per stretch of idleness —
     * a locker already on screen must not be started again every tick. */
    if (cfg->lock_after > 0.0 && cfg->lock[0] && !server->idle_locked &&
        server->idle_secs >= cfg->lock_after) {
        server->idle_locked = 1;
        wlr_log(WLR_INFO, "idle: locking with \"%s\"", cfg->lock);
        server_spawn(cfg->lock);
    }
}
