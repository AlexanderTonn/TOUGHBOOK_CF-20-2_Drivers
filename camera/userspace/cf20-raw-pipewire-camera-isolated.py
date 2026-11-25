#!/usr/bin/python3
"""Supervise the CF-20 raw capture and isolate PipeWire/GStreamer failures."""

import glob
import os
import signal
import subprocess
import sys
import time


# The environment overrides are for an offline supervisor regression test.
# Production uses the package-owned absolute paths below.
CAPTURE = os.environ.get("CF20_CAPTURE_HELPER", "/usr/libexec/cf20-cio2-capture")
UNPACKER = os.environ.get("CF20_UNPACKER_HELPER", "/usr/libexec/cf20-ip3b-unpack")
RGB_CONVERTER = os.environ.get("CF20_RGB_CONVERTER_HELPER", "/usr/libexec/cf20-bayer-rgb")
SINK = os.environ.get("CF20_SINK_HELPER", "/usr/libexec/cf20-pipewire-source")
MAX_RESTARTS = 12
RESTART_WINDOW = 300.0
RESTART_DELAY = 1.5


def read_sysfs(path):
    try:
        with open(path, "r", encoding="ascii") as stream:
            return stream.read().strip()
    except OSError:
        return None


def fail(message):
    print(f"cf20-raw-pipewire-camera: {message}", file=sys.stderr)
    raise SystemExit(1)


def verify_safe_runtime():
    if os.path.isdir("/sys/module/ipu3_imgu"):
        fail("ipu3_imgu is loaded; refusing to start")
    for parameter, unsafe in (
        ("cf20_windows_firmware", {"Y", "1"}),
        ("cf20_css_streaming", {"Y", "1"}),
    ):
        value = read_sysfs(f"/sys/module/ipu3_imgu/parameters/{parameter}")
        if value in unsafe:
            fail(f"unsafe ImgU parameter {parameter}={value}")


def find_cio2_device():
    for name_path in sorted(glob.glob("/sys/class/video4linux/video*/name")):
        if read_sysfs(name_path) != "ipu3-cio2 0":
            continue
        video_name = os.path.basename(os.path.dirname(name_path))
        device = f"/dev/{video_name}"
        if os.path.exists(device):
            return device
    fail("no ipu3-cio2 0 raw capture device found")


def terminate_process(process, name):
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            print(f"cf20-raw-pipewire-camera: killing stuck {name}", file=sys.stderr)
            process.kill()
            process.wait()
    if process.stderr is not None:
        try:
            diagnostic = process.stderr.read()
        except OSError:
            diagnostic = b""
        if diagnostic:
            text = diagnostic.decode("utf-8", errors="replace").strip()
            if text:
                print(f"cf20-raw-pipewire-camera: {name}: {text}", file=sys.stderr)


def run_capture(device, stop_requested):
    capture = None
    unpacker = None
    rgb_converter = None
    sink = None
    try:
        capture = subprocess.Popen(
            [CAPTURE, device], stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0
        )
        unpacker = subprocess.Popen(
            [UNPACKER],
            stdin=capture.stdout,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        capture.stdout.close()
        rgb_converter = subprocess.Popen(
            [RGB_CONVERTER],
            stdin=unpacker.stdout,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        sink = subprocess.Popen(
            [SINK], stdin=rgb_converter.stdout, stdout=subprocess.DEVNULL,
            # Keep the native PipeWire source diagnostics in the service
            # journal immediately.  Buffering them in a pipe until teardown
            # made format/consumer failures look like silent black video and
            # could eventually deadlock a noisy provider.
            stderr=None,
        )
        unpacker.stdout.close()
        rgb_converter.stdout.close()
        print(f"cf20-raw-pipewire-camera: publishing {device}", flush=True)

        return_code = 1
        failed_component = None
        while not stop_requested[0]:
            try:
                return_code = sink.wait(timeout=1)
                failed_component = "pipewire source"
                break
            except subprocess.TimeoutExpired:
                for child, name in (
                    (capture, "raw capture"),
                    (unpacker, "ip3b unpacker"),
                    (rgb_converter, "Bayer-to-RGB converter"),
                ):
                    child_code = child.poll()
                    if child_code is not None:
                        return_code = child_code if child_code != 0 else 1
                        failed_component = name
                        print(
                            f"cf20-raw-pipewire-camera: {name} exited "
                            f"unexpectedly ({child_code})",
                            file=sys.stderr,
                            flush=True,
                        )
                        break
                if failed_component is not None:
                    break
        else:
            return_code = 0
        if return_code != 0:
            print(
                f"cf20-raw-pipewire-camera: {failed_component or 'pipeline'} "
                f"exited with {return_code}",
                file=sys.stderr,
            )
        return return_code
    finally:
        terminate_process(sink, "pipewire sink")
        terminate_process(rgb_converter, "Bayer-to-RGB converter")
        terminate_process(unpacker, "unpacker")
        terminate_process(capture, "capture")


def main():
    verify_safe_runtime()
    for helper in (CAPTURE, UNPACKER, RGB_CONVERTER, SINK):
        if not os.access(helper, os.X_OK):
            fail(f"missing executable helper {helper}")
    device = os.environ.get("CF20_RAW_VIDEO_DEVICE") or find_cio2_device()
    stop_requested = [False]

    def stop(_signum, _frame):
        stop_requested[0] = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    restart_count = 0
    restart_started = 0.0
    while not stop_requested[0]:
        run_capture(device, stop_requested)
        if stop_requested[0]:
            return 0
        now = time.monotonic()
        if now - restart_started > RESTART_WINDOW:
            restart_count = 0
        if restart_count == 0:
            restart_started = now
        restart_count += 1
        if restart_count > MAX_RESTARTS:
            fail("repeated isolated PipeWire failures; stopping fail-closed")
        print(
            f"cf20-raw-pipewire-camera: restarting isolated sink "
            f"({restart_count}/{MAX_RESTARTS})",
            file=sys.stderr,
            flush=True,
        )
        time.sleep(RESTART_DELAY)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
