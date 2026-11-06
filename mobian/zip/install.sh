#!/system/bin/sh
# install.sh prepare|finish: the shell half of the Mobian zip, run by updater-script in
# LineageOS recovery (toybox only: no awk). Between the two steps updater-script inflates
# rootfs.img (an ext4 image shrunk to its content) into /data/lost+found/mobian. That
# directory is never encrypted, which lets the initramfs reach the file without mounting
# /data: it maps the file's extents with dm-linear.
#   prepare  mount /data, check it is ext4 with room, remove an earlier install
#   finish   grow the file to its full size, pin it (immutable), write its extents to the
#            last 512 KiB of the recovery partition, where Linux's init reads them
# LineageOS's updater has no set_perm any more: updater-script runs this through
# /system/bin/sh, and this makes the extents tool executable itself.
# A child of the updater cannot print to the recovery screen: on failure this writes
# error=<why> to /tmp/gts4lv.prop and exits 1, and updater-script aborts with it.
set -u
DIR=/data/lost+found/mobian
IMG=$DIR/rootfs.img
# Recovery's mksh does 32-bit arithmetic ($((9 << 30)) is 1 GiB there): sizes over 2 GiB are
# literal strings, handed to fallocate and the extents tool as they are, and free space is
# counted in GiB.
SIZE=9663676416         # the whole file, 9 GiB: ~5 GB system + ~4 GB room for the user
SIZE_GIB=9
NEED_GIB=10             # and 1 GiB left over for Android
BY=/dev/block/bootdevice/by-name
TAIL=$((512 * 1024))

die() { echo "error=$*" > /tmp/gts4lv.prop; echo "gts4lv install: $*" >&2; exit 1; }

case "$1" in
prepare)
	grep -q " /data " /proc/mounts || mount /data 2>/dev/null ||
		mount -t ext4 -o noatime $BY/userdata /data || die "cannot mount /data"
	FS=$(grep " /data " /proc/mounts | head -n1 | cut -d' ' -f3)
	[ "$FS" = ext4 ] || die "/data is $FS; this installer needs ext4"
	[ -d /data/lost+found ] || die "/data has no lost+found"
	if [ -e $IMG ]; then
		chattr -i $IMG 2>/dev/null
		rm -f $IMG || die "cannot remove the earlier $IMG"
	fi
	FREE_GIB=$(($(stat -f -c %a /data) / (1073741824 / $(stat -f -c %S /data))))
	[ $FREE_GIB -ge $NEED_GIB ] || die "/data has $FREE_GIB GiB free; Linux needs $NEED_GIB GiB"
	mkdir -p $DIR && chmod 700 $DIR || die "cannot create $DIR"
	;;
finish)
	[ -s $IMG ] || die "rootfs.img was not written"
	fallocate -l $SIZE $IMG || die "cannot grow rootfs.img to $SIZE_GIB GiB"
	sync
	chattr +i $IMG || die "chattr +i failed"
	chmod 755 /tmp/gts4lv-extents
	X=/tmp/gts4lv-extents.txt
	/tmp/gts4lv-extents $IMG $SIZE > $X 2> /tmp/gts4lv-extents.err || die "$(cat /tmp/gts4lv-extents.err)"
	[ $(stat -c %s $X) -lt $TAIL ] || die "$(($(wc -l < $X) - 2)) extents do not fit in 512 KiB"
	REC=$BY/recovery
	SEEK=$((($(blockdev --getsize64 $REC) - TAIL) / 512))
	dd if=/dev/zero of=$REC bs=512 seek=$SEEK count=$((TAIL / 512)) 2>/dev/null &&
		dd if=$X of=$REC bs=512 seek=$SEEK conv=notrunc 2>/dev/null || die "cannot write the extent list"
	sync
	echo "extents=$(($(wc -l < $X) - 2))" > /tmp/gts4lv.prop
	umount /data 2>/dev/null
	;;
esac
exit 0
