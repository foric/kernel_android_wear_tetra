/*
* Simple driver for Texas Instruments LM3630A Backlight driver chip
* Copyright (C) 2012 Texas Instruments
* Copyright (C) 2014 Sony Mobile Communications AB
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
*/
#include <linux/module.h>
#include <linux/slab.h>
#ifdef CONFIG_THERMAL
#include <linux/thermal.h>
#endif
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/pwm.h>
#include <linux/platform_data/lm3630a_bl.h>

#define REG_CTRL		0x00
#define REG_CONFIG		0x01
#define REG_BOOST		0x02
#define REG_BRT_A		0x03
#define REG_BRT_B		0x04
#define REG_I_A			0x05
#define REG_I_B			0x06
#define REG_ON_OFF_RAMP		0x07
#define REG_RUN_RAMP		0x08
#define REG_INT_STATUS		0x09
#define REG_INT_EN		0x0A
#define REG_FAULT		0x0B
#define REG_SW_RESET		0x0F
#define REG_PWM_OUTLOW		0x12
#define REG_PWM_OUTHIGH		0x13
#define REG_REVISION		0x1F
#define REG_FILTER_STRENGTH	0x50
#define REG_MAX			REG_FILTER_STRENGTH

#define HYSTERESIS_MASK_REG_I_A	0x80
#define INT_DEBOUNCE_MSEC	10
struct lm3630a_chip {
	struct device *dev;
	struct delayed_work work;

	int irq;
	struct workqueue_struct *irqthread;
	struct lm3630a_platform_data *pdata;
	struct backlight_device *bleda;
	struct backlight_device *bledb;
	struct regmap *regmap;
	struct pwm_device *pwmd;
};

/* i2c access */
static int lm3630a_read(struct lm3630a_chip *pchip, unsigned int reg)
{
	int rval;
	unsigned int reg_val;

	rval = regmap_read(pchip->regmap, reg, &reg_val);
	if (rval < 0)
		return rval;
	return reg_val & 0xFF;
}

static int lm3630a_write(struct lm3630a_chip *pchip,
			 unsigned int reg, unsigned int data)
{
	return regmap_write(pchip->regmap, reg, data);
}

static int lm3630a_update(struct lm3630a_chip *pchip,
			  unsigned int reg, unsigned int mask,
			  unsigned int data)
{
	return regmap_update_bits(pchip->regmap, reg, mask, data);
}

/* initialize chip */
static int lm3630a_chip_init(struct lm3630a_chip *pchip)
{
	int rval;
	struct lm3630a_platform_data *pdata = pchip->pdata;

	if (gpio_request_one(pdata->enable_gpio , GPIOF_OUT_INIT_HIGH, "bl_enable") < 0) {
		pr_err("can't get bl_enable GPIO\n");
		return -EINVAL;
	}
	gpio_set_value(pdata->enable_gpio, 1);

	usleep_range(1000, 2000);
	/* set Filter Strength Register */
	rval = lm3630a_write(pchip, REG_FILTER_STRENGTH,
						pdata->pwm_filter_strength);
	/* set Cofig. register */
	rval |= lm3630a_update(pchip, REG_CONFIG, 0x07, pdata->pwm_ctrl);
	/* set boost control */
	rval |= lm3630a_write(pchip, REG_BOOST, 0x38);
	/* set current A */
	rval |= lm3630a_update(pchip, REG_I_A, 0x1F, pdata->a_max_curr);
	/* set current B */
	rval |= lm3630a_write(pchip, REG_I_B, pdata->b_max_curr);
	/* set ramp */
	rval |= lm3630a_write(pchip, REG_ON_OFF_RAMP, pdata->ramp_on_off);
	rval |= lm3630a_write(pchip, REG_RUN_RAMP, pdata->ramp_run);
	/* set control */
	rval |= lm3630a_update(pchip, REG_CTRL, 0x14, pdata->leda_ctrl);
	rval |= lm3630a_update(pchip, REG_CTRL, 0x0B, pdata->ledb_ctrl);
	usleep_range(1000, 2000);
	/* set brightness A and B */
	rval |= lm3630a_write(pchip, REG_BRT_A, pdata->leda_init_brt);
	rval |= lm3630a_write(pchip, REG_BRT_B, pdata->ledb_init_brt);

	/* set feedback */
	if (pdata->leda_ctrl == LM3630A_LEDA_DISABLE)
		rval |= lm3630a_update(pchip, REG_CONFIG, 0x08, 0);
	else
		rval |= lm3630a_update(pchip, REG_CONFIG, 0x08, 0x08);
	if (pdata->ledb_ctrl == LM3630A_LEDB_DISABLE)
		rval |= lm3630a_update(pchip, REG_CONFIG, 0x10, 0);
	else
		rval |= lm3630a_update(pchip, REG_CONFIG, 0x10, 0x10);

	if (rval < 0)
		dev_err(pchip->dev, "i2c failed to access register\n");
	return rval;
}

/* interrupt handling */
static void lm3630a_delayed_func(struct work_struct *work)
{
	int rval;
	struct lm3630a_chip *pchip;

	pchip = container_of(work, struct lm3630a_chip, work.work);

	rval = lm3630a_read(pchip, REG_INT_STATUS);
	if (rval < 0) {
		dev_err(pchip->dev,
			"i2c failed to access REG_INT_STATUS Register\n");
		return;
	}

	dev_info(pchip->dev, "REG_INT_STATUS Register is 0x%x\n", rval);
}

static irqreturn_t lm3630a_isr_func(int irq, void *chip)
{
	int rval;
	struct lm3630a_chip *pchip = chip;
	unsigned long delay = msecs_to_jiffies(INT_DEBOUNCE_MSEC);

	queue_delayed_work(pchip->irqthread, &pchip->work, delay);

	rval = lm3630a_update(pchip, REG_CTRL, 0x80, 0x00);
	if (rval < 0) {
		dev_err(pchip->dev, "i2c failed to access register\n");
		return IRQ_NONE;
	}
	return IRQ_HANDLED;
}

static int lm3630a_intr_config(struct lm3630a_chip *pchip)
{
	int rval;

	rval = lm3630a_write(pchip, REG_INT_EN, 0x87);
	if (rval < 0)
		return rval;

	INIT_DELAYED_WORK(&pchip->work, lm3630a_delayed_func);
	pchip->irqthread = create_singlethread_workqueue("lm3630a-irqthd");
	if (!pchip->irqthread) {
		dev_err(pchip->dev, "create irq thread fail\n");
		return -ENOMEM;
	}
	if (request_threaded_irq
	    (pchip->irq, NULL, lm3630a_isr_func,
	     IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "lm3630a_irq", pchip)) {
		dev_err(pchip->dev, "request threaded irq fail\n");
		destroy_workqueue(pchip->irqthread);
		return -ENOMEM;
	}
	return rval;
}

/* update and get brightness */
static int lm3630a_bank_a_update_status(struct backlight_device *bl)
{
	int ret;
	struct lm3630a_chip *pchip = bl_get_data(bl);

	/* disable sleep */
	ret = lm3630a_update(pchip, REG_CTRL, 0x80, 0x00);
	if (ret < 0)
		goto out_i2c_err;
	usleep_range(1000, 2000);

	/* minimum brightness is 0x04 */
	ret = lm3630a_write(pchip, REG_BRT_A, bl->props.brightness);
	if (bl->props.brightness < 0x4) {
		ret |= lm3630a_update(pchip, REG_CTRL, LM3630A_LEDA_ENABLE, 0);
		/* enable sleep */
		ret |= lm3630a_update(pchip, REG_CTRL, 0x80, 0x80);
	} else {
		if (bl->props.brightness >= LM3630A_MAX_BRIGHTNESS)
			ret |= lm3630a_update(pchip, REG_I_A,
						HYSTERESIS_MASK_REG_I_A,
						HYSTERESIS_MASK_REG_I_A);
		else
			ret |= lm3630a_update(pchip, REG_I_A,
						HYSTERESIS_MASK_REG_I_A, 0);
		ret |= lm3630a_update(pchip, REG_CTRL,
				      LM3630A_LEDA_ENABLE, LM3630A_LEDA_ENABLE);
	}
	if (ret < 0)
		goto out_i2c_err;
	return bl->props.brightness;

out_i2c_err:
	dev_err(pchip->dev, "i2c failed to access\n");
	return bl->props.brightness;
}

static int lm3630a_bank_a_get_brightness(struct backlight_device *bl)
{
	int brightness, rval;
	struct lm3630a_chip *pchip = bl_get_data(bl);
	enum lm3630a_pwm_ctrl pwm_ctrl = pchip->pdata->pwm_ctrl;

	if ((pwm_ctrl & LM3630A_PWM_BANK_A) != 0) {
		rval = lm3630a_read(pchip, REG_PWM_OUTHIGH);
		if (rval < 0)
			goto out_i2c_err;
		brightness = (rval & 0x01) << 8;
		rval = lm3630a_read(pchip, REG_PWM_OUTLOW);
		if (rval < 0)
			goto out_i2c_err;
		brightness |= rval;
		goto out;
	}

	/* disable sleep */
	rval = lm3630a_update(pchip, REG_CTRL, 0x80, 0x00);
	if (rval < 0)
		goto out_i2c_err;
	usleep_range(1000, 2000);
	rval = lm3630a_read(pchip, REG_BRT_A);
	if (rval < 0)
		goto out_i2c_err;
	brightness = rval;

out:
	bl->props.brightness = brightness;
	return bl->props.brightness;
out_i2c_err:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return 0;
}

static const struct backlight_ops lm3630a_bank_a_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = lm3630a_bank_a_update_status,
	.get_brightness = lm3630a_bank_a_get_brightness,
};

/* update and get brightness */
static int lm3630a_bank_b_update_status(struct backlight_device *bl)
{
	int ret;
	struct lm3630a_chip *pchip = bl_get_data(bl);

	/* disable sleep */
	ret = lm3630a_update(pchip, REG_CTRL, 0x80, 0x00);
	if (ret < 0)
		goto out_i2c_err;
	usleep_range(1000, 2000);

	/* minimum brightness is 0x04 */
	ret = lm3630a_write(pchip, REG_BRT_B, bl->props.brightness);
	if (bl->props.brightness < 0x4) {
		ret |= lm3630a_update(pchip, REG_CTRL, LM3630A_LEDB_ENABLE, 0);
		/* enable sleep */
		ret |= lm3630a_update(pchip, REG_CTRL, 0x80, 0x80);
	} else {
		if (bl->props.brightness >= LM3630A_MAX_BRIGHTNESS)
			ret |= lm3630a_update(pchip, REG_I_A,
						HYSTERESIS_MASK_REG_I_A,
						HYSTERESIS_MASK_REG_I_A);
		else
			ret |= lm3630a_update(pchip, REG_I_A,
						HYSTERESIS_MASK_REG_I_A, 0);
		ret |= lm3630a_update(pchip, REG_CTRL,
				      LM3630A_LEDB_ENABLE, LM3630A_LEDB_ENABLE);
	}
	if (ret < 0)
		goto out_i2c_err;
	return bl->props.brightness;

out_i2c_err:
	dev_err(pchip->dev, "i2c failed to access REG_CTRL\n");
	return bl->props.brightness;
}

static int lm3630a_bank_b_get_brightness(struct backlight_device *bl)
{
	int brightness, rval;
	struct lm3630a_chip *pchip = bl_get_data(bl);
	enum lm3630a_pwm_ctrl pwm_ctrl = pchip->pdata->pwm_ctrl;

	if ((pwm_ctrl & LM3630A_PWM_BANK_B) != 0) {
		rval = lm3630a_read(pchip, REG_PWM_OUTHIGH);
		if (rval < 0)
			goto out_i2c_err;
		brightness = (rval & 0x01) << 8;
		rval = lm3630a_read(pchip, REG_PWM_OUTLOW);
		if (rval < 0)
			goto out_i2c_err;
		brightness |= rval;
		goto out;
	}

	/* disable sleep */
	rval = lm3630a_update(pchip, REG_CTRL, 0x80, 0x00);
	if (rval < 0)
		goto out_i2c_err;
	usleep_range(1000, 2000);
	rval = lm3630a_read(pchip, REG_BRT_B);
	if (rval < 0)
		goto out_i2c_err;
	brightness = rval;

out:
	bl->props.brightness = brightness;
	return bl->props.brightness;
out_i2c_err:
	dev_err(pchip->dev, "i2c failed to access register\n");
	return 0;
}

#ifdef CONFIG_THERMAL
u32 backlight_cooling_get_level(struct thermal_cooling_device *cdev,
				u32 brightness)
{
	return THERMAL_CSTATE_INVALID;
}
EXPORT_SYMBOL(backlight_cooling_get_level);
#endif

static const struct backlight_ops lm3630a_bank_b_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = lm3630a_bank_b_update_status,
	.get_brightness = lm3630a_bank_b_get_brightness,
};

static int lm3630a_backlight_register(struct lm3630a_chip *pchip)
{
	struct backlight_properties props;
	struct lm3630a_platform_data *pdata = pchip->pdata;

	props.type = BACKLIGHT_RAW;
	if (pdata->leda_ctrl != LM3630A_LEDA_DISABLE) {
		props.brightness = pdata->leda_init_brt;
		props.max_brightness = pdata->leda_max_brt;
		pchip->bleda =
		    backlight_device_register("lm3630a_leda", pchip->dev, pchip,
						   &lm3630a_bank_a_ops, &props);
		if (IS_ERR(pchip->bleda))
			return PTR_ERR(pchip->bleda);
	}

	if ((pdata->ledb_ctrl != LM3630A_LEDB_DISABLE) &&
	    (pdata->ledb_ctrl != LM3630A_LEDB_ON_A)) {
		props.brightness = pdata->ledb_init_brt;
		props.max_brightness = pdata->ledb_max_brt;
		pchip->bledb =
		    backlight_device_register("lm3630a_ledb", pchip->dev, pchip,
						   &lm3630a_bank_b_ops, &props);
		if (IS_ERR(pchip->bledb))
			return PTR_ERR(pchip->bledb);
	}
	return 0;
}

static void lm3630a_backlight_unregister(struct lm3630a_chip *pchip)
{
	if (pchip->bleda)
		backlight_device_unregister(pchip->bleda);
	if (pchip->bledb)
		backlight_device_unregister(pchip->bledb);
}

static const struct regmap_config lm3630a_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = REG_MAX,
};

static ssize_t lm3630a_ramp_onoff_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int rval;
	struct i2c_client *client;
	struct lm3630a_chip *pchip;

	client = container_of(dev, struct i2c_client, dev);
	pchip = i2c_get_clientdata(client);

	if (!pchip)
		return -ENODEV;
	rval = lm3630a_read(pchip, REG_ON_OFF_RAMP);
	if (rval < 0)
		return rval;
	return scnprintf(buf, PAGE_SIZE, "%d\n", rval);
}

static ssize_t lm3630a_ramp_onoff_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct i2c_client *client;
	struct lm3630a_chip *pchip;
	uint8_t val;

	client = container_of(dev, struct i2c_client, dev);
	pchip = i2c_get_clientdata(client);

	if (!pchip)
		return -ENODEV;

	if (sscanf(buf, "%hhu", &val) != 1) {
		dev_err(pchip->dev, "Error, buf = %s\n", buf);
		return -EINVAL;
	}

	ret = lm3630a_write(pchip, REG_ON_OFF_RAMP, val);
	if (ret < 0) {
		dev_err(pchip->dev, "i2c error (%d)\n", ret);
		return ret;
	}
	return count;
}

static struct device_attribute lm3630a_attributes[] = {
	__ATTR(ramp_onoff, S_IRUGO|S_IWUSR|S_IWGRP, lm3630a_ramp_onoff_show,
						lm3630a_ramp_onoff_store),
};

static int register_attributes(struct device *dev)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(lm3630a_attributes); i++)
		if (device_create_file(dev, lm3630a_attributes + i))
			goto error;
	return 0;
error:
	dev_err(dev, "%s: Unable to create interface\n", __func__);
	for (--i; i >= 0; i--)
		device_remove_file(dev, lm3630a_attributes + i);
	return -ENODEV;
}

static void remove_attributes(struct device *dev)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(lm3630a_attributes); i++)
		device_remove_file(dev, lm3630a_attributes + i);
}

static int lm3630a_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct lm3630a_platform_data *pdata = dev_get_platdata(&client->dev);
	struct lm3630a_chip *pchip;
	int rval;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "fail : i2c functionality check\n");
		return -EOPNOTSUPP;
	}

	pchip = devm_kzalloc(&client->dev, sizeof(struct lm3630a_chip),
			     GFP_KERNEL);
	if (!pchip)
		return -ENOMEM;
	pchip->dev = &client->dev;

	pchip->regmap = devm_regmap_init_i2c(client, &lm3630a_regmap);
	if (IS_ERR(pchip->regmap)) {
		rval = PTR_ERR(pchip->regmap);
		dev_err(&client->dev, "fail : allocate reg. map: %d\n", rval);
		return rval;
	}

	i2c_set_clientdata(client, pchip);
	if (pdata == NULL) {
		pdata = devm_kzalloc(pchip->dev,
				     sizeof(struct lm3630a_platform_data),
				     GFP_KERNEL);
		if (pdata == NULL)
			return -ENOMEM;
		/* default values */
		pdata->leda_ctrl = LM3630A_LEDA_ENABLE;
		pdata->ledb_ctrl = LM3630A_LEDB_ENABLE;
		pdata->leda_max_brt = LM3630A_MAX_BRIGHTNESS;
		pdata->ledb_max_brt = LM3630A_MAX_BRIGHTNESS;
		pdata->leda_init_brt = LM3630A_MAX_BRIGHTNESS;
		pdata->ledb_init_brt = LM3630A_MAX_BRIGHTNESS;
	}
	pchip->pdata = pdata;

	/* chip initialize */
	rval = lm3630a_chip_init(pchip);
	if (rval < 0) {
		dev_err(&client->dev, "fail : init chip\n");
		return rval;
	}
	/* backlight register */
	rval = lm3630a_backlight_register(pchip);
	if (rval < 0) {
		dev_err(&client->dev, "fail : backlight register.\n");
		return rval;
	}

	/* interrupt enable  : irq 0 is not allowed */
	pchip->irq = client->irq;
	if (pchip->irq) {
		rval = lm3630a_intr_config(pchip);
		if (rval < 0)
			lm3630a_backlight_unregister(pchip);
			return rval;
	}
	dev_info(&client->dev, "LM3630A backlight register OK.\n");

	/* Set initial backlight value */
	if (pdata->leda_ctrl != LM3630A_LEDA_DISABLE)
		lm3630a_bank_a_update_status(pchip->bleda);
	if (pdata->ledb_ctrl != LM3630A_LEDB_DISABLE)
		lm3630a_bank_b_update_status(pchip->bledb);

	rval = register_attributes(&client->dev);
	if (rval)
		dev_err(&client->dev, "%s: failed to register attributes\n",
								__func__);

	return 0;
}

static int lm3630a_remove(struct i2c_client *client)
{
	int rval;
	struct lm3630a_chip *pchip = i2c_get_clientdata(client);

	remove_attributes(pchip->dev);
	rval = lm3630a_write(pchip, REG_BRT_A, 0);
	if (rval < 0)
		dev_err(pchip->dev, "i2c failed to access register\n");

	rval = lm3630a_write(pchip, REG_BRT_B, 0);
	if (rval < 0)
		dev_err(pchip->dev, "i2c failed to access register\n");

	lm3630a_backlight_unregister(pchip);
	if (pchip->irq) {
		free_irq(pchip->irq, pchip);
		flush_workqueue(pchip->irqthread);
		destroy_workqueue(pchip->irqthread);
	}
	return 0;
}

static const struct i2c_device_id lm3630a_id[] = {
	{LM3630A_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, lm3630a_id);

static struct i2c_driver lm3630a_i2c_driver = {
	.driver = {
		   .name = LM3630A_NAME,
		   },
	.probe = lm3630a_probe,
	.remove = lm3630a_remove,
	.id_table = lm3630a_id,
};

module_i2c_driver(lm3630a_i2c_driver);

MODULE_DESCRIPTION("Texas Instruments Backlight driver for LM3630A");
MODULE_AUTHOR("Daniel Jeong <gshark.jeong@gmail.com>");
MODULE_AUTHOR("LDD MLP <ldd-mlp@list.ti.com>");
MODULE_LICENSE("GPL v2");
