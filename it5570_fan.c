// SPDX-License-Identifier: GPL-2.0
/*
 * ITE IT5570 EC Fan Control Driver
 *
 * Provides hwmon interface for fan control on systems with an ITE IT5570
 * embedded controller. Controls the fan via the ACPI EC register interface.
 *
 * EC Register Map (ACPI EC offsets):
 *   0x0E - Fan duty status (read-only, 0-100%)
 *   0x0F - Fan duty control (write 1-100 for manual %, 0 for auto)
 *   0x22 - Fan RPM high byte
 *   0x23 - Fan RPM low byte
 *   0x26 - CPU temperature (°C, filtered)
 *   0xF1 - Board temperature (°C)
 *
 * EC SRAM addresses (via SIO indirect access at 0x4E/0x4F):
 *   0x05B9 - CPU die temperature (°C, raw/unfiltered, faster response)
 *   0x0C44 - Heatsink temperature (°C)
 *   0x0C4A - Chipset temperature (°C)
 *   0x086A - EC internal temperature (°C)
 *
 * The EC is accessed at Super I/O port 0x4E, chip ID 0x5570.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>

#define DRIVER_NAME	"it5570_fan"

/* Super I/O ports for chip detection */
#define SIO_PORT	0x4E
#define SIO_DATA	0x4F

/* ITE IT5570 chip ID */
#define IT5570_DEVID	0x5570

/* ACPI EC ports */
#define EC_SC		0x66	/* EC status/command */
#define EC_DATA		0x62	/* EC data */

/* EC status bits */
#define EC_SC_IBF	BIT(1)	/* Input buffer full */
#define EC_SC_OBF	BIT(0)	/* Output buffer full */

/* EC commands */
#define EC_CMD_READ	0x80
#define EC_CMD_WRITE	0x81

/* EC register offsets (ACPI EC address space) */
#define EC_REG_FAN_DUTY_STATUS	0x0E	/* Current duty, read-only */
#define EC_REG_FAN_DUTY_CTRL	0x0F	/* Duty control: 0=auto, 1-100=manual */
#define EC_REG_FAN_RPM_HI	0x22
#define EC_REG_FAN_RPM_LO	0x23
#define EC_REG_CPU_TEMP		0x26
#define EC_REG_BOARD_TEMP	0xF1

/* EC SRAM addresses (accessed via SIO indirect interface) */
#define SRAM_CPU_DIE_TEMP	0x05B9	/* Raw/unfiltered CPU die temp */
#define SRAM_HEATSINK_TEMP	0x0C44	/* Heatsink temperature */
#define SRAM_CHIPSET_TEMP	0x0C4A	/* Chipset temperature */
#define SRAM_EC_TEMP		0x086A	/* EC internal temperature */

/* PWM conversion: hwmon uses 0-255, EC uses 0-100 */
#define EC_DUTY_MAX	100
#define HWMON_PWM_MAX	255

struct it5570_data {
	struct mutex lock;
	unsigned long last_updated;
	bool valid;

	/* Cached sensor values */
	unsigned int fan_rpm;
	unsigned int fan_duty;	/* 0-100 from EC */
	unsigned int cpu_temp;
	unsigned int board_temp;
	unsigned int cpu_die_temp;	/* Raw/unfiltered, from SRAM */
	unsigned int heatsink_temp;	/* From SRAM */
	unsigned int chipset_temp;	/* From SRAM */
	unsigned int ec_temp;		/* From SRAM */
	unsigned int fan_ctrl;	/* current control register value */
};

static struct platform_device *it5570_pdev;
static DEFINE_MUTEX(ec_io_mutex);

/*
 * Low-level ACPI EC access
 *
 * We use direct port I/O rather than the kernel's ACPI EC interface
 * to avoid potential conflicts with the ACPI EC driver's transaction
 * handling. The kernel ec_read/ec_write functions may not be exported
 * on all configurations.
 */
static int ec_wait_ibf_clear(void)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!(inb(EC_SC) & EC_SC_IBF))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int ec_wait_obf_set(void)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (inb(EC_SC) & EC_SC_OBF)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int ec_read_byte(u8 offset, u8 *val)
{
	int ret;

	mutex_lock(&ec_io_mutex);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(EC_CMD_READ, EC_SC);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(offset, EC_DATA);

	ret = ec_wait_obf_set();
	if (ret)
		goto out;
	*val = inb(EC_DATA);

out:
	mutex_unlock(&ec_io_mutex);
	return ret;
}

static int ec_write_byte(u8 offset, u8 val)
{
	int ret;

	mutex_lock(&ec_io_mutex);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(EC_CMD_WRITE, EC_SC);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(offset, EC_DATA);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(val, EC_DATA);

out:
	mutex_unlock(&ec_io_mutex);
	return ret;
}

/*
 * SIO indirect SRAM access
 *
 * The IT5570 SMFI (Shared Memory Flash Interface) provides indirect
 * access to the EC's full SRAM space via SIO config registers 0x2E/0x2F.
 * Sub-register 0x11 sets the address high byte, 0x10 the low byte,
 * and 0x12 transfers the data byte.
 */
static int sio_sram_read(u16 addr, u8 *val)
{
	mutex_lock(&ec_io_mutex);

	/* Address high byte */
	outb(0x2E, SIO_PORT);
	outb(0x11, SIO_DATA);
	outb(0x2F, SIO_PORT);
	outb((addr >> 8) & 0xFF, SIO_DATA);

	/* Address low byte */
	outb(0x2E, SIO_PORT);
	outb(0x10, SIO_DATA);
	outb(0x2F, SIO_PORT);
	outb(addr & 0xFF, SIO_DATA);

	/* Read data */
	outb(0x2E, SIO_PORT);
	outb(0x12, SIO_DATA);
	outb(0x2F, SIO_PORT);
	*val = inb(SIO_DATA);

	mutex_unlock(&ec_io_mutex);
	return 0;
}

/*
 * Super I/O chip detection
 */
static void sio_enter(void)
{
	outb(0x87, SIO_PORT);
	outb(0x01, SIO_PORT);
	outb(0x55, SIO_PORT);
	outb(0xaa, SIO_PORT);
}

static void sio_exit(void)
{
	outb(0x02, SIO_PORT);
	outb(0x02, SIO_DATA);
}

static u8 sio_read(u8 reg)
{
	outb(reg, SIO_PORT);
	return inb(SIO_DATA);
}

static int it5570_detect(void)
{
	u16 devid;

	if (!request_region(SIO_PORT, 2, DRIVER_NAME))
		return -EBUSY;

	sio_enter();
	devid = (sio_read(0x20) << 8) | sio_read(0x21);
	sio_exit();

	release_region(SIO_PORT, 2);

	if (devid != IT5570_DEVID) {
		pr_info(DRIVER_NAME ": chip ID 0x%04x not supported\n", devid);
		return -ENODEV;
	}

	pr_info(DRIVER_NAME ": found ITE IT5570 (ID 0x%04x)\n", devid);
	return 0;
}

/*
 * Update cached sensor data (rate-limited to 1 Hz)
 */
static int it5570_update(struct it5570_data *data)
{
	u8 hi, lo, val;
	int ret;

	mutex_lock(&data->lock);

	if (data->valid && time_before(jiffies, data->last_updated + HZ))
		goto out;

	ret = ec_read_byte(EC_REG_FAN_RPM_HI, &hi);
	if (ret)
		goto err;
	ret = ec_read_byte(EC_REG_FAN_RPM_LO, &lo);
	if (ret)
		goto err;
	data->fan_rpm = (hi << 8) | lo;

	ret = ec_read_byte(EC_REG_FAN_DUTY_STATUS, &val);
	if (ret)
		goto err;
	data->fan_duty = val;

	ret = ec_read_byte(EC_REG_FAN_DUTY_CTRL, &val);
	if (ret)
		goto err;
	data->fan_ctrl = val;

	ret = ec_read_byte(EC_REG_CPU_TEMP, &val);
	if (ret)
		goto err;
	data->cpu_temp = val;

	ret = ec_read_byte(EC_REG_BOARD_TEMP, &val);
	if (ret)
		goto err;
	data->board_temp = val;

	ret = sio_sram_read(SRAM_CPU_DIE_TEMP, &val);
	if (ret)
		goto err;
	data->cpu_die_temp = val;

	ret = sio_sram_read(SRAM_HEATSINK_TEMP, &val);
	if (ret)
		goto err;
	data->heatsink_temp = val;

	ret = sio_sram_read(SRAM_CHIPSET_TEMP, &val);
	if (ret)
		goto err;
	data->chipset_temp = val;

	ret = sio_sram_read(SRAM_EC_TEMP, &val);
	if (ret)
		goto err;
	data->ec_temp = val;

	data->last_updated = jiffies;
	data->valid = true;

out:
	mutex_unlock(&data->lock);
	return 0;

err:
	mutex_unlock(&data->lock);
	return ret;
}

/*
 * hwmon interface
 */
static umode_t it5570_is_visible(const void *drvdata,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		return 0444;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
		default:
			return 0;
		}
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static int it5570_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	int ret;

	ret = it5570_update(data);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_fan:
		*val = data->fan_rpm;
		return 0;

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			/* Convert EC duty 0-100 to hwmon 0-255 */
			*val = DIV_ROUND_CLOSEST(data->fan_duty * HWMON_PWM_MAX,
						 EC_DUTY_MAX);
			return 0;
		case hwmon_pwm_enable:
			/* 0 = auto (EC control), 1 = manual */
			*val = data->fan_ctrl ? 1 : 2;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_temp:
		/* hwmon temperatures are in millidegrees C */
		switch (channel) {
		case 0:
			*val = data->cpu_temp * 1000;
			return 0;
		case 1:
			*val = data->board_temp * 1000;
			return 0;
		case 2:
			*val = data->cpu_die_temp * 1000;
			return 0;
		case 3:
			*val = data->heatsink_temp * 1000;
			return 0;
		case 4:
			*val = data->chipset_temp * 1000;
			return 0;
		case 5:
			*val = data->ec_temp * 1000;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	default:
		return -EOPNOTSUPP;
	}
}

static int it5570_read_string(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, const char **str)
{
	static const char * const temp_labels[] = {
		"CPU",
		"Board",
		"CPU Die",
		"Heatsink",
		"Chipset",
		"EC",
	};

	if (type == hwmon_temp && attr == hwmon_temp_label &&
	    channel < ARRAY_SIZE(temp_labels)) {
		*str = temp_labels[channel];
		return 0;
	}
	return -EOPNOTSUPP;
}

static int it5570_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	int ret;

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			/* Convert hwmon 0-255 to EC duty 0-100 */
			val = clamp_val(val, 0, HWMON_PWM_MAX);
			val = DIV_ROUND_CLOSEST(val * EC_DUTY_MAX,
						HWMON_PWM_MAX);
			if (val == 0)
				val = 1; /* Minimum 1% when manually set */
			ret = ec_write_byte(EC_REG_FAN_DUTY_CTRL, val);
			if (!ret) {
				mutex_lock(&data->lock);
				data->fan_ctrl = val;
				data->fan_duty = val;
				data->valid = false;
				mutex_unlock(&data->lock);
			}
			return ret;

		case hwmon_pwm_enable:
			if (val == 2 || val == 0) {
				/* Auto mode: write 0 to control register */
				ret = ec_write_byte(EC_REG_FAN_DUTY_CTRL, 0);
				if (!ret) {
					mutex_lock(&data->lock);
					data->fan_ctrl = 0;
					data->valid = false;
					mutex_unlock(&data->lock);
				}
				return ret;
			} else if (val == 1) {
				/*
				 * Manual mode: read current duty and write it
				 * to lock the current speed
				 */
				mutex_lock(&data->lock);
				val = data->fan_duty;
				if (val == 0)
					val = 50; /* Default to 50% */
				mutex_unlock(&data->lock);
				ret = ec_write_byte(EC_REG_FAN_DUTY_CTRL, val);
				if (!ret) {
					mutex_lock(&data->lock);
					data->fan_ctrl = val;
					data->valid = false;
					mutex_unlock(&data->lock);
				}
				return ret;
			}
			return -EINVAL;

		default:
			return -EOPNOTSUPP;
		}

	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info * const it5570_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops it5570_ops = {
	.is_visible = it5570_is_visible,
	.read = it5570_read,
	.read_string = it5570_read_string,
	.write = it5570_write,
};

static const struct hwmon_chip_info it5570_chip_info = {
	.ops = &it5570_ops,
	.info = it5570_info,
};

/*
 * Platform driver
 */
static int it5570_probe(struct platform_device *pdev)
{
	struct it5570_data *data;
	struct device *hwmon_dev;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);

	hwmon_dev = devm_hwmon_device_register_with_info(
		&pdev->dev, DRIVER_NAME, data,
		&it5570_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	platform_set_drvdata(pdev, data);

	/* Initial read */
	it5570_update(data);

	dev_info(&pdev->dev,
		 "CPU: %u°C, Die: %u°C, Board: %u°C, Heatsink: %u°C, Chipset: %u°C, EC: %u°C, Fan: %u RPM (%u%% duty)\n",
		 data->cpu_temp, data->cpu_die_temp, data->board_temp,
		 data->heatsink_temp, data->chipset_temp, data->ec_temp,
		 data->fan_rpm, data->fan_duty);

	return 0;
}

static void it5570_remove(struct platform_device *pdev)
{
	/* Restore auto fan control on unload */
	ec_write_byte(EC_REG_FAN_DUTY_CTRL, 0);
	dev_info(&pdev->dev, "fan control restored to auto mode\n");
}

static struct platform_driver it5570_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe = it5570_probe,
	.remove = it5570_remove,
};

static int __init it5570_init(void)
{
	int ret;

	ret = it5570_detect();
	if (ret)
		return ret;

	ret = platform_driver_register(&it5570_driver);
	if (ret)
		return ret;

	it5570_pdev = platform_device_register_simple(DRIVER_NAME, -1,
						       NULL, 0);
	if (IS_ERR(it5570_pdev)) {
		ret = PTR_ERR(it5570_pdev);
		platform_driver_unregister(&it5570_driver);
		return ret;
	}

	return 0;
}

static void __exit it5570_exit(void)
{
	platform_device_unregister(it5570_pdev);
	platform_driver_unregister(&it5570_driver);
}

module_init(it5570_init);
module_exit(it5570_exit);

MODULE_AUTHOR("Michael");
MODULE_DESCRIPTION("ITE IT5570 EC Fan Control Driver");
MODULE_LICENSE("GPL");
