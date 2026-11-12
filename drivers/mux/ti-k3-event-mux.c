// SPDX-License-Identifier: GPL-2.0
/*
 * TI K3 event mux router driver
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com
 *
 * Based on drivers/mux/mmio.c by Philipp Zabel <kernel@pengutronix.de>
 *
 * Author: Rahul Sharma <r-sharma3@ti.com>
 *
 * Consumer-selected routes, Time Sync Router support and the TRM
 * programming sequence: James Clark <jjc@jclark.com>
 */

#include <linux/bits.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

/*
 * Each router output n has a MUXCNTL register at 0x4 + 4 * n. Its low bits
 * select the input routed to output n, and INT_ENABLE enables the output.
 * Each output is one mux control, and its states are the input numbers.
 */
#define TI_K3_EVENT_MUX_CNTL(n)		(0x4 + 4 * (n))
#define TI_K3_EVENT_MUX_INT_ENABLE	BIT(16)

struct ti_k3_event_mux_data {
	unsigned int num_outputs;
	unsigned int num_inputs;
};

struct ti_k3_event_mux {
	struct regmap *regmap;
	u32 *saved;
};

/*
 * Write MUXCNTL in the order the TRM asks for: clear INT_ENABLE without
 * changing the input selection, change the selection with INT_ENABLE clear,
 * then set INT_ENABLE without changing the selection. Changing the selection
 * of an enabled output can make the output toggle spuriously.
 */
static int ti_k3_event_mux_write(struct regmap *regmap, unsigned int reg,
				 u32 val)
{
	unsigned int old;
	int ret;

	ret = regmap_read(regmap, reg, &old);
	if (ret)
		return ret;

	if (old & TI_K3_EVENT_MUX_INT_ENABLE) {
		ret = regmap_write(regmap, reg,
				   old & ~TI_K3_EVENT_MUX_INT_ENABLE);
		if (ret)
			return ret;
	}

	ret = regmap_write(regmap, reg, val & ~TI_K3_EVENT_MUX_INT_ENABLE);
	if (ret || !(val & TI_K3_EVENT_MUX_INT_ENABLE))
		return ret;

	return regmap_write(regmap, reg, val);
}

static int ti_k3_event_mux_set(struct mux_control *mux, int state)
{
	struct ti_k3_event_mux *priv = mux_chip_priv(mux->chip);
	unsigned int reg = TI_K3_EVENT_MUX_CNTL(mux_control_get_index(mux));
	u32 val;

	if (state == MUX_IDLE_DISCONNECT)
		val = 0;
	else
		val = state | TI_K3_EVENT_MUX_INT_ENABLE;

	return ti_k3_event_mux_write(priv->regmap, reg, val);
}

static const struct mux_control_ops ti_k3_event_mux_ops = {
	.set = ti_k3_event_mux_set,
};

static int ti_k3_event_mux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ti_k3_event_mux_data *data;
	struct regmap_config regmap_cfg = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
	};
	struct ti_k3_event_mux *priv;
	struct mux_chip *mux_chip;
	void __iomem *base;
	unsigned int i;

	data = device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap_cfg.max_register = TI_K3_EVENT_MUX_CNTL(data->num_outputs - 1);

	mux_chip = devm_mux_chip_alloc(dev, data->num_outputs, sizeof(*priv));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	priv = mux_chip_priv(mux_chip);

	priv->regmap = devm_regmap_init_mmio(dev, base, &regmap_cfg);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to init regmap\n");

	priv->saved = devm_kcalloc(dev, data->num_outputs,
				   sizeof(*priv->saved), GFP_KERNEL);
	if (!priv->saved)
		return -ENOMEM;

	for (i = 0; i < data->num_outputs; i++) {
		struct mux_control *mux = &mux_chip->mux[i];

		mux->states = data->num_inputs;
		mux->idle_state = MUX_IDLE_DISCONNECT;
	}

	mux_chip->ops = &ti_k3_event_mux_ops;

	platform_set_drvdata(pdev, mux_chip);

	return devm_mux_chip_register(dev, mux_chip);
}

static int ti_k3_event_mux_suspend_noirq(struct device *dev)
{
	struct mux_chip *mux_chip = dev_get_drvdata(dev);
	struct ti_k3_event_mux *priv = mux_chip_priv(mux_chip);
	unsigned int i;
	int ret;

	for (i = 0; i < mux_chip->controllers; i++) {
		ret = regmap_read(priv->regmap, TI_K3_EVENT_MUX_CNTL(i),
				  &priv->saved[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int ti_k3_event_mux_resume_noirq(struct device *dev)
{
	struct mux_chip *mux_chip = dev_get_drvdata(dev);
	struct ti_k3_event_mux *priv = mux_chip_priv(mux_chip);
	unsigned int i;
	int ret;

	for (i = 0; i < mux_chip->controllers; i++) {
		ret = ti_k3_event_mux_write(priv->regmap,
					    TI_K3_EVENT_MUX_CNTL(i),
					    priv->saved[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static DEFINE_NOIRQ_DEV_PM_OPS(ti_k3_event_mux_pm_ops,
			       ti_k3_event_mux_suspend_noirq,
			       ti_k3_event_mux_resume_noirq);

/* J721E TRM (SPRUIL1D), Table 11-12: TIMESYNC_INTRTR0 */
static const struct ti_k3_event_mux_data j721e_timesync_router_data = {
	.num_outputs = 48,
	.num_inputs = 56,
};

static const struct of_device_id ti_k3_event_mux_of_match[] = {
	{
		.compatible = "ti,j721e-timesync-router",
		.data = &j721e_timesync_router_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ti_k3_event_mux_of_match);

static struct platform_driver ti_k3_event_mux_driver = {
	.driver = {
		.name = "ti-k3-event-mux",
		.of_match_table = ti_k3_event_mux_of_match,
		.pm = pm_sleep_ptr(&ti_k3_event_mux_pm_ops),
	},
	.probe = ti_k3_event_mux_probe,
};
module_platform_driver(ti_k3_event_mux_driver);

MODULE_DESCRIPTION("TI K3 event mux router driver");
MODULE_AUTHOR("Rahul Sharma <r-sharma3@ti.com>");
MODULE_AUTHOR("James Clark <jjc@jclark.com>");
MODULE_LICENSE("GPL");
