#!/bin/sh

# CF-20 safety self-test.
#
# This script is deliberately read-only with respect to the camera.  It must
# never call cam, libcamera, v4l2-ctl, media-ctl, modprobe, rmmod, or request a
# frame.  The historical IPU3 ImgU/CSS path can wedge the complete desktop;
# this check verifies containment and the already-running PipeWire bridge only.
set -u

fail=0

say() {
	printf '%s\n' "$*"
}

check_equal() {
	label=$1
	expected=$2
	actual=$3
	if [ "$actual" = "$expected" ]; then
		printf 'OK: %s=%s\n' "$label" "$actual"
	else
		printf 'FEHLER: %s=%s (erwartet %s)\n' "$label" "${actual:-nicht gesetzt}" "$expected"
		fail=1
	fi
}

say '== CF-20 camera safety self-test (read-only) =='
say 'Keine Kamera, kein libcamera, kein V4L2-Stream und kein Modul-Reload.'

version=$(dpkg-query -W -f='${Version}' cf20-camera-driver 2>/dev/null || true)
printf 'Treiberpaket: %s\n' "${version:-nicht installiert}"
if [ -z "$version" ] || ! dpkg --compare-versions "$version" ge 0.1.0-229; then
	say 'FEHLER: mindestens cf20-camera-driver 0.1.0-229 wird benötigt'
	fail=1
fi

if [ -d /sys/module/ipu3_imgu ]; then
	say 'FEHLER: ipu3_imgu ist geladen; CSS/ImgU bleibt gesperrt'
	fail=1
else
	say 'OK: ipu3_imgu ist nicht geladen'
fi

blacklist_found=0
install_block_found=0
for config in /etc/modprobe.d/cf20-camera-driver.conf \
	/etc/modprobe.d/cf20-camera-ipu3.conf; do
	if [ -f "$config" ] && grep -Eq '^[[:space:]]*blacklist[[:space:]]+ipu3_imgu([[:space:]]|$)' "$config"; then
		blacklist_found=1
	fi
	if [ -f "$config" ] && grep -Eq '^[[:space:]]*install[[:space:]]+ipu3_imgu[[:space:]]+/bin/false([[:space:]]|$)' "$config"; then
		install_block_found=1
	fi
done
if [ "$blacklist_found" -eq 1 ]; then
	say 'OK: ipu3_imgu ist in der Modprobe-Konfiguration gesperrt'
else
	say 'FEHLER: keine ipu3_imgu-Sperre in /etc/modprobe.d gefunden'
	fail=1
fi
if [ "$install_block_found" -eq 1 ]; then
	say 'OK: explizites Laden von ipu3_imgu ist blockiert'
else
	say 'FEHLER: keine harte ipu3_imgu-Installationssperre gefunden'
	fail=1
fi

check_equal allow_cf20_camera_enumeration Y \
	"$(cat /sys/module/ipu_bridge/parameters/allow_cf20_camera_enumeration 2>/dev/null || true)"
check_equal ov8858.allow_power_on Y \
	"$(cat /sys/module/ov8858/parameters/allow_power_on 2>/dev/null || true)"
check_equal ov8858.allow_streaming Y \
	"$(cat /sys/module/ov8858/parameters/allow_streaming 2>/dev/null || true)"

if command -v systemctl >/dev/null 2>&1; then
	if systemctl is-active --quiet cf20-raw-camera-modules.service; then
		say 'OK: Raw-Kernelmodul-Dienst ist aktiv'
	else
		say 'FEHLER: cf20-raw-camera-modules.service ist nicht aktiv'
		fail=1
	fi
	if systemctl --user is-active --quiet cf20-raw-pipewire-camera.service; then
		say 'OK: PipeWire-Rückkamera-Dienst ist aktiv'
	else
		say 'FEHLER: PipeWire-Rückkamera-Dienst ist nicht aktiv'
		fail=1
	fi
else
	say 'FEHLER: systemctl nicht gefunden'
	fail=1
fi

raw_device=
for name_path in /sys/class/video4linux/video*/name; do
	[ -f "$name_path" ] || continue
	if [ "$(cat "$name_path" 2>/dev/null || true)" = 'ipu3-cio2 0' ]; then
		raw_device=/dev/$(basename "$(dirname "$name_path")")
		break
	fi
done
if [ -n "$raw_device" ] && [ -c "$raw_device" ]; then
	say "OK: Raw-CIO2-Gerät vorhanden: $raw_device"
else
	say 'FEHLER: kein Raw-CIO2-Gerät ipu3-cio2 0 vorhanden'
	fail=1
fi

if [ -x /usr/libexec/cf20-bayer-rgb ]; then
	say 'OK: sicherer Bayer-zu-RGB-Konverter ist installiert'
else
	say 'FEHLER: Bayer-zu-RGB-Konverter fehlt'
	fail=1
fi

if command -v pw-cli >/dev/null 2>&1; then
	# pw-dump can remain in its monitor/read loop on this PipeWire build.
	# pw-cli ls Node is a bounded one-shot query and exposes the same node
	# property needed by this read-only check.
	if timeout --signal=TERM --kill-after=2s 5s \
		pw-cli ls Node 2>/dev/null | grep -Fq 'cf20-ov8858-raw'; then
		say 'OK: PipeWire-Rückkameraquelle ist veröffentlicht'
	else
		say 'FEHLER: PipeWire-Rückkameraquelle nicht veröffentlicht'
		fail=1
	fi
else
	say 'FEHLER: pw-cli nicht gefunden'
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	say 'SELFTEST: OK (sicherer Raw-/PipeWire-Zustand)'
	exit 0
fi

say 'SELFTEST: FEHLER'
say 'Keine Kameraoperation wurde gestartet. Die obigen Zustandsfehler zuerst beheben.'
exit 1
