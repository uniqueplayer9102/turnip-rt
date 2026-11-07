# Radish

Experimental **compute-first** Vulkan driver for Adreno 5xx, targeting on-device
inference. Graphics support exists but is secondary; the GPU budget at run time is
compute.

This branch carries the radish driver work. The inherited turnip-rt ray-tracing
spike is preserved on the [`old/main`](../../tree/old/main) branch.

Driver name as reported by Vulkan:

```
Radish Adreno 506 (experimental compute)
```

## Source layout

The driver lives in Mesa's turnip-rt tree:

```
src/freedreno/radish/
    rd_private.h      shared structures and prototypes
    rd_command.c      command buffer recording
    rd_device.c       device creation, queue, GMEM query
    rd_instance.c     physical device, extensions, DRM node properties
    rd_pipeline.c     ir3 pipeline compilation
    rd_pipeline_cache.cpp  pipeline cache
    rd_graphics.c     dynamic rendering: sysmem (bypass) and GMEM paths
    rd_queue.c        submit sequencing and the submit window
    rd_memory.c       BO allocation, dmabuf import/export
    rd_image.c        image layout, DRM format modifiers
    rd_resources.c    resource tracking
    rd_descriptor.c   descriptor sets
    rd_sampling.c     texture sampling lowering
    rd_nir_resources.c  resource lowering pass
    rd_nir_robust.c   bounds-check lowering pass
    rd_transfer.c     copies and clears
    rd_query.c        queries
    rd_event.c        events
    rd_trace.c        tracing
```

Made-for reference, used throughout:

```
src/gallium/drivers/freedreno/a5xx/    fd5_emit.c, fd5_gmem.c, fd5_program.c
src/freedreno/common/                   fd5_hw.h register defines
src/freedreno/registers/adreno/a5xx.xml register semantics
```

## Building

Radish is a Vulkan driver inside Mesa that talks to the Adreno DRM device
through freedreno's `fd_pipe`/`fd_device` layer. It is selected with the
`radish` vulkan driver:

```sh
meson setup build -Dvulkan-drivers=radish -Dplatforms=wayland
ninja -C build/ src/freedreno/radish/libvulkan_radish.so
```

The resulting library installs as `libvulkan_radish.so` and is selected through
the Vulkan loader's ICD JSON, e.g. `VK_ICD_FILENAMES=/path/to/radish_icd.json`.

Radish is in `needs_flex_bison`, so the radish build pulls in flex and bison
like the other Vulkan drivers.

## Runtime options

| Variable | Effect |
| --- | --- |
| `RADISH_GRAPHICS=1` | enable queue families with graphics support (required by the test harness) |
| `RADISH_DEBUG_ERRORS=1` | log validation failures to stderr; also prints the queried GMEM size and base at device creation |
| `RADISH_DEBUG_QUERIES=1` | log query activity |
| `RADISH_DEBUG=1` | broader debug output |
| `RADISH_GMEM=0` | force the sysmem (bypass) render path instead of GMEM |
| `RADISH_GMEM_DUMP=1` | print tile layout for each GMEM render |
| `RADISH_GMEM_STOP=<line>` | flush and return after a given stage; bisection helper |
| `RADISH_ASYNC_QUEUE_SYNC=1` | queue sync behaviour used by the compositor setup |

GMEM is the default for passes it accepts. A running compositor can fall back
to bypass without a restart by creating a marker file:

```sh
touch /home/user/.local/radish/gmem_disabled   # bypass path
rm /home/user/.local/radish/gmem_disabled      # GMEM path
```

`gmem_disabled` wins over the environment default, so it also reverts a
process started without `RADISH_GMEM=0`.

Only passes that carry a single color attachment at one sample with no
depth/stencil, no queries, no indirect draws and no barriers take the GMEM
path; everything else keeps using bypass.

## Supported extensions

```
KHR_device_group                  KHR_descriptor_update_template
KHR_maintenance3                  KHR_bind_memory2
KHR_get_memory_requirements2      KHR_external_memory
KHR_external_memory_fd            KHR_external_semaphore
KHR_external_semaphore_fd         EXT_external_memory_dma_buf
EXT_queue_family_foreign         KHR_synchronization2
KHR_image_format_list            EXT_image_drm_format_modifier
EXT_physical_device_drm           KHR_storage_buffer_storage_class
KHR_16bit_storage                KHR_8bit_storage
KHR_shader_float16_int8           KHR_relaxed_block_layout
KHR_sampler_mirror_clamp_to_edge  EXT_host_query_reset
```

External semaphore export is dynamic: `KHR_external_semaphore[_fd]` is only
advertised when the render node supports sync file import and export.

Push constants go to 256 bytes (Vulkan's minimum is 128), sized through
`RD_MAX_PUSH_CONSTANTS_SIZE`.

## Hardware

Portable across Adreno 5xx; validated on the Adreno 506 (MSM8953) of the Xiaomi
Redmi 5 Plus (`vince`), driving a 1080x2160 DSI panel under Phoc/Phosh.

A5xx caps that shape the render path:

| Property | Value |
| --- | --- |
| GMEM size | 0x22000 |
| GMEM base | 0x100000 |
| tile_align_w | 64 |
| tile_align_h | 32 |
| tile_max_w | 1024 |

## GMEM vs sysmem, measured

Both paths were timed by submitting a clear plus stacked triangle draws to a
1080x2160 RGBA8 attachment and waiting on each submit, so the figure is
per-submit GPU work rather than one-shot pipeline compile time. Adreno 506,
17x4 tiles, 64x544 bins:

| Workload | sysmem | GMEM | Delta |
| --- | --- | --- | --- |
| clear + store only | 4.0 ms | 5.3 ms | GMEM slower |
| 512 triangles/submit | 419.7, 430.0, 418.2 ms | 354.6, 355.8, 355.8 ms | GMEM 15.5% faster |
| 1024 triangles/submit | 855.5 ms | 705.7 ms | GMEM 17.5% faster |

Readback pixels matched byte for byte between the two paths, and no GPU faults
were logged across any run.

The clear-only case being slower is expected: with nothing to amortise them,
the per-tile restore and store blits cost more than they save. The win shows
up as soon as there are real draws, which is why GMEM is the default for the
passes it accepts.

## Related repository

Docs, CTS ledgers and tablet scripts live separately in
[jethac/radish](https://github.com/jethac/radish).
