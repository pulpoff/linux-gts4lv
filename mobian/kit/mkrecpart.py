#!/usr/bin/env python3
"""The recovery partition with U-Boot in front, for the Tab S5e:

  0       U-Boot as a v1 boot image (what Samsung's bootloader starts): gzip'd u-boot-nodtb.bin
          with the tablet DTB appended, and LineageOS's recovery_dtbo
  4 MiB   LineageOS's own recovery as a v2 boot image: its kernel without the appended DTB, its
          ramdisk, and the stock DTB with the board's overlay (dtbo index 3) applied
  34 MiB  Linux as a v2 boot image: Image.gz, initramfs, DTB

The offsets must match img_off in U-Boot's board/qualcomm/gts4lv.env (blocks of 512 bytes)."""
import argparse, gzip, os, struct, subprocess, sys, tempfile, zlib

PAGE = 4096
PART = 72744960
LINEAGE_OFF = 4 << 20
LINUX_OFF = 34 << 20
STOCK = "/home/pulp/gts4lv/backup/recovery.img"

p = argparse.ArgumentParser()
p.add_argument("--uboot", required=True, help="u-boot-nodtb.bin")
p.add_argument("--dtb", required=True, help="the tablet DTB (Linux's, it is also U-Boot's)")
p.add_argument("--kernel", required=True, help="Linux Image.gz, no DTB appended")
p.add_argument("--initramfs", required=True)
p.add_argument("-o", "--output", required=True)
a = p.parse_args()

stock = open(STOCK, "rb").read()
ks, kaddr, rs, raddr, ss, saddr, taddr, ps, hv, os_version = struct.unpack_from("<10I", stock, 8)
assert stock[:8] == b"ANDROID!" and ps == PAGE and hv == 1
name = stock[48:64]
stock_cmd = stock[64:576].rstrip(b"\0") + stock[608:1632].rstrip(b"\0")
pad = lambda b: b + b"\0" * ((-len(b)) % PAGE)
o = PAGE
skernel = stock[o:o + ks]; o += len(pad(skernel))
sramdisk = stock[o:o + rs]; o += len(pad(sramdisk))
dtbo_size, dtbo_off = struct.unpack_from("<IQ", stock, 1632)
sdtbo = stock[dtbo_off:dtbo_off + dtbo_size]


def v1(kernel, ramdisk, dtbo):
    hdr = struct.pack("<8s10I16s512s32s1024sIQI", b"ANDROID!", len(kernel), kaddr, len(ramdisk), raddr,
                      0, saddr, taddr, PAGE, 1, os_version, name, b"", b"", b"",
                      len(dtbo), PAGE + len(pad(kernel)) + len(pad(ramdisk)), 1648)
    return pad(hdr) + pad(kernel) + pad(ramdisk) + pad(dtbo)


def v2(kernel, ramdisk, dtb, cmd=b""):
    assert len(cmd) < 512 + 1024
    hdr = struct.pack("<8s10I16s512s32s1024sIQIIQ", b"ANDROID!", len(kernel), kaddr, len(ramdisk), raddr,
                      0, saddr, taddr, PAGE, 2, os_version, name, cmd[:512], b"", cmd[512:],
                      0, 0, 1660, len(dtb), 0)
    return pad(hdr) + pad(kernel) + pad(ramdisk) + pad(dtb)


# LineageOS's kernel is Image.gz with one DTB appended: split them.
z = zlib.decompressobj(31)
z.decompress(skernel)
sgz = skernel[:len(skernel) - len(z.unused_data)]
sdtb = z.unused_data
assert sdtb[:4] == b"\xd0\x0d\xfe\xed" and struct.unpack(">I", sdtb[4:8])[0] == len(sdtb), "expected one DTB"

# The board's overlay, as Samsung's bootloader applies it (androidboot.dtbo_idx=3).
magic, _, _, esz, cnt, eoff = struct.unpack_from(">6I", sdtbo)
assert magic == 0xd7b7ab1e and cnt > 3
osz, ooff = struct.unpack_from(">II", sdtbo, eoff + 3 * esz)
with tempfile.TemporaryDirectory() as t:
    open(t + "/base.dtb", "wb").write(sdtb)
    open(t + "/ov.dtbo", "wb").write(sdtbo[ooff:ooff + osz])
    subprocess.run(["fdtoverlay", "-i", t + "/base.dtb", "-o", t + "/m.dtb", t + "/ov.dtbo"], check=True)
    lineage_dtb = open(t + "/m.dtb", "rb").read()

dtb = open(a.dtb, "rb").read()
uboot = v1(gzip.compress(open(a.uboot, "rb").read(), 9, mtime=0) + dtb, b"", sdtbo)
lineage = v2(sgz, sramdisk, lineage_dtb, stock_cmd)
linux = v2(open(a.kernel, "rb").read(), open(a.initramfs, "rb").read(), dtb)

assert len(uboot) <= LINEAGE_OFF, len(uboot)
assert LINEAGE_OFF + len(lineage) <= LINUX_OFF, len(lineage)
assert LINUX_OFF + len(linux) <= PART, "Linux is %d bytes, %d fit" % (len(linux), PART - LINUX_OFF)
out = bytearray(LINUX_OFF + len(linux))
out[0:len(uboot)] = uboot
out[LINEAGE_OFF:LINEAGE_OFF + len(lineage)] = lineage
out[LINUX_OFF:] = linux
open(a.output, "wb").write(out)
for n, b, off in (("U-Boot", uboot, 0), ("LineageOS recovery", lineage, LINEAGE_OFF), ("Linux", linux, LINUX_OFF)):
    print("%-20s at %2d MiB: %8d bytes" % (n, off >> 20, len(b)))
print(a.output, len(out), "bytes;", PART - len(out), "left for Linux to grow")
