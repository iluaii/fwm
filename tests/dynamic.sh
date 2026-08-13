#!/bin/sh
# Drive a real fwm through scripted scenarios and fail on anything the
# sanitizers report.
#
#   ./tests/dynamic.sh              run every scenario
#   ./tests/dynamic.sh tiling       run only the named scenario(s)
#   ./tests/dynamic.sh -l           list the scenarios
#   ./tests/dynamic.sh -k           keep the logs instead of deleting them
#   ./tests/dynamic.sh -b build     use another build directory
#
# The compositor runs on the headless backend, so this needs no display of any
# kind and works from a TTY, from inside a session, or in CI. Nothing outside
# the instance it starts is touched.
#
# Every scenario gets its own compositor and shuts it down at the end. That is
# deliberate: teardown is the least exercised path in a compositor, and the
# first bug this harness found was a use-after-free that only happened there.

set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build-asan"
KEEP=0
LIST=0

SCENARIOS="bare clients churn tiling groups desktops overlays physics reload ipc settings outputs monitors xwayland threads kill"

# A scenario whose compositor has to be started differently says so here, in a
# variable named after it. `outputs` needs a second monitor, and the headless
# backend is the only place we can be sure of getting one.
outputs_env="WLR_HEADLESS_OUTPUTS=2"
# `monitors` needs two as well, of different sizes, and things to fall: fwm
# boots in zero-g, and a window that never falls never finds a floor.
monitors_env="WLR_HEADLESS_OUTPUTS=2 FWM_TEST_GRAVITY=1"

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -b) BUILD="$2"; shift 2 ;;
        -k) KEEP=1; shift ;;
        -l) LIST=1; shift ;;
        -h|--help) usage 0 ;;
        -*) echo "unknown option: $1" >&2; usage 1 >&2 ;;
        *) break ;;
    esac
done
[ "$LIST" = 1 ] && { echo $SCENARIOS | tr ' ' '\n'; exit 0; }
[ $# -gt 0 ] && SCENARIOS="$*"

FWM="$BUILD/fwm"
FWMCTL="$BUILD/fwmctl"
[ -x "$FWM" ] || {
    echo "no compositor at $FWM" >&2
    echo "build one with sanitizers first:" >&2
    echo "  cmake -S . -B build-asan -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug \\" >&2
    echo "        -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \\" >&2
    echo "        -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'" >&2
    echo "  cmake --build build-asan -j\$(nproc)" >&2
    exit 1
}

# A terminal is the only client we can rely on being installed. Lightest first.
TERM_CMD=""
for c in foot alacritty kitty; do
    command -v "$c" >/dev/null 2>&1 && { TERM_CMD="$c"; break; }
done

LOGDIR="$(mktemp -d)"
FWM_PID=""
KIDS=""
FAILED=""
PASSED=""

# Everything else here runs the compositor on defaults, which means single
# threaded: the video wallpaper, the cava capture and the collision sound each
# start a thread, and each is off unless a config asks for it. So the `threads`
# scenario gets a config of its own, in a HOME of its own — the developer's own
# ~/.config/fwm is never read and never written.
#
# The video is one of the repo's own demo clips, so there is nothing to fetch
# and nothing to generate. FWM_TEST_CAVA=1 feeds the bars a synthetic spectrum:
# fwm never starts a sound server, and a runner has none, so without it the
# capture thread would decline to start and take the point of this scenario
# with it.
CFGHOME="$LOGDIR/home"
mkdir -p "$CFGHOME/.config/fwm"
cat > "$CFGHOME/.config/fwm/config.toml" <<TOML
[[wallpaper]]
path = "$REPO/assets/demo/launcher-wallpaper-picker.mp4"
fit  = "video"
fps  = 15

[cava]
mode = "both"
bars = 24
TOML
threads_env="HOME=$CFGHOME FWM_TEST_CAVA=1"

# `settings` WRITES a state file — the overlay `fwmctl save` keeps — so it gets
# a state directory of its own under the logs. The developer's own
# ~/.local/state/fwm is never read and never written by the suite.
settings_env="XDG_STATE_HOME=$LOGDIR/state"

cleanup() {
    # Only ever what this script started, always by recorded pid: a pkill by
    # name here would reach into the session the developer is sitting in.
    for p in $KIDS; do kill "$p" 2>/dev/null || true; done
    [ -n "$FWM_PID" ] && kill "$FWM_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    [ "$KEEP" = 1 ] || rm -rf "$LOGDIR"
}
trap cleanup EXIT INT TERM

# ── harness ───────────────────────────────────────────────────────────────

start() {
    log="$LOGDIR/$1.log"
    claims="$LOGDIR/$1.claims"
    : >"$claims"
    shift
    # detect_leaks stays off: wlroots and the GL stack leak on exit by design,
    # and the noise would bury a real report. Use-after-free and overflow are
    # what this harness is for.
    #
    # TSAN_OPTIONS is here for the build-tsan variant of this same harness.
    # halt_on_error=0 for the same reason as the others: one report must not
    # end the scenario, because the interesting second one usually comes later.
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:print_stacktrace=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
    TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:suppressions=$REPO/tests/tsan.supp" \
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    env "$@" "$FWM" >"$log" 2>&1 &
    FWM_PID=$!

    # Wait for the socket rather than sleeping a guessed amount.
    SOCK=""
    i=0
    while [ $i -lt 150 ]; do
        SOCK="$(grep -o 'Wayland socket: [^ ]*' "$log" 2>/dev/null | head -1 | cut -d' ' -f3 || true)"
        [ -n "$SOCK" ] && break
        kill -0 "$FWM_PID" 2>/dev/null || return 1
        i=$((i + 1))
        sleep 0.1
    done
    [ -n "$SOCK" ] || return 1
    export WAYLAND_DISPLAY="$SOCK"
    # Pin fwmctl to the instance we just started. It prefers $FWM_SOCKET over
    # $WAYLAND_DISPLAY, so running this harness from inside a live fwm session
    # would otherwise send every act() below into the developer's own session
    # — which is the one thing this script promises never to touch.
    export FWM_SOCKET="${XDG_RUNTIME_DIR:-/tmp}/fwm-$SOCK.sock"
    KIDS=""
    return 0
}

# Ask the compositor to do something. Failures are ignored on purpose: an
# action that is a no-op in the current state must not fail the scenario, only
# a sanitizer report or a crash may. The command is `dispatch`: an unknown one
# would be swallowed by that same `|| true` and quietly test nothing.
act() { timeout 5 "$FWMCTL" dispatch "$1" >/dev/null 2>&1 || true; sleep "${2:-0.3}"; }

# Any other fwmctl command. Same rule as act(): only a crash or a sanitizer
# report may fail a scenario, so a request the compositor refuses is fine —
# being refused is one of the paths under test.
ctl() { timeout 5 "$FWMCTL" "$@" >/dev/null 2>&1 || true; sleep 0.3; }

client() {
    [ -n "$TERM_CMD" ] || return 0
    timeout 40 "$TERM_CMD" -e sleep "${1:-30}" >/dev/null 2>&1 &
    KIDS="$KIDS $!"
    sleep "${2:-1.2}"
}

# Wait for the world to stop moving. How long a window takes to come to rest
# depends on how far it has to fall, which depends on the screen it is falling
# down — so this polls for two identical readings rather than sleeping a
# guessed amount, which on the tall screen guessed wrong.
settle() {
    _prev=""
    i=0
    while [ $i -lt 40 ]; do
        _now="$(timeout 5 "$FWMCTL" windows 2>/dev/null | grep -o '"y":-\?[0-9]*' || true)"
        [ -n "$_now" ] && [ "$_now" = "$_prev" ] && return 0
        _prev="$_now"
        i=$((i + 1))
        sleep 0.5
    done
    return 0
}

# Most scenarios only have to survive. A few have an answer that can be wrong
# without anything crashing — a window resting below the bottom of its screen
# is invisible, not fatal — so those state what they expect and claim() checks
# it.
#
# Its own file, NOT the compositor's log. The compositor holds that one open on
# a plain descriptor and writes at its own offset, so lines appended to the end
# by anyone else are overwritten by the next thing wlroots has to say — which
# is how the first version of this passed against a compositor that had the bug.
claim() {   # claim <what> <got> <want> [tolerance]
    _tol="${4:-0}"
    if [ -z "$2" ] || [ -z "$3" ]; then
        echo "CLAIM FAILED: $1 — nothing to compare (got '$2', wanted '$3')" >>"$claims"
        return 0
    fi
    _d=$(( $2 - $3 ))
    [ "$_d" -lt 0 ] && _d=$(( - _d ))
    if [ "$_d" -le "$_tol" ]; then
        echo "CLAIM ok: $1 = $2" >>"$claims"
    else
        echo "CLAIM FAILED: $1 = $2, expected $3 (+/- $_tol)" >>"$claims"
    fi
}

stop() {
    [ -n "$FWM_PID" ] || return 0
    for p in $KIDS; do kill "$p" 2>/dev/null || true; done
    KIDS=""
    sleep 0.5
    kill "$FWM_PID" 2>/dev/null || true
    # Give teardown room to run — that is the code under test here.
    i=0
    while [ $i -lt 60 ] && kill -0 "$FWM_PID" 2>/dev/null; do i=$((i + 1)); sleep 0.1; done
    kill -9 "$FWM_PID" 2>/dev/null || true
    wait "$FWM_PID" 2>/dev/null || true
    FWM_PID=""
}

verdict() {
    name="$1"
    log="$LOGDIR/$name.log"
    claims="$LOGDIR/$name.claims"
    bad=""
    # A scenario that states no claims is not thereby correct — but it is also
    # not making a claim, so only a FAILED one counts against it.
    [ -f "$claims" ] || : >"$claims"
    grep -q "ERROR: AddressSanitizer" "$log" 2>/dev/null && bad="$bad AddressSanitizer"
    grep -q "ERROR: LeakSanitizer"    "$log" 2>/dev/null && bad="$bad LeakSanitizer"
    grep -q "runtime error:"          "$log" 2>/dev/null && bad="$bad UBSan"
    grep -q "WARNING: ThreadSanitizer" "$log" 2>/dev/null && bad="$bad ThreadSanitizer"
    grep -qE "Segmentation fault|Assertion .* failed" "$log" 2>/dev/null && bad="$bad crash"
    grep -q "CLAIM FAILED"            "$claims" 2>/dev/null && bad="$bad claim"

    if [ -n "$bad" ]; then
        echo "  FAIL$bad"
        grep "CLAIM FAILED" "$claims" 2>/dev/null | sed 's/^/    /'
        sed -n '/ERROR: \|runtime error:\|WARNING: ThreadSanitizer/,+12p' "$log" | head -30 | sed 's/^/    /'
        FAILED="$FAILED $name"
        KEEP=1   # a failing run's log is worth more than a tidy temp dir
    else
        echo "  ok"
        PASSED="$PASSED $name"
    fi
}

# ── scenarios ─────────────────────────────────────────────────────────────

# Nothing but start and stop. Isolates teardown from anything a client does.
sc_bare() { sleep 1; }

sc_clients() {
    client 20; client 20; client 20
    act toggle_floating_all
    sleep 1
}

# Clients that come and go quickly, to catch teardown racing a live surface.
sc_churn() {
    i=0
    while [ $i -lt 4 ]; do
        [ -n "$TERM_CMD" ] && { timeout 12 "$TERM_CMD" -e sleep 1 >/dev/null 2>&1 & KIDS="$KIDS $!"; }
        i=$((i + 1)); sleep 0.8
    done
    sleep 2
}

sc_tiling() {
    client 25; client 25; client 25
    act toggle_tiling_all 0.6
    act toggle_split
    for d in left right up down; do act "tile_focus:$d" 0.2; done
    for d in left right up down; do act "tile_move:$d" 0.2; done
    act toggle_split
    act toggle_tiling_all
}

sc_groups() {
    client 25; client 25; client 25
    act group_toggle 0.5
    act group_add 0.5
    act group_next; act group_next; act group_prev
    act group_toggle
}

sc_desktops() {
    client 25; client 25
    for d in 1 2 3 9 0; do act "view:$d" 0.25; done
    act "move_to:3" 0.4
    act "move_to_view:5" 0.4
    act "move_camera:1"; act "move_camera:-1"
}

sc_overlays() {
    client 20
    act launcher 0.6;          act launcher 0.4
    act wallpaper_picker 0.6;  act wallpaper_picker 0.4
    act show_hints 0.5;        act show_hints 0.3
    act show_errors 0.5;       act show_errors 0.3
}

sc_physics() {
    client 25; client 25
    act cycle_gravity 0.5; act cycle_gravity 0.5; act cycle_gravity 0.5
    act toggle_nocollide_all
    act pin_window
    act calm_all
    # Free rotation: on, let it turn (the snapshot is rebuilt several times a
    # second while it does), off again.
    act spin_window 1.0; act spin_window 0.5
    act fake_fullscreen 0.4; act fake_fullscreen 0.4
    act real_fullscreen 0.4; act real_fullscreen 0.4
}

# Reload rebuilds config-derived state under live windows.
sc_reload() {
    client 25
    i=0
    while [ $i -lt 5 ]; do act reload_config 0.5; i=$((i + 1)); done
}

sc_xwayland() {
    command -v xterm >/dev/null 2>&1 || { echo "  (no xterm — skipped)"; return 0; }
    timeout 30 xterm -e sleep 12 >/dev/null 2>&1 &
    KIDS="$KIDS $!"
    sleep 3
    act toggle_tiling_all 0.5
    act killclient 0.5
}

# Event subscribers, which are the one IPC client that outlives its request.
# What matters is that the compositor never waits on one: a subscriber that
# stops reading must be dropped, and one killed mid-stream must not be written
# to afterwards.
sc_ipc() {
    timeout 25 "$FWMCTL" subscribe >/dev/null 2>&1 & KIDS="$KIDS $!"
    timeout 25 "$FWMCTL" subscribe window_open,window_close >/dev/null 2>&1 & KIDS="$KIDS $!"
    # Reads nothing for its whole life, while the scenario keeps events coming.
    timeout 25 "$FWMCTL" subscribe gravity >/dev/null 2>&1 & DEAF=$!
    KIDS="$KIDS $DEAF"
    sleep 0.5

    client 20
    i=0
    while [ $i -lt 40 ]; do act cycle_gravity 0.02; i=$((i + 1)); done
    act toggle_tiling_all 0.4
    act reload_config 0.4

    # Killed subscribers, then more events: emitting must survive the gap.
    kill -9 "$DEAF" 2>/dev/null || true
    sleep 0.3
    act cycle_gravity 0.2
    act "view:2" 0.3
    sleep 0.5

    # The reentrant case, which fwmctl cannot express: one connection that
    # subscribes AND dispatches, reading nothing back. Its own events overflow
    # its own backlog while its own command is still on the stack — dropping it
    # there is a use-after-free, and this is what caught that.
    command -v python3 >/dev/null 2>&1 || { echo "  (no python3 — reentrancy case skipped)"; return 0; }
    FWM_SOCKET="$FWM_SOCKET" timeout 60 python3 -c '
import socket, os, time
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["FWM_SOCKET"])
s.sendall(b"subscribe gravity\n"); time.sleep(0.3); s.setblocking(False)
sent, deadline = 0, time.time() + 40
while sent < 60000 and time.time() < deadline:
    try: s.sendall(b"dispatch cycle_gravity\n"); sent += 1
    except BlockingIOError: time.sleep(0.02)
    except (BrokenPipeError, ConnectionResetError): break   # dropped, as intended
' >/dev/null 2>&1 || true
    sleep 0.5
}

# The settings overlay and the per-window verbs: everything `fwmctl` can change
# that is not a keybind. The overlay is a FILE the compositor rewrites while it
# runs and re-reads on every load, so what this is really exercising is a reload
# standing on a file the session itself wrote a moment earlier.
sc_settings() {
    client 6

    # Several at once, then the same command with a bad name in it: the second
    # must change nothing at all, which is the whole point of checking before
    # applying.
    timeout 5 "$FWMCTL" set sun.blur=14 sun.length=40 tiling.gaps_in=12 >/dev/null 2>&1
    timeout 5 "$FWMCTL" set sun.blur=2 sun.not_a_thing=1 >/dev/null 2>&1
    timeout 5 "$FWMCTL" set decor.border_width=999 >/dev/null 2>&1   # out of range

    # Written down, then read back through a reload — the file is applied over
    # the config every time it is loaded.
    timeout 5 "$FWMCTL" save --all >/dev/null 2>&1
    timeout 5 "$FWMCTL" saved >/dev/null 2>&1
    act reload_config 0.5
    timeout 5 "$FWMCTL" save sun.opacity 0.7 >/dev/null 2>&1
    timeout 5 "$FWMCTL" save sun.blur >/dev/null 2>&1
    act reload_config 0.5
    timeout 5 "$FWMCTL" unsave sun.blur >/dev/null 2>&1
    timeout 5 "$FWMCTL" unsave --all >/dev/null 2>&1
    act reload_config 0.5

    # A subscriber watching all of it, and a stream of changes while it does.
    timeout 12 "$FWMCTL" subscribe setting >/dev/null 2>&1 & KIDS="$KIDS $!"
    sleep 0.3
    i=0
    while [ $i -lt 30 ]; do
        timeout 5 "$FWMCTL" set "sun.azimuth=$((i * 12))" >/dev/null 2>&1
        i=$((i + 1))
    done

    # One window at a time, by id: every verb, including the two that end with
    # the window somewhere else and the one that may destroy it.
    ids="$("$FWMCTL" windows 2>/dev/null | grep -o '"id":[0-9]*' | cut -d: -f2)"
    for id in $ids; do
        timeout 5 "$FWMCTL" window "$id" pin=toggle nocollide=on >/dev/null 2>&1
        timeout 5 "$FWMCTL" window "$id" x=300 y=200 >/dev/null 2>&1
        timeout 5 "$FWMCTL" window "$id" desktop=4 focus=on >/dev/null 2>&1
        timeout 5 "$FWMCTL" window "$id" pin=off >/dev/null 2>&1
    done
    sleep 0.4
    # Tiling refuses a position; asking anyway must be an error and not a state.
    act toggle_tiling_all 0.5
    for id in $ids; do
        timeout 5 "$FWMCTL" window "$id" x=10 y=10 >/dev/null 2>&1
    done
    act toggle_tiling_all 0.5

    # Nonsense, last: an id that is gone, a key that does not exist.
    timeout 5 "$FWMCTL" window 99999 pin=on >/dev/null 2>&1
    timeout 5 "$FWMCTL" window abc pin=on >/dev/null 2>&1
    for id in $ids; do
        timeout 5 "$FWMCTL" window "$id" close=on >/dev/null 2>&1
    done
    sleep 0.6
}

# Resolution, scale, rotation and position, on two monitors with windows open.
# Every one of these resizes a screen, and a resize is the heaviest thing that
# happens to fwm without a monitor being unplugged: the world reflows, expo is
# thrown away, wallpapers and status strips are rebuilt and every tiled desktop
# is re-split. Doing it to the PRIMARY screen is the interesting half, because
# that is what changes the size of every desktop.
sc_outputs() {
    names="$("$FWMCTL" outputs 2>/dev/null | grep -o '"name":"[^"]*"' | cut -d'"' -f4)"
    first="$(echo "$names" | head -1)"
    second="$(echo "$names" | tail -1)"
    [ -n "$first" ] || return 0

    client 30
    client 30
    act toggle_tiling_all 0.4

    ctl output "$first" mode=1600x900
    ctl output "$first" scale=1.5
    ctl output "$first" transform=90
    ctl output "$first" transform=normal scale=1 mode=1920x1080

    if [ "$second" != "$first" ]; then
        ctl output "$second" position=1920,0 mode=1280x1024 desktop=4
        ctl output "$second" enabled=off
        # A mode for a screen that is off, and a desktop only it was showing:
        # both are refusals, and a refusal must leave the rest working.
        ctl output "$second" mode=800x600
        act "view:4" 0.3
        ctl output "$second" enabled=on desktop=4
    fi

    # Putting out the screen everything else is anchored to — the primary, the
    # one holding the world's size. Refused outright when it is the only one
    # lit, which with a single monitor is the whole of this step.
    ctl output "$first" enabled=off
    act outputs_on 0.5

    # Malformed and unknown: a rejected request must change nothing at all.
    ctl output "$first" nonsense=1
    ctl output NO-SUCH-OUTPUT mode=640x480

    # The file has the last word, and getting there re-applies every monitor.
    act reload_config 0.6
    act expo 0.5
    act expo 0.5
}

# Monitors of DIFFERENT sizes, which is the one arrangement a single screen
# cannot stand in for. The world is a strip of columns the size of the primary
# monitor, but the floor a window lands on belongs to the glass it lands on: a
# short screen beside a tall one used to get the tall one's floor, and windows
# on it settled below the bottom of the screen where nobody could see them —
# #20, and the reason PhysicsWorld.desktop_h exists.
#
# The only scenario that checks a number. It has to: when this is wrong nothing
# crashes and no sanitizer says a word, the window is just gone from view. The
# claim is the same on both screens — a window comes to rest on the bottom of
# the monitor showing its desktop — which is what makes it a claim about the
# rule rather than about two magic numbers.
sc_monitors() {
    [ -n "$TERM_CMD" ] || { echo "  (no client — skipped)"; return 0; }

    names="$("$FWMCTL" outputs 2>/dev/null | grep -o '"name":"[^"]*"' | cut -d'"' -f4)"
    tall_mon="$(echo "$names" | head -1)"
    short_mon="$(echo "$names" | tail -1)"
    [ -n "$tall_mon" ] && [ "$short_mon" != "$tall_mon" ] || {
        echo "  (one monitor — skipped)"; return 0
    }

    # Which of the two is the primary is deliberately not decided here: the
    # rule under test holds either way round, and the headless backend is free
    # to hand them over in whatever order it likes.
    ctl output "$tall_mon"  mode=1280x1024
    ctl output "$short_mon" mode=1280x600
    sleep 1

    # Read the arrangement back rather than assuming it: a mode can be refused,
    # and a scenario that then measured against the size it ASKED for would
    # report a failure of the floor when what failed was the mode set.
    outs="$("$FWMCTL" outputs 2>/dev/null | tr '{' '\n')"
    desk_of()   { echo "$outs" | grep "\"name\":\"$1\"" | grep -o '"desktop":-\?[0-9]*' | cut -d: -f2; }
    height_of() { echo "$outs" | grep "\"name\":\"$1\"" | grep -o '"height":[0-9]*'     | cut -d: -f2; }

    tall_d="$(desk_of "$tall_mon")";   tall_h="$(height_of "$tall_mon")"
    short_d="$(desk_of "$short_mon")"; short_h="$(height_of "$short_mon")"
    [ "$tall_h" != "$short_h" ] || {
        echo "  (both screens the same size — mode set refused, skipped)"; return 0
    }

    # One window per screen, and never two on one at once: a window that lands
    # on top of another and stops there would be measured against a floor it
    # was never resting on.
    #
    # The window to move is the one that was not there a moment ago, found by
    # difference. Taking the first or the last id instead would be a guess
    # about the order `windows` reports in, and guessing wrong moves the SAME
    # window twice — which leaves both of them on one screen and every claim
    # below measuring the same floor.
    ids_now() { "$FWMCTL" windows 2>/dev/null | grep -o '"id":[0-9]*' | cut -d: -f2 | sort; }
    for d in "$tall_d" "$short_d"; do
        before="$(ids_now)"
        client 35
        id="$(ids_now | grep -vxF "${before:-!}" | head -1)"
        [ -n "$id" ] || { echo "  (client never mapped — skipped)"; return 0; }
        ctl window "$id" "desktop=$d"
    done
    settle

    # Both on one screen means the moves did not take, and every claim below
    # would be measuring one floor twice and passing while the other is broken.
    wins="$("$FWMCTL" windows 2>/dev/null | tr '{' '\n' | grep '"id"' || true)"
    on_short="$(echo "$wins" | grep -c "\"desktop\":$short_d," || true)"
    on_tall="$(echo  "$wins" | grep -c "\"desktop\":$tall_d,"  || true)"
    claim "windows sitting on the short screen" "$on_short" 1 0
    claim "windows sitting on the tall screen"  "$on_tall"  1 0

    # Every window, against the bottom of the screen showing the desktop it is
    # on. A couple of pixels of tolerance: where a window comes to rest is the
    # solver's answer, not arithmetic.
    echo "$wins" | while read -r w; do
        wd="$(echo "$w" | grep -o '"desktop":[0-9]*' | cut -d: -f2)"
        wy="$(echo "$w" | grep -o '"y":-\?[0-9]*'    | cut -d: -f2)"
        wh="$(echo "$w" | grep -o '"height":[0-9]*'  | cut -d: -f2)"
        [ -n "$wd" ] && [ -n "$wy" ] && [ -n "$wh" ] || continue
        if [ "$wd" = "$short_d" ]; then glass="$short_h"; else glass="$tall_h"; fi
        claim "bottom of the window on desktop $wd" "$((wy + wh))" "$glass" 4
    done
}

# The threaded half of the compositor, which every other scenario leaves
# switched off. A video wallpaper decodes on its own thread and hands frames
# over; the cava row captures and runs its FFT on another and hands over bar
# heights that become bodies in the physics world. Both are producers the main
# loop consumes from every frame, which is the shape races come in.
#
# So the point is not the acts themselves but doing them WHILE those threads
# run: switching desktops pauses and resumes the decoder, fullscreen stops it
# outright, and reload rebuilds the wallpaper and the row underneath both.
# Teardown last, with the threads still going, since that is where a producer
# outliving the thing it writes into shows up.
sc_threads() {
    sleep 2                      # let the decoder and the capture get going
    client 20
    for d in 1 2 3 1; do act "desktop:$d" 0.6; done
    act real_fullscreen 0.8      # pauses the decoder
    act real_fullscreen 0.8      # and resumes it
    act throw_random 0.4
    act reload_config 1.0        # rebuilds wallpaper + row under a live window
    for d in 2 1; do act "desktop:$d" 0.5; done
    sleep 1
}

# Clients killed outright rather than exiting: the compositor sees the socket
# drop with surfaces still mapped.
sc_kill() {
    client 30; client 30
    for p in $KIDS; do kill -9 "$p" 2>/dev/null || true; done
    KIDS=""
    sleep 2
}

# ── run ───────────────────────────────────────────────────────────────────

echo "compositor: $FWM"
echo "client:     ${TERM_CMD:-(none found — window scenarios will be thin)}"
echo "logs:       $LOGDIR"
echo

for name in $SCENARIOS; do
    if ! type "sc_$name" >/dev/null 2>&1; then
        echo "$name"; echo "  FAIL no such scenario"; FAILED="$FAILED $name"; continue
    fi
    echo "$name"
    eval "env_extra=\${${name}_env:-}"
    # Unquoted on purpose: the variable holds zero or more VAR=value words.
    # shellcheck disable=SC2086
    if ! start "$name" $env_extra; then
        echo "  FAIL compositor never came up"
        [ -f "$LOGDIR/$name.log" ] && tail -5 "$LOGDIR/$name.log" | sed 's/^/    /'
        FAILED="$FAILED $name"; KEEP=1
        continue
    fi
    "sc_$name" || true
    stop
    verdict "$name"
done

echo
n_pass=$(echo $PASSED | wc -w)
n_fail=$(echo $FAILED | wc -w)
if [ "$n_fail" -gt 0 ]; then
    echo "FAILED ($n_fail):$FAILED"
    echo "passed ($n_pass):$PASSED"
    echo "logs kept in $LOGDIR"
    exit 1
fi
echo "all $n_pass scenarios clean"
