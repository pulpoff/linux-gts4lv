#!/usr/bin/python3
# UI frame benchmark: a fullscreen GTK4 window redraws a moving scene every
# frame for SECONDS, then prints the frame rate and frame-interval spread as
# the compositor actually presented them (GdkFrameClock timings).
import sys, time, math, gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 6
times = []

class Scene(Gtk.DrawingArea):
    def __init__(self):
        super().__init__(hexpand=True, vexpand=True)
        self.set_draw_func(self.draw)
        self.add_tick_callback(self.tick)
        self.t0 = None

    def tick(self, w, clock):
        t = clock.get_frame_time() / 1e6
        if self.t0 is None:
            self.t0 = t
        elif t - self.t0 > 1.0:			# skip the first second (window mapping)
            times.append(t)
        if t - self.t0 > SECONDS + 1:
            app.quit()
            return False
        self.queue_draw()
        return True

    def draw(self, area, cr, w, h):
        t = time.monotonic()
        cr.set_source_rgb(0.1, 0.1, 0.15); cr.paint()
        for i in range(40):				# moderate load: 40 moving translucent discs
            cr.set_source_rgba((i * 37 % 255) / 255, 0.5, 0.8, 0.5)
            cr.arc(w / 2 + math.cos(t * 2 + i) * w / 3, h / 2 + math.sin(t * 3 + i) * h / 3, 80, 0, 6.3)
            cr.fill()

def activate(a):
    win = Gtk.ApplicationWindow(application=a)
    win.set_child(Scene())
    win.fullscreen()
    win.present()

app = Gtk.Application(application_id="de.gts4lv.FrameBench")
app.connect("activate", activate)
app.run([])
d = sorted(b - a for a, b in zip(times, times[1:]))
if d:
    n = len(d)
    print(f"fps {n / (times[-1] - times[0]):.1f}  median {d[n // 2] * 1000:.1f} ms  "
          f"p95 {d[int(n * .95)] * 1000:.1f} ms  max {d[-1] * 1000:.1f} ms  "
          f">20ms {sum(x > 0.020 for x in d)}/{n}")
