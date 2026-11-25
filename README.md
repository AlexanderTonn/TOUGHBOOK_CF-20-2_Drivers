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

## Camera driver feature status

| Function | Status | Notes |
|---|---|---|
| OV8858 sensor detection and power sequencing | Available | CF-20-specific TPS68470 rails, GPIOs and clock handling are included. |
| Raw CIO2 capture | Available | Direct MMAP capture from the IPU3 CIO2 path. |
| IPU3 `ip3b` unpacking | Available | 10-bit packed Bayer frames are unpacked in userspace. |
| Bayer-to-RGB conversion | Available | BGGR10 demosaic and 640×480 RGB output for PipeWire clients. |
| Horizontal image mirroring | Available | The rear-camera image is corrected in the RGB conversion stage. |
| PipeWire camera source | Available | Published as a persistent `Video/Source` camera node. |
| Debian Camera/Snapshot integration | Available | PipeWire/GStreamer provider path is included and tested headlessly. |
| ImgU/CSS safety containment | Available | ImgU is blocked by default; the raw path avoids the historical freezes. |
| Brightness and exposure controls | In development | Sensor controls exist in the driver work; desktop-facing control mapping is still needed. |
| Focus / VCM control | In development | The VCM is detected, but user-facing focus control is not yet exposed. |
| Automatic white balance | In development | Current release applies fixed userspace processing; a proper control/algorithm is pending. |
| Flash-light control | In development | CF-20 flash GPIO/control mapping is not yet implemented. |

Install the bundled release from a checkout with:

```sh
./camera/release/install-cf20-camera-0.1.0.sh
```

The installer updates the kernel modules and initramfs, starts the supervised
raw-camera service, and runs the bounded headless end-to-end test. It does not
run `libcamera`, `cam`, `media-ctl`, or a direct V4L2 capture.
