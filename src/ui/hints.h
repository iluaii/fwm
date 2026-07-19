#ifndef FWM_HINTS_H
#define FWM_HINTS_H

#include <wlr/types/wlr_scene.h>
#include "../config.h"

struct wlr_scene_buffer *hints_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                    const FwmConfig *cfg);

#endif /* FWM_HINTS_H */
