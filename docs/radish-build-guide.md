# Radish build and test guide

This is a practical guide for the `uniqueplayer9102/turnip-rt` fork. Radish is
a Vulkan driver inside Mesa; it is selected with the `radish` Vulkan driver and
talks to Adreno via freedreno's `fd_pipe`/`fd_device` layer.

Two builds are useful:

1. A **host build** in WSL for fast compile and unit-test feedback.
2. An **aarch64 build** in the pmbootstrap chroot for anything that runs on the
   Xiaomi Redmi 5 Plus (`vince`, Adreno 506).

The repository root is `/home/uni/turnip-rt`. An existing host build directory
is `/home/uni/turnip-rt-build-radish.000ecfma`; reuse it unless you need a clean
configuration.

## 1. Host build (WSL)

Prerequisites: `meson`, `ninja`, `cc`/`c++`, `bison`, `flex`, Python 3, and the
usual Mesa build dependencies. Radish is in `needs_flex_bison`, so flex and
bison are pulled in like the other Vulkan drivers.

Minimal configuration, matching the README:

```sh
cd /home/uni/turnip-rt
meson setup build -Dvulkan-drivers=radish -Dplatforms=wayland
ninja -C build/ src/freedreno/radish/libvulkan_radish.so \
                 src/freedreno/radish/radish_icd.json
```

Build only the driver in the already configured tree:

```sh
ninja -C /home/uni/turnip-rt-build-radish.000ecfma -j 6 \
      src/freedreno/radish/libvulkan_radish.so
```

The ICD JSON is a separate target. Both the library and the JSON are needed at
runtime; the JSON's `library_path` must point at the library you intend to use.

## 2. Host tests

Two layers exist:

```sh
cd /home/uni/turnip-rt
python3 src/freedreno/tests/radish_correctness_unit_test.py --include include
meson test -C /home/uni/turnip-rt-build-radish.000ecfma --suite radish --print-errorlogs
```

`radish_correctness_unit_test.py` extracts static helpers from driver sources
into temporary headers, compiles them with `cc`/`c++` under `-Wall -Wextra
-Werror`, and runs the resulting binary. Its `extract()` helper matches a
limited set of return types written on their own line; if you add a new helper
with a different return type, extend that regex. When you add a helper that
should be tested, add it to the extraction list rather than copying code into
the test file.

The Meson suite currently registers `radish-correctness_unit`,
`radish-fp16_layout`, and `radish-gmem_damage`.

## 3. aarch64 build (pmbootstrap chroot)

The phone build is driven by an APKBUILD, not by the host Meson options.

APKBUILD location:

```
/home/uni/.local/var/pmbootstrap/cache_git/pmaports/temp/mesa-radish/APKBUILD
```

Its `build()` uses:

```
-Dvulkan-drivers=radish -Dgallium-drivers= -Dplatforms=wayland,x11
-Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false
-Dglx=disabled -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled
-Dlibunwind=disabled -Dlmsensors=disabled -Dbuildtype=release
```

`makedepends` must include the X11 development packages when `x11` is enabled:
`libx11-dev`, `libxcb-dev`, `xcb-util-dev`, `libxshmfence-dev`, `libxrandr-dev`.
The Mesa Meson files map `x11` to the XCB dependencies and compile
`wsi_common_x11.c` automatically; no extra repo Meson edit is needed.

Chroot source tree:

```
/home/uni/.local/var/pmbootstrap/chroot_buildroot_aarch64/home/pmos/build/src/mesa-radish-27.0.0/
```

Sync the working tree into the chroot (never overwrite its `build/` directory):

```sh
sudo rsync -a --exclude=build/ --exclude=.git/ --exclude=subprojects/packagecache/ \
  /home/uni/turnip-rt/ \
  /home/uni/.local/var/pmbootstrap/chroot_buildroot_aarch64/home/pmos/build/src/mesa-radish-27.0.0/
```

From `/home/uni/pmbootstrap`:

```sh
python3 pmbootstrap.py chroot -b aarch64 --output=stdout -- \
  apk add libx11-dev libxcb-dev xcb-util-dev libxshmfence-dev libxrandr-dev

python3 pmbootstrap.py chroot -b aarch64 --output=stdout -- \
  sh -c 'cd /home/pmos/build/src/mesa-radish-27.0.0 && \
         meson setup --reconfigure build -Dplatforms=wayland,x11 && \
         ninja -C build src/freedreno/radish/libvulkan_radish.so'
```

Resulting library:

```
/home/uni/.local/var/pmbootstrap/chroot_buildroot_aarch64/home/pmos/build/src/mesa-radish-27.0.0/build/src/freedreno/radish/libvulkan_radish.so
```

Verify the expected WSI entrypoints are present before deploying:

```sh
aarch64-linux-gnu-strings <library> | grep -E 'vkCreateXcbSurfaceKHR|vkCreateXlibSurfaceKHR|VK_KHR_xcb_surface'
```

## 4. Deploying to the phone safely

Never overwrite `/usr/lib/libvulkan_radish.so` (the installed driver) without
explicit approval. Stage builds in a new directory instead:

```
/home/user/radish-<tag>/libvulkan_radish.so
/home/user/radish-<tag>/radish_icd.aarch64.json
```

The ICD JSON must have `library_path` pointing at that staged `.so`. Select it
per process with:

```sh
VK_ICD_FILENAMES=/home/user/radish-<tag>/radish_icd.aarch64.json \
VK_DRIVER_FILES=/home/user/radish-<tag>/radish_icd.aarch64.json <app>
```

Or change `/home/user/.phoshdebug` for the whole session — but that requires a
logout/restart, so ask first. Always make a backup and an undo script before
editing it; existing examples are `.phoshdebug.bak-*`, `undo-batch8.sh`,
`undo-syncfix.sh`, and `undo-final.sh` in `/home/user/`.

Useful phone-side checks:

```sh
vulkaninfo --summary
vkcube --wsi xcb --c 30
DISPLAY=:0 xset q
```

`vkcube --wsi wayland` and GTK4 apps with `GSK_RENDERER=vulkan` exercise the
Wayland path; `--wsi xcb` with `DISPLAY=:0` exercises Xwayland.

## 5. Diagnostics knobs

All are environment variables read at startup:

| Variable | Effect |
| --- | --- |
| `RADISH_SUBMIT_DIAG=1` | per-submit CPU line: async selection, waits, signals, marker/fence outcomes, drain waits by reason, emit/kernel/wait timings |
| `RADISH_SUBMIT_DIAG_EVERY=N` | log only every Nth submit |
| `RADISH_WAIT_TIMING=1` | detailed drain timing lines |
| `RADISH_ASYNC_QUEUE_SYNC_DEBUG=1` | log async skip/submit decisions |
| `RADISH_GPU_IMAGE_COPY_DEBUG=1` | log GPU copy dispatches |
| `RADISH_BATCH_CHUNKS=8` | batch multiple compute chunks; eligible compute now defers at any value when async sync is enabled |

`RADISH_PROFILE` and `RADISH_TRACE` disable the async path, so do not use them
for baseline frame-time comparisons.

## 6. Rules for commits

- Commit messages developed with AI must carry the trailer `Generated-by: LLM`
  (or `Assisted-by: LLM` when only assisting).
- Do not contribute to GitLab or any Mesa repository; this fork only.
- Commit only the changes you intentionally made. When another agent has
  uncommitted source edits, leave them alone unless you are asked to review and
  include them.
