#!/bin/sh
# mmdebstrap customize hook: turn the chroot at $1 into the gts4lv Mobian rootfs.
set -eux
R=$1
K=/home/pulp/gts4lv/kit
ch() { chroot "$R" "$@"; }

echo gts4lv > $R/etc/hostname
printf '127.0.0.1\tlocalhost\n127.0.1.1\tgts4lv\n::1\tlocalhost ip6-localhost ip6-loopback\n' > $R/etc/hosts
# The initramfs maps rootfs.img with dm-linear and mounts it rw before switch_root.
echo "/dev/dm-0 / ext4 defaults,noatime 0 0" > $R/etc/fstab
ln -sf /usr/share/zoneinfo/Europe/Berlin $R/etc/localtime
echo "en_US.UTF-8 UTF-8" > $R/etc/locale.gen
ch locale-gen >/dev/null
echo "LANG=en_US.UTF-8" > $R/etc/default/locale

# Users: mobian/1234 as on every Mobian image; root by key only.
G=$(for g in sudo video audio input render netdev plugdev bluetooth; do ch getent group $g >/dev/null && printf %s, $g; done)
ch useradd -m -s /bin/bash -G "${G%,}" mobian
echo mobian:1234 | ch chpasswd
mkdir -p $R/root/.ssh $R/home/mobian/.ssh
cp /home/pulp/.ssh/id_ed25519.pub $R/root/.ssh/authorized_keys
cp /home/pulp/.ssh/id_ed25519.pub $R/home/mobian/.ssh/authorized_keys
chmod 700 $R/root/.ssh $R/home/mobian/.ssh
chmod 600 $R/root/.ssh/authorized_keys $R/home/mobian/.ssh/authorized_keys
ch chown -R mobian:mobian /home/mobian/.ssh

# --- USB network: the initramfs's gadget stays bound; give usb0 its address ---
mkdir -p $R/etc/NetworkManager/conf.d $R/etc/NetworkManager/system-connections $R/etc/modules-load.d
printf '[keyfile]\nunmanaged-devices=interface-name:usb*\n' > $R/etc/NetworkManager/conf.d/gts4lv-usb.conf
cat > $R/etc/systemd/system/gts4lv-usbnet.service <<'EOF'
[Unit]
Description=USB network to the dev box (172.16.42.1 on usb0)
Before=network.target ssh.service
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for n in usb0 usb1; do ip link set $n up; done; ip addr replace 172.16.42.1/24 dev usb0'
[Install]
WantedBy=multi-user.target
EOF

# --- tell the next initramfs this boot made it (it reads pstore) ---
cat > $R/etc/systemd/system/gts4lv-boot-ok.service <<'EOF'
[Unit]
Description=Mark this Mobian boot as good for the initramfs
After=gdm.service ssh.service gts4lv-usbnet.service
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo GTS4LV-MOBIAN-OK > /dev/kmsg'
[Install]
WantedBy=multi-user.target
EOF

# --- modem + Wi-Fi: firmware read-only from Android's partitions, rmtfs on RAM
# copies of the modem storage. Off until /etc/gts4lv/modem.enable exists. ---
mkdir -p $R/etc/gts4lv
install -m 755 /home/pulp/gts4lv/mobian/gts4lv-modem $R/usr/local/sbin/gts4lv-modem
install -m 755 /home/pulp/gts4lv/mobian/gts4lv-audio $R/usr/local/sbin/gts4lv-audio
install -m 644 /home/pulp/gts4lv/mobian/gts4lv-audio.service $R/etc/systemd/system/gts4lv-audio.service
cat > $R/etc/systemd/system/gts4lv-modem.service <<'EOF'
[Unit]
Description=Modem DSP (Wi-Fi firmware runs on it)
ConditionPathExists=/etc/gts4lv/modem.enable
# Not before NetworkManager or GDM: it takes ~6 s (partition copies, DSP boot), and wlan0
# simply appears to NetworkManager once the firmware is up.
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/gts4lv-modem
[Install]
WantedBy=multi-user.target
EOF
# Debian's units point rmtfs at the real partitions; ours starts its own.
ch systemctl mask rmtfs.service >/dev/null 2>&1 || true

# Wi-Fi networks, from the kit's wpa_supplicant fragment (secret, 0600).
awk -v d="$R/etc/NetworkManager/system-connections" '
	/ssid=/ { gsub(/.*ssid="|"$/, ""); s = $0 }
	/psk=/ { gsub(/.*psk="|"$/, ""); p = $0 }
	/priority=/ { gsub(/.*priority=/, ""); pr = $0 }
	/^}/ { f = d "/" s ".nmconnection"
	       print "[connection]\nid=" s "\ntype=wifi\nautoconnect-priority=" pr "\n\n[wifi]\nssid=" s "\nmode=infrastructure\n\n[wifi-security]\nkey-mgmt=wpa-psk\npsk=" p "\n\n[ipv4]\nmethod=auto\n\n[ipv6]\nmethod=auto" > f
	       close(f) }' $K/wifi.conf
chmod 600 $R/etc/NetworkManager/system-connections/*.nmconnection

# --- GNOME: on-screen keyboard always on, in GDM and in the session ---
mkdir -p $R/etc/dconf/profile $R/etc/dconf/db/local.d $R/etc/dconf/db/gdm.d
printf 'user-db:user\nsystem-db:local\n' > $R/etc/dconf/profile/user
printf 'user-db:user\nsystem-db:gdm\nfile-db:/usr/share/gdm/greeter-dconf-defaults\n' > $R/etc/dconf/profile/gdm
for d in local gdm; do
	printf '[org/gnome/desktop/a11y/applications]\nscreen-keyboard-enabled=true\n' > $R/etc/dconf/db/$d.d/00-gts4lv-osk
done
ch dconf update || echo "W: dconf update failed"

ch systemctl enable gts4lv-usbnet.service gts4lv-boot-ok.service gts4lv-modem.service gts4lv-audio.service ssh.service gdm.service >/dev/null
ch systemctl set-default graphical.target >/dev/null
# Root console on the USB gadget's ACM port (the host's /dev/ttyACM*): the way
# in when sshd does not come up. Same access the initramfs gives over the cable.
mkdir -p $R/etc/systemd/system/serial-getty@ttyGS0.service.d
printf '[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin root --noclear %%I 115200 vt220\n' \
	> $R/etc/systemd/system/serial-getty@ttyGS0.service.d/autologin.conf
ch systemctl enable serial-getty@ttyGS0.service >/dev/null

# --- touchscreen: userspace driver until a kernel one exists ---
install -m 755 /home/pulp/gts4lv/mobian/gts4lv-touchd $R/usr/local/sbin/gts4lv-touchd
echo uinput > $R/etc/modules-load.d/gts4lv.conf
echo i2c-qcom-geni >> $R/etc/modules-load.d/gts4lv.conf
cat > $R/etc/systemd/system/gts4lv-touchd.service <<'EOT'
[Unit]
Description=STM FTS touchscreen (userspace driver)
After=systemd-modules-load.service
Before=gdm.service
[Service]
EnvironmentFile=-/etc/gts4lv/touch.env
ExecStart=/usr/local/sbin/gts4lv-touchd
Restart=on-failure
RestartSec=2
[Install]
WantedBy=multi-user.target
EOT
ch systemctl enable gts4lv-touchd.service >/dev/null
install -m 755 /home/pulp/gts4lv/mobian/gts4lv-powerkey $R/usr/local/sbin/gts4lv-powerkey
install -m 644 /home/pulp/gts4lv/mobian/gts4lv-powerkey.service $R/etc/systemd/system/gts4lv-powerkey.service
ch systemctl enable gts4lv-powerkey.service >/dev/null

# ath10k's host-side feature file (linux-firmware, firmware-atheros). Only this
# one: Debian's board-2.bin would win over Samsung's bdwlan.bin.
install -D -m 644 /home/pulp/gts4lv/mobian/ath10k-WCN3990-firmware-5.bin $R/usr/lib/firmware/ath10k/WCN3990/hw1.0/firmware-5.bin

# Bluetooth: gts4lv-modem loads hci_uart once the firmware path is set.
echo "blacklist hci_uart" > $R/etc/modprobe.d/gts4lv-bt.conf
echo "blacklist venus_core" > $R/etc/modprobe.d/gts4lv-venus.conf
# BLE keyboards/mice (HOGP): bluetoothd needs /dev/uhid.
echo uhid > $R/etc/modules-load.d/gts4lv-uhid.conf
# LPASS LPI pins are dead until the ADSP runs (touching them hangs the bus);
# gts4lv-audio loads it once the ADSP is up.
echo "blacklist pinctrl_sdm670_lpass_lpi" > $R/etc/modprobe.d/gts4lv-audio.conf
# UCM for the card ("sdm660" driver, long name "Galaxy Tab S5e").
cp -r /home/pulp/gts4lv/mobian/ucm/Samsung $R/usr/share/alsa/ucm2/
mkdir -p $R/usr/share/alsa/ucm2/conf.d/sdm660
ln -sf ../../Samsung/gts4lv/gts4lv.conf "$R/usr/share/alsa/ucm2/conf.d/sdm660/Galaxy Tab S5e.conf"
# Speakers are silent with S24_LE (PipeWire's default pick); pin outputs to S16LE.
install -D -m 644 /home/pulp/gts4lv/mobian/51-gts4lv-s16.conf $R/etc/wireplumber/wireplumber.conf.d/51-gts4lv-s16.conf

# No suspend at all: it takes USB networking, Wi-Fi and Bluetooth down. The
# power key locks and blanks instead (GNOME's screensaver binding).
for d in local gdm; do
	cat > $R/etc/dconf/db/$d.d/01-gts4lv-nosuspend <<'EOT'
[org/gnome/settings-daemon/plugins/power]
sleep-inactive-ac-type='nothing'
sleep-inactive-battery-type='nothing'
power-button-action='nothing'

[org/gnome/settings-daemon/plugins/media-keys]
# The power key is gts4lv-powerkey's (blank, + lock when logged in).
power=@as []
EOT
done
for t in sleep suspend hibernate hybrid-sleep suspend-then-hibernate; do ln -sf /dev/null $R/etc/systemd/system/$t.target; done
mkdir -p $R/etc/systemd/logind.conf.d $R/etc/NetworkManager/conf.d
printf '[Login]\nHandlePowerKey=ignore\nHandleSuspendKey=ignore\nHandleLidSwitch=ignore\nIdleAction=ignore\n' > $R/etc/systemd/logind.conf.d/gts4lv-power.conf
printf '[connection]\nwifi.powersave=2\n' > $R/etc/NetworkManager/conf.d/gts4lv-wifi-powersave.conf
ch dconf update || true

# The kernel trusts only the upstream regulatory.db signing key, not Debian's.
ch update-alternatives --set regulatory.db /lib/firmware/regulatory.db-upstream

# 200% on the 2560x1600 panel, for the greeter and every new user.
for d in $R/var/lib/gdm3/.config $R/etc/skel/.config $R/home/mobian/.config; do
	install -D -m 644 /home/pulp/gts4lv/mobian/monitors.xml $d/monitors.xml
done
ch chown -R Debian-gdm:Debian-gdm /var/lib/gdm3/.config
ch chown -R mobian:mobian /home/mobian/.config

# systemd arms the APSS watchdog for shutdown; it kept running into ABL and
# reset it over and over until a key reset (2026-10-27).
mkdir -p $R/etc/systemd/system.conf.d
printf '[Manager]\nRuntimeWatchdogSec=off\nRebootWatchdogSec=off\nKExecWatchdogSec=off\nDefaultTimeoutStopSec=10s\n' > $R/etc/systemd/system.conf.d/gts4lv-no-watchdog.conf
mkdir -p $R/etc/systemd/user.conf.d
printf '[Manager]\nDefaultTimeoutStopSec=10s\n' > $R/etc/systemd/user.conf.d/gts4lv-stop.conf

# GPU: the Adreno is its own render-only DRM device (no MDSS yet), and Mesa pairs
# it with simpledrm through kmsro once simpledrm has a driver entry, so GNOME
# renders on FD615 instead of llvmpipe.
echo "options msm separate_gpu_kms=1" > $R/etc/modprobe.d/gts4lv-msm.conf
ln -sf libdril_dri.so $R/usr/lib/aarch64-linux-gnu/dri/simpledrm_dri.so

# GTK4 draws with Vulkan (Turnip), the fastest renderer on these Adrenos.
echo "GSK_RENDERER=vulkan" >> $R/etc/environment

# Boot splash: the red Debian swirl the initramfs draws, held by Plymouth until
# GNOME Shell owns the display (same theme and GDM handover as the Poco).
SP=/home/pulp/gts4lv/mobian/bootsplash
rm -rf $R/usr/share/plymouth/themes/yesman-debian
cp -r $SP/yesman-debian $R/usr/share/plymouth/themes/yesman-debian
install -m 644 $SP/plymouthd.conf $R/etc/plymouth/plymouthd.conf
install -D -m 644 $SP/plymouth-quit-gdm.conf $R/etc/systemd/system/plymouth-quit.service.d/gdm-owns-quit.conf
install -m 644 $SP/plymouth-quit-at-shell.service $R/etc/systemd/system/plymouth-quit-at-shell.service
ch systemctl enable plymouth-quit-at-shell.service >/dev/null

# Cameras: libcamera's software ISP allocates from a dma-buf heap. Only the system
# heap: the CMA one is 32 MB here and libcamera would pick it and run out.
echo system_heap > $R/etc/modules-load.d/gts4lv-dmaheap.conf
install -m 644 /home/pulp/gts4lv/mobian/70-gts4lv-dma-heap.rules $R/etc/udev/rules.d/70-gts4lv-dma-heap.rules
# libcamera 0.7.2 (+gts4lv1, GPU soft ISP, s5k3m5/s5k4ha sensor helpers) and PipeWire's libcamera
# plugin rebuilt against it (1.4.2-1+neo1): with trixie's 0.4 the CPU debayer stalls after a few
# frames. The debs are built on the tablet in /usr/src/neo; copy them to debs/ before a rootfs build.
D=/home/pulp/gts4lv/mobian/debs
if ls $D/libcamera0.7_*.deb >/dev/null 2>&1; then
	cp $D/*.deb $R/tmp/ && ch sh -c 'dpkg -i /tmp/*.deb && rm /tmp/*.deb' >/dev/null
	ch apt-mark hold libcamera0.7 libcamera-ipa gstreamer1.0-libcamera libcamera-tools libcamera-v4l2 libspa-0.2-libcamera iio-sensor-proxy >/dev/null
fi
install -D -m 644 -t $R/usr/share/libcamera/ipa/simple /home/pulp/gts4lv/mobian/libcamera-tuning/s5k3m5.yaml /home/pulp/gts4lv/mobian/libcamera-tuning/s5k4ha.yaml
install -D -m 644 /home/pulp/gts4lv/mobian/60-libcamera-softisp.conf $R/etc/environment.d/60-libcamera-softisp.conf
install -D -m 644 /home/pulp/gts4lv/mobian/60-gts4lv-cameras.conf $R/etc/wireplumber/wireplumber.conf.d/60-gts4lv-cameras.conf

# Sensors (accelerometer, light, compass) are behind the ADSP's SSC: iio-sensor-proxy reads them via libssc.
install -m 644 /home/pulp/gts4lv/mobian/81-gts4lv-ssc-sensors.rules $R/etc/udev/rules.d/81-gts4lv-ssc-sensors.rules
# hexagonrpcd 0.5 and libssc/ssccli are built on the tablet (trixie's hexagonrpcd 0.3 is too old
# for the sensors PD; its own units are masked, ours start when /dev/fastrpc-adsp appears).
S=/home/pulp/gts4lv/mobian/sensors
install -D -m 755 $S/bin/hexagonrpcd $R/usr/local/bin/hexagonrpcd
install -D -m 755 $S/bin/libhexagonrpc.so.0.5 $R/usr/local/lib/aarch64-linux-gnu/libhexagonrpc.so.0.5
ln -sf libhexagonrpc.so.0.5 $R/usr/local/lib/aarch64-linux-gnu/libhexagonrpc.so
install -m 755 $S/bin/ssccli $R/usr/bin/ssccli
install -m 755 $S/bin/libssc.so.2 $R/usr/lib/aarch64-linux-gnu/libssc.so.2
ln -sf libssc.so.2 $R/usr/lib/aarch64-linux-gnu/libssc.so
echo /usr/local/lib > $R/etc/ld.so.conf.d/hexagonrpc.conf
ch ldconfig
for u in hexagonrpcd hexagonrpcd-suspend hexagonrpcd-resume; do ln -sf /dev/null $R/etc/systemd/system/$u.service; done
install -m 644 $S/hexagonrpcd-adsp-rootpd.service $S/hexagonrpcd-adsp-sensorspd.service $S/gts4lv-sensors-setup.service $R/etc/systemd/system/
install -m 644 $S/82-gts4lv-hexagonrpcd.rules $R/etc/udev/rules.d/82-gts4lv-hexagonrpcd.rules
ch systemctl enable hexagonrpcd-adsp-rootpd.service hexagonrpcd-adsp-sensorspd.service gts4lv-sensors-setup.service >/dev/null
# libssc's socinfo, and sns_reg.conf pointing the hw revision at /sys/devices/soc0/ssc_hw_rev.
# The DSP libraries, configs and registry are the device's own: gts4lv-sensors-setup copies them
# from the dsp, vendor and persist partitions on first boot.
B=$R/usr/share/qcom/sdm670/Samsung/gts4lvwifi
install -D -m 644 -t $B/socinfo $S/socinfo/*
install -D -m 644 $S/sensors/sns_reg.conf $B/sensors/sns_reg.conf
install -m 755 $S/gts4lv-sensors-setup $R/usr/local/sbin/gts4lv-sensors-setup

# Debian stays the default boot: restore "boot-recovery" in misc (LineageOS on boot clears it).
install -m 755 /home/pulp/gts4lv/mobian/gts4lv-bootdefault $R/usr/local/sbin/gts4lv-bootdefault
install -m 644 /home/pulp/gts4lv/mobian/gts4lv-bootdefault.service $R/etc/systemd/system/gts4lv-bootdefault.service
ch systemctl enable gts4lv-bootdefault.service >/dev/null

# Settings > About: Samsung's bootloader overlay renames the DT model to "GTS4LVWIFI PROJECT";
# hostnamed (and so GNOME) prefers machine-info.
install -m 644 /home/pulp/gts4lv/mobian/machine-info $R/etc/machine-info

# The panel sometimes stays dark after the bootloader handover; a power-save cycle once the
# GDM greeter is up wakes it (the same as doing it by hand).
install -m 755 /home/pulp/gts4lv/mobian/gts4lv-panel-kick $R/usr/local/sbin/gts4lv-panel-kick
install -m 644 /home/pulp/gts4lv/mobian/gts4lv-panel-kick.service $R/etc/systemd/system/gts4lv-panel-kick.service
# Not enabled since 2026-10-28 (owner: the off-on blink after GDM is worse than the rare dark boot):
# ch systemctl enable gts4lv-panel-kick.service >/dev/null

# The owner's BT keyboard claims a mouse it does not have, so GNOME showed the pointer.
install -m 644 /home/pulp/gts4lv/mobian/71-gts4lv-bt-keyboard.rules $R/etc/udev/rules.d/71-gts4lv-bt-keyboard.rules
install -D -m 644 /home/pulp/gts4lv/mobian/gts4lv-local-overrides.quirks $R/etc/libinput/local-overrides.quirks

# "Android": an app that restarts into LineageOS (helper through pkexec; needs the DT's mode-android)
BA=/home/pulp/gts4lv/mobian/boot-android
install -m 755 $BA/gts4lv-boot-android $R/usr/local/sbin/gts4lv-boot-android
install -m 755 $BA/gts4lv-boot-android-app $R/usr/local/bin/gts4lv-boot-android-app
install -D -m 644 $BA/de.gts4lv.BootAndroid.desktop $R/usr/share/applications/de.gts4lv.BootAndroid.desktop
install -D -m 644 $BA/de.gts4lv.boot-android.policy $R/usr/share/polkit-1/actions/de.gts4lv.boot-android.policy
install -D -m 644 $BA/de.gts4lv.BootAndroid.svg $R/usr/share/icons/hicolor/scalable/apps/de.gts4lv.BootAndroid.svg
install -D -m 644 $BA/de.gts4lv.BootAndroid-symbolic.svg $R/usr/share/icons/hicolor/symbolic/apps/de.gts4lv.BootAndroid-symbolic.svg
ch gtk-update-icon-cache -q /usr/share/icons/hicolor || true
