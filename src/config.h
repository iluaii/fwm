#ifndef FWM_CONFIG_H
#define FWM_CONFIG_H

#include <stddef.h>
#include <X11/X.h>
#include <X11/keysym.h>

#include "defines.h"

#define MOD_KEY Mod4Mask

typedef union {
    int i;
    double f;
    const char **v;
} Arg;

typedef struct {
    unsigned int mod;
    KeySym key;
    void (*func)(Fwm *wm, const Arg *arg);
    Arg arg;
} Key;

#define LENGTH(x) (sizeof(x) / sizeof(x[0]))

static void spawn(Fwm *wm, const Arg *arg);
static void killclient(Fwm *wm, const Arg *arg);
static void toggle_tiling(Fwm *wm, const Arg *arg);
static void view(Fwm *wm, const Arg *arg);
static void fake_fullscreen(Fwm *wm, const Arg *arg);
static void real_fullscreen(Fwm *wm, const Arg *arg);
static void move_camera(Fwm *wm, const Arg *arg);
static void pin_window(Fwm *wm, const Arg *arg);
static void toggle_nocollide(Fwm *wm, const Arg *arg);
static void calm_all(Fwm *wm, const Arg *arg);

static const char *termcmd[] = { "kitty", "-o", "background_opacity=1.0", NULL };
static const char *menucmd[] = { "rofi", "-show", "drun", "-normal-window", NULL };

static Key keys[] = {
    { MOD_KEY, XK_Return, spawn,           { .v = termcmd } },
    { MOD_KEY, XK_space,  spawn,           { .v = menucmd } },
    { MOD_KEY, XK_q,      killclient,      { 0 } },
    { MOD_KEY, XK_t,      toggle_tiling,   { 0 } },
    { MOD_KEY, XK_d,      fake_fullscreen, { 0 } },
    { MOD_KEY, XK_f,      real_fullscreen, { 0 } },
    { MOD_KEY, XK_h, move_camera, {.i = -50} },
    { MOD_KEY, XK_l, move_camera, {.i =  50} },
    { MOD_KEY,             XK_p, pin_window,      { 0 } },
    { MOD_KEY,             XK_n, toggle_nocollide, { 0 } },
    { MOD_KEY | ShiftMask, XK_c, calm_all,         { 0 } },
    { MOD_KEY, XK_1,      view,            { .i = 0 } },
    { MOD_KEY, XK_2,      view,            { .i = 1 } },
    { MOD_KEY, XK_3,      view,            { .i = 2 } },
    { MOD_KEY, XK_4,      view,            { .i = 3 } },
    { MOD_KEY, XK_5,      view,            { .i = 4 } },
    { MOD_KEY, XK_6,      view,            { .i = 5 } },
    { MOD_KEY, XK_7,      view,            { .i = 6 } },
    { MOD_KEY, XK_8,      view,            { .i = 7 } },
    { MOD_KEY, XK_9,      view,            { .i = 8 } },
    { MOD_KEY, XK_0,      view,            { .i = 9 } },
};

#endif /* FWM_CONFIG_H */
