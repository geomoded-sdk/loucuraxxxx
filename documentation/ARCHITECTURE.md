# Architecture

This document describes how LoucuraXXXX GPU is put together and why each
decision was made the way it was.

## The concept, restated precisely

The virtual GPU is a **coordination layer**, not a device. It answers one
question repeatedly: *for this unit of graphics work, which executor should run
it?* The candidate executors are:

1. the **physical GPU**, through a real backend (Vulkan, Metal, Direct3D) when
   one is compiled in and usable on the host;
2. the **CPU**, through a worker pool sized to a fraction of the machine's
   cores;
3. **both**, splitting a pass across the two when that is actually faster.

The layer never removes the physical GPU from the machine and never blocks it.
It adds a path beside it.

## Data flow

```
             ┌────────────────────────────────────────────────────────┐
             │                        Configurator                    │
             │  validate request against real host, clamp, persist    │
             └───────────────┬────────────────────────────────────────┘
                             │ Config (effective)
                             ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │                             Driver                               │
   │  detect host · reserve memory · create backend · connect OS      │
   └───────┬───────────────────────────────────────────────┬──────────┘
           │ owns                                          │ creates
           ▼                                               ▼
   ┌───────────────┐                            ┌──────────────────────┐
   │ MemoryBudget  │◄──── commits ──────────────┤       Manager        │
   │ (one source   │                            │ queue · cost model · │
   │  of truth)    │                            │ placement · telemetry│
   └───────┬───────┘                            └──────┬───────────────┘
           │                                           │ dispatches
           │                                           ▼
           │                                 ┌──────────────────────┐
           │                                 │   Backend (CPU/GPU)  │
           │                                 │  textures · kernels  │
           │                                 └──────────┬───────────┘
           │                                            │
           ▼                                            ▼
   ┌───────────────┐                            ┌──────────────────────┐
   │ BufferPool    │◄──── scratch ──────────────┤      Renderer        │
   └───────────────┘                            │ upload · filters ·   │
                                                │ present · stats      │
                                                └──────────┬───────────┘
                                                           │
                                                           ▼
                                                ┌──────────────────────┐
                                                │   FilterChain        │
                                                │ tone·blur·sharpen·   │
                                                │ bloom·vignette       │
                                                └──────────────────────┘
```

## Modules

### core
Error handling without exceptions crossing module boundaries: `Status` for
outcomes, `Result<T>` for value-or-error, `ErrorCode` for classification. A
small dependency-free JSON reader/writer, a thread-safe logger, and filesystem
helpers. Everything else depends on this and on nothing else.

### hardware
Fills a `HardwareInfo` from **real** operating-system queries: `/proc` and
`sysfs` on Linux, DXGI and the registry on Windows, `sysctl` and Metal on
macOS. When a value cannot be determined it is left at zero rather than
guessed. GPU enumeration deliberately distinguishes real hardware from a
software rasterizer (`GpuKind::Software`), so nothing downstream can confuse
Mesa lavapipe with a discrete card.

Two host facts are modelled because they dominate how work should be placed:
the performance/efficiency **core split** (`CpuInfo::performance_cores` /
`efficiency_cores`) and the **memory topology** (`MemoryTopology::Unified` /
`Discrete`). The topology is derived from the primary device's `unified_memory`
flag; on Apple silicon and most integrated GPUs the CPU and GPU share one pool,
so no host<->device copy is needed. See
[`APPLE_SILICON.md`](APPLE_SILICON.md).

### memory
`MemoryPolicy` computes a safe reservation: a fraction of total RAM is held
back for the OS plus an absolute floor, and the stricter of the two wins.
`MemoryBudget` is the single accounting point every allocation commits against;
a commit that would exceed the budget fails with `OutOfMemory` instead of
overrunning. `BufferPool` recycles scratch blocks by power-of-two size class to
avoid per-frame allocator churn and page faults.

### scheduler
`ThreadPool` is a fixed-size pool with per-worker deques and work stealing, so
independent filter passes and tiles run in parallel without a central lock.
Workers carry a `CoreClass`; `submit_on` steers latency-bound work to
performance cores and background work to efficiency cores, and on macOS each
worker requests the matching QoS class so the OS does the placement. A pool
never drops work to honour a class preference — it runs the task wherever it
can. `CostModel` turns a `DispatchRequest` into a `DispatchDecision` with the
estimated cost of each option, so a placement is explainable rather than
opaque; on a unified-memory host it charges no transfer cost. `TaskGraph`
extracts frame-level parallelism: each node tracks how many predecessors it
still waits on and is submitted the moment that count reaches zero.

### configurator
The only module that reads or writes the configuration file. It validates a
request against the host, clamps anything unsafe (cores above what the machine
has, RAM that would starve the OS), and returns a `ValidationReport` listing
every adjustment and its reason. Nothing is silently altered.

### backends
The abstraction that keeps the pipeline API-agnostic. `Backend` exposes
textures, pipelines, a command encoder and CPU kernels. `BackendKind` selects a
concrete implementation. The **CPU backend** is always compiled and always
works; it is the reference for correctness. The **Vulkan backend** is opt-in
(`-DLX_ENABLE_VULKAN=ON`) because Vulkan is not guaranteed to be present: the
target machine has no Vulkan device, and a build must stay usable without it.
When built, it is a real compute backend. The **OpenGL** and **Direct3D 11**
backends are built by default and run passes as fragment shaders.

Every backend probe reports `BackendProbeResult::is_hardware` alongside
availability, because "this host exposes the API" and "this host has a GPU that
drives it" are different facts. A software rasterizer — Mesa lavapipe for
Vulkan, llvmpipe or softpipe for OpenGL, WARP or the Basic Render Driver for
Direct3D 11 — is reported as usable but **not** accelerated, and
`probe_available_backends()` sorts hardware ahead of software so no placement
decision is made on a false premise. On a machine with no Vulkan device at all,
the Vulkan backend simply does not appear in the list.

The Direct3D 11 constant blocks mirror the C++ push structs field for field,
including explicit padding. HLSL packs a `float2` at the next 4-byte boundary
while the C++/std430 layout places it at offset 8, so a block declared without
the pad word silently feeds a shader a shifted `image_size`. Each pass has a
matching isolated test that compares it against the CPU filter, which is what
pins these layouts.

### filters
`IFilter` is the filter interface: `apply(source, destination, params)` with
`intensity` and `passes`, plus a declaration of whether it can run in place.
Filters self-describe through `FilterInfo` (name, target, maturity, pass
bounds). `FilterChain` runs an ordered list, allocating scratch from the buffer
pool for filters that cannot run in place, and records per-filter timing.

### renderer
Owns a backend, a render target and the filter chain. `render_frame` uploads
input, runs the filters over the target, presents, and records measured
timings. `StatsWindow` computes rolling FPS and frame-time percentiles from
those measurements.

### manager
The coordinator. It owns the CPU worker pool, the priority work queue and the
cost model. `plan()` runs admission control, consults the cost model, forces
CPU placement when no hardware GPU exists, and records the decision and any
data movement in telemetry. It does not execute caller code on its own threads;
it returns the decision so the caller drives execution.

### driver
The lifecycle layer: detect the host, apply the reservation, create the backend,
connect the platform integration. It reports an `IntegrationLevel` that states
honestly how far the OS connection goes — `Unavailable`, `UserSpaceOnly`, or
`KernelAssisted`.

## The invariant that keeps the project honest

Every module has a documented **maturity** (`Implemented`, `Experimental`,
`Planned`) and every claim of support is gated behind a probe that
runs on the real host. There is exactly one place where a resource is accounted
(`MemoryBudget`) and one place where a placement is decided (`CostModel`), so a
report from the project can always be traced back to a measurement.
