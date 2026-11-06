#!/bin/sh
# scrub.sh ROOT: turn a copy of the dev tablet's root file system into a shareable image
# (run as root, on a COPY). The system and every installed app stay; this tablet's credentials,
# pairings and personal data and the developer account (pulp) go. The public account is mobian
# (password 1234), with pulp's GNOME settings minus the personal keys. Yesman ships installed and
# signed out: no session of any account in the image.
set -eu
ROOT=$1
[ -x "$ROOT/usr/local/sbin/gts4lv-touchd" ] || { echo "not the gts4lv root: $ROOT" >&2; exit 1; }
cd "$ROOT"

# --- credentials and pairings of this tablet
rm -f etc/NetworkManager/system-connections/*            # Wi-Fi networks and their passwords
rm -f etc/NetworkManager/conf.d/*wifi-mac*                # this tablet's Wi-Fi MAC (90-gts4lv-wifi-mac.conf)
rm -rf var/lib/NetworkManager/*                          # leases, secret key, timestamps
rm -rf var/lib/bluetooth/*                               # paired devices (the BT keyboard...)
rm -f etc/ssh/ssh_host_*                                 # ssh-keygen -A makes new ones (drop-in below)
mkdir -p etc/systemd/system/ssh.service.d
printf '[Service]\nExecStartPre=\nExecStartPre=/usr/bin/ssh-keygen -A\nExecStartPre=/usr/sbin/sshd -t\n' \
	> etc/systemd/system/ssh.service.d/gts4lv-hostkeys.conf
rm -f etc/systemd/system/multi-user.target.wants/ssh.service   # no sshd unless the owner turns it on
rm -f root/.ssh/authorized_keys home/mobian/.ssh/authorized_keys
: > etc/machine-id                                       # a new one on first boot
rm -f var/lib/dbus/machine-id var/lib/systemd/random-seed var/lib/systemd/credential.secret

# --- the developer account (pulp)
P=$(sed -n 's/^pulp:[^:]*:\([0-9]*\):.*/\1/p' etc/passwd)
if [ -n "$P" ]; then
	sed -i '/^pulp:/d' etc/passwd etc/shadow
	[ -f etc/gshadow ] && sed -i '/^pulp:/d' etc/gshadow
	for f in etc/group etc/gshadow etc/passwd- etc/shadow- etc/group- etc/gshadow-; do
		[ -f $f ] && sed -i -e '/^pulp:/d' -e 's/\([:,]\)pulp\(,\|$\)/\1\2/g; s/,,/,/g; s/:,/:/; s/,$//' $f
	done
	sed -i '/^pulp:/d' etc/subuid etc/subgid 2>/dev/null || true
	rm -f var/lib/AccountsService/users/pulp var/lib/AccountsService/icons/pulp
	grep -rl '^pulp[[:space:]]' etc/sudoers etc/sudoers.d 2>/dev/null | xargs -r sed -i '/^pulp[[:space:]]/d'
	rm -rf home/pulp var/mail/pulp var/spool/cron/crontabs/pulp var/lib/systemd/linger/pulp
	find var/lib/gts4lv-lock etc/gts4lv-lock -name 'pulp*' -delete 2>/dev/null || true
	rm -rf var/lib/gdm3/.local/state/* 2>/dev/null || true
	# Phosh's drop-in ran it as pulp (not enabled: GDM runs the session) and CUPS' subscriptions are pulp's
	sed -i 's/^User=pulp$/User=mobian/; s/^# The tablet user is pulp (1001), not mobian\.$/# The tablet user is mobian./' etc/systemd/system/phosh.service.d/gts4lv.conf 2>/dev/null || true
	rm -f etc/cups/subscriptions.conf etc/cups/subscriptions.conf.O
fi

# --- mobian: a known password, the settings dconf (made by the caller from pulp's), clean dirs
HASH=$(openssl passwd -6 1234)
sed -i "s|^mobian:[^:]*:|mobian:$HASH:|" etc/shadow
H=home/mobian
rm -rf $H/.local/share/recently-used.xbel $H/.local/share/Trash $H/.local/share/keyrings \
       $H/.local/share/evolution $H/.config/evolution $H/.config/goa-1.0 $H/.local/share/tracker3 \
       $H/.local/share/gvfs-metadata $H/.local/state/wireplumber $H/.local/share/gnome-shell/notifications \
       $H/.bash_history $H/.lesshst $H/.python_history $H/.cache $H/.config/yesman/device-id
for d in Pictures Videos Downloads Documents Music; do mkdir -p $H/$d; done

# --- Yesman signed out: no seeded session (api/auth.js _importSeed reads /etc/yesman/demo-session.json)
rm -rf etc/yesman $H/.config/yesman $H/.local/share/keyrings

# --- root's leftovers from development (backups of daemons and modules, notes, history)
find root -mindepth 1 -maxdepth 1 ! -name .bashrc ! -name .profile -exec rm -rf {} +

# --- crash dumps left in / (core.4661 from 2026-10-27)
rm -f core core.[0-9]*

# --- logs, caches, temp
rm -rf var/log/journal/* var/lib/systemd/coredump/* var/lib/systemd/pstore/* tmp/* var/tmp/*
find var/log -type f \( -name '*.log' -o -name '*.gz' -o -name '*.[0-9]' \) -delete 2>/dev/null || true
: > var/log/wtmp; : > var/log/btmp; : > var/log/lastlog 2>/dev/null || true
rm -f var/cache/apt/archives/*.deb var/cache/apt/*.bin
chown -R 1000:1000 $H 2>/dev/null || true
echo scrubbed
