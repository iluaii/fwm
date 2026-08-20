# Keybindings and actions

Nothing here is hard-coded. Every entry in the default table below is an ordinary
`[binds]` line you can change, and every drag verb is an ordinary `[mouse]` line.
`Super+Shift+?` shows the live list on screen.

Contents: [defaults](#default-bindings) · [actions](#actions) ·
[modes](#modes-submaps) · [the radial menu](#the-radial-menu) ·
[the sound panel](#the-sound-panel) ·
[mouse](#mouse-drags) · [gestures](#gestures) ·
[the desktop strip](#keys-inside-the-desktop-strip) ·
[the launcher](#keys-inside-the-launcher)

## Default bindings

These are also the built-ins fwm falls back on when `[binds]` is missing or
unusable, so they work on a machine with no config at all.

### Windows

| Bind | Action | Does |
|---|---|---|
| `Super+Return` | `terminal` | `$TERMINAL`, else the first emulator installed |
| `Super+Space` | `launcher` | the application launcher |
| `Super+Q` | `killclient` | close the focused window |
| `Super+D` | `fake_fullscreen` | fill the work area, keep the tray and the gaps |
| `Super+F` | `real_fullscreen` | true fullscreen, tray hidden |
| `Super+P` | `pin_window` | freeze it: physics stops moving it |
| `Super+N` | `toggle_nocollide` | other windows pass through it |
| `Super+R` | `spin_window` | hand its rotation to the simulation (experimental) |
| `Super+W` | `group_toggle` | tab this window together with the focused group |
| `Super+Tab` / `Super+Shift+Tab` | `group_next` / `group_prev` | walk the tabs |
| `Super+Shift+W` | `group_add` | add the focused window to the group |

### Layout

| Bind | Action | Does |
|---|---|---|
| `Super+T` | `toggle_tiling` | BSP tiling on this desktop, on/off |
| `Super+Alt+Space` | `toggle_floating` | floating mode on this desktop |
| `Super+S` | `toggle_split` | flip the next BSP split direction |
| `Super+←→↑↓` | `tile_focus:l|r|u|d` | move focus between tiles |
| `Super+Shift+←→↑↓` | `tile_move:l|r|u|d` | move the window within the layout |

### Desktops and camera

| Bind | Action | Does |
|---|---|---|
| `Super+1`…`0` | `view:0`…`view:9` | jump to a desktop |
| `Super+Shift+1`…`0` | `move_to:0`…`move_to:9` | send the focused window there |
| `Super+Ctrl+←` / `Super+Ctrl+→` | `view:prev` / `view:next` | one desktop over; wraps round the ring at either end |
| `Super+H` / `Super+L` | `move_camera:-50` / `move_camera:50` | pan the camera (hold to repeat) |
| `Super+A` | `expo` | the desktop strip |

### Everything else

| Bind | Action | Does |
|---|---|---|
| `Super+G` | `cycle_gravity` | walk `gravity_steps` |
| `Super+Shift+C` | `calm_all` | stop every window dead |
| `Super+J` | `toggle_tray` | hide/show the status strip |
| `Super+Shift+P` | `wallpaper_picker` | pick a wallpaper |
| `Print` | `screenshot` | copy the monitor under the pointer |
| `Super+Shift+S` | `screenshot_region` | drag a rectangle out and copy that |
| `Super+Shift+R` | `reload_config` | reload the config file |
| `Super+Shift+?` | `show_hints` | the key hints overlay |
| `Super+Shift+Escape` | `EXIT` | leave the session |

## Actions

Any of these can be bound in `[binds]`, in a `[mode.*]`, in `[mouse]`, in
`[gestures]`, or run from a script with `fwmctl dispatch <action>`. An unknown
action is reported when the config loads rather than doing nothing when pressed.

### Windows

| Action | Does |
|---|---|
| `killclient` | close the focused window |
| `pin_window` | toggle pinned: physics never moves it |
| `toggle_nocollide` | toggle pass-through for this window |
| `toggle_nocollide_all` | ...for every window at once |
| `spin_window` | give the focused window a spin (experimental) |
| `spin_all` | ...every window |
| `calm_all` | zero every velocity |
| `fake_fullscreen` | fill the work area, keep the tray. Sits exactly where a single tile would: same monitor work area, same `[tiling] gaps_out`. |
| `real_fullscreen` | true fullscreen |
| `group_toggle`, `group_next`, `group_prev`, `group_add` | tab groups |

### Layout and desktops

| Action | Does |
|---|---|
| `toggle_tiling` | tiling on this desktop |
| `toggle_tiling_all` | ...on every desktop |
| `toggle_floating` | floating mode on this desktop |
| `toggle_floating_all` | ...on every desktop |
| `toggle_split` | flip the next BSP split |
| `tile_focus:l|r|u|d` | focus the neighbouring tile |
| `tile_move:l|r|u|d` | move the window inside the layout |
| `view:<0-9>` | show that desktop; asking for the one already on screen returns to the one before it (`[camera] back_and_forth`) |
| `view:back` | that same way back, whatever desktop is on screen now |
| `move_to:<0-9>` | send the focused window there |
| `move_to_view:next|prev` | send the focused window one desktop over (`next`/`prev` relative to the one on screen) |
| `move_camera:<px>` | pan by that many px (negative = left); holdable |
| `expo` | the desktop strip |
| `toggle_wrap` | close the strip into a ring, or open it |
| *(inside expo)* `o` | the orrery: the ring of desktops turns by itself, seen from above, with a star at its centre |
| *(inside expo)* `star_collapse` | takes that star one step down the road — burning → ember → pulsar → black hole |
| `star_spawn` / `star_off` | light a star under the pointer, on the desktop on screen / put it out |
| `star_collapse` | collapse the `[star]` on its desktop now — dwarf, pulsar or hole, by the mass thrown into it |

### World

| Action | Does |
|---|---|
| `cycle_gravity` | walk `[physics] gravity_steps` |
| `toggle_sun` | shadows on or off outright |
| `sun_mode` | swap the clock for your hand and back; taking hold leaves the sun where it stood |
| `sun_azimuth:<deg>` | turn the light. `+15` / `-15` step, a bare number points it there |
| `sun_elevation:<deg>` | raise or lower it, same two forms. Below the horizon is night |
| `set:<option><op><value>` | turn any runtime option: `set:sun.blur+2`, `set:physics.gravity=981` — see [below](#set-a-dial-for-every-option) |
| `volume:+N` / `volume:-N` / `volume:<N>` / `volume:mute` | the system volume, with the reading on screen — see [below](#volume-the-system-mixer) |

### Interface

| Action | Does |
|---|---|
| `launcher` | the application launcher |
| `terminal` | a terminal |
| `toggle_tray` | hide/show the status strip |
| `modes_menu` | open the modes menu (same as clicking the tray pill) |
| `radial_menu` | open the ring of actions from `[radial]` — see [below](#the-radial-menu) |
| `mixer` | the sound panel: every application's volume, master at the top — see [below](#the-sound-panel) |
| `show_hints` | the key hints overlay |
| `show_errors` | the config-problem panel (same as clicking the ⚠ pill) |
| `wallpaper_picker` | the wallpaper picker |
| `screenshot` | the whole monitor under the pointer, as a PNG on the clipboard |
| `screenshot_region` | dim the screen, drag a rectangle out, copy that; Escape cancels |
| `reload_config` | reload `config.toml`, discarding every `fwmctl set` |

### Monitors

| Action | Does |
|---|---|
| `output_off` | turn off the monitor under the cursor (never the last lit one) |
| `toggle_internal_output` | the laptop panel on/off — for docking |
| `outputs_on` | turn every monitor back on |

### Escapes

| Action | Does |
|---|---|
| `spawn:<command>` | run it through a shell, so `spawn:$BROWSER --new-window` works |
| `mode:<name>` | switch keymaps; `mode:default` returns to the root map |
| `global:<app_id>:<name>` | hand the key to an external shell — see below |
| `EXIT` | end the session |

### Giving a key to an external shell

A Wayland client cannot see a key it is not focused for, so an outside launcher
can never answer `Super+Space` by itself: the compositor has to hand the press
over. `global:` is that handover, over
[hyprland-global-shortcuts-v1](https://github.com/hyprwm/hyprland-protocols),
which Quickshell exposes as its `GlobalShortcut` type.

The client registers a *named* action and says nothing about keys — which keys
reach it stays fwm's decision:

```qml
GlobalShortcut { appid: "quickshell"; name: "launcher"; onPressed: /* ... */ }
```

```toml
[binds]
"super+space" = "global:quickshell:launcher"   # instead of "launcher"
```

Binding the key away from `launcher` is what makes this *replace* fwm's own
launcher rather than sit beside it. Nothing has registered the name yet — the
shell is not running — and the key does nothing but say so in the log.

## Modes (submaps)

A second keymap you step into, so single keys can mean something. While a mode is
active it owns the keyboard: its binds fire, `Escape` leaves, and every other key
does nothing rather than reaching the application underneath — which is what
makes a bare `g` safe to bind.

```toml
[mode.physics]
enter  = "super+o"
"g"    = "cycle_gravity"
"c"    = "calm_all"
"r"    = "spin_all"
```

One-shot by default: the first action drops you back. `sticky = true` keeps the
mode open until `Escape`, for something you mean to repeat. The tray shows the
mode's name while it is active. Up to 8 modes.

## `set:` — a dial for every option

Everything `fwmctl set` can change, a key can change too:

```toml
[binds]
"super+ctrl+XF86AudioRaiseVolume" = "set:sun.blur+2"     # a step up
"super+ctrl+XF86AudioLowerVolume" = "set:sun.blur-2"     # and back down
"super+ctrl+0"                    = "set:sun.blur=8"     # straight there
```

`+`/`-` step from wherever the value is now; `=` puts it where you say. The
names and their ranges are whatever `fwmctl config` lists — physics, gaps,
opacities, the sun, the shadows, the tray. A colour takes `=#RRGGBB` only,
having no steps to take.

This is the shape a knob wants: one key per detent, each worth a step. Unlike
the socket, **a step clamps at the end of its range instead of being refused** —
a dial that stopped answering near the end would be a broken dial — and the
last click that changed nothing lights the end of the bar on the readout.

That readout is the other half: every `set:` puts the name, the value and its
place in the range low on the screen for about a second (see
[the dial readout](interface.md#the-dial-readout)). Turning a number you
cannot see is not turning anything.

Runtime only, like `fwmctl set`: `reload_config` puts the file's values back.
A misspelled option is logged and ignored — the bind still loads, because the
option table is not consulted until the key is pressed.

## `volume:` — the system mixer

```toml
[binds]
"XF86AudioRaiseVolume" = "volume:+5"
"XF86AudioLowerVolume" = "volume:-5"
"XF86AudioMute"        = "volume:mute"
```

`+N` / `-N` step, a bare number goes straight there, `mute` toggles. Each one
puts `Volume 62%` and a bar on screen for a second — which is the whole reason
this exists rather than `spawn:wpctl …`: fwm cannot show a number it never
learns, and a knob with no readout is a knob you turn while watching the
wallpaper for a clue.

fwm is not a mixer. It runs the commands in [`[volume]`](configuration.md#volume)
— wpctl's by default, pactl's on a machine without wpctl — and shows what they
report. Anything else that speaks to your audio stack works there too.

The reading is **predicted, then confirmed**: the panel shows the value on the
frame the knob turned, while a read runs behind it and corrects the number if
the mixer disagreed (something else moved it, or it clamped). `set` is always
handed an absolute percentage, never a step, so a fast spin cannot drift out of
step with the mixer. The very first turn of a session has nothing to predict
from, so it reads first and lands one detent later; every turn after that is
immediate.

## The radial menu

A ring of actions around a hub, meant for a keyboard with a knob: turning the
knob walks the ring, pressing it fires the petal you stopped on. The petals
bloom out of the hub on springs, like the launcher's tiles.

```toml
[radial]
enter  = "super+shift+XF86AudioMute"   # the knob's own press, with modifiers
radius = 190

[[radial.item]]
label  = "Terminal"
icon   = "foot"          # icon theme name or a path; `text = "T"` instead
action = "spawn:foot"
```

A knob is three ordinary keys — turning it is `XF86AudioRaiseVolume` and
`XF86AudioLowerVolume`, pressing it is `XF86AudioMute` — so the way in is that
press with modifiers held, which leaves the bare press still muting. Check what
yours sends with `libinput debug-events` first; some knobs scroll instead.

Outside every menu the knob is nothing special — three keys, bindable like any
others, and matched on modifiers EXACTLY. So a chord costs no code at all:

```toml
[binds]
"XF86AudioRaiseVolume"      = "spawn:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+"
"ctrl+XF86AudioRaiseVolume" = "view:next"      # ctrl held: a desktop over
"ctrl+XF86AudioLowerVolume" = "view:prev"
"alt+XF86AudioRaiseVolume"  = "move_camera:120"  # alt held: pan between them
"alt+XF86AudioLowerVolume"  = "move_camera:-120"
```

Inside a `[mode.*]` the knob's keys are free without any modifier at all — a
submap owns the keyboard outright, so a bare turn can drive whatever the mode is
for (`sun_azimuth:+15` and its like) and the volume never comes into it.

Bare turning still changes the volume, because `ctrl+…` is a different bind and
the plain one no longer matches. Inside the ring or the desktop strip those keys
belong to the overlay whatever is held down — and there turning already means
"the next one along", so the two readings agree.

**A spun knob carries further than a clicked one.** Detents arriving less than
120ms apart in the same direction build a run, and a run that keeps going is
worth more per click — up to `[input] knob_accel` steps (4 by default, 1 turns
it off). It takes three detents to start, so the first click of a turn is
always worth exactly one, and a pause or a turn the other way puts it back to
one immediately: the last click before the thing you want lands on it. The ring,
the launcher and the desktop strip all ask the same question, so the feel is one
thing in all three — and the launcher, the only list here that can be hundreds
of rows long, is where it is felt.

**The volume does not move while the ring is up.** Those three keys are usually
bound to `wpctl` in `[binds]`, and the menu is asked for them *before* the binds
are, exactly as the launcher is: turning the knob to choose a petal must not
also turn the sound down. Every other key is swallowed too, so nothing reaches
the window behind the ring.

| Key | Does |
|---|---|
| knob turn, `←` `→` `↑` `↓`, `Tab` | move the focus around the ring, wrapping |
| knob press, `Return`, `Space` | fire the focused petal |
| `1`…`9`, `0` | fire that petal directly, 1 at the top |
| pointer | hover to focus, click to fire, click outside to close |
| `Escape`, `Backspace` | back one ring — and from the root, close |
| click the hub | back one ring (at the root the hub does nothing) |

Order in the file is order around the ring, clockwise from twelve o'clock, and
ten petals is the cap on one ring. Past that a ring grows downwards rather than
outwards: an item written with items of its own opens them instead of firing.

```toml
[[radial.item]]
label = "Power"
text  = "⏻"

[[radial.item.item]]
label  = "Shut down"
action = "spawn:systemctl poweroff"

[[radial.item.item]]
label  = "Sleep"
action = "spawn:systemctl suspend"
```

Those petals are marked with three dots on the outer edge. Pressing one blooms
its ring out of the hub, and the hub becomes that petal — press it, `Escape` or
`Backspace` to come back up. The knob alone walks the whole tree: turn, press,
turn, press, and press the middle to back out.

`enter` is shorthand for putting `radial_menu` in `[binds]`. Every field, and
the caps on nesting, are described in [Configuration](configuration.md#radial).

## The sound panel

`mixer`. Every application that is playing, one row each, with the master at the
top — and the one place in fwm where the knob does two things:

```toml
[binds]
"super+XF86AudioMute" = "mixer"

[[radial.item]]          # or, where it was meant to live: a petal in the ring
label  = "Sound"
text   = "🔊"
action = "mixer"
```

Turning walks the list. **Pressing takes hold** of the row you stopped on, and
while a row is held turning moves that row's volume instead of the selection.
Press again to let go. A ring could not do this — turning is how you leave a
ring — which is why this one is a list, and why the corner of the panel says
which of the two the knob means right now.

| Key | Does |
|---|---|
| knob turn, `↑` `↓`, `Tab` | walk the list, wrapping at both ends — or, while a row is held, move its volume |
| knob press, `Return`, `Space` | take hold of the row, or let go of it |
| `←` `→` | move the selected row's volume, held or not |
| `m` | mute the selected row |
| `1`…`9` | jump to that row, 1 being the master |
| `Home`, `End` | the master, the last row |
| wheel | walk the list, or move a held row's volume |
| click a row | take hold of it; **click a bar** to drop the volume where you clicked |
| `Escape` | let go — and with nothing held, close |

The list wraps, as the ring does — off the top is the last row, off the bottom
the master. A knob has no ends, so neither does anything it drives.

The volume keys do not reach `[binds]` while the panel is up, for the ring's
reason and then some — here they are *moving* a volume already. Levels are
predicted and confirmed exactly as [`volume:`](#volume-the-system-mixer) does,
and the list is re-read about once a second so an application that starts
playing appears on its own. What it runs is
[`[mixer]`](configuration.md#mixer), with the master row on
[`[volume]`](configuration.md#volume).

## Mouse drags

```toml
[mouse]
"super+left"       = "move"
"super+shift+left" = "move_nocollide"
"super+right"      = "resize"
"super+ctrl+left"  = "twist"
```

Buttons are `left`, `right`, `middle`, `side`, `extra`; modifiers are matched
exactly. The five verbs are the ones only a drag can express, and what they mean
depends on the desktop's mode:

| Verb | Physics / floating | Tiling |
|---|---|---|
| `move` | drag, and let go while moving to throw | pull the tile out of the layout and put it down anywhere, including on another desktop |
| `move_nocollide` | drag it through everything | swap two tiles |
| `resize` | resize from the nearest corner | resize the tile from its nearest corner, moving the layout dividers |
| `swap` | — | swap two tiles |
| `twist` | turn it about its centre; let go spinning and it keeps spinning | — |

Anything else in the value is an ordinary action, fired once on press. A bind with
no modifier eats that button from every client — allowed, but know that you did
it.

## Gestures

```toml
[gestures]
sensitivity    = 1.0
natural        = true
"swipe3+left"  = "pan_desktop"
"swipe3+right" = "pan_desktop"
"swipe3+up"    = "launcher"
"swipe4+left"  = "move_to_view:prev"
```

`swipe<2..5>+left|right|up|down` and `pinch<2..5>+in|out`, with the same action
vocabulary plus `pan_desktop` — which only makes sense as a gesture: it hands the
camera to your fingers and pans live, settling on a desktop when they lift. Bind
it to both horizontal directions; it is one gesture.

A finger count with nothing bound is passed through untouched, so pinch-to-zoom
in a browser still works.

## Keys inside the desktop strip

`expo` (`Super+A`) takes the keyboard while it is up:

| Key | Does |
|---|---|
| `z` | step out to the wider view — the desktops become cards on a ring |
| `x` | close the strip into a ring, or open it back into a line |
| `←` `→`, knob turn | move one desktop along |
| `↑` `↓` | lift the camera over the ring (far view only) |
| `PgUp` `PgDn` | closer / further (far view only) |
| `Return`, knob press | enter the desktop you are looking at |
| `Escape` | leave the strip |

Windows can be dragged between cards with the mouse, and clicking a card enters
that desktop.

The knob does here what it does in [the radial menu](#the-radial-menu) — turn to
choose, press to commit — and on the same terms: while the strip is up those
three keys steer it and **the volume does not move**, whatever `[binds]` says
about them. Every other bind still works inside the strip, which is why only
those three are intercepted early.

## Keys inside the launcher

Type to filter, `↑`/`↓` (or `Tab` / `Shift+Tab`) to move, `Return` to run,
`Backspace` to erase, `Escape` to close. The launcher owns the keyboard while it
is open, for the same reason the strip does.

A knob works here too — turning it walks the list, pressing it runs the row —
and the wallpaper picker is the same panel, so there the press applies the
image. Nothing had to be done about the volume: the launcher is asked for its
keys before the bind table is, which it always was.
