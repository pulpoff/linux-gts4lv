# --- recovery_dtbo: Lineage's own, byte for byte ---
# ABL refused every overlay we built (no-op ones included) and accepts this one;
# the DTB exports its labels into /masked-devices (gen-dtbo-mask.py) so it applies.
python3 - $HOME/gts4lv/backup/recovery.img $W/recovery_dtbo.img <<'EOT'
import struct, sys
d = open(sys.argv[1], "rb").read()
size, off = struct.unpack_from("<IQ", d, 1632)
open(sys.argv[2], "wb").write(d[off:off + size])
EOT

