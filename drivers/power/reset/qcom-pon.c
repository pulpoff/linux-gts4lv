// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_REV2			0x01
#define PON_PS_HOLD_RST_CTL		0x5a
#define PON_PS_HOLD_RST_CTL2		0x5b
#define  PON_PS_HOLD_ENABLE		BIT(7)
#define  PON_PS_HOLD_TYPE_MASK		0x0f
#define  PON_PS_HOLD_TYPE_SHUTDOWN	4
#define  PON_PS_HOLD_TYPE_HARD_RESET	7
#define PON_SOFT_RB_SPARE		0x8f

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

struct qcom_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
	struct notifier_block secondary_nb;
	struct notifier_block restart_nb;
};

/*
 * A secondary PMIC follows PS_HOLD on its own: unless told otherwise it may
 * only warm-reset with the primary, keeping its regulators in whatever state
 * Linux left them. Have it shut down instead, as downstream does
 * ("qcom,secondary-pon-reset"), so it comes back cold with the primary.
 */
static void qcom_pon_set_ps_hold_type(struct qcom_pon *pon, unsigned int type)
{
	unsigned int rev = 0, enable_reg;

	regmap_read(pon->regmap, pon->baseaddr + PON_REV2, &rev);
	/* Revision 0 has the enable bit in the control register itself. */
	enable_reg = rev ? PON_PS_HOLD_RST_CTL2 : PON_PS_HOLD_RST_CTL;

	regmap_update_bits(pon->regmap, pon->baseaddr + enable_reg,
			   PON_PS_HOLD_ENABLE, 0);
	/* Updates of the enable bit need 3 sleep-clock cycles between them. */
	usleep_range(100, 1000);
	regmap_update_bits(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL,
			   PON_PS_HOLD_TYPE_MASK, type);
	regmap_update_bits(pon->regmap, pon->baseaddr + enable_reg,
			   PON_PS_HOLD_ENABLE, PON_PS_HOLD_ENABLE);
}

static int qcom_pon_secondary_reboot_notify(struct notifier_block *nb,
					    unsigned long code, void *unused)
{
	struct qcom_pon *pon = container_of(nb, struct qcom_pon, secondary_nb);

	qcom_pon_set_ps_hold_type(pon, PON_PS_HOLD_TYPE_SHUTDOWN);
	return NOTIFY_DONE;
}

/*
 * PSCI only drops PS_HOLD; what the PMIC does then is whatever type was last
 * programmed, and the boot firmware leaves it at warm reset. Downstream sets
 * a hard reset before every restart, and some bootloaders (Samsung's ABL on
 * gts4lv) hang on a warm reset after Linux has driven the hardware.
 */
static int qcom_pon_restart_notify(struct notifier_block *nb,
				   unsigned long code, void *unused)
{
	struct qcom_pon *pon = container_of(nb, struct qcom_pon, restart_nb);

	if (code == SYS_RESTART)
		qcom_pon_set_ps_hold_type(pon, PON_PS_HOLD_TYPE_HARD_RESET);
	return NOTIFY_DONE;
}

static int qcom_pon_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct qcom_pon *pon = container_of
			(reboot, struct qcom_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static int qcom_pon_probe(struct platform_device *pdev)
{
	struct qcom_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	/* Reboot modes belong to the primary PMIC; a second one would clash. */
	if (reason_shift != NO_REASON_SHIFT &&
	    !of_property_read_bool(pdev->dev.of_node, "qcom,secondary-pon-reset")) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = qcom_pon_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	if (of_property_read_bool(pdev->dev.of_node, "qcom,secondary-pon-reset")) {
		pon->secondary_nb.notifier_call = qcom_pon_secondary_reboot_notify;
		error = devm_register_reboot_notifier(&pdev->dev, &pon->secondary_nb);
		if (error)
			return error;
	}

	if (of_property_read_bool(pdev->dev.of_node, "qcom,hard-reset-restart")) {
		pon->restart_nb.notifier_call = qcom_pon_restart_notify;
		error = devm_register_reboot_notifier(&pdev->dev, &pon->restart_nb);
		if (error)
			return error;
	}

	platform_set_drvdata(pdev, pon);

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id qcom_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pon_id_table);

static struct platform_driver qcom_pon_driver = {
	.probe = qcom_pon_probe,
	.driver = {
		.name = "qcom-pon",
		.of_match_table = qcom_pon_id_table,
	},
};
module_platform_driver(qcom_pon_driver);

MODULE_DESCRIPTION("Qualcomm Power On driver");
MODULE_LICENSE("GPL v2");
