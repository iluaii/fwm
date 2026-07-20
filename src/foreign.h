#ifndef FWM_FOREIGN_H
#define FWM_FOREIGN_H

#include <stdbool.h>

struct FwmServer;
struct FwmView;

/* wlr-foreign-toplevel-management-v1: publishes the window list so external
 * panels can show a taskbar (waybar's wlr/taskbar) and window switchers can
 * offer more than the compositor's own binds.
 *
 * fwm's own tray does not use this — it reads the view list directly. This
 * exists purely for outside clients. */
void foreign_init(struct FwmServer *server);

/* Mirror one view's lifetime into a handle. map creates it, unmap destroys it,
 * and the rest push state the panel displays. All are no-ops when the manager
 * failed to create. */
void foreign_view_map(struct FwmView *view);
void foreign_view_unmap(struct FwmView *view);
void foreign_view_title_changed(struct FwmView *view);
void foreign_view_set_activated(struct FwmView *view, bool activated);
void foreign_view_set_fullscreen(struct FwmView *view, bool fullscreen);

#endif /* FWM_FOREIGN_H */
