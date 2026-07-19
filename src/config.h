#ifndef FWM_CONFIG_H
#define FWM_CONFIG_H

#include <xkbcommon/xkbcommon.h>

/* Modifiers */
#define FWM_MOD_SHIFT (1 << 0)
#define FWM_MOD_CTRL  (1 << 1)
#define FWM_MOD_ALT   (1 << 2)
#define FWM_MOD_LOGO  (1 << 3)

/* ── physics ─────────────────────────────────────────────────────────── */

typedef struct {
    double friction;
    double mass_density;
    double throw_speed_multiplier;
    double max_throw_speed;
    double stop_speed_threshold;
    double restitution;
    double gravity;
    double tick_rate;
} PhysicsConfig;

/* ── tiling ──────────────────────────────────────────────────────────── */

typedef struct {
    int    gaps_in;    /* gap between adjacent tiles (px) */
    int    gaps_out;   /* gap between tiles and the screen edges (px) */
    double anim_speed; /* tile-glide speed, 1/s (higher = snappier); <= 0 disables */
} TilingConfig;

/* ── decorations ─────────────────────────────────────────────────────── */

typedef struct {
    int   border_width;      /* px; 0 disables borders */
    float col_active[4];     /* RGBA 0..1, focused window */
    float col_inactive[4];   /* RGBA 0..1, unfocused windows */
    double fade_in_ms;       /* window fade-in duration; <= 0 disables */
} DecorConfig;

/* ── binds ───────────────────────────────────────────────────────────── */

/*
 * action string format:
 *   system:  "killclient", "toggle_tiling", "EXIT", ...
 *   spawn:   "spawn:kitty -o background_opacity=1.0"
 *   camera:  "move_camera:-50"
 *   view:    "view:3"
 */

typedef struct {
    unsigned int    mod; /* FWM_MOD_* masks */
    xkb_keysym_t    key;
    char            action[256];
} KeyBind;

/* ── wallpaper / parallax ────────────────────────────────────────────── */

/*
 * Each layer is one image (PNG/JPEG/WebP) that scrolls at `factor` px per camera
 * Layers are drawn back-to-front in the order listed. `fit` controls scaling:
 *   "cover"   (default) fills the screen, cropping overflow; static.
 *   "contain"           shows the whole image centered (letterboxed); static.
 *   "pan"               a background you walk across: the image fills the screen
 *                       and scrolls smoothly from its left edge (desktop 0) to
 *                       its right edge (last desktop). `zoom` trades sharpness
 *                       for how far it travels (auto = native = sharpest).
 * Configured in TOML as an array of tables:
 *
 *   [[wallpaper]]
 *   path = "/home/me/Pictures/scene.jpg"
 *   fit  = "pan"
 *   # zoom = 1.5   # optional: more travel, slightly softer
 */
enum {
    WALLPAPER_FIT_COVER   = 0, /* fill screen, crop overflow, static */
    WALLPAPER_FIT_CONTAIN = 1, /* whole image, letterboxed, static */
    WALLPAPER_FIT_PAN     = 2, /* fill screen height, walk across the width (parallax) */
};

typedef struct {
    char   path[512];
    int    fit;    /* WALLPAPER_FIT_* */
    double zoom;   /* "pan" only: render width = screen_w * zoom; <= 0 = auto
                    * (image's native width — sharpest, no upscaling). Larger
                    * zoom = more travel but the image is scaled up (softer). */
} WallpaperLayer;

/* ── top-level config ────────────────────────────────────────────────── */

typedef struct {
    PhysicsConfig   physics;
    TilingConfig    tiling;
    DecorConfig     decor;
    KeyBind        *keys;
    int             key_count;
    WallpaperLayer *wallpapers;
    int             wallpaper_count;
} FwmConfig;

/* ── api ─────────────────────────────────────────────────────────────── */

void config_load(FwmConfig *cfg, const char *path);
void config_free(FwmConfig *cfg);

#define FWM_CONFIG_PATH "/.config/fwm/config.toml"

#endif /* FWM_CONFIG_H */
