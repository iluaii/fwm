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

#ifndef FWM_EXPO_H
#define FWM_EXPO_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct FwmServer;
struct FwmView;
struct FwmOutput;

typedef struct FwmExpo FwmExpo;

/* The desktop strip: the camera pulls back until several desktops are on
 * screen at once, and windows can be found, moved between desktops and
 * switched to from there.
 *
 * It is cheap for one reason: fwm's ten desktops ALREADY are a continuous
 * horizontal strip in world coordinates (server->camera_x, and a window's x
 * runs across all ten). Pulling back is a scale on an axis that exists, not a
 * cube that has to be built. What is on screen while it runs is a set of
 * snapshots — the live scene is hidden underneath and the simulation is
 * frozen, so nothing walks away while you are looking at it. */

bool expo_active(struct FwmServer *server);

/* Enter, or start leaving. Leaving lands on whatever desktop the strip is
 * looking at. Bound to the `expo` action. */
void expo_toggle(struct FwmServer *server);

/* Second zoom step of the SAME mode (the `z` key while it is open): the whole
 * strip at once, and back. */
void expo_zoom_step(struct FwmServer *server);

/* Frame-time animation. Called from server_animate; a no-op when closed. */
void expo_tick(struct FwmServer *server, double dt);

/* Where the strip is looking, as a fractional desktop index (false when it is
 * closed), and that rounded to a desktop (-1 when closed). The tray's marker
 * reads these so it keeps reporting where you are while the strip pans, rather
 * than freezing on the desktop it was entered from. */
bool expo_view_position(struct FwmServer *server, double *pos);
int expo_view_desktop(struct FwmServer *server);
/* The same, but where the strip is HEADED rather than where it is: what a
 * next/prev step has to be measured from, or two steps in quick succession
 * both resolve to the same neighbour. -1 when the strip is closed. */
int expo_target_desktop(struct FwmServer *server);

/* Send the strip to a desktop, eased, without touching the live camera. False
 * when the strip is not up, so a caller can fall through to its own way of
 * getting there — this is what makes the `view:` binds, the tray's desktop
 * island and its scroll wheel keep meaning "go to desktop N" while the strip
 * is open, instead of moving a world nobody is looking at. */
bool expo_goto_desktop(struct FwmServer *server, int d);

/* True while the desktop at the front of the strip is being kept alive: its
 * clients are handed frame callbacks and its cards retaken. The frame loop has
 * to stay at full rate for that, which is the price of the front desktop not
 * being a photograph — and the reason this is asked rather than assumed. */
bool expo_live_active(struct FwmServer *server);

/* True while the strip is still moving, so the tick keeps the frame loop at
 * full rate instead of dropping to the idle heartbeat. */
bool expo_animating(struct FwmServer *server);

/* Input, while the strip owns it. Each returns true when the event was
 * consumed and must not reach a client. Coordinates are output-local pixels. */
bool expo_handle_key(struct FwmServer *server, xkb_keysym_t sym);
bool expo_handle_motion(struct FwmServer *server, double lx, double ly);
bool expo_handle_button(struct FwmServer *server, uint32_t button, bool pressed,
                        double lx, double ly);
bool expo_handle_axis(struct FwmServer *server, double delta);

/* Re-photograph one desktop's windows after something moved them while the
 * strip was up — a mode change from the strip's own menu, or from the tray.
 * Also settles the tiling glide, which is driven by a physics tick the strip
 * has frozen. No-op when the strip is closed. */
void expo_refresh_desktop(struct FwmServer *server, int d);

/* A window that is going away must not be left with a card standing in for it.
 * Called from the unmap and destroy paths. */
void expo_forget_view(struct FwmServer *server, struct FwmView *view);

/* Tear the strip down without animating, at shutdown or when the outputs
 * change under it. */
void expo_destroy(struct FwmServer *server);

/* A monitor is being freed. The strip holds a pointer to the one it opened on,
 * so it must go before that memory does. No-op for any other monitor. */
void expo_output_gone(struct FwmServer *server, struct FwmOutput *out);

/* The orrery: the ring of desktops turning by itself, seen from above, with a
 * star in the middle. Toggled by `o` inside the strip. */
void expo_orrery_toggle(struct FwmServer *server);

/* One step further down the road for the star at the centre of the ring:
 * burning to ember, ember to pulsar, pulsar to hole. What `star_collapse` does
 * while the strip is up. */
void expo_orrery_collapse(struct FwmServer *server);

/* Its size, multiplicatively, and the tip of its disc out of the ring's plane.
 * Both are driven from inside the strip: what looks right depends on the
 * screen and the camera, which no config file knows. */
void expo_orrery_resize(struct FwmServer *server, double factor);
/* Turn the disc's plane. Tilt changes how squashed it looks; this changes
 * which way round it lies, which is the one that reads as rotating it. */
void expo_orrery_roll(struct FwmServer *server, double delta);

/* Desktops on their own orbits, or back to one plain turning ring. */
void expo_orbits_toggle(struct FwmServer *server);

#endif /* FWM_EXPO_H */
