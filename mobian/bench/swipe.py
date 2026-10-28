#!/usr/bin/python3
# Synthetic touchscreen for repeatable UI tests: a uinput multitouch device
# covering the panel, replaying straight-line swipes.
#   swipe.py x1 y1 x2 y2 [ms] [repeat]      (panel pixels, landscape 2560x1600)
#   swipe.py tap x y [count]
import sys, time
import evdev
from evdev import ecodes as e, AbsInfo

W, H = 2560, 1600
dev = evdev.UInput({
    e.EV_KEY: [e.BTN_TOUCH],
    e.EV_ABS: [(e.ABS_X, AbsInfo(0, 0, W - 1, 0, 0, 10)), (e.ABS_Y, AbsInfo(0, 0, H - 1, 0, 0, 10)),
               (e.ABS_MT_SLOT, AbsInfo(0, 0, 9, 0, 0, 0)),
               (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
               (e.ABS_MT_POSITION_X, AbsInfo(0, 0, W - 1, 0, 0, 10)),
               (e.ABS_MT_POSITION_Y, AbsInfo(0, 0, H - 1, 0, 0, 10))],
}, name="gts4lv test touch", input_props=[e.INPUT_PROP_DIRECT])
time.sleep(1.5)		# let libinput/mutter pick the device up
tid = 100


def down(x, y):
    global tid
    tid += 1
    for c, v in ((e.ABS_MT_SLOT, 0), (e.ABS_MT_TRACKING_ID, tid), (e.ABS_MT_POSITION_X, x),
                 (e.ABS_MT_POSITION_Y, y), (e.ABS_X, x), (e.ABS_Y, y)):
        dev.write(e.EV_ABS, c, v)
    dev.write(e.EV_KEY, e.BTN_TOUCH, 1)
    dev.syn()


def move(x, y):
    for c, v in ((e.ABS_MT_POSITION_X, x), (e.ABS_MT_POSITION_Y, y), (e.ABS_X, x), (e.ABS_Y, y)):
        dev.write(e.EV_ABS, c, v)
    dev.syn()


def up():
    dev.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
    dev.write(e.EV_KEY, e.BTN_TOUCH, 0)
    dev.syn()


a = sys.argv[1:]
if a[0] == "tap":
    x, y, n = int(a[1]), int(a[2]), int(a[3]) if len(a) > 3 else 1
    for _ in range(n):
        down(x, y); time.sleep(0.05); up(); time.sleep(0.12)
else:
    x1, y1, x2, y2 = map(int, a[:4])
    ms = int(a[4]) if len(a) > 4 else 400
    rep = int(a[5]) if len(a) > 5 else 1
    steps = max(2, ms // 8)		# 125 Hz, like the panel's report rate
    for _ in range(rep):
        down(x1, y1)
        for i in range(1, steps + 1):
            time.sleep(ms / 1000 / steps)
            move(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
        time.sleep(0.03)
        up()
        time.sleep(0.6)
time.sleep(0.3)
