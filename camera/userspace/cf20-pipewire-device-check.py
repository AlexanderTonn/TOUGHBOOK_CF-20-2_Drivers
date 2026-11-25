#!/usr/bin/python3
"""Read-only check that GStreamer exposes the CF-20 PipeWire node as a camera.

This only enumerates Gst.DeviceProvider devices.  It does not create a
source element, start a pipeline, touch V4L2, or request a camera frame.
"""

import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst, GObject


TARGET_NODE = "cf20-ov8858-raw"
TARGET_TEXT = "Panasonic CF-20 rear camera"


def property_string(properties, name):
    if properties is None:
        return ""
    try:
        value = properties.get_value(name)
    except (AttributeError, TypeError):
        return ""
    return "" if value is None else str(value)


def main():
    Gst.init(None)
    factory = Gst.DeviceProviderFactory.find("pipewiredeviceprovider")
    if factory is None:
        print("PIPEWIRE_DEVICE_PROVIDER=ERROR", file=sys.stderr)
        return 2
    factory = factory.load()
    provider = GObject.new(factory.get_device_provider_type())
    if provider is None or not provider.start():
        print("PIPEWIRE_DEVICE_PROVIDER=ERROR", file=sys.stderr)
        return 2

    try:
        # DeviceProvider discovery is asynchronous.  Iterate the default
        # context briefly, without opening or starting any device.
        context = GLib.MainContext.default()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            while context.pending():
                context.iteration(False)
            devices = provider.get_devices()
            for device in devices:
                properties = device.get_properties()
                display = device.get_display_name() or ""
                node = property_string(properties, "node.name")
                media_class = property_string(properties, "media.class")
                print(
                    f"GST_DEVICE display={display!r} node={node!r} "
                    f"media.class={media_class!r}"
                )
                if node == TARGET_NODE or TARGET_TEXT in display:
                    print("PIPEWIRE_DEVICE_PROVIDER=YES")
                    return 0
            time.sleep(0.05)

        print("PIPEWIRE_DEVICE_PROVIDER=NO", file=sys.stderr)
        return 1
    finally:
        provider.stop()


if __name__ == "__main__":
    sys.exit(main())
