# Keybindings and actions

Nothing here is hard-coded. Every entry in the default table below is an ordinary
`[binds]` line you can change, and every drag verb is an ordinary `[mouse]` line.
`Super+Shift+?` shows the live list on screen.

Contents: [defaults](#default-bindings) · [actions](#actions) ·
[modes](#modes-submaps) · [mouse](#mouse-drags) · [gestures](#gestures) ·
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
| `view:<0-9>` | show that desktop |
| `move_to:<0-9>` | send the focused window there |
| `move_to_view:next|prev` | send the focused window one desktop over (`next`/`prev` relative to the one on screen) |
| `move_camera:<px>` | pan by that many px (negative = left); holdable |
| `expo` | the desktop strip |
| `toggle_wrap` | close the strip into a ring, or open it |

### World

| Action | Does |
|---|---|
| `cycle_gravity` | walk `[physics] gravity_steps` |

### Interface

| Action | Does |
|---|---|
| `launcher` | the application launcher |
| `terminal` | a terminal |
| `toggle_tray` | hide/show the status strip |
| `modes_menu` | open the modes menu (same as clicking the tray pill) |
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
| `←` `→` | move one desktop along |
| `↑` `↓` | lift the camera over the ring (far view only) |
| `PgUp` `PgDn` | closer / further (far view only) |
| `Return` | enter the desktop you are looking at |
| `Escape` | leave the strip |

Windows can be dragged between cards with the mouse, and clicking a card enters
that desktop.

## Keys inside the launcher

Type to filter, `↑`/`↓` (or `Tab` / `Shift+Tab`) to move, `Return` to run,
`Backspace` to erase, `Escape` to close. The launcher owns the keyboard while it
is open, for the same reason the strip does.
