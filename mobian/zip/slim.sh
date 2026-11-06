#!/bin/sh
# slim.sh ROOT: in an arm64 chroot of the image (binfmt qemu), take out what only development needed:
# compilers and -dev packages, the GTK example apps; and hide htop from the app grid. Run as root,
# on a copy, before scrub.sh. No autoremove: the -dev packages were what kept feedbackd, GSound,
# Qt's Wayland plugin, CUPS and tracker-extract installed, and autoremove would take those too.
set -eu
R=$1
[ -x "$R/usr/local/sbin/gts4lv-touchd" ] || { echo "not the gts4lv root: $R" >&2; exit 1; }
for d in proc sys dev; do mount --bind /$d "$R/$d"; done
trap 'for d in dev sys proc; do umount "$R/$d" 2>/dev/null || true; done' EXIT
ch() { chroot "$R" env DEBIAN_FRONTEND=noninteractive LC_ALL=C "$@"; }

# What goes (cpp stays: xrdb needs it, and gdm3 depends on that through x11-xserver-utils):
# the toolchains, every -dev package, the GTK demos (Demo, Node/Print Editor, Widget Factory),
# GNOME Breakout and Sublime Text (the owner's call, 2026-10-28)
PKGS=$(ch dpkg-query -W -f '${Package}\n' | grep -E '^(gcc|g\+\+|build-essential|dpkg-dev|make|cmake|meson|ninja-build|autoconf|automake|libtool|pkgconf|pkg-config)(-[0-9]+)?(-aarch64-linux-gnu)?$|-dev$|^gtk-4-examples$|^gnome-breakout$|^sublime-text$' || true)
echo "removing: $(echo $PKGS | wc -w) packages"
[ -z "$PKGS" ] || ch apt-get purge -y $PKGS
ch apt-get clean

# htop stays installed (Console) but out of the app grid
mkdir -p "$R/usr/local/share/applications"
{ cat "$R/usr/share/applications/htop.desktop"; echo NoDisplay=true; } > "$R/usr/local/share/applications/htop.desktop"
echo slimmed
