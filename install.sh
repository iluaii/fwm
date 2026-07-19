#!/usr/bin/env bash
# fwm — install / update script.
#
#   ./install.sh            install dependencies, build, install
#   ./install.sh update     git pull, rebuild, reinstall
#   ./install.sh uninstall  remove installed files (config is kept)
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
            wayland wlroots0.20 libxkbcommon cairo pango gdk-pixbuf2 box2d
    elif command -v apt-get >/dev/null 2>&1; then
        msg "Installing dependencies (apt)"
        $SUDO apt-get update
        $SUDO apt-get install -y \
            gcc make cmake pkg-config git \
            libwayland-dev libxkbcommon-dev libcairo2-dev \
            libpango1.0-dev libgdk-pixbuf-2.0-dev
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
            cairo-devel pango-devel gdk-pixbuf2-devel
    elif command -v xbps-install >/dev/null 2>&1; then
        msg "Installing dependencies (xbps/Void)"
        $SUDO xbps-install -Sy \
            gcc make cmake pkg-config git \
            wayland-devel wlroots0.20-devel libxkbcommon-devel \
            cairo-devel pango-devel gdk-pixbuf-devel seatd
        # Void has no systemd-logind: wlroots needs seatd for DRM/input access.
        if [ ! -e /var/service/seatd ]; then
            warn "enable seatd before starting fwm from a TTY:"
            warn "  $SUDO ln -s /etc/sv/seatd /var/service"
            warn "  $SUDO usermod -aG _seatd \$USER   (then re-login)"
        fi
    else
        warn "unknown package manager — install deps manually:"
        warn "  wayland, wlroots-0.20, xkbcommon, cairo, pango, gdk-pixbuf, box2d v3"
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
    $SUDO install -Dm755 "$REPO_DIR/build/fwm-wayland" "$PREFIX/bin/fwm-wayland"
    $SUDO install -Dm644 "$REPO_DIR/session/fwm.desktop" \
        /usr/share/wayland-sessions/fwm.desktop

    # User config: never overwrite an existing one.
    local cfg_dir="${XDG_CONFIG_HOME:-$HOME/.config}/fwm"
    if [ ! -e "$cfg_dir/config.toml" ]; then
        mkdir -p "$cfg_dir"
        cp "$REPO_DIR/config.toml.example" "$cfg_dir/config.toml"
        msg "Default config written to $cfg_dir/config.toml"
    fi
    msg "Done. Log in via your display manager (fwm session) or run: fwm-wayland"
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
    msg "Updating from git"
    git -C "$REPO_DIR" pull --ff-only
    ensure_box2d
    build
    install_files
    ;;
uninstall)
    msg "Uninstalling"
    $SUDO rm -f "$PREFIX/bin/fwm-wayland" /usr/share/wayland-sessions/fwm.desktop
    msg "Removed (user config in ~/.config/fwm kept)"
    ;;
*)
    echo "usage: $0 [install|update|uninstall]" >&2
    exit 1
    ;;
esac
