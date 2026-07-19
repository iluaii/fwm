#ifndef FWM_TRAY_H
#define FWM_TRAY_H

#include <wlr/types/wlr_scene.h>
#include <wlr/render/wlr_renderer.h>

#define TRAY_HEIGHT 28

typedef struct {
    const char *win_name;
    double speed;
    double angle;
    double mass;
    int flying;
    int desktop_window_counts[10];
    int active_desktop;
    double opacity; /* island fill alpha 0..1 (decor.tray_opacity) */
} TrayData;

struct wlr_scene_buffer *tray_init(struct wlr_scene_tree *parent, int screen_width);
void tray_redraw(struct wlr_scene_buffer *tray_buf, const TrayData *data);

#endif /* FWM_TRAY_H */
