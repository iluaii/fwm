# fwm — Physics Window Manager

A lightweight X11 window manager written in C where windows behave as physical objects with **mass, momentum, inertia, and velocity**. Drag a window and throw it — it will slide, bounce off walls, and collide with other windows like real objects.

---
## 🎬 Demonstration

<div align="center">

<iframe width="100%" height="480" src="https://drive.google.com/file/d/14zxA9xeAa8wHVvjgvlzwjNPMm89Qmvj_/preview" frameborder="0" allow="autoplay"></iframe>

**[Watch on Google Drive](https://drive.google.com/file/d/14zxA9xeAa8wHVvjgvlzwjNPMm89Qmvj_/view?usp=drive_link)** (if embedded player doesn't work)

</div>

---

## Features

- **Physics simulation** — windows have mass (proportional to their area), velocity, and friction. Release a window mid-drag and it flies across the screen.
- **Elastic collisions** — windows bounce off each other and off screen edges with configurable restitution.
- **Drag to throw** — velocity is sampled over the last few frames so a quick flick sends windows flying naturally.
- **Shift-drag** — hold `Shift` while dragging to pass through other windows without collision.
- **10 virtual desktops** — the world is 10 screen widths wide. Drag a window to the edge or press `Super+1`…`0` to slide the camera to any desktop. Windows keep their absolute position in the world.
- **Per-desktop tiling mode** — toggle `Super+T` to switch the current desktop between physics and master-stack tiling layout.
- **Adjustable master ratio** — `Super+L` / `Super+H` grow or shrink the master column in tiling mode.
- **Fake fullscreen** (`Super+D`) and **real fullscreen** (`Super+F`).
- **Chamfered window corners** via the X Shape extension.
- **Status tray** — a hexagon-shaped overlay showing desktop occupancy, the focused window's name, speed, angle, and mass.
- **Focus-follows-mouse** with `EnterNotify`.
- **Rofi** is handled as a special floating overlay centered on screen.

---

## Requirements

| Library | Package (Debian/Ubuntu) | Package (Arch) | Package (Fedora) |
|---|---|---|---|
| Xlib | `libx11-dev` | `libx11` | `libX11-devel` |
| Xft | `libxft-dev` | `libxft` | `libXft-devel` |
| Xext / Shape | `libxext-dev` | `libxext` | `libXext-devel` |
| FreeType | `libfreetype-dev` | `freetype2` | `freetype-devel` |
| pkg-config | `pkg-config` | `pkgconf` | `pkgconf-pkg-config` |

---

## Installation

The `install.sh` script detects your package manager (apt / dnf / pacman), installs dependencies, builds the binary, and optionally configures `~/.xinitrc`.

```sh
git clone <repo-url>
cd fwm
bash install.sh          # full install
bash install.sh update   # rebuild and replace binary only
```

The binary is installed to `/usr/local/bin/fwm` by default. Override with `INSTALL_BIN=/your/path bash install.sh`.

---

## Building manually

```sh
make          # build the fwm binary
make run      # launch inside a 1280×800 Xephyr window (good for testing)
make stop     # kill the Xephyr instance
make clean    # remove build artifacts
```

---

## Starting fwm

Add to `~/.xinitrc`:

```sh
exec fwm
```

Then start X:

```sh
startx
```

---

## Key bindings

All bindings use `Super` (Win key) as the modifier. Edit `src/config.h` to change them.

| Binding | Action |
|---|---|
| `Super+Return` | Launch terminal (`kitty`) |
| `Super+Space` | Launch app launcher (`rofi -show drun`) |
| `Super+Q` | Close focused window |
| `Super+T` | Toggle tiling mode on current desktop |
| `Super+L` / `Super+H` | Increase / decrease master column width |
| `Super+D` | Fake fullscreen (fills screen, keeps physics) |
| `Super+F` | Real fullscreen (EWMH `_NET_WM_STATE_FULLSCREEN`) |
| `Super+1` … `Super+0` | Switch to desktop 1–10 |

---

## Mouse controls

| Action | Gesture |
|---|---|
| Drag window | `Super+LMB` drag |
| Drag through walls | `Super+Shift+LMB` drag |
| Resize window | `Super+RMB` drag |
| Throw window | `Super+LMB` drag then release with momentum |

---

## Configuration

Edit `src/config.h` before building. Key options:

```c
#define MOD_KEY  Mod4Mask          // modifier key (Super by default)
static const char *termcmd[] = { "kitty", ... };
static const char *menucmd[] = { "rofi", ... };
```

Physics constants live in `src/defines.h`:

| Constant | Default | Effect |
|---|---|---|
| `MASS_DENSITY` | `0.0005` | Mass per pixel² |
| `FRICTION` | `0.97` | Velocity decay per tick |
| `RESTITUTION` | `0.75` | Elasticity of collisions / wall bounces |
| `THROW_SPEED_MULTIPLIER` | `0.65` | Scales drag velocity on release |
| `MAX_THROW_SPEED` | `1800.0` | Hard cap on throw velocity (px/s) |
| `STOP_SPEED_THRESHOLD` | `1.0` | Speed² below which a window stops flying |
| `PHYSICS_TICK_RATE` | `60.0` | Simulation steps per second |
| `MAX_WINDOWS` | `32` | Maximum tracked windows |

---

## Project structure

```
src/
  main.c          — event loop, tray data aggregation
  wm.c / wm.h     — core WM: event handling, drag/resize, keybindings
  physics.c / .h  — physics world: bodies, step, collisions, throw
  window.c / .h   — geometry helpers, window spawning
  ui/
    tray.c / .h       — status bar (hexagon overlay, Xft rendering)
    decorations.c / .h — borders and chamfered corners (Shape extension)
config.h          — keybindings and launch commands
defines.h         — tunable physics and layout constants
```

---

## License

See [LICENSE](LICENSE).
