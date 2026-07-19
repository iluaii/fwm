#ifndef FWM_WELCOME_H
#define FWM_WELCOME_H

#include <wlr/types/wlr_scene.h>

struct wlr_scene_buffer *welcome_show(struct wlr_scene_tree *parent, int screen_w, int screen_h);
void welcome_set_welcomed(void);

#endif /* FWM_WELCOME_H */
