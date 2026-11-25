# Kernel source payload

These files are the CF-20-specific or CF-20-adjusted sources used when the
release package was built. They are grouped by their upstream kernel
location, but are intentionally kept separate from a complete Linux kernel
checkout.

The package build requires a matching kernel source/build tree and the
PipeWire development headers. The development build helper currently accepts
those locations through its `CF20_*` environment variables; the checked-in
Debian package is the easier installation path for the tested
`7.3.4+deb13-amd64` kernel.

The `ipu3-imgu` sources and `cf20-css_fw.bin` are retained for auditability and
contained diagnostics. The installed release keeps ImgU/CSS disabled and uses
the raw CIO2 path.
