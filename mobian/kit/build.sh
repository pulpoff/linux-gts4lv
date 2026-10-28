#!/bin/sh
# Build the gts4lv mainline debug image and its Lineage-recovery zip.
#   recovery-mainline.img   boot image for the RECOVERY partition
#   gts4lv-mainline-<date>.zip   adb sideload; writes only the recovery partition
set -eu
K=$HOME/gts4lv/kit
B=${B:-$HOME/gts4lv/build}
DEBS=$HOME/gts4lv/debs/root
OUT=$HOME/gts4lv/out
DATE=$(date +%Y%m%d-%H%M)
mkdir -p $OUT
W=$(mktemp -d $OUT/work.XXXX)

# --- kernel + appended DTB (ABL reads the DTB from the end of Image.gz) ---
cat $B/arch/arm64/boot/Image.gz $B/arch/arm64/boot/dts/qcom/sdm670-samsung-gts4lvwifi.dtb > $W/kernel

# --- recovery_dtbo: Lineage's own, byte for byte ---
# ABL refused every overlay we built (no-op ones included) and accepts this one;
# the DTB exports its labels into /masked-devices (gen-dtbo-mask.py) so it applies.
python3 - $HOME/gts4lv/backup/recovery.img $W/recovery_dtbo.img <<'EOT'
import struct, sys
d = open(sys.argv[1], "rb").read()
size, off = struct.unpack_from("<IQ", d, 1632)
open(sys.argv[2], "wb").write(d[off:off + size])
EOT

# --- initramfs ---
R=$W/rootfs
mkdir -p $R/bin $R/usr/sbin $R/usr/lib/aarch64-linux-gnu $R/lib $R/etc $R/root/.ssh
cp $DEBS/usr/bin/busybox $R/bin/busybox
cp $K/init $R/init
cp $HOME/gts4lv/mobian/bootsplash/splash.bgra.gz $R/splash.bgra.gz
chmod 755 $R/init
for f in usr/sbin/dropbear usr/sbin/kexec usr/sbin/wpa_supplicant usr/sbin/wpa_cli usr/sbin/iw \
         usr/bin/rmtfs usr/bin/tqftpserv usr/bin/qrtr-lookup \
         usr/sbin/mke2fs usr/sbin/e2fsck usr/sbin/resize2fs usr/bin/tar; do
	cp $DEBS/$f $R/usr/sbin/
done
# Every library those need (NEEDED closure, computed when the debs were fetched).
while read l || [ -n "$l" ]; do cp -L $DEBS/usr/lib/aarch64-linux-gnu/$l $R/usr/lib/aarch64-linux-gnu/; done < $HOME/gts4lv/debs/idx/libs.txt
cp $K/wifi-up $R/usr/sbin/wifi-up
aarch64-linux-gnu-gcc -static -Os -s -o $R/usr/sbin/rebootmode $K/rebootmode.c
aarch64-linux-gnu-gcc -static -Os -s -o $R/usr/sbin/dmlinear $K/tools/dmlinear.c
# Where Mobian's rootfs.img (in Android's /data/lost+found) sits on userdata.
cp $K/rootfs-extents.txt $R/etc/rootfs-extents.txt
cp $DEBS/etc/mke2fs.conf $R/etc/mke2fs.conf
cp $K/udhcpc.script $R/etc/udhcpc.script
chmod 755 $R/usr/sbin/wifi-up $R/etc/udhcpc.script
{ echo "ctrl_interface=/run/wpa_supplicant"; echo "update_config=0"; echo "country=DE"; cat $K/wifi.conf; } > $R/etc/wpa_supplicant.conf
chmod 600 $R/etc/wpa_supplicant.conf
ln -s ../usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 $R/lib/ld-linux-aarch64.so.1
ln -s ../usr/lib/aarch64-linux-gnu $R/lib/aarch64-linux-gnu
echo "root:x:0:0:root:/root:/bin/sh" > $R/etc/passwd
echo "root::0::::::" > $R/etc/shadow
echo "/bin/sh" > $R/etc/shells
cp $HOME/.ssh/id_ed25519.pub $R/root/.ssh/authorized_keys
chmod 700 $R/root/.ssh; chmod 600 $R/root/.ssh/authorized_keys
(cd $R && find . | cpio -o -H newc -R 0:0 --quiet | gzip -9) > $W/initramfs.cpio.gz

# --- boot image for the recovery partition ---
CMDLINE="console=tty0 fbcon=vc:2-63 logo.nologo vt.global_cursor_default=0 splash loglevel=4 printk.devkmsg=on clk_ignore_unused pd_ignore_unused regulator_ignore_unused icc_ignore_unused panic=10 ath.regdomain=DE cfg80211.ieee80211_regdom=DE"
python3 $K/mkbootimg.py --kernel $W/kernel --ramdisk $W/initramfs.cpio.gz \
	--recovery-dtbo $W/recovery_dtbo.img --cmdline "$CMDLINE" -o $OUT/recovery-mainline.img
SZ=$(stat -c %s $OUT/recovery-mainline.img)
[ "$SZ" -le 72744960 ] || { echo "image $SZ bytes > recovery partition"; exit 1; }

# --- the same partition with U-Boot in front: its menu adds LineageOS and its recovery ---
U=${U:-/usr/src/uboot-gts4lv/build/u-boot-nodtb.bin}
python3 $K/mkrecpart.py --uboot $U --dtb $B/arch/arm64/boot/dts/qcom/sdm670-samsung-gts4lvwifi.dtb \
	--kernel $B/arch/arm64/boot/Image.gz --initramfs $W/initramfs.cpio.gz -o $OUT/recovery-uboot.img

# --- sideload zip: Lineage's own update-binary + an edify script ---
Z=$W/zip
mkdir -p $Z/META-INF/com/google/android
cp $HOME/yesman-zip/lineage-bin/update-binary $Z/META-INF/com/google/android/update-binary
cp $OUT/recovery-mainline.img $Z/recovery-mainline.img
cat > $Z/META-INF/com/google/android/updater-script <<'EOF'
ui_print("gts4lv mainline debug kernel -> RECOVERY partition");
assert(getprop("ro.product.device") == "gts4lvwifi" || getprop("ro.build.product") == "gts4lvwifi" ||
       abort("This package is for the Galaxy Tab S5e Wi-Fi (gts4lvwifi); this device is " + getprop("ro.product.device") + "."););
ui_print("Android (boot, system, data) is not touched.");
package_extract_file("recovery-mainline.img", "/dev/block/bootdevice/by-name/recovery");
ui_print("Done. Reboot to recovery = mainline; normal boot = Android.");
ui_print("USB: ssh root@172.16.42.1 (telnet too)");
EOF
(cd $Z && rm -f $OUT/gts4lv-mainline-$DATE.zip && zip -qr -X $OUT/gts4lv-mainline-$DATE.zip META-INF recovery-mainline.img)
(cd $OUT && sha256sum recovery-mainline.img gts4lv-mainline-$DATE.zip | tee gts4lv-mainline-$DATE.sha256)
echo "work dir: $W"
