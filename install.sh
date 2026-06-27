#!/usr/bin/env bash
set -e

# ─────────────────────────────────────────────
#  fwm — install script
#  Supports: apt (Debian/Ubuntu), dnf (Fedora/RHEL), pacman (Arch)
# ─────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()    { echo -e "${CYAN}[•]${NC} $*"; }
success() { echo -e "${GREEN}[✓]${NC} $*"; }
warn()    { echo -e "${YELLOW}[!]${NC} $*"; }
die()     { echo -e "${RED}[✗]${NC} $*" >&2; exit 1; }

INSTALL_BIN="${INSTALL_BIN:-/usr/local/bin}"
TARGET="fwm"
XINITRC="${HOME}/.xinitrc"

# ── detect package manager ───────────────────

detect_pm() {
    if command -v apt-get &>/dev/null; then
        PM="apt"
    elif command -v dnf &>/dev/null; then
        PM="dnf"
    elif command -v pacman &>/dev/null; then
        PM="pacman"
    else
        die "Unsupported package manager. Install dependencies manually:
  gcc make pkg-config libx11 libxft libxext freetype2"
    fi
    info "Detected package manager: ${BOLD}${PM}${NC}"
}

# ── install dependencies ─────────────────────

install_deps() {
    info "Installing build dependencies..."

    case "$PM" in
        apt)
            DEPS="gcc make pkg-config libx11-dev libxft-dev libxext-dev libfreetype-dev"
            sudo apt-get update -qq
            sudo apt-get install -y $DEPS
            ;;
        dnf)
            DEPS="gcc make pkgconf-pkg-config libX11-devel libXft-devel libXext-devel freetype-devel"
            sudo dnf install -y $DEPS
            ;;
        pacman)
            DEPS="gcc make pkgconf libx11 libxft libxext freetype2"
            sudo pacman -Sy --needed --noconfirm $DEPS
            ;;
    esac

    success "Dependencies installed."
}

# ── build ────────────────────────────────────

build() {
    info "Building ${BOLD}${TARGET}${NC}..."

    if [ ! -f "Makefile" ]; then
        die "Makefile not found. Run this script from the project root (fwm/)."
    fi

    make clean 2>/dev/null || true
    make -j"$(nproc)"

    if [ ! -f "$TARGET" ]; then
        die "Build failed — binary not found."
    fi

    success "Build successful."
}

# ── install binary ───────────────────────────

install_bin() {
    info "Installing ${BOLD}${TARGET}${NC} to ${INSTALL_BIN}..."

    if [ ! -w "$INSTALL_BIN" ]; then
        sudo install -m 755 "$TARGET" "$INSTALL_BIN/$TARGET"
    else
        install -m 755 "$TARGET" "$INSTALL_BIN/$TARGET"
    fi

    success "Installed to ${INSTALL_BIN}/${TARGET}."
}

# ── optional: Xephyr (for dev/test) ─────────

install_xephyr() {
    if command -v Xephyr &>/dev/null; then
        success "Xephyr already installed (for 'make run')."
        return
    fi

    read -r -p "$(echo -e "${YELLOW}[?]${NC} Install Xephyr for testing? (make run) [y/N] ")" yn
    case "$yn" in
        [Yy]*)
            case "$PM" in
                apt)    sudo apt-get install -y xserver-xephyr ;;
                dnf)    sudo dnf install -y xorg-x11-server-Xephyr ;;
                pacman) sudo pacman -Sy --needed --noconfirm xorg-server-xephyr ;;
            esac
            success "Xephyr installed."
            ;;
        *)
            warn "Skipping Xephyr."
            ;;
    esac
}

# ── setup config ────────────────────────────

setup_config() {
    local config_dir="${HOME}/.config/fwm"
    local config_file="${config_dir}/config.toml"
    local default_file="config.toml.default"

    if [ ! -f "$default_file" ]; then
        warn "config.toml.default not found in project root, skipping config setup."
        return
    fi

    mkdir -p "$config_dir"

    if [ -f "$config_file" ]; then
        success "Config already exists at ${config_file}, skipping."
        return
    fi

    cp "$default_file" "$config_file"
    success "Created config at ${config_file}."
}

# ── setup xinitrc ────────────────────────────

setup_xinitrc() {
    local exec_line="exec fwm"

    if [ ! -f "$XINITRC" ]; then
        info "$HOME/.xinitrc not found — creating..."
        cat > "$XINITRC" << XINITRC_EOF
#!/bin/sh
exec fwm
XINITRC_EOF
        chmod +x "$XINITRC"
        success "Created ${XINITRC}."
        return
    fi

    if grep -q "fwm" "$XINITRC"; then
        success "fwm already present in ${XINITRC}, skipping."
        return
    fi

    info "$HOME/.xinitrc already exists."
    read -r -p "$(echo -e "${YELLOW}[?]${NC} Create a backup before modifying? [Y/n] ")" yn
    case "$yn" in
        [Nn]*)
            warn "Skipping backup."
            ;;
        *)
            local backup
            backup="${XINITRC}.bak.$(date +%Y%m%d_%H%M%S)"
            cp "$XINITRC" "$backup"
            success "Backup saved to ${backup}."
            ;;
    esac

    sed -i 's/^\(exec .*\)$/# \1  # commented out by fwm installer/' "$XINITRC"
    echo "" >> "$XINITRC"
    echo "$exec_line" >> "$XINITRC"
    success "Added 'exec fwm' to ${XINITRC}."
}

# ── update ───────────────────────────────────

cmd_update() {
    echo -e "${BOLD}fwm updater${NC} — physics window manager"
    echo ""

    if [ ! -f "Makefile" ]; then
        die "Makefile not found. Run this script from the project root (fwm/)."
    fi

    if ! command -v "$TARGET" &>/dev/null; then
        warn "fwm is not installed yet. Running full install instead..."
        echo ""
        cmd_install
        return
    fi

    if git rev-parse --git-dir &>/dev/null 2>&1; then
        read -r -p "$(echo -e "${YELLOW}[?]${NC} Pull latest changes from git? [Y/n] ")" yn
        case "$yn" in
            [Nn]*)
                warn "Skipping git pull."
                ;;
            *)
                info "Pulling latest changes..."
                git pull || die "git pull failed."
                success "Repository updated."
                ;;
        esac
        echo ""
    fi

    info "Rebuilding ${BOLD}${TARGET}${NC}..."
    make clean 2>/dev/null || true
    make -j"$(nproc)"

    if [ ! -f "$TARGET" ]; then
        die "Build failed — binary not found."
    fi

    success "Build successful."

    info "Replacing binary in ${INSTALL_BIN}..."
    if [ ! -w "$INSTALL_BIN" ]; then
        sudo install -m 755 "$TARGET" "$INSTALL_BIN/$TARGET"
    else
        install -m 755 "$TARGET" "$INSTALL_BIN/$TARGET"
    fi

    echo ""
    success "${BOLD}fwm updated!${NC}"
    setup_config
}

# ── install ───────────────────────────────────

cmd_install() {
    echo -e "${BOLD}fwm installer${NC} — physics window manager"
    echo ""

    detect_pm
    install_deps
    build
    install_xephyr
    install_bin
    setup_config
    setup_xinitrc

    echo ""
    success "${BOLD}All done!${NC}"
    echo -e "  Start X: ${CYAN}startx${NC}"
    echo -e "  Or test: ${CYAN}make run${NC}  (requires Xephyr)"
}

# ── entry point ───────────────────────────────

case "${1:-install}" in
    install) cmd_install ;;
    update)  cmd_update  ;;
    *)
        echo "Usage: $0 [install|update]"
        exit 1
        ;;
esac