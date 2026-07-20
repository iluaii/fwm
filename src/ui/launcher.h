#ifndef FWM_LAUNCHER_H
#define FWM_LAUNCHER_H

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

struct FwmServer;
typedef struct Launcher Launcher;

Launcher *launcher_create(struct FwmServer *server);
void launcher_destroy(Launcher *launcher);

void launcher_toggle(Launcher *launcher);
/* Same overlay, listing images from [wallpaper_picker] dir; Enter applies the
 * selected one immediately. Toggling between modes switches the list. */
void launcher_toggle_wallpapers(Launcher *launcher);
void launcher_toggle_mode(Launcher *launcher, int mode);
bool launcher_is_open(Launcher *launcher);

/* Feed a pressed key while the launcher is open. `utf8` is the text the key
 * produces ("" if none). Returns true if the key was consumed. */
bool launcher_handle_key(Launcher *launcher, xkb_keysym_t sym, const char *utf8);

/* Advance tile physics and redraw the overlay; call once per physics tick. */
void launcher_tick(Launcher *launcher, double dt);

/* Pointer support while open: motion moves the hover selection; a button
 * press launches the tile under the cursor or closes on a click outside.
 * Both are no-ops when closed; handle_button returns true if consumed. */
void launcher_handle_motion(Launcher *launcher, double lx, double ly);
bool launcher_handle_button(Launcher *launcher, double lx, double ly, bool pressed);

#endif /* FWM_LAUNCHER_H */
