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
 * SOUND IS SOMEBODY BEING HERE. A film playing with nobody touching a key is
 * the case every idle timer gets wrong, and the one that frightens you when it
 * does: the screens went out in the middle of a video. A player that raises an
 * idle inhibitor is already handled a paragraph below; the ones that do not —
 * a browser tab being the usual — are handled by asking the sound card whether
 * anything is coming out of it. See idle_audio_holds.
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

#include <glob.h>
#include <stdio.h>
#include <string.h>

#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "server_internal.h"

/* How long before a threshold the sound card starts being asked about, and how
 * long between two asks. Both in seconds; see idle_audio_holds. */
#define IDLE_AUDIO_LOOKAHEAD 5.0
#define IDLE_AUDIO_POLL      1.0

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
        /* A screen that already refused to go dark this stretch is not asked
         * again. See idle_blank_failed on FwmOutput: the threshold stays
         * crossed for as long as the session is left alone, so without this a
         * driver that says no once is handed the same atomic commit on every
         * beat until somebody touches a key — hours of them, on a machine
         * nobody is watching, which is the shape of the fault this guards. */
        if (on && o->idle_blank_failed) continue;

        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, on ? false : true);
        bool ok = wlr_output_commit_state(o->wlr_output, &state);
        wlr_output_state_finish(&state);

        if (ok) {
            o->idle_blanked = on ? 1 : 0;
            o->idle_blank_failed = 0;
        } else if (on) {
            /* Left lit, and not asked again until the next stretch. */
            o->idle_blank_failed = 1;
            wlr_log(WLR_ERROR, "idle: %s refused to blank; leaving it lit",
                    o->wlr_output->name);
        } else {
            /* THE DANGEROUS ONE. A screen we cannot light again is still a
             * screen the user is sitting in front of, and leaving the flag up
             * would have server_schedule_frames skip it for the rest of the
             * session: the panel comes back on by itself and shows a picture
             * that never updates again. Give up the flag rather than the
             * screen — a frame asked for on a monitor that is genuinely off
             * costs nothing, and a monitor that is genuinely on gets drawn. */
            o->idle_blanked = 0;
            wlr_log(WLR_ERROR, "idle: %s refused to light; handing it back to "
                               "the frame loop anyway", o->wlr_output->name);
        }

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
    /* A refusal belongs to one stretch of idleness. The next one asks again:
     * whatever the driver was busy with — a modeset, a lock surface still
     * being configured — is long over by then. */
    FwmOutput *o;
    wl_list_for_each(o, &server->outputs, link) o->idle_blank_failed = 0;
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

/* Is a sound card playing something right now?
 *
 * ALSA's own bookkeeping rather than the sound server's: every playback
 * substream has a status file that reads "state: RUNNING" for exactly as long
 * as the device is being fed, and both PipeWire and PulseAudio close the device
 * a few seconds after the last stream falls quiet. A dozen tiny reads and
 * nothing else — no client library, no connecting to a daemon that may not be
 * there (src/audio.h has the story of what connecting can cost), and the same
 * answer whether the sound came from a browser, mpv or a game.
 *
 * What it cannot see is sound that never reaches a card, which in practice
 * means Bluetooth: the headset is fed over a socket of its own and no ALSA
 * device is involved. Nothing here helps there — a player that raises an idle
 * inhibitor still does, and so does swayidle.
 *
 * The other end of the same trade: a sound server told never to suspend an idle
 * device (wireplumber's session.suspend-timeout-seconds = 0 is the usual
 * reason, to stop a speaker popping) holds a device RUNNING all night, which is
 * indistinguishable from a machine that never stops playing. Such a session
 * wants [idle] audio_holds = false. */
static int idle_audio_playing(void) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob("/proc/asound/card*/pcm*p/sub*/status", 0, NULL, &g) != 0) {
        globfree(&g);   /* NOMATCH is a machine with no sound cards at all */
        return 0;
    }

    int playing = 0;
    for (size_t i = 0; i < g.gl_pathc && !playing; i++) {
        FILE *f = fopen(g.gl_pathv[i], "r");
        if (!f) continue;
        /* One word, "closed", on a free device; on a busy one the state is the
         * first line. Only RUNNING is sound actually coming out — SETUP,
         * PREPARED and XRUN are a device that is open and moving no air. */
        char line[64];
        if (fgets(line, sizeof(line), f) && strstr(line, "RUNNING")) playing = 1;
        fclose(f);
    }
    globfree(&g);
    return playing;
}

/* Should the timers be held off because something is playing?
 *
 * ASKED ONLY WHEN THE ANSWER IS ABOUT TO MATTER — within a few seconds of a
 * threshold that has not fired yet — and at most once a second even then. The
 * tick runs at the monitor's refresh rate, and reading procfs a hundred times a
 * second all day to settle a question that comes up twice an evening would be a
 * poll with nothing to poll for. Holding puts the clock back to zero, so a film
 * that runs for two hours asks about five times per ten minutes and no more.
 *
 * RESETS the clock rather than freezing it, which is where this parts company
 * with an inhibitor. An inhibitor is a client's promise, and it ends when the
 * client says it ends; this is a guess made from a sound card, and a guess that
 * expired the instant the sound stopped would put the screens out in the pause
 * between two songs, or while a film is held to answer the door. Every stretch
 * of quiet gets the whole of blank_after to itself — including the one that
 * begins the moment a poll first hears silence, which is why the tick that
 * hears it still counts as a hold.
 *
 * Not while the session is locked: sound behind a lock screen is a radio
 * playing in an empty room, and holding for it would leave the panels lit all
 * night over a playlist somebody forgot to stop. */
static int idle_audio_holds(FwmServer *server, double dt) {
    const IdleConfig *cfg = &server->config.idle;

    int pending =
        (cfg->blank_after > 0.0 && !server->idle_blanked &&
         server->idle_secs >= cfg->blank_after - IDLE_AUDIO_LOOKAHEAD) ||
        (cfg->lock_after > 0.0 && cfg->lock[0] && !server->idle_locked &&
         server->idle_secs >= cfg->lock_after - IDLE_AUDIO_LOOKAHEAD);

    if (!cfg->audio_holds || server->locked || !pending) {
        server->idle_audio = 0;
        server->idle_audio_wait = 0.0;
        return 0;
    }

    server->idle_audio_wait -= dt;
    if (server->idle_audio_wait > 0.0) return server->idle_audio;
    server->idle_audio_wait = IDLE_AUDIO_POLL;

    int was = server->idle_audio;
    server->idle_audio = idle_audio_playing();
    /* Once per stretch of counting, not once a tick: the hold below puts the
     * clock back to the start, which ends this stretch and clears the flag, so
     * a film says this about as often as blank_after comes round. */
    if (server->idle_audio != was)
        wlr_log(WLR_DEBUG, "idle: %s, the timers start over",
                server->idle_audio ? "sound is playing" : "the sound stopped");

    return server->idle_audio || was;
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

    /* Sound coming out of the machine stands in for a keystroke, on the grounds
     * that a session nobody is in makes no noise. This is the same "the user is
     * here" the inhibitor above decides, arrived at from the other side: what a
     * client says, and what the machine can be seen doing. */
    if (idle_audio_holds(server, dt)) {
        server->idle_secs = 0.0;
        return;
    }

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
    /* And not at all if the session is already locked. Locking by hand and
     * then walking away is the ordinary way this file is reached, and starting
     * a second locker on top of the first is at best a client that is refused
     * (lock.c) and exits, at worst one that retries. Nothing is gained either
     * way: the session is already locked, which is all the timer wanted. */
    if (cfg->lock_after > 0.0 && cfg->lock[0] && !server->idle_locked &&
        !server->locked && server->idle_secs >= cfg->lock_after) {
        server->idle_locked = 1;
        wlr_log(WLR_INFO, "idle: locking with \"%s\"", cfg->lock);
        server_spawn(cfg->lock);
    }
}
