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
    rd_private.h        shared structures and prototypes
    rd_command.c        command buffer recording
    rd_device.c         device creation, queue, GMEM query
    rd_instance.c       physical device, extensions, DRM node properties
    rd_pipeline.c       ir3 pipeline compilation
    rd_pipeline_cache.cpp  pipeline cache
    rd_graphics.c       render execution: sysmem (bypass), GMEM, tiled depth/stencil
    rd_queue.c          submit sequencing, submit window, GPU transfer dispatch
    rd_memory.c         BO allocation, dmabuf import/export
    rd_image.c          image layout, DRM format modifiers, image transfers
    rd_image.h          image/transfer structures
    rd_resources.c      resource and sampler descriptor emission
    rd_descriptor.c     descriptor sets
    rd_sampling.c       software texture-sampling fallback preparation
    rd_sampling.h       sampling key structures
    rd_nir_resources.c  resource lowering pass
    rd_nir_sampling.c   software sampling lowering pass
    rd_nir_robust.c     bounds-check lowering pass
    rd_transfer.c       buffer copies, fills, updates
    rd_format.c         format translation tables
    rd_format.h         format translation prototypes
    rd_query.c          queries
    rd_event.c          events
    rd_wsi.c            WSI integration
    rd_trace.c          GPU/CPU tracing
    rd_trace.h          trace structures
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

All options are read from the environment. Defaults below are the compiled-in
values; a running compositor can also be switched to bypass with a marker file
(see GMEM below).

### Core / debug

| Variable | Default | Effect |
| --- | --- | --- |
| `RADISH_GRAPHICS` | `1` | expose the graphics queue family |
| `RADISH_DEBUG` | unset | broader pipeline/debug output |
| `RADISH_DEBUG_ERRORS` | `0` | log validation failures; print GMEM size/base at device creation |
| `RADISH_DEBUG_QUERIES` | `0` | log query activity |
| `RADISH_RENDER_QUERY_DEBUG` | `0` | log per-draw render query counters |
| `RADISH_RESOURCE_DIAGNOSTICS` | `0` | dump resource/sampler/image descriptor words |
| `RADISH_PROFILE` | `0` | per-command CPU/submit timing to stderr |
| `RADISH_GPU_ELAPSED` | unset | `1` + `RADISH_PROFILE=1`: [`rd_gpu_elapsed_prepare`](src/freedreno/radish/rd_queue.c), [`rd_execute_dispatch`](src/freedreno/radish/rd_queue.c) |
| `RADISH_GPU_ELAPSED_CALIBRATION` | unset | `mad`, `mad-unroll4`, `copy`, `shared-hot`, `packed-a`: [`rd_execute_dispatch`](src/freedreno/radish/rd_queue.c) |
| `RADISH_TRACE` | unset | path to write a GPU/CPU trace file |

### Render path

| Variable | Default | Effect |
| --- | --- | --- |
| `RADISH_GMEM` | `1` | use the GMEM tiled path for eligible passes; `0` forces bypass |
| `RADISH_GMEM_MIN_DRAWS` | `4` | minimum draw count for a pass to take GMEM |
| `RADISH_GMEM_CULL` | `1` | skip draws whose scissor bounds miss a tile |
| `RADISH_GMEM_VIEWPORT_REUSE` | `1` | carry viewport state across tiles (requires cull) |
| `RADISH_GMEM_DUMP` | `0` | print tile layout and cull/viewport counters per GMEM render |
| `RADISH_GMEM_STOP` | `-1` | flush and return after a given stage line; bisection helper |
| `RADISH_GMEM_DISABLE_FILE` | unset | override the bypass marker-file path |
| `RADISH_GFX_CACHE` | `1` | `0`: `cache = VK_NULL_HANDLE` in [`rd_CreateGraphicsPipelines`](src/freedreno/radish/rd_graphics.c); `rd_pipeline_cache_restore` / `rd_pipeline_cache_store` |
| `RADISH_GFX_CACHE_DEBUG` | `0` | `RADISH_GFX_CACHE stage=… enabled=… hit=…`: [`rd_gfx_compile`](src/freedreno/radish/rd_graphics.c) |
| `RADISH_QUERY_FREE_RENDER` | `1` | allow the query-free render fast path |
| `RADISH_NO_COVERAGE_PASS` | `0` | disable the coverage pass used by some query paths |

### Transfers / copies

| Variable | Default | Effect |
| --- | --- | --- |
| `RADISH_GPU_IMAGE_COPY` | `1` | use a compute shader for buffer→image copies |
| `RADISH_GPU_IMAGE_COPY_SMALL` | `1` | allow GPU copies below the 512 KiB floor in async submits |
| `RADISH_GPU_IMAGE_COPY_BATCH` | `8` | max copies batched into one GPU submit (1–8) |
| `RADISH_GPU_IMAGE_COPY_DEBUG` | `0` | log GPU copy dispatches and batches |
| `RADISH_GPU_BUFFER_TRANSFER` | `1` | use compute for large buffer copies/fills |
| `RADISH_GPU_BUFFER_TRANSFER_DEBUG` | `0` | log GPU buffer transfers |
| `RADISH_CPU_IMAGE_COPY_BULK` | `1` | coalesce contiguous rows in CPU image copies |
| `RADISH_CPU_IMAGE_COPY_DEBUG` | `0` | log CPU image copy call counts |
| `RADISH_CPU_IMAGE_CLEAR_BULK` | `1` | bulk-fill CPU image clears |
| `RADISH_CPU_IMAGE_CLEAR_DEBUG` | `0` | log CPU image clear paths |
| `RADISH_IMAGE_TRANSFER_CACHE_RANGES` | `1` | flush/invalidate only touched cache ranges |
| `RADISH_IMAGE_TRANSFER_CACHE_DEBUG` | `0` | log cache maintenance ranges |

### Submit / compute batching

| Variable | Default | Effect |
| --- | --- | --- |
| `RADISH_ASYNC_QUEUE_SYNC` | `0` | pass sync-file fences to MSM instead of CPU-waiting each submit |
| `RADISH_ASYNC_QUEUE_SYNC_DEBUG` | `0` | log async submit/skip decisions |
| `RADISH_BATCH_CHUNKS` | `1` | compute chunks per submit; `8` enables deferred compute batching |
| `RADISH_WINDOW_WAIT_OLDEST` | `0` | drain only the oldest submit-window slot instead of the whole window |
| `RADISH_GROUPS` | `0` | plain workgroup batching override |
| `RADISH_SHARED_GROUPS` | `1` | shared-memory workgroup batching |
| `RADISH_BARRIER_GROUPS` | `0` | barrier workgroup batching override |
| `RADISH_TILED_BATCH_CHUNKS` | `1` | tiled compute chunks per submit |
| `RADISH_TILED_GROUPS_X` | `1` | tiled workgroup X factor (`2` enables x2 tiling) |
| `RADISH_WAVESIZE` | unset (`IR3_SINGLE_ONLY`) | `double`: `IR3_DOUBLE_ONLY`; `any`: `IR3_SINGLE_OR_DOUBLE` ([`rd_real_wavesize`](src/freedreno/radish/rd_pipeline.c)) |

### Descriptors / sampling / compatibility

| Variable | Default | Effect |
| --- | --- | --- |
| `RADISH_DESCRIPTOR_ARENA` | `1` | arena-allocate per-draw descriptor snapshots |
| `RADISH_LAYOUT_COMPATIBILITY_CACHE` | `1` | cache pipeline-layout compatibility hashes |
| `RADISH_LAYOUT_COMPATIBILITY_DEBUG` | `0` | log layout compatibility computation |
| `RADISH_SW_TEXEL_FETCH` | `0` | force the software texture-sampling fallback |
| `RADISH_GTK_YCBCR_COMPAT` | `0` | relaxed layout compatibility for GTK YCbCr descriptor sets |
| `RADISH_A506_RB_DP2_WORKAROUND` | `1` | A506 RB depth-plane errata workaround |

### GMEM eligibility and the bypass marker

GMEM is the default for passes it accepts. A running compositor can fall back
to bypass without a restart by creating a marker file:

```sh
touch ~/.local/radish/gmem_disabled   # bypass path
rm ~/.local/radish/gmem_disabled      # GMEM path
```

`gmem_disabled` wins over the environment default, so it also reverts a
process started without `RADISH_GMEM=0`. The path can be overridden with
`RADISH_GMEM_DISABLE_FILE`.

A pass takes the GMEM path only when it has a single color attachment at one
sample, no depth/stencil, no queries, no indirect draws, no barriers, no input
attachments, a single layer, no multiview, and at least `RADISH_GMEM_MIN_DRAWS`
draws. Everything else keeps using bypass. Within a GMEM render, program and
linkage state is cached across consecutive draws with the same pipeline; any
draw that emits program state is pinned so tile culling cannot skip it.

## Supported extensions

```
KHR_swapchain                     KHR_incremental_present
KHR_device_group                  KHR_descriptor_update_template
KHR_maintenance1                  KHR_maintenance3
KHR_sampler_ycbcr_conversion      KHR_bind_memory2
KHR_get_memory_requirements2      KHR_external_memory
KHR_external_memory_fd            KHR_external_semaphore
KHR_external_semaphore_fd         EXT_external_memory_dma_buf
EXT_queue_family_foreign          KHR_synchronization2
KHR_image_format_list             EXT_image_drm_format_modifier
EXT_physical_device_drm           KHR_storage_buffer_storage_class
KHR_16bit_storage                 KHR_8bit_storage
KHR_shader_float16_int8           KHR_relaxed_block_layout
KHR_sampler_mirror_clamp_to_edge  EXT_host_query_reset
KHR_copy_commands2
```

`KHR_external_semaphore[_fd]` is only advertised when the render node supports
sync file import and export. `KHR_timeline_semaphore` is advertised when the
render node's syncobj support includes the required timeline features.

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
| `RD_IMAGE_MAX_2D_SIZE` | 8192 |
| `RD_IMAGE_MAX_LEVELS` | 14 |
| `RD_MAX_SHARED_SIZE` | 16384 |
| `RD_MAX_WORKGROUP_INVOCATIONS` | 256 |

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
passes it accepts. These figures predate the draw-count threshold, viewport
reuse, and program cache; re-measure before quoting them for current builds.

## Related repository

Docs, CTS ledgers and tablet scripts live separately in
[jethac/radish](https://github.com/jethac/radish).
