#!/usr/bin/python3
import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

Gst.init(None)
monitor = Gst.DeviceMonitor()
monitor.add_filter("Video/Source", None)
monitor.start()

pipeline = Gst.parse_launch(
    "videotestsrc is-live=true pattern=ball ! "
    "video/x-raw,format=RGB,width=320,height=240,framerate=5/1 ! "
    "pipewiresink name=sink"
)
sink = pipeline.get_by_name("sink")
sink.set_property("client-name", "CF-20 PipeWire device-test")
props = Gst.Structure.new_empty("props")
for key, value in {
    "media.class": "Video/Source",
    "media.category": "Capture",
    "media.role": "Camera",
    "node.name": "cf20-device-test",
    "node.description": "CF-20 device-provider synthetic camera test",
}.items():
    props.set_value(key, value)
sink.set_property("stream-properties", props)
pipeline.set_state(Gst.State.PLAYING)
loop = GLib.MainLoop()
GLib.timeout_add(2000, lambda: (loop.quit() or False))
loop.run()
pipeline.set_state(Gst.State.NULL)
devices = monitor.get_devices()
for device in devices:
    if "CF-20 device-provider synthetic" in device.get_display_name():
        print("DEVICE_PROVIDER_SEES_CF20_SYNTHETIC=YES", flush=True)
        break
else:
    print("DEVICE_PROVIDER_SEES_CF20_SYNTHETIC=NO", flush=True)
monitor.stop()
