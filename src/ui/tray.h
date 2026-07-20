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
    double active_pos; /* fractional desktop position (camera_x / screen_w):
                        * the underline marker glides with the camera slide */
    double opacity; /* island fill alpha 0..1 (decor.tray_opacity) */
    char kbd_layout[8]; /* short active-layout tag ("EN", "RU"); "" hides it */
    int error_count;    /* config problems; >0 draws the warning pill */
    int error_expanded; /* detail panel open — pill renders as active */
} TrayData;

struct wlr_scene_buffer *tray_init(struct wlr_scene_tree *parent, int screen_width);
void tray_redraw(struct wlr_scene_buffer *tray_buf, const TrayData *data);

/* Hit-test for the config-error pill, in tray-buffer-local coordinates.
 * Valid once the pill has been drawn; returns 0 when no pill is on screen. */
int tray_error_pill_hit(double x, double y);

#endif /* FWM_TRAY_H */
