#!/bin/sh
# snapshot.sh IMG [TABLET]: the public root file system, from the running dev tablet (ssh root@TABLET over
# USB, default 172.16.42.1) into a new ext4 image IMG, then slim.sh, scrub.sh, mobian's settings
# (dconf-mobian.ini next to IMG, made from the developer's dconf without personal keys), and shrunk
# to its content (the first boot grows it). Run as root. IMG must not exist.
#
# Mount it ONLY through this script: a service that starts while it is mounted (geoclue did) keeps
# its own copy of the mount, the loop device stays attached after umount, and a later mount then
# wrote the pre-shrink metadata back over the shrunk file system (2026-10-28). So the shrink refuses
# to run while any loop device still holds IMG.
set -eu
IMG=$1
T=${2:-172.16.42.1}
Z=$(dirname "$0")
M=$(dirname "$IMG")/mnt.$$
[ ! -e "$IMG" ] || { echo "$IMG exists" >&2; exit 1; }

truncate -s 12G "$IMG"
mke2fs -q -t ext4 -L mobian -b 4096 -I 256 "$IMG"
mkdir -p "$M"
mount -o loop,noatime "$IMG" "$M"
trap 'umount "$M" 2>/dev/null || true' EXIT

echo "== copying from $T"
# Only the running kernel's modules (the dev tablet keeps the previous trees for rollback), and no
# system sleep while the copy runs: gts4lv-powerkey honours a logind sleep inhibitor.
${SSHCMD:-ssh} root@$T 'cd / && X=$(for d in usr/lib/modules/*; do [ "${d##*/}" = "$(uname -r)" ] || echo "--exclude=./$d"; done) &&
	systemd-inhibit --what=sleep --who=snapshot.sh --why="image snapshot" \
	tar --one-file-system --xattrs --xattrs-include="*" --acls --numeric-owner -cf - $X \
	--exclude=./root/* --exclude=./root/.[!.]* --exclude=./home/pulp --exclude=./var/cache/apt/archives/*.deb \
	--exclude=./var/log/journal/* --exclude=./usr/src/neo --exclude=./opt/chromium-rpt \
	--exclude=./lost+found --exclude=./var/tmp/* \
	--exclude=./swapfile --exclude="./core.*" .' | tar -C "$M" --xattrs --xattrs-include='*' --acls --numeric-owner -xpf - || true
[ -d "$M/usr/lib/modules/$(${SSHCMD:-ssh} root@$T uname -r)" ] || { echo "the running kernel's modules are missing" >&2; exit 1; }
[ -x "$M/usr/local/sbin/gts4lv-touchd" ] || { echo "copy failed" >&2; exit 1; }

# The developer's own home is not copied, but two of the shell extensions enabled in dconf-mobian.ini
# were installed into it: ship them system-wide.
echo "== shell extensions"
${SSHCMD:-ssh} root@$T 'tar -czf - -C /home/pulp/.local/share/gnome-shell/extensions \
	hidetopbar@mathieu.bidon.ca just-perfection-desktop@just-perfection' |
	tar -C "$M/usr/share/gnome-shell/extensions" --no-same-owner -xzf -
chmod -R u=rwX,go=rX "$M/usr/share/gnome-shell/extensions"

echo "== slim"; sh "$Z/slim.sh" "$M"
echo "== scrub"; sh "$Z/scrub.sh" "$M"

echo "== mobian's settings"
D=$(mktemp -d); mkdir -p "$D/dconf"
[ -f "$M/home/mobian/.config/dconf/user" ] && cp "$M/home/mobian/.config/dconf/user" "$D/dconf/user"
XDG_CONFIG_HOME=$D dbus-run-session -- dconf load / < "$(dirname "$IMG")/dconf-mobian.ini"
install -D -o 1000 -g 1000 -m 644 "$D/dconf/user" "$M/home/mobian/.config/dconf/user"
rm -rf "$D"

echo "== shrink"
umount "$M"; rmdir "$M"; trap - EXIT
sleep 1
if losetup -a | grep -qF "($IMG)"; then
	echo "a loop device still holds $IMG (a service with a private copy of the mount?): not shrinking" >&2
	losetup -a | grep -F "($IMG)" >&2; exit 1
fi
e2fsck -fy "$IMG" >/dev/null || [ $? -le 1 ]
resize2fs -M "$IMG" >/dev/null 2>&1; resize2fs -M "$IMG" >/dev/null 2>&1
BC=$(dumpe2fs -h "$IMG" 2>/dev/null | awk -F: '/^Block count/{print $2+0}')
truncate -s $((BC * 4096)) "$IMG"
e2fsck -fn "$IMG"
ls -la "$IMG"
