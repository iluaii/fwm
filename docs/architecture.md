# Architecture

For anyone changing the code. fwm is a single-threaded wlroots compositor in C11,
with three worker threads that never touch compositor state and one hard rule:
**nothing blocks the event loop**, because fwm *is* the display server and a
compositor that waits is a session that has stopped.

Contents: [the mirror](#the-mirror) · [the tick](#the-tick) ·
[threads](#threads) · [the file map](#the-file-map) ·
[UI overlays](#ui-overlays) · [config](#config) · [tests](#tests) ·
[conventions](#conventions)

## The mirror

The compositor never talks to Box2D. `PhysicsWorld` (src/physics.h) holds a plain
array of `PhysicsBody` — position, size, velocity, flags — and *that* is what the
rest of fwm reads and writes. `physics_step()` is the only bridge:

```
push the mirror into Box2D  →  b2World_Step  →  pull Box2D back into the mirror
```

Two things make this work:

- **Shadow fields.** Each body slot remembers the values last written back. If the
  mirror differs from its shadow, something outside changed it (a drag, a throw, a
  teleport) and it is forced into Box2D; otherwise Box2D's own evolution is left
  alone. This is what lets a drag write only a position and still produce correct
  contacts.
- **Slot pairing.** `world->bodies[i]` always corresponds to engine slot `i`. A
  closed window releases its Box2D body immediately so the slot is safe to hand
  to the next window.

Everything the simulation needs about one window — mass, bounce, friction, gravity
scale — is resolved *per step* in `material_for()` from three layers: the world's
`[physics]`, the desktop's profile, and the window's `[[rule]]`. That is why a
window dragged across a desktop edge changes as it crosses.

See [Physics](physics.md) for the model itself.

## The tick

`server_tick.c` is the heartbeat. Its shape matters:

- A **fixed 1/60 s** simulation step with an accumulator: as many whole steps as
  the elapsed time paid for, usually one, capped at two after a stall so a hitch
  cannot make the world jump.
- Everything **visual** — impact squash, camera shake, the drag pendulum, the
  render angle of a spinning window — advances at *frame* time in
  `server_animate()` (src/server_output.c), immediately before the scene is
  committed. Two unsynchronised 60Hz clocks beat against each other, and a fade
  that is perfectly even in the log arrives visibly uneven on screen.
- The loop **idles at 200 ms** when nothing is moving. Any UI animation therefore
  has to clamp its `dt` to one 60Hz frame, or the first tick after idle carries the
  whole 200 ms gap and finishes the animation in a single step.
- Per-tick syncs (`server_cava_sync`, `server_sound_sync`, `server_mass_sync`) are
  cheap and idempotent by design: each compares the config against what it last
  acted on, so a keybind, `fwmctl set`, the modes menu and a config reload all
  reach the same code without any of them knowing the others exist.

## Threads

Three, all of them for things that block. None touches compositor state; each
hands over data through a lock or an atomic.

| Thread | Why it exists |
|---|---|
| Video wallpaper decode (`src/video.c`) | libav decode of a clip, into a queue the compositor drains |
| Audio capture (`src/audio.c`) | `pw_context_new()` **deadlocks** with no sound server running — measured, still stuck after 90s. Everything PipeWire happens off the main thread, setup included |
| Sound mixer (`src/sound.c`) | `pa_simple_write` parks until the device has room |

Both audio threads are **never joined**. They are told to stop and ownership is
refcounted, so whichever of the two lets go last frees the handle. Joining cost
218–786 ms of frozen session every time `[cava]` was switched off, which is not a
price to pay for turning a feature *off*.

## The file map

| Area | Files |
|---|---|
| Simulation | `physics.c` (the mirror + Box2D), `wobble.c` (drag lattice) |
| Compositor core | `server.c`, `server_lifecycle.c`, `server_tick.c`, `server_output.c`, `server_shell.c`, `server_seat.c` |
| Input | `server_input.c` (keyboard, binds, modes), `server_pointer.c`, `server_drag.c`, `gestures.c` + `server_gestures.c` |
| Windows | `view.c`, `view_effects.c` (squash/wobble/fade), `snapshot.c`, `rotate.c` (the one GLES2 path), `group.c` (tabs) |
| Layout | `bsp.c` (the tree), `server_tiling.c` (applying it), `server_desktop.c` |
| Desktops | `expo*.c` (the strip and its 3D view) |
| Config | `config.c`, `config_binds.c`, `config_set.c`, `toml.c` |
| State & IPC | `session.c`, `ipc.c`, `ipc_events.c`, `server_config.c` (paths, reload, remembered choices) |
| Wallpaper | `wallpaper.c`, `video.c` |
| Audio | `audio.c` (capture), `cava.c` (FFT + bars), `sound.c` (mixer), `wav.c` |
| Light | `sun.c` (where the sun is — arithmetic, no scene), `shadow.c` (what it draws: one shared nine-patch image, nine slices per window) |
| Session | `server_idle.c` (the blank and lock timers), `lock.c` (the lock protocol), `clipboard.c` (the seat's selection: fwm's own offers, and text kept past the window that copied it), `battery.c` (the charge, and when it is worth interrupting somebody over) |
| Extras | `ram.c` (memory footprints), `theme.c` (palette), `layer.c`, `foreign.c` |
| UI | `ui/*.c` — see below, plus `ui/toast.c`: one line at the bottom of the screen, used by anything with something short to say (where a screenshot went, how much charge is left) |

`server.c` was split by area rather than by size; `server_internal.h` is the
contract between those pieces and is where a new one is declared.

## UI overlays

Everything fwm draws itself is a cairo surface in the scene graph, built through
`ui/cairo_overlay.c`: it owns the buffer, the node animation (fade and slide) and
the redraw plumbing. An overlay that also animates *inside* its buffer — a switch
knob sliding, a highlight travelling — runs its own tick, because a node transform
cannot express that (`ui/modes.c` is the example).

| File | Draws |
|---|---|
| `ui/tray.c` | the status strip, including the modes pill |
| `ui/modes.c` | the modes menu: icons, rows, switches, segmented controls |
| `ui/launcher.c` | the application launcher |
| `ui/hints.c` | the key hints, read from the live config |
| `ui/errors.c` | the config-problem panel |
| `ui/welcome.c` | first-run panel |
| `ui/expo_menu.c`, `ui/expo_hints.c` | inside the desktop strip |
| `ui/logo.c` | the mark |

Icons are cairo paths, not glyphs: a compositor cannot assume a font has them.

## Config

`config.c` parses TOML (vendored `toml.c`) into one `FwmConfig`, and carries one
promise: **it always returns a working config**. Every failure is recorded in
`cfg->errors[]` and defaults stand in; when `[binds]` yields nothing usable, the
built-in binds are installed so a typo cannot leave a compositor with no way to
open a terminal or exit.

Two hand-kept lists have to agree with the code:

- `action_is_known()` (config.c) vs. the dispatch switch in `server_actions.c` —
  when they drift, a perfectly good action is *reported as unknown* and dropped.
  `tests/test_config.c` binds every documented action to catch exactly that.
- `default_binds[]` (config_binds.c) vs. `config.toml.example`.

`config_set.c` is the runtime-settable table: `{name, type, offset, min, max,
help}` into `FwmConfig`, which is how `fwmctl get`/`set`/`config` work without a
line of code per option. Writes never touch the file. `config_option_check` is
the same parse storing nothing, which is what lets a request carrying several
settings be refused whole rather than half-applied.

Choices the UI makes (the picked wallpaper, the modes menu) live in
`~/.local/state/fwm/` and are applied *over* the config after every load — see
`server_config.c`. The user's file is never rewritten.

`~/.local/state/fwm/settings` is the same idea opened up to the socket: what
`fwmctl save` writes, applied over the config on every load. Three things hang
together there and are easy to break separately:

- The **baseline** — every option's value with the config file alone, captured
  between parsing it and applying anything over it. `save --all` and `unsave`
  are both questions about the difference, and after the overlay is on, nothing
  else can tell the two apart.
- The **order**: settings overlay first, modes file second, so the two switches
  the modes menu owns still answer to the menu.
- `server_settings_notify` is the ONE place the `setting` event comes from. It
  compares every option against what was last announced, at the end of
  `server_apply_config` and after every dispatched action — because an option
  changes through the socket, through a keybind and through the modes menu, and
  a per-site emit is two chances to add a fourth route and forget.

## Tests

```sh
cmake -S . -B build-test -DFWM_TESTS=ON && ctest --test-dir build-test
```

Only modules that need no compositor, no display and no sound card. Each suite
exists because its subject cannot be checked by hand in a running session.

| Suite | Covers |
|---|---|
| `test_bsp` | the tiling tree: splits, removal, leaf collection |
| `test_config` | parsing, every documented action, and the promise that a broken file still yields a usable config |
| `test_input` | bind matching, modes, mouse binds |
| `test_physics` | the ends of the world: throws, walls, the ring join, mass scaling |
| `test_physics_speed` | what has to keep working at speed — the drag that used to pass through windows, the world ceiling, walls under absurd throws |
| `test_ipc_events` | the event name/bit vocabulary |
| `test_wobble` | the drag lattice, which only runs while a button is held |
| `test_gestures` | what a swipe means, without a touchpad |
| `test_sun` | where the sun is at each hour, and what dusk does — half of it only happens at sunset |
| `test_ram` | `/proc` parsing and the process-tree sum |
| `test_wav` | the WAV reader, fed truncated and lying headers |

A compositor cannot be driven by a harness — there is no way to inject a keypress
or move the pointer — so anything pointer-driven is tested through a temporary
`FWM_TEST_*` hook and run under ASan instead. See
[Troubleshooting](troubleshooting.md#debug-switches).

## Conventions

- **Comments say why, not what.** The interesting ones record a measurement or a
  bug that was paid for; that is the house style, and `git log` continues it.
- **px, y-down, top-left origin** everywhere outside physics.c, which converts at
  its boundary (100 px per metre).
- **Failures degrade.** A missing sound server, an unreadable image, a broken
  config, a client that dies mid-drag: each has a defined fallback, and none of
  them may take the session down.
- **`-Wall -Wextra -fstack-protector-strong`** in every build, release included: a
  compositor cannot report its own memory corruption.
- Commits explain the reasoning at length. Read a few before writing one.
