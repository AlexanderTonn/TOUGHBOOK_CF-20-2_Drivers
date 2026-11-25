/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TI TPS68470 PMIC platform data definition.
 *
 * Copyright (c) 2021 Red Hat Inc.
 *
 * Red Hat authors:
 * Hans de Goede <hdegoede@redhat.com>
 */
#ifndef __PDATA_TPS68470_H
#define __PDATA_TPS68470_H

#include <linux/types.h>

struct device;

enum tps68470_regulators {
	TPS68470_CORE,
	TPS68470_ANA,
	TPS68470_VCM,
	TPS68470_VIO,
	TPS68470_VSIO,
	TPS68470_AUX1,
	TPS68470_AUX2,
	TPS68470_NUM_REGULATORS
};

struct regulator_init_data;

struct tps68470_regulator_platform_data {
	const struct regulator_init_data *reg_init_data[TPS68470_NUM_REGULATORS];
	/* CF-20 needs S_I2C_CTL bit 1 before bit 0 during sensor power-on. */
	bool cf20_split_s_i2c;
};

struct tps68470_clk_consumer {
	const char *consumer_dev_name;
	const char *consumer_con_id;
};

struct tps68470_clk_platform_data {
	/* Optional board-specific output rate; zero keeps the generic default. */
	unsigned long rate;
	/* Use Panasonic CF-20 vendor PLL/divider values for 24 MHz output. */
	unsigned int vendor_cf20_config;
	unsigned int n_consumers;
	struct tps68470_clk_consumer consumers[];
};

int tps68470_cf20_set_sensor_daisy_chain(struct device *sensor, bool enable);

#endif
