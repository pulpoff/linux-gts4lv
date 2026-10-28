Linux 7.2.13 for the Samsung Galaxy Tab S5e (gts4lvwifi, SM-T720)
==================================================================

Mainline Linux for the Samsung Galaxy Tab S5e Wi-Fi (SM-T720, codename gts4lvwifi, Snapdragon 670 /
SDM670). It runs the tablet on **Mobian** (Debian trixie with Mobian's repository) with GNOME and GDM.

The tree is stable **v7.2.13**, plus the SDM670 work of
[sdm670-mainline/linux](https://gitlab.com/sdm670-mainline/linux) (tag `sdm670-v7.2.3_beta2`, brought up
to 7.2.13 with the stable incremental patches), plus the tablet changes listed below.

New in this version
===================

* **Sleep works.** The tablet suspends (s2idle) and the power key wakes it. Until now every suspend ended
  in a reboot: the bootloader leaves the watchdog running, `qcom-wdt` only stopped it for suspend when
  userspace had opened it, and the kernel's own pinging stops in s2idle, so the bark woke the tablet and
  the pretimeout panicked it. The image now sleeps two minutes after the screen goes off on battery, with
  Wi-Fi and Bluetooth off; see Power and reset.
* **Boot in 12 s instead of 27 s** (kernel 2 s, userspace 10 s). `qcom-cpufreq-hw` waits for the OSM L3
  interconnect provider, which was a module loaded about 10 s into boot: everything before it, the
  whole early userspace included, ran at the clock the bootloader left. It is built in now.

What works
==========

| Part | State |
| --- | --- |
| Display | 2560x1600 AMOLED on the MSM display engine, brightness with Samsung's gamma per level, fades |
| GPU | Adreno 615, OpenGL ES 3.2 / GL 4.6 and Vulkan (Turnip) |
| Touchscreen | STM FTS1BA90A, through a small userspace driver (`mobian/gts4lv-touchd.c`) |
| Wi-Fi | WCN3990, 2.4 and 5 GHz |
| Bluetooth | WCN3990 on uart6 |
| Sound | four CS35L41 speaker amps, both microphones |
| Battery | SM5705 fuel gauge (new driver), charging |
| USB | dual role: device mode (networking, serial) and host mode for OTG devices (see USB below) |
| Cameras | rear S5K3M5 13 MP and front S5K4HA 8 MP (new driver), through libcamera's software ISP |
| Video | Venus hardware H.264 and HEVC decode and encode, 1080p60, several sessions at once |
| Sensors | accelerometer (rotation), magnetometer, ambient light, via the ADSP sensor core and libssc |
| Cover | hall sensor as a lid switch |
| Buttons | power (sent as KEY_SCREENLOCK), volume |
| Suspend | s2idle, woken by the power key or a charger |

Not done: rear camera autofocus, the cameras' Samsung colour tuning, the SoC's deepest sleep (in suspend
the CPUs power down, but the chip does not yet reach AOSS sleep or CX collapse), 24-bit audio playback
(16-bit only), and USB fast charging. The
cameras can stop delivering frames after many starts and stops; that is being looked at. OTG host mode is
in place but not yet tested with a device.

Booting it
==========

The tablet keeps LineageOS. Linux goes into the **recovery** partition; the Debian root filesystem is a
file inside LineageOS's data partition, mapped with dm-linear by the initramfs without mounting it. The
bootloader starts recovery while the `misc` partition asks for it, which a small Debian service keeps
set. Samsung's bootloader needs LineageOS's own `recovery_dtbo` next to the kernel: it refuses any
overlay that does not apply, so the DTB exports the labels that overlay refers to as empty nodes
(`sdm670-samsung-gts4lvwifi-dtbo-mask.dtsi`).

The kernel is `Image.gz` with the DTB appended, built from `arch/arm64/configs/defconfig` plus
`sdm670.config`.

Boot with `fw_devlink.sync_state=timeout`. The GPU's GMU is set up by the Adreno driver and never gets a
driver of its own, so GCC's and GPUCC's sync_state never runs, and without the timeout every power
domain the bootloader left on (UFS, the SMMU TBUs, camera, video) stays on for good.

Changes for the Galaxy Tab S5e
==============================

Device tree
-----------

* **`sdm670-samsung-gts4lvwifi.dts`**, new: display (dual DSI, command mode), GPU with its zap shader,
  Wi-Fi, Bluetooth, the ADSP with audio (four CS35L41 amps on a TDM bus, the PM660L analog codec), the
  modem DSP for Wi-Fi, cameras on CAMSS (CSIPHY0/1, CCI), Venus, the SM5705 fuel gauge and charger on
  bit-banged I2C buses, buttons, the hall sensor, and the regulators.
* **Sensor power.** The sensor chips run from an LDO switched by TLMM GPIO 29 (stock
  `qcom,gpio-sensor-ldo`). The ADSP drives the sensors but does not vote for their PMIC rails: Samsung's
  kernel keeps L17 and L18 always on with no consumer, and the ambient light sensor does not probe
  without L18. Note for anyone copying the voltages: PM660 PLDOs step 8 mV from 1.664 V; a value that is
  not a step (2.95 V) fails the whole regulator node, eMMC I/O rail included.
* **Protected clocks** (GCC_QSPI_*): touching them resets the SoC within a second of boot.
* **Reserved GPIOs** 0-3 and 81-84 (secure world).

Display: Samsung AMSA05RB01 panel behind an Anapass ANA38401 (`panel-samsung-ana38401`, new)
--------------------------------------------------------------------------------------------

* 2560x1600, two 1280x1600 halves on two DSI links, command mode, continuous HS clock (the TCON runs
  off it; without it frames tear). Samsung addresses DSI0 only, so `qcom,sync-dual-dsi` must stay off.
* Takes over the panel the bootloader left running (`handover`, default on): a cold start from reset
  stays black with this DSI host.
* Brightness: AID (emission duty) plus a gamma (83h) per luminance step, from Samsung's smart-dimming
  algorithm run offline (`panel-samsung-ana38401-gamma.h`). 83h has to be rewritten before every gamma
  latch. `min_level` sets the lowest level; changes fade over about half a second (`fade`).
* Off from probe until the first enable(): systemd restoring the saved brightness before the DSI link
  was up flashed the panel bright, with lines over half of it.
* Off and on: from disable() to enable() a brightness change is only stored, and a fade still running is
  cancelled. A DCS write while the link was going down failed (-110), and every later commit then waited
  out a "ctl start" timeout: about 90 ms a frame, the whole UI at 5 fps.
* debugfs `dcs` file for DCS reads and writes during bring-up.
* `dispcc-sdm845`: the MDP clock source uses the no-init-park ops so the running display survives
  probe.

Cameras
-------

* **S5K4HA** (`drivers/media/i2c/s5k4ha.c`, new): 8 MP front camera, 4 lanes, RAW10, 3264x2448 and
  1632x1224. Register tables from the tablet's camera module data and Samsung's fimc-is driver.
* **S5K3M5**: a `samsung,s5k3m5-gts4lv` variant with the tablet's own register tables
  (`s5k3m5-gts4lv.h`): with mainline's (SM8550) tables the sensor streams but this CSIPHY receives
  nothing. 4128x3096 and 1008x756. Also fixes `get_selection` returning the width as the height.

Audio
-----

* **cs35l41**: `cirrus,asp-rx-slots` property. Four amps share one TDM bus and each needs its own slot;
  Android sets them from mixer paths, and there is no mixer control for it.

USB
---

* **Dual role.** The DWC3 port is `dr_mode = "otg"` with `usb-role-switch`, device by default, and the role is
  set from userspace through `/sys/class/usb_role`. The Type-C/PD controller that notices an OTG adapter, an
  S2MM005, has no mainline driver, and VBUS for a device comes from the SM5705 charger's boost. Both are
  driven over I2C by a small daemon that ships with the image (it reads the S2MM005's attach and data-role
  state, and refuses host mode while a charger supplies VBUS).
* **Modules for OTG devices**, in `sdm670.config`: USB Wi-Fi (rtw88, rtw89, rtl8xxxu, mt76, rt2800, ath9k_htc,
  carl9170), USB Ethernet and RNDIS/QMI modems, storage with UAS, printers, CH341/PL2303 serial, sound cards
  (USB audio), webcams (UVC), Bluetooth RFCOMM/BNEP, and HID drivers for keyboards, mice and game pads (xpad).

Video: Venus
------------

* **One video core.** SDM670 shares SDM845's Venus resources, which list two video cores, but this firmware
  has one: a session placed on core 2 is refused (session error 0x100e on `CONFIG_VIDEOCORES_USAGE`). With
  one session open, every second one failed, which broke any player that probes before it plays (Chromium).
  On SDM670 every session goes to core 1.
* **hfi_parser.** The `parse_alloc_mode` / `parse_caps` / `parse_raw_formats` size checks that came with the
  v7.2.13 stable patches are reverted: with them this HFI 4xx firmware's property list is walked at the wrong
  offsets, and every encoder and decoder session fails.
* VP8 and VP9 decode are hidden by default (`venus_dec.vpx=1` shows them). On this firmware VP8 decode crashes
  the video core and VP9 decode never returns a frame, and a browser that sees them offered picks them over
  H.264.

Power and reset
---------------

* **SM5705 fuel gauge and charger status** (`drivers/power/supply/sm5705_battery.c`, new, read-only):
  capacity, voltage, current, charging state, and charge, energy and power (`charge_now` from the gauge's
  1/256 % state of charge; energy at the battery's nominal 3.85 V, 27.1 Wh), which UPower needs for the
  discharge rate and GNOME's "time remaining". The chips charge on their own.
* **Suspend** (s2idle; the SoC offers no deeper state to Linux):
  * **qcom-wdt** stops the watchdog for suspend, and starts it again on resume, whenever it runs, not only
    when userspace opened it. The bootloader leaves it running, the watchdog core pings it from a kernel
    worker, and that worker does not run in s2idle.
  * **RPMh sleep stats**: `qcom,sdm845-rpmh-stats` at 0xc3f0000 (`/sys/kernel/debug/qcom_stats`: aosd,
    cxsd, and each subsystem's sleep). This needed the AOSS QMP node cut down to the 0x400 bytes of its
    message RAM, as on SDM845; it covered 1 MiB, the stats included.
  * Userspace has to stop the FastRPC servers (hexagonrpcd) before suspend. Frozen in the middle of a call,
    they crashed the ADSP's sensor process, and its recovery ended in a NoC error and a hang.
* **CPU frequency from the start**: `CONFIG_INTERCONNECT_QCOM_OSM_L3=y` (see New in this version).
* **Restart into Android**: `mode-android` (Samsung's NORMALBOOT, 0x14) on the PON, so `systemctl reboot
  --reboot-argument=android` starts LineageOS from the boot partition; every other restart asks for
  RECOVERY, where this system lives. Userspace also clears "boot-recovery" from `misc` (see
  `mobian/boot-android`).
* **qcom-pon**: `qcom,secondary-pon-reset` makes PM660L shut down on PS_HOLD instead of keeping its
  state across a reboot, as Samsung's kernel does; and every restart is programmed as a hard reset.
  Samsung's bootloader looped on warm resets after Linux had driven the hardware.

Other
-----

* **CPU name**: `cpu-model-name` under `/cpus` gives `/proc/cpuinfo` a "model name" line (Qualcomm
  Snapdragon 670), which system information tools show.
* **ath regdomain**: `ath.regdomain=<country>` overrides the country of the Wi-Fi calibration data.
* **Bring-up switches**: `icc_ignore_unused` keeps the bootloader's interconnect votes, and
  `pd_ignore_unused` also holds at genpd sync_state. Neither is needed on a normal boot.
* `msm_dsi_host_cmd_rx` prints the bytes of an invalid response.
* **Config** (`sdm670.config`): 1000 Hz ticks for touch latency, zram with zstd for compressed swap, and
  netconsole plus the soft/hard lockup and hung task detectors for catching crashes over the USB network.

Outside the kernel
==================

The image also needs, in userspace: the touchscreen daemon (it also does double tap to wake), the power key
daemon (blank and lock, and the sleep policy: two minutes after the screen goes off on battery, not while
audio plays, Wi-Fi, Bluetooth, the sensor stack and touch off for the sleep), the USB role daemon, LineageOS's firmware (read from the tablet's own partitions at boot), hexagonrpcd and libssc for the
sensors, and a patched iio-sensor-proxy (a sensor claimed while it was still being opened never started; the
light level is smoothed). They are in [`mobian/`](mobian/), with the scripts that build the image and U-Boot's boot menu.
