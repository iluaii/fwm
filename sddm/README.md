# The fwm login screen

An SDDM theme in fwm's style: the wallpaper fwm was showing, its palette, the
tray's pills along the top, and each user as a card of frosted glass that is
a body.

- **Throw it.** Grab a card anywhere but the password field and throw it: it
  bounces off the edges of the screen and off the other cards. A spring
  always brings it back to where you type within a second, and the password
  field keeps the keys the whole time.
- **It drops in** from above when the screen comes up.
- **A wrong password** jolts the card down and to one side; it springs back,
  emptied and ready.
- **The right one** flings it up and away while the session starts; the
  other cards fall out of the way.
- **The power pills** (suspend, reboot, power off) want a second press within
  three seconds.

Only the primary monitor gets the cards and the pills; the others show their
own wallpaper.

## Install

```sh
sudo sddm/install.sh       # theme, /var/lib/fwm-sddm, sddm.conf.d, fwm-sddm-sync
fwm-sddm-sync              # from inside fwm: wallpapers and palette, no root
```

The greeter runs as its own user and cannot read your home, so
`fwm-sddm-sync` copies the wallpapers (first frame, for a video), the matugen
palette and fwm's `tray_opacity` into `/var/lib/fwm-sddm`, which
`install.sh` made yours. Run it again whenever the wallpaper changes — from
`[startup] exec` and from whatever already watches the wallpaper picker.

## The display server

SDDM draws its greeter on an X server by default, and fwm does not need one,
so a machine running only fwm may have none. Either install one for the
greeter alone (the session itself stays Wayland):

```sh
sudo xbps-install xorg-minimal
```

or give SDDM a Wayland compositor for the greeter instead, in
`/etc/sddm.conf.d/20-display.conf`:

```ini
[General]
DisplayServer=wayland

[Wayland]
CompositorCommand=weston --shell=kiosk
```

## Trying it without logging out

```sh
sddm-greeter-qt6 --test-mode --theme sddm/fwm
```

Test mode cannot log in or power off: the power pills stay hidden and the
password goes nowhere.
