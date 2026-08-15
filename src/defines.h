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

#ifndef FWM_DEFINES_H
#define FWM_DEFINES_H

/* Concurrent windows with a physics body. Slots are recycled when a window
 * closes, so this is a ceiling on what is on screen at once, not on what has
 * been opened over the session. Costs ~256 * (sizeof(PhysicsBody) +
 * sizeof(BodySlot)) of static memory, which is tens of kilobytes. */
#define MAX_WINDOWS             256
/* Virtual desktops on the strip. The world is this many screens wide. */
#define FWM_DESKTOPS            10
_Static_assert(FWM_DESKTOPS >= 4, "FWM_DESKTOPS must be at least 4");
#define DRAG_MARGIN             5
#define PHYSICS_MARGIN          3
#define MASS_DENSITY            0.0005
#define FRICTION                0.985
#define THROW_SPEED_MULTIPLIER  0.65
#define MAX_THROW_SPEED         1800.0
#define STOP_SPEED_THRESHOLD    1.0
#define RESTITUTION             0.3
#define PHYSICS_TICK_RATE       60.0
/* Approach speed (px/s) a collision must reach before it counts as an impact
 * worth reacting to. Above resting jitter, below a short drop. */
#define PHYSICS_HIT_MIN_SPEED   120.0
/* How long a window destroyed by a collision ([physics] hp) is left alone
 * after the close request goes out. Long enough that the several impacts of
 * one landing do not each send their own close, short enough that a client
 * which put up a save-your-work dialog instead of dying is not invulnerable
 * for the rest of the session. */
#define HP_CLOSE_GRACE_S        2.0
/* There used to be a DRAG_PUSH_MAX_SPEED here: a 600 px/s ceiling on the
 * VELOCITY handed to Box2D for a window being dragged, meant to stop a fast
 * shove launching its neighbour onto the next desktop. It did that, and it also
 * broke collision outright above 600 px/s of mouse movement — which is an
 * ordinary mouse movement.
 *
 * The reason is worth keeping. A dragged body is kinematic: its TRANSFORM is
 * teleported to wherever the cursor put it, 33px in one step at 2000 px/s, while
 * its velocity said 600. The solver believed the velocity, so it predicted a
 * contact that was already 30px deep, and pushed the other window out slower
 * than the dragged one advanced. Measured: the overlap grew monotonically to
 * 295px — three quarters of a 400px window — and the dragged window came out the
 * far side. Worse, the faster you dragged the LESS the other window was pushed
 * (591 px/s at a 600 px/s drag, 98 px/s at 12000), because the lie got bigger.
 *
 * Box2D now gets the true velocity, so nothing tunnels, and the launch it would
 * otherwise hand over is bounded on the OUTCOME instead: no dynamic body may
 * exceed [physics] max_throw_speed after a step (physics.c). That keeps the
 * property the old ceiling was really after — a shove can never outrun the
 * hardest deliberate throw — without lying to the solver to get it. */
/* How far anything the mouse is carrying may advance in one SOLVER step before
 * physics_step splits the tick into substeps (px, and how many are allowed).
 *
 * The honest velocity above bought collision back up to about 3000 px/s of hand
 * movement, and no further: a drag is a teleport with a velocity attached, so at
 * 6000 px/s the window arrives 100px inside its neighbour and the solver's first
 * sight of the contact is already that deep. Push speed is finite, the window
 * keeps arriving, and the overlap runs away — measured 269px through a 300px
 * window at 6000 px/s and above, which is a flick across one screen in a third
 * of a second and something any hand can do.
 *
 * Substepping is the fix the report asked for: the tick is cut into pieces small
 * enough that the drag enters a contact rather than materialising inside it, so
 * every step the solver sees is one it can still resolve. 32px is a quarter of
 * the narrowest window anyone runs and an eighth of an ordinary one, and the
 * ceiling of 16 bounds the cost of a hand nothing can keep up with (past ~30000
 * px/s the pieces grow again — see docs/physics.md). */
#define PHYSICS_MAX_STEP_ADVANCE 32.0
#define PHYSICS_MAX_SUBSTEPS     16
/* Ceiling (px/s) on how fast a visualiser bar may RISE, before [cava] push
 * scales it. A bar is kinematic — infinitely heavy — so whatever speed it has
 * on contact goes straight into the window, and a kick drum moves a bar most of
 * its travel inside one tick: 137px in 1/60s is 8200 px/s, which pinned a
 * window against the ceiling and held it there.
 *
 * A single kick at this speed only lifts v²/2g ≈ 30px, which looks far too
 * timid on paper — but a window spans dozens of bands that peak at different
 * moments, so it gets struck again on the way down and pumped like a ball on a
 * paddle. Real music through this reaches ~90px; the 400 that one kick's worth
 * of arithmetic suggested measured 232px and put the window off the top of the
 * screen. Tune against music, not against the formula.
 *
 * Only the upward direction is capped. A bar dropping away from a window never
 * touches it, so limiting the fall would just make the physical row lag the
 * drawn one for nothing. */
#define BAR_MAX_RISE_SPEED      250.0
#define GRAVITY  981.0   // px/s² (earth ~9.8 m/s² at 100 px/m)

/* How long the slide across the ring's join takes, and it is deliberately
 * quicker than a desktop switch (camera.anim_ms): the picture travels a whole
 * screen, but what it stands for is one step. */
#define WRAP_SLIDE_MS       220.0

/* The strip's key hints: how long they stay after it opens, how long they
 * linger once the cursor has left the band that calls them back, and how fast
 * they slide. Five seconds is long enough to read a row and short enough that
 * nobody has to look at it twice. */
#define EXPO_HINT_SHOW_S    5.0
#define EXPO_HINT_LINGER_S  0.8
#define EXPO_HINT_SLIDE     14.0   /* 1/s, exponential like everything else */

/* ── expo, the desktop strip ──────────────────────────────────────────── */
/* Resolution the strip's snapshots are taken at, as a fraction of the real
 * thing. Ten screen-sized cards at 1:1 is most of a hundred megabytes of
 * GPU memory for a picture that is never shown larger than a third of a
 * screen; half-resolution is still sharper than the smallest zoom step and
 * costs a quarter of that. */
#define EXPO_SNAP_SCALE     0.5
/* Desktops across the screen at each zoom step. The near step is deliberately
 * barely more than one: enough to see that the desktops either side exist and
 * to drop a window onto them, not so far that the desktop you were working on
 * becomes a thumbnail. The far step is the same idea one notch out — the whole
 * ten-desktop strip was tried and is unusable: nine of the cards are empty and
 * the tenth is too small to aim at. */
#define EXPO_ZOOM_NEAR      1.3
#define EXPO_ZOOM_FAR       3.0
/* How fast the zoom chases its target (1/s, exponential — the same
 * framerate-independent form as the free camera pan, which is what keeps a
 * retarget mid-flight from restarting an ease). */
#define EXPO_ZOOM_SPEED     11.0
/* Gap between desktop cards, in screen widths, reached at the near zoom step
 * and held beyond it. It closes as the strip zooms back in to 1.0, so the
 * identity view has no seams in it. */
#define EXPO_GAP_FRAC       0.04
/* The edge drawn around every card, in SCREEN px — not scaled with the strip,
 * because its job is to say where one desktop ends at any zoom. The desktop the
 * strip is looking at gets the accent colour, the rest a dim grey. */
#define EXPO_EDGE_PX        2
#define EXPO_EDGE_GREY      0.34f
/* Wallpaper layers a card will reproduce. The parallax rarely runs past two or
 * three; beyond this the extra layers are dropped from the card rather than
 * multiplied by ten desktops. */
#define EXPO_MAX_WP_LAYERS  4
/* A desktop with no wallpaper still has to read as a slot on the strip, and
 * the drum's end caps want to read as a lid rather than as another desktop. */
#define EXPO_CARD_GREY      0.13f
/* The inside of the drum's far wall: the UI's own dark tint, with a hint of the
 * desktop still printed on it. There is no floor and no lid — a disc across the
 * middle of the ring reads as a black plate wherever it is put, and without one
 * the ring is simply see-through, which is what a ring is. */
#define EXPO_INNER_ALPHA    0.88f
#define EXPO_INNER_IMAGE    0.22f
/* How far the camera pulls back as it rises, as a fraction of the tilt in
 * radians. Without it, lifting over the ring walks the near wall off the
 * bottom of the screen and fills the frame with floor. */
#define EXPO_TILT_PULLBACK  1.0
/* How fast a pan (arrows, wheel) chases where it was sent, 1/s. Quicker than
 * the zoom: a step sideways should feel like a nudge, not like a journey. */
#define EXPO_PAN_SPEED      14.0
/* ── perspective ──────────────────────────────────────────────────────
 * The strip is not flat once it opens: it lies on a cylinder whose whole
 * circumference is the ten desktops, so the card in front of you faces you and
 * its neighbours turn away. Both numbers below are in strip pitches (one
 * desktop plus its gap), which is the only length the strip has.
 *
 * The curvature is scaled by how far the strip has opened, so at the live view
 * it is exactly zero and entering does not bend anything: the flat mapping is
 * the k = 0 case of the same projection, not a separate code path. */
/* The seam: how far apart the two ends of the strip stand while it is a LINE,
 * in desktop pitches. The strip always lies on a circle — closing it is these
 * two ends coming together, not the whole thing bending further, which is both
 * what "join" means and the only version that still reads as a join when the
 * ring is looked at from an angle.
 *
 * It also has to be the SAME number the placement wraps by. Wrapping cards by
 * ten pitches while the circle was thirteen long is what teleported the end
 * card while the ring closed. */
#define EXPO_SEAM_PITCHES   3.0
/* How fast the seam closes, 1/s. Slower than the zoom on purpose: it is a
 * change of shape, and one that is over before it is seen has not been seen. */
#define EXPO_RING_SPEED     5.0
/* Dragging a window to the edge of the screen turns the strip under it, so a
 * window can be carried to a desktop that is not on screen — and, on a ring,
 * all the way round. Trigger band in screen px, and the speed in pitches per
 * second at the very edge. */
#define EXPO_DRAG_EDGE_PX   90
#define EXPO_DRAG_PAN_SPEED 1.6
/* Orbit: how far the camera may be lifted above the ring, and how far it may
 * be pulled back, as a multiple of the base distance. Roll is deliberately not
 * offered: it shows nothing and disorients immediately. */
#define EXPO_TILT_MAX       1.05   /* radians, ~60 degrees */
#define EXPO_TILT_STEP      0.12   /* one arrow press */
#define EXPO_DIST_MIN       0.6
#define EXPO_DIST_MAX       3.0
#define EXPO_DIST_STEP      1.18   /* one wheel notch, multiplicative */
/* How fast the camera eases to where it was sent, 1/s — and how fast it comes
 * BACK, which is quicker on purpose: the strip may only collapse into the live
 * screen from the canonical view, so the camera has to be home before the zoom
 * lands rather than at the same time as it. */
#define EXPO_ORBIT_SPEED    9.0
#define EXPO_ORBIT_HOME_SPEED 18.0
/* Live cards: how often the desktop being looked at is re-photographed, and
 * nothing else is. Everything on the strip is a still picture, which is what
 * makes ten desktops affordable — but the one in front of you being a still
 * picture is the difference between a window manager and a screenshot of one. */
#define EXPO_LIVE_S         (1.0 / 30.0)
/* Letting go of the ring mid-turn leaves it turning. `EXPO_SPIN_DECAY` is how
 * fast that dies away (1/s), and below `EXPO_SPIN_MIN` (strip px/s) it is over
 * — a flywheel in a compositor whose windows are rigid bodies is less a
 * flourish than a consistency. */
#define EXPO_SPIN_DECAY     2.4
#define EXPO_SPIN_MIN       40.0
/* Viewer distance from the front of the strip. Smaller is a wider lens: more
 * dramatic, and quicker to look wrong at the edges of the screen. */
#define EXPO_CAM_DIST       2.2
/* Columns a card and a window are tessellated into. The projection is exact
 * per column (only the vertical axis turns, so a column is at one depth), so
 * this only has to resolve the CURVE — a card spans 36 degrees, and 16 columns
 * put a joint every 2.25 of them. */
#define EXPO_CARD_COLS      16
#define EXPO_WIN_COLS       8

/* How far the live wallpaper behind the strip is dimmed — the strip keeps it
 * on screen rather than blacking the background out — and the frame drawn
 * around the window under the cursor (px at 1:1, so it scales with the strip). */
#define EXPO_BACKDROP_ALPHA 0.72
#define EXPO_HILIGHT_PX     6

#endif /* FWM_DEFINES_H */