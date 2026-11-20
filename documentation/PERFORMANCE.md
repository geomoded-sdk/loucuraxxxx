# Performance

The project does not publish performance numbers it did not measure. This file
explains how to measure and how to read the result. Run the harness on your own
machine to get your own numbers.

## Measuring

```sh
build/bin/lx_bench --width 640 --height 480 --iterations 30
```

The harness prints the host, the workload, and for each case min / median /
mean / p95 / max wall time in microseconds, plus a throughput figure derived
from the median. It also prints the cost model's placement for several working
set sizes, so the placement logic can be inspected rather than trusted.

## The three comparisons the harness makes

1. **Baseline** — the filter chain run directly on host memory, with no virtual
   GPU layer, no manager and no backend indirection. This is what an application
   would do today without LoucuraXXXX GPU. It runs with the *same* thread count
   as the virtual GPU case it is compared against. An earlier version ran the
   baseline serially against a 2-worker virtual GPU and reported 29.2 ms against
   15.3 ms, which measured the denied core rather than the layer; on equal terms
   the two are 15.1 ms and 15.2 ms.
2. **Virtual GPU** — the same filters driven through the renderer and the CPU
   backend, including upload and readback of the target texture.
3. **Reservation sweep** — the same filter pass with no pool (serial) and then at
   1, 2, 4, ... worker counts, to show how the CPU contribution scales with the
   cores actually granted.

When a GPU backend is compiled in and usable, a fourth group appears: one row per
real backend the host exposes (`vulkan`, `opengl`, `d3d11`), each measured
through the same filter chain. A row is labelled `[software rasterizer]` when the
probe found no hardware device behind the API, so a timing from Mesa lavapipe,
llvmpipe or WARP is never read as a GPU result. On a host with no GPU at all,
only the CPU row appears.

A comparison is only meaningful when the workload and the host are identical
across the cases being compared. The harness prints both.

## Reserved cores reach the frame

`FilterChain::run` splits a filter that reports `supports_row_split()` into one
contiguous band per worker and runs the bands in parallel. The reserved cores
therefore do real work, not just bookkeeping: `lx_cli render --cores N` sizes the
renderer's filter pool, and `RendererDesc::cpu_worker_count` carries the
Configurator's reservation into the render path.

Measured on a 4-logical-core AMD EPYC 9B14 host, CPU reference backend, the
`tone + blur + vignette` chain at 1920x1080 (median of 8 iterations, re-measured
after timings moved to nanosecond resolution):

| cores | median | throughput |
|-------|--------|------------|
| 0 (serial) | 195 ms | 10.6 M items/s |
| 1 | 197 ms | 10.6 M items/s |
| 2 | 103 ms | 20.1 M items/s |
| 4 | 103 ms | 20.1 M items/s |

The 1-core row matches serial because a pool of one has no work to overlap, and
4 cores is no faster than 2 here: the pass is memory-bound, so the second pair
of workers adds nothing. Both are reported as measured rather than smoothed into
a tidier curve.

The chain `tone + blur + vignette` at the same size and the same host shows the
same shape. These are single-host numbers; run the harness on your machine for
yours.

## What the numbers do and do not mean

* On the CPU backend, the virtual GPU path includes upload and readback that the
  baseline does not. On equal thread counts it therefore costs about 1% at
  640x480 (15.2 ms against 15.1 ms) — that overhead is the cost of the
  abstraction, and measuring it is the point. The layer only pays off when it
  can place work on a real GPU that would otherwise be idle, or when it extracts
  parallelism the application did not. Note that this is close to run-to-run
  noise on this host, so the honest claim is "the abstraction is not free and is
  small here", not a precise percentage. The overhead is not a constant: the same
  320x240 comparison under Wine on the same container measured 7.7 ms for the
  virtual CPU path against 4.4 ms for the baseline. Whether that is thread-pool
  scheduling under Wine or the abstraction itself is not established here, so it
  is reported as an unexplained host difference rather than a property of the
  layer.
* The reservation sweep is bounded by memory bandwidth, not core count, for a
  memory-bound pass. Seeing flat scaling past a few cores is the expected and
  honest result; it is reported, not hidden.
* A `[software rasterizer]` row measures a CPU implementation behind a GPU API.
  It is not GPU acceleration and is not compared as if it were: on the host used
  here, lavapipe was slower than the CPU backend and llvmpipe was in the same
  range, which is exactly what running filters through a CPU rasterizer should
  cost. The label exists so that number cannot be misread.
* The cost model's constants start at conservative defaults and are meant to be
  replaced by measured values from this harness. Until calibration runs on a
  specific machine, the placement for that machine is a reasonable estimate, not
  a measurement.

## Where the time goes

The renderer records, per frame: total CPU time, filter time, present time, and
bytes uploaded/downloaded. `StatsWindow` turns a rolling window of frames into
average FPS and frame-time percentiles. The CLI prints these:

```sh
build/bin/lx_cli render --width 640 --height 480 --frames 10 --filter bloom
```

## Every measurement waits for the work it measures

A GPU backend submits asynchronously, so a timing that does not wait is a timing
of the submission call, not of the work. `wait_idle()` therefore has to block
until the device is idle on every backend, and each one uses its own primitive:
`vkQueueWaitIdle` on Vulkan, `glFinish` on OpenGL, and a `D3D11_QUERY_EVENT`
polled with `GetData` on Direct3D 11 (D3D11 exposes no blocking wait). If the
event query cannot be created the backend reports `NotImplemented` instead of
returning success on a `Flush` that does not wait.

The filter chain waits once, at the end, rather than after every pass. Ordering
between passes does not need a device wait: the OpenGL path is command-ordered on
a single context, Direct3D 11 rewrites its constant buffer with `WRITE_DISCARD`
(a rename, not an in-place update), and Vulkan passes real push constants while
its encoder already waits on its own fence. Waiting between passes only
serialised the queue. The single end-of-chain wait is still counted inside
`filter_time_micros`, so the reported filter time covers the GPU work and not
just the submissions. Correctness is what makes the change safe rather than
merely shorter: the 4-pass chain still matches the CPU reference within tolerance
in `gpu_backends.full_renderer_filter_chain_runs_on_gpu`, on both hosts.

The effect is measurable at small frame sizes and inside the noise at large ones.
At 320x240 the CPU path no longer beats the Direct3D 11 path (it did before, 17.2
against 14.8 M items/s); at 640x480 an interleaved A/B of the two builds showed
no difference outside run-to-run spread. That is the honest reading: removing the
per-pass drain fixed a real serialisation, and the benchmark only shows it where
the drain dominated.

This matters for reported numbers, not just for correctness. Before the Direct3D
11 wait was real, the harness timed only the enqueue: a 640x480 chain reported
113 M items/s with a minimum of 713 µs against a median of 2708 µs, because the
GPU work spilled into the next iteration. With a real wait the same chain
measures 23 M items/s with min 12.9 ms and median 13.3 ms. The earlier figure was
not a fast path; it was an unfinished measurement, and it was removed.

## The rule

No performance gain is claimed without a benchmark that reproduces it. If a
change is meant to be faster, the same workload is measured before and after on
the same host, and both numbers are recorded.

## What the numbers in this document are measured on

Every number here was taken in this repository's development container, which
has **no graphics hardware**: there is no `/dev/dri`, and Wine synthesises a
Direct3D 11 adapter from its own card database. Wine names an "NVIDIA GeForce
GTX 470" while wined3d logs `using GL_RENDERER "llvmpipe (LLVM 19.1.7, 256
bits)"`, and the adapter's PCI vendor id is 0. The probe reports the adapter as
software (`lx_cli info` prints `[software]`), so these are software-rasterizer
numbers and backend auto-selection correctly prefers the CPU path here.

That makes the numbers useful for exactly one thing: comparing two versions of
this code on an identical host, which is what they are used for. They are not a
claim about performance on a real GPU, and none is made. A real adapter would
change both the absolute figures and, at small frame sizes, the ordering between
the GPU and CPU paths.

## When the GPU path is slower, that is the result

The virtual GPU is not automatically faster than the CPU chain, and the
benchmarks say so. Measured here (`lx_bench`, 2 workers, the Direct3D 11 path
against the CPU backend, same chain, same software rasterizer):

| Frame | CPU | Direct3D 11 path | Verdict |
| --- | --- | --- | --- |
| 640x480 | 18.2 M items/s (16.9 ms) | 23.1 M items/s (13.3 ms) | GPU path ahead by ~27% |
| 320x240 | 15.8–17.3 M items/s (4.4–5.4 ms) | 13.4–19.3 M items/s (3.8–5.7 ms) | too close to call |

Both paths run on the same cores here, so the 640x480 gap is the cost of the
upload/download round trip against the CPU's in-place work, not a hardware
speedup. At 320x240 the two are within each other's run-to-run spread. Before
the per-pass device wait was removed, 320x240 was not close: the CPU won (17.2
against 14.8 M items/s) because every pass paid a full device drain. The
ordering at small sizes is reported as "too close to call" rather than as a win
in either direction.

One case where the CPU still wins outright is a tiny frame through
`lx_cli render`: at 320x240 that path reaches a few hundred fps on the Direct3D
11 path against ~12.7 k fps on the CPU, because at that size the frame is almost
entirely transfer and the CPU never leaves memory. Both are correct outcomes,
not defects to hide. The placement policy exists precisely because the faster
unit depends on the working set: `lx_cli render` reports the backend it actually
used, and the cost model in the Manager is what decides between them for larger
work. A software rasterizer is ranked below the CPU path for the same reason,
since it does identical work on identical cores with driver overhead added.
