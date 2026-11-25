#!/usr/bin/python3
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

Gst.init(None)
pipeline = Gst.parse_launch(
    "videotestsrc name=source is-live=true pattern=ball ! "
    "video/x-raw,format=RGB,width=320,height=240,framerate=5/1 ! "
    "pipewiresink name=sink"
)
sink = pipeline.get_by_name("sink")
sink.set_property("client-name", "CF-20 PipeWire synthetic test")
props = Gst.Structure.new_empty("props")
for key, value in {
    "media.class": "Video/Source",
    "media.category": "Capture",
    "media.role": "Camera",
    "node.name": "cf20-synthetic-test",
    "node.description": "CF-20 synthetic camera test",
}.items():
    props.set_value(key, value)
sink.set_property("stream-properties", props)

loop = GLib.MainLoop()
bus = pipeline.get_bus()
bus.add_signal_watch()


def on_message(_bus, message):
    if message.type == Gst.MessageType.ERROR:
        error, debug = message.parse_error()
        print(f"ERROR: {error}", file=sys.stderr)
        if debug:
            print(debug, file=sys.stderr)
        loop.quit()
    elif message.type == Gst.MessageType.EOS:
        loop.quit()


bus.connect("message", on_message)
if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
    print("ERROR: could not start synthetic PipeWire source", file=sys.stderr)
    pipeline.set_state(Gst.State.NULL)
    raise SystemExit(1)
print("synthetic PipeWire source started", flush=True)
GLib.timeout_add(3000, lambda: (loop.quit() or False))
try:
    loop.run()
finally:
    pipeline.set_state(Gst.State.NULL)
print("synthetic PipeWire source stopped", flush=True)
