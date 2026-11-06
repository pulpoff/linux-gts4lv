#!/bin/sh
# pack.sh IMG RECOVERY OUTDIR: gts4lv-mobian-<date>.zip for LineageOS recovery's "Apply update":
#   META-INF/.../update-binary   LineageOS's own (static aarch64 edify interpreter)
#   META-INF/.../updater-script  this folder's updater-script
#   install.sh, extents          the shell half and kit/tools/extents (static)
#   rootfs.img                   IMG (snapshot.sh: the shrunk ext4 root)
#   recovery.img                 RECOVERY (kit/build.sh with PUBLIC=1: U-Boot, Lineage recovery, Linux)
# Deflate: package_extract_file inflates rootfs.img straight into /data as it reads it; zip 3.0
# switches to zip64 by itself for an entry over 4 GB.
set -eu
IMG=$1; REC=$2; OUT=$3
Z=$(cd "$(dirname "$0")" && pwd)
K=$HOME/gts4lv/kit
W=$OUT/zip.$$
mkdir -p "$W/META-INF/com/google/android"
cp "$HOME/yesman-zip/lineage-bin/update-binary" "$W/META-INF/com/google/android/update-binary"
cp "$Z/updater-script" "$W/META-INF/com/google/android/updater-script"
cp "$Z/install.sh" "$W/install.sh"
cp "$K/tools/extents" "$W/extents"
ln "$IMG" "$W/rootfs.img" 2>/dev/null || cp "$IMG" "$W/rootfs.img"
cp "$REC" "$W/recovery.img"
F=$OUT/gts4lv-mobian-$(date +%Y%m%d).zip
rm -f "$F"
(cd "$W" && zip -q -r -6 -X "$F" META-INF install.sh extents recovery.img rootfs.img)
rm -rf "$W"
(cd "$OUT" && sha256sum "$(basename "$F")" > "$(basename "$F").sha256")
ls -la "$F"; cat "$F.sha256"; unzip -l "$F"
