# CF-20 OV8858 camera release

This directory contains the Panasonic Toughbook CF-20 rear-camera driver
sources, the safe raw-CIO2 userspace bridge, extracted CSS firmware, and a
reproducible bundled Debian release.

## Install

From the repository root:

```sh
./camera/release/install-cf20-camera-0.1.0.sh
```

The script installs `cf20-camera-driver_0.1.0-237_amd64.deb`, refreshes the
running-kernel module index and initramfs, starts the protected raw-camera
services, and runs `cf20-camera-final-test`.

The bundled package is built for the development/test kernel
`7.3.4+deb13-amd64`. For another kernel, use the sources and the build tool
under `tools/` to produce a matching package before installing it.

The release path is:

```text
OV8858 -> IPU3 CIO2 raw capture -> ip3b unpack -> BGGR10 demosaic/RGB
        -> PipeWire Video/Source -> desktop camera consumers
```

The IPU3 ImgU/CSS module is shipped only for contained diagnostics and is
blocked by default. The camera app therefore cannot activate the unsafe CSS
streaming path that previously caused freezes and black screens.

## Current release scope

Included:

- horizontal correction for the CF-20 rear-camera image;
- direct CIO2 MMAP capture without libcamera or ImgU;
- IPU3 `ip3b` unpacking and userspace BGGR10-to-RGB conversion;
- PipeWire source publication with isolated consumer handling;
- bounded native and GStreamer/PipeWire end-to-end tests;
- OV8858 sensor power sequencing and Windows-derived CSS firmware artifact.

Not yet included:

- user controls for brightness/exposure;
- focus/VCM control exposed to the desktop app;
- automatic white-balance controls exposed to the desktop app;
- flash-light GPIO/control integration.

## Layout

- `kernel/` — CF-20 kernel driver sources and the IPU3 support sources used by
  the package build;
- `userspace/` — raw capture, unpacking, demosaic, PipeWire, and tests;
- `firmware/` — extracted CF-20 CSS firmware artifact;
- `release/` — installable Debian package and executable installer;
- `tools/` — package build and WirePlumber integration files.
