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

Radish is a drm-shim-free Vulkan driver inside Mesa, selected with the
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
| `RADISH_GMEM=1` | enable the opt-in color-only GMEM (tiled) render path |
| `RADISH_GMEM_DUMP=1` | print tile layout for each GMEM render |
| `RADISH_GMEM_STOP=<line>` | flush and return after a given stage; bisection helper |
| `RADISH_ASYNC_QUEUE_SYNC=1` | queue sync behaviour used by the compositor setup |

GMEM can also be enabled at run time without restarting a live compositor by
creating a marker file:

```sh
touch /home/user/.local/radish/gmem_enabled   # enable
rm /home/user/.local/radish/gmem_enabled      # disable, back to sysmem
```

The env var and the marker file are OR'd, so either enables the path.

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

## Related repository

Docs, CTS ledgers and tablet scripts live separately in
[jethac/radish](https://github.com/jethac/radish).
