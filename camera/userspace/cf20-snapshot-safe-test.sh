#!/bin/sh

# Controlled GNOME Snapshot test. Run only after cf20-camera-selftest.sh is OK.
set -u

test_dir=$(mktemp -d /tmp/cf20-snapshot-test.XXXXXX) || exit 1
start_time=$(date --iso-8601=seconds)

echo "== CF-20 GNOME Snapshot safe test =="
printf 'Testverzeichnis: %s\n' "$test_dir"

version=$(dpkg-query -W -f='${Version}' cf20-camera-driver 2>/dev/null || true)
printf 'Treiberpaket: %s\n' "${version:-nicht installiert}"
if [ "$version" != '0.1.0-147' ]; then
	echo "FEHLER: erwartet wird cf20-camera-driver 0.1.0-147"
	exit 1
fi

for parameter in allow_power_on allow_streaming; do
	value=$(cat "/sys/module/ov8858/parameters/$parameter" 2>/dev/null || true)
	printf '%s: %s\n' "$parameter" "${value:-nicht geladen}"
	if [ "$value" != Y ]; then
		echo "FEHLER: ov8858 $parameter ist nicht Y"
		exit 1
	fi
done

if ! command -v snapshot >/dev/null 2>&1; then
	echo "FEHLER: snapshot ist nicht installiert"
	exit 1
fi

echo "Snapshot läuft maximal 20 Sekunden; kein Modul-Reload."
if [ "${CF20_ENABLE_CAPTURE:-0}" != 1 ]; then
	echo "SICHERHEITSSPERRE: Snapshot-Test deaktiviert, bis der Kernelhang analysiert ist."
	exit 2
fi

set +e
timeout --signal=TERM --kill-after=5s 20s \
	env LIBCAMERA_LOG_LEVELS=*:INFO snapshot --debug \
	>"$test_dir/snapshot.log" 2>&1
status=$?
set -e

cat "$test_dir/snapshot.log"
printf 'Snapshot exit status: %s\n' "$status"

echo "== kernel camera log =="
journalctl -k -b --since "$start_time" --no-pager 2>/dev/null \
	| grep -Ei 'ov8858|ipu3|cio2|csi|first frame|timeout|EREMOTEIO|error|fail' \
	|| true

echo "Logdatei: $test_dir/snapshot.log"
if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
	echo "GUI-TEST: kontrolliert nach 20 Sekunden beendet"
	exit 0
fi

if [ "$status" -ne 0 ]; then
	echo "GUI-TEST: Snapshot vor Ablauf fehlgeschlagen"
	exit "$status"
fi

echo "GUI-TEST: Snapshot wurde beendet"
