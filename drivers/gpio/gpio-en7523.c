// SPDX-License-Identifier: GPL-2.0-only

#include <linux/types.h>
#include <linux/io.h>
#include <linux/bits.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/generic.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define AIROHA_GPIO_MAX		32

/**
 * struct airoha_gpio_ctrl - Airoha GPIO driver data
 * @gen_gc: Associated gpio_generic_chip instance.
 * @data: The data register.
 * @dir: [0] The direction register for the lower 16 pins.
 * [1]: The direction register for the higher 16 pins.
 * @output: The output enable register.
 */
struct airoha_gpio_ctrl {
	struct gpio_generic_chip gen_gc;
	void __iomem *data;
	void __iomem *dir[2];
	void __iomem *output;
};

static int airoha_dir_set(struct gpio_chip *gc, unsigned int gpio,
			  int val, int out)
{
	struct airoha_gpio_ctrl *ctrl = gpiochip_get_data(gc);
	u32 dir = ioread32(ctrl->dir[gpio / 16]);
	u32 output = ioread32(ctrl->output);
	u32 mask = BIT((gpio % 16) * 2);

	if (out) {
		dir |= mask;
		output |= BIT(gpio);
	} else {
		dir &= ~mask;
		output &= ~BIT(gpio);
	}

	iowrite32(dir, ctrl->dir[gpio / 16]);

	if (out)
		gpio_generic_chip_set(&ctrl->gen_gc, gpio, val);

	iowrite32(output, ctrl->output);

	return 0;
}

static int airoha_dir_out(struct gpio_chip *gc, unsigned int gpio,
			  int val)
{
	return airoha_dir_set(gc, gpio, val, 1);
}

static int airoha_dir_in(struct gpio_chip *gc, unsigned int gpio)
{
	return airoha_dir_set(gc, gpio, 0, 0);
}

static int airoha_get_dir(struct gpio_chip *gc, unsigned int gpio)
{
	struct airoha_gpio_ctrl *ctrl = gpiochip_get_data(gc);
	u32 dir = ioread32(ctrl->dir[gpio / 16]);
	u32 mask = BIT((gpio % 16) * 2);

	return (dir & mask) ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int airoha_gpio_probe(struct platform_device *pdev)
{
	struct gpio_generic_chip_config config = { };
	struct device *dev = &pdev->dev;
	struct airoha_gpio_ctrl *ctrl;
	int err;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->data = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctrl->data))
		return PTR_ERR(ctrl->data);

	ctrl->dir[0] = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ctrl->dir[0]))
		return PTR_ERR(ctrl->dir[0]);

	ctrl->dir[1] = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(ctrl->dir[1]))
		return PTR_ERR(ctrl->dir[1]);

	ctrl->output = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(ctrl->output))
		return PTR_ERR(ctrl->output);

	config.dev = dev;
	config.sz = 4;
	config.dat = ctrl->data;

	err = gpio_generic_chip_init(&ctrl->gen_gc, &config);
	if (err)
		return dev_err_probe(dev, err, "unable to init generic GPIO");

	ctrl->gen_gc.gc.ngpio = AIROHA_GPIO_MAX;
	ctrl->gen_gc.gc.owner = THIS_MODULE;
	ctrl->gen_gc.gc.direction_output = airoha_dir_out;
	ctrl->gen_gc.gc.direction_input = airoha_dir_in;
	ctrl->gen_gc.gc.get_direction = airoha_get_dir;

	return devm_gpiochip_add_data(dev, &ctrl->gen_gc.gc, ctrl);
}

static const struct of_device_id airoha_gpio_of_match[] = {
	{ .compatible = "airoha,en7523-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_gpio_of_match);

static struct platform_driver airoha_gpio_driver = {
	.driver = {
		.name = "airoha-gpio",
		.of_match_table	= airoha_gpio_of_match,
	},
	.probe = airoha_gpio_probe,
};
module_platform_driver(airoha_gpio_driver);

MODULE_DESCRIPTION("Airoha GPIO support");
MODULE_AUTHOR("John Crispin <john@phrozen.org>");
MODULE_LICENSE("GPL v2");
