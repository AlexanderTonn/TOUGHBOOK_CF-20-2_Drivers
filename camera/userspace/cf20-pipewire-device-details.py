#!/usr/bin/python3
"""Read-only inspection of the PipeWire camera device-provider metadata."""

import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst, GObject


TARGET = "cf20-ov8858-raw"


def main():
    Gst.init(None)
    factory = Gst.DeviceProviderFactory.find("pipewiredeviceprovider")
    if factory is None:
        print("DEVICE_PROVIDER=NOT_FOUND", file=sys.stderr)
        return 2
    provider = GObject.new(factory.load().get_device_provider_type())
    if provider is None or not provider.start():
        print("DEVICE_PROVIDER=START_FAILED", file=sys.stderr)
        return 2

    try:
        context = GLib.MainContext.default()
        deadline = time.monotonic() + 4.0
        while time.monotonic() < deadline:
            while context.pending():
                context.iteration(False)
            for device in provider.get_devices():
                properties = device.get_properties()
                node_name = ""
                if properties is not None:
                    try:
                        node_name = str(properties.get_value("node.name") or "")
                    except (AttributeError, TypeError):
                        pass
                if node_name != TARGET:
                    continue

                print(f"DEVICE_DISPLAY={device.get_display_name()}")
                print(f"DEVICE_CLASS={device.get_device_class()}")
                print(
                    "DEVICE_PROPERTIES="
                    + (properties.to_string() if properties is not None else "NONE")
                )
                element = device.create_element("cf20-device-details")
                if element is None:
                    print("DEVICE_ELEMENT=NONE")
                    return 1
                print(f"DEVICE_ELEMENT={element.get_factory().get_name()}")
                for prop in element.list_properties():
                    if "target" in prop.name or "object" in prop.name:
                        try:
                            value = element.get_property(prop.name)
                        except (AttributeError, TypeError):
                            value = "<unreadable>"
                        print(f"ELEMENT_PROPERTY_{prop.name}={value}")
                return 0
            time.sleep(0.05)

        print("DEVICE=NOT_FOUND", file=sys.stderr)
        return 1
    finally:
        provider.stop()


if __name__ == "__main__":
    sys.exit(main())
