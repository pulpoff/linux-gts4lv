/*
 * gts4lv-touchd: userspace driver for the Galaxy Tab S5e's STM FTS touchscreen
 * (fts1ba90a, i2c 0x49, IRQ gpio123 active low) until a kernel driver exists.
 * Reads the chip's event FIFO through i2c-dev on every IRQ edge and replays
 * the contacts as a multitouch (protocol B) uinput device.
 *
 * Protocol (from Samsung's fts1ba90a driver): 8-byte events; 0x60 reads one,
 * 0x61 the rest of the FIFO, whose length is in byte 7 of the first.
 * Coordinate event: b0 = eid[1:0] tid[5:2] status[7:6], x = b1 << 4 | b3 >> 4,
 * y = b2 << 4 | (b3 & 0xf), b4 major, b5 minor, b6 z[5:0].
 *
 * Double tap to wake: SIGUSR1 (sent by gts4lv-powerkey when the panel goes
 * off) puts the chip in low-power mode with its wake-up gesture armed, and
 * touches are no longer reported; a double tap then arrives as a gesture event
 * and goes out as KEY_WAKEUP on a second uinput device ("gts4lv double tap"),
 * which gts4lv-powerkey grabs. SIGUSR2 (panel on) returns to normal sensing.
 * Commands from Samsung's fts_ts.c (fts_set_opmode, fts_write_to_sponge):
 * 31 01 low power, 39 02 wake-up gesture, AA 00 00 <mode> + C0 00 00 the
 * gesture library's mode (bit 2 double tap); 31 00 back to normal.
 * SIGHUP (system suspend): sensing off altogether (A0 00 00), no double tap;
 * SIGUSR1/SIGUSR2 turn it back on.
 * Gesture event: b0 = eid[1:0] (2) stype[5:2] sf[7:6]; double tap is sf 1 /
 * stype 1, or gesture id (b1) 1.
 *
 * usage: gts4lv-touchd [/dev/i2c-N] (default: the bus at a84000)
 * GTS4LV_TOUCH_SWAP=1/0, _INVX=1, _INVY=1 adjust the axes to the panel.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/uinput.h>

#define ADDR 0x49
#define IRQ_GPIO 123
#define EV_SIZE 8
#define FIFO_MAX 32
#define SLOTS 10
#define RAW_X 1600	/* stm,max_coords = <0x63f 0x9ff> */
#define RAW_Y 2560

static int i2c, ui, gpio, wake_ui;
static volatile sig_atomic_t want_lpm, lpm;	/* 0 sensing, 1 double tap only, 2 off */
static int swap = 1, invx, invy = 1, debug;	/* verified on the panel 2026-10-27 */
static int lastx[SLOTS], lasty[SLOTS];

static int xfer(unsigned char *w, int wl, unsigned char *r, int rl)
{
	struct i2c_msg m[2] = {
		{ .addr = ADDR, .flags = 0, .len = wl, .buf = w },
		{ .addr = ADDR, .flags = I2C_M_RD, .len = rl, .buf = r },
	};
	struct i2c_rdwr_ioctl_data d = { .msgs = m, .nmsgs = rl ? 2 : 1 };

	return ioctl(i2c, I2C_RDWR, &d) < 0 ? -errno : 0;
}

static int cmd(unsigned char *w, int wl)
{
	return xfer(w, wl, NULL, 0);
}

static void emit(int type, int code, int val)
{
	struct input_event e = { .type = type, .code = code, .value = val };

	if (write(ui, &e, sizeof(e)) != sizeof(e))
		perror("uinput write");
}

static void emit_wake(void)
{
	struct input_event e[] = {
		{ .type = EV_KEY, .code = KEY_WAKEUP, .value = 1 },
		{ .type = EV_SYN, .code = SYN_REPORT },
		{ .type = EV_KEY, .code = KEY_WAKEUP, .value = 0 },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};

	if (write(wake_ui, e, sizeof(e)) != sizeof(e))
		perror("wake uinput write");
}

static void abs_setup(int code, int max)
{
	struct uinput_abs_setup a = { .code = code, .absinfo = { .maximum = max } };

	ioctl(ui, UI_SET_ABSBIT, code);
	ioctl(ui, UI_ABS_SETUP, &a);
}

static int find_bus(char *path, size_t n)
{
	DIR *d = opendir("/sys/bus/i2c/devices");
	struct dirent *e;
	char l[PATH_MAX], t[PATH_MAX];

	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "i2c-", 4))
			continue;
		snprintf(l, sizeof(l), "/sys/bus/i2c/devices/%s/of_node", e->d_name);
		if (realpath(l, t) && strstr(t, "i2c@a84000")) {
			snprintf(path, n, "/dev/%s", e->d_name);
			closedir(d);
			return 0;
		}
	}
	if (d)
		closedir(d);
	return -1;
}

static int open_irq(void)
{
	DIR *d = opendir("/dev");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		struct gpiochip_info ci;
		struct gpio_v2_line_request req = { 0 };
		char p[300];
		int fd;

		if (strncmp(e->d_name, "gpiochip", 8))
			continue;
		snprintf(p, sizeof(p), "/dev/%s", e->d_name);
		if ((fd = open(p, O_RDWR)) < 0)
			continue;
		if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &ci) < 0 || ci.lines < 150 ||
		    !strstr(ci.label, "pinctrl")) {
			close(fd);
			continue;
		}
		req.offsets[0] = IRQ_GPIO;
		req.num_lines = 1;
		req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_FALLING;
		strcpy(req.consumer, "gts4lv-touchd");
		if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
			perror("GPIO_V2_GET_LINE_IOCTL");
			return -1;
		}
		close(fd);
		closedir(d);
		return req.fd;
	}
	if (d)
		closedir(d);
	return -1;
}

static int irq_low(void)
{
	struct gpio_v2_line_values v = { .mask = 1 };

	if (ioctl(gpio, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
		return 0;
	return !(v.bits & 1);
}

static void chip_init(void)
{
	unsigned char clear[] = { 0x62 };
	unsigned char touchtype[] = { 0x30, 0x61, 0x00 };	/* touch | palm | wet */
	unsigned char calib[] = { 0x13 };
	unsigned char scan[] = { 0xA0, 0x00, 0x01 };		/* mutual + self sensing */

	unsigned char reset[] = { 0x12 }, one = 0x60, ev[EV_SIZE];
	int i;

	/* Soft reset, then wait for "controller ready" (status/info, id 0x00):
	 * commands sent while the chip is still booting are silently lost. */
	cmd(reset, 1);
	for (i = 0; i < 50; i++) {
		usleep(20000);
		if (!xfer(&one, 1, ev, EV_SIZE) && ev[0] == 0x49 && ev[1] == 0x00)
			break;
	}
	if (i == 50)
		fprintf(stderr, "no ready event after reset\n");
	cmd(clear, 1);
	cmd(touchtype, 3);
	usleep(10000);
	cmd(calib, 1);
	usleep(500000);
	cmd(clear, 1);
	if (cmd(scan, 3))
		fprintf(stderr, "scan on failed\n");
}

static void on_signal(int sig)
{
	want_lpm = sig == SIGHUP ? 2 : sig == SIGUSR1;
}

static void set_opmode(int on);

static void set_lpm(int on)
{
	unsigned char scan_off[] = { 0xA0, 0x00, 0x00 }, scan_on[] = { 0xA0, 0x00, 0x01 };

	if (on == 2) {
		if (cmd(scan_off, 3))
			fprintf(stderr, "scan off failed\n");
		lpm = 2;
		fprintf(stderr, "sensing off (suspend)\n");
		return;
	}
	if (lpm == 2 && cmd(scan_on, 3))
		fprintf(stderr, "scan on failed\n");
	set_opmode(on);
}

static void set_opmode(int on)
{
	unsigned char opmode[] = { 0x31, on ? 0x01 : 0x00 };
	unsigned char gesture[] = { 0x39, 0x02 };
	unsigned char sponge[] = { 0xAA, 0x00, 0x00, 0x04 };	/* mode: double tap */
	unsigned char notify[] = { 0xC0, 0x00, 0x00 };

	if (cmd(opmode, 2))
		fprintf(stderr, "opmode %d failed\n", on);
	usleep(20000);
	if (on) {
		cmd(gesture, 2);
		cmd(sponge, 4);
		cmd(notify, 3);
	}
	lpm = on;
	fprintf(stderr, "low power %s\n", on ? "on (double tap armed)" : "off");
}

static void handle(unsigned char *ev, int *active)
{
	int eid = ev[0] & 3, tid = (ev[0] >> 2) & 0xf, st = ev[0] >> 6;
	int rx = ev[1] << 4 | ev[3] >> 4, ry = ev[2] << 4 | (ev[3] & 0xf);
	int ttype = (ev[6] >> 6) << 2 | ev[7] >> 6;
	int x, y;

	if (debug)
		fprintf(stderr, "ev %02x %02x %02x %02x %02x %02x %02x %02x\n",
			ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
	if (eid == 1) {		/* status event: log errors, reset on a full queue */
		if (((ev[0] >> 2) & 0xf) == 1)
			fprintf(stderr, "status error %02x %02x\n", ev[1], ev[2]);
		return;
	}
	if (eid == 2) {		/* gesture event */
		int sf = ev[0] >> 6, stype = (ev[0] >> 2) & 0xf;

		fprintf(stderr, "gesture sf %d type %d id %d\n", sf, stype, ev[1]);
		if (lpm && ((sf == 1 && stype == 1) || ev[1] == 0x01))
			emit_wake();
		return;
	}
	if (lpm || eid != 0 || tid >= SLOTS)
		return;
	if (ttype != 0 && ttype != 3 && ttype != 6) {	/* palm etc.: lift */
		st = 3;
	}
	x = swap ? ry : rx;
	y = swap ? rx : ry;
	if (invx)
		x = (swap ? RAW_Y : RAW_X) - 1 - x;
	if (invy)
		y = (swap ? RAW_X : RAW_Y) - 1 - y;

	emit(EV_ABS, ABS_MT_SLOT, tid);
	if (st == 3) {
		if (*active & 1 << tid)
			emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
		*active &= ~(1 << tid);
		return;
	}
	if (!(*active & 1 << tid)) {
		static int track;

		emit(EV_ABS, ABS_MT_TRACKING_ID, track++ & 0xffff);
		*active |= 1 << tid;
	}
	emit(EV_ABS, ABS_MT_POSITION_X, x);
	emit(EV_ABS, ABS_MT_POSITION_Y, y);
	lastx[tid] = x;
	lasty[tid] = y;
	emit(EV_ABS, ABS_MT_TOUCH_MAJOR, ev[4]);
	emit(EV_ABS, ABS_MT_TOUCH_MINOR, ev[5]);
}

static void drain(int *active)
{
	unsigned char buf[FIFO_MAX * EV_SIZE], one = 0x60, all = 0x61;
	int left, i, before = *active;

	if (xfer(&one, 1, buf, EV_SIZE))
		return;
	left = buf[7] & 0x3f;
	if (left >= FIFO_MAX)
		left = FIFO_MAX - 1;
	if (left && xfer(&all, 1, buf + EV_SIZE, left * EV_SIZE))
		left = 0;
	for (i = 0; i <= left; i++)
		handle(buf + i * EV_SIZE, active);

	/* Single-touch emulation (first active slot) for clients that want it. */
	emit(EV_KEY, BTN_TOUCH, *active != 0);
	if (*active) {
		for (i = 0; i < SLOTS && !(*active & 1 << i); i++)
			;
		emit(EV_ABS, ABS_X, lastx[i]);
		emit(EV_ABS, ABS_Y, lasty[i]);
	}
	if (*active || before)
		emit(EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv)
{
	struct uinput_setup us = { .id = { .bustype = BUS_I2C, .vendor = 0x0483, .product = 0x1ba9 } };
	char bus[64];
	int active = 0;
	const char *s;

	if ((s = getenv("GTS4LV_TOUCH_SWAP")))
		swap = atoi(s);
	invx = (s = getenv("GTS4LV_TOUCH_INVX")) && atoi(s);
	if ((s = getenv("GTS4LV_TOUCH_INVY")))
		invy = atoi(s);
	debug = (s = getenv("GTS4LV_TOUCH_DEBUG")) && atoi(s);

	if (argc > 1)
		snprintf(bus, sizeof(bus), "%s", argv[1]);
	else if (find_bus(bus, sizeof(bus))) {
		fprintf(stderr, "no i2c bus at a84000\n");
		return 1;
	}
	if ((i2c = open(bus, O_RDWR)) < 0) {
		perror(bus);
		return 1;
	}
	if ((gpio = open_irq()) < 0) {
		fprintf(stderr, "cannot get gpio%d\n", IRQ_GPIO);
		return 1;
	}
	chip_init();

	if ((ui = open("/dev/uinput", O_WRONLY)) < 0) {
		perror("/dev/uinput");
		return 1;
	}
	ioctl(ui, UI_SET_EVBIT, EV_KEY);
	ioctl(ui, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(ui, UI_SET_EVBIT, EV_ABS);
	ioctl(ui, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	abs_setup(ABS_X, (swap ? RAW_Y : RAW_X) - 1);
	abs_setup(ABS_Y, (swap ? RAW_X : RAW_Y) - 1);
	abs_setup(ABS_MT_SLOT, SLOTS - 1);
	abs_setup(ABS_MT_TRACKING_ID, 0xffff);
	abs_setup(ABS_MT_POSITION_X, (swap ? RAW_Y : RAW_X) - 1);
	abs_setup(ABS_MT_POSITION_Y, (swap ? RAW_X : RAW_Y) - 1);
	abs_setup(ABS_MT_TOUCH_MAJOR, 255);
	abs_setup(ABS_MT_TOUCH_MINOR, 255);
	strcpy(us.name, "STM FTS1BA90A Touchscreen");
	if (ioctl(ui, UI_DEV_SETUP, &us) < 0 || ioctl(ui, UI_DEV_CREATE) < 0) {
		perror("uinput setup");
		return 1;
	}
	fprintf(stderr, "gts4lv-touchd: %s, irq gpio%d, swap=%d invx=%d invy=%d\n",
		bus, IRQ_GPIO, swap, invx, invy);

	if ((wake_ui = open("/dev/uinput", O_WRONLY)) < 0) {
		perror("/dev/uinput");
		return 1;
	}
	{
		struct uinput_setup ws = { .id = { .bustype = BUS_VIRTUAL } };

		ioctl(wake_ui, UI_SET_EVBIT, EV_KEY);
		ioctl(wake_ui, UI_SET_KEYBIT, KEY_WAKEUP);
		strcpy(ws.name, "gts4lv double tap");
		if (ioctl(wake_ui, UI_DEV_SETUP, &ws) < 0 || ioctl(wake_ui, UI_DEV_CREATE) < 0) {
			perror("wake uinput setup");
			return 1;
		}
	}
	{
		struct sigaction sa = { .sa_handler = on_signal };	/* no SA_RESTART: poll wakes */

		sigaction(SIGUSR1, &sa, NULL);
		sigaction(SIGUSR2, &sa, NULL);
		sigaction(SIGHUP, &sa, NULL);
	}

	for (;;) {
		struct pollfd p = { .fd = gpio, .events = POLLIN };
		struct gpio_v2_line_event le;

		if (want_lpm != lpm) {
			if (want_lpm && active) {	/* lift every contact first */
				int i;

				for (i = 0; i < SLOTS; i++)
					if (active & 1 << i) {
						emit(EV_ABS, ABS_MT_SLOT, i);
						emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
					}
				active = 0;
				emit(EV_KEY, BTN_TOUCH, 0);
				emit(EV_SYN, SYN_REPORT, 0);
			}
			set_lpm(want_lpm);
		}

		if (poll(&p, 1, irq_low() ? 0 : 1000) > 0 &&
		    read(gpio, &le, sizeof(le)) < 0)
			perror("gpio event");
		while (irq_low())
			drain(&active);
	}
}
