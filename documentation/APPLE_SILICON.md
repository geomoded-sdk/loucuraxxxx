# Apple silicon (M-series) support

This document describes how LoucuraXXXX GPU is designed around the properties
that make an Apple M-series chip — M1 through M4 — different from a
conventional x86 machine with a discrete GPU. It also states plainly what is
implemented, what is experimental, and what is not yet done.

The goal is not to *pretend* the layer is an M4. It is to exploit the same
architectural facts the M4 does, on any host that has them, and to fall back
cleanly where it does not.

## The three facts that matter

### 1. Unified memory

On Apple silicon the CPU and GPU address the **same physical memory**. There is
no separate VRAM and no PCIe bus between them. This removes the dominant cost of
GPU work on a discrete card: copying data across the bus, twice, for every pass
that needs its result back.

The layer models this as `hardware::MemoryTopology`:

* `Unified` — one pool (Apple silicon, most integrated GPUs).
* `Discrete` — separate device memory (a discrete card).
* `Unknown` — not determined; treated conservatively as discrete.

`HardwareInfo::memory_topology()` derives it from the primary device's own
`unified_memory` flag. Each backend also reports
`BackendCapabilities::unified_memory`, and the manager uses the backend as the
authority: if the backend says its textures are host memory, the cost model
charges no transfer.

**Effect on scheduling.** `scheduler::CostModel` sets transfer cost to zero when
`CostConstants::unified_memory` is true, and lowers the size threshold at which a
CPU/GPU split becomes worthwhile from 4 MiB to 256 KiB. The practical result is
that mid-sized passes that a discrete card would run on the CPU (because the
copy would dominate) run on the GPU instead. This is asserted by
`scheduler.unified_memory_changes_placement`.

### 2. Performance and efficiency cores

M-series chips have a mix of performance and efficiency cores. Latency-sensitive
work belongs on P-cores; background work belongs on E-cores, leaving the fast
cores for the foreground application.

The layer models this with `scheduler::CoreClass`:

* `ThreadPoolConfig::performance_threads` marks the leading N workers as
  performance-class.
* `submit_on(task, CoreClass::Performance | Efficiency)` steers work. If no
  worker of the requested class exists, the task still runs — the pool never
  drops work to honour a preference.
* `recommend_performance_threads(total, p_cores, e_cores)` computes the split
  proportionally, always keeping at least one worker of each class.

On macOS each worker calls `pthread_set_qos_class_self_np` with
`QOS_CLASS_USER_INITIATED` (performance) or `QOS_CLASS_UTILITY` (efficiency).
This is the supported way to express the intent: the macOS scheduler, not the
application, decides the actual core. Thread affinity masks are deliberately
**not** used on macOS, because they are not a supported way to pin to a core
class.

Core counts come from `hw.perflevel0.physicalcpu` and `hw.perflevel1.physicalcpu`
on macOS, and from `cpu_capacity` in sysfs on Linux (which also covers Intel
P/E and ARM big.LITTLE parts).

### 3. Metal, in user space

Metal is the only low-level graphics API on modern macOS, and it is a user-space
framework. There is no kernel extension and no privileged component to install —
which is exactly what the platform permits, and what this project does.

The Metal backend (`backends/metal/`) is real code:

* allocates `MTLBuffer`s with `MTLStorageModeShared`, so a texture *is* host
  memory — `map_texture()` returns the buffer's own `contents` pointer;
* exposes each buffer as an `MTLTexture` view for compute shaders;
* compiles Metal Shading Language from source (`register_shader_source`);
* records compute dispatches into a command buffer and submits them.

It is marked **Experimental**, not Implemented, because this repository's CI is
Linux and the code has not been run on a Mac. It is not claimed to work until it
has been tested there.

## How the pieces fit together

```
hardware::detect_hardware()
    │  CPU core classes, GPU unified-memory flag
    ▼
configurator::HostLimits  (hybrid_cpu, performance/efficiency_cores, unified_memory)
    │
    ▼
manager::ManagerConfig  (performance_workers, cost_constants.unified_memory)
    │
    ├─► scheduler::ThreadPool   workers carry a CoreClass; macOS QoS on start
    └─► scheduler::CostModel    zero transfer cost when unified
            │
            ▼
        renderer::Renderer
            └─► backend.map_texture()  ← zero-copy upload on unified backends
```

## What works today, and where

| Capability | Linux (this CI) | Windows | macOS |
|---|---|---|---|
| Detect P/E core split | yes (`cpu_capacity`) | yes (`EfficiencyClass`) | yes (`hw.perflevel*`) |
| Detect unified memory | yes (sysfs) | yes (DXGI) | yes (arm64) |
| Core-class pool + `submit_on` | yes | yes | yes |
| macOS QoS placement | n/a | n/a | compiled, untested |
| Unified cost model (no transfer) | yes | yes | yes |
| Zero-copy render upload | yes (CPU backend) | CPU backend only (D3D11 is discrete) | via Metal backend |
| Real-GPU filter passes | yes (OpenGL; opt-in Vulkan) — software rasterizer here | Direct3D 11 path runs the chain, on a software rasterizer here | not built |
| Metal GPU backend | not built | not built | compiled, untested |

The Windows column for Direct3D 11 is exercised, not inferred: the backend runs
the full filter chain and each pass matches the CPU reference output, and the
Windows test suite passes under Wine in this repository's development container.
It is not a measurement on real graphics hardware. The container has no
`/dev/dri`, and Wine synthesises the adapter it reports — it names an
"NVIDIA GeForce GTX 470" while wined3d logs `using GL_RENDERER "llvmpipe"` and
the PCI vendor id is 0. The probe therefore classifies the adapter as software,
`lx_cli info` prints it as `[software]`, and backend auto-selection picks the CPU
path on this host. Numbers taken here are software-rasterizer numbers. The macOS
column stays unverified because no Apple hardware is available here.

## Honest limits

* **No Metal verification.** The Metal backend is compiled only on Apple
  platforms and has not run on Apple hardware in this CI. Treat it as
  Experimental.
* **No kernel driver.** Nothing here loads a kernel extension. Metal is used in
  user space; that is the whole story.
* **No invented numbers.** Performance claims require a reproducible benchmark
  (`benchmarks/lx_bench`). There is no "M4 speedup" figure in this repository
  because none has been measured.
* **No shader compatibility claim.** Metal uses MSL, not SPIR-V. The Metal
  backend refuses SPIR-V rather than pretending to consume it.

## Building for macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLX_ENABLE_METAL=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`LX_ENABLE_METAL` is on by default and takes effect only on Apple platforms;
elsewhere the configure step reports that it was skipped.
