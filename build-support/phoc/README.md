# Phoc one-pass SDR experiment

This directory contains an experimental wlroots patch and test tools for Phoc
0.58.0 / wlroots 0.20.2. It is kept separate from the Radish driver because it
changes the compositor, not Mesa.

The default wlroots Vulkan renderer blends into an FP16 attachment and then
runs a second subpass to apply the output color transform. `wlroots-one-pass-sdr.patch`
adds two opt-in environment switches:

| Variable | Effect |
| --- | --- |
| `PHOC_VULKAN_ONE_PASS_SDR=1` | Use the direct sRGB framebuffer when available. One render pass, hardware sRGB conversion. |
| `PHOC_VULKAN_ONE_PASS_RAW=1` | Use a direct non-sRGB framebuffer and leave default gamma-encoded client content unchanged. One render pass, no gamma shader or sRGB framebuffer. |

Explicit color transforms still use the original two-pass path. Both switches
default to off, so an unpatched Phoc behaves normally.

## Build

The patch is against the wlroots subproject inside the Phoc 0.58.0 release
tarball. Apply it to `subprojects/wlroots-0.20.x` and build with the same flags
as Alpine's `phoc` package:

```sh
patch -p1 -d subprojects/wlroots-0.20.x < wlroots-one-pass-sdr.patch
meson setup output --prefix=/usr \
   -Db_lto=true -Dembed-wlroots=enabled -Dtests=false -Dman=false \
   --default-library=static --wrap-mode=nodownload
ninja -C output src/phoc
```

`sdr-color-probe.c` is a small Wayland client that draws a deterministic
gradient and gamma strip. Build it against `wayland-client` and
`xdg-shell-client-protocol.[ch]`. `compare-ppm.py` compares two `grim -t ppm`
captures and reports mean/p95/max absolute differences per channel.

`present-stream.c` commits every frame callback instead of waiting for
presentation feedback before submitting the next frame. It measures actual
vsync intervals and avoids the extra misses caused by feedback-gated probes.
Build it with `presentation-time-client-protocol.[ch]` as well.

## Validation

On an Adreno 506 with Radish, the raw one-pass output was pixel-identical to
GLES2 in a 1080x2160 headless capture: 0 of 6,998,400 channel values changed.
The sRGB variant differed slightly from both GLES2 and the original Vulkan
output. In 300-frame headless presentation tests, raw mode was around
16.9–17.2 ms mean interval versus 17.1–17.3 ms for the default two-pass path;
GLES2 was around 16.7–16.9 ms. Raw mode also used less CPU time in most runs.

Use the same staged compositor for all A/B runs; do not change the ICD,
compositor build, or display mode mid-run. These are headless results; verify
on the real panel before changing login defaults.
