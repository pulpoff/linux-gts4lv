#!/bin/sh
# Mobian (Debian trixie + repo.mobian.org) with GNOME/GDM for the Tab S5e, as a
# tar on stdout, so it can be streamed straight onto the tablet without a copy
# on this disk:
#   ./build-rootfs.sh | ssh root@172.16.42.1 'tar -xpf - --xattrs --xattrs-include="*" -C /mnt/root'
# The kernel and modules come from ~/gts4lv/build, not from Debian.
set -eu
M=$HOME/gts4lv/mobian

PKGS="systemd-sysv udev dbus dbus-user-session systemd-timesyncd systemd-resolved
 kmod sudo openssh-server ca-certificates locales iproute2 iputils-ping less vim-tiny
 e2fsprogs htop i2c-tools
 mobian-base mobian-archive-keyring
 network-manager wpasupplicant iw wireless-regdb
 rmtfs tqftpserv qrtr-tools hexagonrpcd libqmi-glib5 libqrtr-glib0 libprotobuf-c1 python3-evdev
 gdm3 gnome-shell gnome-session gnome-settings-daemon gnome-control-center
 gnome-console nautilus gnome-text-editor gnome-keyring gnome-shell-extensions
 xwayland accountsservice polkitd upower iio-sensor-proxy
 libgl1-mesa-dri libegl-mesa0 mesa-vulkan-drivers
 adwaita-icon-theme fonts-cantarell fonts-dejavu-core gnome-backgrounds
 pipewire-pulse wireplumber bluez alsa-ucm-conf mesa-utils vulkan-tools
 v4l-utils libcamera-tools libcamera-ipa gstreamer1.0-libcamera gstreamer1.0-tools pipewire-libcamera"

exec sudo -n TMPDIR=$HOME/gts4lv/mobian/tmp mmdebstrap \
	--arch=arm64 --variant=important --format=tar \
	--components=main,non-free-firmware \
	--include="$(echo $PKGS | tr ' ' ',')" \
	--customize-hook="$M/customize.sh \"\$1\"" \
	--skip=cleanup/apt/lists \
	trixie - \
	"deb http://deb.debian.org/debian trixie main non-free-firmware" \
	"deb http://deb.debian.org/debian trixie-updates main non-free-firmware" \
	"deb http://security.debian.org/debian-security trixie-security main non-free-firmware" \
	"deb [signed-by=/usr/share/keyrings/mobian-archive-keyring.gpg] http://repo.mobian.org trixie main non-free-firmware"
