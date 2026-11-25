#!/bin/sh

# One bounded, headless end-to-end test for the installed safe pipeline.
# It never opens V4L2 directly, never loads/reloads a module, and never starts
# libcamera or the desktop camera application.  The kernel raw stream is
# already owned by the supervised service; these tests exercise only its
# PipeWire source and the GStreamer device-provider path used by the app.
set -u

fail=0
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/cf20-camera-final-test.XXXXXX") || exit 1
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

say() {
	printf '%s\n' "$*"
}

say '== CF-20 camera final test (headless) =='
initial_safety_log=$tmpdir/selftest-initial.log
initial_safety_ok=1
for _attempt in $(seq 1 30); do
	if /usr/sbin/cf20-camera-selftest >"$initial_safety_log" 2>&1; then
		initial_safety_ok=0
		break
	fi
	sleep 1
done
cat "$initial_safety_log"
if [ "$initial_safety_ok" -ne 0 ]; then
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	say 'FINAL-TEST: FEHLER (Sicherheits-/Dienstzustand)'
	exit 1
fi

# The PipeWire source accepts RGB frames only.  Make sure the supervised
# process chain contains the explicit Bayer-to-RGB stage; a source that merely
# forwards non-zero Bayer bytes can otherwise pass the frame-count checks while
# producing visual noise.
if ps -eo args= | grep -Eq '[c]f20-bayer-rgb'; then
	say 'OK: laufender Bayer-to-RGB-Konverter ist in der Capture-Kette'
else
	say 'FEHLER: Bayerdaten werden nicht durch den RGB-Konverter geleitet'
	fail=1
fi

native_log=$tmpdir/native.log
if timeout --signal=TERM --kill-after=3s 10s \
	/usr/libexec/cf20-pipewire-native-consumer >"$native_log" 2>&1; then
	native_frames=$(sed -n 's/^PIPEWIRE_NATIVE_CONSUMER_FRAMES=//p' "$native_log")
	native_invalid=$(sed -n \
		's/^PIPEWIRE_NATIVE_CONSUMER_INVALID_BUFFERS=//p' "$native_log")
	native_nonzero=$(sed -n \
		's/^PIPEWIRE_NATIVE_CONSUMER_NONZERO_FRAMES=//p' "$native_log")
	if [ "${native_frames:-0}" -gt 0 ] &&
		[ "${native_invalid:-1}" -eq 0 ] &&
		[ "${native_nonzero:-0}" -gt 0 ] 2>/dev/null; then
		say "OK: native PipeWire consumer empfängt vollständige RGB-Frames: $native_frames"
	else
		say 'FEHLER: native PipeWire consumer empfängt keine gültigen RGB-Frames'
		cat "$native_log"
		fail=1
	fi
else
	say 'FEHLER: native PipeWire consumer fehlgeschlagen'
	cat "$native_log"
	fail=1
fi

gstreamer_log=$tmpdir/gstreamer.log
if timeout --signal=TERM --kill-after=3s 12s \
	/usr/libexec/cf20-pipewire-consumer-test >"$gstreamer_log" 2>&1; then
	gstreamer_frames=$(sed -n 's/^PIPEWIRE_CONSUMER_FRAMES=//p' "$gstreamer_log")
	if [ "${gstreamer_frames:-0}" -gt 0 ] 2>/dev/null; then
		say "OK: GStreamer/PipeWire consumer empfängt Frames: $gstreamer_frames"
	else
		say 'FEHLER: GStreamer/PipeWire consumer empfängt keine Frames'
		cat "$gstreamer_log"
		fail=1
	fi
else
	say 'FEHLER: GStreamer/PipeWire consumer fehlgeschlagen'
	cat "$gstreamer_log"
	fail=1
fi

say '== Stabilität nach Consumer-Trennung =='
post_safety_log=$tmpdir/selftest-post.log
post_safety_ok=1
for _attempt in $(seq 1 10); do
	if /usr/sbin/cf20-camera-selftest >"$post_safety_log" 2>&1; then
		post_safety_ok=0
		break
	fi
	sleep 1
done
cat "$post_safety_log"
if [ "$post_safety_ok" -eq 0 ]; then
	say 'OK: Raw-Dienst und PipeWire-Quelle bleiben nach den Tests aktiv'
else
	say 'FEHLER: Raw-Dienst oder PipeWire-Quelle nach Consumer-Trennung nicht stabil'
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	say 'FINAL-TEST: OK (Raw-CIO2 -> Bayer -> RGB -> PipeWire -> GStreamer)'
	exit 0
fi

say 'FINAL-TEST: FEHLER'
exit 1
