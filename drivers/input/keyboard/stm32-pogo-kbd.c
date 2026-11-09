// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Galaxy Tab S5e (SM-T720, "gts4lv") POGO keyboard: the Book Cover
 * Keyboard EJ-FT720.
 *
 * Port of Samsung's downstream drivers/input/keyboard/stm32_pogo_keyboard.c
 * and drivers/fpga/pogo_fpga_dev.c (LineageOS android_kernel_samsung_sdm670,
 * branch lineage-22.2), without sec_class, wakelocks, the MUIC notifier chain
 * or the keyboard firmware update (FOTA) path, none of which typing needs.
 *
 * Hardware, from the tablet's own device tree:
 *
 *  - The keyboard's MCU is an STM32L4 at I2C address 0x2a on QUP0 SE0
 *    (i2c0: gpio0 SDA, gpio1 SCL, 400 kHz), with a level-low event interrupt
 *    on TLMM gpio108 and a 3.3 V supply switched by TLMM gpio89.
 *  - That bus does not reach the pogo pins directly: it goes through a
 *    Lattice iCE5LP4K (iCE40 Ultra) on the tablet. The FPGA is SRAM based, so
 *    Samsung's bitstream is loaded over SPI on every boot (QUP1 SE0 = spi8,
 *    chip select gpio105 as a GPIO, 12 MHz, mode 0). FPGA lines: CRESET_B
 *    gpio2, CDONE gpio107, logic reset gpio106 (low = held in reset), supply
 *    enable gpio116. Samsung's driver checks CDONE before it talks to the
 *    keyboard and resets the FPGA logic after every failed transfer, so the
 *    bus is taken to need the loaded bridge.
 *  - Attach detection: the pogo connector's ID line goes to the SM5705 MUIC
 *    ADC (i2c-gpio bus "i2c-charger", 0x25, register 0x09); the keyboard
 *    reads as 49.9 kOhm, ADC code 0x11. Samsung's MUIC driver turned that
 *    into a notifier call. Mainline has no SM5705 MUIC driver (the initramfs
 *    drives it through i2c-dev), so this driver polls that one register with
 *    raw transfers on the adapter, without claiming the address. Writing
 *    the "connected" attribute overrides it.
 *
 * Keyboard protocol: a write of one command byte, a STOP, then a read.
 *   0x00 version: hw_rev, model_id, minor, major
 *   0x03 number of pending events (1 byte; 0xff = release every key)
 *   0x04 the events, one byte each: bit 0 press, bits 1-4 column, 5-7 row
 *   0x07 mode: 1 application, 2 DFU (firmware update); 0x18 leaves DFU
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define POGO_FPGA_FW_DEFAULT		"samsung/gts4lv/pogo_fpga.fw"
#define POGO_FPGA_SPI_HZ		12000000
/* Samsung clocks out 13 zero bytes (~100 clocks) after the bitstream. */
#define POGO_FPGA_TAIL_BYTES		13
#define POGO_FPGA_HOUSEKEEPING_US	1200

#define POGO_CMD_VERSION		0x00
#define POGO_CMD_EVENT_COUNT		0x03
#define POGO_CMD_EVENTS			0x04
#define POGO_CMD_GET_MODE		0x07
#define POGO_CMD_ABORT_DFU		0x18

#define POGO_MODE_DFU			2
#define POGO_COUNT_RELEASE_ALL		0xff
#define POGO_MAX_EVENTS			100
#define POGO_MAX_ROWS			8
#define POGO_MAX_COLS			16

#define POGO_EV_PRESS			BIT(0)
#define POGO_EV_COL(b)			(((b) >> 1) & 0xf)
#define POGO_EV_ROW(b)			(((b) >> 5) & 0x7)

#define POGO_I2C_RETRIES		3
#define POGO_MAX_IRQ_ERRORS		10

#define SM5705_MUIC_ADDR		0x25
#define SM5705_MUIC_REG_ADC		0x09
#define SM5705_ADC_MASK			0x1f
#define SM5705_ADC_KEYBOARD		0x11	/* 49.9 kOhm on the ID line */

#define POGO_VENDOR_SAMSUNG		0x04e8
#define POGO_PRODUCT_KEYBOARD		0xa035

static unsigned int id_poll_ms = 500;
module_param(id_poll_ms, uint, 0644);
MODULE_PARM_DESC(id_poll_ms,
		 "Poll the MUIC ID ADC for the keyboard every N ms (0 = only the sysfs 'connected' switch)");

/* ------------------------------------------------------------------------ */
/* The iCE40 bridge                                                         */
/* ------------------------------------------------------------------------ */

struct pogo_fpga {
	struct spi_device *spi;
	struct gpio_desc *creset;	/* CRESET_B: asserted = configuration reset */
	struct gpio_desc *cdone;
	struct gpio_desc *reset;	/* logic reset (RSTN): asserted = held */
};

static struct spi_driver pogo_fpga_driver;

/* Small register writes to the FPGA logic; spi_write_then_read bounces them. */
static int pogo_fpga_cmd(struct pogo_fpga *fpga, const u8 *cmd, size_t len)
{
	return spi_write_then_read(fpga->spi, cmd, len, NULL, 0);
}

/* Downstream fpga_rstn_control(): high, low, 1 ms, high. */
static void pogo_fpga_logic_reset(struct pogo_fpga *fpga)
{
	gpiod_set_value_cansleep(fpga->reset, 0);
	gpiod_set_value_cansleep(fpga->reset, 1);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(fpga->reset, 0);
}

/* Downstream fpga_rstn_high(): done before every keyboard access. */
static void pogo_fpga_logic_run(struct pogo_fpga *fpga)
{
	gpiod_set_value_cansleep(fpga->reset, 0);
}

static bool pogo_fpga_configured(struct pogo_fpga *fpga)
{
	return gpiod_get_value_cansleep(fpga->cdone) > 0;
}

/*
 * iCE40 slave SPI configuration, in Samsung's order: SS_B low, CRESET_B
 * pulsed low, housekeeping with SS_B still low, then the bitstream and the
 * trailing clocks in one transfer.
 */
static int pogo_fpga_load(struct pogo_fpga *fpga, const struct firmware *fw)
{
	struct spi_device *spi = fpga->spi;
	struct device *dev = &spi->dev;
	struct spi_transfer assert_cs = {
		.cs_change = 1,		/* leave SS_B asserted after the message */
		.delay = { .value = 1, .unit = SPI_DELAY_UNIT_USECS },
	};
	struct spi_transfer housekeeping = {
		.cs_change = 1,
		.delay = {
			.value = POGO_FPGA_HOUSEKEEPING_US,
			.unit = SPI_DELAY_UNIT_USECS,
		},
	};
	struct spi_transfer image = { };
	struct spi_message msg;
	u8 *buf;
	int ret;

	buf = kzalloc(fw->size + POGO_FPGA_TAIL_BYTES, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, fw->data, fw->size);
	image.tx_buf = buf;
	image.len = fw->size + POGO_FPGA_TAIL_BYTES;

	spi_bus_lock(spi->controller);

	gpiod_set_value_cansleep(fpga->creset, 1);
	spi_message_init_with_transfers(&msg, &assert_cs, 1);
	ret = spi_sync_locked(spi, &msg);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(fpga->creset, 0);
	if (ret)
		goto unlock;

	if (pogo_fpga_configured(fpga)) {
		dev_err(dev, "CDONE still high after CRESET_B\n");
		ret = -EIO;
		goto unlock;
	}

	spi_message_init_with_transfers(&msg, &housekeeping, 1);
	ret = spi_sync_locked(spi, &msg);
	if (ret)
		goto unlock;

	spi_message_init_with_transfers(&msg, &image, 1);
	ret = spi_sync_locked(spi, &msg);

unlock:
	spi_bus_unlock(spi->controller);
	kfree(buf);
	return ret;
}

static int pogo_fpga_configure(struct pogo_fpga *fpga, const struct firmware *fw)
{
	/* Samsung's "reset sequence" for the logic's three channels. */
	static const u8 init_seq[][5] = {
		{ 0x00, 0x00, 0xff, 0xff, 0x80 },
		{ 0x08, 0x00, 0xff, 0xff, 0x80 },
		{ 0x10, 0x00, 0xff, 0xff, 0x80 },
	};
	struct device *dev = &fpga->spi->dev;
	int i, ret;

	ret = pogo_fpga_load(fpga, fw);
	if (ret)
		return dev_err_probe(dev, ret, "bitstream transfer failed\n");

	pogo_fpga_logic_reset(fpga);

	for (i = 0; i < ARRAY_SIZE(init_seq); i++) {
		ret = pogo_fpga_cmd(fpga, init_seq[i], sizeof(init_seq[i]));
		if (ret)
			return dev_err_probe(dev, ret, "init sequence failed\n");
	}
	usleep_range(1000, 1500);

	/* Pulse, then hold the logic in reset until a keyboard shows up. */
	gpiod_set_value_cansleep(fpga->reset, 1);
	gpiod_set_value_cansleep(fpga->reset, 0);
	msleep(10);
	gpiod_set_value_cansleep(fpga->reset, 1);

	if (!pogo_fpga_configured(fpga))
		return dev_err_probe(dev, -EIO, "CDONE low after configuration\n");

	return 0;
}

static int pogo_fpga_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	const char *fw_name = POGO_FPGA_FW_DEFAULT;
	const struct firmware *fw;
	struct pogo_fpga *fpga;
	int ret;

	fpga = devm_kzalloc(dev, sizeof(*fpga), GFP_KERNEL);
	if (!fpga)
		return -ENOMEM;
	fpga->spi = spi;

	/* Both resets asserted before the supply comes up. */
	fpga->creset = devm_gpiod_get(dev, "creset", GPIOD_OUT_HIGH);
	if (IS_ERR(fpga->creset))
		return dev_err_probe(dev, PTR_ERR(fpga->creset), "no CRESET_B GPIO\n");

	fpga->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(fpga->reset))
		return dev_err_probe(dev, PTR_ERR(fpga->reset), "no logic reset GPIO\n");

	fpga->cdone = devm_gpiod_get(dev, "cdone", GPIOD_IN);
	if (IS_ERR(fpga->cdone))
		return dev_err_probe(dev, PTR_ERR(fpga->cdone), "no CDONE GPIO\n");

	ret = devm_regulator_get_enable(dev, "vcc");
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable the FPGA supply\n");
	usleep_range(10000, 11000);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz || spi->max_speed_hz > POGO_FPGA_SPI_HZ)
		spi->max_speed_hz = POGO_FPGA_SPI_HZ;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi_setup failed\n");

	device_property_read_string(dev, "firmware-name", &fw_name);
	ret = request_firmware(&fw, fw_name, dev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot load %s\n", fw_name);

	ret = pogo_fpga_configure(fpga, fw);
	release_firmware(fw);
	if (ret)
		return ret;

	spi_set_drvdata(spi, fpga);
	dev_info(dev, "configured from %s\n", fw_name);
	return 0;
}

static int pogo_fpga_suspend(struct device *dev)
{
	struct pogo_fpga *fpga = dev_get_drvdata(dev);
	static const u8 cmd[] = { 0x38, 0x00, 0xff, 0xff, 0x00 };
	int ret;

	ret = pogo_fpga_cmd(fpga, cmd, sizeof(cmd));
	if (ret)
		dev_warn(dev, "suspend command failed: %d\n", ret);
	gpiod_set_value_cansleep(fpga->reset, 1);
	return 0;
}

static int pogo_fpga_resume(struct device *dev)
{
	struct pogo_fpga *fpga = dev_get_drvdata(dev);
	static const u8 cmd[] = { 0x38, 0x00, 0xff, 0xff, 0x01 };
	int ret;

	ret = pogo_fpga_cmd(fpga, cmd, sizeof(cmd));
	if (ret)
		dev_warn(dev, "resume command failed: %d\n", ret);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pogo_fpga_pm_ops, pogo_fpga_suspend, pogo_fpga_resume);

static const struct of_device_id pogo_fpga_of_match[] = {
	{ .compatible = "samsung,gts4lv-pogo-fpga" },
	{ }
};
MODULE_DEVICE_TABLE(of, pogo_fpga_of_match);

static const struct spi_device_id pogo_fpga_spi_ids[] = {
	{ "gts4lv-pogo-fpga" },
	{ }
};
MODULE_DEVICE_TABLE(spi, pogo_fpga_spi_ids);

static struct spi_driver pogo_fpga_driver = {
	.driver = {
		.name = "gts4lv-pogo-fpga",
		.of_match_table = pogo_fpga_of_match,
		.pm = pm_sleep_ptr(&pogo_fpga_pm_ops),
	},
	.id_table = pogo_fpga_spi_ids,
	.probe = pogo_fpga_probe,
};

/* ------------------------------------------------------------------------ */
/* The keyboard                                                             */
/* ------------------------------------------------------------------------ */

struct pogo_kbd {
	struct i2c_client *client;
	struct pogo_fpga *fpga;
	struct regulator *vdd;
	struct i2c_adapter *muic;	/* NULL: attach only through sysfs */

	unsigned int rows, cols, row_shift;
	unsigned short *keycodes;

	struct mutex lock;		/* everything below */
	struct input_dev *input;	/* registered while attached */
	bool want;			/* requested state */
	bool connected;			/* actual state */
	bool irq_on;
	bool suspended;
	bool irq_pending;		/* fired while suspended */
	bool irq_wake;
	unsigned int errors;
	u16 key_state[POGO_MAX_ROWS];
	u8 version[4];			/* hw_rev, model_id, minor, major */
	bool have_version;

	int adc_prev, adc_last;
	bool id_attached;

	struct work_struct connect_work;
	struct delayed_work check_work;
	struct delayed_work detect_work;
};

static void pogo_kbd_irq_enable(struct pogo_kbd *kbd, bool on)
{
	if (kbd->irq_on == on)
		return;
	kbd->irq_on = on;
	if (on)
		enable_irq(kbd->client->irq);
	else
		disable_irq_nosync(kbd->client->irq);
}

static void pogo_kbd_release_all(struct pogo_kbd *kbd)
{
	unsigned int row, col, code;

	if (!kbd->input)
		return;

	for (row = 0; row < kbd->rows; row++) {
		for (col = 0; col < kbd->cols; col++) {
			if (!(kbd->key_state[row] & BIT(col)))
				continue;
			code = MATRIX_SCAN_CODE(row, col, kbd->row_shift);
			input_event(kbd->input, EV_MSC, MSC_SCAN, code);
			input_report_key(kbd->input, kbd->keycodes[code], 0);
		}
		kbd->key_state[row] = 0;
	}
	input_sync(kbd->input);
}

/* Downstream: on any failed transfer release all keys, kick the bridge, wait. */
static void pogo_kbd_bus_error(struct pogo_kbd *kbd)
{
	pogo_kbd_release_all(kbd);
	pogo_fpga_logic_reset(kbd->fpga);
	usleep_range(10000, 11000);
}

static int pogo_kbd_send(struct pogo_kbd *kbd, u8 cmd)
{
	int i, ret = -EIO;

	for (i = 0; i < POGO_I2C_RETRIES; i++) {
		ret = i2c_master_send(kbd->client, &cmd, 1);
		if (ret == 1)
			return 0;
		dev_dbg(&kbd->client->dev, "write %02x failed: %d\n", cmd, ret);
		pogo_kbd_bus_error(kbd);
	}
	return ret < 0 ? ret : -EIO;
}

static int pogo_kbd_recv(struct pogo_kbd *kbd, u8 *buf, int len)
{
	int i, ret = -EIO;

	for (i = 0; i < POGO_I2C_RETRIES; i++) {
		ret = i2c_master_recv(kbd->client, buf, len);
		if (ret == len)
			return 0;
		dev_dbg(&kbd->client->dev, "read of %d failed: %d\n", len, ret);
		pogo_kbd_bus_error(kbd);
	}
	return ret < 0 ? ret : -EIO;
}

static int pogo_kbd_cmd_read(struct pogo_kbd *kbd, u8 cmd, u8 *buf, int len)
{
	int ret;

	ret = pogo_kbd_send(kbd, cmd);
	if (ret)
		return ret;
	return pogo_kbd_recv(kbd, buf, len);
}

static int pogo_kbd_read_version(struct pogo_kbd *kbd)
{
	u8 v[4];
	int ret;

	ret = pogo_kbd_cmd_read(kbd, POGO_CMD_VERSION, v, sizeof(v));
	if (ret)
		return ret;

	if (!kbd->have_version || memcmp(v, kbd->version, sizeof(v)))
		dev_info(&kbd->client->dev,
			 "keyboard firmware %u.%u, model %u, hw rev %u\n",
			 v[3], v[2], v[1], v[0]);
	memcpy(kbd->version, v, sizeof(v));
	kbd->have_version = true;
	return 0;
}

static int pogo_kbd_read_events(struct pogo_kbd *kbd)
{
	u8 ev[POGO_MAX_EVENTS];
	u8 count;
	int i, ret;

	pogo_fpga_logic_run(kbd->fpga);

	ret = pogo_kbd_cmd_read(kbd, POGO_CMD_EVENT_COUNT, &count, 1);
	if (ret)
		return ret;

	if (!count || count > POGO_MAX_EVENTS) {
		if (count == POGO_COUNT_RELEASE_ALL)
			pogo_kbd_release_all(kbd);
		else if (count)
			dev_dbg(&kbd->client->dev, "event count %u\n", count);
		/* Downstream reads the version here as well; do the same. */
		return pogo_kbd_read_version(kbd);
	}

	ret = pogo_kbd_cmd_read(kbd, POGO_CMD_EVENTS, ev, count);
	if (ret)
		return ret;

	for (i = 0; i < count; i++) {
		unsigned int row = POGO_EV_ROW(ev[i]);
		unsigned int col = POGO_EV_COL(ev[i]);
		bool press = ev[i] & POGO_EV_PRESS;
		unsigned int code;

		if (row >= kbd->rows || col >= kbd->cols)
			continue;

		if (press)
			kbd->key_state[row] |= BIT(col);
		else
			kbd->key_state[row] &= ~BIT(col);

		code = MATRIX_SCAN_CODE(row, col, kbd->row_shift);
		dev_dbg(&kbd->client->dev, "%s r%u c%u code %u\n",
			press ? "press" : "release", row, col, kbd->keycodes[code]);
		input_event(kbd->input, EV_MSC, MSC_SCAN, code);
		input_report_key(kbd->input, kbd->keycodes[code], press);
		input_sync(kbd->input);
	}
	return 0;
}

static irqreturn_t pogo_kbd_irq(int irq, void *data)
{
	struct pogo_kbd *kbd = data;

	guard(mutex)(&kbd->lock);

	if (!kbd->connected || !kbd->input || !kbd->irq_on)
		return IRQ_HANDLED;

	if (kbd->suspended) {
		/* Level IRQ: park it until the bridge and the bus are back. */
		pogo_kbd_irq_enable(kbd, false);
		kbd->irq_pending = true;
		return IRQ_HANDLED;
	}

	if (!pogo_kbd_read_events(kbd)) {
		kbd->errors = 0;
	} else if (++kbd->errors >= POGO_MAX_IRQ_ERRORS) {
		dev_err(&kbd->client->dev,
			"keyboard not answering, interrupt off until it is attached again\n");
		pogo_kbd_irq_enable(kbd, false);
	}
	return IRQ_HANDLED;
}

static void pogo_kbd_check_work(struct work_struct *work)
{
	struct pogo_kbd *kbd = container_of(to_delayed_work(work),
					    struct pogo_kbd, check_work);
	u8 mode;

	guard(mutex)(&kbd->lock);

	if (!kbd->connected)
		return;

	if (pogo_kbd_read_version(kbd) ||
	    pogo_kbd_cmd_read(kbd, POGO_CMD_GET_MODE, &mode, 1)) {
		dev_warn(&kbd->client->dev, "keyboard does not answer\n");
		return;
	}

	if (mode == POGO_MODE_DFU) {
		dev_info(&kbd->client->dev, "keyboard in DFU mode, back to its boot bank\n");
		pogo_kbd_send(kbd, POGO_CMD_ABORT_DFU);
	}
}

static int pogo_kbd_start(struct pogo_kbd *kbd)
{
	struct device *dev = &kbd->client->dev;
	struct input_dev *input;
	int ret;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;

	input->name = "Samsung Book Cover Keyboard";
	input->phys = "pogo/input0";
	input->id.bustype = BUS_I2C;
	input->id.vendor = POGO_VENDOR_SAMSUNG;
	input->id.product = POGO_PRODUCT_KEYBOARD;
	input->dev.parent = dev;

	memset(kbd->keycodes, 0, sizeof(*kbd->keycodes) * (kbd->rows << kbd->row_shift));
	ret = matrix_keypad_build_keymap(NULL, NULL, kbd->rows, kbd->cols,
					 kbd->keycodes, input);
	if (ret) {
		dev_err(dev, "bad linux,keymap: %d\n", ret);
		goto err_free;
	}
	input_set_capability(input, EV_MSC, MSC_SCAN);
	__set_bit(EV_REP, input->evbit);
	input_set_drvdata(input, kbd);

	ret = input_register_device(input);
	if (ret)
		goto err_free;
	kbd->input = input;

	ret = regulator_enable(kbd->vdd);
	if (ret) {
		dev_err(dev, "cannot enable the keyboard supply: %d\n", ret);
		goto err_unregister;
	}
	msleep(50);

	memset(kbd->key_state, 0, sizeof(kbd->key_state));
	kbd->errors = 0;
	kbd->have_version = false;
	kbd->connected = true;

	if (pogo_fpga_configured(kbd->fpga)) {
		pogo_fpga_logic_reset(kbd->fpga);
		queue_delayed_work(system_freezable_wq, &kbd->check_work,
				   msecs_to_jiffies(1000));
	} else {
		dev_warn(dev, "bridge FPGA not configured (CDONE low)\n");
	}

	pogo_kbd_irq_enable(kbd, true);
	dev_info(dev, "keyboard attached\n");
	return 0;

err_unregister:
	input_unregister_device(input);
	kbd->input = NULL;
	return ret;
err_free:
	input_free_device(input);
	return ret;
}

static void pogo_kbd_stop(struct pogo_kbd *kbd)
{
	pogo_kbd_irq_enable(kbd, false);
	kbd->irq_pending = false;
	cancel_delayed_work(&kbd->check_work);

	pogo_kbd_release_all(kbd);
	regulator_disable(kbd->vdd);
	kbd->connected = false;

	input_unregister_device(kbd->input);
	kbd->input = NULL;
	dev_info(&kbd->client->dev, "keyboard detached\n");
}

static void pogo_kbd_connect_work(struct work_struct *work)
{
	struct pogo_kbd *kbd = container_of(work, struct pogo_kbd, connect_work);

	guard(mutex)(&kbd->lock);

	if (kbd->want == kbd->connected)
		return;

	if (kbd->want) {
		if (pogo_kbd_start(kbd))
			kbd->want = false;
	} else {
		pogo_kbd_stop(kbd);
	}
}

static void pogo_kbd_request(struct pogo_kbd *kbd, bool attach)
{
	scoped_guard(mutex, &kbd->lock)
		kbd->want = attach;
	queue_work(system_freezable_wq, &kbd->connect_work);
}

static int pogo_kbd_read_id_adc(struct pogo_kbd *kbd)
{
	union i2c_smbus_data data;
	int ret;

	/* No client: userspace keeps reaching the MUIC through i2c-dev. */
	ret = i2c_smbus_xfer(kbd->muic, SM5705_MUIC_ADDR, 0, I2C_SMBUS_READ,
			     SM5705_MUIC_REG_ADC, I2C_SMBUS_BYTE_DATA, &data);
	return ret < 0 ? ret : data.byte & SM5705_ADC_MASK;
}

static void pogo_kbd_detect_work(struct work_struct *work)
{
	struct pogo_kbd *kbd = container_of(to_delayed_work(work),
					    struct pogo_kbd, detect_work);
	unsigned int period = READ_ONCE(id_poll_ms);
	int adc;

	if (!period)
		return;

	adc = pogo_kbd_read_id_adc(kbd);
	if (adc >= 0) {
		bool attached = adc == SM5705_ADC_KEYBOARD;

		/* Two equal readings in a row before acting on a change. */
		if (adc == kbd->adc_prev && attached != kbd->id_attached) {
			kbd->id_attached = attached;
			dev_dbg(&kbd->client->dev, "ID ADC 0x%02x\n", adc);
			pogo_kbd_request(kbd, attached);
		}
		kbd->adc_prev = adc;
	}
	WRITE_ONCE(kbd->adc_last, adc);

	queue_delayed_work(system_freezable_power_efficient_wq, &kbd->detect_work,
			   msecs_to_jiffies(max(period, 100U)));
}

static ssize_t connected_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct pogo_kbd *kbd = dev_get_drvdata(dev);

	guard(mutex)(&kbd->lock);
	return sysfs_emit(buf, "%d\n", kbd->connected);
}

static ssize_t connected_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct pogo_kbd *kbd = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	pogo_kbd_request(kbd, on);
	flush_work(&kbd->connect_work);
	return count;
}
static DEVICE_ATTR_RW(connected);

static ssize_t fw_version_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct pogo_kbd *kbd = dev_get_drvdata(dev);

	guard(mutex)(&kbd->lock);
	if (!kbd->connected || !kbd->have_version)
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%u.%u model %u hw %u\n", kbd->version[3],
			  kbd->version[2], kbd->version[1], kbd->version[0]);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t id_adc_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct pogo_kbd *kbd = dev_get_drvdata(dev);
	int adc = READ_ONCE(kbd->adc_last);

	if (adc < 0)
		return sysfs_emit(buf, "%d\n", adc);
	return sysfs_emit(buf, "0x%02x\n", adc);
}
static DEVICE_ATTR_RO(id_adc);

static struct attribute *pogo_kbd_attrs[] = {
	&dev_attr_connected.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_id_adc.attr,
	NULL
};
ATTRIBUTE_GROUPS(pogo_kbd);

static void pogo_kbd_put_device(void *data)
{
	put_device(data);
}

static void pogo_kbd_put_adapter(void *data)
{
	i2c_put_adapter(data);
}

static int pogo_kbd_get_fpga(struct pogo_kbd *kbd)
{
	struct device *dev = &kbd->client->dev;
	struct device_node *np;
	struct device *fdev;
	int ret;

	np = of_parse_phandle(dev->of_node, "samsung,pogo-fpga", 0);
	if (!np)
		return dev_err_probe(dev, -ENODEV, "no samsung,pogo-fpga\n");

	fdev = bus_find_device_by_of_node(&spi_bus_type, np);
	of_node_put(np);
	if (!fdev)
		return dev_err_probe(dev, -EPROBE_DEFER, "bridge FPGA not there yet\n");

	ret = devm_add_action_or_reset(dev, pogo_kbd_put_device, fdev);
	if (ret)
		return ret;

	if (fdev->driver != &pogo_fpga_driver.driver || !dev_get_drvdata(fdev))
		return dev_err_probe(dev, -EPROBE_DEFER, "bridge FPGA not bound yet\n");

	/* Unbinds us before the FPGA and orders suspend/resume after it. */
	if (!device_link_add(dev, fdev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return dev_err_probe(dev, -EINVAL, "cannot link to the bridge FPGA\n");

	kbd->fpga = dev_get_drvdata(fdev);
	return 0;
}

static int pogo_kbd_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np;
	struct pogo_kbd *kbd;
	int ret;

	if (!client->irq)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");

	kbd = devm_kzalloc(dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;
	kbd->client = client;
	kbd->adc_prev = -1;
	kbd->adc_last = -ENODATA;

	ret = devm_mutex_init(dev, &kbd->lock);
	if (ret)
		return ret;
	INIT_WORK(&kbd->connect_work, pogo_kbd_connect_work);
	INIT_DELAYED_WORK(&kbd->check_work, pogo_kbd_check_work);
	INIT_DELAYED_WORK(&kbd->detect_work, pogo_kbd_detect_work);

	ret = pogo_kbd_get_fpga(kbd);
	if (ret)
		return ret;

	kbd->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(kbd->vdd))
		return dev_err_probe(dev, PTR_ERR(kbd->vdd), "no vdd supply\n");

	ret = matrix_keypad_parse_properties(dev, &kbd->rows, &kbd->cols);
	if (ret)
		return ret;
	if (kbd->rows > POGO_MAX_ROWS || kbd->cols > POGO_MAX_COLS)
		return dev_err_probe(dev, -EINVAL, "matrix %ux%u too big\n",
				     kbd->rows, kbd->cols);
	kbd->row_shift = get_count_order(kbd->cols);
	kbd->keycodes = devm_kcalloc(dev, kbd->rows << kbd->row_shift,
				     sizeof(*kbd->keycodes), GFP_KERNEL);
	if (!kbd->keycodes)
		return -ENOMEM;

	np = of_parse_phandle(dev->of_node, "samsung,muic-i2c-bus", 0);
	if (np) {
		kbd->muic = of_find_i2c_adapter_by_node(np);
		of_node_put(np);
		if (!kbd->muic)
			return dev_err_probe(dev, -EPROBE_DEFER, "MUIC bus not there yet\n");
		ret = devm_add_action_or_reset(dev, pogo_kbd_put_adapter, kbd->muic);
		if (ret)
			return ret;
	}

	i2c_set_clientdata(client, kbd);

	ret = devm_request_threaded_irq(dev, client->irq, NULL, pogo_kbd_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), kbd);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request the interrupt\n");

	/* Wake-capable, but off unless userspace turns power/wakeup on. */
	device_set_wakeup_capable(dev, true);

	if (kbd->muic)
		queue_delayed_work(system_freezable_power_efficient_wq,
				   &kbd->detect_work, 0);
	else
		dev_info(dev, "no samsung,muic-i2c-bus: attach through 'connected'\n");

	return 0;
}

static void pogo_kbd_remove(struct i2c_client *client)
{
	struct pogo_kbd *kbd = i2c_get_clientdata(client);

	disable_delayed_work_sync(&kbd->detect_work);
	disable_work_sync(&kbd->connect_work);
	disable_delayed_work_sync(&kbd->check_work);

	scoped_guard(mutex, &kbd->lock) {
		kbd->want = false;
		if (kbd->connected)
			pogo_kbd_stop(kbd);
	}
	device_set_wakeup_capable(&client->dev, false);
}

static int pogo_kbd_suspend(struct device *dev)
{
	struct pogo_kbd *kbd = dev_get_drvdata(dev);

	guard(mutex)(&kbd->lock);
	kbd->suspended = true;
	/* Best effort: gpio108 has no PDC wake line on SDM670. */
	if (kbd->connected && device_may_wakeup(dev))
		kbd->irq_wake = !enable_irq_wake(kbd->client->irq);
	return 0;
}

static int pogo_kbd_resume(struct device *dev)
{
	struct pogo_kbd *kbd = dev_get_drvdata(dev);

	guard(mutex)(&kbd->lock);
	if (kbd->irq_wake) {
		disable_irq_wake(kbd->client->irq);
		kbd->irq_wake = false;
	}
	kbd->suspended = false;
	if (kbd->irq_pending && kbd->connected) {
		kbd->irq_pending = false;
		pogo_kbd_irq_enable(kbd, true);
	}
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pogo_kbd_pm_ops, pogo_kbd_suspend, pogo_kbd_resume);

static const struct of_device_id pogo_kbd_of_match[] = {
	{ .compatible = "samsung,gts4lv-pogo-kbd" },
	{ }
};
MODULE_DEVICE_TABLE(of, pogo_kbd_of_match);

static const struct i2c_device_id pogo_kbd_i2c_ids[] = {
	{ "gts4lv-pogo-kbd" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pogo_kbd_i2c_ids);

static struct i2c_driver pogo_kbd_driver = {
	.driver = {
		.name = "gts4lv-pogo-kbd",
		.of_match_table = pogo_kbd_of_match,
		.dev_groups = pogo_kbd_groups,
		.pm = pm_sleep_ptr(&pogo_kbd_pm_ops),
	},
	.id_table = pogo_kbd_i2c_ids,
	.probe = pogo_kbd_probe,
	.remove = pogo_kbd_remove,
};

static int __init pogo_kbd_init(void)
{
	int ret;

	ret = spi_register_driver(&pogo_fpga_driver);
	if (ret)
		return ret;

	ret = i2c_add_driver(&pogo_kbd_driver);
	if (ret)
		spi_unregister_driver(&pogo_fpga_driver);
	return ret;
}
module_init(pogo_kbd_init);

static void __exit pogo_kbd_exit(void)
{
	i2c_del_driver(&pogo_kbd_driver);
	spi_unregister_driver(&pogo_fpga_driver);
}
module_exit(pogo_kbd_exit);

MODULE_FIRMWARE(POGO_FPGA_FW_DEFAULT);
MODULE_DESCRIPTION("Samsung Galaxy Tab S5e POGO keyboard (STM32 behind an iCE40 bridge)");
MODULE_LICENSE("GPL");
