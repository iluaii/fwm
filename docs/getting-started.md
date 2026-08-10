# Getting started

## Build and install

```sh
git clone https://github.com/iluaii/fwm
cd fwm
./install.sh
```

`install.sh` installs the dependencies for pacman, apt, dnf or xbps, builds
Box2D v3 from source where the distribution ships something older (Debian's
`libbox2d-dev` is 2.4, which is a different library), builds fwm and installs it
under `/usr/local` — `PREFIX=…` moves that. `./install.sh update` pulls and
rebuilds; `./install.sh uninstall` removes the installed files and keeps your
config.

Your `config.toml` is never overwritten by an install or an update, which does
mean it stops mentioning options added since you wrote it. `./install.sh update
config` merges: you get the current example — its comments, its ordering, every
option that is new — with your values put back into it, your own binds kept at
the end of the section they were in, and anything the example has never heard of
(a `[mode.*]` of yours, a `[[rule]]`) kept at the end of the file. It offers to
back the old one up first, in the `config.toml.bak`, `.bak2`, `.bak3` convention,
and never replaces an existing backup without being told to.

To build by hand:

```sh
cmake -S . -B build
cmake --build build -j$(nproc)
```

The dependency list, and which of them are optional, is in the
[README](../README.md#requirements). Two are worth repeating here:

- **wlroots 0.20** exactly. The API breaks between minor versions, so the build
  refuses 0.21 loudly instead of failing halfway with confusing errors.
- **libpipewire and/or libpulse-simple** are optional. Without them everything
  works except the audio visualiser and the collision sound.

## Starting a session

From a TTY:

```sh
fwm
```

Better, from your display manager or your shell profile:

```sh
fwm-session
```

`fwm-session` restarts fwm if it dies and relaunches the applications recorded
in `~/.local/state/fwm/session` on the desktops they were on. A compositor is
the display server, so a crash otherwise costs the whole working layout; this
turns that into a few seconds. Applications are *relaunched*, not resumed —
anything unsaved inside them is still gone. Leaving deliberately
(`Super+Shift+Escape`) ends the session for good, and three failures inside a
minute give up rather than spinning in front of you.

Set `FWM_BINARY=/path/to/build/fwm` to have the session script run a build of
your own.

## The first five minutes

Nothing needs configuring to start. `~/.config/fwm/config.toml` does not have to
exist — without it fwm uses built-in defaults and built-in keybindings, and says
so in the tray. Copy [`config.toml.example`](../config.toml.example) over when
you want to change something.

| Do this | To see |
|---|---|
| `Super+Return` | a terminal (whatever `$TERMINAL` says, else the first one installed) |
| Drag a window with `Super` held, and let go while moving | it flies, hits a wall, bounces, glides to a stop |
| `Super+G` | gravity: zero-g → a lick of it → earth. Windows fall and stack |
| `Super+L`, `Super+H` | pan across the strip of desktops |
| `Super+A` | the desktop strip; then `z` for the wider view, drag windows between cards |
| `Super+T` | tiling on this desktop. `Super+T` again and the same windows are physical objects |
| `Super+Space` | the launcher |
| `Super+Shift+?` | the key hints |
| Click the pill left of the clock | the modes menu: layout, gravity, mass, sound, visualiser, ring |
| `Super+Shift+Escape` | leave |

Every one of those is a rebindable action — see
[Keybindings and actions](keybindings.md).

## Where fwm keeps things

| Path | What |
|---|---|
| `~/.config/fwm/config.toml` | your configuration; fwm never writes to it |
| `~/.local/state/fwm/session` | the applications to relaunch after a crash |
| `~/.local/state/fwm/wallpaper` | the wallpaper the picker last chose |
| `~/.local/state/fwm/modes` | what the modes menu was left set to |
| `~/.local/state/fwm/crash.log` | why the last run died, written by `fwm-session` |

`XDG_STATE_HOME` and `XDG_CONFIG_HOME` are respected. The three state files are
fwm's own to rewrite, which is exactly why the choices made in the UI live there
instead of being edited into your config file.

## Trying a change without ending your session

fwm can run nested inside your current session, which is how it is developed:

```sh
./dev.sh          # rebuild and run in a window
./dev.sh -n 2     # ...with two terminals already open
./dev.sh -g 1     # ...with gravity on
```

The inner compositor can be restarted as often as you like while the session you
are working in never notices. `./dev.sh -h` prints the rest of its flags — a
screenshot-and-quit mode, a starting desktop, the visualiser on synthetic audio.
Keybinds are the one thing a nested run cannot test: the outer compositor claims
the `Super` combinations first. See
[Troubleshooting](troubleshooting.md#nested-runs) for the traps.
