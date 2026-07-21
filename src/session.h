#ifndef FWM_SESSION_H
#define FWM_SESSION_H

#include <sys/types.h>

struct FwmServer;
struct FwmView;

/*
 * Session save & restore.
 *
 * A Wayland compositor cannot hot-restart the way i3 can: it IS the display
 * server, so every client's connection — and all the protocol state behind it —
 * dies with the process. Preserving the listening socket across exec() does not
 * help, because the new process cannot adopt connections whose object tables
 * live in the old one's heap.
 *
 * So instead of pretending to restart, fwm remembers WHAT was running and on
 * WHICH desktop, and puts it back afterwards. Applications are relaunched, not
 * resumed: anything unsaved inside them is still lost. What this buys is that a
 * crash (or a deliberate restart after a rebuild) costs you a few seconds
 * instead of your whole working layout.
 *
 * State lives in ~/.local/state/fwm/session, one line per application:
 *
 *     <desktop>\t<arg0>\t<arg1>...
 *
 * Tab-separated rather than space-separated so that arguments containing
 * spaces survive the round trip; the argv is read straight out of
 * /proc/<pid>/cmdline and executed back with execvp, never through a shell,
 * so nothing has to be quoted or escaped.
 */

/* Write the current window set if it differs from what is on disk. Cheap and
 * self-debouncing — call it freely from the frame path. */
void session_maybe_save(struct FwmServer *server);

/* Read the state file and relaunch what it lists. Call once, after the
 * compositor is accepting clients. */
void session_restore(struct FwmServer *server);

/* If this newly mapped window belongs to something session_restore launched,
 * return the desktop it should go to and forget the entry; -1 otherwise. */
int session_claim_desktop(struct FwmServer *server, struct FwmView *view);

/* Free the pending-restore list. */
void session_finish(struct FwmServer *server);

#endif /* FWM_SESSION_H */
