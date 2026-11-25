#!/usr/bin/python3
"""Isolated GStreamer/PipeWire sink for the CF-20 raw camera bridge.

This process deliberately owns all GStreamer state.  The parent process keeps
the V4L2 capture and can restart this process if a PipeWire consumer exposes a
GStreamer/pipewiresink failure.
"""

import os
import signal
import sys

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstApp", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import GLib, Gst, GstApp


WIDTH = 1632
HEIGHT = 1224
FPS = 15
FRAME_OUT = WIDTH * HEIGHT * 2


def make_pipeline():
    pipeline = Gst.Pipeline.new("cf20-raw-camera")
    source = Gst.ElementFactory.make("appsrc", "raw-source")
    raw_caps = Gst.Caps.from_string(
        "video/x-bayer,format=bggr10le,width=1632,height=1224,framerate=15/1"
    )
    source.set_property("caps", raw_caps)
    source.set_property("is-live", True)
    source.set_property("block", False)
    source.set_property("max-bytes", FRAME_OUT * 2)
    source.set_property("leaky-type", 2)
    source.set_property("emit-signals", False)
    source.set_property("format", Gst.Format.TIME)
    source.set_property("do-timestamp", False)

    debayer = Gst.ElementFactory.make("bayer2rgb", "debayer")
    rate = Gst.ElementFactory.make("videorate", "rate")
    scale = Gst.ElementFactory.make("videoscale", "scale")
    convert = Gst.ElementFactory.make("videoconvert", "convert")
    output_filter = Gst.ElementFactory.make("capsfilter", "output-format")
    output_filter.set_property(
        "caps",
        Gst.Caps.from_string(
            "video/x-raw,format=RGB,width=640,height=480,framerate=15/1"
        ),
    )
    sink = Gst.ElementFactory.make("pipewiresink", "camera-source")
    sink.set_property("client-name", "CF-20 OV8858 raw camera")
    # This is a persistent camera source, not a render sink.  The provide
    # mode creates the source-side stream and deliberately keeps the provider
    # alive when a consumer connects or disconnects.
    sink.set_property("mode", "provide")
    # A provider must expose an active source-side pool when a consumer
    # negotiates its format.
    sink.set_property("use-bufferpool", True)
    print(
        "cf20-pipewire-sink: mode=%s use-bufferpool=%s"
        % (sink.get_property("mode"), sink.get_property("use-bufferpool")),
        flush=True,
    )
    properties = Gst.Structure.new_empty("props")
    for key, value in {
        "media.class": "Video/Source",
        "media.category": "Capture",
        "media.role": "Camera",
        "node.name": "cf20-ov8858-raw",
        "node.description": "Panasonic CF-20 rear camera (RAW CIO2)",
        "object.register": "true",
        "object.export": "true",
        "node.always-process": "true",
    }.items():
        properties.set_value(key, value)
    sink.set_property("stream-properties", properties)

    elements = [source, debayer, rate, scale, convert, output_filter, sink]
    if any(element is None for element in elements):
        raise RuntimeError("required GStreamer element is not installed")
    for element in elements:
        pipeline.add(element)
    for left, right in zip(elements, elements[1:]):
        if not left.link(right):
            raise RuntimeError(f"could not link {left.get_name()} to {right.get_name()}")
    return pipeline, source


class SinkReader:
    def __init__(self, source, loop):
        self.source = source
        self.loop = loop
        self.watch_id = 0
        self.frame_buffer = bytearray()
        self.frame_number = 0
        self.stopping = False
        self.failed = False

    def start(self):
        os.set_blocking(sys.stdin.fileno(), False)
        self.watch_id = GLib.io_add_watch(
            sys.stdin.fileno(),
            GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR | GLib.IO_NVAL,
            self._on_io,
        )
        if not self.watch_id:
            raise RuntimeError("could not attach raw input to the GLib main loop")

    def _on_io(self, _source, condition):
        if self.stopping:
            self.watch_id = 0
            return False
        try:
            chunk = os.read(sys.stdin.fileno(), FRAME_OUT - len(self.frame_buffer))
        except BlockingIOError:
            return True
        except OSError as error:
            self.failed = True
            self.stopping = True
            print(f"cf20-pipewire-sink: raw input failed: {error}", file=sys.stderr)
            self.loop.quit()
            self.watch_id = 0
            return False

        if not chunk:
            self.stopping = True
            self.loop.quit()
            self.watch_id = 0
            return False

        self.frame_buffer.extend(chunk)
        if len(self.frame_buffer) < FRAME_OUT:
            if condition & (GLib.IO_HUP | GLib.IO_ERR | GLib.IO_NVAL):
                self.failed = True
                self.stopping = True
                print(
                    f"cf20-pipewire-sink: truncated raw frame ({len(self.frame_buffer)} of {FRAME_OUT})",
                    file=sys.stderr,
                )
                self.loop.quit()
                self.watch_id = 0
                return False
            return True

        frame = bytes(self.frame_buffer)
        self.frame_buffer.clear()
        buffer = Gst.Buffer.new_allocate(None, len(frame), None)
        buffer.fill(0, frame)
        timestamp = Gst.util_uint64_scale_int(self.frame_number, Gst.SECOND, FPS)
        buffer.pts = timestamp
        buffer.dts = timestamp
        buffer.duration = Gst.util_uint64_scale_int(1, Gst.SECOND, FPS)
        # Importing GstApp is required for PyGObject to attach the typed
        # GstAppSrc methods to the object returned by ElementFactory.make.
        # Calling source.push_buffer() without that import raises
        # AttributeError in Debian's GI bindings and leaves consumers waiting
        # forever for their first frame.
        result = GstApp.AppSrc.push_buffer(self.source, buffer)
        if result != Gst.FlowReturn.OK:
            self.failed = True
            self.stopping = True
            print(
                f"cf20-pipewire-sink: GStreamer rejected raw frame: {result.value_nick}",
                file=sys.stderr,
            )
            self.loop.quit()
            self.watch_id = 0
            return False
        self.frame_number += 1
        return not self.stopping

    def stop(self):
        self.stopping = True
        if self.watch_id:
            GLib.source_remove(self.watch_id)
            self.watch_id = 0


def main():
    Gst.init(None)
    pipeline, source = make_pipeline()
    loop = GLib.MainLoop()
    reader = SinkReader(source, loop)
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    failed = False

    def on_message(_bus, message):
        nonlocal failed
        if message.type == Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            print(
                f"cf20-pipewire-sink: GStreamer error: {error}"
                + (f" ({debug})" if debug else ""),
                file=sys.stderr,
            )
            failed = True
            reader.stopping = True
            loop.quit()
        elif message.type == Gst.MessageType.EOS:
            reader.stopping = True
            loop.quit()

    bus.connect("message", on_message)

    def stop(_signum, _frame):
        reader.stopping = True
        loop.quit()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
        pipeline.set_state(Gst.State.NULL)
        print("cf20-pipewire-sink: could not start pipeline", file=sys.stderr)
        return 1

    print("cf20-pipewire-sink: PipeWire source active", flush=True)
    try:
        reader.start()
        loop.run()
    finally:
        reader.stop()
        bus.remove_signal_watch()
        pipeline.set_state(Gst.State.NULL)
    return 1 if failed or reader.failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"cf20-pipewire-sink: {error}", file=sys.stderr)
        raise SystemExit(1)
