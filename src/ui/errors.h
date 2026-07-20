#ifndef FWM_ERRORS_H
#define FWM_ERRORS_H

#include <wlr/types/wlr_scene.h>
#include "../config.h"

/* Detail panel for config problems, opened from the tray's warning pill.
 * Anchored under the tray rather than centred: it annotates the pill. */
struct wlr_scene_buffer *errors_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                     const FwmConfig *cfg);

#endif /* FWM_ERRORS_H */
