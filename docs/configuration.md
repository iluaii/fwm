# Configuration

Everything lives in one file: `~/.config/fwm/config.toml`
(`$XDG_CONFIG_HOME/fwm/config.toml` if that is set). Nothing in it is required —
every section is optional and every missing value falls back to a default.
[`config.toml.example`](../config.toml.example) is a complete annotated file to
copy.

Three promises about this file:

- **A broken config never costs you the session.** A syntax error, an unknown
  key, a colour that is not a colour: fwm carries on with defaults for whatever
  it could not read, installs its built-in keybindings if yours yielded none, and
  reports the problem in a tray pill you can click to read.
- **fwm never writes to it.** Choices made in the UI (the picked wallpaper, the
  modes menu) go to `~/.local/state/fwm/`, so your comments and formatting are
  yours. `fwmctl set` changes a value for the session only.
- **`Super+Shift+R` reloads it live**, and reloading discards every `fwmctl set`
  override — the file is the source of truth.

Contents: [physics](#physics) · [per-desktop physics](#per-desktop-physics) ·
[window rules](#window-rules) · [tiling](#tiling) · [camera](#camera) ·
[decor](#decor) · [input](#input) · [focus](#focus) · [effects](#effects) ·
[session](#session) · [binds](#binds) · [modes](#modes) · [mouse](#mouse) ·
[gestures](#gestures) · [wallpaper](#wallpaper) · [wallpaper_picker](#wallpaper_picker) ·
[cava](#cava) · [sound](#sound) · [output](#output)

---

## physics

```toml
[physics]
friction               = 0.985   # velocity kept per tick in zero-g (0..1)
mass_density           = 0.0005  # window mass = width * height * density
mass                   = "size"  # or "ram" — see below
mass_ram_ref           = 300.0   # MB that weighs a normal window
mass_ram_max           = 20.0    # heaviest a window may get, x normal
throw_speed_multiplier = 0.65    # how much of the flick becomes velocity
max_throw_speed        = 1800.0  # the world's one speed limit, px/s
stop_speed_threshold   = 1.0     # px/s below which a window counts as at rest
restitution            = 0.3     # bounciness, 0 = dead stop, 1 = superball
gravity                = 981.0   # px/s^2; 981 = earth at 100 px/m
tick_rate              = 60.0    # simulation steps per second
gravity_steps          = [0.0, 0.15, 1.0]   # what Super+G cycles
hp_break_speed         = 0.0     # 0 = follow max_throw_speed
solid_bars             = false   # do panels hold the floor up?
```

| Key | Meaning |
|---|---|
| `friction` | Per-tick velocity retention, the brake on a zero-g glide. 0.985 keeps 98.5% of the speed every tick. Under gravity, windows are slowed by contact friction instead, not by this. |
| `mass_density` | Mass per pixel of window area. Only ratios matter: a window twice the area of another is twice as heavy. |
| `mass` | What decides a window's weight: `"size"` (area × density) or `"ram"` (how much memory the application is using, its size ignored). See [Physics](physics.md#mass). |
| `mass_ram_ref` | Under `mass = "ram"`, the footprint in MB that weighs the same as an ordinary window did. Larger = only the real hogs get heavy. |
| `mass_ram_max` | Ceiling on how heavy `mass = "ram"` can make one window, in multiples of normal. Not optional: an uncapped 6GB browser is not a heavy window, it is a wall, and Box2D answers a 300:1 mass ratio by pushing the light body through the floor. |
| `throw_speed_multiplier` | How much of the hand's speed a release turns into window velocity. |
| `max_throw_speed` | The one speed limit in the world: **no dynamic window ever moves faster than this**, however it was set moving — thrown, shoved by a drag, kicked by a visualiser bar. 0 removes the limit. |
| `stop_speed_threshold` | Below this a window is considered stopped, its velocity is zeroed and the tray stops calling it flying. |
| `restitution` | How much of an impact comes back as bounce. 0.3 is heavy furniture; 0.9 is a beach ball. |
| `gravity` | Downward acceleration in px/s², before the `Super+G` multiplier. fwm starts in zero-g whatever this says. |
| `tick_rate` | Simulation steps per second. Read once at startup; a change needs a restart, not a reload. |
| `gravity_steps` | The multipliers `cycle_gravity` (`Super+G`) walks, in order. Up to 8. |
| `hp_break_speed` | How fast a window must hit another **of its own mass** to destroy it, px/s. 0 (the default) means "use `max_throw_speed`". Only tunes the mode; it cannot switch it on. |
| `solid_bars` | Turn a layer-shell panel that reserved space into a solid object: the floor stands on top of a bar at the bottom of the screen, and a bar at the top is a ceiling. Off by default, and windows slide underneath one as they always have. Only **exclusive zones** count — fwm's own tray floats above the windows and is not one. The floor is one line across all ten desktops, so it follows the **primary** monitor's bars. |

### breakable windows

**There is no `hp` key, and turning this on is not a setting you can save.** It is
switched from the **Breakable** row of the modes menu, it applies to that session
only, and every session starts with windows unbreakable — nothing in
`config.toml` or `~/.local/state/fwm/` can change that. A mode that destroys
unsaved work should never be something you are living under because of a choice
you made last week and have forgotten.

`hp_break_speed` is a config key because it only says *how* breaking works if you
ever turn it on, which is a preference; whether windows break at all is not.

With `hp` on, every collision is worth

```
damage(A → B) = mass(A) × (speed / hp_break_speed)² × hardness(A)
hp(B)         = mass(B) × toughness(B)
```

and **B is destroyed if a single hit is worth more than all of its hit points.**

Damage is never accumulated. A window that survives a blow is in perfect health
afterwards, so a stack that settles overnight is not a slow execution, and there
is no hidden state to wonder about.

Both sides of a collision are worked out the same way, so two equal windows
meeting fast enough destroy each other. Walls deal no damage at all — otherwise
a window dropped from the top of the screen would shatter on the floor every
time it was thrown.

Speed is squared because breaking things is about energy, and because it is what
keeps this survivable: at half the break speed a hit is worth a quarter, so
nudges are safe and only real slams are lethal.

Left at the default, `hp_break_speed` follows `max_throw_speed` — and since
nothing in the world may travel faster than that limit, damage can never exceed
the attacker's own mass. **A window is then physically incapable of breaking
anything heavier than itself.** Set it lower and a fast light window can punch
above its weight.

Under `mass = "ram"` a window's hit points are frozen at the moment that mode
takes hold, rather than drifting with its memory use — a window should not
become unkillable while you are not looking at it.

`toughness` and `hardness` in `[[rule]]` do persist, because they only describe
what a window is made of. They do nothing at all until the mode is on.

Windows are asked to close, not killed, so a client with unsaved work can still
put up its own dialog. If it declines, the next hard enough hit asks again.

**Some windows therefore cannot be destroyed at all.** A client that never
handles the close request — Firefox's "Firefox is already running" dialog is the
one everybody meets — simply ignores every blow it is dealt and survives on a
desktop where everything else breaks. This is deliberate and not a bug you have
found: the alternative is killing the client outright, and a compositor may not
take an application down because one of its windows was hit too hard. If a
window will not close when you press your `killclient` bind, it will not close
when a window falls on it either; those are the same request.

One consequence to know about: reloading the config turns Breakable back off,
because the mode lives in the physics config and a reload replaces that
wholesale with the file plus its defaults. It fails toward windows being safe,
which is the right direction for it to fail in, but it does mean an edit-and-
reload puts the mode back where every session starts.

## per-desktop physics

A `[physics.<name>]` table is a **diff against `[physics]`** and names the
desktops it applies to. A desktop no profile claims behaves exactly as
`[physics]` says, so adding this section changes nothing until you point one at a
desktop.

```toml
[physics.moon]
gravity  = 160.0          # a sixth of earth: long, slow falls
desktops = [3, 4]

[physics.water]
friction    = 0.9         # everything stops quickly
restitution = 0.05        # and lands without a bounce
gravity     = 300.0
desktops    = [7]
```

Writable keys: `friction`, `mass_density`, `restitution`, `gravity`. Up to 10
profiles. A desktop claimed by two profiles goes to the last one. A profile that
names no valid desktop is reported rather than silently ignored.

Windows carry their desktop's physics with them: drag one from the moon onto the
water desktop and it slows down as it crosses the edge. `Super+G` is still the
master switch and 0 there is 0 everywhere.

## window rules

Per-window overrides, applied once when the window is mapped. Matchers are POSIX
extended regexes against the window's `app_id` and `title`; a rule with several
matchers needs all of them to hit. Rules are evaluated in file order and every
match is applied, so a later rule overrides an earlier one field by field. Up to
64 rules.

```toml
[[rule]]
app_id    = "^mpv$"
mass      = 8.0        # shrugs off anything that shoves it
nocollide = false
pin       = false

[[rule]]
title     = "Picture-in-Picture"
pin       = true       # immovable
nocollide = true       # and other windows pass through it
desktop   = 2          # opens here

[[rule]]
app_id    = "^Alacritty$"
gravity   = -0.2       # a balloon: drifts up in a room where things fall
bounce    = 0.9
friction  = 0.999      # glides for a long time

[[rule]]
app_id    = "^org.gnome.Calculator$"
toughness = 0.2        # made of glass, if [physics] hp is on
hardness  = 3.0        # ... but hits like a brick
```

| Key | Meaning |
|---|---|
| `app_id`, `title` | Matchers. At least one is required, or the rule is reported as useless. |
| `mass` | Multiplies the desktop's `mass_density` for this window. 8 = heavy, 0.1 = skittish. |
| `gravity` | Multiplies the desktop's gravity. 0 = weightless, negative = floats up. |
| `bounce` | Restitution, absolute 0..1 — it replaces the desktop's, not multiplies it. |
| `friction` | Per-tick velocity retention, absolute 0..1. |
| `toughness` | Multiplies this window's hit points. 0 = glass, broken by any hit at all; 10 = a safe. Only means anything while `[physics] hp` is on. |
| `hardness` | Multiplies the damage this window **deals**. Separate from `toughness` so a heavy soft thing does not have to be a hammer. |
| `nocollide` | Other windows pass through it; it still cannot leave the play area. |
| `pin` | Immovable: physics never moves it. |
| `desktop` | 0..9, where the window opens. |

There is deliberately no per-window "float": tiling on fwm is a property of the
**desktop**, so such a rule could not be honoured.

## tiling

```toml
[tiling]
gaps_in    = 6      # px between adjacent tiles
gaps_out   = 14     # px between tiles and the screen edge
anim_speed = 12.0   # tile-glide rate, 1/s; 0 = snap into place
```

## camera

```toml
[camera]
anim_ms    = 350.0  # desktop-switch slide; 0 = instant
free_speed = 14.0   # how tightly a held move_camera follows, 1/s
wrap       = false  # the strip is a ring
```

`wrap` closes the strip into a ring: stepping right off the last desktop arrives
at the first, and a window thrown off one end flies on and appears at the other.
Only **steps** wrap — next/prev, the wheel, a gesture; a free pan still stops at
the ends. The join is never drawn as a slide across nine desktops; the camera
jumps and the transition is a short slide of one screen.

## decor

```toml
[decor]
border_width      = 2
col_active        = "#89b4fa"     # "#RRGGBB" or "#RRGGBBAA"
col_inactive      = "#45475a"
fade_in_ms        = 260.0         # window open fade; 0 disables
wallpaper_fade_ms = 420.0         # wallpaper cross-fade; 0 = instant cut
tray_opacity      = 0.92          # tray island fill alpha
tray_yield        = false         # hide the strip where a bar reserved the top
launcher_opacity  = 0.92
icon_theme        = ""            # launcher icons; "" = auto (gtk3, then hicolor)
color_source      = "config"      # or "wallpaper"
tint_strength     = 0.4           # 0..1, only with color_source = "wallpaper"
```

`tray_yield` is what lets an external bar **replace** the status strip rather
than stack with it: on a monitor where a layer-shell client reserved space along
the top, fwm's strip hides and stops reserving its own band. Off by default —
a client asking for space should not be able to take fwm's chrome off the screen
on its own. See [the interface](interface.md#the-tray).

`color_source = "wallpaper"` derives the whole UI palette — island fill, accent,
text — from the current wallpaper image, so the tray belongs to the desktop
behind it. `tint_strength` is how far the fill moves toward the wallpaper's hue.
Colours are premultiplied internally; write them as plain hex.

## input

```toml
[input]
kbd_layout   = "us,ru"
kbd_variant  = ""
kbd_options  = "grp:alt_shift_toggle"
repeat_rate  = 25       # chars/s
repeat_delay = 600      # ms before repeat starts

tap = true              # tap-to-click
#tap_drag         = true
#drag_lock        = false
#natural_scroll   = false
#dwt              = true             # ignore the pad while typing
#middle_emulation = false
#left_handed      = false
#accel_speed      = 0.0              # -1 (slow) .. 1 (fast)
#accel_profile    = "adaptive"       # or "flat"
#scroll_method    = "two_finger"     # or "edge", "button", "none"
#click_method     = "button_areas"   # or "clickfinger", "none"
```

Keep a Latin layout **first** in `kbd_layout`: binds fall back to it, so
`Super+Q` keeps working while you are typing in another script.

Every touchpad key except `tap` is tri-state — leave it out and the device keeps
whatever libinput decided for that model, which is usually a better answer than a
number in a config file. `tap` is the exception and defaults to on, because
libinput ships tap-to-click disabled and a laptop where touching the pad does not
click is not one anyone can work on. A pad that cannot do a setting ignores it;
`libinput list-devices` shows which is which.

## focus

```toml
[focus]
on_activate = "same_desktop"   # "never" | "same_desktop" | "always"
```

What an xdg-activation request may do — an application asking to be raised, like
a link opening in a running browser. Split three ways because on fwm the
disruptive part is not the focus change but the **camera** leaving the desktop you
are working on. `same_desktop` (the default) focuses the window only if it is
already on the visible desktop.

## effects

```toml
[effects]
camera_shake = 1.0   # screen shake on impact; 0 disables
squash       = 1.0   # squash & stretch on impact; 0 disables
jelly        = 1.0   # wobble while dragging; 0 disables
spin         = 1.0   # strength of the spin_window kick (experimental)
live         = 1.0   # live window content under spin/wobble; 0 = still frame
shot_fly     = 1.0   # region screenshot peels off and flies away; 0 disables
```

`shot_fly` is the one effect that is not about windows: when `screenshot_region`
takes its picture, a frozen copy of the region lifts off the screen, shrinks,
tilts and flies down into the line that says it was copied, with the live screen
carrying on underneath. The value scales duration and distance together — `0.5` is a
quick flick, `2.0` a slow drift, `0` nothing at all. Only the region shot has
it; `Print` stays instant. The tilt needs the GLES2 renderer — anywhere
arbitrary rotation is unavailable the copy flies upright instead, which is a
smaller loss than dropping the effect.

`live = 0` puts every window under an effect back on a periodic snapshot — a
still frame that costs the machine nothing, which is the better trade on hardware
where the effect judders.

The wobble has a stretch limit, like real jelly. It lags behind the hand in
proportion to how fast you are moving — and the warped picture is drawn into a
buffer only so much bigger than the window, so past roughly 2000 px/s the sheet
would leave that buffer, be clipped by its edge, and fold through itself. Instead
it saturates: shake a window as hard as you like and the wobble grows exactly as
it always did up to the point it would have torn, then simply stops growing. A
higher `jelly` scales the deformation, so it reaches that ceiling proportionally
sooner.

## session

```toml
[session]
restore = "crash"    # "crash" | "always" | "never"
```

When a restarted fwm puts your applications back. The distinction that matters is
not "did fwm start" but "did the last run end the way you meant it to": the state
file is deleted on a clean shutdown, so finding one at startup means the previous
run died. `never` records nothing at all.

Two limits: an application whose window belongs to a different process than the
one launched (some browsers, Electron apps) comes back on the current desktop
rather than its old one, and at most 64 applications are recorded.

## binds

```toml
[binds]
"super+Return"    = "terminal"
"super+q"         = "killclient"
"super+shift+1"   = "move_to:0"
"super+p"         = "spawn:playerctl play-pause"
```

Keys are `mod+mod+key` with the modifiers `super`, `alt`, `ctrl`, `shift` and an
xkb keysym name (`Return`, `space`, `question`, `Escape`, `Left`, …). Modifiers
are matched exactly. Values are actions —
see [Keybindings and actions](keybindings.md) for the whole vocabulary; an
unknown one is reported at load time rather than doing nothing when pressed.

If `[binds]` is missing, empty, or every line in it is broken, fwm installs its
built-in binds so that a forgotten quote cannot leave you with a compositor that
cannot open a terminal or exit.

## modes

A second keymap you step into, so single keys can mean something. Up to 8.

```toml
[mode.physics]
enter    = "super+o"
"g"      = "cycle_gravity"
"c"      = "calm_all"
"r"      = "spin_all"

[mode.resize]
enter    = "super+shift+e"
sticky   = true
"h"      = "tile_move:l"
"l"      = "tile_move:r"
```

While a mode is active it owns the keyboard outright: its binds fire, `Escape`
leaves, and every other key does nothing rather than reaching the application
underneath — which is what makes a bare `g` safe to bind. A mode is one-shot by
default (the first action drops back to the root map); `sticky = true` keeps it
open until `Escape`.

`enter` is a convenience that registers `mode:<name>` in the root map for you.
`mode:<name>` works as an ordinary action anywhere, and `mode:default` returns to
the root map.

## mouse

```toml
[mouse]
"super+left"       = "move"
"super+shift+left" = "move_nocollide"
"super+right"      = "resize"
"super+ctrl+left"  = "twist"
"super+middle"     = "killclient"     # any ordinary action works too
```

Buttons: `left`, `right`, `middle`, `side`, `extra`. The five drag **verbs** are
the ones only a drag can express:

| Verb | Physics / floating desktop | Tiling desktop |
|---|---|---|
| `move` | drag the window; let go while moving to throw it | drags the tile out of the layout |
| `move_nocollide` | drag it through everything else | swaps two tiles |
| `resize` | resize from the nearest corner | resizes the tile from its nearest corner, moving the layout dividers |
| `swap` | — | swaps two tiles |
| `twist` | turn the window about its centre; let go spinning and it keeps spinning | — |

Anything else in the value is an ordinary action, fired once on press. Modifiers
are matched exactly, and a bind with no modifier at all will eat that button from
every client — yours to do, knowingly. Up to 16 binds.

## gestures

Touchpad gestures, keyed on how many fingers moved which way. Up to 32.

```toml
[gestures]
sensitivity    = 1.0   # camera px per finger px
natural        = true  # the strip follows the fingers
"swipe3+left"  = "pan_desktop"
"swipe3+right" = "pan_desktop"
"swipe3+up"    = "launcher"
"swipe3+down"  = "toggle_tray"
"swipe4+left"  = "move_to_view:prev"
"swipe4+right" = "move_to_view:next"
#"pinch3+in"   = "calm_all"
```

Names are `swipe<2..5>+left|right|up|down` and `pinch<2..5>+in|out`. The action
vocabulary is the same one `[binds]` uses, plus `pan_desktop`, which only makes
sense as a gesture: it hands the camera to your fingers and pans live, settling
on a desktop when they lift. Bind it to **both** horizontal directions — it is
one gesture, not two.

A finger count with nothing bound is left alone entirely, so a client that
understands gestures itself (pinch-to-zoom in a browser) still gets them.

## wallpaper

An array of layers, drawn back to front, each scrolling at its own rate as the
camera moves across the strip.

```toml
[[wallpaper]]
path   = "~/Pictures/sky.jpg"
fit    = "pan"        # "cover" | "contain" | "pan" | "video"
#zoom  = 1.5          # "pan" only: more travel, slightly softer
#pan_crop = 0.2       # "pan" only: how much height may be given up for travel

[[wallpaper]]
path   = "~/Videos/rain.mp4"
fit    = "video"
#fps   = 30           # "video" only: cap the presentation rate
```

| `fit` | What it does |
|---|---|
| `cover` | fills the screen, crops the overflow, static (the default) |
| `contain` | shows the whole image, letterboxed, static |
| `pan` | a background you walk across: the image scrolls from its left edge on desktop 0 to its right edge on the last one |
| `video` | a looping clip, any format ffmpeg decodes, scaled to cover |

Video decodes on its own thread in software; playback pauses whenever a
fullscreen window covers it, and `fps` trims CPU further on weak hardware.

## wallpaper_picker

```toml
[wallpaper_picker]
dir = "~/Pictures"
fps = 30.0     # base fps cap for videos chosen through the picker
```

What `wallpaper_picker` (`Super+Shift+P`) browses. A picked image is remembered
in `~/.local/state/fwm/wallpaper` and applied over the configured one on every
load, so a reload keeps it rather than snapping back.

## cava

The audio visualiser: a row of spectrum bars fed by loopback capture of whatever
is playing, with no external `cava` process.

```toml
[cava]
mode        = "both"    # "off" | "visual" | "physical" | "both"
bars        = 48        # 2..128
height      = 160.0     # px the loudest band reaches
gap         = 3.0
sensitivity = 1.0
smoothing   = 0.75      # bar fall inertia, 0..0.99
push        = 1.0       # physical bar height vs. the drawn one
opacity     = 0.85
min_hz      = 40.0
max_hz      = 12000.0
```

The two halves of `mode` are independent: `visual` draws bars that never touch a
window, `physical` makes them real but invisible — a row of solid bodies along
the floor that windows stand on and get thrown by. `both` is the point of the
feature. Pushing only works on a desktop in physics mode, and the row stands on
the floor of the desktop you are looking at.

Every knob is live: `fwmctl set cava.push 2` while the music plays. `bars` is the
exception — it rebuilds every bar body, so it takes a reload.

## sound

Windows knock when they collide. See [The interface](interface.md#collision-sound).

```toml
[sound]
collisions = true
path       = "~/sounds/knock.wav"   # omit for the built-in click
volume     = 0.6
min_speed  = 200.0    # px/s: quieter than this is not worth hearing
max_speed  = 2000.0   # px/s: full volume at and above this
```

`path` takes a WAV (16-bit PCM or 32-bit float, mono or stereo). Anything it
cannot read is reported and the built-in click stands in, so a typo never leaves
the feature silently doing nothing. Volume follows how hard the hit was; heavier
windows knock deeper.

## output

Where a monitor sits, how it is driven, and what it starts on. Up to 8 entries.
Without one, a monitor comes up in its preferred mode, is placed to the right of
the screens already there, and takes the lowest free desktop.

```toml
[[output]]
name      = "HDMI-A-1"      # as fwm logs it at startup
mode      = "1920x1080@60"  # refresh optional
scale     = 1.5
transform = "90"            # normal|90|180|270|flipped|flipped-90|...
x         = 1366            # top-left in layout coordinates; both or neither
y         = 0
desktop   = 3
enabled   = true
```

Every field except `name` is optional, and leaving one out means "as the backend
brought it up" rather than any value fwm could pick — an entry that only moves a
screen must not also force a resolution on it. `fwmctl outputs` lists the names
and every mode each screen offers; `fwmctl output` changes one live.
