# TOUGHBOOK_CF-20-2_Drivers
Drivers for the Panasonic Toughbook CF-20 2 for Linux which are not delivered by the kernel

## CF-20 OV8858 rear camera

The current camera release is in [`camera/`](camera/). It uses the safe raw
CIO2 path and deliberately does not load or stream through the IPU3 ImgU/CSS
pipeline, which was the source of the historical hangs and black screens.

The bundled first release corrects the rear-camera horizontal mirroring and
publishes a userspace Bayer-to-RGB PipeWire source. Brightness, focus,
white-balance controls and flash-light control are not part of this first
release yet.

Install the bundled release from a checkout with:

```sh
./camera/release/install-cf20-camera-0.1.0.sh
```

The installer updates the kernel modules and initramfs, starts the supervised
raw-camera service, and runs the bounded headless end-to-end test. It does not
run `libcamera`, `cam`, `media-ctl`, or a direct V4L2 capture.
