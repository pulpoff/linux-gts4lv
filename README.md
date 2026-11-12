Linux 7.2.15 for the Samsung Galaxy Tab S5e (gts4lvwifi, SM-T720)
==================================================================

Mainline Linux for the Samsung Galaxy Tab S5e Wi-Fi (SM-T720, codename gts4lvwifi, Snapdragon 670 /
SDM670). It runs the tablet on **Mobian** (Debian trixie with Mobian's repository) with GNOME and GDM.

The tree is stable **v7.2.15**, plus the SDM670 work of
[sdm670-mainline/linux](https://gitlab.com/sdm670-mainline/linux) (tag `sdm670-v7.2.3_beta2`, brought up
to 7.2.15 with the stable incremental patches), plus the tablet changes listed below.

7.2.15 is the last 7.2 stable release: kernel.org has marked 7.2 end of life, and this tree stays on it.
Use the `main` branch.

New in this version
===================

November 12:

* **The Book Cover Keyboard is on.** Its device tree nodes are now in the DTB by default, and gpio0-2 are
  no longer reserved (Samsung's kernel drives them on this tablet for the keyboard). The keyboard sits
  behind a Lattice FPGA whose bitstream is Samsung's and is not in this tree: the driver asks for
  `samsung/gts4lv/pogo_fpga.fw`, and the image copies it at boot from the LineageOS recovery already on
  the tablet. Not yet tried with a keyboard on a tablet.
* **Replugging HDMI no longer freezes both screens** (written for the freeze after a docked panel-off,
  undock and redock): the display engine reserves its mixers again only on a modeset, not whenever a gamma
  or colour matrix changes. Not yet tested on the tablet. See Display: DisplayPort over USB-C.
* **The speakers work with no TV connected.** With the HDMI device in the UCM profile, PipeWire dropped the
  whole profile, speakers included, whenever a session started without a TV. On the tablet since
  November 10. See Audio.
* **Plugging in or pulling out a cable wakes the tablet** and changes the charging state at once, not at the
  next 30 s poll: the SM5705 charger's interrupt is wired up. The interrupt itself has run on the tablet
  since November 10; a wake from sleep by the cable is not verified yet. See Power and reset.
* **About 35 MiB more memory.** Carve-outs for Samsung's splash, logs and panel driver, and the camera
  ICP firmware, which nothing in this kernel reads, are given back. On the tablet since November 9. See
  Device tree.

The previous update (Linux 7.2.15, the eMMC bus clock, the panel's power in sleep) is commit ed62044b5:

* **Linux 7.2.15.** 7.2.14 and 7.2.15 change only core code (io_uring, keys, signals and exit, tracing, the
  scheduler, Bluetooth, SMB, exec), none of the tablet's drivers.
* **Sleep, continued (November 7 and 8).**
  * The eMMC controller's bus clock (`sdhci-msm`) was switched on at probe and never off, and through
    GPLL0 it held the 19.2 MHz crystal in sleep. It now follows the controller's runtime PM.
  * Blanking powers the panel off (display off, reset, supply off, as downstream does): kept powered, the
    panel drew about 90 mA more, in suspend too. With the panel off, the tablet drew about 47 mA awake and
    about 20 mA in s2idle (radios and sensors off as the power key daemon leaves them). "Display on" now
    goes out a few frames after the panel is enabled, and again until the panel reports it on: sent before
    the first frame, it was lost, and a cold wake stayed black. **But** on this panel the first picture after
    such a cold wake still comes up broken (the two halves skewed) or black, so the tablet runs with
    `panel_samsung_ana38401.keep_power=1` for now (see Display), and asleep it draws about 190 mA, about
    2.7 % an hour.
  * Linux's own RPMh sleep votes are clean in s2idle (the crystal, CX, MX and every bus at 0), and the
    ADSP and the modem sleep the whole time, yet the SoC still never reaches AOSS sleep or CX collapse
    (`aosd`, `cxsd` stay 0). What holds it is outside those votes and not found yet.
* November 9: **HDMI out over USB-C, with sound.** The USB-C port's DisplayPort alt mode drives a TV through a
  USB-C-to-HDMI dock: tested on an LG TV through a Parade PS176 dock at 4096x2160 30 Hz and 1920x1080
  120 Hz, and the TV plays the tablet's sound. GDM shows its login on the TV when it is connected at boot,
  the touchscreen can drive the TV, and while docked the power key only switches the panel off and on. See
  Display: DisplayPort over USB-C, and Audio.
* **Book Cover Keyboard driver** (`stm32-pogo-kbd`, new). Its device tree nodes then stayed out of the DTB
  unless it was built with `-DGTS4LV_POGO_KEYBOARD`; they are on since November 12.
* Earlier (November 6): **sleep costs less.** Asleep, the tablet drew 220-250 mA: the ADSP never power-collapsed, and
  the SoC's 19.2 MHz crystal never stopped. The digital codec's MCLK and the speaker link's bit clock held
  the ADSP, and a clock leaked by the touchscreen's I2C bus held the crystal; all three are fixed (see
  Power and reset and Audio). The SM5705 fuel gauge's poll no longer counts as a wakeup event either.
  Before, the battery dropped 2.2-3 % an hour in long sleeps. A first two-hour run with the ADSP fixes
  lost 2 % in all, and the ADSP now sleeps through the suspend.
* Earlier (October 28): sleep works at all (`qcom-wdt`), and boot takes 12 s instead of 27 s (OSM L3
  built in). See Power and reset.

What works
==========

| Part | State |
| --- | --- |
| Display | 2560x1600 AMOLED on the MSM display engine, brightness with Samsung's gamma per level, fades |
| HDMI | DisplayPort alt mode on the USB-C port, through a USB-C-to-HDMI dock: up to 4096x2160 30 Hz, with sound |
| GPU | Adreno 615, OpenGL ES 3.2 / GL 4.6 and Vulkan (Turnip) |
| Touchscreen | STM FTS1BA90A, through a small userspace driver (`mobian/gts4lv-touchd.c`) |
| Wi-Fi | WCN3990, 2.4 and 5 GHz |
| Bluetooth | WCN3990 on uart6 |
| Sound | four CS35L41 speaker amps, both microphones |
| Battery | SM5705 fuel gauge (new driver), charging; the USB daemon sets the charger's input limit from the USB-C current or the PD contract (5 V up to 3 A, or 9 V at 1.65 A, as stock), with a guard on the battery's own thermistor |
| USB | dual role: device mode (networking, serial) and host mode for OTG devices (see USB below) |
| Cameras | rear S5K3M5 13 MP and front S5K4HA 8 MP (new driver), through libcamera's software ISP |
| Video | Venus hardware H.264 and HEVC decode and encode, 1080p60, several sessions at once |
| Sensors | accelerometer (rotation), magnetometer, ambient light, via the ADSP sensor core and libssc |
| Cover | hall sensor as a lid switch |
| Buttons | power (sent as KEY_SCREENLOCK), volume |
| Suspend | s2idle, woken by the power key; the ADSP and the modem sleep through it |

Not done: rear camera autofocus, the cameras' Samsung colour tuning, the SoC's deepest sleep (in suspend
the CPUs power down, but the chip does not yet reach AOSS sleep or CX collapse), 24-bit audio playback
(16-bit only), and a clean picture after the panel was powered off in a blank (see Display). A cable
plugged in while the tablet sleeps should now wake it; that is not verified yet. Of the charging cases, 9 V
from a dock was checked; 5 V at 3 A from a PD charger was not. The
cameras can stop delivering frames after many starts and stops; that is being looked at. OTG host mode is
in place but not yet tested with a device. The Book Cover Keyboard has not been tried on a tablet yet. HDMI:
only one plug orientation of the USB-C cable has been tried, the tested dock needs 24 bits per pixel
(`msm.dp_max_bpp=24`, no picture at 30), and the fix for both screens freezing when the dock was unplugged
and plugged in again after the panel was switched off is in this tree but not yet tested on the tablet.

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
* **Reserved GPIOs** 3 and 81-84 (secure world). gpio0-2 were reserved too until the keyboard needed them
  (below).
* **Memory carve-outs given back.** Samsung's kernel reserves 35 MiB at 0x9c000000 for its splash, plus
  regions for its own logs and panel driver (dfps-data, ss-plog, sec-debug and its autocomment page);
  `sdm670.dtsi` reserves 5 MiB for the camera ICP firmware. Nothing in this kernel reads any of them. The
  framebuffer region keeps the 16 MiB the boot frame needs (2560x1600 at 4 bytes a pixel); about 35 MiB
  goes back to the kernel.
* **Charger interrupt.** The SM5705 charger's nINT is TLMM gpio22: open drain, active low, with its own
  pull-up. The TLMM comes out of reset with a pull-down on that pin, which holds it low (a storm of level
  interrupts), so the pin state sets bias-disable, as Samsung's kernel does.
* **Book Cover Keyboard (EJ-FT720)**: the keyboard's MCU on QUP0 SE0 I2C, its Lattice FPGA bridge on QUP1
  SE0 SPI, and their supplies and pins.

Display: Samsung AMSA05RB01 panel behind an Anapass ANA38401 (`panel-samsung-ana38401`, new)
--------------------------------------------------------------------------------------------

* 2560x1600, two 1280x1600 halves on two DSI links, command mode, continuous HS clock (the TCON runs
  off it; without it frames tear). Samsung addresses DSI0 only, so `qcom,sync-dual-dsi` must stay off.
* `handover` (off by default) takes over the panel the bootloader left running at probe instead of starting
  it from reset; the tablet runs with it on.
* Blank powers the panel off by default (display off, reset, supply off, as downstream does). `keep_power=1`
  blanks with display off only and keeps the TCON and panel powered, about 90 mA more, in suspend too.
  After a power-off, "display on" (29h) goes out from a delayed work 100 ms after enable(), when the first
  frames have reached the panel, and again (up to 5 times) until its power mode (0Ah) reads display on: sent
  from enable(), before any frame, it was lost and the panel fell back to black. **Known problem:** even so,
  the first picture after such a cold wake comes up broken (the two DSI halves skewed by some lines, a
  touch redraws it) or stays black. The cause is not proven; the commands sent after enable() reach DSI0
  only and probably delay its frames against DSI1's. The tablet runs with `keep_power=1` until that is
  solved.
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

Display: DisplayPort over USB-C
-------------------------------

* **Device tree.** `sdm670.dtsi` gains the QMP USB3-DP combo PHY and the DisplayPort controller (SDM845's:
  the same registers, interrupt and clocks). The tablet is wired as on stock: all four SuperSpeed lanes go
  to DP through an NXP PTN36502 redriver on QUP0 SE3, the SBU lines through a GPIO mux, and DP audio is
  MultiMedia4 to `DISPLAY_PORT_RX`. USB stays at 2.0 meanwhile, as it is on this port anyway (below).
* **Hot plug from the S2MM005.** The Type-C controller runs the alt mode with the dock. The USB daemon starts
  it and reads the result over I2C, as Samsung's `ccic_alternate` does, and hands orientation, pin
  assignment and HPD to the `gts4lv-dp-altmode` glue module (in `mobian/kit/`). That module sets the Type-C
  switch and mux and signals HPD to the DP controller through an aux-hpd bridge. The redriver's
  orientation is the inverse of what the S2MM005 reports.
* **dp_ctrl**: with HPD from such a bridge the controller's own HPD state stays 0, so a failed link
  training gave up instead of falling back to a lower rate or fewer lanes. It now asks the sink over AUX
  whether it is still there.
* **dp_panel**: `msm.dp_max_bpp` caps the bits per pixel. The tested dock's EDID offers deep colour, but it
  shows nothing at 30 bpp; the image sets 24.
* **DPU**:
  * **One primary and one cursor plane per CRTC** (`possible_crtcs`) instead of every plane on every CRTC.
    Mutter 51 hands out primary planes in the order of its own monitor configuration and never reads the
    kernel's. With every plane allowed everywhere it asked to swap the panel's and the TV's planes in one
    commit, which the DRM core refuses ("switching CRTC directly"): with the TV connected at boot the login
    stayed black on both screens.
  * **CRTCs without DSPPs.** SDM670 has two DSPPs, on LM_0 and LM_1, and the dual-DSI panel holds both,
    while a 4096-wide mode needs LM_2 and LM_5. Mutter sets a gamma ramp on every CRTC, which asks for a
    DSPP, so the TV's modeset failed. A CRTC that cannot get DSPPs is now reserved without them (no colour
    management on it); the "LM_2, invalid DSPP_-1" lines of each such modeset are the first attempt.
  * A CRTC reserved without DSPPs no longer keeps the DSPP pointers of its previous state (the state is a
    copy), and no longer frees DSPP_0 from the CRTC that owns it. Both made the panel and the TV program the
    same DSPP after a replug.
  * **Reserve again only on a modeset.** `dpu_crtc_atomic_check()` reserved the CRTC's resources again
    whenever its colour management changed, without a modeset, so a gamma ramp or colour matrix appearing or
    going away could move the CRTC to other mixers, while the encoder takes the pingpongs of its mixers only
    at a modeset. After a docked panel-off, undock and redock, the panel waited on PP_2/PP_3 while LM_0/LM_1
    fed PP_0/PP_1: kickoff and vblank timeouts on both screens until a reboot. A gamma or colour matrix
    turning on or off is now a modeset (the CRTC state remembers whether its last reservation asked for
    DSPPs); a new LUT alone keeps the mixers. Not yet tested on the tablet.

Input: the Book Cover Keyboard (`stm32-pogo-kbd`, new)
-----------------------------------------------------

* A port of Samsung's `stm32_pogo_keyboard` and `pogo_fpga_dev` for the Book Cover Keyboard (EJ-FT720),
  without sec_class, wakelocks, the MUIC notifier chain or the keyboard's firmware update. The keyboard's
  STM32 sits at I2C 0x2a behind a Lattice iCE5LP4K on the tablet, which is SRAM based: the driver loads
  Samsung's bitstream (`samsung/gts4lv/pogo_fpga.fw`) over SPI at every boot. Attach detection reads the
  SM5705 MUIC's ADC (the keyboard reads as 49.9 kOhm), since mainline has no driver for that MUIC.
* Not yet tried with a keyboard on a tablet.

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
* **msm8916-wcd-digital**: runtime PM. Its MCLK is the ADSP's LPASS clock, which the driver took at probe
  and held until removal, so the ADSP never power-collapsed. Now the clocks run only while ASoC holds the
  codec (a stream, or DAPM while its bias is above OFF), with a 3 s autosuspend. The register cache is
  filled from the hardware at probe and every register is written back on resume. `idle_bias_on` is gone:
  with no bias to set it only kept DAPM's runtime PM reference forever.
* **sm8250, DisplayPort**: SDM670's ADSP acknowledges `AFE_PORT_CMD_DEVICE_START` for the DP port only once
  the DP controller's audio engine runs, and `AFE_PORT_CMD_DEVICE_STOP` only while it still runs. ASoC
  prepares the CPU DAI (the AFE port) before the codec DAI (`hdmi-codec`, which starts the engine) and shuts
  the codec DAI down first, so both commands timed out (-110) and the port stayed started. The link's
  prepare now starts the engine first, and its hw_free stops the port while the engine still runs.
* **sm8250, DisplayPort with no display.** PipeWire checks a UCM profile by preparing every device in it,
  and one failure marks the whole profile unsupported. With nothing on the DisplayPort output both DAI
  prepares of its back end fail (the ADSP holds the AFE start until the DP audio engine runs, and
  `hdmi-codec`'s prepare returns -EINVAL while the link is off), so the HiFi profile, and the speakers with
  it, was dropped whenever a session started without a TV. While the DP jack is out, the link's CPU and
  codec DAI prepares now return 0; a stream then plays into nothing, and WirePlumber does not pick a sink
  whose jack is out.
* **sm8250**: a back-end shutdown that sets the bit clock its startup turned on back to 0. That clock is an
  AFE vote in the ADSP, and after the first stream it kept the ADSP awake for good.

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

* **SM5705 fuel gauge and charger status** (`drivers/power/supply/sm5705_battery.c`, new):
  capacity, voltage, current, charging state, and charge, energy and power (`charge_now` from the gauge's
  1/256 % state of charge; energy at the battery's nominal 3.85 V, 27.1 Wh), which UPower needs for the
  discharge rate and GNOME's "time remaining". The chips charge on their own; the driver writes only the
  charger's interrupt masks. Its 30 s poll is not a wakeup source and is frozen across system sleep: each
  one ended an s2idle. `charge_now` follows the gauge's model, not a coulomb counter: after unplugging it
  can hold still for most of an hour and then catch up.
  * **Cable wake.** Only VBUSPOK and VBUSUVLO are unmasked, a threaded handler reads INT1 to INT4 (they
    clear on read), and the interrupt is a wake IRQ: a cable plugged in or pulled out wakes the system and
    changes the online state at once. The interrupt comes from the DT (gpio22, see Device tree), or, for a
    DT without it, from the `chg_int_line` module parameter (`<gpiochip label>:<line>`), with the pin bias
    set through a node-less faux device. A line still low with nothing latched is refused. Without an
    interrupt the poll is all there is, as before.
* **Suspend** (s2idle; the SoC offers no deeper state to Linux):
  * **qcom-wdt** stops the watchdog for suspend, and starts it again on resume, whenever it runs, not only
    when userspace opened it. The bootloader leaves it running, the watchdog core pings it from a kernel
    worker, and that worker does not run in s2idle.
  * **RPMh sleep stats**: `qcom,sdm845-rpmh-stats` at 0xc3f0000 (`/sys/kernel/debug/qcom_stats`: aosd,
    cxsd, and each subsystem's sleep). This needed the AOSS QMP node cut down to the 0x400 bytes of its
    message RAM, as on SDM845; it covered 1 MiB, the stats included.
  * Userspace has to stop the FastRPC servers (hexagonrpcd) before suspend. Frozen in the middle of a call,
    they crashed the ADSP's sensor process, and its recovery ended in a NoC error and a hang. Running,
    they also keep the ADSP awake through the suspend.
  * **i2c-qcom-geni**: a probe deferred while the GPI DMA driver is not there yet (both are modules) left
    the SE clocks enabled: the init's `pm_runtime_put` queued a suspend that the failed probe's
    `pm_runtime_disable` cancelled, and the device was then marked suspended with its clocks on. The
    touchscreen's bus collected nine of them per boot, and through them the crystal stayed on in sleep. A
    failed init now suspends synchronously.
  * **sdhci-msm**: the eMMC controller's "bus" clock (GCC_AGGRE_UFS_PHY_AXI_CLK on SDM670, fed by GPLL0 and
    so by the crystal) was switched on at probe and never off, which kept the crystal voted on in the RPMh
    sleep set. It is now gated with the other clocks in runtime suspend, as downstream gates its
    "bus_aggr_clk". sdhci-msm is built in.
  * Measured with these fixes: Linux's RPMh sleep votes are all clean in s2idle (crystal, CX and MX levels
    and every bus at 0), and the ADSP and the modem sleep through it, yet `aosd` and `cxsd` stay 0: the
    SoC never reaches AOSS sleep or CX collapse. What holds it lies outside these votes (another RSC, or a
    PMIC mode) and is not found yet. The panel is the bigger cost meanwhile (see Display).
* **CPU frequency from the start**: `CONFIG_INTERCONNECT_QCOM_OSM_L3=y`. `qcom-cpufreq-hw` waits for the OSM
  L3 interconnect provider; as a module it came about 10 s into boot, and everything before it ran at the
  bootloader's clock. Boot went from 27 s to 12 s.
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
daemon (blank and lock, and the sleep policy: two minutes after the screen goes off, on battery and with no
display on the DP output, not while audio plays, Wi-Fi, Bluetooth, the sensor stack and touch off for the
sleep; on the charger it stays awake), the image's `gts4lv-pogo-fw` (it copies the keyboard's FPGA bitstream
from the LineageOS recovery on the tablet at boot), the USB role daemon (which
also sets the charge rate), LineageOS's firmware (read from the tablet's own partitions at boot), hexagonrpcd and libssc for the
sensors, and a patched iio-sensor-proxy (a sensor claimed while it was still being opened never started; the
light level is smoothed). Also there: Chromium's flags and a Shell extension that opens the on-screen keyboard
for its text fields, for HDMI the DP alt mode in the USB daemon with its `gts4lv-dp-altmode` module, a GDM
monitor layout that puts the login on the TV (`monitors-tv.xml`), a user service that maps the touchscreen
to the TV at the login screen, `gts4lv-panel` for the power key while docked, and an HDMI device in the
UCM profile, the Venus codec under the device names Chromium looks for, the tablet's own Wi-Fi MAC from
the EFS partition, and `zip/`, which turns the running system into a zip for LineageOS recovery's "Apply
update". They are in [`mobian/`](mobian/), with the scripts that build the image and U-Boot's boot menu.
