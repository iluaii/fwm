#!/bin/sh
# Rebuild fwm and run it — nested inside the current Wayland session, or
# natively on DRM/KMS when started from a bare TTY. The mode is picked from
# WAYLAND_DISPLAY; there is no flag for it.
#
# A compositor cannot hot-restart: it IS the display server, so when it exits
# every client's connection dies with it. Nesting is the working substitute —
# the inner fwm can be restarted as often as you like while the session you are
# actually working in never notices.
#
# Nesting cannot test keybinds, though: an outer fwm claims the Super
# combinations before the inner one ever sees them. Run from a TTY for those.
#
#   ./dev.sh                      rebuild and run
#   ./dev.sh -n 2                 ... with 2 terminals already open
#   ./dev.sh -g 1                 ... with gravity on (fwm boots in zero-g)
#   ./dev.sh -a toggle_tiling_all ... firing one action after startup
#   ./dev.sh -c 3                 ... parked on desktop 3
#   ./dev.sh -d                   ... with debug logging
#   ./dev.sh -s shot.png          screenshot after a few seconds, then quit
#   ./dev.sh -B                   skip the rebuild
#
# Everything it starts, it stops. Nothing outside the nested session is touched.

set -eu

REPO="$(cd "$(dirname "$0")" && pwd)"
BUILD="$REPO/build"
TERM_CMD="${TERMINAL:-kitty}"

windows=0
build=1
shot=""
shot_delay=4
run_env=""

# Print the header block, stopping at the first blank line, so the usage text
# does not have to be kept in sync with hard-coded line numbers.
usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -n) windows="$2"; shift 2 ;;
        -g) run_env="$run_env FWM_TEST_GRAVITY=$2"; shift 2 ;;
        -a) run_env="$run_env FWM_TEST_ACTION=$2"; shift 2 ;;
        -c) run_env="$run_env FWM_TEST_CAMERA=$2"; shift 2 ;;
        -d) run_env="$run_env FWM_DEBUG=1"; shift ;;
        --picker) run_env="$run_env FWM_OPEN_PICKER=1"; shift ;;
        --hints)  run_env="$run_env FWM_SHOW_HINTS=1"; shift ;;
        -t) TERM_CMD="$2"; shift 2 ;;
        -s) shot="$2"; shift 2 ;;
        -B) build=0; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown option: $1" >&2; usage 1 >&2 ;;
    esac
done

# A Wayland session to nest inside means nested; otherwise take over the TTY
# directly. Forcing WLR_BACKENDS=wayland in the native case would make wlroots
# look for a parent compositor that is not there and fail at startup.
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    mode="nested"
    backend_env="WLR_BACKENDS=wayland"
else
    mode="native"
    backend_env=""
    if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
        echo "dev.sh: XDG_RUNTIME_DIR is unset — wlroots needs it for the socket." >&2
        exit 1
    fi
fi

if [ "$build" = 1 ]; then
    [ -d "$BUILD" ] || cmake -S "$REPO" -B "$BUILD" >/dev/null
    cmake --build "$BUILD" || exit 1
fi

log="$(mktemp)"
fwm_pid=""
kids=""

cleanup() {
    # Only ever kill what this script started: never pkill by name, or a stray
    # pattern takes out the terminals in the real session too.
    for p in $kids; do kill "$p" 2>/dev/null || true; done
    [ -n "$fwm_pid" ] && kill "$fwm_pid" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -f "$log"
}
trap cleanup EXIT INT TERM

# shellcheck disable=SC2086
env $run_env $backend_env "$BUILD/fwm" >"$log" 2>&1 &
fwm_pid=$!

# Wait for the inner socket rather than sleeping a guessed amount.
sock=""
i=0
while [ $i -lt 100 ]; do
    sock="$(grep -o 'Wayland socket: [^ ]*' "$log" 2>/dev/null | head -1 | cut -d' ' -f3 || true)"
    [ -n "$sock" ] && break
    kill -0 "$fwm_pid" 2>/dev/null || { echo "fwm exited during startup:" >&2; cat "$log" >&2; exit 1; }
    i=$((i + 1))
    sleep 0.1
done
[ -n "$sock" ] || { echo "dev.sh: fwm never reported a socket" >&2; cat "$log" >&2; exit 1; }

echo "dev.sh: $mode fwm on $sock (pid $fwm_pid)"

n=0
while [ "$n" -lt "$windows" ]; do
    WAYLAND_DISPLAY="$sock" $TERM_CMD >/dev/null 2>&1 &
    kids="$kids $!"
    n=$((n + 1))
    sleep 0.6
done

if [ -n "$shot" ]; then
    sleep "$shot_delay"
    if WAYLAND_DISPLAY="$sock" grim "$shot" 2>/dev/null; then
        echo "dev.sh: wrote $shot"
    else
        echo "dev.sh: grim failed (is it installed?)" >&2
    fi
    exit 0
fi

echo "dev.sh: Ctrl-C to stop. Log: tail -f $log"
wait "$fwm_pid" || true
echo "dev.sh: fwm exited. Last lines:"
tail -5 "$log"
