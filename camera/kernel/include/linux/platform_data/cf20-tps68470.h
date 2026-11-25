/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PDATA_CF20_TPS68470_H
#define __PDATA_CF20_TPS68470_H

#include <linux/types.h>
#include <linux/platform_data/tps68470.h>

struct device;
struct clk;
struct regulator_init_data;

/*
 * The 7.3.x Debian TPS68470 header does not carry the CF-20 split-S_I2C
 * flag.  Keep the board-data extension private instead of shadowing the
 * upstream header; all board-data objects in this driver use this layout.
 */
struct tps68470_regulator_platform_data_ext {
	const struct regulator_init_data *reg_init_data[TPS68470_NUM_REGULATORS];
	bool cf20_split_s_i2c;
};

/*
 * Keep CF-20 clock metadata separate from the upstream structure.  Debian
 * 7.3.x carries the upstream tps68470_clk_platform_data without the two
 * board-specific fields used by this driver; using an extended private type
 * keeps the platform-data layout identical on both kernel generations.
 */
struct cf20_tps68470_clk_platform_data {
	unsigned long rate;
	unsigned int vendor_cf20_config;
	unsigned int n_consumers;
	struct tps68470_clk_consumer consumers[];
};

int tps68470_cf20_set_sensor_daisy_chain(struct device *sensor, bool enable);
int tps68470_cf20_set_sensor_io_active(struct device *sensor, bool enable);
int tps68470_cf20_set_vendor_gpio(struct device *sensor, bool enable);
int tps68470_cf20_set_quanta_gpio(struct device *sensor, bool enable);
int tps68470_cf20_set_sensor_gpio_path(struct device *sensor, bool enable);
int tps68470_cf20_log_sensor_state(struct device *sensor);
int tps68470_cf20_sensor_power_valid(struct device *sensor);
int tps68470_cf20_restore_sensor_rails(struct device *sensor);
int tps68470_cf20_set_sensor_clock(struct clk *clk, bool enable);

#endif
