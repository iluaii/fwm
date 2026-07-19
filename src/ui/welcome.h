#ifndef FWM_WELCOME_H
#define FWM_WELCOME_H

#include <wlr/types/wlr_scene.h>
#include "../config.h"

struct wlr_scene_buffer *welcome_show(struct wlr_scene_tree *parent, int screen_w, int screen_h,
                                      const FwmConfig *cfg);
void welcome_set_welcomed(void);

#endif /* FWM_WELCOME_H */
