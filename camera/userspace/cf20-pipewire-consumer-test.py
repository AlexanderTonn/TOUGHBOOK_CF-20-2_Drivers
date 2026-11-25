#!/usr/bin/python3
"""Controlled, headless PipeWire consumer test for the CF-20 source.

This connects only a GStreamer fakesink.  It does not open V4L2, libcamera,
the GNOME camera application, or any kernel module.
"""

import os
import sys

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst, GObject


TARGET = os.environ.get("CF20_TARGET_OBJECT", "cf20-ov8858-raw")
SECONDS = 6


def main():
    Gst.init(None)
    provider_factory = Gst.DeviceProviderFactory.find("pipewiredeviceprovider")
    if provider_factory is None:
        print("PIPEWIRE_CONSUMER=DEVICE_PROVIDER_MISSING", file=sys.stderr)
        return 2
    provider = GObject.new(provider_factory.load().get_device_provider_type())
    if provider is None or not provider.start():
        print("PIPEWIRE_CONSUMER=DEVICE_PROVIDER_START_FAILED", file=sys.stderr)
        return 2

    context = GLib.MainContext.default()
    source = None
    deadline = GLib.get_monotonic_time() + 4 * GLib.USEC_PER_SEC
    while GLib.get_monotonic_time() < deadline and source is None:
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
            if node_name == TARGET:
                source = device.create_element("cf20-consumer-source")
                break
        if source is None:
            GLib.usleep(50000)

    sink = Gst.ElementFactory.make("fakesink", "sink")
    if source is None:
        provider.stop()
        print("PIPEWIRE_CONSUMER=DEVICE_NOT_FOUND", file=sys.stderr)
        return 1
    if sink is None:
        provider.stop()
        print("PIPEWIRE_CONSUMER=ELEMENT_MISSING", file=sys.stderr)
        return 2

    try:
        target = source.get_property("target-object")
    except (AttributeError, TypeError):
        target = "<unreadable>"
    try:
        fd = source.get_property("fd")
    except (AttributeError, TypeError):
        fd = "<unreadable>"
    override = os.environ.get("CF20_TARGET_OBJECT")
    if override:
        source.set_property("target-object", override)
        target = source.get_property("target-object")
    print(f"PIPEWIRE_CONSUMER_PROVIDER_TARGET={target}")
    print(f"PIPEWIRE_CONSUMER_PROVIDER_FD={fd}")
    sink.set_property("sync", False)
    sink.set_property("signal-handoffs", True)

    pipeline = Gst.Pipeline.new("cf20-pipewire-consumer-test")
    pipeline.add(source)
    pipeline.add(sink)
    if not source.link(sink):
        provider.stop()
        print("PIPEWIRE_CONSUMER=LINK_FAILED", file=sys.stderr)
        return 2

    frames = [0]

    def on_handoff(_sink, _buffer, _pad):
        frames[0] += 1

    sink.connect("handoff", on_handoff)

    loop = GLib.MainLoop()
    error_text = [None]
    bus = pipeline.get_bus()
    bus.add_signal_watch()

    def on_message(_bus, message):
        if message.type == Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            error_text[0] = str(error)
            if debug:
                print(f"GSTREAMER_DEBUG={debug}", file=sys.stderr)
            loop.quit()
        elif message.type == Gst.MessageType.EOS:
            loop.quit()

    bus.connect("message", on_message)
    result = pipeline.set_state(Gst.State.PLAYING)
    if result == Gst.StateChangeReturn.FAILURE:
        # A source provider can report the useful PipeWire error on the bus
        # after set_state() returns.  Drain it before reporting the generic
        # PLAY_FAILED result.
        while True:
            message = bus.timed_pop_filtered(
                200 * Gst.MSECOND,
                Gst.MessageType.ERROR | Gst.MessageType.WARNING,
            )
            if message is None:
                break
            if message.type == Gst.MessageType.ERROR:
                error, debug = message.parse_error()
                print(f"GSTREAMER_ERROR={error}", file=sys.stderr)
                if debug:
                    print(f"GSTREAMER_DEBUG={debug}", file=sys.stderr)
            elif message.type == Gst.MessageType.WARNING:
                warning, debug = message.parse_warning()
                print(f"GSTREAMER_WARNING={warning}", file=sys.stderr)
                if debug:
                    print(f"GSTREAMER_DEBUG={debug}", file=sys.stderr)
        print("PIPEWIRE_CONSUMER=PLAY_FAILED", file=sys.stderr)
        pipeline.set_state(Gst.State.NULL)
        provider.stop()
        return 1

    GLib.timeout_add_seconds(SECONDS, loop.quit)
    try:
        loop.run()
    finally:
        pipeline.set_state(Gst.State.NULL)
        bus.remove_signal_watch()
        provider.stop()

    pad = source.get_static_pad("src")
    caps = pad.get_current_caps() if pad is not None else None
    print(f"PIPEWIRE_CONSUMER_FRAMES={frames[0]}")
    print(f"PIPEWIRE_CONSUMER_CAPS={caps.to_string() if caps else 'NONE'}")
    if error_text[0] is not None:
        print(f"PIPEWIRE_CONSUMER_ERROR={error_text[0]}", file=sys.stderr)
        return 1
    return 0 if frames[0] > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
