// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB-C DisplayPort alt mode on the Galaxy Tab S5e (gts4lv), driven from userspace.
 *
 * The S2MM005 Type-C/PD controller has no mainline driver: gts4lv-usb-otgd talks to it
 * over i2c-dev and runs the DP alt mode exchange (Discover, Enter Mode, Status Update,
 * Configure) itself. This is the kernel half. It owns the usb-c-connector graph and, on
 * writes to its sysfs files, sets the orientation switches (PTN36502 redriver, SBU mux and,
 * through the redriver, the QMP PHY), the mode (the redriver as retimer, the SBU mux as mode
 * switch, the QMP PHY behind the redriver) and DP HPD through an aux-hpd bridge at the end
 * of the DP bridge chain. HPD has no wire on this board: it only exists in the DP VDMs.
 *
 *   orientation  none | normal | reverse
 *   mode         safe | usb | dp-c | dp-d | dp-e   (DP pin assignment C, D or E)
 *   hpd          0 | 1 | irq                       (irq: an IRQ_HPD with HPD high)
 *
 * Order for a DP sink: orientation, mode, then hpd 1. For an unplug: hpd 0, then mode safe
 * and orientation none (the QMP PHY keeps its mode while DP still has it powered).
 */

#include <linux/auxiliary_bus.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_retimer.h>

#include <drm/bridge/aux-bridge.h>

struct gts4lv_dp {
	struct device *dev;
	struct mutex lock; /* protects the state below and the order of the switch calls */
	struct auxiliary_device *bridge;
	struct typec_switch *sw;
	struct typec_mux *mux;
	struct typec_retimer *retimer;
	struct typec_altmode dp_alt;
	enum typec_orientation orientation;
	unsigned long mode;
	bool hpd;
};

static const char * const orientation_names[] = {
	[TYPEC_ORIENTATION_NONE] = "none",
	[TYPEC_ORIENTATION_NORMAL] = "normal",
	[TYPEC_ORIENTATION_REVERSE] = "reverse",
};

static const struct {
	const char *name;
	unsigned long mode;
} modes[] = {
	{ "safe", TYPEC_STATE_SAFE },
	{ "usb", TYPEC_STATE_USB },
	{ "dp-c", TYPEC_DP_STATE_C },
	{ "dp-d", TYPEC_DP_STATE_D },
	{ "dp-e", TYPEC_DP_STATE_E },
};

static int gts4lv_dp_set_mode(struct gts4lv_dp *dp, bool irq_hpd)
{
	struct typec_displayport_data dp_data = {};
	struct typec_retimer_state retimer_state = {};
	struct typec_mux_state state = {};
	int ret;

	state.mode = dp->mode;
	if (dp->mode >= TYPEC_STATE_MODAL) {
		dp_data.status = DP_STATUS_ENABLED;
		if (dp->hpd)
			dp_data.status |= DP_STATUS_HPD_STATE;
		if (irq_hpd)
			dp_data.status |= DP_STATUS_IRQ_HPD;
		dp_data.conf = DP_CONF_SET_PIN_ASSIGN(BIT(dp->mode - TYPEC_STATE_MODAL));
		state.alt = &dp->dp_alt;
		state.data = &dp_data;
	}

	ret = typec_mux_set(dp->mux, &state);
	if (ret)
		dev_err(dp->dev, "mode switch to %lu failed: %d\n", dp->mode, ret);

	retimer_state.alt = state.alt;
	retimer_state.data = state.data;
	retimer_state.mode = state.mode;
	ret = typec_retimer_set(dp->retimer, &retimer_state);
	if (ret)
		dev_err(dp->dev, "retimer switch to %lu failed: %d\n", dp->mode, ret);

	return ret;
}

static void gts4lv_dp_notify(struct gts4lv_dp *dp)
{
	drm_aux_hpd_bridge_notify(&dp->bridge->dev, dp->hpd ? connector_status_connected
							    : connector_status_disconnected);
}

static ssize_t orientation_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gts4lv_dp *dp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", orientation_names[dp->orientation]);
}

static ssize_t orientation_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct gts4lv_dp *dp = dev_get_drvdata(dev);
	int orientation, ret;

	orientation = sysfs_match_string(orientation_names, buf);
	if (orientation < 0)
		return orientation;

	mutex_lock(&dp->lock);
	ret = typec_switch_set(dp->sw, orientation);
	if (!ret)
		dp->orientation = orientation;
	mutex_unlock(&dp->lock);

	return ret ?: count;
}
static DEVICE_ATTR_RW(orientation);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gts4lv_dp *dp = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++)
		if (modes[i].mode == dp->mode)
			return sysfs_emit(buf, "%s\n", modes[i].name);

	return sysfs_emit(buf, "%lu\n", dp->mode);
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct gts4lv_dp *dp = dev_get_drvdata(dev);
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(modes); i++)
		if (sysfs_streq(buf, modes[i].name))
			break;
	if (i == ARRAY_SIZE(modes))
		return -EINVAL;

	mutex_lock(&dp->lock);
	dp->mode = modes[i].mode;
	ret = gts4lv_dp_set_mode(dp, false);
	mutex_unlock(&dp->lock);

	return ret ?: count;
}
static DEVICE_ATTR_RW(mode);

static ssize_t hpd_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gts4lv_dp *dp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", dp->hpd);
}

static ssize_t hpd_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct gts4lv_dp *dp = dev_get_drvdata(dev);
	bool irq = sysfs_streq(buf, "irq");
	bool hpd;
	int ret = 0;

	if (irq)
		hpd = true;
	else if (kstrtobool(buf, &hpd))
		return -EINVAL;

	mutex_lock(&dp->lock);
	if (hpd && dp->mode < TYPEC_STATE_MODAL) {
		ret = -EBUSY;	/* the lanes are not in a DP mode */
	} else {
		dp->hpd = hpd;
		if (dp->mode >= TYPEC_STATE_MODAL)
			gts4lv_dp_set_mode(dp, irq);
		gts4lv_dp_notify(dp);
	}
	mutex_unlock(&dp->lock);

	return ret ?: count;
}
static DEVICE_ATTR_RW(hpd);

static struct attribute *gts4lv_dp_attrs[] = {
	&dev_attr_orientation.attr,
	&dev_attr_mode.attr,
	&dev_attr_hpd.attr,
	NULL
};
ATTRIBUTE_GROUPS(gts4lv_dp);

static void gts4lv_dp_put_mux(void *data)
{
	typec_mux_put(data);
}

static void gts4lv_dp_put_retimer(void *data)
{
	typec_retimer_put(data);
}

static void gts4lv_dp_put_switch(void *data)
{
	typec_switch_put(data);
}

static int gts4lv_dp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *connector;
	struct gts4lv_dp *dp;
	int ret;

	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;

	dp->dev = dev;
	mutex_init(&dp->lock);
	dp->orientation = TYPEC_ORIENTATION_NONE;
	dp->mode = TYPEC_STATE_SAFE;
	dp->dp_alt.svid = USB_TYPEC_DP_SID;
	dp->dp_alt.mode = USB_TYPEC_DP_MODE;
	dp->dp_alt.active = 1;

	connector = device_get_named_child_node(dev, "connector");
	if (!connector)
		return dev_err_probe(dev, -ENODEV, "no connector node\n");

	dp->bridge = devm_drm_dp_hpd_bridge_alloc(dev, to_of_node(connector));
	if (IS_ERR(dp->bridge)) {
		ret = PTR_ERR(dp->bridge);
		goto out;
	}

	dp->mux = fwnode_typec_mux_get(connector);
	if (IS_ERR(dp->mux)) {
		ret = dev_err_probe(dev, PTR_ERR(dp->mux), "no mode-switch\n");
		goto out;
	}
	ret = devm_add_action_or_reset(dev, gts4lv_dp_put_mux, dp->mux);
	if (ret)
		goto out;

	dp->retimer = fwnode_typec_retimer_get(connector);
	if (IS_ERR(dp->retimer)) {
		ret = dev_err_probe(dev, PTR_ERR(dp->retimer), "no retimer-switch\n");
		goto out;
	}
	ret = devm_add_action_or_reset(dev, gts4lv_dp_put_retimer, dp->retimer);
	if (ret)
		goto out;

	dp->sw = fwnode_typec_switch_get(connector);
	if (IS_ERR(dp->sw)) {
		ret = dev_err_probe(dev, PTR_ERR(dp->sw), "no orientation-switch\n");
		goto out;
	}
	ret = devm_add_action_or_reset(dev, gts4lv_dp_put_switch, dp->sw);
	if (ret)
		goto out;

	platform_set_drvdata(pdev, dp);

	ret = devm_drm_dp_hpd_bridge_add(dev, dp->bridge);
out:
	fwnode_handle_put(connector);
	return ret;
}

static const struct of_device_id gts4lv_dp_of_match[] = {
	{ .compatible = "samsung,gts4lv-dp-altmode" },
	{ }
};
MODULE_DEVICE_TABLE(of, gts4lv_dp_of_match);

static struct platform_driver gts4lv_dp_driver = {
	.probe = gts4lv_dp_probe,
	.driver = {
		.name = "gts4lv-dp-altmode",
		.of_match_table = gts4lv_dp_of_match,
		.dev_groups = gts4lv_dp_groups,
	},
};
module_platform_driver(gts4lv_dp_driver);

MODULE_DESCRIPTION("Galaxy Tab S5e USB-C DisplayPort alt mode, driven from userspace");
MODULE_LICENSE("GPL");
