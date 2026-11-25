#!/bin/sh
# Read-only preflight for the Panasonic Toughbook CF-20 camera test.
# This script never loads modules and never writes ACPI, EC, I2C or sysfs.

set -u

fail=0

report() {
	printf '%s\n' "$1"
}

require_equal() {
	label=$1
	actual=$2
	expected=$3
	if [ "$actual" = "$expected" ]; then
		report "OK   $label=$actual"
	else
		report "STOP $label=$actual (expected $expected)"
		fail=1
	fi
}

module_for_symbol() {
	case "$1" in
		CONFIG_INTEL_SKL_INT3472=m) printf '%s' 'intel_skl_int3472_tps68470.ko' ;;
		CONFIG_COMMON_CLK_TPS68470=m) printf '%s' 'clk-tps68470.ko' ;;
		CONFIG_GPIO_TPS68470=m) printf '%s' 'gpio-tps68470.ko' ;;
		CONFIG_REGULATOR_TPS68470=m) printf '%s' 'tps68470-regulator.ko' ;;
		CONFIG_VIDEO_IPU3_CIO2=m) printf '%s' 'ipu3-cio2.ko' ;;
		CONFIG_VIDEO_IMX135=m) printf '%s' 'imx135.ko' ;;
		CONFIG_VIDEO_AD5816=m) printf '%s' 'ad5816.ko' ;;
		*) printf '%s' '' ;;
	esac
}

require_acpi_status() {
	label=$1
	status=$2

	if [ "${CF20_REQUIRE_ACTIVE:-0}" = '1' ]; then
		require_equal "$label status" "$status" '15'
	elif [ "$status" = '15' ]; then
		report "OK   $label status=$status (active)"
	elif [ "$status" = '0' ]; then
		report "SAFE $label status=0 (inactive; safety gates closed)"
	else
		report "STOP $label status=$status (expected 0 or 15)"
		fail=1
	fi
}

read_dmi() {
	cat "/sys/class/dmi/id/$1" 2>/dev/null || printf '%s' '<unavailable>'
}

require_equal sys_vendor "$(read_dmi sys_vendor)" 'Panasonic Corporation'
require_equal product_name "$(read_dmi product_name)" 'CF-20-2'
require_equal board_name "$(read_dmi board_name)" 'CF20-2'

kernel_config=${CF20_KERNEL_CONFIG:-/boot/config-$(uname -r)}
if [ -r "$kernel_config" ]; then
	for symbol in \
		'CONFIG_INTEL_SKL_INT3472=m' \
		'CONFIG_COMMON_CLK_TPS68470=m' \
		'CONFIG_GPIO_TPS68470=m' \
		'CONFIG_REGULATOR_TPS68470=m' \
		'CONFIG_VIDEO_IPU3_CIO2=m' \
		'CONFIG_VIDEO_IMX135=m' \
		'CONFIG_VIDEO_AD5816=m' \
		'CONFIG_VIDEOBUF2_CORE=m' \
		'CONFIG_VIDEOBUF2_DMA_SG=m' \
		'CONFIG_VIDEOBUF2_V4L2=m' \
		'CONFIG_V4L2_CCI=m' \
		'CONFIG_V4L2_CCI_I2C=m' \
		'CONFIG_VIDEO_V4L2_SUBDEV_API=y'; do
		if grep -qx "$symbol" "$kernel_config"; then
			report "OK   $symbol"
			elif [ -n "${CF20_MODULE_DIR:-}" ]; then
				module=$(module_for_symbol "$symbol")
				if [ -n "$module" ] && [ -r "$CF20_MODULE_DIR/$module" ]; then
					report "OK   external $symbol -> $module"
				else
					report "STOP missing $symbol"
					fail=1
				fi
		else
			report "STOP missing $symbol"
			fail=1
		fi
	done
else
	report "STOP missing $kernel_config"
	fail=1
fi

camera_status=/sys/bus/acpi/devices/INT3471:00/status
if [ -r "$camera_status" ]; then
	status=$(cat "$camera_status")
	require_acpi_status 'ACPI INT3471:00' "$status"
else
	report "STOP missing $camera_status"
	fail=1
fi

camera_path=/sys/bus/acpi/devices/INT3471:00/path
if [ -r "$camera_path" ]; then
	path=$(cat "$camera_path")
	require_equal 'ACPI INT3471:00 path' "$path" '\_SB_.PCI0.I2C2.CAM0'
else
	report "STOP missing $camera_path"
	fail=1
fi

pmic_status=/sys/bus/acpi/devices/INT3472:00/status
if [ -r "$pmic_status" ]; then
	status=$(cat "$pmic_status")
	require_acpi_status 'ACPI INT3472:00' "$status"
else
	report "STOP missing $pmic_status"
	fail=1
fi

pmic_path=/sys/bus/acpi/devices/INT3472:00/path
if [ -r "$pmic_path" ]; then
	path=$(cat "$pmic_path")
	require_equal 'ACPI INT3472:00 path' "$path" '\_SB_.PCI0.I2C2.PMIC'
else
	report "STOP missing $pmic_path"
	fail=1
fi

if [ -n "${CF20_MODULE_DIR:-}" ]; then
	for module in ipu-bridge.ko ipu3-cio2.ko \
		videobuf2-common.ko videobuf2-dma-sg.ko videobuf2-dma-contig.ko \
		videobuf2-memops.ko videobuf2-v4l2.ko videobuf2-vmalloc.ko \
		imx135.ko ad5816.ko \
		intel_skl_int3472_common.ko intel_skl_int3472_tps68470.ko \
		clk-tps68470.ko gpio-tps68470.ko tps68470-regulator.ko; do
		path="$CF20_MODULE_DIR/$module"
		if [ -r "$path" ]; then
			report "OK   artifact $path"
		else
			report "STOP missing artifact $path"
			fail=1
		fi
	done
else
	report 'INFO CF20_MODULE_DIR unset; module artifact check skipped'
fi

report 'SAFE no module load or hardware write performed'
exit "$fail"
