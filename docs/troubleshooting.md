# Troubleshooting

Contents: [it will not start](#it-will-not-start) ·
[the config did nothing](#the-config-did-nothing) · [it died](#it-died) ·
[no sound](#no-sound) · [windows behave oddly](#windows-behave-oddly) ·
[monitors](#monitors) · [nested runs](#nested-runs) ·
[debug switches](#debug-switches) · [tests](#tests)

## It will not start

**"wlroots-0.20 was built without Xwayland support"** — a configure-time failure.
fwm needs Xwayland at runtime for X11 clients; install `xorg-xwayland` and a
wlroots built with it.

**wlroots version errors** — fwm pins wlroots 0.20 exactly. The API breaks between
minor versions, so the build refuses 0.21 rather than failing halfway with
confusing errors.

**"XDG_RUNTIME_DIR is unset"** — wlroots needs it for the socket. Your login setup
should provide it; from a bare TTY, a session manager or `elogind`/`seatd` usually
does.

**Nothing on screen, no error** — run with `FWM_DEBUG=1` and read the log. On a
TTY, redirect it: `FWM_DEBUG=1 fwm > /tmp/fwm.log 2>&1`.

## The config did nothing

Look at the tray: a ⚠ pill means fwm read your file and did not like parts of it.
Click it (or bind `show_errors`) for the list — it names the section and the key.

Things that are easy to get wrong:

- **A missing quote.** TOML needs `"super+q" = "killclient"`, quotes on both
  sides. A `[binds]` section where every line is broken falls back to the built-in
  binds, which is why your custom keys may all be gone at once.
- **An unknown action.** Reported at load time. The complete list is in
  [Keybindings](keybindings.md#actions).
- **A `[[rule]]` with no matcher**, or a `[physics.<name>]` profile that names no
  desktop: both are reported as useless rather than silently ignored.
- **`tick_rate`** is read once at startup — a reload will not change it.
- **You are looking at a `fwmctl set` override.** `Super+Shift+R` (or
  `fwmctl reload`) discards every override and goes back to the file.
- **The UI remembered something.** The mass mode and the collision sound come from
  `~/.local/state/fwm/modes`, which wins over the config because it was written by
  a click. Delete the file to go back.
- **The wallpaper is not the one in the config.** Same thing:
  `~/.local/state/fwm/wallpaper` holds the picker's choice.

## It died

`fwm-session` writes `~/.local/state/fwm/crash.log` and brings fwm back, with the
applications from `~/.local/state/fwm/session` relaunched on the desktops they
were on. Three failures inside a minute stop the loop rather than flapping in
front of you.

Windows that came back on the wrong desktop, or did not come back at all: an
application whose window belongs to a different process than the one launched
(some browsers, Electron apps) cannot be matched, and at most 64 are recorded. Set
`[session] restore = "never"` to switch the whole thing off.

## No sound

Neither the visualiser nor the collision knock will ever start a sound server —
libpulse would happily autospawn one, and a second daemon fighting the first for
the card costs you your audio and not just the bars. So:

1. **Is a server running?** `pactl info` or `pw-cli info 0`. If your daemon is
   autospawned by the first client that wants sound, there is none at login: fwm
   keeps looking and the bars appear when something starts playing.
2. **Was fwm built with the libraries?** The log says
   `audio: no sound server running` when there is none, and CMake prints
   `[cava] capture backends: …` at configure time. No line at all means it was
   built without both.
3. **Collision sound specifically needs libpulse-simple.** PipeWire boxes serve
   that through pipewire-pulse. Without it the log says
   `sound: built without libpulse-simple`.
4. **`[sound] path`** — if the WAV cannot be read, the log says why
   (`not 16-bit PCM or 32-bit float`, `no data chunk`, …) and the built-in click
   plays instead, which sounds exactly like a working feature.

## Windows behave oddly

**A window will not move.** It is pinned (`Super+P`), fullscreen, or the desktop
is in tiling or floating mode — in all of those, physics is not allowed to move
it. A `[[rule]]` may have pinned it; `fwmctl windows` shows `pinned` and
`nocollide` per window, which is the only way to see whether a rule took hold.

**Windows pass through each other.** Either `nocollide` is set on one of them, or
you are dragging the mouse faster than about three times `max_throw_speed` — see
[Physics](physics.md#speed) for exactly what that limit is and why it exists.

**Windows will not fall.** fwm starts in zero-g whatever `[physics] gravity` says;
`Super+G` cycles it on. The tray's gravity icon is lit when it is.

**Windows drift forever.** That is zero-g with `friction` close to 1. Lower
`[physics] friction`, or turn gravity on.

**A window sits half off the screen.** A window larger than the play area cannot
fit in it; the walls hold what they can. Resize or fullscreen it.

**Everything is jittery under a spin or a drag.** `[effects] live = 0` puts
windows under an effect back on a periodic still frame, which is the better trade
on slower hardware. `FWM_DEBUG_EFFECTS=1` prints frame timings.

## Screen sharing

Discord, Zoom, OBS and browsers do not capture a Wayland screen themselves. They
ask a *portal* — `xdg-desktop-portal` — which hands the request to a backend that
knows the compositor. fwm implements both capture protocols — `wlr-screencopy-v1`,
which every backend on disk today still speaks, and `ext-image-copy-capture-v1`,
which is what they are moving to now that the wlr one is deprecated — so a
backend of either generation finds what it needs. The one to install today is
**`xdg-desktop-portal-wlr`**. Install it alongside `pipewire`, which
carries the frames:

```sh
# Void
sudo xbps-install -S xdg-desktop-portal-wlr
# Arch
sudo pacman -S xdg-desktop-portal-wlr
# Debian/Ubuntu, Fedora
sudo apt install xdg-desktop-portal-wlr    # sudo dnf install xdg-desktop-portal-wlr
```

Then log out and back in. fwm sets `XDG_CURRENT_DESKTOP=fwm:wlroots` and pushes
its environment to the D-Bus session bus at startup, and `install.sh` puts a
`fwm-portals.conf` in place that routes capture to the wlr backend — the portal
needs all three, and it only picks them up on a fresh session.

**The share dialog lists nothing — no monitor to pick.** If the backend is
installed, this is almost always its *chooser*. xdg-desktop-portal-wlr asks
which source to share before it captures anything, and it asks by running an
external menu — it tries `slurp`, `wmenu`, `wofi`, `rofi`, `bemenu`, `mew` and
`fuzzel` in turn. With none of them installed it exhausts the list, logs
`no output found`, and closes the session, which arrives at the application as
an empty list rather than as an error. It does this even with a single monitor.

Either install a chooser (`slurp` is the usual one), or skip the question in
`~/.config/xdg-desktop-portal-wlr/config`:

```ini
[screencast]
chooser_type=none
output_name=DVI-D-1   # a name from `fwmctl outputs`
```

`none` shares `output_name` without asking. With two monitors you want the
chooser instead, so that you can pick between them.

**The shared image is black, or nothing is offered at all.** Check that the
backend is installed and reachable:

```sh
busctl --user list | grep portal      # xdg-desktop-portal + a backend
echo "$XDG_CURRENT_DESKTOP"           # must contain fwm or wlroots
```

If `XDG_CURRENT_DESKTOP` is empty inside a running session, the compositor was
started in a way that bypassed its own startup — or the D-Bus bus predates it and
never got the update. `dbus-update-activation-environment WAYLAND_DISPLAY
XDG_CURRENT_DESKTOP` repairs it for the session in progress.

**`fwm: no D-Bus session bus` in the log, and no portal anywhere.** There is no
session bus to publish anything to. A display manager gives you one; starting
fwm by hand from a bare VT on a system without systemd's user manager usually
does not. Wrap the session in one:

```sh
exec dbus-run-session fwm
```

fwm checks for the bus and skips the publish when there is none, rather than
letting D-Bus go looking for one on its own — left to itself it asks the X
server for the address, and the X server it would ask is fwm's own Xwayland,
which cannot answer a compositor that is still starting up.

**The picker offers windows and only the whole screen works.** xdg-desktop-portal-wlr
captures outputs, not individual windows; fwm serves the output capture sources
of `ext-image-copy-capture` but not the per-toplevel ones, which want
`ext-foreign-toplevel-list` first. Share the screen and put the window on a
desktop of its own.

**Flatpak applications still see nothing.** They need the portal *and* PipeWire
across the sandbox boundary. `flatpak info --show-permissions com.discordapp.Discord`
should show a `pipewire` socket; if it does not, grant it:
`flatpak override --user --socket=pipewire com.discordapp.Discord`.

**A Flatpak bar, clipboard manager or screen recorder does nothing at all.** That
is deliberate, and it is not a Flatpak bug. A sandboxed app announces itself to
fwm through `security-context-v1`, and fwm answers by leaving the privileged
protocols out of the registry it sends that client: the clipboard read behind
everyone's back, screen capture, layer-shell, the window list, the workspaces,
global shortcuts, the backlight and the gamma ramps. The app sees no such
global, so waybar-in-a-flatpak has nowhere to put its bar and cliphist-in-a-
flatpak sees no selection.

Nothing started from the session itself is affected — a bar installed from your
distro's packages is an ordinary client and gets everything. Run the tool
outside the sandbox, or, for capture, let it ask the portal like Discord does.

## Monitors

`fwmctl outputs` lists what fwm sees, including every mode each screen offers, and
`fwmctl output <name> …` changes one live — that is the whole of display
configuration, and it applies through the same code `[[output]]` uses.

**A screen went dark and the pointer is on it.** `outputs_on` turns everything
back on and needs no pointer and no visible monitor. `output_off` refuses to turn
off the last lit screen.

**Names** are what fwm logs at startup (`output eDP-1: 1920x1080, scale 1.00`) and
what `[[output]] name` expects.

## The screen goes dark, or refuses to

`[idle] blank_after` is the timer, ten minutes out of the box, and `0` turns it
off entirely. `fwmctl set idle.blank_after=0` does it for the session without
touching the file — and if the screens are already dark when you do, they come
back on the spot.

**It blanked during a film.** The player has to say so: a client that asks for
an idle inhibitor freezes the timer where it stands, and one that does not is
indistinguishable from a still window. mpv needs `--stop-screensaver` (its
default) and browsers only inhibit while a video is actually playing fullscreen.
The inhibitor also has to be on a desktop you are looking at — a video paused on
desktop 7 keeps nothing awake, by design.

**It never blanks.** Something is holding an inhibitor, or `blank_after` is 0.
`fwmctl config | grep idle` shows the timers as they are now, including anything
`fwmctl set` changed.

**A screen stayed dark after it came back.** Only the screens fwm put out are
lit again by input — one turned off by the lid, by `output_off` or by an
external tool is left where it was on purpose. `outputs_on` brings everything
back.

**It locked and nothing appeared.** `[idle] lock` names a command, and fwm does
not draw a lock screen itself: with an empty `lock` nothing locks, and with a
locker that is not installed the session log has the shell's complaint about it.

**swayidle instead.** It still works, on its own timers, and can do more than two
numbers can (dim first, blank later, a different timeout on battery). Set
`blank_after = 0` and `lock_after = 0` so the two are not both counting.

## Nested runs

fwm runs inside your session, which is how it is developed:

```sh
./dev.sh -h        # what it can do
./dev.sh -n 2 -g 1 # two terminals, gravity on
```

Three traps, all learned the hard way:

- **`FWM_SOCKET` targets whatever it points at.** A `fwmctl` call from your shell
  goes to the *outer* session unless you set it:
  `FWM_SOCKET=/run/user/1000/fwm-wayland-1.sock fwmctl state`. The socket name is
  in the inner instance's log (`Wayland socket: wayland-1`).
- **Session restore can fork fwm inside fwm.** A nested run reads the same state
  file as your real session, so it may relaunch what your session recorded. Point
  `XDG_STATE_HOME` somewhere temporary for anything experimental.
- **Keybinds cannot be tested nested.** The outer compositor claims the `Super`
  combinations first. Run from a bare TTY for those, or use
  `FWM_TEST_ACTION=<action>` to fire one action a second after startup.

## Debug switches

Environment variables, all read at startup:

| Variable | Does |
|---|---|
| `FWM_DEBUG=1` | wlroots debug logging |
| `FWM_DEBUG_EFFECTS=1` | frame timings for spin/wobble, once a second while one is running |
| `FWM_THEME_DEBUG=1` | what the palette was derived from |
| `FWM_TEST_ACTION=<action>` | dispatch one action ~1.5s after startup |
| `FWM_TEST_GRAVITY=<scale>` | start with gravity on |
| `FWM_TEST_CAMERA=<n>` | start parked on desktop *n* |
| `FWM_TEST_CAVA=1`, `FWM_TEST_CAVA_MODE=<mode>` | the visualiser on synthetic audio, for a nested run with nothing playing |
| `FWM_TEST_ORBIT`, `FWM_TEST_ORBIT_DIST` | open the desktop strip in the wider view |
| `FWM_TEST_ORRERY=1` | open the strip straight into the orrery (its own key cannot be injected) |
| `FWM_OPEN_PICKER=1`, `FWM_SHOW_HINTS=1` | open the picker / hints at startup |

They exist because a compositor cannot be driven by a test harness: there is no
way to inject a keypress or move the pointer, so the code paths a bind reaches
have to be reachable some other way. `dev.sh` wraps most of them in flags.

## Tests

```sh
cmake -S . -B build-test -DFWM_TESTS=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

Ten suites, none of which need a compositor, a display or a sound card. What each
covers is in [Architecture](architecture.md#tests).
