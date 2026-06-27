#ifndef FWM_CONFIG_H
#define FWM_CONFIG_H

#include <X11/X.h>
#include <X11/keysym.h>

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

/* ── binds ───────────────────────────────────────────────────────────── */

/*
 * action string format:
 *   system:  "killclient", "toggle_tiling", "EXIT", ...
 *   spawn:   "spawn:kitty -o background_opacity=1.0"
 *   camera:  "move_camera:-50"
 *   view:    "view:3"
 */

typedef struct {
    unsigned int mod;
    KeySym       key;
    char         action[256];
} KeyBind;

/* ── top-level config ────────────────────────────────────────────────── */

typedef struct {
    PhysicsConfig physics;
    KeyBind      *keys;
    int           key_count;
} FwmConfig;

/* ── api ─────────────────────────────────────────────────────────────── */

void config_load(FwmConfig *cfg, const char *path);
void config_free(FwmConfig *cfg);

#define FWM_CONFIG_PATH "/.config/fwm/config.toml"

#endif /* FWM_CONFIG_H */