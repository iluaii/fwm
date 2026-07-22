/*
 * fwm — a Wayland compositor
 * Copyright (C) 2026 Ilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef FWM_CONFIG_H
#define FWM_CONFIG_H

#include <regex.h>
#include <stddef.h>
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

/* ── camera ──────────────────────────────────────────────────────────── */

typedef struct {
    double anim_ms;    /* desktop-switch slide duration; <= 0 = instant snap */
    double free_speed; /* held move_camera: chase rate, 1/s; higher = tighter */
} CameraConfig;

/* ── decorations ─────────────────────────────────────────────────────── */

typedef struct {
    int   border_width;      /* px; 0 disables borders */
    float col_active[4];     /* RGBA 0..1, focused window */
    float col_inactive[4];   /* RGBA 0..1, unfocused windows */
    double fade_in_ms;       /* window fade-in duration; <= 0 disables */
    double wallpaper_fade_ms;/* wallpaper cross-fade duration; <= 0 = instant cut */
    double tray_opacity;     /* island fill alpha 0..1 for the tray bar */
    double launcher_opacity; /* island fill alpha 0..1 for the app launcher */
    char   icon_theme[64];   /* launcher icon theme; "" = auto (gtk3 setting, then hicolor) */
    int    color_source;     /* COLOR_SOURCE_* — where the UI palette comes from */
    double tint_strength;    /* 0..1: how far the island fill moves toward the
                              * wallpaper hue when color_source = wallpaper */
} DecorConfig;

enum {
    COLOR_SOURCE_CONFIG    = 0, /* col_active/col_inactive + built-in dark scheme */
    COLOR_SOURCE_WALLPAPER = 1, /* tint + accent derived from the wallpaper image */
};

/* ── input ───────────────────────────────────────────────────────────── */

typedef struct {
    char kbd_layout[64];   /* xkb layout list, e.g. "us,ru"; "" = environment */
    char kbd_variant[64];  /* xkb variant list, may be empty */
    char kbd_options[128]; /* xkb options, e.g. "grp:alt_shift_toggle" */
    int  repeat_rate;      /* key repeat, chars/s */
    int  repeat_delay;     /* ms before repeat starts */
} InputConfig;

/* ── focus ───────────────────────────────────────────────────────────── */

/* What an xdg-activation request (an app asking to be raised — a link opening
 * in a running browser, a chat client jumping to a message) is allowed to do.
 * Split three ways because on fwm the disruptive part is not the focus change
 * but the CAMERA leaving the desktop the user is working on. */
typedef enum {
    FOCUS_ACTIVATE_NEVER = 0,   /* ignore activation requests entirely */
    FOCUS_ACTIVATE_SAME_DESKTOP,/* focus only if already on the visible desktop */
    FOCUS_ACTIVATE_ALWAYS,      /* focus, and pan the camera to reach it */
} FocusActivatePolicy;

typedef struct {
    FocusActivatePolicy on_activate;
} FocusConfig;

/* ── effects ─────────────────────────────────────────────────────────── */

typedef struct {
    double camera_shake;  /* impact shake strength; 0 disables, 1 = default */
    double squash;        /* impact squash & stretch; 0 disables, 1 = default */
} EffectsConfig;

/* ── session ─────────────────────────────────────────────────────────── */

/*
 * When a restarted fwm should put your applications back.
 *
 * The distinction that matters is not "did fwm start" but "did the last run
 * end the way you meant it to". Coming back from a crash with your windows
 * where you left them is a rescue; doing the same after you deliberately
 * closed everything and logged out is just an unwanted pile of windows.
 *
 * fwm tells the two apart without help: the state file is deleted on a clean
 * shutdown, so finding one at startup means the previous run died.
 */
typedef enum {
    SESSION_RESTORE_CRASH = 0, /* default — only after an unclean exit */
    SESSION_RESTORE_ALWAYS,    /* every start, including a normal login */
    SESSION_RESTORE_NEVER,     /* never; nothing is recorded either */
} SessionRestorePolicy;

typedef struct {
    SessionRestorePolicy restore;
} SessionConfig;

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
    double pan_crop; /* "pan" only, 0..0.9: how much of the image height may be
                      * given up to buy pan travel when the image is not wide
                      * enough to pan on its own. 0 (default) = never crop, so
                      * only genuinely wide images move. */
    double zoom;   /* "pan" only: render width = screen_w * zoom; <= 0 = auto
                    * (image's native width — sharpest, no upscaling). Larger
                    * zoom = more travel but the image is scaled up (softer). */
} WallpaperLayer;

/* ── window rules ────────────────────────────────────────────────────── */

/*
 * Per-window overrides applied ONCE, when the window is mapped. Configured as
 * an array of tables, each with at least one matcher:
 *
 *   [[rule]]
 *   app_id    = "^mpv$"
 *   nocollide = true
 *   pin       = true
 *
 * Matchers are POSIX extended regexes (libc regcomp, so no new dependency)
 * tested against the window's app_id / title — the same anchored style
 * Hyprland users already write. A rule with several matchers requires ALL of
 * them to hit. Rules are evaluated in file order and every match is applied,
 * so a later rule overrides an earlier one field by field.
 *
 * There is deliberately no per-window "float": tiling on fwm is a property of
 * the DESKTOP, not of the window, so such a rule could not be honoured.
 */

#define CONFIG_MAX_RULES 64

typedef struct {
    /* Compiled matchers; the has_* flag says whether the regex_t is live. */
    int     has_app_id;
    int     has_title;
    regex_t re_app_id;
    regex_t re_title;
    char    pat_app_id[128];  /* kept for diagnostics / `fwmctl rules` */
    char    pat_title[128];

    /* Properties. -1 means "this rule says nothing about it", so an earlier
     * rule's decision survives. */
    int nocollide;  /* PhysicsBody.no_collide */
    int pin;        /* PhysicsBody.pinned */
    int desktop;    /* 0..9; where the window opens */
} ConfigRule;

/* ── runtime-settable options ────────────────────────────────────────── */

/*
 * The scalar knobs, addressable by name ("physics.gravity") so that IPC can
 * read and write any of them without a line of code per option — the one part
 * of Hyprland's string-keyed config registry that pays for itself at this
 * size. Arrays ([binds], [[wallpaper]], [[rule]]) are NOT here: they are not
 * scalars, and reloading is the right way to change them.
 *
 * Writes are RUNTIME-ONLY. config.toml is never rewritten — it is the source
 * of truth, and `reload_config` (super+shift+r) discards every override. This
 * matches how the wallpaper picker already behaves.
 */

typedef enum {
    CFG_OPT_DOUBLE,
    CFG_OPT_INT,
    CFG_OPT_COLOR,   /* float[4] premultiplied RGBA, written as "#RRGGBB[AA]" */
} ConfigOptType;

typedef struct {
    const char   *name;   /* "section.field" */
    ConfigOptType type;
    size_t        offset; /* into FwmConfig */
    double        min, max;
    const char   *help;
} ConfigOption;

/* ── config diagnostics ──────────────────────────────────────────────── */

/* A broken config must never leave the compositor unusable: config_load always
 * produces a working FwmConfig (defaults, plus built-in binds when the file
 * yielded none) and records what went wrong here. The tray surfaces the count
 * as a clickable pill; the expanded panel lists these messages. */

#define CONFIG_MAX_ERRORS 24
#define CONFIG_ERR_LEN    200

typedef struct {
    char msg[CONFIG_ERR_LEN];
} ConfigError;

/* ── top-level config ────────────────────────────────────────────────── */

typedef struct {
    PhysicsConfig   physics;
    TilingConfig    tiling;
    CameraConfig    camera;
    DecorConfig     decor;
    InputConfig     input;
    FocusConfig     focus;
    EffectsConfig   effects;
    SessionConfig   session;
    KeyBind        *keys;
    int             key_count;
    WallpaperLayer *wallpapers;
    int             wallpaper_count;
    ConfigRule     *rules;
    int             rule_count;
    /* [wallpaper_picker] dir — where the built-in picker looks for images.
     * "~" is expanded at load. */
    char            wallpaper_dir[512];

    /* diagnostics from the last config_load */
    ConfigError     errors[CONFIG_MAX_ERRORS];
    int             error_count;    /* messages stored in errors[] */
    int             error_total;    /* problems seen (may exceed the stored cap) */
    int             fallback_binds; /* built-in binds in use (file had none usable) */
    char            source[512];    /* file this config was loaded from */
} FwmConfig;

/* ── api ─────────────────────────────────────────────────────────────── */

void config_load(FwmConfig *cfg, const char *path);
void config_free(FwmConfig *cfg);

/* Record a config problem for the tray pill. Used by config.c itself and by
 * consumers that only discover a mistake when they act on the value (e.g. the
 * theme asking for wallpaper colours when no wallpaper is set). */
void config_report_error(FwmConfig *cfg, const char *fmt, ...);

/* Runtime-settable options (see ConfigOption above). */

/* The table itself, for listing. */
const ConfigOption *config_options(int *count);

/* Look one up by name; NULL if unknown. */
const ConfigOption *config_option_find(const char *name);

/* Parse `value` and store it. Returns 1 on success; on failure returns 0 and
 * writes a human-readable reason into err. Out-of-range values are rejected
 * rather than clamped: a silent clamp over a socket is indistinguishable from
 * the value having been accepted. */
int config_option_set(FwmConfig *cfg, const ConfigOption *opt,
                      const char *value, char *err, size_t errcap);

/* Format the current value into out (never fails for a valid opt). */
void config_option_get(const FwmConfig *cfg, const ConfigOption *opt,
                       char *out, size_t cap);

/* Fold every matching rule's properties into `out`, in file order. Returns 0
 * if nothing matched. app_id/title may be NULL. */
int config_match_rules(const FwmConfig *cfg, const char *app_id, const char *title,
                       ConfigRule *out);

/* The bind whose key and modifiers match, or NULL. `mods` must equal the
 * bind's own mask exactly — a bind on super+q does not fire for super+shift+q.
 * The keysym is compared case-insensitively; see the implementation for why
 * that is load-bearing rather than lenient. */
const KeyBind *config_match_bind(const FwmConfig *cfg, xkb_keysym_t sym, unsigned int mods);

/* Whether holding the key down should keep firing the action. */
int config_action_is_repeatable(const char *action);

#define FWM_CONFIG_PATH "/.config/fwm/config.toml"

#endif /* FWM_CONFIG_H */
