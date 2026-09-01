# Changelog

## 0.6.0

Two days on top of 0.5.0, cut for the last thing in it: a window closing beside
another one could take the whole session down, which is the one bug a
compositor cannot ask anyone to live with.

The rest is what the first days of living on 0.5.0 turned up — per-monitor
wallpapers, the tiling layout read from the config, menus that stay on their
own screen, and a resize that ends without a jolt.

### Tiling

- **A new window opens beside the window under the pointer**, splitting it on
  the edge the pointer is nearest, instead of always splitting the focused one
  along its longer side. It is the rule a window carried in and dropped was
  already put down by, given to a window that arrives on its own: the hand is
  usually over the place you meant it to go. The pointer over another desktop
  or in a gap between tiles still splits the focused window, and
  `[tiling] spawn_cursor = false` turns the whole thing off.
- **Every desktop can start tiled.** `[tiling] default = true` brings all ten
  up in the layout instead of under physics, for the life that is lived there
  and where flipping each desktop by hand at every login is a chore. It also
  takes a mode name (`"floating"`) or a list, one entry per desktop from the
  first up. It is a starting mode, not a lock: `toggle_tiling` still takes a
  desktop out, and a reload leaves it out — the line is read once, at start.
- **...or the modes the last session ended in.** `[tiling] remember = true`
  writes each desktop's mode to `~/.local/state/fwm/modes` as it changes and
  starts from there, so a desktop switched over by hand outlives the session.
  Turning it off and reloading goes back to what `default` says.
- **A split can be uneven.** `[tiling] split_ratio` is the share of a split the
  window that was already there keeps — `0.6` is a master area with none of the
  machinery of one. Mirrored for a window dropped in front of it, so the older
  window keeps its share whichever edge the newcomer came in on, and splits
  already made keep the ratio they were made with.
- **A split can be forced.** `[tiling] force_split = "vertical" | "horizontal"`
  stops reading the longer side of the slot and always cuts the same way. A
  window dropped on a named edge still goes where it was dropped.
- **`[tiling] smart_gaps`**: a desktop holding one window loses its outer gap —
  the window is the whole layout, and the margin was holding it away from
  nothing. The gap returns with the second window.
- **`fwm -debug` shows both halves again.** Its first desktop is meant to hold a
  window under physics and its second a tile; a config that tiles every desktop
  by default (or `remember` carrying the last session's choices over) had tiled
  the first one too, so the scratch session came up tiled on both. It now sets
  both modes itself. A `-debug` run also no longer writes the modes-and-settings
  state file, which belongs to the session it was started beside.

### Wallpaper

- **Each monitor can carry its own wallpaper.** A `[[wallpaper]]` layer takes an
  `output = "HDMI-A-1"`, and a monitor named anywhere in the array shows only
  the layers naming it; every other monitor shows only the layers that name
  none. That is what makes the usual shape — one wallpaper everywhere and one
  screen that differs — two blocks, without the general image sitting underneath
  the special one. A layer naming an unplugged screen is simply not built and
  comes back with it, and each monitor pans its own image with its own camera.
- **The picker sets the screen you are on.** With one monitor nothing changes:
  the pick is the desktop's, remembered in `~/.local/state/fwm/wallpaper`. With
  two, it lands on the active monitor and is remembered per monitor in
  `~/.local/state/fwm/wallpaper.<OUTPUT>`, so the other screen keeps its image
  and a dark screen still gets its own back. Only the monitors that actually
  show the changed image are rebuilt, so a pick no longer re-decodes a wallpaper
  the other screen was already showing.
- **With `color_source = "wallpaper"`, each monitor is drawn in the colours of
  its own image.** One palette for the session meant a red accent lifted from a
  bright picture standing on top of the dark screen next door, and it never even
  moved: the palette was picked before any monitor was up — so it came from
  whichever `[[wallpaper]]` block happened to be written first — and only a
  reload or the picker changed it again. A palette is now derived per monitor,
  and each screen's tray, window frames and panels are drawn from the image that
  screen is showing. What is not tied to a screen — the launcher, the ring, the
  OSD, `fwmctl theme` and the `palette` event — follows the monitor the pointer
  is on, which is where those open.

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

### Menus

- **A right-click menu stays on the screen it was opened on, and out from under
  the bar.** Three separate ways it did not: it was fitted to the whole
  monitor, so a menu near the top sat under a tray that draws above it and ate
  the clicks; a submenu was measured from the menu above it instead of from the
  window, and slid further off the edge the deeper it went; and a menu the
  client moved after opening it — which is how Chromium and Firefox walk a
  submenu along — was never fitted again and was free to walk off the screen.
- **A bar's own menu opens at the icon that opened it**, not in the corner of
  the primary screen: the tree those popups live in follows the bar now.

### Resizing and dragging

- **A resize ends smoothly instead of on a jolt.** A client is not obliged to
  take the size it is offered, and a terminal answers in whole character cells
  — so the last cell of a drag used to arrive in a single frame, the one where
  the rubber came down. That difference is now ridden out: the window is eased
  from the size in the hand to the size the client took. A new gesture on the
  window ends the settle where it stands, and the tiling divider settles the
  same way.
- **An edge dragged by hand goes as far as the hand does**, and a window being
  resized stays on the screen it is being resized on.
- **A window held under an effect is drawn as the window, not as the client's
  buffer.** A client-decorated toplevel paints its shadow margins into the same
  surface and calls a sub-rectangle of it the window; handing that texture
  straight to the wobble or the spin stretched the margins over the window's
  box and looked like one client's bug (Discord's, since an undecorated
  terminal does not do it).
- **A window dragged by its own titlebar does not leave the client holding the
  button.** If the press went to the client and the compositor then took the
  drag over, the release went nowhere and the seat's button count stayed above
  zero. The release is now always sent; one the client is not owed is ignored
  by the seat.

### Crashes

- **Closing several windows at once no longer takes the session with it.** A
  client's last commit before it destroys a window was run like any other, and
  one of the things that runs creates a physics body when it cannot find one —
  so the window that had just been unmapped got a new one, became a focus
  candidate again, and being focused configured a surface wlroots had already
  torn down. That is an assertion inside wlroots, which is the display server
  gone. Three terminals closing together was enough. The use-after-free hiding
  behind it — the focus left pointing at a window freed without ever being
  mapped — is closed too, and so is the same abort reached from a popup a
  client repositions before it has committed.
- **An allocation that fails costs a device, not the session.** A keyboard
  arriving on a failed `calloc`, and the launcher's directory scan growing its
  list with an unchecked `realloc`, both wrote through NULL.

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
