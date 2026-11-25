// SPDX-License-Identifier: GPL-2.0
/*
 * TI TPS68470 PMIC platform data definition.
 *
 * Copyright (c) 2021 Dan Scally <djrscally@gmail.com>
 * Copyright (c) 2021 Red Hat Inc.
 *
 * Red Hat authors:
 * Hans de Goede <hdegoede@redhat.com>
 */

#include <linux/dmi.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/platform_data/tps68470.h>
#include <linux/property.h>
#include <linux/regulator/machine.h>
#include <linux/string.h>
#include "tps68470.h"

/*
 * CF-20 board data is opt-in.  The rail mapping is reverse engineered and
 * must not be enabled merely by loading the generic INT3472 driver.
 */
static bool allow_cf20_camera_power;
module_param_named(allow_cf20_camera_power, allow_cf20_camera_power, bool, 0444);
MODULE_PARM_DESC(allow_cf20_camera_power,
		 "Enable reverse-engineered Panasonic CF-20 TPS68470 camera rails");

static struct regulator_consumer_supply int347a_core_consumer_supplies[] = {
	REGULATOR_SUPPLY("dvdd", "i2c-INT347A:00"),
};

static struct regulator_consumer_supply int347a_ana_consumer_supplies[] = {
	REGULATOR_SUPPLY("avdd", "i2c-INT347A:00"),
};

static struct regulator_consumer_supply int347a_vcm_consumer_supplies[] = {
	REGULATOR_SUPPLY("vdd", "i2c-INT347A:00-VCM"),
};

static struct regulator_consumer_supply int347a_vsio_consumer_supplies[] = {
	REGULATOR_SUPPLY("dovdd", "i2c-INT347A:00"),
	REGULATOR_SUPPLY("vsio", "i2c-INT347A:00-VCM"),
	REGULATOR_SUPPLY("vddd", "i2c-INT347E:00"),
};

static struct regulator_consumer_supply int347a_aux1_consumer_supplies[] = {
	REGULATOR_SUPPLY("vdda", "i2c-INT347E:00"),
};

static struct regulator_consumer_supply int347a_aux2_consumer_supplies[] = {
	REGULATOR_SUPPLY("vdddo", "i2c-INT347E:00"),
};

static const struct regulator_init_data surface_go_tps68470_core_reg_init_data = {
	.constraints = {
		.min_uV = 1200000,
		.max_uV = 1200000,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int347a_core_consumer_supplies),
	.consumer_supplies = int347a_core_consumer_supplies,
};

static const struct regulator_init_data surface_go_tps68470_ana_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int347a_ana_consumer_supplies),
	.consumer_supplies = int347a_ana_consumer_supplies,
};

static const struct regulator_init_data surface_go_tps68470_vcm_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int347a_vcm_consumer_supplies),
	.consumer_supplies = int347a_vcm_consumer_supplies,
};

/* Ensure the always-on VIO regulator has the same voltage as VSIO */
static const struct regulator_init_data surface_go_tps68470_vio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = true,
		.always_on = true,
	},
};

static const struct regulator_init_data surface_go_tps68470_vsio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int347a_vsio_consumer_supplies),
	.consumer_supplies = int347a_vsio_consumer_supplies,
};

static const struct regulator_init_data surface_go_tps68470_aux1_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int347a_aux1_consumer_supplies),
	.consumer_supplies = int347a_aux1_consumer_supplies,
};

static const struct regulator_init_data surface_go_tps68470_aux2_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int347a_aux2_consumer_supplies),
	.consumer_supplies = int347a_aux2_consumer_supplies,
};

static const struct tps68470_regulator_platform_data_ext surface_go_tps68470_pdata = {
	.reg_init_data = {
		[TPS68470_CORE] = &surface_go_tps68470_core_reg_init_data,
		[TPS68470_ANA]  = &surface_go_tps68470_ana_reg_init_data,
		[TPS68470_VCM]  = &surface_go_tps68470_vcm_reg_init_data,
		[TPS68470_VIO] = &surface_go_tps68470_vio_reg_init_data,
		[TPS68470_VSIO] = &surface_go_tps68470_vsio_reg_init_data,
		[TPS68470_AUX1] = &surface_go_tps68470_aux1_reg_init_data,
		[TPS68470_AUX2] = &surface_go_tps68470_aux2_reg_init_data,
	},
};

/* Settings for Dell 7212 Tablet */

static struct regulator_consumer_supply int3479_vsio_consumer_supplies[] = {
	REGULATOR_SUPPLY("avdd", "i2c-INT3479:00"),
};

static struct regulator_consumer_supply int3479_aux1_consumer_supplies[] = {
	REGULATOR_SUPPLY("dvdd", "i2c-INT3479:00"),
};

static struct regulator_consumer_supply int3479_aux2_consumer_supplies[] = {
	REGULATOR_SUPPLY("dovdd", "i2c-INT3479:00"),
};

static const struct regulator_init_data dell_7212_tps68470_core_reg_init_data = {
	.constraints = {
		.min_uV = 1200000,
		.max_uV = 1200000,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static const struct regulator_init_data dell_7212_tps68470_ana_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static const struct regulator_init_data dell_7212_tps68470_vcm_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static const struct regulator_init_data dell_7212_tps68470_vio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static const struct regulator_init_data dell_7212_tps68470_vsio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int3479_vsio_consumer_supplies),
	.consumer_supplies = int3479_vsio_consumer_supplies,
};

static const struct regulator_init_data dell_7212_tps68470_aux1_reg_init_data = {
	.constraints = {
		.min_uV = 1213200,
		.max_uV = 1213200,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int3479_aux1_consumer_supplies),
	.consumer_supplies = int3479_aux1_consumer_supplies,
};

static const struct regulator_init_data dell_7212_tps68470_aux2_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(int3479_aux2_consumer_supplies),
	.consumer_supplies = int3479_aux2_consumer_supplies,
};

static const struct tps68470_regulator_platform_data_ext dell_7212_tps68470_pdata = {
	.reg_init_data = {
		[TPS68470_CORE] = &dell_7212_tps68470_core_reg_init_data,
		[TPS68470_ANA]  = &dell_7212_tps68470_ana_reg_init_data,
		[TPS68470_VCM]  = &dell_7212_tps68470_vcm_reg_init_data,
		[TPS68470_VIO]  = &dell_7212_tps68470_vio_reg_init_data,
		[TPS68470_VSIO] = &dell_7212_tps68470_vsio_reg_init_data,
		[TPS68470_AUX1] = &dell_7212_tps68470_aux1_reg_init_data,
		[TPS68470_AUX2] = &dell_7212_tps68470_aux2_reg_init_data,
	},
};

/* Settings for MSI Prestige 14 AI+ Evo C2VMG laptop. */
static struct regulator_consumer_supply ovti5675_avdd_consumer_supplies[] = {
	REGULATOR_SUPPLY("avdd", "i2c-OVTI5675:00"),
};

static struct regulator_consumer_supply ovti5675_dovdd_consumer_supplies[] = {
	REGULATOR_SUPPLY("dovdd", "i2c-OVTI5675:00"),
};

static struct regulator_consumer_supply ovti5675_dvdd_consumer_supplies[] = {
	REGULATOR_SUPPLY("dvdd", "i2c-OVTI5675:00"),
};

static const struct regulator_init_data msi_prestige_ai_evo_tps68470_core_reg_init_data = {
	.constraints = {
		.min_uV = 1200000,
		.max_uV = 1200000,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(ovti5675_dvdd_consumer_supplies),
	.consumer_supplies = ovti5675_dvdd_consumer_supplies,
};

static const struct regulator_init_data msi_prestige_ai_evo_tps68470_ana_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(ovti5675_avdd_consumer_supplies),
	.consumer_supplies = ovti5675_avdd_consumer_supplies,
};

static const struct regulator_init_data msi_prestige_ai_evo_tps68470_vio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static const struct regulator_init_data msi_prestige_ai_evo_tps68470_vsio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(ovti5675_dovdd_consumer_supplies),
	.consumer_supplies = ovti5675_dovdd_consumer_supplies,
};

static const struct tps68470_regulator_platform_data_ext msi_prestige_ai_evo_tps68470_pdata = {
	.reg_init_data = {
		[TPS68470_CORE] = &msi_prestige_ai_evo_tps68470_core_reg_init_data,
		[TPS68470_ANA]  = &msi_prestige_ai_evo_tps68470_ana_reg_init_data,
		[TPS68470_VIO]  = &msi_prestige_ai_evo_tps68470_vio_reg_init_data,
		[TPS68470_VSIO] = &msi_prestige_ai_evo_tps68470_vsio_reg_init_data,
	},
};

/* Settings for Intel NVL platform */

static struct regulator_consumer_supply ovti13b1_core_consumer_supplies[] = {
	REGULATOR_SUPPLY("dvdd", "i2c-OVTI13B1:01"),
};

static struct regulator_consumer_supply ovti13b1_ana_consumer_supplies[] = {
	REGULATOR_SUPPLY("avdd", "i2c-OVTI13B1:01"),
};

static struct regulator_consumer_supply ovti13b1_vcm_consumer_supplies[] = {
	REGULATOR_SUPPLY("vcc", "i2c-OVTI13B1:01-VCM"),
};

static struct regulator_consumer_supply ovti13b1_vsio_consumer_supplies[] = {
	REGULATOR_SUPPLY("dovdd", "i2c-OVTI13B1:01"),
};

static const struct regulator_init_data intel_nvl_tps68470_core_reg_init_data = {
		.constraints = {
		.min_uV = 1200000,
		.max_uV = 1200000,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(ovti13b1_core_consumer_supplies),
	.consumer_supplies = ovti13b1_core_consumer_supplies,
};

static const struct regulator_init_data intel_nvl_tps68470_ana_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(ovti13b1_ana_consumer_supplies),
	.consumer_supplies = ovti13b1_ana_consumer_supplies,
};
static const struct regulator_init_data intel_nvl_tps68470_vcm_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(ovti13b1_vcm_consumer_supplies),
	.consumer_supplies = ovti13b1_vcm_consumer_supplies,
};

/* Ensure the always-on VIO regulator has the same voltage as VSIO */
static const struct regulator_init_data intel_nvl_tps68470_vio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = true,
		.always_on = true,
	},
};
static const struct regulator_init_data intel_nvl_tps68470_vsio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = ARRAY_SIZE(ovti13b1_vsio_consumer_supplies),
	.consumer_supplies = ovti13b1_vsio_consumer_supplies,
};

static const struct tps68470_regulator_platform_data_ext intel_nvl_tps68470_pdata = {
	.reg_init_data = {
		[TPS68470_CORE] = &intel_nvl_tps68470_core_reg_init_data,
		[TPS68470_ANA]  = &intel_nvl_tps68470_ana_reg_init_data,
		[TPS68470_VCM]  = &intel_nvl_tps68470_vcm_reg_init_data,
		[TPS68470_VIO] = &intel_nvl_tps68470_vio_reg_init_data,
		[TPS68470_VSIO] = &intel_nvl_tps68470_vsio_reg_init_data,
	},
};

static struct gpiod_lookup_table surface_go_int347a_gpios = {
	.dev_id = "i2c-INT347A:00",
	.table = {
		GPIO_LOOKUP("tps68470-gpio", 9, "reset", GPIO_ACTIVE_LOW),
		GPIO_LOOKUP("tps68470-gpio", 7, "powerdown", GPIO_ACTIVE_LOW),
		{ }
	}
};

static struct gpiod_lookup_table surface_go_int347e_gpios = {
	.dev_id = "i2c-INT347E:00",
	.table = {
		GPIO_LOOKUP("tps68470-gpio", 5, "enable", GPIO_ACTIVE_HIGH),
		{ }
	}
};

static struct gpiod_lookup_table dell_7212_int3479_gpios = {
	.dev_id = "i2c-INT3479:00",
	.table = {
		GPIO_LOOKUP("tps68470-gpio", 3, "reset", GPIO_ACTIVE_LOW),
		GPIO_LOOKUP("tps68470-gpio", 4, "powerdown", GPIO_ACTIVE_LOW),
		{ }
	}
};

static struct gpiod_lookup_table msi_prestige_ai_evo_ovti5675_gpios = {
	.dev_id = "i2c-OVTI5675:00",
	.table = {
		GPIO_LOOKUP("tps68470-gpio", 9, "reset", GPIO_ACTIVE_LOW),
		{ }
	}
};

static struct gpiod_lookup_table intel_nvl_tps68470_gpios = {
	.dev_id = "i2c-OVTI13B1:01",
	.table = {
		GPIO_LOOKUP("tps68470-gpio", 9, "reset", GPIO_ACTIVE_LOW),
		{ }
	}
};

static const struct property_entry int3472_tps68470_daisy_chain_gpio_props[] = {
	PROPERTY_ENTRY_BOOL("daisy-chain-enable"),
	{ }
};

static const struct software_node int3472_tps68470_daisy_chain_gpio_swnode = {
	.properties = int3472_tps68470_daisy_chain_gpio_props,
};

static struct gpiod_lookup_table cf20_tps68470_gpios = {
	.dev_id = "i2c-INT3471:00",
	.table = {
		/* Vendor GpioOper: SGPO bit 0 = enable, bit 2 = resetn. */
		GPIO_LOOKUP("tps68470-gpio", 7, "enable", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("tps68470-gpio", 9, "resetn", GPIO_ACTIVE_HIGH),
		{ }
	}
};

static struct gpiod_lookup_table cf20_tps68470_ov8858_gpios = {
	.dev_id = "i2c-INT3477:00",
	.table = {
		GPIO_LOOKUP("tps68470-gpio", 7, "enable", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("tps68470-gpio", 9, "resetn", GPIO_ACTIVE_LOW),
		/* s_enable is active-high, so the V4L2 powerdown semantic is inverted. */
		GPIO_LOOKUP("tps68470-gpio", 7, "powerdown", GPIO_ACTIVE_LOW),
		/* s_resetn is physically active-low; expose logical reset semantics. */
		GPIO_LOOKUP("tps68470-gpio", 9, "reset", GPIO_ACTIVE_LOW),
		{ }
	}
};

static const struct int3472_tps68470_board_data surface_go_tps68470_board_data = {
	.dev_name = "i2c-INT3472:05",
	.tps68470_regulator_pdata = &surface_go_tps68470_pdata,
	.n_gpiod_lookups = 2,
	.tps68470_gpio_lookup_tables = {
		&surface_go_int347a_gpios,
		&surface_go_int347e_gpios,
	},
};

static const struct int3472_tps68470_board_data surface_go3_tps68470_board_data = {
	.dev_name = "i2c-INT3472:01",
	.tps68470_regulator_pdata = &surface_go_tps68470_pdata,
	.n_gpiod_lookups = 2,
	.tps68470_gpio_lookup_tables = {
		&surface_go_int347a_gpios,
		&surface_go_int347e_gpios,
	},
};

static const struct int3472_tps68470_board_data dell_7212_tps68470_board_data = {
	.dev_name = "i2c-INT3472:05",
	.tps68470_regulator_pdata = &dell_7212_tps68470_pdata,
	.n_gpiod_lookups = 1,
	.tps68470_gpio_lookup_tables = {
		&dell_7212_int3479_gpios,
	},
};

static const struct int3472_tps68470_board_data msi_prestige_ai_evo_tps68470_board_data = {
	.dev_name = "i2c-INT3472:06",
	.tps68470_regulator_pdata = &msi_prestige_ai_evo_tps68470_pdata,
	.tps68470_gpio_swnode = &int3472_tps68470_daisy_chain_gpio_swnode,
	.n_gpiod_lookups = 1,
	.tps68470_gpio_lookup_tables = {
		&msi_prestige_ai_evo_ovti5675_gpios,
	},
};

static const struct int3472_tps68470_board_data intel_nvl_tps68470_board_data = {
	.dev_name = "i2c-INT3472:04",
	.tps68470_regulator_pdata = &intel_nvl_tps68470_pdata,
	.tps68470_gpio_swnode = &int3472_tps68470_daisy_chain_gpio_swnode,
	.n_gpiod_lookups = 1,
	.tps68470_gpio_lookup_tables = {
		&intel_nvl_tps68470_gpios,
	},
};

/* Settings for Panasonic Toughbook CF-20 Mk2 camera module. */
static struct regulator_consumer_supply cf20_core_consumer_supplies[] = {
	REGULATOR_SUPPLY("dvdd", "i2c-INT3471:00"),
	/* Rear camera exposed by current CF-20 DSDT. */
	REGULATOR_SUPPLY("dvdd", "i2c-INT3477:00"),
};

static struct regulator_consumer_supply cf20_ana_consumer_supplies[] = {
	REGULATOR_SUPPLY("avdd", "i2c-INT3471:00"),
	REGULATOR_SUPPLY("avdd", "i2c-INT3477:00"),
};

static struct regulator_consumer_supply cf20_vcm_consumer_supplies[] = {
	REGULATOR_SUPPLY("vdd", "i2c-INT3471:00-VCM"),
	REGULATOR_SUPPLY("vcm", "i2c-INT3471:00"),
	/* Rear OV8858 VCM rail, exposed by the same TPS68470 VCM output. */
	REGULATOR_SUPPLY("vcm", "i2c-INT3477:00"),
};

static struct regulator_consumer_supply cf20_vsio_consumer_supplies[] = {
	REGULATOR_SUPPLY("dovdd", "i2c-INT3471:00"),
	REGULATOR_SUPPLY("dovdd", "i2c-INT3477:00"),
};

static const struct regulator_init_data cf20_core_reg_init_data = {
	.constraints = {
		/* Vendor VoltageWF default: VD request 1050 mV. */
		.min_uV = 1050000,
		.max_uV = 1050000,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS |
				  REGULATOR_CHANGE_VOLTAGE,
	},
	.num_consumer_supplies = ARRAY_SIZE(cf20_core_consumer_supplies),
	.consumer_supplies = cf20_core_consumer_supplies,
};

static const struct regulator_init_data cf20_ana_reg_init_data = {
	.constraints = {
		/* Ghidra: imx135.sys requests VA=2700 mV; TPS68470 rounds to 2708.4 mV. */
		.min_uV = 2708400,
		.max_uV = 2708400,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS |
				  REGULATOR_CHANGE_VOLTAGE,
	},
	.num_consumer_supplies = ARRAY_SIZE(cf20_ana_consumer_supplies),
	.consumer_supplies = cf20_ana_consumer_supplies,
};

static const struct regulator_init_data cf20_vcm_reg_init_data = {
	.constraints = {
		.min_uV = 2815200,
		.max_uV = 2815200,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS |
				  REGULATOR_CHANGE_VOLTAGE,
	},
	.num_consumer_supplies = ARRAY_SIZE(cf20_vcm_consumer_supplies),
	.consumer_supplies = cf20_vcm_consumer_supplies,
};

static const struct regulator_init_data cf20_vio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = true,
		.always_on = true,
	},
};

static const struct regulator_init_data cf20_vsio_reg_init_data = {
	.constraints = {
		.min_uV = 1800600,
		.max_uV = 1800600,
		.apply_uV = true,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS |
				  REGULATOR_CHANGE_VOLTAGE,
	},
	.num_consumer_supplies = ARRAY_SIZE(cf20_vsio_consumer_supplies),
	.consumer_supplies = cf20_vsio_consumer_supplies,
};

static const struct tps68470_regulator_platform_data_ext cf20_tps68470_pdata = {
	.cf20_split_s_i2c = true,
	.reg_init_data = {
		[TPS68470_CORE] = &cf20_core_reg_init_data,
		[TPS68470_ANA] = &cf20_ana_reg_init_data,
		[TPS68470_VCM] = &cf20_vcm_reg_init_data,
		[TPS68470_VIO] = &cf20_vio_reg_init_data,
		[TPS68470_VSIO] = &cf20_vsio_reg_init_data,
	},
};

static const struct int3472_tps68470_board_data cf20_tps68470_board_data = {
	/* The current DSDT exposes the active control-logic node as :05. */
	.dev_name = "i2c-INT3472:05",
	.tps68470_regulator_pdata = &cf20_tps68470_pdata,
	/*
	 * Do not attach the static daisy-chain software node here.  Its GPIO
	 * child creates a duplicate gpiochip software node on module reload.
	 * The vendor sequence enables S_I2C_CTL bit 0 from SensorPowerOn.
	 */
	.tps68470_gpio_swnode = NULL,
	.n_gpiod_lookups = 2,
	.tps68470_gpio_lookup_tables = {
		&cf20_tps68470_gpios,
		&cf20_tps68470_ov8858_gpios,
	},
	/* CAM0.SSDB[1] == 0x50: high nibble 5 selects iactrllogic64's
	 * TPS68470 path, not the separate TCA9535 power path. */
	/* ACPI SSDB and IMX135 driver require a 24 MHz sensor clock. */
	.clk_rate = 24000000,
	/* Ghidra: match iactrllogic64.sys SSTps68470Clock::StartClock. */
	.vendor_cf20_clock_config = 1,
};

static const struct dmi_system_id int3472_tps68470_board_data_table[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Microsoft Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Surface Go"),
		},
		.driver_data = (void *)&surface_go_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Microsoft Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Surface Go 2"),
		},
		.driver_data = (void *)&surface_go_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Microsoft Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Surface Go 3"),
		},
		.driver_data = (void *)&surface_go3_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Latitude 7212 Rugged Extreme Tablet"),
		},
		.driver_data = (void *)&dell_7212_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Micro-Star International Co., Ltd."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Prestige 13 AI+ Evo A2VMG"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "MS-13Q3"),
		},
		.driver_data = (void *)&msi_prestige_ai_evo_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Micro-Star International Co., Ltd."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Prestige 14 AI+ Evo C2VMG"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "MS-14N3"),
		},
		.driver_data = (void *)&msi_prestige_ai_evo_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Micro-Star International Co., Ltd."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Prestige 16 AI+ Evo B2VMG"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "MS-15A3"),
		},
		.driver_data = (void *)&msi_prestige_ai_evo_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Nova Lake Client Platform"),
		},
		.driver_data = (void *)&intel_nvl_tps68470_board_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Panasonic Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "CF-20-2"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "CF20-2"),
		},
		.driver_data = (void *)&cf20_tps68470_board_data,
	},
	{ }
};

static bool cf20_acpi_path_is_camera_pmic(struct acpi_device *adev,
						 bool *dynamic_address)
{
	struct acpi_buffer path = { ACPI_ALLOCATE_BUFFER, NULL };
	bool match = false;

	*dynamic_address = false;

	if (!adev || ACPI_FAILURE(acpi_get_name(adev->handle,
						       ACPI_FULL_PATHNAME, &path)))
		return false;

	/*
	 * CLP0 is the active dynamic control-logic node on the CF-20-2.
	 * PMIC is the fixed fallback node, normally hidden by _STA when
	 * SCSS == 0.  Both are accepted only at the TPS68470 address.
	 */
	match = !strcmp(path.pointer, "\\_SB_.PCI0.CLP0");
	if (match)
		*dynamic_address = true;
	else
		match = !strcmp(path.pointer, "\\_SB_.PCI0.I2C2.PMIC");
	if (!match)
		dev_err(&adev->dev, "CF-20 PMIC ACPI path rejected: %s\n",
			(char *)path.pointer);

	kfree(path.pointer);
	return match;
}

const struct int3472_tps68470_board_data *
int3472_tps68470_get_board_data(struct i2c_client *client)
{
	const struct int3472_tps68470_board_data *board_data;
	const struct dmi_system_id *match;
	const char *client_name = dev_name(&client->dev);
	const char *sys_vendor = dmi_get_system_info(DMI_SYS_VENDOR);
	const char *product_name = dmi_get_system_info(DMI_PRODUCT_NAME);
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);
	bool dynamic_address;

	if (!allow_cf20_camera_power && sys_vendor && product_name && board_name &&
	    !strcmp(sys_vendor, "Panasonic Corporation") &&
	    !strcmp(product_name, "CF-20-2") &&
	    !strcmp(board_name, "CF20-2"))
		return NULL;

	for (match = dmi_first_match(int3472_tps68470_board_data_table);
	     match;
	     match = dmi_first_match(match + 1)) {
		board_data = match->driver_data;

		if (board_data == &cf20_tps68470_board_data) {
			/*
			 * Do not bind CF-20 data to an arbitrary INT3472 node.
			 * CLP0 gets its address from the dynamic ACPI _CRS (C0IA).
			 * The fixed PMIC fallback has a statically decoded 0x4c address.
			 */
			if (!cf20_acpi_path_is_camera_pmic(
				    ACPI_COMPANION(&client->dev), &dynamic_address) ||
			    (!dynamic_address && client->addr != 0x4c))
				return NULL;

			return board_data;
		}

		if (strcmp(board_data->dev_name, client_name) == 0)
			return board_data;
	}

	return NULL;
}
