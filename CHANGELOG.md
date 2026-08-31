# Changelog

## Unreleased

### Tiling

- **A new window opens beside the window under the pointer**, splitting it on
  the edge the pointer is nearest, instead of always splitting the focused one
  along its longer side. It is the rule a window carried in and dropped was
  already put down by, given to a window that arrives on its own: the hand is
  usually over the place you meant it to go. The pointer over another desktop
  or in a gap between tiles still splits the focused window, and
  `[tiling] spawn_cursor = false` turns the whole thing off.

### Shadows

- **A shadow follows the hand during a resize, not the client's last answer.**
  The shadow was only re-cast when the client committed a new size, so dragging
  an edge faster than the window redraws left it the size of a frame or two ago:
  it slid out from under the window and back on every answer. It is now moved by
  the same pass that moves the frame, which was given the asked-for box for
  exactly this reason. Applies to a floating resize and to a tiling divider
  alike.

### Monitors of different sizes

- **A desktop is the size of the largest monitor, not the primary one.** A
  screen bigger than the primary used to have a band of glass along its right
  and its bottom that was not part of any desktop: tiling stopped short of it,
  a window could not be put there, and nothing could rest there to be seen.
  Sized off the biggest, every screen shows a whole desktop.
- **The right edge of a smaller screen is a fence.** A window pushed along by
  its neighbours stops where the glass does instead of being pressed on into
  the part of the desktop behind the bezel, and one that comes down out there is
  put back. A window in flight goes straight through it — that is how a window
  crosses to the next desktop, and it stays crossable.
- Together with the per-desktop floor, this closes #20.

### Clipboard

- **The kept copy is read a moment after the copy, not during it.** A copy is
  several flavours at once, and asking a Chromium-based client for one of them
  while it is still arranging the others wedged that client's own clipboard
  until it was restarted. fwm now waits for the selection to stand still first.
  The cost is a copy made in the last fraction of a second of a window's life,
  which is not kept.

## 0.5.0

The first release since the Wayland port settled. 277 commits since 0.3.0.

0.4.0 exists as a tag from the middle of that stretch and was never released;
it is left where it is rather than moved, so nothing anyone has already
fetched changes underneath them.

### Read this before upgrading

- **The licence is now GPLv2**, not MIT.
- **The compositor is called `fwm`**, not `fwm-wayland`. `install.sh` removes
  the old binary; anything pointing at the old name must be updated.
- **wlroots is pinned to the 0.20 branch.** `install.sh` builds it from source
  on distributions that package a different one.
- **Your config is out of date.** `./install.sh update config` merges the new
  options into it and keeps your values and your binds.
- Extra monitors are supported now. They were not in 0.3.0.

### Desktops

- Ten desktops sit on one continuous strip the camera pans across, and the
  strip is a closed ring: a window thrown off the end arrives at the other.
- The desktop strip, on `super+grave`: pull the camera back, see every desktop
  at once, and pick one. Its own keys are labelled along the bottom.
- Every monitor gets its own desktop, wallpaper and tray. Two monitors trade
  desktops when one is asked for a desktop the other is showing.
- Per-desktop mode: physics, BSP tiling, or floating.
- Desktop-to-desktop navigation on `super+ctrl+left`/`right`; sending a window
  to another desktop flies it across the strip rather than dropping it there.

### Tiling

- BSP tiling with dividers you can drag; the dragged windows are snapshotted
  into rubber pictures, so a resize is smooth while clients redraw behind.
- Windows carried out of the layout round off into a drop and pour back in.
- Tiles are positioned from the sizes clients actually commit, and a layout is
  centred in its area rather than anchored top-left.
- Fullscreen and fake fullscreen behave inside a tiled desktop.

### Physics

- Per-window materials and per-desktop physics settings.
- Drag wobble rebuilt as a sheet of springs; impact squash and stretch;
  collision sounds; windows that break on a hard enough hit.
- Free window rotation on `super+r`.
- Windows that dance to whatever is playing.
- Grass along the bottom of every monitor, and wind that can be switched off.

### The interface

- Built-in launcher, tray, radial menu, sound panel, screenshot tool, wallpaper
  picker, spectrum visualiser and one-line message bar — no rofi, grim, slurp
  or cava alongside.
- The tray carries load, memory, charge, network traffic and the active mode.
- Panels are drawn on frosted glass, cut to the shape they paint.
- Animated (video) wallpapers, with the palette derived from them; the colour
  script dresses the desktop it starts on.
- A black hole and a star that can be put on the desktop, and an orrery.
- Keybind sheet on `super+slash`, grouped under the modifier that holds each
  bind.

### Input

- Separate bind tables for the mouse and the keyboard; touchpad gestures and
  libinput settings.
- Binds, the desktop strip and the sound panel all work on a non-Latin
  keyboard layout — an unmatched key is retried against the first layout.
- Volume knob support: a dial with a face, a ring of actions, and the ability
  to walk the launcher.
- CapsLock that locks only while held.

### Wayland and compatibility

- XWayland, with tab-stacks, popup and input routing fixed. One known bug is
  documented in the README.
- ext-session-lock-v1, ext-idle-notify, idle-inhibit, xdg-activation behind a
  `[focus]` policy, pointer constraints and relative pointer, drag-and-drop,
  output power management, gamma control, cursor-shape,
  wlr-foreign-toplevel-management, and the capture protocol the portals are
  moving to.
- Sandboxed clients are shown only what is their own.
- Screen sharing works: the portal is pointed at the wlroots backend.
- The clipboard survives the window that filled it closing, and a copied image
  is no longer asked for its text.

### fwmctl and scripting

- A control socket and the `fwmctl` CLI: read state, change any setting live
  without touching the config file, and stream events a script can react to.
- The socket is kept to the user it belongs to.
- An external shell can take the desktop over rather than sit under it, and a
  bind that belongs to it can have it.
- Session restore after an unclean exit.

### Stability and security

- **A locked session no longer loses the keyboard.** Anything mapping behind
  the lock screen could move focus off the password field, and the password
  was then typed into a window the lock had hidden.
- Crash fixes: a resize on any desktop but the first, a constraint listener
  removed twice, workspace groups freed by wlroots, a poisoned velocity
  reaching the solver, `bsp_collect_leaves` writing past its caller's array.
- Spawned applications no longer pile up as zombies.
- CI runs the build, unit tests and a dynamic harness that drives the
  compositor under ASan/UBSan and ThreadSanitizer.
- Built with warnings and a stack protector on.

### Build and install

- Nix flake support.
- `install.sh` runs the package manager only when something is actually
  missing, so a rebuild does not wait on a repository sync. `FWM_SKIP_DEPS=1`
  skips the check too.
- wlroots 0.20 and Box2D v3 are built from source when the distribution does
  not ship them.
- `install.sh update config` merges an older config into the current example.
- The manual lives in `docs/`; the README is the tour.

## 0.3.0 and earlier

See the release notes on GitHub.
