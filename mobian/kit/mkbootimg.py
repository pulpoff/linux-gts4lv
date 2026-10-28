#!/usr/bin/env python3
"""Boot image header v1 for the Tab S5e, laid out like the stock recovery:
page 4096, Image.gz with the DTB appended, ramdisk, and a recovery_dtbo."""
import argparse, hashlib, struct

p = argparse.ArgumentParser()
p.add_argument("--kernel", required=True)
p.add_argument("--ramdisk", required=True)
p.add_argument("--recovery-dtbo", required=True)
p.add_argument("--cmdline", default="")
p.add_argument("--name", default="SRPSA14B001")  # stock board name; ABL may check it
p.add_argument("-o", "--output", required=True)
a = p.parse_args()

PAGE = 4096
# Load addresses and os_version/patch level copied from the stock recovery header.
stock = open("/home/pulp/gts4lv/backup/recovery.img", "rb").read(PAGE)
(kaddr,) = struct.unpack_from("<I", stock, 12)
(raddr,) = struct.unpack_from("<I", stock, 20)
(saddr, taddr) = struct.unpack_from("<II", stock, 28)
(os_version,) = struct.unpack_from("<I", stock, 44)
print("stock: kernel %#x ramdisk %#x second %#x tags %#x os_version %#x" % (kaddr, raddr, saddr, taddr, os_version))

kernel = open(a.kernel, "rb").read()
ramdisk = open(a.ramdisk, "rb").read()
dtbo = open(a.recovery_dtbo, "rb").read()

def pad(b):
    return b + b"\0" * ((-len(b)) % PAGE)

dtbo_off = PAGE + len(pad(kernel)) + len(pad(ramdisk))
cmd = a.cmdline.encode()
assert len(cmd) < 512 + 1024
sha = hashlib.sha1()
for blob in (kernel, ramdisk, b""):
    sha.update(blob); sha.update(struct.pack("<I", len(blob)))
sha.update(dtbo); sha.update(struct.pack("<I", len(dtbo)))

hdr = struct.pack("<8s10I16s512s32s1024sIQI",
    b"ANDROID!", len(kernel), kaddr, len(ramdisk), raddr,
    0, saddr, taddr, PAGE, 1, os_version,
    a.name.encode(), cmd[:512], sha.digest().ljust(32, b"\0"), cmd[512:],
    len(dtbo), dtbo_off, 1648)
out = pad(hdr) + pad(kernel) + pad(ramdisk) + pad(dtbo)
open(a.output, "wb").write(out)
print(a.output, len(out), "bytes; recovery_dtbo at", hex(dtbo_off))
