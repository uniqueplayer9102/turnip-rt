# Radish on-device benchmark matrix

Keep the driver build, workload/input, compositor, refresh rate, and power and
thermal conditions fixed. Warm up consistently, repeat each row, record the
exact environment, and check the output against the baseline. Use a fresh
process for each row; restart the compositor too when changing its driver options.

| Sweep | Settings |
| --- | --- |
| Compute batching | `RADISH_BATCH_CHUNKS=1/2/4/8`, with async sync and tail fences off initially. |
| Queue sync | Repeat the batching rows with `RADISH_ASYNC_QUEUE_SYNC=1`. On a workload with pending binary fences, compare `RADISH_PENDING_KERNEL_WAIT=1` against `=0`. With `=0`, `RADISH_PENDING_BLOCKING_WAIT=0/1` selects the legacy sleep/yield fallback. Include CPU time as well as frame intervals. |
| Tail fence | Compare `RADISH_COMPUTE_TAIL_FENCE=0/1` with `RADISH_BATCH_CHUNKS=8` and async sync off, using an eligible final direct dispatch and an importable signal sync. |
| Wave size | Compare `RADISH_WAVESIZE` unset, `double`, and `any`. |
| Workgroups per chunk | Sweep `RADISH_SHARED_GROUPS` (default `1`, eligible barrier kernels), `RADISH_GROUPS` (default `0`, plain kernels), and `RADISH_BARRIER_GROUPS` (default `0`, barrier kernels) one at a time. The barrier override takes precedence over the shared setting. |
| Draw snapshots | Compare `RADISH_DRAW_DESCRIPTOR_CACHE=0/1` with `RADISH_DESCRIPTOR_ARENA=1` on a graphics workload. |
| Tile-local FP16 input | After the pixel test below passes, compare `RADISH_GMEM_FP16_SUBPASS=0/1` with `RADISH_RENDER_BATCH=1` in an isolated Phoc session, then on the real panel. |

**FP16 correctness on the A506:** with the staged Radish ICD selected in
`VK_DRIVER_FILES`, run:

```sh
python3 src/freedreno/tests/radish_fp16_render_test.py --include include --gmem-subpasses
```

Use `--vulkan-library=-l:libvulkan.so.1` if only the versioned loader is
installed. The probe compares sysmem and GMEM pixels across partial updates,
verifies that the FP16 attachment can skip stores when allowed, and checks
that a nonlocal dependency stays on the regular path. Do not enable the
prototype in the login session if a test fails or the kernel reports a GPU fault.

**Record:** wall-clock time to completion, latency to the first usable result
from the same start point, Vulkan and low-level submit counts, compute chunk
counts, drain counts and time by reason, GPU elapsed time where measured, and
p95/p99 intervals between actual presentations (not frame submission times).
Record workload failures, device loss/timeouts, and kernel GPU faults or resets
even when timing data is incomplete.

**Discovery runs:** start with `RADISH_SUBMIT_DIAG=1` (log every submit) and add
`RADISH_WAIT_TIMING=1` for individual wait/drain lines. Use the submit IDs and
`seq - seq_before` to distinguish Vulkan from low-level submits. Use
`RADISH_ASYNC_QUEUE_SYNC_DEBUG=1` or `RADISH_RESOURCE_DIAGNOSTICS=1` only to
investigate path selection or deferred-window occupancy. In a separate compute
run, `RADISH_PROFILE=1` reports dispatched chunks; add `RADISH_GPU_ELAPSED=1`
for GPU elapsed samples on supported dispatches and keep only `valid=1` samples.
`RADISH_RENDER_GPU_TIMING=1` is available for render GPU timing. Profiling and
`RADISH_TRACE` change submit behavior; do not use them to time async or tail-fence
comparisons. Mark diagnostic-only or unavailable measurements as such.

**Final run:** rerun the baseline and selected rows from a clean environment
with only the workload and tuning options set. Leave all driver diagnostics
off: no profiling, GPU timing/calibration, tracing, submit/wait timing, submit
diagnostics, debug, resource dumps, or GMEM dumps. Use application wall-clock
and presentation timestamps for the final comparison; keep the discovery
logs separate.
