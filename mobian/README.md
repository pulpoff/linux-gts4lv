Mobian image for the Galaxy Tab S5e (gts4lvwifi, SM-T720)
==========================================================

The userspace half of running this kernel on the tablet: the scripts that build the Debian (Mobian) root
filesystem and the boot image, and the daemons and configuration the hardware needs. The kernel is the
rest of this repository; see the top-level README.md.

The scripts were written for one build machine and expect this layout (paths are absolute):
`~/gts4lv/mobian` (this folder), `~/gts4lv/kit` (`kit/` here), the kernel built in
`~/gts4lv/build-7.2.13`, and U-Boot in `/usr/src/uboot-gts4lv`. Adjust the paths before using them
elsewhere.

Building
========

1. `build-rootfs.sh` debootstraps Debian trixie with Mobian's repository (`mobian.gpg`) and runs
   `customize.sh` in it: users, GNOME/GDM, firmware, the daemons and configuration below. The result is
   `rootfs.tar`, which goes into a file inside LineageOS's data partition.
2. `kit/build.sh` (with `B=` pointing at the kernel build) builds the initramfs (`kit/init`: it maps the
   root filesystem file with dm-linear from a FIEMAP extent list, no mounting of the data partition) and
   the recovery partition: `kit/mkrecpart.py` puts U-Boot at 0, LineageOS's own recovery at 4 MiB and
   Linux at 34 MiB. Write `out/recovery-uboot.img` to the recovery partition (from a running Linux:
   `dd of=/dev/disk/by-partlabel/recovery`).
3. Wi-Fi networks for the image come from `kit/wifi.conf` (a wpa_supplicant fragment, not included).

U-Boot (`uboot/`)
=================

Built from [sm7150-mainline/u-boot](https://github.com/sm7150-mainline/u-boot) with
`make qcom_defconfig gts4lv.config`. `gts4lv.env` is the boot menu and the Linux command line:

* Linux with `fw_devlink.sync_state=timeout` (the GPU's GMU never gets a driver, so GCC's sync_state never
  runs without it, and every power domain the bootloader left on stays on), `clk_ignore_unused` and
  `regulator_ignore_unused` (still needed: msm is a module, and the panel must survive until it loads).
* Volume Down at boot opens the menu: Linux, Android, Android recovery, rescue shell, fastboot, USB mass
  storage, serial gadget, U-Boot shell, reset, power off. The panel is in DSI command mode and U-Boot does
  not push frames to it, so the menu is on the serial console only for now.

`simplefb-memory-region.patch` lets U-Boot's simplefb take its framebuffer from a `memory-region`.

Daemons and services
====================

| File | What it does |
| --- | --- |
| `gts4lv-touchd.c` | Userspace driver for the STM FTS1BA90A touchscreen over i2c-dev, as a multitouch uinput device. SIGUSR1: low power with double tap to wake; SIGUSR2: normal; SIGHUP: sensing off (system sleep). Build with `aarch64-linux-gnu-gcc -O2`. |
| `gts4lv-powerkey` | Power key: blank and lock, wake with a fade; volume keys follow the screen rotation; the cover's hall sensor. Also the **sleep policy**, below. |
| `gts4lv-usb-otgd` | USB role: reads the S2MM005 Type-C controller's attach state over I2C and switches the DWC3 port between device and host, with VBUS from the SM5705 boost. |
| `gts4lv-usb-mode` | Sets the USB role by hand (`host`, `device`) and the SM5705's boost or charge mode with it. |
| `gts4lv-audio` | Starts the ADSP after GDM and loads the LPASS LPI pinctrl once it runs (its registers are dead until then). |
| `gts4lv-modem` | Starts the modem DSP, which Wi-Fi needs. |
| `gts4lv-bootdefault` | Keeps the `misc` partition asking for recovery, so the tablet boots Linux. |
| `gts4lv-panel-kick` | Old workaround for a black panel at boot; disabled in the image. |
| `gts4lv-autorotate` | Old rotation helper for when mutter left touch mode; not installed any more (the cause was the a11y keyboard setting, see below). |
| `boot-android/` | The **Android** app: one GNOME confirmation, then `gts4lv-boot-android` (root, through pkexec; polkit lets the user at the tablet run it without a password) clears "boot-recovery" from `misc`, tells `gts4lv-bootdefault` to leave it at shutdown, and restarts with `--reboot-argument=android` (the DT's `mode-android`). Back from LineageOS: Restart → Recovery. |
| `touchboost/` | Raises the CPU frequency floor while a finger is down. |
| `sensors/` | hexagonrpcd units and the sensor registry for the ADSP sensor core (libssc, iio-sensor-proxy). |
| `lock/`, `pattern-lock@gts4lv/` | Pattern lock for GDM and the lock screen (GNOME Shell extension, PAM helper, polkit action). |
| `bootsplash/` | Plymouth theme. |
| `ucm/`, `51-gts4lv-s16.conf` | ALSA UCM for the four CS35L41 amps and the PM660L codec; 16-bit output. |
| `libcamera-tuning/`, `*.patch` | Tuning files for the two cameras and patches for libcamera's PipeWire plugin and iio-sensor-proxy. |

Sleep
-----

`gts4lv-powerkey` suspends the tablet (s2idle) once the screen has been off for two minutes on battery:
after the power key, or after GNOME's own blank timeout. It does not while audio plays or while a logind
`sleep` inhibitor in block mode is held. Before sleeping it blocks Wi-Fi and Bluetooth (only those that
were on), stops iio-sensor-proxy and both hexagonrpcd servers (frozen in the middle of a call they crashed
the ADSP), and turns touch sensing off. The power key wakes it, and so does a charger. Woken by anything
else with the screen still off, it sleeps again after 10 s. The power key's `wakeup_count` never moves, so
the daemon tells a key wake from the key's interrupt count. Test overrides (environment):
`GTS4LV_SLEEP_AFTER`, `GTS4LV_SLEEP_ON_CHARGER=1`, `GTS4LV_SLEEP_ALARM` (RTC wake after N s),
`GTS4LV_RESLEEP_AFTER`, `GTS4LV_SLEEP_DEBUG=1` (what is still on at suspend, in `/run/gts4lv-sleep.txt`).
On the kernel side this needed the qcom-wdt fix described in the top-level README.

Not yet in customize.sh
=======================

Set up by hand on the tablet and still to be folded into the image:

* `91-gts4lv-mutter-idle.conf` in `/etc/environment.d` (`MUTTER_DEBUG_RESET_IDLETIME=1`, which the power key
  daemon's wake needs).
* `org.gnome.desktop.a11y.keyboard enable=false` for the user: with it on, mutter creates a virtual pointer,
  leaves touch mode, and auto-rotation goes away.
* A fixed Wi-Fi MAC (the tablet's own, from the EFS partition) in NetworkManager, so the address stays the
  same.
* The power key daemon's sleep and the touch daemon's SIGHUP (both above).
