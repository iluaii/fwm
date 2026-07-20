#ifndef FWM_THEME_H
#define FWM_THEME_H

#include "config.h"

/* Every colour the UI overlays draw with. One theme is live at a time
 * (`theme_get()`); the overlays read it instead of holding their own
 * constants, so a palette change repaints the whole system consistently.
 *
 * With [decor] color_source = "wallpaper" the island fill is tinted toward the
 * wallpaper's dominant hue and the accent is lifted from its most vivid
 * colour. Text stays put: the fill is always kept dark, so contrast never
 * depends on the image. */
typedef struct {
    double pill[3];   /* island / panel fill */
    double sel[3];    /* launcher selection row */
    double text[3];   /* primary text */
    double muted[3];  /* secondary text */
    double dim[3];    /* empty desktop dot */
    double accent[3]; /* active desktop marker, tab underline, focus border */

    /* Border colours for wlr_scene_rect — PREMULTIPLIED, matching
     * parse_hex_color's output. */
    float border_active[4];
    float border_inactive[4];
} FwmTheme;

/* The palette the overlays draw with. Never NULL: falls back to the built-in
 * dark scheme before theme_build() has run. */
const FwmTheme *theme_get(void);

/* Bumped by every theme_build(). Overlays that skip redraws by comparing a
 * content signature (the tray) must fold this in, or a reloaded palette would
 * not repaint until something else happened to change. */
unsigned theme_generation(void);

/* Build the live theme from `cfg`. With color_source = "wallpaper" this reads
 * the first wallpaper layer (downscaled, ~10ms) and derives tint + accent;
 * anything that goes wrong falls back to the configured colours, reporting
 * through the caller's config diagnostics where it is the user's mistake.
 * Safe to call again on config reload. */
void theme_build(FwmConfig *cfg);

#endif /* FWM_THEME_H */
