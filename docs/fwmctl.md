# fwmctl and the IPC

fwm listens on a unix socket and speaks one line of text per request, replying
with one line of JSON. `fwmctl` is a thin client for it that links nothing at all,
so it keeps working whatever happens to the compositor's dependencies.

```
$XDG_RUNTIME_DIR/fwm-$WAYLAND_DISPLAY.sock     # e.g. /run/user/1000/fwm-wayland-0.sock
```

`fwmctl` finds it through `$FWM_SOCKET` if set, else from `$WAYLAND_DISPLAY`. fwm
exports `FWM_SOCKET` to its children, so a script started from inside the session
needs no configuration. **A nested run is a different socket**: point `FWM_SOCKET`
at it explicitly or you will be talking to the outer session.

## Commands

| Command | Does |
|---|---|
| `fwmctl state` | compositor state: desktop, camera, per-monitor desktops, gravity, per-desktop modes, focused title |
| `fwmctl windows` | every window: id, title, app_id, geometry, desktop, focus, pinned, nocollide, xwayland |
| `fwmctl outputs` | monitors, their current mode and every mode they offer |
| `fwmctl memory` | fwm's own memory, split into heap, mapped libraries and client buffers |
| `fwmctl output <name> k=v …` | change one monitor |
| `fwmctl config` | every settable option with its value, range and one-line help |
| `fwmctl theme` | the UI palette in hex, where it came from, and the wallpaper it was derived from |
| `fwmctl get <name>` | read one option |
| `fwmctl set <name> <value>` | change one option, this session only |
| `fwmctl set <a>=<v> <b>=<v> …` | several at once, applied together or not at all |
| `fwmctl save <name> [value]` | ...and remember it across reloads and restarts |
| `fwmctl save --all` | remember everything this session has changed |
| `fwmctl unsave <name>` / `--all` | forget it, and put the configured value back now |
| `fwmctl saved` | what is remembered, and what each of those is worth right now |
| `fwmctl window <id> k=v …` | change one window: desktop, position, pin, collision, focus, close |
| `fwmctl dispatch <action>` | run any keybind action |
| `fwmctl urgent <d> [on\|off]` | light desktop `d`'s number red until you go there |
| `fwmctl reload` | reload `config.toml`, discarding every `set` |
| `fwmctl version` | the running fwm's release, which binary is answering (path, mtime, pid) and the IPC version |
| `fwmctl subscribe [events]` | stream events as JSON lines until killed |

`fwmctl -h` prints the same list with the `output` keys.

## Reading state

```console
$ fwmctl state
{"ok":true,"desktop":2,"camera_x":3840,"windows":4,"screen_width":1920,
 "screen_height":1080,"outputs":[{"name":"eDP-1","desktop":2,...}],
 "gravity":1.000,"locked":false,"mode":"physics",
 "modes":["physics","tiling",...],"urgent":[5],"focused":"~/fwm"}
```

`state` is the only place that answers "which desktop am I on" for every monitor
at once — with independent screens the question has more than one answer — and the
only place that reports each desktop's mode.

## What fwm itself is using

```console
$ fwmctl memory
{"ok":true,"rss_mb":141.5,"anon_mb":31.6,"file_mb":83.9,"shmem_mb":22.7}
```

`top` shows fwm as one number — RES, well past a hundred megabytes — and it reads
like a compositor with a leak. The split says otherwise, and it is the reason this
command exists:

- **`anon_mb`** is fwm's own heap and stacks. This is the compositor's real
  footprint, and the only figure a bug here can inflate.
- **`file_mb`** is mapped executables and libraries — mesa, ffmpeg, pango, cairo.
  Those pages are shared with every other process that maps them, so the same
  memory is counted again inside every one of their totals.
- **`shmem_mb`** is client buffers mapped for compositing. Charged to fwm *and* to
  the client that owns them; it tracks how many windows are open and how big they
  are, not anything fwm holds onto.

Only `rss_mb` is the sum, and it is the one number that answers no question on its
own. Watch `anon_mb` across a session — that is where a leak in fwm would show.

## Changing settings live

```console
$ fwmctl get physics.gravity
{"ok":true,"name":"physics.gravity","value":"981.000"}

$ fwmctl set physics.gravity 300
{"ok":true,"name":"physics.gravity","value":"300.000"}

$ fwmctl config | jq -r '.options[] | "\(.name) = \(.value)"' | head -3
physics.friction = 0.985
physics.mass = 0
physics.mass_ram_ref = 300.000
```

Everything scalar is settable: the `[physics]`, `[tiling]`, `[camera]`,
`[decor]`, `[effects]`, `[input]`, `[gestures]`, `[cava]` and `[sound]` numbers.
`fwmctl config` is generated from the same table the setter uses, so it documents
itself rather than needing this page kept in sync.

Not settable, on purpose:

- **Arrays** — `[binds]`, `[[wallpaper]]`, `[[rule]]`, `[mode.*]`. They are not
  scalars; reloading is the right way to change them.
- **`physics.tick_rate`** — the tick timer is armed once at startup, so accepting
  a new value would report success and change nothing.
- **`cava.bars`** — it rebuilds every scene rect and every kinematic body.
- **`sound.path`** — the sample is loaded once; a reload picks up a new one.
- **Strings** (`icon_theme`, the `kbd_*` keys) — re-read only by a full reload.

**`set` is runtime-only.** `config.toml` is never rewritten; `fwmctl reload` (or
`Super+Shift+R`) puts everything back to what the file says. Enumerated options
are numbers here because the table is typed: `physics.mass` is 0 for `size` and 1
for `ram`, `cava.mode` is 0–3.

Several settings in one command are checked **before any of them is applied**, so
a typo in the third pair leaves the first two alone — and they land in a single
re-apply, which is what keeps a script changing three related knobs from
producing a frame of the half-changed world:

```console
$ fwmctl set sun.azimuth=120 sun.elevation=25 sun.length=40
{"ok":true,"set":[{"name":"sun.azimuth","value":"120.000"}, …]}

$ fwmctl set sun.blur=4 sun.nonsense=1
{"ok":false,"error":"unknown option \"sun.nonsense\" (try: config)"}   # blur unchanged
```

## Keeping what you found — `save`

`set` is the right default and, on its own, a dead end: everything found by
trying it is lost at the next reload unless you go and edit the file by hand.
`save` writes it down instead — not into `config.toml`, which stays yours, but
into an overlay fwm owns and applies over the config after every load:

```
~/.local/state/fwm/settings      # one `name = value` per line
```

```console
$ fwmctl set sun.blur 18          # try it
$ fwmctl save sun.blur            # keep it — a bare name saves what it is worth now
{"ok":true,"name":"sun.blur","value":"18.000","saved":[…]}

$ fwmctl save sun.opacity 0.6     # or set and keep in one go
$ fwmctl save --all               # everything this session has changed
{"ok":true,"count":3}

$ fwmctl saved
{"ok":true,"saved":[{"name":"sun.blur","value":"18.000","live":"4.000"}]}

$ fwmctl unsave sun.blur          # forget it, and the configured value is back now
$ fwmctl unsave --all
```

`saved` reports both what is written down and what the option is worth **now**;
they differ whenever a later `set` has moved one, which is exactly the state
somebody asking the question is trying to see. `unsave` does not need a reload —
a reload would also discard every other `set` the session is standing on, which
is a heavy price for taking back one line.

Three things worth knowing:

- Editing `config.toml` still works and still wins for anything the overlay does
  not name. The overlay is a diff, not a copy.
- The modes menu's two switches (`physics.mass`, `sound.collisions`) are applied
  **after** the overlay, so the menu keeps the last word on them. Saving one is
  legal and a later click will overrule it — the pill on screen shows what the
  menu chose, and a saved value that quietly beat it would make the pill a liar.
- A name this fwm does not have is skipped in silence, so a file written by a
  newer build never stops an older one from starting. A name it *does* have with
  a value it will not accept is reported through the tray's ⚠ pill, like any
  other config problem.

## One window

`dispatch` acts on whatever has the focus, because that is what a keybind means.
A script has an id out of `windows` and something it wants done to that window:

```console
$ fwmctl window 7 desktop=4 pin=on
{"ok":true,"window":{"id":7,"title":"~/fwm", …,"desktop":4,"pinned":true}}
```

| Key | Does |
|---|---|
| `desktop=0-9` | send it there |
| `x=`, `y=` | put it down, in the same coordinates `windows` reports |
| `pin=on\|off\|toggle` | hold it still |
| `nocollide=on\|off\|toggle` | let everything pass through it |
| `focus=on` | give it the keyboard |
| `close=on` | ask it to close — it may decline and put up a save dialog |

Parsed in full before anything is applied, like `output`, and answered with the
window as it now stands. A window is *dropped* where you put it rather than
thrown: it arrives with no velocity, whatever it had before. Position is refused
while the window's desktop is tiling, because there the geometry belongs to the
layout, which would put it straight back — saying so beats appearing to work for
one frame.

## Monitors

```console
$ fwmctl outputs | jq -r '.outputs[].name'
eDP-1
HDMI-A-1

$ fwmctl output HDMI-A-1 mode=2560x1440@144 scale=1.25 position=0,0 desktop=3
```

Keys: `mode=WxH[@Hz]`, `scale=`, `transform=normal|90|180|270|flipped|flipped-90|…`,
`position=X,Y`, `desktop=0-9`, `enabled=on|off`. Every token is parsed before any
of them is applied — a typo in the third setting must not leave a screen halfway
through the other two, which matters most here because a monitor mid-change may be
showing nothing readable. The last lit screen cannot be turned off.

Like `set`, this is for the session: `[[output]]` in the config file has the last
word on reload.

## The palette

```console
$ fwmctl theme
{"ok":true,"source":"wallpaper","wallpaper":"/home/me/wall.png","generation":6,
 "colors":{"pill":"#171a16","sel":"#292e2a","text":"#e8ebf0","muted":"#8a9398",
           "dim":"#525766","accent":"#dc781e",
           "border_active":"#dc781e","border_inactive":"#544333"}}
```

The colours the tray, the launcher, the tab bars and the window borders are
drawn with — the same `FwmTheme` the overlays read, not a second copy that can
drift from it. `source` is `config` or `wallpaper`; with `wallpaper` the palette
was [derived from the image](configuration.md#decor) and `wallpaper` is the path
it came from.

Hex, in the form `config.toml` writes a colour: `#RRGGBB`, or `#RRGGBBAA` when
it carries alpha, so a value can be read here and pasted back into the file
unchanged. Internally the borders are premultiplied; the alpha is divided back
out before printing, so what you get is the colour that was written and not the
colour as it happens to be blended.

The `palette` event fires when any of that changes — a new wallpaper, a reload,
a `set` that reached `[decor]`. It carries the whole palette, so a subscriber
never has to ask a second time, and it is emitted by comparing the built theme
against the one last announced: a knob dragged through a range rebuilds the
theme on every step and sends nothing, because nothing about the colours moved.

That is the hook for a colour generator. fwm dresses its own chrome and stops
there — it does not write your GTK theme, your terminal config or anything else
under `~/.config` — but it will tell you what it chose, and `matugen` or `pywal`
can do the rest:

```sh
#!/bin/sh
# Follow fwm's palette into GTK and everything else. Name this in
# [startup] exec and it runs for the life of the session.
apply() {
    wall=$(printf '%s' "$1" | jq -r '.wallpaper // empty')
    if [ -n "$wall" ]; then
        # The picture beats one colour out of it: a generator building a whole
        # scheme has more to work with.
        matugen image "$wall"
    else
        matugen color hex "$(printf '%s' "$1" | jq -r .colors.accent)"
    fi
}

# The stream only ever reports a *change*, and at login nothing has changed
# yet — so dress what is already on screen before waiting for news.
apply "$(fwmctl theme)"

fwmctl subscribe palette | while read -r ev; do
    # The first line is the subscription's own reply, not a palette.
    printf '%s' "$ev" | jq -e '.event == "palette"' >/dev/null || continue
    apply "$ev"
done
```

`theme` and the event carry the same fields, which is why one function handles
both. With `adw-gtk3` installed and `gtk-application-prefer-dark-theme` set,
that is GTK 3 and libadwaita apps following the wallpaper along with the tray —
the ones already open when the wallpaper changes read the new `gtk.css` at their
next start, not before.

## Notifications, and the red desktop

fwm is not a notification daemon and has no plans to become one: popups, actions
and history are what `dunst` and `mako` already do well, and taking the D-Bus
service over would mean re-implementing all of it inside the compositor. What
those daemons cannot answer is the one thing the compositor knows — **where** the
thing that wants you is. So fwm draws that half, and only that half: a desktop
can be marked urgent, and its number in the tray goes red until you go and look.

```console
$ fwmctl urgent 3
{"ok":true,"desktop":3,"urgent":true}
```

Desktops count from 0, as everywhere else in the IPC — `urgent 3` is the number
you reach with super+4. It goes out on its own the moment that desktop is on a
screen, however you got there: the `view:` binds, a click in the tray, a
three-finger swipe, expo, or a second monitor being plugged in. Nothing has to
clear it, and `fwmctl urgent 3 off` exists only for a script that changed its
mind.

A desktop that is already on a screen is never marked. The reply says so with
`"urgent":false` instead of failing — on a two-monitor setup half the strip is in
front of you at all times, and a red number for something you are looking at is
noise, not a signal. `state` lists the ones that are lit, and the `urgent` event
reports both edges:

```console
$ fwmctl subscribe urgent
{"event":"urgent","desktop":3,"urgent":true}
{"event":"urgent","desktop":3,"urgent":false}
```

Two things raise it without any script at all. An **xdg-activation** request for
a window on a desktop nobody is showing — that is a Wayland app asking for the
keyboard, and `[focus] on_activate` decides what happens: `always` takes the
camera there, while `same_desktop` (the default) and `never` turn it into the red
number instead of dropping it silently. And the **X11 urgency hint**, which is
what Telegram, Thunderbird and every other XWayland client with an inbox still
sets — on X11 a window manager answers it by flashing a taskbar entry; here it is
the same red number. The hint is only ever raised from, never lowered: clients
drop it when their window is activated, which under XWayland it usually is not,
so honouring the falling edge would put the digit out while the message is still
unread.

For everything else — a Wayland app that notifies without asking for focus, a
build that finished, a script of your own — the daemon calls `fwmctl`. In dunst
that is a rule with a `script`, which is handed the notification's own fields:

```sh
#!/bin/sh
# ~/.local/bin/fwm-urgent — from dunstrc:
#
#   [fwm-urgent]
#       appname = "*"
#       script  = fwm-urgent
#
# Whichever window belongs to the app that notified decides which number
# lights up. DUNST_DESKTOP_ENTRY is the desktop-entry hint, which is what
# app_id is; the app name is the fallback for daemons that send neither.
app=$(printf '%s' "${DUNST_DESKTOP_ENTRY:-$DUNST_APP_NAME}" | tr 'A-Z' 'a-z')
[ -n "$app" ] || exit 0

d=$(fwmctl windows | jq --arg app "$app" '
      [.windows[] | select((.app_id | ascii_downcase) | contains($app))]
      | first | .desktop // empty')

# No window: the message came from something with no face on screen, and
# there is no desktop to point at. Say nothing rather than guess.
[ -n "$d" ] && fwmctl urgent "$d"
```

mako reaches the same script through `on-notify=exec`; it hands over no fields,
so a rule there is per-app in the config rather than per-app in the script.

## Events

```console
$ fwmctl subscribe
{"event":"window_open","id":7,"title":"~/fwm","app_id":"foot","desktop":2}
{"event":"window_focus","id":7,"title":"~/fwm","app_id":"foot","desktop":2}
{"event":"window_focus","id":null}
{"event":"desktop","desktop":3}
{"event":"mode","desktop":3,"mode":"tiling"}
{"event":"gravity","gravity":1.000}
{"event":"config_reload"}
{"event":"setting","name":"sun.blur","value":"18.000","saved":true}
{"event":"ui","what":"launcher","open":true}
{"event":"urgent","desktop":5,"urgent":true}
{"event":"palette","source":"wallpaper","wallpaper":"/home/me/wall.png","generation":6,
 "colors":{"pill":"#171a16",...,"accent":"#dc781e"}}
```

Subscribe to everything, or a comma-separated subset:
`window_open`, `window_close`, `window_focus`, `window_title`, `desktop`, `mode`,
`gravity`, `config_reload`, `setting`, `ui`, `palette`, `urgent`. The reply names what was actually subscribed, so a
client can log it rather than assume its request was understood. Subscribing twice
widens the set rather than replacing it.

`ui` covers fwm's own panels — `launcher`, `wallpaper_picker`, `radial`, `mixer`,
`expo` —
because they are not windows and nothing else in the stream mentions them. `open`
carries the state the panel is in after the toggle, so a subscriber that starts
mid-session and misses the opening still reads the closing correctly.

`setting` fires whichever hand moved the knob — the socket, a keybind, the modes
menu — because a subscriber cannot tell them apart and should not have to. It is
emitted by comparing what the options are worth against what was last announced,
rather than from each place that changes one, so a route added later is covered
by construction. `saved` says whether the value is also in the overlay, which is
the difference between a bar redrawing itself and one that can expect the change
to outlive a reload.

`"id":null` on a focus event is not "window 0": focus genuinely goes nowhere when
the last window on a desktop closes, and a subscriber has to be able to tell the
two apart.

Nothing in the IPC can stall the compositor: writes go through an outbound queue,
and a subscriber that stops reading is dropped rather than allowed to hold
compositor state hostage.

## Scripting

`subscribe` plus `dispatch` is the whole plugin story — no shared address space,
so a script that crashes takes nothing with it:

```sh
# Turn gravity off while a video player is open, back on when it closes.
fwmctl subscribe window_open,window_close | while read -r ev; do
    case "$ev" in
        *window_open*mpv*)  fwmctl set physics.gravity 0 ;;
        *window_close*mpv*) fwmctl set physics.gravity 981 ;;
    esac
done
```

```sh
# A waybar/eww module: the focused window and its speed.
fwmctl state | jq -r '"\(.focused) — desktop \(.desktop)"'
```

```sh
# Heavier windows while a browser is up, from a keybind:
#   [binds] "super+m" = "spawn:fwmctl set physics.mass 1"
```
