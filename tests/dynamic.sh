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

SCENARIOS="bare clients churn tiling groups desktops overlays physics reload xwayland kill"

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
    shift
    # detect_leaks stays off: wlroots and the GL stack leak on exit by design,
    # and the noise would bury a real report. Use-after-free and overflow are
    # what this harness is for.
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:print_stacktrace=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
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
    KIDS=""
    return 0
}

# Ask the compositor to do something. Failures are ignored on purpose: an
# action that is a no-op in the current state must not fail the scenario, only
# a sanitizer report or a crash may.
act() { timeout 5 "$FWMCTL" action "$1" >/dev/null 2>&1 || true; sleep "${2:-0.3}"; }

client() {
    [ -n "$TERM_CMD" ] || return 0
    timeout 40 "$TERM_CMD" -e sleep "${1:-30}" >/dev/null 2>&1 &
    KIDS="$KIDS $!"
    sleep "${2:-1.2}"
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
    bad=""
    grep -q "ERROR: AddressSanitizer" "$log" 2>/dev/null && bad="$bad AddressSanitizer"
    grep -q "ERROR: LeakSanitizer"    "$log" 2>/dev/null && bad="$bad LeakSanitizer"
    grep -q "runtime error:"          "$log" 2>/dev/null && bad="$bad UBSan"
    grep -qE "Segmentation fault|Assertion .* failed" "$log" 2>/dev/null && bad="$bad crash"

    if [ -n "$bad" ]; then
        echo "  FAIL$bad"
        sed -n '/ERROR: \|runtime error:/,+12p' "$log" | head -30 | sed 's/^/    /'
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
    if ! start "$name"; then
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
