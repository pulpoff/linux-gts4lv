// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5705 fuel gauge and charger status, read-only.
 *
 * The fuel gauge (i2c 0x71) runs its own model, so this only reports what it
 * computes; the charger (i2c 0x49) charges on its own and is only asked
 * whether VBUS is present. With an interrupt, a cable plugged in or pulled
 * out also wakes the system. Register formats are from Samsung's downstream
 * drivers (sm5705_fuelgauge.c, sm5705_charger.c).
 */

#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/device/faux.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pm_wakeirq.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define FG_REG_SOC		0x05	/* [15:8] %, [7:0] /256 */
#define FG_REG_VOLTAGE		0x07	/* [13:11] V, [10:0] /2048 V */
#define FG_REG_CURRENT		0x08	/* [15] sign, [12:11] A, [10:0] /2048 A */
#define FG_REG_TEMPERATURE	0x09	/* [15] sign, [14:8] °C, [7:4] /16 °C */

/* INT1..INT4 latch events and clear on read; nINT is low while any unmasked one is set */
#define CHG_REG_INT1		0x00
#define CHG_REG_INTMSK1		0x04	/* INTMSK1..4: 1 = masked */
#define CHG_INT1_VBUSPOK	BIT(0)
#define CHG_INT1_VBUSUVLO	BIT(1)
#define CHG_INT_GROUPS		4
#define CHG_REG_STATUS1		0x08
#define CHG_STATUS1_VBUSPOK	BIT(0)
#define CHG_REG_STATUS2		0x09
#define CHG_STATUS2_CHGON	BIT(3)
#define CHG_STATUS2_TOPOFF	BIT(5)
#define CHG_STATUS2_DONE	BIT(6)

#define POLL_INTERVAL		(30 * HZ)

/* EB-BT725ABU: 7040 mAh, 3.85 V nominal (27.1 Wh), for UPower's energy and rate */
#define NOMINAL_UV		3850000

struct sm5705 {
	struct i2c_client *client;
	struct power_supply *psy;
	struct delayed_work work;
	u32 charge_full_design;
	struct gpio_desc *int_gpio;	/* nINT from chg_int_line, else NULL */
};

static int fg_read(struct sm5705 *sm, u8 reg)
{
	return i2c_smbus_read_word_data(sm->client, reg);
}

/* Battery status from the charger next to it, if it has probed. */
static struct power_supply *sm5705_charger(void)
{
	return power_supply_get_by_name("sm5705-charger");
}

static int sm5705_battery_status(struct sm5705 *sm, int soc, int cur_ma)
{
	struct power_supply *chg = sm5705_charger();
	union power_supply_propval online = { 0 };

	if (chg) {
		power_supply_get_property(chg, POWER_SUPPLY_PROP_ONLINE, &online);
		power_supply_put(chg);
	}
	if (!online.intval)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (soc >= 100 || cur_ma < 30)
		return POWER_SUPPLY_STATUS_FULL;
	return POWER_SUPPLY_STATUS_CHARGING;
}

static int fg_current_ma(int raw)
{
	int ma = ((raw & 0x1800) >> 11) * 1000 + ((raw & 0x07ff) * 1000) / 2048;

	return (raw & 0x8000) ? -ma : ma;
}

/* The charge, from the state of charge in 1/256 %: in µAh, or in µWh at the nominal voltage. */
static int fg_charge_uah(struct sm5705 *sm, int *uah)
{
	int raw = fg_read(sm, FG_REG_SOC);

	if (raw < 0)
		return raw;
	*uah = div_u64((u64)clamp(raw, 0, 100 << 8) * sm->charge_full_design, 100 << 8);
	return 0;
}

static int uah_to_uwh(int uah)
{
	return div_u64((u64)uah * NOMINAL_UV, 1000000);
}

static int sm5705_fg_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct sm5705 *sm = power_supply_get_drvdata(psy);
	int raw, soc;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = sm->charge_full_design;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		val->intval = uah_to_uwh(sm->charge_full_design);
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		return fg_charge_uah(sm, &val->intval);
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		raw = fg_charge_uah(sm, &soc);
		if (raw)
			return raw;
		val->intval = uah_to_uwh(soc);
		return 0;
	case POWER_SUPPLY_PROP_POWER_NOW:
		/* UPower wants the rate as a magnitude; the sign is in the status */
		soc = fg_read(sm, FG_REG_VOLTAGE);
		raw = fg_read(sm, FG_REG_CURRENT);
		if (soc < 0 || raw < 0)
			return -EIO;
		soc = ((soc & 0x3800) >> 11) * 1000 + ((soc & 0x07ff) * 1000) / 2048;	/* mV */
		val->intval = abs(fg_current_ma(raw)) * soc;	/* mA * mV = µW */
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		raw = fg_read(sm, FG_REG_SOC);
		if (raw < 0)
			return raw;
		val->intval = clamp(raw >> 8, 0, 100);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		raw = fg_read(sm, FG_REG_VOLTAGE);
		if (raw < 0)
			return raw;
		val->intval = (((raw & 0x3800) >> 11) * 1000 +
			       ((raw & 0x07ff) * 1000) / 2048) * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		raw = fg_read(sm, FG_REG_CURRENT);
		if (raw < 0)
			return raw;
		val->intval = fg_current_ma(raw) * 1000;
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		raw = fg_read(sm, FG_REG_TEMPERATURE);
		if (raw < 0)
			return raw;
		val->intval = ((raw & 0x7f00) >> 8) * 10 + ((raw & 0x00f0) * 10) / 256;
		if (raw & 0x8000)
			val->intval = -val->intval;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		soc = fg_read(sm, FG_REG_SOC);
		raw = fg_read(sm, FG_REG_CURRENT);
		if (soc < 0 || raw < 0)
			return -EIO;
		val->intval = sm5705_battery_status(sm, soc >> 8, fg_current_ma(raw));
		return 0;
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property sm5705_fg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
};

static const struct power_supply_desc sm5705_fg_desc = {
	.name = "sm5705-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm5705_fg_props,
	.num_properties = ARRAY_SIZE(sm5705_fg_props),
	.get_property = sm5705_fg_get_property,
};

static int sm5705_chg_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct sm5705 *sm = power_supply_get_drvdata(psy);
	int raw;

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	raw = i2c_smbus_read_byte_data(sm->client, CHG_REG_STATUS1);
	if (raw < 0)
		return raw;
	val->intval = !!(raw & CHG_STATUS1_VBUSPOK);
	return 0;
}

static const enum power_supply_property sm5705_chg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sm5705_chg_desc = {
	.name = "sm5705-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm5705_chg_props,
	.num_properties = ARRAY_SIZE(sm5705_chg_props),
	.get_property = sm5705_chg_get_property,
};

/*
 * Neither chip interrupts us here; poll so UPower sees changes. Frozen across
 * system sleep: a wakeup event every 30 s kept s2idle from lasting.
 */
static void sm5705_poll(struct work_struct *work)
{
	struct sm5705 *sm = container_of(work, struct sm5705, work.work);

	power_supply_changed(sm->psy);
	queue_delayed_work(system_freezable_wq, &sm->work, POLL_INTERVAL);
}

/*
 * nINT for a DT that does not name it yet, as "<gpiochip label>:<line>"
 * ("3400000.pinctrl:22" on the Tab S5e).
 */
static char *chg_int_line;
module_param(chg_int_line, charp, 0444);
MODULE_PARM_DESC(chg_int_line, "charger nINT as <gpiochip label>:<line>, when the DT has no interrupt");

static irqreturn_t sm5705_chg_irq(int irq, void *data)
{
	struct sm5705 *sm = data;
	u32 seen = 0;
	int i, ret;

	/* reading clears them, which releases nINT */
	for (i = 0; i < CHG_INT_GROUPS; i++) {
		ret = i2c_smbus_read_byte_data(sm->client, CHG_REG_INT1 + i);
		if (ret > 0)
			seen |= (u32)ret << (8 * i);
	}
	if (!seen)
		return IRQ_NONE;
	dev_dbg(&sm->client->dev, "INT1..4 %08x\n", seen);
	power_supply_changed(sm->psy);
	return IRQ_HANDLED;
}

static unsigned long sm5705_no_pull[] = { PIN_CONF_PACKED(PIN_CONFIG_BIAS_DISABLE, 0) };

/*
 * No pull on the pin, as the DT's pinctrl would set it: nINT has its own
 * pull-up, and the pull-down the TLMM comes out of reset with held it low.
 * A pinctrl map does not apply to a device with a DT node and no pinctrl-0,
 * so it goes through a node-less stand-in. Qualcomm's TLMM names the pin's
 * group "gpio<line>".
 */
static int sm5705_chg_int_no_pull(struct device *dev, const char *ctrl, unsigned int line)
{
	struct pinctrl_map map;
	struct faux_device *fdev;
	struct pinctrl *pins;
	char group[16];
	int ret;

	fdev = faux_device_create("sm5705-nint", dev, NULL);
	if (!fdev)
		return -ENOMEM;
	snprintf(group, sizeof(group), "gpio%u", line);
	map = (struct pinctrl_map)PIN_MAP_CONFIGS_GROUP("sm5705-nint", PINCTRL_STATE_DEFAULT,
							ctrl, group, sm5705_no_pull);
	ret = pinctrl_register_mappings(&map, 1);
	if (!ret) {
		pins = pinctrl_get_select_default(&fdev->dev);
		if (IS_ERR(pins))
			ret = PTR_ERR(pins);
		else
			pinctrl_put(pins);	/* a config is not undone on release */
		pinctrl_unregister_mappings(&map);
	}
	faux_device_destroy(fdev);
	return ret;
}

/* The interrupt of chg_int_line, with the pin set up as the DT would. */
static int sm5705_chg_irq_from_param(struct sm5705 *sm, unsigned long *flags)
{
	struct gpio_device *gdev __free(gpio_device_put) = NULL;
	struct device *dev = &sm->client->dev;
	char label[64], *colon;
	unsigned int line;
	int ret;

	strscpy(label, chg_int_line, sizeof(label));
	colon = strrchr(label, ':');
	if (!colon || kstrtouint(colon + 1, 0, &line))
		return dev_err_probe(dev, -EINVAL, "chg_int_line \"%s\": not <label>:<line>\n", chg_int_line);
	*colon = '\0';
	gdev = gpio_device_find_by_label(label);
	if (!gdev)
		return -EPROBE_DEFER;
	sm->int_gpio = gpio_device_get_desc(gdev, line);
	if (IS_ERR(sm->int_gpio))
		return PTR_ERR(sm->int_gpio);

	ret = sm5705_chg_int_no_pull(dev, label, line);
	if (ret)
		return ret;

	*flags = IRQF_TRIGGER_LOW;
	return gpiod_to_irq(sm->int_gpio);
}

/*
 * The charger's nINT: VBUS coming (VBUSPOK) and going (VBUSUVLO) only, so the
 * cable wakes the system and the online state changes at once. Without an
 * interrupt the 30 s poll is all there is.
 */
static int sm5705_chg_setup_irq(struct sm5705 *sm)
{
	struct device *dev = &sm->client->dev;
	unsigned long flags = 0;
	int irq = sm->client->irq, i, ret;

	if (irq <= 0 && chg_int_line)
		irq = sm5705_chg_irq_from_param(sm, &flags);
	if (irq < 0)
		return irq;
	if (!irq)
		return 0;

	for (i = 0; i < CHG_INT_GROUPS; i++) {
		ret = i2c_smbus_write_byte_data(sm->client, CHG_REG_INTMSK1 + i,
						i ? 0xff : (u8)~(CHG_INT1_VBUSPOK | CHG_INT1_VBUSUVLO));
		if (ret)
			return ret;
	}
	for (i = 0; i < CHG_INT_GROUPS; i++)
		i2c_smbus_read_byte_data(sm->client, CHG_REG_INT1 + i);
	/* still low with nothing latched (after the pull-up had time): it would only ever interrupt for nothing */
	usleep_range(1000, 2000);
	if (sm->int_gpio && gpiod_get_raw_value(sm->int_gpio) == 0)
		return dev_err_probe(dev, -EIO, "%s stays low\n", chg_int_line);

	ret = devm_request_threaded_irq(dev, irq, NULL, sm5705_chg_irq, flags | IRQF_ONESHOT,
					"sm5705-charger", sm);
	if (ret)
		return dev_err_probe(dev, ret, "irq %d\n", irq);
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;
	return devm_pm_set_wake_irq(dev, irq);
}

static int sm5705_probe(struct i2c_client *client)
{
	const struct power_supply_desc *desc = of_device_get_match_data(&client->dev);
	struct power_supply_config cfg = {};
	struct sm5705 *sm;
	int ret;

	sm = devm_kzalloc(&client->dev, sizeof(*sm), GFP_KERNEL);
	if (!sm)
		return -ENOMEM;
	sm->client = client;
	device_property_read_u32(&client->dev, "charge-full-design-microamp-hours",
				 &sm->charge_full_design);

	/* Fail early on a chip that is not there. */
	ret = desc == &sm5705_fg_desc ? fg_read(sm, FG_REG_SOC) :
		i2c_smbus_read_byte_data(client, CHG_REG_STATUS1);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "no response\n");

	cfg.drv_data = sm;
	cfg.fwnode = dev_fwnode(&client->dev);
	/* polled status, not an event: never a reason to stay awake */
	cfg.no_wakeup_source = true;
	sm->psy = devm_power_supply_register(&client->dev, desc, &cfg);
	if (IS_ERR(sm->psy))
		return PTR_ERR(sm->psy);

	if (desc == &sm5705_chg_desc) {
		/* without it the charger still reports, polled */
		ret = sm5705_chg_setup_irq(sm);
		if (ret == -EPROBE_DEFER)
			return ret;
		if (ret)
			dev_warn(&client->dev, "no VBUS interrupt: %pe\n", ERR_PTR(ret));
	}

	ret = devm_delayed_work_autocancel(&client->dev, &sm->work, sm5705_poll);
	if (ret)
		return ret;
	queue_delayed_work(system_freezable_wq, &sm->work, POLL_INTERVAL);
	return 0;
}

static const struct of_device_id sm5705_of_match[] = {
	{ .compatible = "siliconmitus,sm5705-fuel-gauge", .data = &sm5705_fg_desc },
	{ .compatible = "siliconmitus,sm5705-charger", .data = &sm5705_chg_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5705_of_match);

static struct i2c_driver sm5705_driver = {
	.driver = {
		.name = "sm5705-battery",
		.of_match_table = sm5705_of_match,
	},
	.probe = sm5705_probe,
};
module_i2c_driver(sm5705_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5705 fuel gauge and charger status");
MODULE_LICENSE("GPL");
