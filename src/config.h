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
#include <stdbool.h>
#include <stddef.h>
#include <xkbcommon/xkbcommon.h>

#include "defines.h"

/* Modifiers */
#define FWM_MOD_SHIFT (1 << 0)
#define FWM_MOD_CTRL  (1 << 1)
#define FWM_MOD_ALT   (1 << 2)
#define FWM_MOD_LOGO  (1 << 3)

/* ── physics ─────────────────────────────────────────────────────────── */

/*
 * A named set of physics a desktop can be given:
 *
 *   [physics.moon]
 *   gravity  = 160.0
 *   desktops = [3, 4]
 *
 * Every field starts at the [physics] value and is overridden only by what the
 * profile writes, so a profile is a diff against the world, not a second copy
 * of it. A desktop no profile claims keeps the [physics] values exactly, which
 * is why an existing config behaves as it always did.
 */
#define CONFIG_MAX_PROFILES 10

typedef struct {
    char   name[32];
    double friction;
    double mass_density;
    double restitution;
    double gravity;
} PhysicsProfileConfig;

/* How many steps cycle_gravity walks through. */
#define CONFIG_MAX_GRAVITY_STEPS 8

/* What decides how heavy a window is.
 *
 *   [physics]
 *   mass = "size"   # area * mass_density — what it has always been
 *   mass = "ram"    # how much memory the application is using
 *
 * "ram" is the joke asked for in issue #5: Chrome, having eaten two gigabytes,
 * becomes the heaviest thing on the desktop and shoves everything else aside.
 * It replaces area rather than multiplying it — under "ram" a window's size
 * says nothing about its weight, which is the whole point of the choice. */
enum {
    PHYSICS_MASS_SIZE = 0,
    PHYSICS_MASS_RAM,
};

typedef struct {
    double friction;
    double mass_density;
    int    mass_mode;      /* PHYSICS_MASS_* */
    /* The memory footprint that weighs the same as a window did before any of
     * this existed, and the ceiling on how much heavier one can get. Without the
     * ceiling a 6GB browser next to a 20MB terminal is not a heavy window, it is
     * an immovable wall — and Box2D solves a 300:1 mass ratio by shoving the
     * light body through the floor. */
    double mass_ram_ref;   /* MB that counts as normal weight */
    double mass_ram_max;   /* clamp, in multiples of normal */

    /* Windows can be destroyed by a hard enough collision (issue #9). Off by
     * default and switched from the modes menu, because it is the one physics
     * setting that can lose unsaved work.
     *
     * Damage is not accumulated: a window dies only if a SINGLE hit is worth
     * more than all of its hit points. There is no wearing something down, so
     * a stack that settles overnight is not a slow execution.
     *
     *   damage(A->B) = mass_A * (v / hp_break_speed)^2 * hardness_A
     *   hp(B)        = mass_B * toughness_B
     *
     * Both sides of a collision are worked out the same way, so two equal
     * windows meeting fast enough destroy each other.
     *
     * Squared because breaking things is about energy, and because it is the
     * shape that keeps this survivable: at half the break speed a hit is worth
     * a quarter, so nudges are safe and only real slams are lethal.
     *
     * hp_break_speed reads as "how fast a window must hit another of its own
     * mass to destroy it". Left at max_throw_speed it gives the system a
     * property worth having: since nothing may travel faster than that clamp
     * (see physics.c), damage can never exceed the attacker's own mass, so a
     * window is physically incapable of breaking anything heavier than itself.
     * Set it lower and a fast light window can punch above its weight. */

    /* SESSION STATE, not a setting. There is no [physics] hp key and nothing
     * writes it to ~/.local/state/fwm/modes: every session starts with windows
     * unbreakable, and the only way to turn it on is the modes menu, this
     * session, on purpose.
     *
     * That is the whole design. A mode which destroys unsaved work must never
     * be something you are living under because of a choice you made last week
     * and have forgotten, and a config file you wrote once is exactly that. */
    int    hp;               /* 0 = windows are indestructible */
    double hp_break_speed;   /* px/s; <= 0 means "use max_throw_speed" */

    double throw_speed_multiplier;
    double max_throw_speed;
    double stop_speed_threshold;
    double restitution;
    double gravity;
    double tick_rate;

    /* A layer-shell bar that reserved space with an exclusive zone is a solid
     * object: the floor stands on top of a bar at the bottom of the screen and
     * the ceiling hangs under one at the top, so a thrown window lands on the
     * bar instead of sliding away underneath it.
     *
     * Off by default, because it changes how the world feels rather than how it
     * looks, and because without a bar running there is nothing to notice.
     *
     * Only EXCLUSIVE ZONES count — fwm's own tray is not one. The tray floats
     * over the windows and always has; a window resting under it is visibly
     * under something translucent, while a window buried under an opaque bar
     * simply looks lost, which is the difference this setting is about. */
    int    solid_bars;

    PhysicsProfileConfig profiles[CONFIG_MAX_PROFILES];
    int    profile_count;
    /* Which profile each desktop was assigned, or -1 for the [physics] values.
     * Resolved at load: a desktop claimed by two profiles goes to the last one,
     * matching how a later [[rule]] wins over an earlier one. */
    int    desktop_profile[FWM_DESKTOPS];

    /* The gravity multipliers cycle_gravity walks, in order. Defaults to the
     * built-in zero-g / space / earth trio. */
    double gravity_steps[CONFIG_MAX_GRAVITY_STEPS];
    int    gravity_step_count;
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
    /* The strip is a ring: stepping right off the last desktop arrives on the
     * first, and a window sent that way goes with it.
     *
     * Only STEPS wrap — next/prev, the wheel, a gesture. A free pan still
     * stops at the ends, and the seam is never drawn: a wrapping step jumps
     * the camera outright instead of sliding it back across nine desktops,
     * which is both what the eye expects and the only cheap answer (the
     * desktops are one continuous strip in world coordinates, so there is no
     * such thing as a picture of the join). */
    int wrap;
} CameraConfig;

/* ── decorations ─────────────────────────────────────────────────────── */

typedef struct {
    int   border_width;      /* px; 0 disables borders */
    float col_active[4];     /* RGBA 0..1, focused window */
    float col_inactive[4];   /* RGBA 0..1, unfocused windows */
    double fade_in_ms;       /* window fade-in duration; <= 0 disables */
    double wallpaper_fade_ms;/* wallpaper cross-fade duration; <= 0 = instant cut */
    double tray_opacity;     /* island fill alpha 0..1 for the tray bar */
    /* Stand the status strip down on a monitor where a layer-shell panel has
     * reserved space along the top — an external bar replacing it rather than
     * stacking with it.
     *
     * OFF by default, and deliberately so: fwm's chrome is part of what fwm is,
     * and a client should not be able to take it off the screen by asking. The
     * user who runs a bar says so here, once. */
    int    tray_yield;
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

/*
 * Keyboard, and the pointer/touchpad knobs libinput owns.
 *
 * The touchpad half is all TRI-STATE: -1 means "say nothing about it", and the
 * device keeps whatever libinput itself decided. That matters because these
 * settings are per-device capabilities — a mouse has no tap and no
 * disable-while-typing — and because libinput's defaults are chosen per model,
 * which is a better answer than a number in a config file.
 *
 * `tap` is the one exception: it defaults to ON. libinput ships tap-to-click
 * disabled, and a laptop where touching the pad does not click is not a laptop
 * anyone can work on. Set tap = false for libinput's own default back.
 */
typedef struct {
    char kbd_layout[64];   /* xkb layout list, e.g. "us,ru"; "" = environment */
    char kbd_variant[64];  /* xkb variant list, may be empty */
    char kbd_options[128]; /* xkb options, e.g. "grp:alt_shift_toggle" */
    int  repeat_rate;      /* key repeat, chars/s */
    int  repeat_delay;     /* ms before repeat starts */

    /* Touchpad / pointer. -1 = leave libinput's default alone. */
    int  tap;              /* tap-to-click (default 1, see above) */
    int  tap_drag;         /* tap-and-drag: tap then slide moves */
    int  drag_lock;        /* a dragged item stays held between slides */
    int  natural_scroll;   /* content follows the fingers */
    int  dwt;              /* disable the pad while typing */
    int  middle_emulation; /* left+right together = middle click */
    int  left_handed;      /* swap the buttons */
    double accel_speed;    /* -1..1; 2.0 = untouched (out of libinput's range) */
    char accel_profile[16];/* "adaptive" | "flat"; "" = untouched */
    char scroll_method[16];/* "two_finger" | "edge" | "button" | "none"; "" = untouched */
    char click_method[16]; /* "button_areas" | "clickfinger" | "none"; "" = untouched */
} InputConfig;

/* accel_speed value meaning "the file said nothing"; libinput's range is
 * -1..1, so anything outside it can carry that. */
#define INPUT_ACCEL_UNSET 2.0

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
    /* Wobble while a window is dragged: it lags behind the cursor and stretches
     * along the way it is being pulled. 0 disables, 1 = default. Never applies
     * to a spinning window. */
    double jelly;
    /* Free window rotation (EXPERIMENTAL), strength of the spin_window kick.
     * 0 makes the bind do nothing. Nothing rotates on its own: a window only
     * ever spins because the bind told it to. */
    double spin;
    /* Live content behind the spin and the wobble: a window that is a single
     * surface is drawn from the client's own texture and keeps drawing while
     * the effect hides it. 0 puts every window back on the periodic snapshot —
     * a still frame that costs the machine nothing, which is the better trade
     * on hardware where the effect judders under a slow hand. */
    double live;
    /* The region screenshot peeling off the screen and flying away toward the
     * line that says it was copied. Strength doubles as duration: 0 disables,
     * 1 = default, 2 makes it twice as slow and twice as far. Only the region
     * shot has it — a whole screen taking off is a lot of motion for something
     * you may be doing every minute. */
    double shot_fly;
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

/* ── modes (submaps) ─────────────────────────────────────────────────── */

/*
 * A second keymap you step into, so single keys can mean something:
 *
 *   [mode.physics]
 *   enter  = "super+o"
 *   "g"    = "cycle_gravity"
 *   "c"    = "calm_all"
 *   "r"    = "spin_all"
 *
 * While a mode is active it owns the keyboard outright: its own binds fire,
 * Escape leaves, and every other key does nothing rather than reaching the
 * application underneath. That is the whole point of a mode — a bare "g" is
 * only safe to bind if nothing else can receive it — and it is why leaving is
 * unconditional and always available.
 *
 * By default a mode is one-shot: the first action fires and drops back to the
 * root map, because that is what a leader key is usually for. `sticky = true`
 * keeps it open until Escape, for a mode meant to be held (nudging a window's
 * size a dozen times).
 *
 * `enter` is a convenience: it registers "mode:<name>" in the root map for
 * you. Binding "mode:<name>" by hand in [binds] does the same thing, and
 * "mode:default" from anywhere returns to the root map.
 */
#define CONFIG_MAX_MODES 8

/* Action prefix that switches modes, and the name that means "back to root". */
#define FWM_MODE_ACTION  "mode:"
#define FWM_MODE_DEFAULT "default"

typedef struct {
    char      name[32];
    int       sticky;     /* stay in the mode until Escape */
    KeyBind  *keys;
    int       key_count;
} ConfigMode;

/* ── mouse binds ─────────────────────────────────────────────────────── */

/*
 * What a drag with a modifier held does:
 *
 *   [mouse]
 *   "super+left"       = "move"
 *   "super+shift+left" = "move_nocollide"
 *   "super+right"      = "resize"
 *   "super+ctrl+left"  = "twist"
 *
 * These used to be four lines of C in the button handler, which is a strange
 * thing to be unable to change when every key on the keyboard is yours to
 * rebind. The verbs below are the ones only a drag can express; anything else
 * in the value is an ordinary [binds] action, fired once on the press.
 *
 * Modifiers are matched EXACTLY, like keybinds: super+left does not fire while
 * shift is also down. A bind with no modifier at all is honoured, and will eat
 * that button from every client — yours to do, but do it knowingly.
 */

/* Buttons, kept as an enum rather than linux/input-event-codes.h values so
 * config.c stays free of kernel headers (and testable off a compositor).
 * server_pointer.c maps them to BTN_*. */
enum {
    FWM_BTN_LEFT = 0,
    FWM_BTN_RIGHT,
    FWM_BTN_MIDDLE,
    FWM_BTN_SIDE,
    FWM_BTN_EXTRA,
    FWM_BTN_COUNT,
};

/* The drag verbs. Everything else is dispatched as a normal action.
 *
 * What a verb means depends on the desktop's mode, exactly as the hard-coded
 * behaviour it replaces did: on a tiling desktop `resize` drags the BSP border
 * under the cursor and `move_nocollide` swaps two tiles, because the layout
 * owns tile geometry and there is nothing else those gestures could mean
 * there. */
#define FWM_MOUSE_MOVE           "move"
#define FWM_MOUSE_MOVE_NOCOLLIDE "move_nocollide"
#define FWM_MOUSE_RESIZE         "resize"
#define FWM_MOUSE_SWAP           "swap"
#define FWM_MOUSE_TWIST          "twist"

#define CONFIG_MAX_MOUSE 16

typedef struct {
    unsigned int mod;    /* FWM_MOD_* masks */
    int          button; /* FWM_BTN_* */
    char         action[256];
} MouseBind;

typedef struct {
    MouseBind binds[CONFIG_MAX_MOUSE];
    int       bind_count;
} MouseConfig;

/* ── touchpad gestures ───────────────────────────────────────────────── */

/*
 * Gestures are binds like any other, keyed on how many fingers moved which
 * way instead of on a keysym:
 *
 *   [gestures]
 *   "swipe3+left"  = "pan_desktop"
 *   "swipe3+up"    = "launcher"
 *   "pinch2+in"    = "calm_all"
 *
 * The action vocabulary is the same one [binds] uses, plus `pan_desktop`,
 * which only makes sense as a gesture: it hands the camera to the fingers and
 * pans across the desktop strip live, settling on a desktop when they lift.
 * Bind it to both horizontal directions — it is one gesture, not two.
 *
 * A finger count with nothing bound to it is left alone entirely, so a client
 * that understands gestures itself (pinch-to-zoom in a browser or an image
 * viewer) still gets them. See gestures.h for the state machine.
 */

enum {
    GESTURE_SWIPE_LEFT = 0,
    GESTURE_SWIPE_RIGHT,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN,
    GESTURE_PINCH_IN,
    GESTURE_PINCH_OUT,
};

/* The one action that exists only as a gesture (see above). */
#define GESTURE_ACTION_PAN "pan_desktop"

#define CONFIG_MAX_GESTURES 32

typedef struct {
    int  fingers;      /* 2..5, as libinput reports them */
    int  dir;          /* GESTURE_SWIPE_* / GESTURE_PINCH_* */
    char action[256];
} GestureBind;

typedef struct {
    double      sensitivity; /* px of camera travel per px of finger travel */
    int         natural;     /* 1 = the desktop strip follows the fingers */
    GestureBind binds[CONFIG_MAX_GESTURES];
    int         bind_count;
} GesturesConfig;

/* ── audio visualiser ────────────────────────────────────────────────── */

/*
 * A row of spectrum bars along the bottom of the screen, fed by loopback
 * capture of whatever is playing (see src/audio.h, src/cava.c).
 *
 *   [cava]
 *   mode = "both"    # off | visual | physical | both
 *   bars = 48
 *   height = 160
 *
 * The two halves of `mode` are genuinely independent, which is why this is one
 * key with four values rather than two booleans nobody would think to combine:
 *
 *   "visual"    draw the bars, do not touch the windows
 *   "physical"  the bars are invisible but REAL — a row of kinematic bodies
 *               along the floor that windows stand on and get tossed by
 *   "both"      the default, and the point of the feature
 *   "off"       nothing runs, no audio stream is opened at all
 *
 * `push` scales the physical bar height against the drawn one, so the dance can
 * be tuned down (or up past what is drawn) without changing the look.
 */
enum {
    CAVA_MODE_OFF      = 0,
    CAVA_MODE_VISUAL   = 1,       /* bit 0: drawn */
    CAVA_MODE_PHYSICAL = 2,       /* bit 1: collides */
    CAVA_MODE_BOTH     = 3,
};

/* Bars are one kinematic body each while physical, so this is a real cost, not
 * just an array bound. 128 across a 4K screen is a 30px bar. */
#define CONFIG_MAX_BARS 128

typedef struct {
    int    mode;        /* CAVA_MODE_* */
    int    bars;        /* how many bands, 2..CONFIG_MAX_BARS */
    double height;      /* px the loudest band reaches */
    double gap;         /* px between bars */
    double sensitivity; /* multiplies the normalised magnitude */
    double smoothing;   /* 0..1 fall inertia; 0 = bars snap straight down */
    double push;        /* physical bar height as a fraction of `height` */
    double opacity;     /* 0..1 on the drawn bars */
    double min_hz, max_hz; /* band edges of the log-spaced spectrum */
} CavaConfig;

/* ── collision sound ─────────────────────────────────────────────────── */

/*
 * Windows make a noise when they hit something (issue #6):
 *
 *   [sound]
 *   collisions = true
 *   path       = "~/sounds/knock.wav"   # empty = the built-in click
 *   volume     = 0.6
 *   min_speed  = 200.0                  # px/s below which a hit is silent
 *   max_speed  = 2000.0                 # ... and at which it is at full volume
 *
 * Off by default, like [cava] and for the same reason: opening a playback stream
 * is not something a compositor should do because it was installed. Also
 * switchable from the modes menu, which remembers the choice.
 *
 * `path` is a WAV file (16-bit PCM or 32-bit float, mono or stereo). Anything
 * else — a missing file, an mp3, a header we cannot read — falls back to the
 * built-in click and reports the problem, because a config typo must not leave
 * the feature silently doing nothing.
 */
typedef struct {
    int    collisions;  /* the feature's on/off, the only thing the menu flips */
    char   path[512];   /* WAV to play; "" = the built-in click */
    double volume;      /* 0..1 master gain */
    double min_speed;   /* px/s: quieter than this is not worth hearing */
    double max_speed;   /* px/s: full volume at and above this */
} SoundConfig;

/* ── stats pill ──────────────────────────────────────────────────────── */

/*
 * What the machine is doing, in the tray:
 *
 *   [stats]
 *   items    = ["cpu", "ram", "gpu", "vol"]
 *   interval = 2.0
 *   vol = "pactl get-sink-volume @DEFAULT_SINK@ | grep -o '[0-9]*%' | head -1"
 *
 * `items` is both the selection and the order. Three names are built in — cpu,
 * ram, gpu — and ANY OTHER KEY in the table defines a sensor of that name whose
 * value is what its command prints on the first line.
 *
 * One line per sensor is the whole point of the shape. The alternative,
 * [[stats]] with name/label/exec/interval fields, is what the rest of this file
 * does for wallpapers and outputs — but those have four things to say about
 * each entry and this has one, and four lines of ceremony to put the volume in
 * the tray is how a feature ends up unused.
 *
 * A name in `items` that is neither built in nor defined is a config error, not
 * a silent omission: a typo in a sensor name is otherwise indistinguishable
 * from a sensor that has nothing to report yet.
 */
#define STATS_MAX_ITEMS   12
#define STATS_NAME_MAX    16
#define STATS_CMD_MAX    512

typedef struct {
    char name[STATS_NAME_MAX];
    char cmd[STATS_CMD_MAX];
} StatsCustom;

typedef struct {
    /* Names in draw order, as written. Held as text rather than resolved here
     * because config.c cannot know which sensors the machine can actually
     * answer — that is stats.c's business, and it needs the name to say so. */
    char        items[STATS_MAX_ITEMS][STATS_NAME_MAX];
    int         item_count;
    double      interval;   /* seconds between runs of the custom commands */
    StatsCustom custom[STATS_MAX_ITEMS];
    int         custom_count;
} StatsConfig;

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
    WALLPAPER_FIT_VIDEO   = 3, /* looping video, scaled to cover the screen */
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
    double fps;    /* "video" only: cap presentation fps; <= 0 = source rate
                    * (clamped to 30). Lower it to cut CPU on weak hardware. */
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

/* Rarely more than three; the cap only stops a typo'd file from allocating. */
#define CONFIG_MAX_OUTPUTS 8

/*
 * Where a monitor sits, how it is driven, and what it starts on.
 *
 *   [[output]]
 *   name      = "HDMI-A-1"     # as fwm logs it at startup
 *   mode      = "1920x1080@60" # resolution, refresh optional
 *   scale     = 1.5            # HiDPI factor
 *   transform = "90"           # rotation / flip
 *   x         = 1366           # top-left in layout coordinates; both or neither
 *   y         = 0
 *   desktop   = 3              # the desktop it comes up on
 *   enabled   = false          # leave it dark
 *
 * Without an entry a monitor comes up in its preferred mode, is placed to the
 * right of the ones already there and takes the lowest free desktop, which is
 * what fwm did before this existed.
 */
typedef struct {
    char name[64];
    int  have_pos;   /* x/y were given */
    int  x, y;
    int  desktop;    /* -1: take whatever is free */
    int  enabled;    /* 0 only when the file says so */

    /* Mode, scale and rotation. Each has an "unset" value rather than a
     * default, because "leave the monitor as the backend brought it up" is a
     * different thing from any value we could pick: an entry that only moves a
     * screen must not also force a resolution on it. */
    int    have_mode;
    int    mode_w, mode_h;
    int    mode_refresh;   /* mHz; 0 = any refresh at that size */
    double scale;          /* 0 = leave alone */
    int    transform;      /* -1 = leave alone; else a wl_output_transform */
} ConfigOutput;

/* "1920x1080" or "1920x1080@59.94", the wlr-randr spelling. The refresh is
 * given in Hz and comes back in mHz, or 0 when the string named none. False on
 * anything malformed, and the out-params are then left untouched.
 *
 * Lives here rather than beside the compositor because both the config file
 * and `fwmctl output` take the same spelling, and two parsers would drift. */
bool config_parse_mode(const char *s, int *w, int *h, int *refresh_mhz);

/* "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180",
 * "flipped-270" — the wlr-randr vocabulary. Returns the matching
 * wl_output_transform value (0..7), or -1 if the name is not one of them. */
int config_parse_transform(const char *s);

/* The spelling config_parse_transform would accept for this value, for error
 * messages and for `fwmctl outputs`. "?" if it is not a valid transform. */
const char *config_transform_name(int transform);

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

    /* What the window is made of. NAN is the "says nothing" state here, because
     * 0 is a meaningful value for every one of them: a weightless window, one
     * that does not bounce, one that slides forever. Applied to the physics
     * body, which resolves them against the desktop's profile every step — so
     * a heavy window stays heavy when it is dragged onto the moon desktop.
     *
     *   mass     multiplies the desktop's mass_density. 8 = a window that
     *            shrugs off anything shoving it, 0.1 = one that skitters.
     *   gravity  multiplies the desktop's gravity. 0 = weightless in a room
     *            where everything else falls, -0.2 = a balloon.
     *   bounce   restitution, absolute 0..1.
     *   friction per-tick velocity retention, absolute 0..1, like
     *            [physics] friction.
     *
     * The last two only mean anything while [physics] hp is on:
     *
     *   toughness multiplies the window's hit points, which are otherwise
     *             simply its mass. 0 = made of glass, 10 = a safe.
     *   hardness  multiplies the damage this window DEALS. A window can be
     *             tough without being dangerous, and the two are separate so
     *             a heavy soft thing (a browser full of tabs) does not have to
     *             be a hammer. */
    double mass;
    double gravity;
    double bounce;
    double friction;
    double toughness;
    double hardness;
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
    MouseConfig     mouse;
    GesturesConfig  gestures;
    CavaConfig      cava;
    SoundConfig     sound;
    StatsConfig     stats;
    KeyBind        *keys;
    int             key_count;
    ConfigMode      modes[CONFIG_MAX_MODES];
    int             mode_count;
    WallpaperLayer *wallpapers;
    int             wallpaper_count;
    ConfigRule     *rules;
    int             rule_count;
    ConfigOutput    outputs[CONFIG_MAX_OUTPUTS];
    int             output_count;
    /* [wallpaper_picker] dir — where the built-in picker looks for images.
     * "~" is expanded at load. */
    char            wallpaper_dir[512];
    double          wallpaper_picker_fps; /* base fps cap for videos set via the
                                           * picker; 0 = the clip's own rate */

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

/* The [[output]] entry for a monitor of this name, or NULL. */
const ConfigOutput *config_find_output(const FwmConfig *cfg, const char *name);

/* The bind whose key and modifiers match, or NULL. `mods` must equal the
 * bind's own mask exactly — a bind on super+q does not fire for super+shift+q.
 * The keysym is compared case-insensitively; see the implementation for why
 * that is load-bearing rather than lenient. */
const KeyBind *config_match_bind(const FwmConfig *cfg, xkb_keysym_t sym, unsigned int mods);

/* A mode by name, or -1. The name may be the whole action string ("mode:foo")
 * or just the name — callers have one or the other and neither should have to
 * do string surgery to ask. */
int config_mode_find(const FwmConfig *cfg, const char *name);

/* The bind for this key inside `mode` (an index from config_mode_find), or
 * NULL. Same exact-modifier rule as config_match_bind. */
const KeyBind *config_match_mode_bind(const FwmConfig *cfg, int mode,
                                      xkb_keysym_t sym, unsigned int mods);

/* The mouse bind for this button and modifier set, or NULL. Same exact-match
 * rule as config_match_bind. */
const MouseBind *config_match_mouse(const FwmConfig *cfg, int button, unsigned int mods);

/* Whether an action string is one of the drag verbs (FWM_MOUSE_*), i.e. only
 * meaningful as a mouse bind. */
int config_action_is_drag(const char *action);

/* Whether holding the key down should keep firing the action. */
int config_action_is_repeatable(const char *action);

#define FWM_CONFIG_PATH "/.config/fwm/config.toml"

#endif /* FWM_CONFIG_H */
