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
| `fwmctl get <name>` | read one option |
| `fwmctl set <name> <value>` | change one option, this session only |
| `fwmctl dispatch <action>` | run any keybind action |
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
 "modes":["physics","tiling",...],"focused":"~/fwm"}
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

**Writes are runtime-only.** `config.toml` is never rewritten; `fwmctl reload` (or
`Super+Shift+R`) puts everything back to what the file says. Enumerated options
are numbers here because the table is typed: `physics.mass` is 0 for `size` and 1
for `ram`, `cava.mode` is 0–3.

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
```

Subscribe to everything, or a comma-separated subset:
`window_open`, `window_close`, `window_focus`, `window_title`, `desktop`, `mode`,
`gravity`, `config_reload`. The reply names what was actually subscribed, so a
client can log it rather than assume its request was understood. Subscribing twice
widens the set rather than replacing it.

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
