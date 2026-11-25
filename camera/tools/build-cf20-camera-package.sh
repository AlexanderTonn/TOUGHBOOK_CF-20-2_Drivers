#!/bin/sh
# Build the CF-20 camera module and a kernel-version-specific Debian package.
# This script only compiles and stages files. It never installs, loads modules,
# accesses sysfs/I2C, updates initramfs, or reboots the machine.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel_source=${CF20_KERNEL_SOURCE:-/home/alto/cf20-kernel}
kernel_release=${CF20_KERNEL_RELEASE:-$(uname -r)}
kernel_build=${CF20_KERNEL_BUILD:-/lib/modules/$kernel_release/build}
base_package=${CF20_BASE_PACKAGE:-/home/alto/cf20-build/cf20-camera-driver_0.1.0-224_amd64.deb}
package_version=${CF20_PACKAGE_VERSION:-0.1.0-237}
output_package=${CF20_OUTPUT_PACKAGE:-/home/alto/cf20-build/cf20-camera-driver_${package_version}_amd64.deb}
cf20_build_root=${CF20_BUILD_ROOT:-/home/alto/cf20-build}
wireplumber_create_item=${CF20_WIREPLUMBER_CREATE_ITEM:-$script_dir/cf20-node-create-item.lua}
camera_selftest_source=${CF20_CAMERA_SELFTEST:-$cf20_build_root/cf20-camera-selftest.sh}
camera_final_test_source=${CF20_CAMERA_FINAL_TEST:-$cf20_build_root/cf20-camera-final-test.sh}
raw_supervisor_source=${CF20_RAW_SUPERVISOR:-$cf20_build_root/cf20-raw-pipewire-camera-isolated.py}
pipewire_include=${CF20_PIPEWIRE_INCLUDE:-$cf20_build_root/pipewire-dev/usr/include}
pipewire_source_c=${CF20_PIPEWIRE_SOURCE_C:-$cf20_build_root/cf20-pipewire-source.c}
pipewire_native_c=${CF20_PIPEWIRE_NATIVE_C:-$cf20_build_root/cf20-pipewire-native-consumer.c}
cio2_capture_c=${CF20_CIO2_CAPTURE_C:-$cf20_build_root/cf20-cio2-capture.c}
ip3b_unpack_c=${CF20_IP3B_UNPACK_C:-$cf20_build_root/cf20-ip3b-unpack.c}
bayer_rgb_c=${CF20_BAYER_RGB_C:-$cf20_build_root/cf20-bayer-rgb.c}
pipewire_source_binary=
pipewire_native_binary=
cio2_capture_binary=
ip3b_unpack_binary=
bayer_rgb_binary=
module_source=$kernel_source/drivers/media/i2c/imx135.ko
ov8858_source=$kernel_source/drivers/media/i2c/ov8858.ko
module_dir=$kernel_source/drivers/media/i2c
pmic_source=$kernel_source/drivers/platform/x86/intel/int3472/intel_skl_int3472_tps68470.ko
pmic_dir=$kernel_source/drivers/platform/x86/intel/int3472
regulator_source=$kernel_source/drivers/regulator/tps68470-regulator.ko
regulator_dir=$kernel_source/drivers/regulator
clk_source=$kernel_source/drivers/clk/clk-tps68470.ko
clk_dir=$kernel_source/drivers/clk
gpio_source=$kernel_source/drivers/gpio/gpio-tps68470.ko
gpio_dir=$kernel_source/drivers/gpio
ipu_bridge_source=$kernel_source/drivers/media/pci/intel/ipu-bridge.ko
ipu_bridge_dir=$kernel_source/drivers/media/pci/intel
cio2_source=$kernel_source/drivers/media/pci/intel/ipu3/ipu3-cio2.ko
cio2_dir=$kernel_source/drivers/media/pci/intel/ipu3
imgu_source=$kernel_source/drivers/staging/media/ipu3/ipu3-imgu.ko
imgu_dir=$kernel_source/drivers/staging/media/ipu3
cf20_imgu_firmware=/home/alto/cf20-panasonic-driver/css_fw.bin

extra_symbols="$kernel_source/drivers/media/v4l2-core/Module.symvers \
$kernel_source/drivers/media/mc/Module.symvers \
$kernel_source/drivers/media/common/videobuf2/Module.symvers \
$kernel_source/drivers/clk/Module.symvers \
$kernel_source/drivers/platform/x86/intel/int3472/Module.symvers"

cleanup_build_artifacts()
{
	rm -rf "${stage:-}"
	rm -f "${pipewire_source_binary:-}" "${pipewire_native_binary:-}"
	rm -f "${cio2_capture_binary:-}" "${ip3b_unpack_binary:-}"
	rm -f "${bayer_rgb_binary:-}"
}
trap cleanup_build_artifacts EXIT HUP INT TERM

# Prefer the exact running-kernel export table when the separate kernel build
# tree was prepared without its top-level Module.symvers.  This only affects
# modpost validation; it does not read or write hardware.
kernel_symvers=${CF20_KERNEL_SYMVERS:-/lib/modules/$kernel_release/build/Module.symvers}
kernel_symvers_real=$(readlink -f "$kernel_symvers" 2>/dev/null || true)
kernel_build_symvers_real=$(readlink -f "$kernel_build/Module.symvers" 2>/dev/null || true)
if [ -r "$kernel_symvers" ] &&
	[ "$kernel_symvers_real" != "$kernel_build_symvers_real" ]; then
	extra_symbols="$kernel_symvers $extra_symbols"
fi

# Build the CF-20 PMIC first.  imx135 imports the Ghidra-derived IoActive
# helper from this module, so the package must carry the matching PMIC build.
make -C "$kernel_build" \
	M="$pmic_dir" \
	CONFIG_INTEL_SKL_INT3472_TPS68470=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols" \
	intel_skl_int3472_tps68470.ko

test -r "$pmic_source"

# Build and ship the matching TPS68470 MFD children as well.  The CF-20
# regulator provider must use the same source tree as the INT3472 core; in
# particular, its synchronous probe is what prevents ov8858 from binding to
# dummy regulators during an asynchronous boot.
make -C "$kernel_build" \
	M="$regulator_dir" \
	CONFIG_REGULATOR_TPS68470=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols" \
	tps68470-regulator.ko

test -r "$regulator_source"

make -C "$kernel_build" \
	M="$clk_dir" \
	CONFIG_COMMON_CLK_TPS68470=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols" \
	clk-tps68470.ko

test -r "$clk_source"

make -C "$kernel_build" \
	M="$gpio_dir" \
	CONFIG_GPIO_TPS68470=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols" \
	gpio-tps68470.ko

test -r "$gpio_source"

# Build the bridge first.  imx135 imports its namespaced reconnect helper;
# feeding this Module.symvers into the sensor build makes modprobe record the
# real dependency and preserves safe load order after a module reload.
make -C "$kernel_build" \
	M="$ipu_bridge_dir" \
	CONFIG_VIDEO_IPU_BRIDGE=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols" \
	ipu-bridge.ko

test -r "$ipu_bridge_source"

# Build the matching CIO2 module as well.  The CF-20 fwnode-to-I2C async
# fallback lives in ipu3-cio2, so shipping only ipu-bridge would leave the
# running kernel's stock CIO2 code active and silently omit the fix.
make -C "$kernel_build" \
	M="$cio2_dir" \
	CONFIG_VIDEO_IPU3_CIO2=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols $ipu_bridge_dir/Module.symvers" \
	ipu3-cio2.ko

test -r "$cio2_source"

# Build the matching staging IMGU module as well.  The CF-20 package carries
# the Windows-proven CSS firmware selector; without this module the running
# kernel would keep using the stock firmware loader and the selector would be
# inert.
make -C "$kernel_build" \
	M="$imgu_dir" \
	CONFIG_VIDEO_IPU3_IMGU=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols" \
	ipu3-imgu.ko

test -r "$imgu_source"
test -r "$cf20_imgu_firmware"
test -r "$wireplumber_create_item"
test -r "$camera_selftest_source"
test -r "$camera_final_test_source"
test -r "$raw_supervisor_source"
test -r "$pipewire_source_c"
test -r "$pipewire_native_c"
test -r "$cio2_capture_c"
test -r "$ip3b_unpack_c"
test -r "$bayer_rgb_c"

# Build the userspace PipeWire endpoints from the sources in this checkout.
# The reused base package contains older binaries; replacing them here makes
# the release self-contained and prevents an accidental regression to the
# GStreamer provider that previously crashed on disconnect.
pipewire_source_binary=$(mktemp /tmp/cf20-pipewire-source.XXXXXX)
pipewire_native_binary=$(mktemp /tmp/cf20-pipewire-native.XXXXXX)
cio2_capture_binary=$(mktemp /tmp/cf20-cio2-capture.XXXXXX)
ip3b_unpack_binary=$(mktemp /tmp/cf20-ip3b-unpack.XXXXXX)
bayer_rgb_binary=$(mktemp /tmp/cf20-bayer-rgb.XXXXXX)
cc -O2 -g -Wall -Wextra -Werror \
	-I"$pipewire_include/pipewire-0.3" \
	-I"$pipewire_include/spa-0.2" \
	-o "$pipewire_source_binary" "$pipewire_source_c" \
	-L/usr/lib/x86_64-linux-gnu \
	-Wl,-rpath,/usr/lib/x86_64-linux-gnu \
	-Wl,-z,now -l:libpipewire-0.3.so.0 -pthread
cc -O2 -g -Wall -Wextra -Werror \
	-I"$pipewire_include/pipewire-0.3" \
	-I"$pipewire_include/spa-0.2" \
	-o "$pipewire_native_binary" "$pipewire_native_c" \
	-L/usr/lib/x86_64-linux-gnu \
	-Wl,-rpath,/usr/lib/x86_64-linux-gnu \
	-Wl,-z,now -l:libpipewire-0.3.so.0 -pthread
cc -O2 -g -Wall -Wextra -Werror \
	-o "$cio2_capture_binary" "$cio2_capture_c"
cc -O2 -g -Wall -Wextra -Werror \
	-o "$ip3b_unpack_binary" "$ip3b_unpack_c"
cc -O2 -g -Wall -Wextra -Werror \
	-o "$bayer_rgb_binary" "$bayer_rgb_c"

make -C "$kernel_build" \
	M="$module_dir" \
	CONFIG_VIDEO_OV8858=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols $ipu_bridge_dir/Module.symvers" \
	ov8858.ko

test -r "$ov8858_source"

make -C "$kernel_build" \
	M="$module_dir" \
	CONFIG_VIDEO_IMX135=m \
	KERNELRELEASE="$kernel_release" \
	KBUILD_MODPOST_WARN=1 \
	KBUILD_EXTRA_SYMBOLS="$extra_symbols $ipu_bridge_dir/Module.symvers" \
	imx135.ko

test -r "$module_source"

stage=$(mktemp -d /tmp/cf20-camera-package.XXXXXX)

dpkg-deb -x "$base_package" "$stage/root"
find "$stage/root" -type d -name __pycache__ -prune -exec rm -rf {} +
# Keep the package documentation self-identifying when the release number is
# advanced without rebuilding the older base package first.
if [ -f "$stage/root/usr/share/doc/cf20-camera-driver/README.Debian" ]; then
	sed -i "1s/^CF-20 camera driver .*/CF-20 camera driver $package_version/" \
		"$stage/root/usr/share/doc/cf20-camera-driver/README.Debian"
fi
install -D -m 0755 "$pipewire_source_binary" \
	"$stage/root/usr/libexec/cf20-pipewire-source"
install -D -m 0755 "$pipewire_native_binary" \
	"$stage/root/usr/libexec/cf20-pipewire-native-consumer"
install -D -m 0755 "$cio2_capture_binary" \
	"$stage/root/usr/libexec/cf20-cio2-capture"
install -D -m 0755 "$ip3b_unpack_binary" \
	"$stage/root/usr/libexec/cf20-ip3b-unpack"
install -D -m 0755 "$bayer_rgb_binary" \
	"$stage/root/usr/libexec/cf20-bayer-rgb"
install -D -m 0755 "$raw_supervisor_source" \
	"$stage/root/usr/libexec/cf20-raw-pipewire-camera-isolated"
# The base package may contain modules for an older custom kernel.  A
# version-specific package must not carry those files: depmod and initramfs
# should see exactly one CF-20 module set for the selected release.
if [ -d "$stage/root/lib/modules" ]; then
	for old_module_dir in "$stage/root/lib/modules"/*; do
		[ -d "$old_module_dir" ] || continue
		[ "$(basename "$old_module_dir")" = "$kernel_release" ] ||
			rm -rf "$old_module_dir"
	done
fi
mkdir -p "$stage/root/DEBIAN"
dpkg-deb -e "$base_package" "$stage/root/DEBIAN"
sed -i "s/^Version: .*/Version: $package_version/" "$stage/root/DEBIAN/control"

# The desktop camera path is mediated by WirePlumber.  Keep the dependency
# and the provider-session override in the package instead of requiring a
# manual post-install copy or a per-test local configuration.
sed -i \
	'/^Depends:/ { /\<wireplumber\>/! s/$/, wireplumber/; }' \
	"$stage/root/DEBIAN/control"
install -D -m 0644 "$wireplumber_create_item" \
	"$stage/root/usr/share/wireplumber/scripts/cf20-node-create-item.lua"
install -D -m 0755 "$camera_selftest_source" \
	"$stage/root/usr/sbin/cf20-camera-selftest"
install -D -m 0755 "$camera_final_test_source" \
	"$stage/root/usr/sbin/cf20-camera-final-test"
install -D -m 0644 /dev/null \
	"$stage/root/etc/wireplumber/wireplumber.conf.d/90-cf20-camera.conf"
cat >"$stage/root/etc/wireplumber/wireplumber.conf.d/90-cf20-camera.conf" <<'EOF'
# CF-20 raw camera: classify generic pipewiresrc camera consumers as video
# before WirePlumber creates their SessionItem.
wireplumber.components.rules = [
  {
    matches = [
      { provides = hooks.node.create-session-item }
    ]
    actions = {
      merge = {
        name = cf20-node-create-item.lua
      }
    }
  }
]
EOF
if ! grep -qxF /etc/wireplumber/wireplumber.conf.d/90-cf20-camera.conf \
	"$stage/root/DEBIAN/conffiles"; then
	printf '%s\n' /etc/wireplumber/wireplumber.conf.d/90-cf20-camera.conf \
		>>"$stage/root/DEBIAN/conffiles"
fi

# Remove only the initramfs entries/hooks introduced by earlier CF-20 package
# revisions.  This preserves unrelated administrator-selected modules while
# preventing an obsolete name-based hook from reintroducing stock IPU3 files.
cat >"$stage/root/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -eu

kernel_release=__CF20_KERNEL_RELEASE__
modules_file=/etc/initramfs-tools/modules
if [ -f "$modules_file" ]; then
	for module in ipu3_imgu ipu3_cio2 ipu_bridge; do
		sed -i "\\|^${module}\\([[:space:]].*\\)\\?$|d" "$modules_file"
	done
fi
rm -f /etc/initramfs-tools/hooks/cf20-camera-driver

if command -v depmod >/dev/null 2>&1; then
	depmod -a "$kernel_release"
fi
if command -v update-initramfs >/dev/null 2>&1; then
	update-initramfs -u -k "$kernel_release"
fi

exit 0
EOF
sed -i "s/__CF20_KERNEL_RELEASE__/$kernel_release/" \
	"$stage/root/DEBIAN/postinst"
chmod 0755 "$stage/root/DEBIAN/postinst"

# The base package may carry a postrm for an older kernel release.  Replace it
# as well, otherwise dpkg can regenerate an obsolete initramfs while unwinding
# a failed upgrade.
cat >"$stage/root/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -eu

kernel_release=__CF20_KERNEL_RELEASE__
if command -v depmod >/dev/null 2>&1; then
	depmod -a "$kernel_release"
fi
if command -v update-initramfs >/dev/null 2>&1; then
	update-initramfs -u -k "$kernel_release"
fi

exit 0
EOF
sed -i "s/__CF20_KERNEL_RELEASE__/$kernel_release/" \
	"$stage/root/DEBIAN/postrm"
chmod 0755 "$stage/root/DEBIAN/postrm"

install -D -m 0644 "$module_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/imx135.ko"
install -D -m 0644 "$pmic_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/intel_skl_int3472_tps68470.ko"
install -D -m 0644 "$regulator_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/tps68470-regulator.ko"
install -D -m 0644 "$clk_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/clk-tps68470.ko"
install -D -m 0644 "$gpio_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/gpio-tps68470.ko"
install -D -m 0644 "$ipu_bridge_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/ipu-bridge.ko"
install -D -m 0644 "$cio2_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/ipu3-cio2.ko"
install -D -m 0644 "$imgu_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/ipu3-imgu.ko"
install -D -m 0644 "$ov8858_source" \
	"$stage/root/lib/modules/$kernel_release/updates/extra/ov8858.ko"
install -D -m 0644 "$cf20_imgu_firmware" \
	"$stage/root/lib/firmware/intel/ipu/cf20-css_fw.bin"

# Keep the IMGU firmware/safety choice separate from the user-edited camera
# conffile, so installing this package does not require replacing local
# allow_power_on/allow_streaming settings. Both risky CF-20 paths are
# disabled by default; the Windows image remains available for explicit,
# interactive diagnostics.
cat >"$stage/root/etc/modprobe.d/cf20-camera-ipu3.conf" <<'EOF'
# The CSS firmware extracted from the confirmed-working Windows CF-20
# rear-camera driver is installed for explicit diagnostics only.
options ipu3_imgu cf20_windows_firmware=0
options ipu3_imgu cf20_css_streaming=0
options ipu3_imgu cf20_isp_freq=0
EOF

# The base package is intentionally reused for its metadata, but its
# modprobe conffile may contain experimental values from an earlier session.
# Strip every CF-20 power/enumeration/sensor option and append one
# deterministic safe block.  Otherwise --force-confnew cannot help: dpkg
# compares the generated file with the already modified conffile and the
# stale allow_power_on=1 values can survive an upgrade.
camera_config="$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
sed -i \
	-e '/^options intel_skl_int3472_tps68470 /d' \
	-e '/^options ipu_bridge /d' \
	-e '/^options imx135 /d' \
	-e '/^options ov8858 /d' \
	"$camera_config"
cat >>"$camera_config" <<'EOF'

# The raw CIO2 camera path is enabled by default. Safety is provided by the
# separate ImgU/CSS containment below; the proven raw path must be present
# after a normal installation so the desktop camera app can discover it.
options intel_skl_int3472_tps68470 allow_cf20_camera_power=1
options ipu_bridge allow_cf20_camera_enumeration=1
options imx135 allow_power_on=0 allow_streaming=0 diagnostic=0
options imx135 sensor_io_active_before_power=0
options imx135 sensor_gpio_pre_power=0
options imx135 allow_cf20_acpi_disabled=0
options ov8858 allow_power_on=1 allow_streaming=1
EOF

# Keep the ImgU/CSS firmware-status override disabled by default. The raw
# camera path above is intentionally enabled; the separate ImgU module stays
# blacklisted and is never loaded by the raw-camera service.

# The Debian initramfs may already contain the stock IPU3 modules.  Do not put
# same-named IPU3 modules in /etc/initramfs-tools/modules: initramfs-tools
# resolves those names again after hooks have run, which can silently select
# the stock copy.  The late hook below installs the exact CF-20 files instead.
if [ -f "$stage/root/etc/initramfs-tools/modules" ]; then
	for module in ipu3_imgu ipu3_cio2 ipu_bridge; do
		sed -i "\\|^${module}\\([[:space:]].*\\)\\?$|d" \
			"$stage/root/etc/initramfs-tools/modules"
	done
fi

# initramfs-tools can otherwise resolve a same-named stock module from an
# already existing image.  The late hook below removes those copies and adds
# the replacements by absolute path from the freshly installed module tree.
install -D -m 0755 /dev/null \
	"$stage/root/etc/initramfs-tools/hooks/zz-cf20-camera-driver"
cat >"$stage/root/etc/initramfs-tools/hooks/zz-cf20-camera-driver" <<'EOF'
#!/bin/sh
# initramfs-tools' add_firmware() uses an unset local found_fwloc while
# searching firmware locations.  Do not enable nounset in this hook: Debian's
# hook API is otherwise correct, and a missing firmware file is handled by the
# explicit add_firmware check below.
set -e

if [ "${1:-}" = prereqs ]; then
		exit 0
fi

. /usr/share/initramfs-tools/hook-functions

# Do not leave same-named stock modules in the generated image.  The CF-20
# files are installed below by exact path, avoiding a second name-based module
# resolution pass that could select the stock copy.
for old_module in \
	"$DESTDIR/lib/modules/${version}/kernel/drivers/staging/media/ipu3/ipu3-imgu.ko" \
	"$DESTDIR/lib/modules/${version}/kernel/drivers/media/pci/intel/ipu3/ipu3-cio2.ko" \
	"$DESTDIR/lib/modules/${version}/kernel/drivers/media/pci/intel/ipu-bridge.ko" \
	"$DESTDIR/usr/lib/modules/${version}/kernel/drivers/staging/media/ipu3/ipu3-imgu.ko" \
	"$DESTDIR/usr/lib/modules/${version}/kernel/drivers/media/pci/intel/ipu3/ipu3-cio2.ko" \
	"$DESTDIR/usr/lib/modules/${version}/kernel/drivers/media/pci/intel/ipu-bridge.ko"; do
	 rm -f "$old_module"
done

# Install the exact CF-20 raw modules and their dependencies.  Using an
# absolute module path is intentional: the stock and CF-20 modules have the
# same kernel module names but different implementations.  ImgU is removed
# above and deliberately not installed in the initramfs.
_call_dracut_install -m \
	"/lib/modules/${version}/updates/extra/ipu-bridge.ko" \
	"/lib/modules/${version}/updates/extra/ipu3-cio2.ko"

# Load only the exact raw files at initramfs time. Keep the CF-20 parameters
# in the copied modprobe.d files instead of spelling them out here. ImgU is
# intentionally absent: it must not be started during boot.
printf '%s\n' \
	ipu_bridge \
	ipu3_cio2 >>"$DESTDIR/conf/modules"
# The Windows image is shipped for explicit diagnostic work, but is not
# copied into the normal initramfs path.  Loading it is a deliberate opt-in.
# initramfs-tools may already have copied these files before this late hook.
# Status 1 means "already present" and is not an error here.
copy_file config /etc/modprobe.d/cf20-camera-ipu3.conf || :
copy_file config /etc/modprobe.d/cf20-camera-driver.conf || :
EOF

# OV8858 is the CF-20 rear camera. The raw path is enabled as part of the
# package; the ImgU/CSS path remains disabled and blacklisted separately.
if ! grep -q '^options ov8858 ' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"; then
	cat >>"$stage/root/etc/modprobe.d/cf20-camera-driver.conf" <<'EOF'
options ov8858 allow_power_on=1 allow_streaming=1
EOF
fi

# The OV8858 can bind as soon as its ACPI I2C client appears.  Make the
# matching TPS68470 provider modules available and registered first, so the
# sensor never snapshots dummy regulators during boot.
if ! grep -q '^softdep ov8858 ' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"; then
	cat >>"$stage/root/etc/modprobe.d/cf20-camera-driver.conf" <<'EOF'
softdep ov8858 pre: ipu3-cio2 intel_skl_int3472_tps68470 intel_skl_int3472_common clk-tps68470 gpio-tps68470 tps68470-regulator
EOF
fi

# The raw path must be enabled after installation, while IMGU/CSS stays
# blocked by cf20-camera-ipu3.conf and the module blacklist.
grep -q '^options intel_skl_int3472_tps68470 allow_cf20_camera_power=1$' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q '^options ipu_bridge allow_cf20_camera_enumeration=1$' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q '^options imx135 allow_power_on=0 allow_streaming=0 diagnostic=0$' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q '^options ov8858 allow_power_on=1 allow_streaming=1$' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q 'allow_cf20_acpi_disabled=0' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q 'sensor_gpio_pre_power=0' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q 'sensor_io_active_before_power=0' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q 'allow_hardware=0' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"
grep -q '^options ov8858 allow_power_on=1 allow_streaming=1' \
	"$stage/root/etc/modprobe.d/cf20-camera-driver.conf"

# Replace the historical README from the reused base package.  Older
# revisions described experimental 7.2.12 settings and could be read as an
# instruction to enable the Windows CSS image.  Keep the operational recipe
# aligned with the safe defaults and the explicit Debian-CSS opt-in above.
install -D -m 0644 /dev/null \
	"$stage/root/usr/share/doc/cf20-camera-driver/README.Debian"
cat >"$stage/root/usr/share/doc/cf20-camera-driver/README.Debian" <<EOF
CF-20 camera driver ${package_version}
====================================

Safe installation
-----------------

The raw CIO2 camera path is enabled by default.  The package does not load
ipu3_imgu and does not start IPU3/CSS streaming.  The supplied Windows
css_fw.bin is installed only as an offline reference and is rejected by the
Linux CF-20 IMGU driver; it is not a Linux CSS replacement.

After installation, run depmod and reboot.  Before any camera application,
verify the package and these parameters:

  dpkg-query -W -f='\${Version}\\n' cf20-camera-driver
  cat /sys/module/ipu3_imgu/parameters/cf20_windows_firmware
  cat /sys/module/ipu3_imgu/parameters/cf20_css_streaming
  cat /sys/module/ipu3_imgu/parameters/cf20_isp_freq
  cat /sys/module/ipu_bridge/parameters/allow_cf20_camera_enumeration

The safe values for the three camera-path parameters are Y, Y, and Y.  The
separate ImgU values must remain 0/N and ipu3_imgu must remain unloaded.

Raw PipeWire bridge
-------------------

The raw-only bridge uses direct CIO2 MMAP, ip3b unpacking, userspace Bayer
demosaicing, and a bounded RGB PipeWire source.  It never starts libcamera or
the unsafe IPU3 ImgU/CSS path.

  sudo systemctl enable --now cf20-raw-camera-modules.service
  systemctl --user enable --now cf20-raw-pipewire-camera.service
  systemctl --user restart wireplumber
  systemctl --user restart cf20-raw-pipewire-camera.service

The package also installs a WirePlumber 0.5 component override.  It classifies
only explicitly targeted generic pipewiresrc camera streams as Video before
their SessionItem is activated; this is required by the Debian Camera/Snapshot
GStreamer provider path.

Read-only checks and one bounded final test:

  /usr/sbin/cf20-camera-selftest
  /usr/sbin/cf20-camera-final-test

Do not enable CSS, reload IPU3 modules, run cam, or perform direct V4L2 tests
on the tablet.  The raw bridge is intentionally fail-closed if its kernel
stream cannot be started.
EOF

tmp_package=$stage/$(basename "$output_package")
dpkg-deb --build --root-owner-group "$stage/root" "$tmp_package" >/dev/null
install -D -m 0644 "$tmp_package" "$output_package"

printf '%s\n' "built $output_package"
dpkg-deb -I "$output_package" | grep -E '^( Package| Version| Architecture| Depends):'
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/imx135.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/intel_skl_int3472_tps68470.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/tps68470-regulator.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/clk-tps68470.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/gpio-tps68470.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/ipu-bridge.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/ipu3-cio2.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/ipu3-imgu.ko"
dpkg-deb -c "$output_package" | grep "/$kernel_release/updates/extra/ov8858.ko"
dpkg-deb -c "$output_package" | grep "/lib/firmware/intel/ipu/cf20-css_fw.bin"
dpkg-deb -c "$output_package" | grep "/etc/modprobe.d/cf20-camera-ipu3.conf"
dpkg-deb -c "$output_package" | grep "/etc/wireplumber/wireplumber.conf.d/90-cf20-camera.conf"
dpkg-deb -c "$output_package" | grep "/usr/share/wireplumber/scripts/cf20-node-create-item.lua"
if dpkg-deb -c "$output_package" | grep -q '/__pycache__/'; then
	echo 'error: Python cache files leaked into the package' >&2
	exit 1
fi
