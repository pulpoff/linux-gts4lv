#!/bin/sh
# Run the UI benchmarks in pulp's GNOME session: frame pacing, GPU, CPU.
B=unix:path=/run/user/1001/bus
U() { runuser -u pulp -- env DBUS_SESSION_BUS_ADDRESS=$B XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 "$@"; }
echo "kernel $(uname -r)"
echo "frames: $(U python3 /usr/local/bin/framebench.py 6 2>/dev/null | tail -1)"
command -v glmark2-es2-wayland >/dev/null && \
	echo "glmark2: $(U glmark2-es2-wayland -b build:use-vbo=true -b texture -b shading:shading=phong -b refract -s 800x600 2>/dev/null | grep -i score)"
echo "cpu1: $(python3 -c 'import time;t=time.perf_counter();sum(i*i for i in range(3_000_000));print(round(time.perf_counter()-t,3),"s")')"
cat /sys/class/devfreq/5000000.gpu/trans_stat | tail -1
