#!/bin/sh

# Install the bundled CF-20 OV8858 raw-CIO2 release and run its bounded,
# headless end-to-end test.  This script is relocatable inside this repository.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
package="$script_dir/cf20-camera-driver_0.1.0-237_amd64.deb"
expected_kernel=7.3.4+deb13-amd64

if [ ! -r "$package" ]; then
	echo "missing release package: $package" >&2
	exit 2
fi
if [ "$(uname -r)" != "$expected_kernel" ]; then
	echo "this release requires kernel $expected_kernel (running: $(uname -r))" >&2
	exit 3
fi

systemctl --user stop cf20-raw-pipewire-camera.service 2>/dev/null || true
sudo dpkg --force-confnew -i "$package"
sudo depmod -a "$(uname -r)"
sudo update-initramfs -u -k "$(uname -r)"
sudo systemctl enable --now cf20-raw-camera-modules.service

systemctl --user daemon-reload
systemctl --user reset-failed cf20-raw-pipewire-camera.service 2>/dev/null || true
systemctl --user enable --now cf20-raw-pipewire-camera.service
sleep 5

exec /usr/sbin/cf20-camera-final-test
