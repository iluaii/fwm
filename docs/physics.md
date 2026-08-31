# Physics

Windows in fwm are rigid bodies in a real solver — [Box2D](https://box2d.org/)
v3 — not an animation that looks like one. This page is what that means in
practice: the units, what decides how a window behaves, and where the simulation
is deliberately bounded.

Contents: [the world](#the-world) · [units](#units) · [what a window is made
of](#what-a-window-is-made-of) · [mass](#mass) · [gravity](#gravity) ·
[throwing and dragging](#throwing-and-dragging) · [speed](#speed) ·
[rotation](#rotation) · [what physics does not own](#what-physics-does-not-own) ·
[soft windows](#soft-windows-and-why-there-are-none)

## The world

One strip, ten screens wide, with walls: a floor, a ceiling, and an end at each
extreme. Desktop *n* is the region from `n × screen_width` to
`(n+1) × screen_width`; a window's desktop is simply where its centre is. That is
why dragging a window past the screen edge moves it to the next desktop and why
throwing one can send it there.

Ten because that is what `FWM_DESKTOPS` says. The width of the world is not a
separate number: it is one screen per desktop (`PHYSICS_WORLD_W`), and the walls,
the wrap and the escape net all measure from it. They have to — a world wider
than the desktops has space belonging to none of them, and a narrower one leaves
desktops standing outside the wall where no window can arrive.

With `[camera] wrap = true` the strip closes into a ring and the two end walls
are removed. A window thrown off one end flies on and arrives at the other with
its velocity intact — only once it is *entirely* past, so it is never visible at
both ends at once.

Windows on a **tiling** or **floating** desktop are not simulated: the layout (or
you) owns their geometry, so physics treats them as immovable anchors. Switch the
desktop back to physics mode and they are objects again.

## Units

| Quantity | Unit | Note |
|---|---|---|
| Length | px | Box2D works in metres internally at **100 px per metre**, so a window is a 1–20 m box — the size range the solver is tuned for. |
| Speed | px/s | 1800 px/s, the default cap, is 18 m/s. |
| Acceleration | px/s² | `gravity = 981` is earth at that scale. |
| Mass | area × `mass_density` | Arbitrary units; only ratios matter. |
| Time | fixed 1/60 s steps | Four solver substeps each. The tick catches up with whole steps when a frame runs late, so the simulation never depends on the frame rate. |

## What a window is made of

Four properties, resolved fresh every step, in this order:

1. **`[physics]`** — the world's values.
2. **`[physics.<name>]`** — the profile assigned to the desktop the window is
   currently on. A window dragged from the moon desktop onto the water desktop
   changes as it crosses the edge, mid-flight.
3. **`[[rule]]`** — what matched this window when it opened. `mass` and `gravity`
   multiply the desktop's; `bounce` and `friction` replace it.
4. **The mass mode** — if `mass = "ram"`, what the application's memory footprint
   says (below).

Resolving per step rather than per window is what lets all four coexist: a heavy
window stays heavy when it is dragged onto the moon, and a config reload changes
what everything is made of without touching a body.

## Mass

`[physics] mass` picks what decides a window's weight.

**`"size"`** (default) — `mass = width × height × mass_density`. A big window is
heavy, a small one skitters.

**`"ram"`** — how much memory the application is using, with size ignored
entirely. The window's own process tree is summed (a browser's tabs count toward
its window), so the browser that has eaten two gigabytes becomes the heaviest
thing on the desktop and shoves everything else aside. `/proc` is walked once
every 1.5 s and never at all while the mode is off.

The arithmetic: a window using `mass_ram_ref` MB weighs what a 1280×720 window
weighed under `"size"`, scaled linearly by its actual footprint and clamped to
`mass_ram_max` times that either way. The clamp is not optional — an uncapped 6GB
browser beside a 20MB terminal is not a heavy window, it is a wall, and Box2D
answers a 300:1 mass ratio by pushing the light body through the floor.

Both modes are switchable live from the modes menu, which remembers the choice.

## Gravity

fwm starts in **zero-g** whatever the config says, and `cycle_gravity`
(`Super+G`) walks `gravity_steps` — by default zero-g, a lick of gravity (0.15),
and earth (1.0). The step is a multiplier on `[physics] gravity`, and 0 anywhere
is 0 everywhere: it is the master switch.

In zero-g a window keeps `friction` of its speed every tick and glides a long
way. Under gravity, damping is nearly switched off and windows are stopped by
contact friction instead — which is what real objects do, and why a window slides
along the floor and grinds to a halt rather than gliding as if on ice.

A `[physics.<name>]` profile can give one desktop its own gravity (the moon), and
a `[[rule]]` can give one window a multiple of it — `gravity = 0` for a window
that hangs in a room where everything falls, `-0.2` for a balloon.

## Throwing and dragging

**Throwing** is not a gesture fwm recognises; it is what happens when you let go
of a moving window. The drag samples the cursor over the last frames, the release
hands that velocity to the body times `throw_speed_multiplier`, and
`max_throw_speed` caps it.

**Dragging** makes the window kinematic — infinitely heavy, its position owned by
the mouse — so it shoves the windows it meets instead of being pushed back by
them. The velocity Box2D is told is derived from how far the cursor actually
moved it since the last step, which matters more than it sounds:

> A contact with a body that has no approach speed produces no hit event. Handing
> Box2D a standing-still velocity while teleporting the transform across the
> screen is why shoving one window into another once produced no squash and no
> sound, while a thrown window bouncing off a wall — dynamic and honestly moving —
> always did.

A window in your hand goes where you go. Dragging one into the screen edge scrolls
the strip under it, and the drag's anchor is carried along with the camera every
frame of that slide, so the window arrives on the new desktop still under the
cursor — including across the ring's join, which moves the camera nine screens at
once.

How fast the cursor is allowed to move it, and what happens when you fling it, is
[speed](#speed) and [substepping](#substepping).

**Spinning** is `spin_window` (`Super+R`) or the `twist` mouse verb; see
[rotation](#rotation).

## Speed

There is exactly **one speed limit**: `[physics] max_throw_speed`. No dynamic
window ever exceeds it, however it was set moving — thrown, shoved by a drag,
kicked by a visualiser bar, or launched by a stack collapsing on itself. Setting
it to 0 removes the limit.

One limit rather than several is a deliberate change. What it buys:

- A shove can never outrun the hardest deliberate throw, which is the property a
  drag-specific ceiling used to protect by *lying to the solver* about how fast
  the dragged window was moving — and thereby breaking collision outright above
  600 px/s of mouse movement.
- At the 1800 px/s default a window covers 30 px per step, an order of magnitude
  less than the smallest window, so window-through-window tunnelling is not
  reachable.
- Box2D's own hidden limit (400 m/s, i.e. 40 000 px/s) no longer silently clips a
  raised `max_throw_speed`: the engine's ceiling is kept above fwm's.

The one exception is the hand. While a window is being dragged the ceiling rises
to whatever the mouse is doing, for one reason: the window being shoved has to be
allowed to keep up with the window shoving it, or a hand moving faster than the
cap simply walks through because its victim physically cannot get out of the way.
Box2D's own ceiling rises with it. The moment you let go the ordinary limit
applies again, so nothing stays that fast.

It is invisible in ordinary use. A window falling the full height of a 1080px
screen under earth gravity arrives at about 1456 px/s, below the default cap, so
gravity, bounces and throws behave exactly as they always did.

### Measured limits

From `tests/test_physics_speed.c` and the diagnostics behind it:

| Situation | Result |
|---|---|
| Throw at 12 000 / 48 000 / 96 000 px/s into a wall, with the ceiling lifted | contained, every time, no NaN, no escape |
| Drag into a window at up to 3000 px/s | clean shove, no visible overlap |
| Drag through a row of three windows at up to 4000 px/s | all three pushed along |
| Flick into a window at 6000 – 20 000 px/s | clean shove; worst overlap 6 – 21 px |
| Flick at 40 000 px/s | still a shove, not a pass-through; worst overlap 132 px |
| A settled stack hit at the default cap | worst overlap 3 px, nothing leaves the screen |

### Substepping

The hand is the only thing in this world with no speed limit, and a drag is a
teleport with a velocity attached: at 6000 px/s the window lands 100 px inside its
neighbour and the solver's first sight of the contact is already too deep to undo.
That used to run away — 269 px of overlap through a 300 px window, with the dragged
window coming out the far side — for any flick faster than about 3000 px/s, which
is a third of a second across one screen.

So the tick is cut. When the mouse has moved a window more than
`PHYSICS_MAX_STEP_ADVANCE` (32 px) since the last tick, `physics_step` splits its
1/60 into as many solver steps as it takes to bring each one back under that,
capped at 16. The dragged body is no longer teleported to the cursor either: it
starts the tick where it was and is carried the rest of the way by its own
velocity, so Box2D's kinematic integration *is* the interpolation and each piece
of the tick moves it a piece of the way. Over a whole tick it lands in exactly the
same place, so nothing about an ordinary drag changes — and an ordinary drag asks
for one substep and pays nothing.

A jump of a whole monitor in one tick is not a hand — no arm covers 1920px in
16ms — so it is left a teleport and swept through nothing. That is what the ring's
join is, and send-to-desktop, and a session being restored: the window appears at
the far end rather than travelling there, hands out no momentum when it lands, and
is not a speed anything else is measured against. Without that line, crossing the
join mid-drag ploughed a furrow through the desktop you were leaving.

Past 16 substeps (about 30 000 px/s) the pieces grow again and so does the
overlap, gracefully. That is a hand no display can show you. The drag exemption on
the speed limit stops rising there too: past the point substepping can resolve a
shove, lifting the ceiling further only makes the mess faster — and it keeps a
window whose position is *teleported* mid-drag from being read as a 115 000 px/s
hand and becoming the speed limit for everything it sweeps past.

## Rotation

Off by default and experimental. `spin_window` hands a window's rotation to the
simulation: the collision box really turns, so it wedges into corners and shoves
its neighbours corner-first — it is not a picture spinning over an upright box.

The press itself is only a nudge (about a quarter turn a second, scaled by
`[effects] spin`). The spinning comes from what happens next: dragged, a spinning
window hangs from the point you took hold of, swings behind your hand, settles
hanging down under gravity, and keeps whatever spin it had when you let go.
Stirring the mouse winds it up. `twist` in `[mouse]` turns one by hand.

A window is only as free to turn as the room around it: one whose diagonal is
taller than the screen wedges against the floor and ceiling instead of coming
round.

## What physics does not own

- **Tiled and floating windows.** Immovable anchors; the layout owns them.
- **Fullscreen windows.** Likewise, while they are fullscreen.
- **Pinned windows** (`pin_window`, or `pin` in a rule).
- **The window being resized**, and one being turned by hand: frozen so the mouse
  owns them completely.
- **The desktop strip** (`expo`). While it is up the simulation is frozen —
  otherwise the windows in those still pictures would quietly have moved by the
  time you dropped one.
- **`no_collide` windows** pass through other windows but never through a wall:
  the play area is not optional.

## Soft windows, and why there are none

A window in fwm is a rigid box, and it is going to stay one. Windows do not sag
under their own weight, do not squash where another window lands on them, and do
not stretch as they spin. The drag wobble (`[effects] jelly`) is the whole of the
softness there is: a picture of the window bent through a spring lattice while
your hand is on it, and rigid again the moment you let go.

This is a deliberate stopping point rather than a gap waiting to be filled, so
here is what it would take, for anyone tempted.

The cheap half is deceptive. The lattice already exists (`src/wobble.c`), the
mesh already draws (`warp_blit` in `src/rotate.c`), and stepping a 9x9 sheet of
springs for every window costs a few hundred thousand floating-point operations
a frame — nothing. Gravity, contacts and volume preservation are each a few
dozen lines on top of that, and they work: an afternoon gets you windows that
flatten against the floor and bulge at the sides.

What it does not get you is a compositor. Every part of fwm outside the
simulation assumes a window is an upright rectangle — the focus border is a
scene rectangle and cannot bend, so a permanently soft window is a permanently
borderless one; the hit test is a box, and a corner hanging 60px from where the
box says it is has to be clickable; the client's texture is a fixed grid of
pixels, so a stretched window is a blurred one. None of those are hard problems
individually. Together they are a rewrite of the plumbing in service of an
effect.

And the effect is the part that cannot be engineered. Softness is judged
entirely by eye, in motion, on the screen it runs on: no measurement stands in
for it. Numbers say a landing dents the surface 68px and the sheet crosses its
rest shape five times; whether that reads as jelly or as a bug is a question
only looking at it answers, and every wrong answer costs another round of
building, restarting the compositor and looking again. That loop is what closed
this, not the arithmetic.

If it is ever reopened, reopen it with a way to see the result first.
