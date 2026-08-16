#!/usr/bin/env python3
r"""Drive the keyboard backlight from fwm's event stream.

fwm says what it is doing on its IPC socket; this turns that into light on an
Ajazz AK820 (1a2c:9605, the wired single-colour version). It links nothing and
lives outside the compositor: if it dies, fwm does not notice.

    tools/fwm-kbd.py                 follow the session until it ends
    tools/fwm-kbd.py --demo          play every reaction once, without fwm
    tools/fwm-kbd.py --off           backlight off, then exit

It is written to be left running. A keyboard that is unplugged, an fwmctl that
dies, a compositor that restarts underneath it — all of those are waited out
rather than treated as failures. It ends when the session does.

Two channels reach the keyboard, both as feature report 0x07, both read out of
the vendor's own driver:

    ff ff <mode> <bright> <speed> <colour> 00      pick one of 20 built-in effects
    23 45 67 89 <decay> <level>                    set the brightness, 0..255

The second is the interesting one. The vendor uses it to push audio loudness
while music plays, which makes it a continuous knob rather than a preset
switch: an event here is a swell of light that decays back to rest, drawn at
50 Hz, over whatever effect is running underneath.

The keyboard is single-colour and its firmware ignores the direction field, so
a reaction is shaped by brightness and by which effect sits under it — never by
which way something moves. `--install-udev` prints the rule that drops root.
"""

import argparse
import fcntl
import signal
import glob
import json
import os
import select
import shutil
import subprocess
import sys
import time

REPORT_ID = 0x07
PAYLOAD = 7
VENDOR_COLLECTION = bytes.fromhex("0601ff")
HID_ID = "0003:00001A2C:00009605"
UDEV_RULE = (
    'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1a2c", ATTRS{idProduct}=="9605", '
    'TAG+="uaccess"'
)

# Effects worth naming, from walking all 20 by hand against the keyboard and
# from the driver's own string table: its mode 1 is "Steady", 2 "Custom mode",
# 3 "Breathing", and the wire value is one lower than the number it shows.
EFFECT_STEADY = 0x00      # just lit, nothing moving
EFFECT_CUSTOM = 0x01      # the pattern Fn+~ records into the keyboard
EFFECT_BREATHE = 0x02
EFFECT_CIRCLE = 0x03      # circles around the centre
EFFECT_WAVE = 0x04        # wave to the right
EFFECT_CENTRE = 0x05      # waves out of the centre, both ways
EFFECT_ON_PRESS = 0x0A    # only what you press, swelling and fading
EFFECT_SNAKE = 0x0F       # a snake down the rows from Esc
EFFECT_DIAGONAL = 0x12    # corner to corner

FPS = 50
REST = 0xC0     # the level the light returns to
# 0xff switches the backlight off rather than driving it hardest, so the swell
# stops short of it. The vendor's own code clamps to 0xff and would hit the
# same wall; its music mode presumably never reaches full scale.
CEILING = 0xF0
PEAK = 0xE0
# The fall is drawn here, frame by frame, so the firmware's own fade is left
# off: with both running, its trailing fade outlives the last packet and drops
# the top row on its own.
DECAY = 0
# The level channel is an override the firmware lets lapse: stop sending and it
# goes back to the effect's own brightness after a moment. So the resting level
# is repeated on a slow beat, which is also what keeps a lost frame from
# showing.
KEEPALIVE = 0.4
DIP = 0.30  # a reaction from a bright rest goes down instead of up, to this much

# What each event does: the effect to sit under it, and the swell to draw.
# `hold` keeps the peak for a moment before the fall, which is what makes a
# reaction legible rather than a twitch.
REACTIONS = {
    "desktop":      {"effect": EFFECT_WAVE,     "peak": PEAK, "hold": 0.10, "fall": 0.45},
    "ui.launcher":  {"effect": EFFECT_CENTRE,   "peak": PEAK, "hold": 0.15, "fall": 0.60},
    "ui.radial":    {"effect": EFFECT_CIRCLE,   "peak": PEAK, "hold": 0.15, "fall": 0.60},
    "ui.wallpaper": {"effect": EFFECT_DIAGONAL, "peak": 0xC8,  "hold": 0.15, "fall": 0.60},
    "ui.expo":      {"effect": EFFECT_CENTRE,   "peak": 0xC8,  "hold": 0.15, "fall": 0.60},
    "window_open":  {"effect": None,            "peak": 0xB4,  "hold": 0.02, "fall": 0.30},
    "window_close": {"effect": None,            "peak": 0x8C,  "hold": 0.02, "fall": 0.30},
    "mode":         {"effect": None,            "peak": 0xA0,  "hold": 0.05, "fall": 0.35},
}

# The effect that idles under each desktop mode.
# Only consulted with --effects. Left alone, the background stays put and a
# reaction is a swell of brightness over it, which is what a steady backlight
# wants: the light answers without ever walking off on its own.
MODE_EFFECTS = {"tiling": EFFECT_STEADY, "physics": EFFECT_WAVE}


def _ioc(direction, typ, nr, size):
    return (direction << 30) | (size << 16) | (ord(typ) << 8) | nr


HIDIOCSFEATURE = _ioc(3, "H", 0x06, 1 + PAYLOAD)


def find_node():
    """Two hidraw nodes belong to this keyboard; take the one with report 0x07."""
    for dev in sorted(glob.glob("/sys/class/hidraw/hidraw*/device")):
        try:
            uevent = open(dev + "/uevent").read()
            rdesc = open(dev + "/report_descriptor", "rb").read()
        except OSError:
            continue
        if HID_ID in uevent and VENDOR_COLLECTION in rdesc:
            return "/dev/" + dev.split("/")[4]
    return None


class Backlight:
    """The keyboard, reopened as needed — unplugging one is not an error."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self.fd = None
        self.effect = None
        self.level = None

    def _open(self):
        if self.fd is not None:
            return True
        node = find_node()
        if node is None:
            return False
        try:
            self.fd = open(node, "rb+", buffering=0)
        except PermissionError:
            sys.exit(f"{node} is not writable — see {sys.argv[0]} --install-udev")
        except OSError:
            return False
        self.effect = self.level = None  # a fresh handle knows nothing
        return True

    def _send(self, payload, log=True):
        if not self._open():
            return
        buf = bytearray([REPORT_ID]) + bytearray(payload)
        try:
            fcntl.ioctl(self.fd, HIDIOCSFEATURE, buf, True)
        except OSError as e:
            if self.verbose:
                print(f"send failed ({e}), will reopen", file=sys.stderr)
            self.fd.close()
            self.fd = None
            return
        if self.verbose and log:
            print("  ->", bytes(payload).hex(" "), file=sys.stderr)

    def set_effect(self, effect, bright=4, speed=3, colour=0):
        if effect is None or effect == self.effect:
            return
        self._send([0xFF, 0xFF, effect, bright, speed & 0xF, colour, 0])
        self.effect = effect
        self.level = None  # the effect resets what the level channel was doing

    def set_level(self, level, decay=DECAY, force=False):
        level = max(0, min(CEILING, int(level)))
        if level == self.level and not force:
            return
        # A keepalive repeat says nothing new; logging it buries everything else.
        self._send([0x23, 0x45, 0x67, 0x89, decay, level, 0], log=level != self.level)
        self.level = level


class Swell:
    """One reaction in flight: hold the peak, then fall back to rest."""

    def __init__(self, peak, hold, fall, rest):
        self.peak = peak
        self.rest = rest
        self.hold_until = time.monotonic() + hold
        self.end = self.hold_until + fall
        self.fall = fall

    def level(self, now):
        if now < self.hold_until:
            return self.peak
        if now >= self.end:
            return None  # done, the caller settles at rest
        share = (self.end - now) / self.fall
        return self.rest + (self.peak - self.rest) * share


class Session:
    """fwm's state, as far as the light cares about it."""

    def __init__(self, light, rest=REST, effects=False, base=EFFECT_STEADY,
                 keepalive=KEEPALIVE):
        self.light = light
        self.rest = rest
        self.effects = effects
        self.base = base
        self.keepalive = keepalive
        self.desktop = 0
        self.mode = "physics"
        self.gravity = 1.0
        self.locked = False
        self.swell = None
        self.refresh_at = 0.0

    def idle_effect(self):
        if not self.effects:
            return self.base
        return MODE_EFFECTS.get(self.mode, EFFECT_WAVE)

    def react(self, key):
        reaction = REACTIONS.get(key)
        if not reaction or self.locked:
            return
        if self.effects:
            self.light.set_effect(reaction["effect"] or self.idle_effect())
        rest = self.rest_level()
        peak = reaction["peak"]
        if peak <= rest + 16:
            peak = int(rest * DIP)  # no headroom above: dip instead
        self.swell = Swell(peak, reaction["hold"], reaction["fall"], rest)

    def rest_level(self):
        """Heavier gravity sits brighter — the one knob with a physical sense."""
        return max(20, min(CEILING, int(self.rest * (0.6 + 0.4 * self.gravity))))

    def draw(self):
        if self.locked:
            self.light.set_level(0)
            return
        now = time.monotonic()
        if self.swell is not None:
            level = self.swell.level(now)
            if level is None:
                self.swell = None
            else:
                self.light.set_level(level)
                return
        rest = self.rest_level()
        if now >= self.refresh_at:
            self.refresh_at = now + self.keepalive
            self.light.set_level(rest, force=True)
            return
        self.light.set_level(rest)

    def handle(self, event):
        kind = event.get("event")
        if kind == "desktop":
            self.desktop = event.get("desktop", self.desktop)
            self.react("desktop")
        elif kind == "mode":
            self.mode = event.get("mode", self.mode)
            if self.effects:
                self.light.set_effect(self.idle_effect())
            self.react("mode")
        elif kind == "gravity":
            self.gravity = event.get("gravity", self.gravity)
        elif kind == "ui":
            # Emitted by fwm for the launcher, wallpaper picker, radial menu
            # and expo. Older builds do not send it; nothing here needs it.
            if event.get("open"):
                self.react("ui." + str(event.get("what", "")))
        elif kind in ("window_open", "window_close"):
            self.react(kind)


def read_state(session):
    """Start from where the session already is, not from a blank slate."""
    try:
        out = subprocess.run(["fwmctl", "state"], capture_output=True,
                             text=True, timeout=5).stdout
        state = json.loads(out)
    except (OSError, ValueError, subprocess.SubprocessError):
        return
    session.desktop = state.get("desktop", 0)
    session.mode = state.get("mode", "physics")
    session.gravity = state.get("gravity", 1.0)
    session.locked = state.get("locked", False)


def socket_path():
    """Where fwm listens, by the same rules fwmctl uses to find it."""
    if "FWM_SOCKET" in os.environ:
        return os.environ["FWM_SOCKET"]
    display = os.environ.get("WAYLAND_DISPLAY")
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if display and runtime:
        return os.path.join(runtime, f"fwm-{display}.sock")
    return None


def follow_once(session, verbose=False):
    """One run of the event stream. Returns when it ends, however it ends."""
    events = "desktop,mode,gravity,window_open,window_close,ui"
    try:
        proc = subprocess.Popen(["fwmctl", "subscribe", events],
                                stdout=subprocess.PIPE, text=True, bufsize=1)
    except OSError:
        return
    frame = 1.0 / FPS
    try:
        while proc.poll() is None:
            ready, _, _ = select.select([proc.stdout], [], [], frame)
            if ready:
                line = proc.stdout.readline()
                if not line:
                    break
                if verbose:
                    print(line.strip(), file=sys.stderr)
                try:
                    session.handle(json.loads(line))
                except ValueError:
                    pass
            session.draw()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()


def follow(session, verbose=False):
    """Follow the session for as long as there is one.

    fwm can restart under a running fwm-session without the socket path
    changing, so a subscribe that ends is a reason to reconnect, not to give
    up. The socket disappearing for good is what ends this."""
    path = socket_path()
    while True:
        if path and not os.path.exists(path):
            # No compositor right now: it may be coming back up.
            for _ in range(30):
                time.sleep(1)
                if os.path.exists(path):
                    break
            else:
                return  # gone for half a minute: the session is over
            continue
        read_state(session)
        follow_once(session, verbose)
        if verbose:
            print("event stream ended, reconnecting", file=sys.stderr)
        time.sleep(1)


def demo(session):
    """Every reaction once, so the mapping can be judged without a session."""
    for key in ("desktop", "ui.launcher", "ui.radial", "ui.wallpaper",
                "window_open", "window_close"):
        print(f"  {key}")
        session.react(key)
        end = time.monotonic() + 1.5
        while time.monotonic() < end:
            session.draw()
            time.sleep(1.0 / FPS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rest", type=int, default=REST,
                    help=f"the level the light idles at, 0..255 (default {REST})")
    ap.add_argument("--keepalive", type=float, default=KEEPALIVE,
                    help="seconds between repeats of the resting level; the "
                         f"override lapses without them (default {KEEPALIVE})")
    ap.add_argument("--effects", action="store_true",
                    help="also switch the background effect per event "
                         "(off by default: the background stays steady)")
    ap.add_argument("--base", type=lambda v: int(v, 16), default=EFFECT_STEADY,
                    help="hex effect to sit under everything (default 00, steady)")
    ap.add_argument("--demo", action="store_true", help="play the reactions once")
    ap.add_argument("--off", action="store_true", help="backlight off, then exit")
    ap.add_argument("--install-udev", action="store_true", help="how to drop root")
    ap.add_argument("-v", "--verbose", action="store_true", help="log both directions")
    args = ap.parse_args()

    if args.install_udev:
        path = "/etc/udev/rules.d/60-fwm-ak820.rules"
        print(f"# {path}\n{UDEV_RULE}\n")
        # The directory is not there on every distribution — Void ships rules
        # in /usr/lib/udev/rules.d and leaves /etc/udev/rules.d to be made.
        print(f"sudo mkdir -p {os.path.dirname(path)}")
        print(f"printf '%s\\n' '{UDEV_RULE}' | sudo tee {path}")
        print("sudo udevadm control --reload-rules && sudo udevadm trigger")
        print("\nThen replug the keyboard: uaccess is granted as a device "
              "appears, not retroactively.")
        return

    if find_node() is None and not (args.off or args.demo):
        print("no AK820 on USB yet — waiting for it", file=sys.stderr)
    elif find_node() is None:
        sys.exit("no AK820 on USB — the vendor report is only there over the cable")

    light = Backlight(args.verbose)
    if args.off:
        # Mode 0 is "Steady", not "off" — darkness is a level of zero.
        light.set_level(0)
        return

    session = Session(light, rest=args.rest, effects=args.effects, base=args.base,
                      keepalive=args.keepalive)
    if args.demo:
        demo(session)
        light.set_effect(session.idle_effect())
        session.draw()
        return

    if not shutil.which("fwmctl"):
        sys.exit("fwmctl is not on PATH — this follows a running fwm session")
    if "FWM_SOCKET" not in os.environ and "WAYLAND_DISPLAY" not in os.environ:
        # Almost always sudo, which drops the session's environment on the way
        # in and leaves fwmctl with no socket to find.
        sys.exit("no FWM_SOCKET or WAYLAND_DISPLAY — start this inside the "
                 "session. Under sudo the environment is gone: install the udev "
                 f"rule ({sys.argv[0]} --install-udev) and run without it, or "
                 "use `sudo -E`.")

    # A signalled exit should put the light back the way a Ctrl-C does.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    signal.signal(signal.SIGHUP, lambda *_: sys.exit(0))

    light.set_effect(session.idle_effect())
    try:
        follow(session, args.verbose)
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        session.swell = None
        session.draw()


if __name__ == "__main__":
    main()
