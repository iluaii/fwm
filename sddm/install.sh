#!/bin/sh
# Installs the fwm login screen for SDDM. Run once, as root:
#
#   sudo sddm/install.sh
#
# What it does:
#   - copies the theme to /usr/share/sddm/themes/fwm
#   - makes /var/lib/fwm-sddm, owned by the user who ran sudo, where
#     fwm-sddm-sync puts the wallpapers and the palette without needing root
#   - links the theme's theme.conf.user to the one in there
#   - selects the theme in /etc/sddm.conf.d/10-fwm.conf
#   - installs fwm-sddm-sync to /usr/local/bin
#
# It does not enable the sddm service and does not pick SDDM's display
# server; see sddm/README.md for both.
#
# DESTDIR=<dir> lays the same tree out under <dir> instead, without root —
# for trying it out.

set -eu

D=${DESTDIR:-}
if [ -z "$D" ]; then
    [ "$(id -u)" -eq 0 ] || { echo "install.sh: run it with sudo" >&2; exit 1; }
    USER_NAME=${SUDO_USER:-}
    [ -n "$USER_NAME" ] || { echo "install.sh: run it with sudo, not as root directly" >&2; exit 1; }
else
    USER_NAME=$(id -un)
fi

HERE=$(cd "$(dirname "$0")" && pwd)
THEME=$D/usr/share/sddm/themes/fwm
SHARED=$D/var/lib/fwm-sddm

rm -rf "$THEME"
install -d -m 755 "$THEME"
install -m 644 "$HERE"/fwm/*.qml "$HERE"/fwm/*.js "$HERE"/fwm/metadata.desktop \
               "$HERE"/fwm/theme.conf "$THEME"/

install -d -m 755 "$SHARED"
chown "$USER_NAME" "$SHARED"
if [ ! -e "$SHARED/theme.conf.user" ]; then
    : > "$SHARED/theme.conf.user"
    chmod 644 "$SHARED/theme.conf.user"
    chown "$USER_NAME" "$SHARED/theme.conf.user"
fi
# The link names the real path even under DESTDIR: it is followed where SDDM
# runs, not where the tree was laid out.
ln -sfn /var/lib/fwm-sddm/theme.conf.user "$THEME/theme.conf.user"

install -d -m 755 "$D/etc/sddm.conf.d"
printf '[Theme]\nCurrent=fwm\n' > "$D/etc/sddm.conf.d/10-fwm.conf"

install -d -m 755 "$D/usr/local/bin"
install -m 755 "$HERE/fwm-sddm-sync" "$D/usr/local/bin/fwm-sddm-sync"

echo "theme:  $THEME"
echo "shared: $SHARED (owned by $USER_NAME)"
echo "next:   run fwm-sddm-sync from your fwm session, as $USER_NAME"
