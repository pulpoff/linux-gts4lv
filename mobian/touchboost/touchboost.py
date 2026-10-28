#!/usr/bin/env python3
"""Touch input boost for the Galaxy Tab S5e (SDM670) on mainline, from the POCO X3 NFC one (what Android kernels do with cpu_input_boost):
while the touchscreen reports contact, and for TOUCHBOOST_HOLD_MS after its last event, the little cluster
runs at >= 1.32 GHz, the big cluster at >= 1.36 GHz and the GPU at >= TOUCHBOOST_GPU_HZ; otherwise schedutil
and the GPU's simple_ondemand are free to go down. This chip's speed bin stops the GPU at 430 MHz (of the
780 in the OPP table), and it idles at 180: the first frames of a scroll rendered while it climbed. The hold covers a
fling's kinetic scroll after the finger lifts, not only the touch.
While a video records (a camera streams and the Venus codec is up) the CPU floors hold as well, for the whole
recording: the soft ISP and the RGB->NV12 conversion are 2.5 cores of work at 1080p60, and a frame that waits for
schedutil to ramp is a frame missing from the file.
Reads the touchscreen passively (no grab, the compositor keeps receiving every event) and only writes
sysfs when the boost state changes."""
import os, select, sys, time
import evdev
from evdev import ecodes as e

BOOST = {'policy0': 1324800, 'policy6': 1363200}       # kHz
HOLD = float(os.environ.get('TOUCHBOOST_HOLD_MS', '1000')) / 1000
GPU = '/sys/class/devfreq/5000000.gpu'
# 0 = leave the GPU alone, the default here: a 430 MHz floor (its top step on this
# speed bin) made scrolling and animations stutter badly instead of helping.
GPU_BOOST = int(os.environ.get('TOUCHBOOST_GPU_HZ', '0'))
BASE = '/sys/devices/system/cpu/cpufreq'

def touch_devices(known):
    """Every touchscreen (multitouch device) not yet open: the panel, and any that appears later (a virtual one
    replaying swipes for a benchmark, the panel again after its driver restarts)."""
    found = {}
    for p in evdev.list_devices():
        if p in known: continue
        try:
            d = evdev.InputDevice(p)
        except OSError:
            continue
        if e.ABS_MT_POSITION_X in dict(d.capabilities().get(e.EV_ABS, [])): found[p] = d
        else: d.close()
    return found

def floor(policy):
    with open(f'{BASE}/{policy}/cpuinfo_min_freq') as f: return int(f.read())

def set_min(values, gpu=None):
    for pol, khz in values.items():
        try:
            with open(f'{BASE}/{pol}/scaling_min_freq', 'w') as f: f.write(str(khz))
        except OSError as ex: print(f'{pol}: {ex}', file=sys.stderr)
    if gpu and GPU_BOOST:
        try:
            with open(f'{GPU}/min_freq', 'w') as f: f.write(str(gpu))
        except OSError as ex: print(f'gpu: {ex}', file=sys.stderr)

def gpu_floor():
    try:
        with open(f'{GPU}/min_freq') as f: return int(f.read())
    except OSError: return 0

CAMSS = '/sys/devices/platform/soc@0/acb3000.isp/power/runtime_status'

def camera_streaming():
    """The camera subsystem is powered only while a camera streams. Then the GPU, which debayers every frame, rests
    at GPU_BOOST too (the POCO's 20 MP front camera lost 5 fps at the GPU's floor)."""
    try:
        with open(CAMSS) as f: return f.read().strip() == 'active'
    except OSError: return False

VENUS = '/sys/devices/platform/soc@0/aa00000.video-codec/power/runtime_status'

def recording():
    """Camera streaming into the hardware encoder. Venus alone is a video playing, which needs no CPU floor."""
    try:
        with open(VENUS) as f: return camera_streaming() and f.read().strip() == 'active'
    except OSError: return False

def main():
    devs = touch_devices({})
    print(f'touchboost: {[d.name for d in devs.values()]}, hold {HOLD*1000:.0f} ms, boost {BOOST}, gpu {GPU_BOOST}',
          flush=True)
    rest = {p: floor(p) for p in BOOST}
    gpu_rest = gpu_floor()
    touched, last, scanned = False, 0.0, time.monotonic()
    applied = None
    while True:
        timeout = max(0.0, last + HOLD - time.monotonic()) if touched else 1.0
        r, _, _ = select.select([d.fd for d in devs.values()], [], [], timeout)
        for fd in r:
            path = next(p for p, d in devs.items() if d.fd == fd)
            try:
                for _ in devs[path].read(): pass       # drain; any event = a finger is on the glass
            except OSError:
                devs.pop(path).close()                  # gone (unplugged, driver restarted)
                continue
            last = time.monotonic()
            if not touched:
                touched = True
                if applied != 'touch': set_min(BOOST, GPU_BOOST); applied = 'touch'
        if touched and time.monotonic() - last >= HOLD: touched = False
        if not touched:
            want = 'record' if recording() else 'camera' if camera_streaming() else 'rest'
            if want != applied:
                set_min(BOOST if want == 'record' else rest, gpu_rest if want == 'rest' else GPU_BOOST)
                applied = want
        if time.monotonic() - scanned >= 5.0:
            devs.update(touch_devices(devs)); scanned = time.monotonic()

if __name__ == '__main__':
    try: main()
    finally:
        try: set_min({p: floor(p) for p in BOOST})
        except Exception: pass
