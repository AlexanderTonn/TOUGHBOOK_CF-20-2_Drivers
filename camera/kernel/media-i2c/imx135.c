// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX135 sensor driver for the Panasonic Toughbook CF-20.
 *
 * The ACPI instance is INT3471 at I2C address 0x10.  The board provides a
 * 24 MHz clock, four CSI-2 lanes and three TPS68470 camera rails.  The mode
 * tables below use the public IMX135 register interface.  Panasonic tuning
 * blobs are deliberately not part of this driver.
 */

#include <linux/clk.h>
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include "../../../include/linux/platform_data/cf20-tps68470.h"
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include <media/media-entity.h>
#include "../../../include/media/ipu-bridge.h"
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX135_XVCLK_FREQ		24000000
#define IMX135_NUM_LANES		4
#define IMX135_PIXEL_RATE		360000000U
#define IMX135_LINK_FREQ		450000000LL

#define IMX135_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX135_CHIP_ID			0x0135

#define IMX135_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX135_MODE_STANDBY		0x00
#define IMX135_MODE_STREAMING		0x01
#define IMX135_REG_SW_RESET		CCI_REG8(0x0103)
#define IMX135_REG_GROUP_HOLD		CCI_REG8(0x0104)
#define IMX135_REG_ORIENTATION		CCI_REG8(0x0101)

#define IMX135_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX135_REG_ANALOG_GAIN	CCI_REG8(0x0205)
#define IMX135_REG_DIGITAL_GAIN_GR	CCI_REG16(0x020e)
#define IMX135_REG_DIGITAL_GAIN_R	CCI_REG16(0x0210)
#define IMX135_REG_DIGITAL_GAIN_B	CCI_REG16(0x0212)
#define IMX135_REG_DIGITAL_GAIN_GB	CCI_REG16(0x0214)
#define IMX135_REG_FRAME_LENGTH	CCI_REG16(0x0340)

#define IMX135_EXPOSURE_OFFSET		4
#define IMX135_ANALOG_GAIN_MAX		0xff
#define IMX135_DIGITAL_GAIN_DEFAULT	0x0100

static bool allow_power_on;
module_param_named(allow_power_on, allow_power_on, bool, 0444);
MODULE_PARM_DESC(allow_power_on,
		 "Allow IMX135 rail/clock enable on the Panasonic CF-20");

static bool allow_streaming;
module_param_named(allow_streaming, allow_streaming, bool, 0444);
MODULE_PARM_DESC(allow_streaming,
		 "Allow IMX135 register programming and CSI-2 streaming");

/*
 * The CF-20 firmware currently reports CAM0._STA=0 on Linux even when the
 * BIOS camera setting is enabled. Keep the safe default: _STA=0 blocks all
 * hardware access. This is an explicit, DMI-gated diagnostic escape hatch
 * for this exact board; it does not enable streaming.
 */
static bool allow_cf20_acpi_disabled;
module_param_named(allow_cf20_acpi_disabled, allow_cf20_acpi_disabled, bool,
			   0444);
MODULE_PARM_DESC(allow_cf20_acpi_disabled,
		 "Allow explicit CF-20 test access while CAM0._STA is zero");

static bool diagnostic;
module_param_named(diagnostic, diagnostic, bool, 0444);
MODULE_PARM_DESC(diagnostic,
		 "Log read-only CF-20 I2C transfer diagnostics");

static bool combined_read;
module_param_named(combined_read, combined_read, bool, 0444);
MODULE_PARM_DESC(combined_read,
		 "Use a combined write-address/read transfer for CF-20 sensor reads");

/*
 * Ghidra: imx135.sys opens Windows Resource Hub channel 10 and sends
 * TCA9535-like register/value writes.  CAM0._CRS resource 1 at 0x0e
 * belongs to the AD5816 VCM.  The _DSM value 0x02005002 is the Windows
 * Resource Hub connection for CAM0._CRS resource 2 at physical I2C address
 * 0x50.  Chip identity is not independently verified; the target NACKs
 * until the TPS68470 camera I2C path is active.
 *
 * Keep this opt-in.  Registration is write-free; the exact vendor sequence
 * is issued only from the explicitly enabled power path.
 */
static bool tca9535_mode;
module_param_named(tca9535_mode, tca9535_mode, bool, 0444);
MODULE_PARM_DESC(tca9535_mode,
		 "Enable experimental CF-20 TCA9535-like sequence at ACPI resource 2");

/*
 * Ghidra: iactrllogic64.sys calls the resource-hub/TCA9535 power-on
 * operation before SensorPowerOn. Keep this separate from tca9535_mode so
 * the post-daisy experiment remains reproducible. The default is false;
 * both switches must be explicitly enabled before any TCA9535 write.
 */
static bool tca9535_before_power;
module_param_named(tca9535_before_power, tca9535_before_power, bool, 0444);
MODULE_PARM_DESC(tca9535_before_power,
		 "Issue the recovered CF-20 TCA9535 sequence before camera rails");

/*
 * Resource-Hub writes on Windows open and close target channel 10 for every
 * register operation.  A Linux I2C transfer keeps the adapter active between
 * those operations.  Allow only a small, opt-in retry window for a transient
 * target NACK; payload and register order remain unchanged.
 */
static unsigned int tca9535_retries = 2;
module_param_named(tca9535_retries, tca9535_retries, uint, 0444);
MODULE_PARM_DESC(tca9535_retries,
		 "Retry CF-20 TCA9535 writes after EREMOTEIO (0-2, opt-in path)");

/*
 * Linux has no Windows Resource-Hub/SPB transport for the CF-20 auxiliary
 * target.  The normal Linux i2c client is therefore only a diagnostic
 * fallback: keep it disabled unless a developer explicitly requests a
 * direct bus experiment.  This prevents a guessed topology from writing
 * vendor power-control registers during normal probe or boot.
 */
static bool allow_direct_tca_i2c;
module_param_named(allow_direct_tca_i2c, allow_direct_tca_i2c, bool, 0444);
MODULE_PARM_DESC(allow_direct_tca_i2c,
		 "Allow experimental direct Linux I2C access to CF-20 TCA9535");

/*
 * Ghidra: iactrllogic64.sys SensorPowerOn calls GpioOper(type 5).  That
 * updates SGPO bit 2 (resetn), waits 1 ms, then updates bit 0 (enable).
 * Keep timing selectable for hardware A/B testing, but use vendor timing
 * when power-on is explicitly enabled:
 *   0 = leave SGPO unchanged
 *   1 = assert resetn/enable before HCLK (vendor sequence)
 *   2 = assert resetn/enable after the complete vendor power sequence
 */
static int sensor_gpio_mode;
module_param_named(sensor_gpio_mode, sensor_gpio_mode, int, 0444);
MODULE_PARM_DESC(sensor_gpio_mode,
		 "CF-20 sensor GPIO timing: 0=none, 1=before-HCLK, 2=after-daisy");

/*
 * Ghidra: CrdG2TiSensor::SetGpio is a separate vendor operation from
 * CrdG2TiSensor::SensorPowerOn.  Keep an explicit A/B mode that performs
 * the recovered SGPO resetn -> 1 ms -> enable sequence before any camera
 * rail or clock operation.  It is disabled by default because this changes
 * hardware state earlier than the normal Linux power sequence.
 */
static bool sensor_gpio_pre_power;
module_param_named(sensor_gpio_pre_power, sensor_gpio_pre_power, bool, 0444);
MODULE_PARM_DESC(sensor_gpio_pre_power,
		 "CF-20 test: run SGPO reset/enable before camera power-on");

/*
 * Ghidra: CrdG2TiSensor::SensorPowerOn calls VoltageWF::IoActive first.
 * That operation sets S_I2C_CTL bit 1 before VoltageWF::PowerOn; the
 * daisy-chain operation sets bit 0 separately later.
 * Keep this exact ordering as a separate opt-in test.
 */
static bool sensor_io_active_before_power;
module_param_named(sensor_io_active_before_power,
		   sensor_io_active_before_power, bool, 0444);
MODULE_PARM_DESC(sensor_io_active_before_power,
		 "CF-20 test: set S_I2C_CTL bit 1 before camera power-on");

/*
 * Ghidra: the CF-20 CrdG2TiSensor selects CrdG2TiQuantaGpio.  Its type-5
 * operation writes GPCTL3A=0xc2 and toggles GPDO bit 3.  This is the
 * recovered vendor path; it is separate from the experimental GPCTL5/6
 * sensor_path_mode and from the SGPO reset/enable test path above.
 */
static bool quanta_gpio_mode;
module_param_named(quanta_gpio_mode, quanta_gpio_mode, bool, 0444);
MODULE_PARM_DESC(quanta_gpio_mode,
		 "Enable the recovered CF-20 Quanta GPIO power path");

/*
 * Ghidra: iactrllogic64.sys also contains CrdG2TiGpio::GpioOper(type 5),
 * which updates TPS68470 SGPO bit 2, waits 1 ms, then updates bit 0.  It is
 * not the Quanta path above.  Keep both vendor variants explicit and
 * mutually opt-in; default remains no vendor GPIO write.
 */
static bool vendor_gpio_mode;
module_param_named(vendor_gpio_mode, vendor_gpio_mode, bool, 0444);
MODULE_PARM_DESC(vendor_gpio_mode,
		 "Enable exact CF-20 TPS68470 SGPO vendor GPIO path");

/*
 * Ghidra: iactrllogic64.sys has a separate GpioOper(type 4) that programs
 * TPS68470 GPCTL5A/6A and GPDO5/6. Keep it off by default. Values 1 and 2
 * are hardware A/B tests for when that path is required by the camera stack.
 */
static int sensor_path_mode;
module_param_named(sensor_path_mode, sensor_path_mode, int, 0444);
MODULE_PARM_DESC(sensor_path_mode,
		 "CF-20 PMIC sensor path: 0=none, 1=before-HCLK, 2=after-daisy");

static bool imx135_cf20_machine(void)
{
	return dmi_match(DMI_SYS_VENDOR, "Panasonic Corporation") &&
	       dmi_match(DMI_PRODUCT_NAME, "CF-20-2") &&
	       dmi_match(DMI_BOARD_NAME, "CF20-2");
}

static const char * const imx135_supply_names[] = {
	"avdd",
	"dvdd",
	"vcm",
	"dovdd",
};

/* Ghidra-recovered CF-20 VoltageWF requests, in the same order as above. */
static const unsigned int imx135_supply_voltages[] = {
	2708400, /* VA: TPS68470 VAVAL = 0x67 */
	1050000, /* VD: TPS68470 VDVAL = 0x06 */
	2815200, /* VCM: TPS68470 VCMVAL = 0x6d */
	1800600, /* sensor I/O: TPS68470 VSIOVAL = 0x34 */
};

/* Ghidra: VoltageWF::Initialize writes VA, VSIO, VD, then VCM. */
static const unsigned int imx135_voltage_order[] = { 0, 3, 1, 2 };

/*
 * Ghidra: CrdG2TiSensor::SensorPowerOn calls IoActive first. On CF-20,
 * Linux dovdd is VSIO; its regulator callback sets S_I2C_CTL bit 1. Then
 * VoltageWF::PowerOn enables VA, VD, and VCM. The separate daisy-chain
 * helper sets S_I2C_CTL bit 0 later.
 */
static const unsigned int imx135_enable_order[] = { 3, 0, 1, 2 };

struct imx135_mode {
	u32 width;
	u32 height;
	u32 frame_length;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct imx135 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *xvclk;
	struct i2c_client *tca9535;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *resetn_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx135_supply_names)];

	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;

	const struct imx135_mode *cur_mode;
	unsigned int num_lanes;
	bool have_link_freq;
	bool streaming;
	bool tca9535_powered;
};

static int imx135_tca9535_write(struct imx135 *imx135, u8 reg, u8 value)
{
	u8 payload[2] = { reg, value };
	struct i2c_msg msg;
	unsigned int attempt;
	int ret;

	if (!imx135->tca9535)
		return -ENODEV;

	msg.addr = imx135->tca9535->addr;
	msg.flags = 0;
	msg.len = sizeof(payload);
	msg.buf = payload;

	for (attempt = 0; ; attempt++) {
		ret = i2c_transfer(imx135->tca9535->adapter, &msg, 1);
		if (ret == 1)
			return 0;
		if (ret != -EREMOTEIO || attempt >= min(tca9535_retries, 2U))
			return ret < 0 ? ret : -EIO;

		/* Keep retry delay bounded. No new register value is generated. */
		usleep_range(1000U << attempt, 1500U << attempt);
		dev_warn(imx135->dev,
			 "CF-20 TCA9535 write reg 0x%02x retry %u after -EREMOTEIO\n",
			 reg, attempt + 1);
	}
}

static int imx135_tca9535_register(struct imx135 *imx135)
{
	struct i2c_board_info board_info = {
		.type = "cf20-tca9535",
		/*
		 * CAM0 already owns the ACPI name INT3471:00 at 0x10.
		 * Keep CAM0 as the firmware source for resource lookup, but give
		 * the auxiliary resource its normal bus/address name so it does
		 * not collide with the sensor client in sysfs.
		 */
		.dev_name = "2-0050",
		.fwnode = dev_fwnode(imx135->dev),
	};

	if (!tca9535_mode)
		return 0;

	/*
	 * CAM0._CRS resource 1 (0x0e) is the AD5816 VCM.  The Windows
	 * Resource-Hub ID 0x02005002 maps to CAM0._CRS resource 2, the
	 * CF-20 GPIO/power target at physical I2C address 0x50.
	 */
	imx135->tca9535 = i2c_acpi_new_device_by_fwnode(board_info.fwnode, 2,
							&board_info);
	if (IS_ERR(imx135->tca9535)) {
		int ret = PTR_ERR(imx135->tca9535);

		imx135->tca9535 = NULL;
		return dev_err_probe(imx135->dev, ret,
				     "failed to instantiate CF-20 TCA9535 resource 2\n");
	}

	if (imx135->tca9535->addr != 0x50) {
		dev_err(imx135->dev,
			"CF-20 TCA9535 resource 2 resolved to unexpected address 0x%02x\n",
			imx135->tca9535->addr);
		i2c_unregister_device(imx135->tca9535);
		imx135->tca9535 = NULL;
		return -ENODEV;
	}

	dev_info(imx135->dev,
		 "CF-20 TCA9535 resource 2 prepared at 0x%02x (no write yet)\n",
		 imx135->tca9535->addr);
	return 0;
}

static void imx135_tca9535_unregister(struct imx135 *imx135)
{
	if (!imx135->tca9535)
		return;

	i2c_unregister_device(imx135->tca9535);
	imx135->tca9535 = NULL;
}

static int imx135_tca9535_power_on(struct imx135 *imx135)
{
	static const struct {
		u8 reg;
		u8 value;
	} sequence[] = {
		/* Ghidra FUN_1400069fc(selector 0/6), exact vendor order. */
		{ 0x07, 0xfe },
		{ 0x03, 0x01 },
		{ 0x06, 0xe0 },
		{ 0x02, 0xe1 },
	};
	unsigned int i;
	int ret;

	if (!imx135->tca9535)
		return -ENODEV;
	if (!allow_direct_tca_i2c) {
		dev_err(imx135->dev,
			"CF-20 TCA9535 requires Windows Resource-Hub transport; "
			"direct Linux I2C disabled, no write attempted\n");
		return -EOPNOTSUPP;
	}

	dev_info(imx135->dev,
		 "CF-20 TCA9535: power-on sequence at ACPI resource 2 (0x%02x)\n",
		 imx135->tca9535->addr);
	for (i = 0; i < ARRAY_SIZE(sequence); i++) {
		ret = imx135_tca9535_write(imx135, sequence[i].reg,
					   sequence[i].value);
		if (ret) {
			dev_err_probe(imx135->dev, ret,
				       "CF-20 TCA9535 write reg 0x%02x value 0x%02x failed\n",
				       sequence[i].reg, sequence[i].value);
			return ret;
		}
	}

	return 0;
}

static int imx135_tca9535_power_off(struct imx135 *imx135)
{
	int ret;

	if (!imx135->tca9535)
		return -ENODEV;
	/* No direct write was made when the guarded power-on path was rejected. */
	if (!allow_direct_tca_i2c)
		return 0;

	/* Ghidra FUN_1400069fc(selector 0/6), exact vendor power-off order. */
	ret = imx135_tca9535_write(imx135, 0x06, 0xe0);
	if (ret)
		return ret;

	return imx135_tca9535_write(imx135, 0x01, 0x1e);
}

static inline struct imx135 *to_imx135(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx135, sd);
}

/* Public IMX135 setup values.  Stream-on is intentionally separate. */
static const struct cci_reg_sequence imx135_common_regs[] = {
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x0105), 0x01 },
	{ CCI_REG8(0x0110), 0x00 },
	{ CCI_REG8(0x0220), 0x01 },
	{ CCI_REG8(0x3302), 0x01 },
	{ CCI_REG8(0x3833), 0x20 },
	{ CCI_REG8(0x3893), 0x00 },
	{ CCI_REG8(0x3906), 0x08 },
	{ CCI_REG8(0x3907), 0x01 },
	{ CCI_REG8(0x391b), 0x01 },
	{ CCI_REG8(0x3c09), 0x01 },
	{ CCI_REG8(0x600a), 0x00 },
	{ CCI_REG8(0x3008), 0xb0 },
	{ CCI_REG8(0x320a), 0x01 },
	{ CCI_REG8(0x320d), 0x10 },
	{ CCI_REG8(0x3216), 0x2e },
	{ CCI_REG8(0x322c), 0x02 },
	{ CCI_REG8(0x3409), 0x0c },
	{ CCI_REG8(0x340c), 0x2d },
	{ CCI_REG8(0x3411), 0x39 },
	{ CCI_REG8(0x3414), 0x1e },
	{ CCI_REG8(0x3427), 0x04 },
	{ CCI_REG8(0x3480), 0x1e },
	{ CCI_REG8(0x3484), 0x1e },
	{ CCI_REG8(0x3488), 0x1e },
	{ CCI_REG8(0x348c), 0x1e },
	{ CCI_REG8(0x3490), 0x1e },
	{ CCI_REG8(0x3494), 0x1e },
	{ CCI_REG8(0x3511), 0x8f },
	{ CCI_REG8(0x364f), 0x2d },
	{ CCI_REG8(0x011e), 0x18 },
	{ CCI_REG8(0x011f), 0x00 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x01 },
	{ CCI_REG8(0x0305), 0x0b },
	{ CCI_REG8(0x0309), 0x05 },
	{ CCI_REG8(0x030c), 0x01 },
	{ CCI_REG8(0x030d), 0x9c },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x0108), 0x03 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0392), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x4082), 0x00 },
	{ CCI_REG8(0x4083), 0x00 },
	{ CCI_REG8(0x7006), 0x04 },
	{ CCI_REG8(0x0700), 0x00 },
	{ CCI_REG8(0x3a63), 0x00 },
	{ CCI_REG8(0x4100), 0xf8 },
	{ CCI_REG8(0x4203), 0xff },
	{ CCI_REG8(0x4344), 0x00 },
	{ CCI_REG8(0x441c), 0x01 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0210), 0x01 },
	{ CCI_REG8(0x0211), 0x00 },
	{ CCI_REG8(0x0212), 0x01 },
	{ CCI_REG8(0x0213), 0x00 },
	{ CCI_REG8(0x0214), 0x01 },
	{ CCI_REG8(0x0215), 0x00 },
	{ CCI_REG8(0x0230), 0x00 },
	{ CCI_REG8(0x0231), 0x00 },
	{ CCI_REG8(0x0233), 0x00 },
	{ CCI_REG8(0x0234), 0x00 },
	{ CCI_REG8(0x0235), 0x40 },
	{ CCI_REG8(0x0238), 0x01 },
	{ CCI_REG8(0x0239), 0x04 },
	{ CCI_REG8(0x023b), 0x00 },
	{ CCI_REG8(0x023c), 0x01 },
	{ CCI_REG8(0x33b0), 0x04 },
	{ CCI_REG8(0x33b1), 0x00 },
	{ CCI_REG8(0x33b3), 0x00 },
	{ CCI_REG8(0x33b4), 0x01 },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3a43), 0x01 },
};

static const struct cci_reg_sequence imx135_4208x3120_regs[] = {
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x3a06), 0x11 },
	{ CCI_REG8(0x0390), 0x00 },
	{ CCI_REG8(0x0391), 0x11 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x4082), 0x01 },
	{ CCI_REG8(0x4083), 0x01 },
	{ CCI_REG8(0x0340), 0x0c },
	{ CCI_REG8(0x0341), 0xc8 },
	{ CCI_REG8(0x0342), 0x11 },
	{ CCI_REG8(0x0343), 0xdc },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x10 },
	{ CCI_REG8(0x0349), 0x6f },
	{ CCI_REG8(0x034a), 0x0c },
	{ CCI_REG8(0x034b), 0x2f },
	{ CCI_REG8(0x034c), 0x10 },
	{ CCI_REG8(0x034d), 0x70 },
	{ CCI_REG8(0x034e), 0x0c },
	{ CCI_REG8(0x034f), 0x30 },
	{ CCI_REG8(0x0350), 0x00 },
	{ CCI_REG8(0x0351), 0x00 },
	{ CCI_REG8(0x0352), 0x00 },
	{ CCI_REG8(0x0353), 0x00 },
	{ CCI_REG8(0x0354), 0x10 },
	{ CCI_REG8(0x0355), 0x70 },
	{ CCI_REG8(0x0356), 0x0c },
	{ CCI_REG8(0x0357), 0x30 },
	{ CCI_REG8(0x301d), 0x30 },
	{ CCI_REG8(0x3310), 0x10 },
	{ CCI_REG8(0x3311), 0x70 },
	{ CCI_REG8(0x3312), 0x0c },
	{ CCI_REG8(0x3313), 0x30 },
	{ CCI_REG8(0x331c), 0x01 },
	{ CCI_REG8(0x331d), 0x68 },
	{ CCI_REG8(0x4084), 0x00 },
	{ CCI_REG8(0x4085), 0x00 },
	{ CCI_REG8(0x4086), 0x00 },
	{ CCI_REG8(0x4087), 0x00 },
	{ CCI_REG8(0x4400), 0x00 },
	{ CCI_REG8(0x0830), 0x87 },
	{ CCI_REG8(0x0831), 0x3f },
	{ CCI_REG8(0x0832), 0x67 },
	{ CCI_REG8(0x0833), 0x3f },
	{ CCI_REG8(0x0834), 0x3f },
	{ CCI_REG8(0x0835), 0x4f },
	{ CCI_REG8(0x0836), 0xdf },
	{ CCI_REG8(0x0837), 0x47 },
	{ CCI_REG8(0x0839), 0x1f },
	{ CCI_REG8(0x083a), 0x17 },
	{ CCI_REG8(0x083b), 0x02 },
	{ CCI_REG8(0x0202), 0x0c },
	{ CCI_REG8(0x0203), 0xc4 },
};

static const struct cci_reg_sequence imx135_1920x1080_regs[] = {
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x3a06), 0x11 },
	{ CCI_REG8(0x0390), 0x01 },
	{ CCI_REG8(0x0391), 0x22 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0x3a },
	{ CCI_REG8(0x0342), 0x11 },
	{ CCI_REG8(0x0343), 0xdc },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0xb8 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0xe0 },
	{ CCI_REG8(0x0348), 0x0f },
	{ CCI_REG8(0x0349), 0xb7 },
	{ CCI_REG8(0x034a), 0x0a },
	{ CCI_REG8(0x034b), 0x4f },
	{ CCI_REG8(0x034c), 0x07 },
	{ CCI_REG8(0x034d), 0x80 },
	{ CCI_REG8(0x034e), 0x04 },
	{ CCI_REG8(0x034f), 0x38 },
	{ CCI_REG8(0x0350), 0x00 },
	{ CCI_REG8(0x0351), 0x00 },
	{ CCI_REG8(0x0352), 0x00 },
	{ CCI_REG8(0x0353), 0x00 },
	{ CCI_REG8(0x0354), 0x07 },
	{ CCI_REG8(0x0355), 0x80 },
	{ CCI_REG8(0x0356), 0x04 },
	{ CCI_REG8(0x0357), 0x38 },
	{ CCI_REG8(0x301d), 0x30 },
	{ CCI_REG8(0x3310), 0x07 },
	{ CCI_REG8(0x3311), 0x80 },
	{ CCI_REG8(0x3312), 0x04 },
	{ CCI_REG8(0x3313), 0x38 },
	{ CCI_REG8(0x331c), 0x04 },
	{ CCI_REG8(0x331d), 0xb0 },
	{ CCI_REG8(0x4084), 0x07 },
	{ CCI_REG8(0x4085), 0x80 },
	{ CCI_REG8(0x4086), 0x04 },
	{ CCI_REG8(0x4087), 0x38 },
	{ CCI_REG8(0x4400), 0x00 },
	{ CCI_REG8(0x0830), 0x87 },
	{ CCI_REG8(0x0831), 0x3f },
	{ CCI_REG8(0x0832), 0x67 },
	{ CCI_REG8(0x0833), 0x3f },
	{ CCI_REG8(0x0834), 0x3f },
	{ CCI_REG8(0x0835), 0x4f },
	{ CCI_REG8(0x0836), 0xdf },
	{ CCI_REG8(0x0837), 0x47 },
	{ CCI_REG8(0x0839), 0x1f },
	{ CCI_REG8(0x083a), 0x17 },
	{ CCI_REG8(0x083b), 0x02 },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0x36 },
};

/* Ghidra table 0x140022c30: 3840x2160, 24 MHz MCLK, 450 MHz CSI-2. */
static const struct cci_reg_sequence imx135_3840x2160_regs[] = {
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x3a06), 0x11 },
	{ CCI_REG8(0x0390), 0x00 },
	{ CCI_REG8(0x0391), 0x11 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x4082), 0x01 },
	{ CCI_REG8(0x4083), 0x01 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0x3a },
	{ CCI_REG8(0x0342), 0x11 },
	{ CCI_REG8(0x0343), 0xdc },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0xb8 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0xe0 },
	{ CCI_REG8(0x0348), 0x0f },
	{ CCI_REG8(0x0349), 0xb7 },
	{ CCI_REG8(0x034a), 0x0a },
	{ CCI_REG8(0x034b), 0x4f },
	{ CCI_REG8(0x034c), 0x0f },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x08 },
	{ CCI_REG8(0x034f), 0x70 },
	{ CCI_REG8(0x0350), 0x00 },
	{ CCI_REG8(0x0351), 0x00 },
	{ CCI_REG8(0x0352), 0x00 },
	{ CCI_REG8(0x0353), 0x00 },
	{ CCI_REG8(0x0354), 0x0f },
	{ CCI_REG8(0x0355), 0x00 },
	{ CCI_REG8(0x0356), 0x08 },
	{ CCI_REG8(0x0357), 0x70 },
	{ CCI_REG8(0x301d), 0x30 },
	{ CCI_REG8(0x3310), 0x0f },
	{ CCI_REG8(0x3311), 0x00 },
	{ CCI_REG8(0x3312), 0x08 },
	{ CCI_REG8(0x3313), 0x70 },
	{ CCI_REG8(0x331c), 0x01 },
	{ CCI_REG8(0x331d), 0x68 },
	{ CCI_REG8(0x4084), 0x00 },
	{ CCI_REG8(0x4085), 0x00 },
	{ CCI_REG8(0x4086), 0x00 },
	{ CCI_REG8(0x4087), 0x00 },
	{ CCI_REG8(0x4400), 0x00 },
	{ CCI_REG8(0x0830), 0x87 },
	{ CCI_REG8(0x0831), 0x3f },
	{ CCI_REG8(0x0832), 0x67 },
	{ CCI_REG8(0x0833), 0x3f },
	{ CCI_REG8(0x0834), 0x3f },
	{ CCI_REG8(0x0835), 0x4f },
	{ CCI_REG8(0x0836), 0xdf },
	{ CCI_REG8(0x0837), 0x47 },
	{ CCI_REG8(0x0839), 0x1f },
	{ CCI_REG8(0x083a), 0x17 },
	{ CCI_REG8(0x083b), 0x02 },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0x36 },
};

static const struct imx135_mode imx135_modes[] = {
	{
		.width = 4208,
		.height = 3120,
		.frame_length = 0x0cc8,
		.regs = imx135_4208x3120_regs,
		.num_regs = ARRAY_SIZE(imx135_4208x3120_regs),
	},
	{
		.width = 1920,
		.height = 1080,
		.frame_length = 0x0a3a,
		.regs = imx135_1920x1080_regs,
		.num_regs = ARRAY_SIZE(imx135_1920x1080_regs),
	},
	{
		.width = 3840,
		.height = 2160,
		.frame_length = 0x0a3a,
		.regs = imx135_3840x2160_regs,
		.num_regs = ARRAY_SIZE(imx135_3840x2160_regs),
	},
};

/* Both supported mode tables use the same 360 MHz pixel timing. */
static const s64 imx135_link_freq_menu[] = {
	IMX135_LINK_FREQ,
};

static void imx135_fill_format(struct v4l2_mbus_framefmt *fmt,
				       const struct imx135_mode *mode)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx135_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx135_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(imx135_modes) ||
	    fse->code != MEDIA_BUS_FMT_SRGGB10_1X10)
		return -EINVAL;

	fse->min_width = fse->max_width = imx135_modes[fse->index].width;
	fse->min_height = fse->max_height = imx135_modes[fse->index].height;
	return 0;
}

static int imx135_update_exposure_range(struct imx135 *imx135,
					 u32 frame_length)
{
	s64 exposure_max = frame_length - IMX135_EXPOSURE_OFFSET;
	s64 exposure_def = min_t(s64, imx135->exposure->default_value,
					 exposure_max);

	return __v4l2_ctrl_modify_range(imx135->exposure, 1, exposure_max, 1,
					       exposure_def);
}

static int imx135_write_exposure_gain(struct imx135 *imx135)
{
	u32 frame_length = imx135->cur_mode->height + imx135->vblank->val;
	u32 exposure = min_t(u32, imx135->exposure->val,
				    frame_length - IMX135_EXPOSURE_OFFSET);
	int ret;
	int release_ret;

	/* Vendor requires frame length >= exposure + 4. */
	frame_length = max(frame_length, exposure + IMX135_EXPOSURE_OFFSET);
	frame_length = min_t(u32, frame_length, U16_MAX);

	ret = cci_write(imx135->regmap, IMX135_REG_GROUP_HOLD, 1, NULL);
	if (ret)
		return ret;

	ret = cci_write(imx135->regmap, IMX135_REG_FRAME_LENGTH,
				frame_length, NULL);
	if (!ret)
		ret = cci_write(imx135->regmap, IMX135_REG_EXPOSURE,
					exposure, NULL);
	if (!ret)
		ret = cci_write(imx135->regmap, IMX135_REG_ANALOG_GAIN,
					imx135->analogue_gain->val, NULL);
	if (!ret)
		ret = cci_write(imx135->regmap, IMX135_REG_DIGITAL_GAIN_GR,
					IMX135_DIGITAL_GAIN_DEFAULT, NULL);
	if (!ret)
		ret = cci_write(imx135->regmap, IMX135_REG_DIGITAL_GAIN_R,
					IMX135_DIGITAL_GAIN_DEFAULT, NULL);
	if (!ret)
		ret = cci_write(imx135->regmap, IMX135_REG_DIGITAL_GAIN_B,
					IMX135_DIGITAL_GAIN_DEFAULT, NULL);
	if (!ret)
		ret = cci_write(imx135->regmap, IMX135_REG_DIGITAL_GAIN_GB,
					IMX135_DIGITAL_GAIN_DEFAULT, NULL);

	/* Always release Group-Hold after a failed partial sequence. */
	release_ret = cci_write(imx135->regmap, IMX135_REG_GROUP_HOLD, 0, NULL);
	if (!ret)
		ret = release_ret;

	return ret;
}

static int imx135_set_fmt(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	const struct imx135_mode *mode;

	mode = v4l2_find_nearest_size(imx135_modes, ARRAY_SIZE(imx135_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);
	imx135_fill_format(&fmt->format, mode);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		struct imx135 *imx135 = to_imx135(sd);

		__v4l2_ctrl_modify_range(imx135->vblank, 0,
					 0xffff - mode->height, 1,
					  mode->frame_length - mode->height);
		imx135->cur_mode = mode;
		imx135_update_exposure_range(imx135, mode->frame_length);
	}

	return 0;
}

static int imx135_init_state(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = state ? V4L2_SUBDEV_FORMAT_TRY :
				 V4L2_SUBDEV_FORMAT_ACTIVE,
	};

	imx135_fill_format(&fmt.format, &imx135_modes[0]);
	return imx135_set_fmt(sd, state, &fmt);
}

static int imx135_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx135 *imx135 = container_of(ctrl->handler, struct imx135,
						     ctrls);
	int ret;

	if (!allow_streaming || !imx135_cf20_machine())
		return -EPERM;

	if (ctrl->id == V4L2_CID_VBLANK) {
		ret = imx135_update_exposure_range(imx135,
						   imx135->cur_mode->height + ctrl->val);
		if (ret)
			return ret;
	}

	ret = pm_runtime_get_if_in_use(imx135->dev);
	if (ret <= 0)
		return ret;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
	case V4L2_CID_ANALOGUE_GAIN:
	case V4L2_CID_VBLANK:
		ret = imx135_write_exposure_gain(imx135);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = cci_write(imx135->regmap, IMX135_REG_ORIENTATION,
				(imx135->hflip->val ? 1 : 0) |
				(imx135->vflip->val ? 2 : 0), NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx135->dev);
	return ret;
}

static int imx135_read_sensor_reg(struct imx135 *imx135, u32 reg, u64 *val);

/* Ghidra: the vendor readback path reads exposure from 0x0202 and analogue
 * gain from 0x0205.  Keep the public control surface limited to those
 * documented values; no arbitrary register access is exposed. */
static int imx135_get_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx135 *imx135 = container_of(ctrl->handler, struct imx135,
						     ctrls);
	u64 value;
	int ret;

	if (!allow_streaming || !imx135_cf20_machine())
		return -EPERM;

	ret = pm_runtime_get_if_in_use(imx135->dev);
	if (ret <= 0)
		return ret ? ret : -EACCES;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = imx135_read_sensor_reg(imx135, IMX135_REG_EXPOSURE, &value);
		if (!ret)
			ctrl->val = min_t(u64, value,
					 imx135->cur_mode->frame_length -
					 IMX135_EXPOSURE_OFFSET);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx135_read_sensor_reg(imx135, IMX135_REG_ANALOG_GAIN, &value);
		if (!ret)
			ctrl->val = min_t(u64, value, IMX135_ANALOG_GAIN_MAX);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx135->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx135_ctrl_ops = {
	.s_ctrl = imx135_set_ctrl,
	.g_volatile_ctrl = imx135_get_ctrl,
};

static int imx135_start_streaming(struct imx135 *imx135)
{
	int ret;

	ret = cci_write(imx135->regmap, IMX135_REG_SW_RESET, 0x01, NULL);
	if (ret)
		return ret;
	/* Vendor SensorReset holds reset for at least 1 ms. */
	usleep_range(1000, 2000);

	ret = cci_multi_reg_write(imx135->regmap, imx135_common_regs,
				  ARRAY_SIZE(imx135_common_regs), NULL);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(imx135->regmap, imx135->cur_mode->regs,
				  imx135->cur_mode->num_regs, NULL);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_handler_setup(&imx135->ctrls);
	if (ret)
		return ret;

	ret = cci_write(imx135->regmap, IMX135_REG_MODE_SELECT,
				IMX135_MODE_STREAMING, NULL);
	if (!ret)
		usleep_range(3000, 4000);

	return ret;
}

static int imx135_stop_streaming(struct imx135 *imx135)
{
	return cci_write(imx135->regmap, IMX135_REG_MODE_SELECT,
				IMX135_MODE_STANDBY, NULL);
}

static int imx135_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx135 *imx135 = to_imx135(sd);
	int ret;

	if (enable && (!allow_streaming || !imx135_cf20_machine()))
		return -EPERM;
	if (enable && !imx135->have_link_freq) {
		dev_err(imx135->dev,
			"streaming blocked: CSI-2 link frequency is not verified\n");
		return -ENODATA;
	}
	if (!enable && !imx135->streaming)
		return 0;

	if (enable) {
		ret = pm_runtime_resume_and_get(imx135->dev);
		if (ret < 0)
			return ret;

		ret = imx135_start_streaming(imx135);
		if (ret)
			pm_runtime_put(imx135->dev);
		else
			imx135->streaming = true;
		return ret;
	}

	ret = imx135_stop_streaming(imx135);
	imx135->streaming = false;
	pm_runtime_put(imx135->dev);
	return ret;
}

static const struct v4l2_subdev_video_ops imx135_video_ops = {
	.s_stream = imx135_s_stream,
};

static const struct v4l2_subdev_pad_ops imx135_pad_ops = {
	.enum_mbus_code = imx135_enum_mbus_code,
	.enum_frame_size = imx135_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx135_set_fmt,
};

static const struct v4l2_subdev_ops imx135_subdev_ops = {
	.video = &imx135_video_ops,
	.pad = &imx135_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx135_internal_ops = {
	.init_state = imx135_init_state,
};

static int imx135_power_on(struct device *dev)
{
	struct imx135 *imx135 = dev_get_drvdata(dev);
	unsigned int rail;
	unsigned int i;
	unsigned int enabled = 0;
	bool hclk_enabled = false;
	bool clk_prepared = false;
	bool sensor_gpios_touched = false;
	bool sensor_io_touched = false;
	bool quanta_gpio_touched = false;
	bool vendor_gpio_touched = false;
	bool sensor_path_touched = false;
	int ret;

	if (sensor_gpio_mode < 0 || sensor_gpio_mode > 2)
		return -EINVAL;
	if (sensor_path_mode < 0 || sensor_path_mode > 2)
		return -EINVAL;
	if (sensor_gpio_pre_power && sensor_gpio_mode != 1)
		return -EINVAL;

	/*
	 * Windows enables the CF-20 GPIO/power target before SensorPowerOn.
	 * Keep this opt-in and perform the exact vendor sequence on CAM0._CRS
	 * resource 2 (0x50), never on the AD5816 at 0x0e. The target is already
	 * registered write-free in probe().
	 */
	if (tca9535_mode && tca9535_before_power) {
		dev_info(imx135->dev,
			 "CF-20 power: enabling TCA9535 target before camera rails\n");
		/* Mark before the first write so partial sequences get cleanup. */
		imx135->tca9535_powered = true;
		ret = imx135_tca9535_power_on(imx135);
		if (ret)
			goto disable_daisy;
		/* Vendor FUN_1400069fc waits 20 ms after the four writes. */
		usleep_range(20000, 21000);
	}

	/*
	 * The Windows driver exposes SetGpio separately from SensorPowerOn.
	 * This opt-in path mirrors that separation: assert SGPO before the first
	 * rail/PLL operation, then leave it asserted for SensorPowerOn.
	 */
	if (sensor_gpio_pre_power) {
		dev_info(imx135->dev,
			 "CF-20 power: applying recovered SetGpio before camera rails\n");
		if (imx135->resetn_gpio)
			gpiod_set_value_cansleep(imx135->resetn_gpio, 1);
		usleep_range(1000, 2000);
		if (imx135->enable_gpio)
			gpiod_set_value_cansleep(imx135->enable_gpio, 1);
		sensor_gpios_touched = true;
	}

	/* Vendor IoActive precedes VoltageWF::PowerOn and enables both paths. */
	if (sensor_io_active_before_power) {
		dev_info(imx135->dev,
			 "CF-20 power: applying recovered IoActive before camera rails\n");
		ret = tps68470_cf20_set_sensor_io_active(imx135->dev, true);
		if (ret)
			goto disable_daisy;
		sensor_io_touched = true;
	}
	/*
	 * Ghidra recovered VoltageWF::Initialize's order as VA, VSIO, VD, then
	 * VCM. Set every recovered voltage before enabling any rail. Relying only
	 * on regulator init constraints leaves VSEL at reset on this ACPI path.
	 * This function is reached only after the DMI/allow_power_on safety gate.
	 */
	for (i = 0; i < ARRAY_SIZE(imx135_supply_names); i++) {
		rail = imx135_voltage_order[i];
		dev_info(imx135->dev,
			 "CF-20 power: setting rail %s to %u uV\n",
			 imx135_supply_names[rail], imx135_supply_voltages[rail]);
		ret = regulator_set_voltage(imx135->supplies[rail].consumer,
					    imx135_supply_voltages[rail],
					    imx135_supply_voltages[rail]);
		if (ret) {
			dev_err_probe(imx135->dev, ret,
				       "failed to set camera rail %s voltage\n",
				       imx135_supply_names[rail]);
			goto disable_daisy;
		}
	}

	/* Program PLL rate before power-on, but start output only after GPIO. */
	ret = clk_set_rate(imx135->xvclk, 24000000);
	if (ret) {
		dev_err_probe(imx135->dev, ret,
			       "failed to program sensor clock rate\n");
		goto disable_daisy;
	}

	/*
	 * Vendor SSCrdG2TiSensor::SensorPowerOn calls EnablePLL before
	 * enabling any camera rail.  clk_prepare_enable() emits the recovered
	 * StartClock register sequence and starts only the PMIC PLL; HCLK-A is
	 * switched to the sensor below, after rails and GPIOs are ready.
	 */
	dev_info(imx135->dev, "CF-20 power: starting sensor PLL before rails\n");
	ret = clk_prepare_enable(imx135->xvclk);
	if (ret) {
		dev_err_probe(imx135->dev, ret, "failed to prepare sensor clock\n");
		goto disable_daisy;
	}
	clk_prepared = true;

	/* Vendor order: IoActive/VSIO, then VA, VD, and VCM. */
	for (i = 0; i < ARRAY_SIZE(imx135_supply_names); i++) {
		rail = imx135_enable_order[i];
		dev_info(imx135->dev, "CF-20 power: enabling rail %s\n",
			 imx135_supply_names[rail]);
		ret = regulator_enable(imx135->supplies[rail].consumer);
		if (ret) {
			dev_err_probe(imx135->dev, ret,
				       "failed to enable camera rail %s\n",
				       imx135_supply_names[rail]);
			goto disable_daisy;
		}
		enabled++;
		dev_info(imx135->dev, "CF-20 power: rail %s enabled\n",
			 imx135_supply_names[rail]);
	}

	if (vendor_gpio_mode) {
		dev_info(imx135->dev,
			 "CF-20 power: enabling exact vendor GPIO path\n");
		ret = tps68470_cf20_set_vendor_gpio(imx135->dev, true);
		if (ret)
			goto disable_daisy;
		vendor_gpio_touched = true;
	}

	if (quanta_gpio_mode) {
		dev_info(imx135->dev,
			 "CF-20 power: enabling recovered Quanta GPIO path\n");
		ret = tps68470_cf20_set_quanta_gpio(imx135->dev, true);
		if (ret)
			goto disable_daisy;
		quanta_gpio_touched = true;
	}

	if (sensor_path_mode == 1) {
		dev_info(imx135->dev,
			 "CF-20 power: enabling sensor GPIO path before HCLK\n");
		ret = tps68470_cf20_set_sensor_gpio_path(imx135->dev, true);
		if (ret)
			goto disable_daisy;
		sensor_path_touched = true;
	}

	if (sensor_gpio_mode == 1 && !sensor_gpios_touched) {
		dev_info(imx135->dev,
			 "CF-20 power: asserting reset/enable GPIOs before HCLK\n");
		if (imx135->resetn_gpio)
			gpiod_set_value_cansleep(imx135->resetn_gpio, 1);
		/* Ghidra: vendor GpioOper(type 5) waits 1 ms between lines. */
		usleep_range(1000, 2000);
		if (imx135->enable_gpio)
			gpiod_set_value_cansleep(imx135->enable_gpio, 1);
		sensor_gpios_touched = true;
	} else if (sensor_gpio_mode == 1) {
		dev_info(imx135->dev,
			 "CF-20 power: recovered SetGpio already asserted before rails\n");
	} else {
		dev_info(imx135->dev,
			 "CF-20 power: leaving sensor GPIOs unchanged (mode=%d)\n",
			 sensor_gpio_mode);
	}

	/* Ghidra: SGPO/GPIO assertion precedes MclkOutput. */
	dev_info(imx135->dev, "CF-20 power: enabling 24 MHz clock output\n");
	ret = tps68470_cf20_set_sensor_clock(imx135->xvclk, true);
	if (ret) {
		dev_err_probe(imx135->dev, ret,
			       "failed to enable sensor clock output\n");
		goto disable_daisy;
	}
	hclk_enabled = true;

	/* Vendor SensorPowerOn waits 1 ms after MCLK output. */
	usleep_range(1000, 2000);
	/* Vendor enables the sensor I2C path, then waits 11 ms and 1 ms. */
	dev_info(imx135->dev, "CF-20 power: enabling I2C daisy chain\n");
	ret = tps68470_cf20_set_sensor_daisy_chain(imx135->dev, true);
	if (ret)
		goto disable_daisy;
	if (sensor_path_mode == 2) {
		dev_info(imx135->dev,
			 "CF-20 power: enabling sensor GPIO path after daisy\n");
		ret = tps68470_cf20_set_sensor_gpio_path(imx135->dev, true);
		if (ret)
			goto disable_daisy;
		sensor_path_touched = true;
	}
	/*
	 * Compatibility mode for the earlier Linux experiment: issue the target
	 * after the daisy-chain. The Windows order is selected explicitly with
	 * tca9535_before_power=1 above.
	 */
	if (tca9535_mode && !tca9535_before_power) {
		dev_info(imx135->dev,
			 "CF-20 power: enabling TCA9535 target after I2C daisy chain\n");
		/* Mark before first write so partial sequences get cleanup. */
		imx135->tca9535_powered = true;
		ret = imx135_tca9535_power_on(imx135);
		if (ret)
			goto disable_daisy;
		/* Vendor FUN_1400069fc waits 20 ms after the four writes. */
		usleep_range(20000, 21000);
	}
	usleep_range(11000, 12000);
	usleep_range(1000, 2000);
	if (sensor_gpio_mode == 2) {
		dev_info(imx135->dev,
			 "CF-20 power: asserting reset/enable GPIOs after daisy\n");
		if (imx135->resetn_gpio)
			gpiod_set_value_cansleep(imx135->resetn_gpio, 1);
		usleep_range(1000, 2000);
		if (imx135->enable_gpio)
			gpiod_set_value_cansleep(imx135->enable_gpio, 1);
		sensor_gpios_touched = true;
	}
	/*
	 * Ghidra: imx135.sys FUN_140004a84 waits 20 ms after the vendor
	 * SensorPowerOn call and before its CMOS/chip-ID check.  Keep this
	 * settle time outside the PMIC sequence; it is a sensor-readback
	 * guard, not another hardware write.
	 */
	dev_info(imx135->dev,
		 "CF-20 power: settling sensor 20 ms before identification\n");
	usleep_range(20000, 21000);
	/* Read-only diagnostic. Never fail power-on because of logging. */
	tps68470_cf20_log_sensor_state(imx135->dev);
	/*
	 * Windows performs these writes through Resource Hub channel 10. Linux
	 * uses the matching CAM0._CRS resource 2 (physical 0x50), never the
	 * logical _DSM value as an I2C address and never the AD5816 at 0x0e.
	 */
	dev_info(imx135->dev, "CF-20 power: sequence complete\n");
	return 0;

disable_daisy:
	/*
	 * The vendor target is downstream of the active camera topology.
	 * Tear it down before removing the daisy-chain, clock, or rails so a
	 * partial probe can still reach the target for its best-effort cleanup.
	 */
	if (imx135->tca9535_powered) {
		if (imx135_tca9535_power_off(imx135))
			dev_warn(imx135->dev,
				 "CF-20 TCA9535 power-off cleanup failed\n");
		imx135->tca9535_powered = false;
	}
	if (sensor_io_touched)
		tps68470_cf20_set_sensor_io_active(imx135->dev, false);
	if (sensor_path_touched) {
		tps68470_cf20_set_sensor_gpio_path(imx135->dev, false);
		sensor_path_touched = false;
	}
	tps68470_cf20_set_sensor_daisy_chain(imx135->dev, false);
	if (hclk_enabled) {
		tps68470_cf20_set_sensor_clock(imx135->xvclk, false);
		hclk_enabled = false;
	}
	if (quanta_gpio_touched) {
		tps68470_cf20_set_quanta_gpio(imx135->dev, false);
		quanta_gpio_touched = false;
	}
	if (vendor_gpio_touched) {
		tps68470_cf20_set_vendor_gpio(imx135->dev, false);
		vendor_gpio_touched = false;
	}
	if (sensor_path_touched) {
		tps68470_cf20_set_sensor_gpio_path(imx135->dev, false);
		sensor_path_touched = false;
	}
	if (hclk_enabled)
		tps68470_cf20_set_sensor_clock(imx135->xvclk, false);
	if (clk_prepared)
		clk_disable_unprepare(imx135->xvclk);
	if (sensor_gpios_touched) {
		if (imx135->resetn_gpio)
			gpiod_set_value_cansleep(imx135->resetn_gpio, 0);
		usleep_range(1000, 2000);
		if (imx135->enable_gpio)
			gpiod_set_value_cansleep(imx135->enable_gpio, 0);
	}

	while (enabled) {
		rail = imx135_enable_order[--enabled];
		regulator_disable(imx135->supplies[rail].consumer);
	}

	return ret;

}

static int imx135_power_off(struct device *dev)
{
	struct imx135 *imx135 = dev_get_drvdata(dev);
	unsigned int i;

	if (imx135->tca9535_powered) {
		int ret = imx135_tca9535_power_off(imx135);

		if (ret)
			dev_warn(imx135->dev,
				 "CF-20 TCA9535 power-off failed: %d\n", ret);
		imx135->tca9535_powered = false;
	}
	if (sensor_io_active_before_power)
		tps68470_cf20_set_sensor_io_active(imx135->dev, false);

	/* Reverse the proven TPS68470 order before removing the rails. */
	if (sensor_path_mode != 0)
		tps68470_cf20_set_sensor_gpio_path(imx135->dev, false);
	tps68470_cf20_set_sensor_daisy_chain(imx135->dev, false);
	tps68470_cf20_set_sensor_clock(imx135->xvclk, false);
	clk_disable_unprepare(imx135->xvclk);
	if (quanta_gpio_mode) {
		/* Vendor SensorPowerOff waits 4 ms before Quanta GPIO off. */
		usleep_range(4000, 5000);
		tps68470_cf20_set_quanta_gpio(imx135->dev, false);
	}
	if (vendor_gpio_mode) {
		usleep_range(4000, 5000);
		tps68470_cf20_set_vendor_gpio(imx135->dev, false);
	}
	if (sensor_gpio_mode != 0) {
		/* Vendor SensorPowerOff waits 4 ms before disabling GPIOs. */
		usleep_range(4000, 5000);
		if (imx135->resetn_gpio)
			gpiod_set_value_cansleep(imx135->resetn_gpio, 0);
		/* Ghidra: vendor GpioOper waits 1 ms before clearing enable. */
		usleep_range(1000, 2000);
		if (imx135->enable_gpio)
			gpiod_set_value_cansleep(imx135->enable_gpio, 0);
	}
	/* Reverse SensorPowerOn: IoActive/VSIO, VCM, VD, then VA. */
	for (i = ARRAY_SIZE(imx135_supply_names); i; i--)
		regulator_disable(imx135->supplies[imx135_enable_order[i - 1]].consumer);
	return 0;
}

static const struct dev_pm_ops imx135_pm_ops = {
	SET_RUNTIME_PM_OPS(imx135_power_off, imx135_power_on, NULL)
};

static struct fwnode_handle *imx135_get_endpoint(struct device *dev)
{
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct fwnode_handle *ep;

	if (IS_ERR_OR_NULL(fwnode))
		return NULL;

	/*
	 * ACPI camera devices on this platform carry the IPU bridge graph as
	 * a secondary software fwnode.  The primary ACPI fwnode has no graph
	 * endpoint of its own.  Search both layers; do not invent a topology.
	 */
	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (ep)
		return ep;

	/*
	 * A CF-20 PMIC/ACPI module reload can detach the bridge's secondary
	 * graph while leaving CAM0 client registered.  Restore only that
	 * software graph before rejecting the sensor.  No hardware access occurs.
	 */
	if (imx135_cf20_machine() &&
	    !ipu_bridge_reconnect_cf20_sensor_graph(dev))
		return fwnode_graph_get_next_endpoint(fwnode->secondary, NULL);

	if (!fwnode->secondary)
		return NULL;

	return fwnode_graph_get_next_endpoint(fwnode->secondary, NULL);
}

static int imx135_parse_hw_config(struct imx135 *imx135)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	unsigned int i;
	int ret;

	imx135->xvclk = devm_v4l2_sensor_clk_get(imx135->dev, NULL);
	if (IS_ERR(imx135->xvclk))
		return dev_err_probe(imx135->dev, PTR_ERR(imx135->xvclk),
				     "failed to get sensor clock\n");

	if (clk_get_rate(imx135->xvclk) != IMX135_XVCLK_FREQ)
		return dev_err_probe(imx135->dev, -EINVAL,
				     "sensor clock is not 24 MHz\n");

	ep = imx135_get_endpoint(imx135->dev);
	if (!ep)
		return dev_err_probe(imx135->dev, -EINVAL,
				     "CSI-2 endpoint not found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(imx135->dev, ret,
				     "failed to parse CSI-2 endpoint\n");

	imx135->num_lanes = bus_cfg.bus.mipi_csi2.num_data_lanes;
	if (imx135->num_lanes != IMX135_NUM_LANES) {
		dev_err(imx135->dev, "requires four CSI-2 lanes, got %u\n",
			imx135->num_lanes);
		ret = -EINVAL;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++) {
		dev_dbg(imx135->dev, "firmware link frequency %llu Hz\n",
			bus_cfg.link_frequencies[i]);
		if (bus_cfg.link_frequencies[i] == IMX135_LINK_FREQ)
			imx135->have_link_freq = true;
	}
	if (!imx135->have_link_freq) {
		dev_err(imx135->dev,
			"firmware does not advertise the required 450 MHz link frequency\n");
		ret = -EINVAL;
	}
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int imx135_init_controls(struct imx135 *imx135)
{
	const struct imx135_mode *mode = imx135->cur_mode;
	struct v4l2_ctrl_handler *hdl = &imx135->ctrls;
	int ret;

	ret = v4l2_ctrl_handler_init(hdl, 8);
	if (ret)
		return ret;

	imx135->pixel_rate = v4l2_ctrl_new_std(hdl, NULL,
					       V4L2_CID_PIXEL_RATE,
					       IMX135_PIXEL_RATE,
					       IMX135_PIXEL_RATE, 1,
					       IMX135_PIXEL_RATE);
	if (imx135->pixel_rate)
		imx135->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx135->link_freq = v4l2_ctrl_new_int_menu(hdl, NULL,
						 V4L2_CID_LINK_FREQ, 0, 0,
						 imx135_link_freq_menu);
	if (imx135->link_freq)
		imx135->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx135->vblank = v4l2_ctrl_new_std(hdl, &imx135_ctrl_ops,
					V4L2_CID_VBLANK, 0,
					0xffff - mode->height, 1,
					mode->frame_length - mode->height);
	imx135->exposure = v4l2_ctrl_new_std(hdl, &imx135_ctrl_ops,
					 V4L2_CID_EXPOSURE, 1,
					 mode->frame_length - IMX135_EXPOSURE_OFFSET,
					 1, mode->frame_length - IMX135_EXPOSURE_OFFSET);
	if (imx135->exposure)
		imx135->exposure->flags |= V4L2_CTRL_FLAG_VOLATILE;
	imx135->analogue_gain = v4l2_ctrl_new_std(hdl, &imx135_ctrl_ops,
						  V4L2_CID_ANALOGUE_GAIN, 0,
						  IMX135_ANALOG_GAIN_MAX, 1, 0);
	if (imx135->analogue_gain)
		imx135->analogue_gain->flags |= V4L2_CTRL_FLAG_VOLATILE;
	imx135->hflip = v4l2_ctrl_new_std(hdl, &imx135_ctrl_ops,
					 V4L2_CID_HFLIP, 0, 1, 1, 0);
	imx135->vflip = v4l2_ctrl_new_std(hdl, &imx135_ctrl_ops,
					 V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_cluster(2, &imx135->hflip);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	imx135->sd.ctrl_handler = hdl;
	return 0;
}

/*
 * The Panasonic Windows driver performs a register-address write and the
 * following read as two separate I2C transfers.  The normal path below keeps
 * that vendor-compatible STOP-separated behavior.  combined_read is an
 * explicit, read-only A/B diagnostic for hardware that requires a repeated
 * START between the address phase and the data phase.
 *
 * This helper only writes the register address pointer and then reads data;
 * it never writes a sensor register value.
 */
static int imx135_read_sensor_reg(struct imx135 *imx135, u32 reg, u64 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	struct i2c_msg msgs[2];
	u8 address[2];
	u8 data[8] = { 0 };
	unsigned int len = CCI_REG_WIDTH_BYTES(reg);
	unsigned int i;
	unsigned int attempt;
	int ret;

	if (!client || !val || !len || len > sizeof(data))
		return -EINVAL;

	put_unaligned_be16(CCI_REG_ADDR(reg), address);
	for (attempt = 0; attempt < 2; attempt++) {
		if (combined_read) {
			msgs[0].addr = client->addr;
			msgs[0].flags = 0;
			msgs[0].len = sizeof(address);
			msgs[0].buf = address;
			msgs[1].addr = client->addr;
			msgs[1].flags = I2C_M_RD;
			msgs[1].len = len;
			msgs[1].buf = data;

			ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
			if (diagnostic)
				dev_info(imx135->dev,
					 "CF-20 I2C diagnostic: protocol=combined/write16BE+repeated-start/read reg=0x%04x transfer-ret=%d expected=%zu data=%*ph\n",
					 (unsigned int)CCI_REG_ADDR(reg), ret,
					 ARRAY_SIZE(msgs), len, data);
			if (ret == ARRAY_SIZE(msgs))
				break;
		} else {
			ret = i2c_master_send(client, address, sizeof(address));
			if (diagnostic)
				dev_info(imx135->dev,
					 "CF-20 I2C diagnostic: reg=0x%04x address-phase ret=%d expected=%zu\n",
					 (unsigned int)CCI_REG_ADDR(reg), ret, sizeof(address));
			if (ret != sizeof(address))
				goto retry_or_fail;

			ret = i2c_master_recv(client, data, len);
			if (diagnostic)
				dev_info(imx135->dev,
					 "CF-20 I2C diagnostic: reg=0x%04x read-phase ret=%d expected=%u data=%*ph\n",
					 (unsigned int)CCI_REG_ADDR(reg), ret, len, len, data);
			if (ret == len)
				break;
		}

	retry_or_fail:
		if (attempt == 0 && ret < 0) {
			dev_info(imx135->dev,
				 "CF-20 I2C read error %d; retrying after 2 ms\n", ret);
			usleep_range(2000, 3000);
			continue;
		}
		return ret < 0 ? ret : -EIO;
	}

	*val = 0;
	if (reg & CCI_REG_LE) {
		for (i = len; i; i--)
			*val = (*val << 8) | data[i - 1];
	} else {
		for (i = 0; i < len; i++)
			*val = (*val << 8) | data[i];
	}

	return 0;
}

static void imx135_log_i2c_path(struct imx135 *imx135)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx135->sd);
	unsigned long functionality;

	if (!diagnostic || !client || !client->adapter)
		return;

	functionality = i2c_get_functionality(client->adapter);
	dev_info(imx135->dev,
		 "CF-20 I2C diagnostic: adapter=%s addr=0x%02x functionality=0x%lx raw-i2c=%s protocol=%s\n",
		 client->adapter->name, client->addr, functionality,
		 (functionality & I2C_FUNC_I2C) ? "yes" : "no",
		 combined_read ? "combined/write16BE+repeated-start/read" :
		 "STOP/write16BE+STOP/read");
}

static int imx135_read_chip_id(struct imx135 *imx135, u64 *id)
{
	int ret;

	ret = imx135_read_sensor_reg(imx135, IMX135_REG_CHIP_ID, id);
	if (ret)
		return dev_err_probe(imx135->dev, ret,
				     "chip ID read failed at I2C address 0x10\n");
	return 0;
}

static void imx135_log_probe_register(struct imx135 *imx135, u16 reg,
					      const char *name)
{
	u64 value;
	int ret;

	/* Read-only diagnostics. Never alter sensor or PMIC state here. */
	ret = imx135_read_sensor_reg(imx135, CCI_REG16(reg), &value);
	if (ret)
		dev_info(imx135->dev,
			 "CF-20 probe read %s reg 0x%04x failed: %d\n",
			 name, reg, ret);
	else
		dev_info(imx135->dev,
			 "CF-20 probe read %s reg 0x%04x = 0x%04llx\n",
			 name, reg, value);
}

static void imx135_log_probe_registers(struct imx135 *imx135)
{
	imx135_log_probe_register(imx135, 0x0000, "model-high");
	imx135_log_probe_register(imx135, 0x0001, "model-low");
	imx135_log_probe_register(imx135, 0x0016, "chip-id");
	imx135_log_probe_register(imx135, 0x0100, "mode-select");
}

static int imx135_identify(struct imx135 *imx135)
{
	u64 id;
	int ret;

	ret = imx135_read_chip_id(imx135, &id);
	if (ret)
		return ret;

	if (id == IMX135_CHIP_ID)
		goto detected;

	dev_err(imx135->dev, "chip id mismatch: expected 0x%04x, got 0x%04llx\n",
		IMX135_CHIP_ID, id);
	imx135_log_probe_registers(imx135);

	/* Ghidra: vendor startup retries with an empty callback on 0x0000. */
	return -ENODEV;

detected:
	dev_info(imx135->dev, "Sony IMX135 detected\n");
	return 0;
}

/*
 * CAM0._STA is controlled by the CF-20 firmware camera gate (SCSS).  The
 * camera client can still be created by the optional Linux IPU bridge
 * enumeration path while _STA is zero.  That client is not permission to
 * power hardware: with the firmware gate closed, refuse before registering
 * the downstream target or touching any rail, clock, GPIO, or PMIC bit.
 */
static int imx135_require_acpi_camera_enabled(struct device *dev)
{
	struct acpi_device *adev = ACPI_COMPANION(dev);
	int ret;

	if (!adev)
		return -ENODEV;

	ret = acpi_bus_get_status(adev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "CF-20 ACPI camera status unavailable; hardware access blocked\n");

	if (!adev->status.present || !adev->status.enabled) {
		if (!allow_cf20_acpi_disabled)
			return dev_err_probe(dev, -EPERM,
					     "CF-20 camera disabled by firmware (_STA=0); enable Camera (Rear) in BIOS\n");

		dev_warn(dev,
			 "CF-20 firmware reports CAM0._STA=0; explicit diagnostic override enabled\n");
	}

	return 0;
}

static int imx135_probe(struct i2c_client *client)
{
	struct imx135 *imx135;
	unsigned int i;
	int ret;

	if (client->addr != 0x10)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unsupported IMX135 I2C address 0x%02x\n",
				     client->addr);

	dev_info(&client->dev, "CF-20 IMX135 probe entered\n");

	imx135 = devm_kzalloc(&client->dev, sizeof(*imx135), GFP_KERNEL);
	if (!imx135)
		return -ENOMEM;

	imx135->dev = &client->dev;
	for (i = 0; i < ARRAY_SIZE(imx135_supply_names); i++)
		imx135->supplies[i].supply = imx135_supply_names[i];

	ret = devm_regulator_bulk_get(imx135->dev,
				      ARRAY_SIZE(imx135_supply_names),
				      imx135->supplies);
	if (ret)
		return dev_err_probe(imx135->dev, ret,
				     "failed to get camera regulators\n");

	/* Probe must not change GPIO state while the safety gate is closed. */
	imx135->enable_gpio = devm_gpiod_get_optional(imx135->dev, "enable",
						     GPIOD_ASIS);
	if (IS_ERR(imx135->enable_gpio))
		return dev_err_probe(imx135->dev, PTR_ERR(imx135->enable_gpio),
				     "failed to get enable GPIO\n");

	imx135->resetn_gpio = devm_gpiod_get_optional(imx135->dev, "resetn",
						      GPIOD_ASIS);
	if (IS_ERR(imx135->resetn_gpio))
		return dev_err_probe(imx135->dev, PTR_ERR(imx135->resetn_gpio),
				     "failed to get resetn GPIO\n");

	imx135->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx135->regmap))
		return dev_err_probe(imx135->dev, PTR_ERR(imx135->regmap),
				     "failed to initialize CCI\n");

	v4l2_i2c_subdev_init(&imx135->sd, client, &imx135_subdev_ops);
	imx135->sd.internal_ops = &imx135_internal_ops;
	dev_set_drvdata(imx135->dev, imx135);

	ret = imx135_parse_hw_config(imx135);
	if (ret)
		return ret;

	imx135->cur_mode = &imx135_modes[0];
	if (!allow_power_on || !imx135_cf20_machine()) {
		dev_info(imx135->dev,
			 "safe mode: CF-20 DMI and allow_power_on=1 required\n");
		return -EPERM;
	}
	ret = imx135_require_acpi_camera_enabled(imx135->dev);
	if (ret)
		return ret;
	ret = imx135_tca9535_register(imx135);
	if (ret)
		return ret;
	if (sensor_gpio_mode != 0 &&
	    (!imx135->enable_gpio || !imx135->resetn_gpio))
		goto unregister_tca9535;

	ret = imx135_power_on(imx135->dev);
	if (ret)
		goto unregister_tca9535;

	dev_info(imx135->dev, "CF-20 sensor: reading chip ID\n");
	imx135_log_i2c_path(imx135);
	ret = imx135_identify(imx135);
	if (ret)
		goto power_off;

	ret = imx135_init_controls(imx135);
	if (ret)
		goto power_off;

	imx135->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx135->pad.flags = MEDIA_PAD_FL_SOURCE;
	imx135->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&imx135->sd.entity, 1, &imx135->pad);
	if (ret)
		goto free_ctrls;

	imx135->sd.state_lock = imx135->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&imx135->sd);
	if (ret)
		goto cleanup_entity;

	pm_runtime_set_active(imx135->dev);
	pm_runtime_enable(imx135->dev);

	ret = v4l2_async_register_subdev_sensor(&imx135->sd);
	if (ret)
		goto disable_pm;

	pm_runtime_idle(imx135->dev);
	return 0;

disable_pm:
	pm_runtime_disable(imx135->dev);
	v4l2_subdev_cleanup(&imx135->sd);
cleanup_entity:
	media_entity_cleanup(&imx135->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&imx135->ctrls);
power_off:
	imx135_power_off(imx135->dev);
	unregister_tca9535:
	if (sensor_gpio_mode != 0 &&
	    (!imx135->enable_gpio || !imx135->resetn_gpio))
		ret = -ENODEV;
	if (ret)
		dev_err_probe(imx135->dev, ret,
			      "CF-20 sensor power-on/setup failed\n");
	imx135_tca9535_unregister(imx135);
	return ret;
}

static void imx135_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx135 *imx135 = to_imx135(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&imx135->ctrls);
	pm_runtime_disable(imx135->dev);
	if (!pm_runtime_status_suspended(imx135->dev))
		imx135_power_off(imx135->dev);
	imx135_tca9535_unregister(imx135);
}

static const struct of_device_id imx135_of_match[] = {
	{ .compatible = "sony,imx135" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx135_of_match);

static const struct acpi_device_id imx135_acpi_match[] = {
	{ "INT3471" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, imx135_acpi_match);

/*
 * CAM0 is created explicitly from the hidden ACPI node.  That node is not
 * necessarily the primary physical ACPI node, so ACPI matching alone is not
 * sufficient for the resulting I2C client.  Keep the normal I2C name match
 * as the safe, bus-local fallback.
 */
static const struct i2c_device_id imx135_i2c_match[] = {
	{ "imx135", 0 },
	{ "INT3471:00", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, imx135_i2c_match);

static struct i2c_driver imx135_i2c_driver = {
	.driver = {
		.name = "imx135",
		.of_match_table = imx135_of_match,
		.acpi_match_table = imx135_acpi_match,
		.pm = &imx135_pm_ops,
	},
	.flags = I2C_DRV_ACPI_WAIVE_D0_PROBE,
	.probe = imx135_probe,
	.remove = imx135_remove,
	.id_table = imx135_i2c_match,
};

module_i2c_driver(imx135_i2c_driver);

MODULE_DESCRIPTION("Sony IMX135 camera sensor driver");
MODULE_AUTHOR("CF-20 reverse engineering project");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("INTEL_IPU_BRIDGE");
MODULE_SOFTDEP("pre: intel_skl_int3472_tps68470");
