<p align="center">
  <img src="assets/logo-brackets.svg" width="230" alt="fwm logo">
</p>

# fwm — Physics Window Manager (Wayland)

A Wayland compositor written in C (wlroots) where windows behave as physical objects with **mass, momentum, inertia, and velocity** — simulated by a real rigid-body engine ([Box2D](https://box2d.org/) v3). Drag a window and throw it — it slides, bounces off walls, stacks under gravity, and comes to rest like a real object.

This is the primary, actively developed version. The legacy X11 version lives on the [`x11`](https://github.com/iluaii/fwm/tree/x11) branch and is no longer supported.

---

## 🎬 Demonstration
![Demonstration](demo.gif)

[Full-quality demo (mp4, 1.8 MB)](https://github.com/iluaii/fwm/releases/download/v0.3.0/demo.mp4)

## Features

### Physics
- **Real rigid-body simulation** — powered by Box2D: impulse-based collisions, proper mass ratios, resting contact and sleeping. No hand-rolled solver artifacts.
- **Drag to throw** — velocity is sampled over the last frames, a quick flick sends windows flying naturally.
- **Gravity modes** — cycle `Super+G` between zero-g, space mode, and Earth gravity (9.8 m/s² at the compositor's 100 px/m scale). Windows fall, thud, and stack on the floor.
- **Realistic feel** — dull heavy bounces (restitution 0.3), contact friction, no mid-air braking under gravity; long weighty glides in zero-g.
- **Continuous collision** — fast throws never tunnel through walls.
- **Per-window toggles** — pin (`Super+P`), collision off (`Super+N`), calm everything (`Super+Shift+C`).

### World
- **10 virtual desktops** on one continuous strip — the world is 10 screens wide, windows keep absolute positions.
- **Smooth camera** — scroll with `Super+H`/`Super+L` (hold to repeat), jump with `Super+1`…`0`.
- **Parallax wallpaper** — multi-layer background images (PNG/JPEG/WebP) that scroll as you move across desktops. `pan` mode turns one large image into a world you walk across.

### Tiling (Hyprland-style)
- **Per-desktop BSP tiling** — toggle `Super+T`; dwindle-style splits along the longer side.
- **Smooth tile animations** — windows glide into their slots (~250 ms, configurable) instead of teleporting.
- **Configurable gaps** — inner (`gaps_in`) and outer (`gaps_out`).
- **Keyboard control** — directional focus (`Super+Arrows`), move window (`Super+Shift+Arrows`), flip split orientation (`Super+S`).
- **Mouse control** — drag BSP borders with `Super+RightDrag` (instant, no animation lag), swap tiles with `Super+Shift+Drag`.

### Tab-stacks
- **Hyprland-style groups** — stack windows into one slot with a chevron tab bar: `Super+W` toggles a stack, `Super+Shift+W` joins the window below, `Super+Tab` / `Super+Shift+Tab` cycle tabs.

### Built-in overlays
- **App launcher** (`Super+Space`) — fuzzy search over desktop entries with icons, no external `rofi` needed. Launched windows drop into the world with physics.
- **Wallpaper picker** (`Super+Shift+P`) — browse a folder and apply an image instantly; the choice is remembered without ever rewriting your config.
- **Keybind cheat-sheet** (`Super+Shift+/`) — generated from your actual binds, not a static list.
- **Config never costs you the session** — a broken file falls back to built-in binds and reports the problem in a tray pill; fix it and press `Super+Shift+R` to reload live.

### Visuals
- **Focus borders** — accent color on the focused window, muted on the rest; colors and width in the config.
- **Window fade-in** — new windows ease in over ~260 ms (configurable, 0 disables).
- **Impact effects** — windows squash and stretch where they hit; optional camera shake on hard landings.
- **Wallpaper-derived palette** — optionally tint the whole UI toward the wallpaper's dominant hue (`color_source = "wallpaper"`).
- **Minimal tray** — three flat chevron-ended islands: focused window + physics readout, desktop indicators, clock. No titlebars anywhere (server-side decorations).
- **Transparency** — client alpha (e.g. kitty `background_opacity`) is rendered as-is.
- **Fake fullscreen** (`Super+D`) keeps the tray visible; **real fullscreen** (`Super+F`) hides it and covers the whole output.

### Desktop integration
Runs the software you already use: **XWayland** (X11 apps as ordinary physics windows), **layer-shell** (waybar, mako, rofi, swaybg), **ext-session-lock** (hyprlock, swaylock), **idle protocols** (swayidle, no blanking during video), **xdg-activation**, **screencopy** (screenshots, screen share), **gamma-control** (wlsunset), **pointer constraints** (games and mouse-look), **foreign-toplevel** (taskbars), plus drag-and-drop and primary selection.

Known gaps: no HiDPI / fractional output scale, no multimonitor, no IME (xkb layouts do work).

---

## Requirements

| Library | Arch package |
|---|---|
| wlroots 0.20 | `wlroots0.20` |
| wayland-server | `wayland` |
| xkbcommon | `libxkbcommon` |
| cairo + pango | `cairo`, `pango` |
| gdk-pixbuf | `gdk-pixbuf2` |
| Box2D 3.x | `box2d` |
| CMake + pkg-config | `cmake`, `pkgconf` |
| Xwayland (runtime) | `xorg-xwayland` |

wlroots must be built with Xwayland support — CMake refuses to configure otherwise. Xwayland itself starts lazily, only when the first X11 client appears.

---

## Install

One command — installs dependencies (pacman / apt / dnf / xbps), builds Box2D v3 from source if your distro doesn't ship it, builds fwm, and registers a `fwm` session for your display manager:

```sh
git clone https://github.com/iluaii/fwm.git
cd fwm
./install.sh
```

Updating later:

```sh
./install.sh update      # git pull + rebuild + reinstall
./install.sh uninstall   # removes binary + session file, keeps your config
```

## Build (manual)

```sh
cmake -B build
cmake --build build
```

Run from a TTY:

```sh
./build/fwm-wayland
```

Or nested inside another compositor / X session for testing:

```sh
WLR_BACKENDS=x11 ./build/fwm-wayland      # nested X11 window
WLR_BACKENDS=wayland ./build/fwm-wayland  # nested Wayland window
```

---

## Development

A compositor cannot hot-restart the way an X11 window manager can: it *is* the
display server, so when the process exits every client's connection dies with
it and no amount of saved state brings the windows back. Nesting is the working
substitute — run fwm inside your current session and restart it as often as you
like while the session you actually work in never notices.

```sh
./dev.sh                      # rebuild and run nested
./dev.sh -n 2 -g 1            # ... with two terminals, gravity on
./dev.sh -a toggle_tiling_all # ... firing one action after startup
./dev.sh -s shot.png          # screenshot after a few seconds, then quit
./dev.sh -h                   # all options
```

Because keys cannot be injected into a nested compositor, a few env hooks stand
in for them: `FWM_TEST_ACTION`, `FWM_TEST_GRAVITY` (fwm boots in zero-g),
`FWM_TEST_CAMERA`, `FWM_OPEN_PICKER`, `FWM_SHOW_HINTS`, `FWM_DEBUG`. `dev.sh`
wraps each in a flag.

## Configuration

Everything lives in `~/.config/fwm/config.toml`. All sections are optional — missing values fall back to sane defaults.

```toml
[physics]
friction              = 0.985   # zero-g glide brake (per-frame factor)
restitution           = 0.3     # bounciness: 0 = dead, 1 = superball
gravity               = 981.0   # px/s^2; 981 = Earth at 100 px/m
mass_density          = 0.0005  # window mass = area * density
throw_speed_multiplier = 0.65
max_throw_speed       = 1800.0
stop_speed_threshold  = 1.0     # below this a window is considered at rest
tick_rate             = 60.0    # physics steps per second

[input]
# Several layouts + a grp:* option gives you layout switching for free.
# Keep a Latin layout FIRST: binds fall back to it, so ctrl+c and super+q
# keep working while you are typing in another script.
kbd_layout   = "us,ru"
kbd_options  = "grp:alt_shift_toggle"
kbd_variant  = ""
repeat_rate  = 25
repeat_delay = 600

[tiling]
gaps_in    = 6       # px between tiles
gaps_out   = 12      # px between tiles and screen edges
anim_speed = 12.0    # tile glide speed (1/s); 0 = instant

[camera]
anim_ms    = 350.0   # desktop-switch slide (ease-in-out); 0 = instant snap
free_speed = 14.0    # how tightly the camera follows a held move_camera: bind

[effects]
camera_shake = 0.0   # jolt the view on hard impacts; off by default, 1.0 to enable
squash       = 1.0   # windows deform on impact, scaled by speed; 0 disables

[focus]
# When an app asks to be raised (xdg-activation): "never" ignores it,
# "same_desktop" (default) focuses it only if it is already on screen,
# "always" also pans the camera to whichever desktop it lives on.
on_activate = "same_desktop"

[decor]
border_width = 2
col_active   = "#7aa2f7"   # "#RRGGBB" or "#RRGGBBAA"
col_inactive = "#3b4261"
fade_in_ms   = 260.0       # window appear/close animation; 0 disables
wallpaper_fade_ms = 420.0  # cross-fade on runtime wallpaper swap; 0 = cut
tray_opacity     = 0.92    # island fill opacity (0..1)
launcher_opacity = 0.92
icon_theme   = ""          # launcher icons; "" = auto (gtk settings, hicolor)
# Where the UI palette comes from:
#   "config"    — the colours above plus the built-in dark scheme (default)
#   "wallpaper" — tint the tray/panels toward the wallpaper's dominant hue and
#                 take the accent (focus border, desktop marker, tab underline)
#                 from its most vivid colour. Islands stay dark, so text keeps
#                 its contrast whatever the image looks like.
color_source  = "config"
tint_strength = 0.4        # 0..1: how far the islands move toward that hue

# Where the built-in wallpaper picker (super+shift+p) looks for images.
# The chosen image is remembered in ~/.local/state/fwm/wallpaper and overrides
# the first [[wallpaper]] layer below on the next start — your config file is
# never rewritten.
[wallpaper_picker]
dir = "~/Pictures"

# Wallpaper layers, drawn back-to-front.
# fit = "cover" (fill+crop) | "contain" (letterboxed) | "pan" (walk across)
[[wallpaper]]
path = "/path/to/image.png"
fit  = "pan"
# "pan" moves only images wide enough to stick out past the screen at native
# scale; those pan with nothing cropped. A narrower one is shown still, since
# travel would have to be bought by cropping its height. Opt in per layer:
# pan_crop = 0.25   # trade 25% of the height for some travel
# zoom     = 1.6    # or set the render width directly (screen_w * zoom)

[binds]
"super+Return"       = "spawn:kitty"
"super+space"        = "launcher"
"super+q"            = "killclient"
"super+t"            = "toggle_tiling"
"super+d"            = "fake_fullscreen"
"super+f"            = "real_fullscreen"
"super+h"            = "move_camera:-50"
"super+l"            = "move_camera:50"
"super+p"            = "pin_window"
"super+n"            = "toggle_nocollide"
"super+g"            = "cycle_gravity"
"super+s"            = "toggle_split"
"super+shift+c"      = "calm_all"
"super+shift+n"      = "toggle_nocollide_all"
"super+shift+t"      = "toggle_tiling_all"
"super+shift+slash"  = "show_hints"
"super+shift+r"      = "reload_config"
"super+shift+p"      = "wallpaper_picker"
"super+shift+l"      = "spawn:hyprlock"
"super+shift+Escape" = "EXIT"
"super+Left"         = "tile_focus:l"    # Right/Up/Down likewise
"super+shift+Left"   = "tile_move:l"
"super+w"            = "group_toggle"    # tab-stacks
"super+Tab"          = "group_next"
"super+shift+w"      = "group_add"
"super+1"            = "view:0"          # ... "super+0" = "view:9"
```

### Actions

| Action | Meaning |
|---|---|
| `spawn:<cmd>` | run a command |
| `killclient` | close focused window |
| `toggle_tiling` | physics ⇄ BSP tiling for the current desktop |
| `fake_fullscreen` / `real_fullscreen` | fullscreen below the tray / whole output |
| `move_camera:<px>` | scroll the camera (repeats while held) |
| `view:<0-9>` | jump to desktop |
| `tile_focus:l\|r\|u\|d` | focus tile in direction |
| `tile_move:l\|r\|u\|d` | swap tile in direction |
| `toggle_split` | flip split orientation of the focused tile |
| `pin_window`, `toggle_nocollide`, `calm_all`, `cycle_gravity` | physics toggles |
| `toggle_nocollide_all` / `toggle_tiling_all` | same, but every window / every desktop at once |
| `group_toggle`, `group_add`, `group_next`, `group_prev` | tab-stacks: make a stack, join it, cycle tabs |
| `launcher` | built-in app launcher |
| `show_hints` | keybind cheat-sheet overlay |
| `reload_config` | re-read the config file and apply it without restarting |
| `wallpaper_picker` | built-in wallpaper browser; Enter applies the image at once |
| `show_errors` | open the config-problem panel (same as clicking the tray's ⚠ pill) |
| `EXIT` | quit the compositor |

### Mouse

| Gesture | Effect |
|---|---|
| `Super+LeftDrag` | move / throw a window (floating) |
| `Super+Shift+LeftDrag` | move through windows (no collision) — or swap tiles when tiling |
| `Super+RightDrag` | resize (floating) / drag BSP border (tiling) |

---

## License

MIT — see [LICENSE](LICENSE).
