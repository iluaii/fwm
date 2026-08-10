#!/usr/bin/env bash
# fwm — install / update script.
#
#   ./install.sh                install dependencies, build, install
#   ./install.sh update         git pull, rebuild, reinstall
#   ./install.sh update config  merge new options into your config.toml
#   ./install.sh uninstall      remove installed files (config is kept)
#
# Supported package managers: pacman (Arch), apt (Debian/Ubuntu),
# dnf (Fedora), xbps (Void). Box2D v3 is built from source when the
# distro does not ship it (Debian's libbox2d-dev is 2.4 — too old).

set -eu

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BOX2D_VERSION="v3.1.1"

# ── privilege helper (Void users often have doas instead of sudo) ──────
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
elif command -v doas >/dev/null 2>&1; then
    SUDO="doas"
else
    echo "error: need root, sudo, or doas" >&2
    exit 1
fi

msg()  { printf '\033[1;33m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;31mwarning:\033[0m %s\n' "$*" >&2; }

# ── dependencies ───────────────────────────────────────────────────────
install_deps() {
    if command -v pacman >/dev/null 2>&1; then
        msg "Installing dependencies (pacman)"
        $SUDO pacman -S --needed --noconfirm \
            gcc make cmake pkgconf git \
            wayland wlroots0.20 libxkbcommon cairo pango gdk-pixbuf2 box2d \
            ffmpeg libpipewire libpulse xorg-xwayland
    elif command -v apt-get >/dev/null 2>&1; then
        msg "Installing dependencies (apt)"
        $SUDO apt-get update
        $SUDO apt-get install -y \
            gcc make cmake pkg-config git \
            libwayland-dev libxkbcommon-dev libcairo2-dev \
            libpango1.0-dev libgdk-pixbuf-2.0-dev \
            libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
            libpipewire-0.3-dev libpulse-dev xwayland
        # wlroots: the 0.20 -dev package name varies by release; try in order.
        local ok=0
        for p in libwlroots-0.20-dev libwlroots-0.19-dev libwlroots-dev; do
            if $SUDO apt-get install -y "$p" 2>/dev/null; then ok=1; break; fi
        done
        [ "$ok" = 1 ] || warn "no wlroots -dev package found; install wlroots 0.20 manually"
        # NOTE: libbox2d-dev on Debian is v2.4 — do not install it; v3 is
        # handled by ensure_box2d below.
    elif command -v dnf >/dev/null 2>&1; then
        msg "Installing dependencies (dnf)"
        $SUDO dnf install -y \
            gcc make cmake pkgconf-pkg-config git \
            wayland-devel wlroots-devel libxkbcommon-devel \
            cairo-devel pango-devel gdk-pixbuf2-devel ffmpeg-free-devel \
            pipewire-devel pulseaudio-libs-devel xorg-x11-server-Xwayland
    elif command -v xbps-install >/dev/null 2>&1; then
        msg "Installing dependencies (xbps/Void)"
        # "already installed" lines are noise; a real failure is usually a
        # partial upgrade — installing deps pulls a newer shlib that some
        # already-installed package was not rebuilt against yet.
        if ! $SUDO xbps-install -Sy \
            gcc make cmake pkg-config git \
            wayland-devel wlroots0.20-devel libxkbcommon-devel \
            cairo-devel pango-devel gdk-pixbuf-devel ffmpeg6-devel seatd \
            pipewire-devel pulseaudio-devel xorg-server-xwayland
        then
            warn "xbps aborted (unresolved shlibs?) — bring the system up to date first:"
            warn "  $SUDO xbps-install -Su"
            warn "then re-run this script"
            exit 1
        fi
        # Void has no systemd-logind: wlroots needs seatd for DRM/input access.
        if [ ! -e /var/service/seatd ]; then
            warn "enable seatd before starting fwm from a TTY:"
            warn "  $SUDO ln -s /etc/sv/seatd /var/service"
            warn "  $SUDO usermod -aG _seatd \$USER   (then re-login)"
        fi
    else
        warn "unknown package manager — install deps manually:"
        warn "  wayland, wlroots-0.20, xkbcommon, cairo, pango, gdk-pixbuf, box2d v3, ffmpeg (libav*), xwayland"
    warn "  optional: pipewire and/or pulseaudio (headers) — the [cava] visualiser"
    warn "  picks whichever sound server is actually running; with neither it is left out"
    fi
}

# ── Box2D v3 (cmake package `box2d`) ───────────────────────────────────
have_box2d3() {
    # box2d v3 installs a cmake config; v2.4 packages don't provide box2dConfig
    # with the b2WorldId C API, so also reject anything without box2d/box2d.h.
    local cfg
    cfg=$(find /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 \
               -name 'box2dConfig.cmake' -path '*cmake*' 2>/dev/null | head -1)
    [ -n "$cfg" ] || return 1
    [ -e /usr/include/box2d/box2d.h ] || [ -e /usr/local/include/box2d/box2d.h ]
}

ensure_box2d() {
    if have_box2d3; then
        msg "Box2D v3 found"
        return
    fi
    msg "Box2D v3 not found — building $BOX2D_VERSION from source"
    local tmp
    tmp=$(mktemp -d)
    git clone --depth 1 --branch "$BOX2D_VERSION" \
        https://github.com/erincatto/box2d.git "$tmp/box2d"
    cmake -S "$tmp/box2d" -B "$tmp/box2d/build" \
        -DCMAKE_BUILD_TYPE=Release -DBOX2D_SAMPLES=OFF -DBOX2D_UNIT_TESTS=OFF \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    cmake --build "$tmp/box2d/build" -j"$(nproc)"
    $SUDO cmake --install "$tmp/box2d/build"
    rm -rf "$tmp"
}

# ── build + install ────────────────────────────────────────────────────
build() {
    msg "Building fwm"
    cmake -S "$REPO_DIR" -B "$REPO_DIR/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$REPO_DIR/build" -j"$(nproc)"
}

install_files() {
    msg "Installing to $PREFIX"
    $SUDO install -Dm755 "$REPO_DIR/build/fwm" "$PREFIX/bin/fwm"
    # The compositor used to install as fwm-wayland. Left alone it stays on
    # PATH forever as a stale build, and anything still pointing at it keeps
    # silently running the old binary instead of the one just installed.
    if [ -e "$PREFIX/bin/fwm-wayland" ]; then
        msg "Removing the old fwm-wayland binary (it is now just fwm)"
        $SUDO rm -f "$PREFIX/bin/fwm-wayland"
    fi
    # The control-socket CLI. Without it on PATH, scripting fwm means finding
    # the binary in a build directory, which nobody will do.
    $SUDO install -Dm755 "$REPO_DIR/build/fwmctl" "$PREFIX/bin/fwmctl"
    # Supervisor: restarts fwm after a crash and lets session restore put the
    # applications back. The session entry below launches this, not the
    # compositor directly.
    $SUDO install -Dm755 "$REPO_DIR/session/fwm-session" "$PREFIX/bin/fwm-session"
    # Substitute the real binary path rather than shipping a bare "fwm-session":
    # display managers run sessions with a trimmed PATH that need not contain
    # $PREFIX/bin, and the failure mode is the worst kind — the session shows up
    # in the list, you pick it, and it drops straight back to the login screen
    # with nothing to explain why.
    $SUDO install -d /usr/share/wayland-sessions
    sed "s|^Exec=.*|Exec=$PREFIX/bin/fwm-session|" "$REPO_DIR/session/fwm.desktop" \
        | $SUDO tee /usr/share/wayland-sessions/fwm.desktop >/dev/null
    $SUDO chmod 644 /usr/share/wayland-sessions/fwm.desktop

    # Portal routing: sends screen capture to the wlroots backend. Harmless if
    # xdg-desktop-portal-wlr is not installed — the frontend falls back — but
    # without the file screen sharing silently picks a backend that cannot see
    # fwm's outputs.
    # Note the directory: portals/ holds .portal files describing backends,
    # while the routing config belongs one level up, next to portals.conf.
    $SUDO install -Dm644 "$REPO_DIR/session/fwm-portals.conf" \
        /usr/share/xdg-desktop-portal/fwm-portals.conf

    # User config: never overwrite an existing one. `update config` is what
    # brings an existing one up to date.
    local cfg_dir
    cfg_dir="$(config_dir)"
    if [ ! -e "$cfg_dir/config.toml" ]; then
        mkdir -p "$cfg_dir"
        cp "$REPO_DIR/config.toml.example" "$cfg_dir/config.toml"
        msg "Default config written to $cfg_dir/config.toml"
    fi
    msg "Done. Log in via your display manager (fwm session) or run: fwm"
}

# ── the config ─────────────────────────────────────────────────────────
# fwm reads $HOME/.config/fwm/config.toml — HOME, not XDG_CONFIG_HOME; see
# server_config_path() in src/server_config.c. Everything here follows it, so
# the file this script writes is the file the compositor opens.
config_dir() { printf '%s/.config/fwm' "$HOME"; }

# A yes/no question, default in $2. Read from the terminal rather than stdin:
# this script is often piped into a shell, and then stdin is the script itself.
# With no terminal at all the default stands and says so.
ask() {
    local q="$1" def="${2:-y}" hint ans
    if [ "$def" = y ]; then hint="[Y/n]"; else hint="[y/N]"; fi
    # Opened, not stat'd: /dev/tty exists in every container and cron job, and
    # only opening it says whether anyone is there.
    if ! { : < /dev/tty; } 2>/dev/null; then
        msg "$q $hint -> $def (nothing to ask on)"
        [ "$def" = y ]
        return
    fi
    printf '\033[1;33m==>\033[0m %s %s ' "$q" "$hint" > /dev/tty
    read -r ans < /dev/tty || ans=""
    [ -n "$ans" ] || ans="$def"
    case "$ans" in [Yy]*) return 0 ;; *) return 1 ;; esac
}

# Copy the config aside, in the .bak / .bak2 / .bak3 convention the directory
# already uses. An existing backup is never quietly replaced: it may be the one
# good copy of a config someone spent an evening on.
backup_config() {
    local cfg="$1" newest="" target="" n=2 f
    for f in "$cfg".bak*; do [ -e "$f" ] && newest="$f"; done

    if [ -n "$newest" ]; then
        if ask "A backup exists ($(basename "$newest")). Overwrite it? No keeps it and makes another." n; then
            target="$newest"
        else
            while [ -e "$cfg.bak$n" ]; do n=$((n + 1)); done
            if [ -e "$cfg.bak" ]; then target="$cfg.bak$n"; else target="$cfg.bak"; fi
        fi
    else
        target="$cfg.bak"
    fi

    cp -- "$cfg" "$target"
    msg "Backed up to $target"
}

# Bring a config written against an older fwm up to the current example: the
# example's comments, ordering and new options, your values and your own binds.
# See tools/config-merge.awk for what exactly survives.
merge_config() {
    local dir cfg example merged stats mine kept extra sections fresh
    dir="$(config_dir)"
    cfg="$dir/config.toml"
    example="$REPO_DIR/config.toml.example"

    [ -r "$example" ] || { warn "no config.toml.example in $REPO_DIR"; return 1; }

    if [ ! -e "$cfg" ]; then
        mkdir -p "$dir"
        cp "$example" "$cfg"
        msg "No config here yet — wrote the default to $cfg"
        return 0
    fi

    if ask "Back up $cfg before merging?" y; then
        backup_config "$cfg"
    else
        warn "No backup taken."
    fi

    merged="$(mktemp)"
    stats="$(mktemp)"
    awk -f "$REPO_DIR/tools/config-merge.awk" -v stats="$stats" "$cfg" "$example" > "$merged"

    # A merge that came out empty, or lost the section every config has, is a
    # merge that goes in the bin rather than over the only copy of your config.
    if [ ! -s "$merged" ] || ! grep -q '^\[binds\]' "$merged"; then
        warn "merge produced nothing usable — $cfg left untouched"
        rm -f "$merged" "$stats"
        return 1
    fi

    cat "$merged" > "$cfg"      # keeps the file's own permissions
    read -r mine kept extra sections fresh < "$stats" || true
    rm -f "$merged" "$stats"

    msg "Merged into $cfg"
    msg "  $kept of your $mine settings kept in place, $extra kept aside, $sections whole sections kept"
    msg "  $fresh option(s) here that your config did not have yet"
    msg "Read it over before the next login: a value written across several"
    msg "lines is the one thing the merge cannot see."
}

# ── commands ───────────────────────────────────────────────────────────
case "${1:-install}" in
install)
    install_deps
    ensure_box2d
    build
    install_files
    ;;
update)
    case "${2:-}" in
    "")
        msg "Updating from git"
        git -C "$REPO_DIR" pull --ff-only
        ensure_box2d
        build
        install_files
        ;;
    config)
        merge_config
        ;;
    *)
        echo "usage: $0 update [config]" >&2
        exit 1
        ;;
    esac
    ;;
uninstall)
    msg "Uninstalling"
    $SUDO rm -f "$PREFIX/bin/fwm" "$PREFIX/bin/fwm-wayland" "$PREFIX/bin/fwmctl" "$PREFIX/bin/fwm-session" \
                /usr/share/wayland-sessions/fwm.desktop \
                /usr/share/xdg-desktop-portal/fwm-portals.conf
    msg "Removed (user config in ~/.config/fwm kept)"
    ;;
*)
    echo "usage: $0 [install|update [config]|uninstall]" >&2
    exit 1
    ;;
esac
