// SPDX-License-Identifier: GPL-2.0
/* Author: Dan Scally <djrscally@gmail.com> */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/tps68470.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_data/tps68470.h>
#include "../../../../../include/linux/platform_data/cf20-tps68470.h"
#include <linux/platform_data/x86/int3472.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "tps68470.h"

#define DESIGNED_FOR_CHROMEOS		1
#define DESIGNED_FOR_WINDOWS		2

#define TPS68470_WIN_MFD_CELL_COUNT	3

static const struct mfd_cell tps68470_cros[] = {
	{ .name = "tps68470-gpio" },
	{ .name = "tps68470_pmic_opregion" },
};

static DEFINE_MUTEX(tps68470_cf20_regmap_lock);
static struct regmap *tps68470_cf20_regmap;

static bool tps68470_cf20_machine(void)
{
	return dmi_match(DMI_SYS_VENDOR, "Panasonic Corporation") &&
	       dmi_match(DMI_PRODUCT_NAME, "CF-20-2") &&
	       dmi_match(DMI_BOARD_NAME, "CF20-2");
}

int tps68470_cf20_set_sensor_daisy_chain(struct device *sensor, bool enable)
{
	struct regmap *regmap;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
	} else {
		if (sensor)
			dev_info(sensor, "CF-20 daisy: updating cached PMIC register 0x43\n");
		ret = regmap_update_bits(regmap, TPS68470_REG_S_I2C_CTL,
					BIT(0), enable ? BIT(0) : 0);
		if (sensor)
			dev_info(sensor, "CF-20 daisy: register update returned %d\n",
				 ret);
	}
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret && sensor)
		dev_err_probe(sensor, ret, "CF-20 TPS68470 daisy-chain %s failed\n",
			      enable ? "enable" : "disable");

	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_set_sensor_daisy_chain);

int tps68470_cf20_set_sensor_io_active(struct device *sensor, bool enable)
{
	struct regmap *regmap;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
	} else {
		/* Ghidra: VoltageWF::IoActive updates S_I2C_CTL bit 1. */
		if (sensor)
			dev_info(sensor,
				 "CF-20 IoActive: %s S_I2C_CTL bit 1\n",
				 enable ? "enable" : "disable");
		ret = regmap_update_bits(regmap, TPS68470_REG_S_I2C_CTL,
					BIT(1), enable ? BIT(1) : 0);
	}
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret && sensor)
		dev_err_probe(sensor, ret, "CF-20 TPS68470 IoActive %s failed\n",
			      enable ? "enable" : "disable");

	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_set_sensor_io_active);

int tps68470_cf20_set_vendor_gpio(struct device *sensor, bool enable)
{
	struct regmap *regmap;
	unsigned int old_sgpo;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}

	/*
	 * Exact iactrllogic64.sys CrdG2TiGpio::GpioOper(type 5):
	 * update SGPO bit 2, wait 1 ms, then update SGPO bit 0.
	 * This is deliberately separate from the Quanta and type-4 paths.
	 */
	ret = regmap_read(regmap, TPS68470_REG_SGPO, &old_sgpo);
	if (ret)
		goto out_unlock;

	if (sensor)
		dev_info(sensor,
			 "CF-20 vendor GPIO: %s SGPO bit 2, then bit 0\n",
			 enable ? "enable" : "disable");

	ret = regmap_update_bits(regmap, TPS68470_REG_SGPO, BIT(2),
				 enable ? BIT(2) : 0);
	if (ret)
		goto out_unlock;

	usleep_range(1000, 2000);
	ret = regmap_update_bits(regmap, TPS68470_REG_SGPO, BIT(0),
				 enable ? BIT(0) : 0);
	if (ret) {
		/* Best effort: never leave a half-applied GPIO transition. */
		regmap_write(regmap, TPS68470_REG_SGPO, old_sgpo);
	}

out_unlock:
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret && sensor)
		dev_err_probe(sensor, ret, "CF-20 vendor GPIO %s failed\n",
			      enable ? "enable" : "disable");

	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_set_vendor_gpio);

int tps68470_cf20_set_quanta_gpio(struct device *sensor, bool enable)
{
	struct regmap *regmap;
	unsigned int old_gpctl3a, gpdo, old_gpdo;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}

	/*
	 * Ghidra: iactrllogic64.sys selects CrdG2TiQuantaGpio for the CF-20
	 * TPS68470 path.  GpioOper(type 5) writes GPCTL3A=0xc2, then updates
	 * GPDO bit 3.  Keep the read/modify/write under the same lock as the
	 * other CF-20 PMIC helpers and restore both registers if the second
	 * write fails.
	 */
	ret = regmap_read(regmap, TPS68470_REG_GPCTL3A, &old_gpctl3a);
	if (ret)
		goto out_unlock;
	ret = regmap_read(regmap, TPS68470_REG_GPDO, &old_gpdo);
	if (ret)
		goto out_unlock;

	if (sensor)
		dev_info(sensor,
			 "CF-20 Quanta GPIO: %s GPCTL3A=0xc2, GPDO bit 3\n",
			 enable ? "enable" : "disable");

	ret = regmap_write(regmap, TPS68470_REG_GPCTL3A, 0xc2);
	if (ret)
		goto out_unlock;

	gpdo = old_gpdo;
	if (enable)
		gpdo |= BIT(3);
	else
		gpdo &= ~BIT(3);
	ret = regmap_write(regmap, TPS68470_REG_GPDO, gpdo);
	if (ret) {
		/* Best effort: do not leave a half-applied vendor GPIO update. */
		regmap_write(regmap, TPS68470_REG_GPDO, old_gpdo);
		regmap_write(regmap, TPS68470_REG_GPCTL3A, old_gpctl3a);
	}

out_unlock:
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret && sensor)
		dev_err_probe(sensor, ret, "CF-20 Quanta GPIO %s failed\n",
			      enable ? "enable" : "disable");

	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_set_quanta_gpio);

int tps68470_cf20_set_sensor_gpio_path(struct device *sensor, bool enable)
{
	struct regmap *regmap;
	unsigned int gpdo;
	unsigned int old_gpctl5a, old_gpctl6a, old_gpdo;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}

	/*
	 * Ghidra: SSCrdG2TiGpio::GpioOper(op=4) writes 0xc2 to
	 * GPCTL5A/GPCTL6A and then sets or clears GPDO bits 5/6.  Read the
	 * old state first so a failed two-stage update can be rolled back.
	 */
	ret = regmap_read(regmap, TPS68470_REG_GPCTL5A, &old_gpctl5a);
	if (ret)
		goto out_unlock;
	ret = regmap_read(regmap, TPS68470_REG_GPCTL6A, &old_gpctl6a);
	if (ret)
		goto out_unlock;
	ret = regmap_read(regmap, TPS68470_REG_GPDO, &old_gpdo);
	if (ret)
		goto out_unlock;

	if (sensor)
		dev_info(sensor,
			 "CF-20 sensor GPIO path: %s GPCTL5A/6A=0xc2, GPDO bits 5/6\n",
			 enable ? "enable" : "disable");

	ret = regmap_write(regmap, TPS68470_REG_GPCTL5A, 0xc2);
	if (ret)
		goto rollback;
	ret = regmap_read(regmap, TPS68470_REG_GPDO, &gpdo);
	if (ret)
		goto rollback;
	if (enable)
		gpdo |= BIT(5);
	else
		gpdo &= ~BIT(5);
	ret = regmap_write(regmap, TPS68470_REG_GPDO, gpdo);
	if (ret)
		goto rollback;

	ret = regmap_write(regmap, TPS68470_REG_GPCTL6A, 0xc2);
	if (ret)
		goto rollback;
	ret = regmap_read(regmap, TPS68470_REG_GPDO, &gpdo);
	if (ret)
		goto rollback;
	if (enable)
		gpdo |= BIT(6);
	else
		gpdo &= ~BIT(6);
	ret = regmap_write(regmap, TPS68470_REG_GPDO, gpdo);
	if (ret)
		goto rollback;

	ret = 0;
	goto out_unlock;

rollback:
	/* Best effort: never leave only one vendor GPIO path half-enabled. */
	regmap_write(regmap, TPS68470_REG_GPDO, old_gpdo);
	regmap_write(regmap, TPS68470_REG_GPCTL5A, old_gpctl5a);
	regmap_write(regmap, TPS68470_REG_GPCTL6A, old_gpctl6a);

out_unlock:
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret && sensor)
		dev_err_probe(sensor, ret, "CF-20 sensor GPIO path %s failed\n",
			      enable ? "enable" : "disable");

	return ret;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_set_sensor_gpio_path);

int tps68470_cf20_log_sensor_state(struct device *sensor)
{
	static const unsigned int regs[] = {
		TPS68470_REG_SGPO,
		TPS68470_REG_VCMVAL,
		TPS68470_REG_VIOVAL,
		TPS68470_REG_VSIOVAL,
		TPS68470_REG_VAVAL,
		TPS68470_REG_VDVAL,
		TPS68470_REG_S_I2C_CTL,
		TPS68470_REG_VCMCTL,
		TPS68470_REG_VACTL,
		TPS68470_REG_VDCTL,
		TPS68470_REG_PLLCTL,
		TPS68470_REG_CLKCFG1,
		TPS68470_REG_CLKCFG2,
	};
	/*
	 * Ghidra recovered the vendor CheckPMICStatus read set.  The
	 * individual meanings are not asserted here; keep this diagnostic
	 * read-only and do not acknowledge or clear any status latch.
	 */
	static const unsigned int vendor_status_regs[] = { 0x01, 0x02, 0x03, 0x29 };
	unsigned int values[ARRAY_SIZE(regs)];
	unsigned int vendor_status[ARRAY_SIZE(vendor_status_regs)];
	unsigned int gpio_modes[4];
	unsigned int sensor_gpio_modes[2];
	unsigned int gpdi;
	unsigned int gpdo;
	struct regmap *regmap;
	unsigned int i;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_read(regmap, regs[i], &values[i]);
		if (ret)
			goto out_unlock;
	}
	for (i = 0; i < ARRAY_SIZE(vendor_status_regs); i++) {
		ret = regmap_read(regmap, vendor_status_regs[i], &vendor_status[i]);
		if (ret)
			goto out_unlock;
	}
	ret = 0;

out_unlock:
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret)
		return dev_err_probe(sensor, ret,
				     "CF-20 TPS68470 state read failed\n");

	dev_info(sensor,
		 "CF-20 PMIC state: SGPO=0x%02x VCMVAL=0x%02x VIOVAL=0x%02x "
		 "VSIOVAL=0x%02x VAVAL=0x%02x VDVAL=0x%02x S_I2C=0x%02x "
		 "VCMCTL=0x%02x VACTL=0x%02x VDCTL=0x%02x PLLCTL=0x%02x "
		 "CLKCFG1=0x%02x CLKCFG2=0x%02x\n",
		 values[0], values[1], values[2], values[3], values[4], values[5],
		 values[6], values[7], values[8], values[9], values[10], values[11],
		 values[12]);
	dev_info(sensor,
		 "CF-20 vendor PMIC status: reg01=0x%02x reg02=0x%02x "
		 "reg03=0x%02x reg29=0x%02x (read-only)\n",
		 vendor_status[0], vendor_status[1], vendor_status[2],
		 vendor_status[3]);

	/*
	 * Read-only diagnostic for the vendor's I2C-daisy-chain path.  The
	 * Windows control-logic binary does not expose these fields in its
	 * SensorPowerOn trace, while Linux's optional daisy-chain GPIO setup can
	 * change them.  Do not change them here: this log only distinguishes an
	 * unconfigured GPIO1/2 input path from a sensor-side response problem.
	 */
	{
		static const unsigned int gpio_regs[] = {
			TPS68470_GPIO_CTL_REG_A(1),
			TPS68470_GPIO_CTL_REG_B(1),
			TPS68470_GPIO_CTL_REG_A(2),
			TPS68470_GPIO_CTL_REG_B(2),
		};

		for (i = 0; i < ARRAY_SIZE(gpio_regs); i++) {
			ret = regmap_read(regmap, gpio_regs[i], &gpio_modes[i]);
			if (ret)
				return dev_err_probe(sensor, ret,
						     "CF-20 GPIO mode read failed\n");
		}
		ret = regmap_read(regmap, TPS68470_REG_GPDI, &gpdi);
		if (ret)
			return dev_err_probe(sensor, ret,
					     "CF-20 GPIO input read failed\n");
		ret = regmap_read(regmap, TPS68470_REG_GPDO, &gpdo);
		if (ret)
			return dev_err_probe(sensor, ret,
					     "CF-20 GPIO output read failed\n");

		ret = regmap_read(regmap, TPS68470_REG_GPCTL5A,
				  &sensor_gpio_modes[0]);
		if (ret)
			return dev_err_probe(sensor, ret,
					     "CF-20 sensor GPIO5 mode read failed\n");
		ret = regmap_read(regmap, TPS68470_REG_GPCTL6A,
				  &sensor_gpio_modes[1]);
		if (ret)
			return dev_err_probe(sensor, ret,
					     "CF-20 sensor GPIO6 mode read failed\n");
	}
	dev_info(sensor,
		 "CF-20 I2C GPIO state: GPCTL1A=0x%02x GPCTL1B=0x%02x "
		 "GPCTL2A=0x%02x GPCTL2B=0x%02x GPDI=0x%02x GPDO=0x%02x "
		 "modes=%u/%u/%u/%u; sensor-path GPCTL5A=0x%02x "
		 "GPCTL6A=0x%02x GPDO5/6=%u/%u\n",
		 gpio_modes[0], gpio_modes[1], gpio_modes[2], gpio_modes[3], gpdi, gpdo,
		 (unsigned int)(gpio_modes[0] & TPS68470_GPIO_MODE_MASK),
		 (unsigned int)(gpio_modes[1] & TPS68470_GPIO_MODE_MASK),
		 (unsigned int)(gpio_modes[2] & TPS68470_GPIO_MODE_MASK),
		 (unsigned int)(gpio_modes[3] & TPS68470_GPIO_MODE_MASK),
		 sensor_gpio_modes[0], sensor_gpio_modes[1],
		 !!(gpdo & BIT(5)), !!(gpdo & BIT(6)));
	return 0;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_log_sensor_state);

int tps68470_cf20_sensor_power_valid(struct device *sensor)
{
	static const unsigned int regs[] = {
		TPS68470_REG_VCMVAL,
		TPS68470_REG_VIOVAL,
		TPS68470_REG_VSIOVAL,
		TPS68470_REG_VAVAL,
		TPS68470_REG_VDVAL,
		TPS68470_REG_VCMCTL,
		TPS68470_REG_VACTL,
		TPS68470_REG_VDCTL,
	};
	unsigned int values[ARRAY_SIZE(regs)];
	struct regmap *regmap;
	unsigned int i;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_read(regmap, regs[i], &values[i]);
		if (ret)
			goto out_unlock;
	}
	ret = 0;

out_unlock:
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret)
		return dev_err_probe(sensor, ret,
				     "CF-20 PMIC rail state read failed\n");

	/*
	 * These are read-only latch/value registers.  A previous CF-20
	 * failure showed VAVAL and VDVAL at zero even though regulator_enable()
	 * returned success; do not touch the sensor I2C bus in that state.
	 */
	if (!values[0] || !values[1] || !values[2] || !values[3] ||
	    !values[4] || !(values[5] & BIT(0)) || !(values[6] & BIT(0)) ||
	    !(values[7] & BIT(0))) {
		dev_err(sensor,
			"CF-20 PMIC rail state not latched: VCMVAL=0x%02x "
			"VIOVAL=0x%02x VSIOVAL=0x%02x VAVAL=0x%02x "
			"VDVAL=0x%02x VCMCTL=0x%02x VACTL=0x%02x VDCTL=0x%02x\n",
			values[0], values[1], values[2], values[3], values[4],
			values[5], values[6], values[7]);
		return -EREMOTEIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_sensor_power_valid);

/*
 * The CF-20 PMIC can lose the selector/control latch when the camera
 * domain is shut down while the regulator core still considers a consumer
 * enabled.  In that case regulator_enable() may be a software no-op on the
 * next start.  These values are the verified Windows/Linux CF-20 sensor
 * settings; restore them only after the DMI/ACPI gate and only on an
 * explicitly requested recovery path.
 */
int tps68470_cf20_restore_sensor_rails(struct device *sensor)
{
	struct regmap *regmap;
	int ret;

	if (!tps68470_cf20_machine())
		return -ENODEV;

	mutex_lock(&tps68470_cf20_regmap_lock);
	regmap = tps68470_cf20_regmap;
	if (!regmap) {
		ret = -EPROBE_DEFER;
		goto out_unlock;
	}

	/* Selector values: VCM=2.8152 V, VIO/VSIO=1.8006 V,
	 * VA=2.7084 V and VD=1.0500 V. */
	ret = regmap_write(regmap, TPS68470_REG_VCMVAL, 0x6d);
	if (ret)
		goto out_unlock;
	ret = regmap_write(regmap, TPS68470_REG_VIOVAL, 0x34);
	if (ret)
		goto out_unlock;
	ret = regmap_write(regmap, TPS68470_REG_VSIOVAL, 0x34);
	if (ret)
		goto out_unlock;
	ret = regmap_write(regmap, TPS68470_REG_VAVAL, 0x67);
	if (ret)
		goto out_unlock;
	ret = regmap_write(regmap, TPS68470_REG_VDVAL, 0x06);
	if (ret)
		goto out_unlock;

	/* Preserve unrelated control bits; set only each enable bit. */
	ret = regmap_update_bits(regmap, TPS68470_REG_VCMCTL,
                TPS68470_VCMCTL_EN_MASK,
				TPS68470_VCMCTL_EN_MASK);
	if (ret)
		goto out_unlock;
	ret = regmap_update_bits(regmap, TPS68470_REG_VACTL,
                TPS68470_VACTL_EN_MASK,
				TPS68470_VACTL_EN_MASK);
	if (ret)
		goto out_unlock;
	ret = regmap_update_bits(regmap, TPS68470_REG_VDCTL,
                TPS68470_VDCTL_EN_MASK,
				TPS68470_VDCTL_EN_MASK);

out_unlock:
	mutex_unlock(&tps68470_cf20_regmap_lock);
	if (ret)
		return dev_err_probe(sensor, ret,
                     "CF-20 PMIC sensor-rail restore failed\n");

	if (sensor)
		dev_warn(sensor, "CF-20 PMIC sensor-rail latch restored\n");

	return 0;
}
EXPORT_SYMBOL_GPL(tps68470_cf20_restore_sensor_rails);

static const struct regmap_config tps68470_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TPS68470_REG_MAX,
};

static int tps68470_chip_init(struct device *dev, struct regmap *regmap)
{
	unsigned int version;
	int ret;

	/*
	 * Read first.  A reset is a hardware write; do not issue it until the
	 * addressed device answers like a PMIC.  The caller also applies the
	 * DMI/ACPI/address board-data gate before reaching this function.
	 */
	ret = regmap_read(regmap, TPS68470_REG_REVID, &version);
	if (ret) {
		dev_err(dev, "Failed to read revision register: %d\n", ret);
		return ret;
	}
	if (version == 0x00 || version == 0xff) {
		dev_err(dev, "Invalid TPS68470 revision: 0x%02x\n", version);
		return -ENODEV;
	}

	/* Force software reset only after the preflight read succeeded. */
	ret = regmap_write(regmap, TPS68470_REG_RESET, TPS68470_REG_RESET_MASK);
	if (ret)
		return ret;

	dev_info(dev, "TPS68470 REVID: 0x%02x\n", version);

	return 0;
}

/** skl_int3472_tps68470_calc_type: Check what platform a device is designed for
 * @adev: A pointer to a &struct acpi_device
 *
 * Check CLDB buffer against the PMIC's adev. If present, then we check
 * the value of control_logic_type field and follow one of the
 * following scenarios:
 *
 *	1. No CLDB - likely ACPI tables designed for ChromeOS. We
 *	create platform devices for the GPIOs and OpRegion drivers.
 *
 *	2. CLDB, with control_logic_type = 2 - probably ACPI tables
 *	made for Windows 2-in-1 platforms. Register pdevs for GPIO,
 *	Clock and Regulator drivers to bind to.
 *
 *	3. Any other value in control_logic_type, we should never have
 *	gotten to this point; fail probe and return.
 *
 * Return:
 * * 1		Device intended for ChromeOS
 * * 2		Device intended for Windows
 * * -EINVAL	Where @adev has an object named CLDB but it does not conform to
 *		our expectations
 */
static int skl_int3472_tps68470_calc_type(struct acpi_device *adev)
{
	struct int3472_cldb cldb = { 0 };
	int ret;

	/*
	 * A CLDB buffer that exists, but which does not match our expectations
	 * should trigger an error so we don't blindly continue.
	 */
	ret = skl_int3472_fill_cldb(adev, &cldb);
	if (ret && ret != -ENODEV)
		return ret;

	if (ret)
		return DESIGNED_FOR_CHROMEOS;

	if (cldb.control_logic_type != 2)
		return -EINVAL;

	return DESIGNED_FOR_WINDOWS;
}

/*
 * Return the size of the flexible array member, because we'll need that later
 * on to pass .pdata_size to cells.
 */
static int
skl_int3472_fill_clk_pdata(struct device *dev,
			   struct cf20_tps68470_clk_platform_data **clk_pdata)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	struct acpi_device *consumer;
	unsigned int n_consumers = 0;
	const char *sensor_name;
	unsigned int i = 0;

	for_each_acpi_consumer_dev(adev, consumer)
		n_consumers++;

	if (!n_consumers) {
		/*
		 * The CF-20 bridge creates CAM0 dynamically because firmware keeps
		 * it hidden (_STA == 0).  After an INT3472 module reload, ACPI no
		 * longer reports that dynamic I2C client as a consumer.  Keep the
		 * clock child usable with the known, DMI-gated sensor name.  The
		 * caller still applies int3472_tps68470_get_board_data() before
		 * any PMIC reset or MFD child registration.
		 */
		if (dmi_match(DMI_SYS_VENDOR, "Panasonic Corporation") &&
		    dmi_match(DMI_PRODUCT_NAME, "CF-20-2") &&
		    dmi_match(DMI_BOARD_NAME, "CF20-2") &&
		    acpi_dev_hid_match(adev, "INT3472")) {
			*clk_pdata = devm_kzalloc(dev,
						 struct_size(*clk_pdata, consumers, 1),
						 GFP_KERNEL);
			if (!*clk_pdata)
				return -ENOMEM;

			(*clk_pdata)->n_consumers = 1;
			(*clk_pdata)->consumers[0].consumer_dev_name =
				devm_kstrdup(dev, "i2c-INT3477:00", GFP_KERNEL);
			if (!(*clk_pdata)->consumers[0].consumer_dev_name)
				return -ENOMEM;

			return 1;
		}

		dev_err(dev, "INT3472 seems to have no dependents\n");
		return -ENODEV;
	}

	*clk_pdata = devm_kzalloc(dev, struct_size(*clk_pdata, consumers, n_consumers),
				  GFP_KERNEL);
	if (!*clk_pdata)
		return -ENOMEM;

	(*clk_pdata)->n_consumers = n_consumers;
	i = 0;

	for_each_acpi_consumer_dev(adev, consumer) {
		sensor_name = devm_kasprintf(dev, GFP_KERNEL, I2C_DEV_NAME_FORMAT,
					     acpi_dev_name(consumer));
		if (!sensor_name) {
			acpi_dev_put(consumer);
			return -ENOMEM;
		}

		(*clk_pdata)->consumers[i].consumer_dev_name = sensor_name;
		i++;
	}

	return n_consumers;
}

static int skl_int3472_tps68470_probe(struct i2c_client *client)
{
	struct acpi_device *adev = ACPI_COMPANION(&client->dev);
	const struct int3472_tps68470_board_data *board_data;
	struct cf20_tps68470_clk_platform_data *clk_pdata;
	struct mfd_cell *cells;
	struct regmap *regmap;
	int n_consumers;
	int device_type;
	int ret;
	int i;

	if (!adev)
		return -ENODEV;

	n_consumers = skl_int3472_fill_clk_pdata(&client->dev, &clk_pdata);
	if (n_consumers < 0)
		return n_consumers;

	/*
	 * Do the ACPI-only classification and CF-20 board-data gate before
	 * tps68470_chip_init().  That function writes the PMIC reset register;
	 * reverse-engineered CF-20 board data is opt-in and must be checked first.
	 */
	device_type = skl_int3472_tps68470_calc_type(adev);
	if (device_type == DESIGNED_FOR_WINDOWS) {
		if (dmi_match(DMI_SYS_VENDOR, "Panasonic Corporation") &&
		    dmi_match(DMI_PRODUCT_NAME, "CF-20-2") &&
		    dmi_match(DMI_BOARD_NAME, "CF20-2"))
			dev_info(&client->dev,
				 "CF-20 candidate: address=0x%02x adapter=%d\n",
				 client->addr, client->adapter->nr);

		board_data = int3472_tps68470_get_board_data(client);
		if (!board_data)
			return dev_err_probe(&client->dev, -ENODEV,
					     "No board-data found for this model\n");

		clk_pdata->rate = board_data->clk_rate;
		clk_pdata->vendor_cf20_config = board_data->vendor_cf20_clock_config;
	}

	regmap = devm_regmap_init_i2c(client, &tps68470_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "Failed to create regmap: %ld\n", PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	i2c_set_clientdata(client, regmap);

	ret = tps68470_chip_init(&client->dev, regmap);
	if (ret < 0) {
		dev_err(&client->dev, "TPS68470 init error %d\n", ret);
		return ret;
	}

	switch (device_type) {
	case DESIGNED_FOR_WINDOWS:
		cells = kzalloc_objs(*cells, TPS68470_WIN_MFD_CELL_COUNT);
		if (!cells)
			return -ENOMEM;

		/*
		 * The order of the cells matters here! The clk must be first
		 * because the regulator depends on it. The gpios must be last,
		 * acpi_gpiochip_add() calls acpi_dev_clear_dependencies() and
		 * the clk + regulators must be ready when this happens.
		 */
		cells[0].name = "tps68470-clk";
		cells[0].platform_data = clk_pdata;
		cells[0].pdata_size = struct_size(clk_pdata, consumers, n_consumers);
		cells[1].name = "tps68470-regulator";
		cells[1].platform_data = (void *)board_data->tps68470_regulator_pdata;
		cells[1].pdata_size = sizeof(struct tps68470_regulator_platform_data_ext);
		cells[2].name = "tps68470-gpio";
		cells[2].swnode = board_data->tps68470_gpio_swnode;

		for (i = 0; i < board_data->n_gpiod_lookups; i++)
			gpiod_add_lookup_table(board_data->tps68470_gpio_lookup_tables[i]);

		/* Remove child devices explicitly in .remove for safe module reload. */
		ret = mfd_add_devices(&client->dev, PLATFORM_DEVID_NONE,
				      cells, TPS68470_WIN_MFD_CELL_COUNT,
				      NULL, 0, NULL);
		kfree(cells);

		if (ret) {
			for (i = 0; i < board_data->n_gpiod_lookups; i++)
				gpiod_remove_lookup_table(board_data->tps68470_gpio_lookup_tables[i]);
		}

		break;
	case DESIGNED_FOR_CHROMEOS:
		ret = mfd_add_devices(&client->dev, PLATFORM_DEVID_NONE,
				      tps68470_cros, ARRAY_SIZE(tps68470_cros),
				      NULL, 0, NULL);
		break;
	default:
		dev_err(&client->dev, "Failed to add MFD devices\n");
		return device_type;
	}

	/*
	 * No acpi_dev_clear_dependencies() here, since the acpi_gpiochip_add()
	 * for the GPIO cell already does this.
	 */

	if (!ret && tps68470_cf20_machine() && client->addr == 0x4c &&
	    ACPI_COMPANION(&client->dev) &&
	    acpi_dev_hid_match(ACPI_COMPANION(&client->dev), "INT3472")) {
		mutex_lock(&tps68470_cf20_regmap_lock);
		tps68470_cf20_regmap = regmap;
		mutex_unlock(&tps68470_cf20_regmap_lock);
	}

	return ret;
}

static void skl_int3472_tps68470_remove(struct i2c_client *client)
{
	const struct int3472_tps68470_board_data *board_data;
	int i;

	/* Child GPIO/clock/regulator devices must be gone before re-probe. */
	mfd_remove_devices(&client->dev);

	if (tps68470_cf20_machine() && client->addr == 0x4c) {
		mutex_lock(&tps68470_cf20_regmap_lock);
		tps68470_cf20_regmap = NULL;
		mutex_unlock(&tps68470_cf20_regmap_lock);
	}

	board_data = int3472_tps68470_get_board_data(client);
	if (board_data) {
		for (i = 0; i < board_data->n_gpiod_lookups; i++)
			gpiod_remove_lookup_table(board_data->tps68470_gpio_lookup_tables[i]);
	}
}

static const struct acpi_device_id int3472_device_id[] = {
	{ "INT3472", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, int3472_device_id);

static struct i2c_driver int3472_tps68470 = {
	.driver = {
		.name = "int3472-tps68470",
		.acpi_match_table = int3472_device_id,
	},
	.probe = skl_int3472_tps68470_probe,
	.remove = skl_int3472_tps68470_remove,
};
module_i2c_driver(int3472_tps68470);

MODULE_DESCRIPTION("Intel SkyLake INT3472 ACPI TPS68470 Device Driver");
MODULE_AUTHOR("Daniel Scally <djrscally@gmail.com>");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("INTEL_INT3472");
MODULE_SOFTDEP("pre: clk-tps68470 tps68470-regulator");
