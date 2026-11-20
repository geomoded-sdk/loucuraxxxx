# Module status

This file is the project's honesty contract. It states, per module, what is
**implemented and tested**, what is **experimental**, and what is **planned**
but not yet written. If the code and this file disagree, the code is wrong.

Maturity values come from `lx::Maturity`:

* **Implemented** — built, tested, exercised by the automated suite.
* **Experimental** — built and functional, but limited coverage or stability.
* **Planned** — documented intent, no working code yet.

## Summary

| Module | Status | Notes |
|---|---|---|
| core (status, json, log, files) | Implemented | Unit tested |
| hardware detection | Implemented | Real OS queries; tested on Linux and Windows (Wine) |
| memory policy / budget / pool | Implemented | Unit + concurrency tested |
| scheduler (pool, cost model, graph) | Implemented | Unit tested |
| configurator | Implemented | Round-trip save/load tested |
| backend interface | Implemented | Includes unified-memory + host-mapping contract |
| CPU backend | Implemented | Reference backend; tested; unified + host-mappable |
| Metal backend | Experimental | Real Metal code; macOS-only; not exercised in this CI |
| Vulkan backend | Experimental | Opt-in (`-DLX_ENABLE_VULKAN=ON`); real compute pipelines; tested against a software ICD |
| OpenGL backend | Experimental | Built by default; fragment-shader passes; runs on llvmpipe here |
| Direct3D 11 backend | Experimental | Built by default on Windows; fragment-shader passes; suite passes under Wine |
| Virtual adapter (Vulkan ICD) | Experimental | Opt-in (`-DLX_BUILD_ICD=ON`); enumerable by the real loader; real memory + buffer copies; no shader execution, no display |

Validation layers: `BackendCreateInfo::allow_validation` is honoured by Direct3D 11
(`D3D11_CREATE_DEVICE_DEBUG`) and by Vulkan (`VK_LAYER_KHRONOS_validation`, enabled
only when the layer is installed, otherwise a warning is logged). The OpenGL and
Metal backends do not implement it and do not pretend to: the field has no effect
there. Asking for validation never fails backend creation on any backend — a
missing layer is reported, not fatal.
| Direct3D 12 backend | Planned | Requires Windows toolchain |
| shader library | Implemented | Loads manifest; optional SPIR-V |
| filters (5 built-ins) | Implemented | Pixel math + chain tested |
| renderer | Implemented | CPU path end to end; GPU filter chain on D3D11/OpenGL/Vulkan |
| manager | Implemented | Placement + telemetry tested |
| driver lifecycle | Implemented | |
| GUI (`lx_gui`) | Implemented | Display-free `gui_model` tested; Win32 and X11 front ends build; headless mode |
| Linux integration | Experimental | User-space DRM probe only |
| Windows integration | Experimental | User-space DXGI probe; suite runs on Windows under Wine, not on a real Windows host |
| macOS integration | Experimental | Probe code; not exercised on macOS (deferred — no macOS host available) |
| benchmarks | Implemented | Reproducible, no invented numbers |

## Details

### core — Implemented
`Status`/`Result<T>`/`ErrorCode` with no exceptions across module boundaries.
Strict JSON parser. Thread-safe logger. Filesystem helpers. Covered by
`tests/test_core.cpp`.

### hardware — Implemented
CPU model, core counts, ISA features, cache sizes, memory total/available, page
size, OS name/version/architecture and GPU enumeration are read from the
operating system. On Linux this is `/proc/cpuinfo`, `/proc/meminfo`,
`sysfs` and `/dev/dri`. Windows and macOS implementations follow the same
contract.

Two host properties are modelled explicitly because they change how work is
placed: the **performance/efficiency core split** (read from
`hw.perflevel0/1` on macOS and `cpu_capacity` on Linux) and the **memory
topology** — whether the GPU shares the CPU's physical memory (Apple silicon,
most integrated GPUs) or has its own (discrete cards). Covered by
`tests/test_hardware.cpp`.

### memory — Implemented
`MemoryPolicy::plan` never grants a reservation that would starve the OS.
`MemoryBudget` is atomic and returns `OutOfMemory` rather than overrunning.
`BufferPool` is thread-safe and recycles by size class. Covered by
`tests/test_memory.cpp`, including a multi-threaded stress test.

### scheduler — Implemented
Fixed-size `ThreadPool` with work stealing and optional CPU pinning. Workers
carry a **core class** (performance or efficiency); `submit_on` steers work to
the right kind of core, and on macOS each worker requests the matching QoS class
so the OS places it on a P-core or an E-core. `recommend_performance_threads`
derives the split from the host's core counts.

`CostModel` produces a decision plus the estimated cost of each option. When the
host has **unified memory** it charges no transfer cost, which is the single
biggest lever on Apple silicon. `TaskGraph` detects cycles and runs
dependency-ordered work. Covered by `tests/test_scheduler.cpp`.

### configurator — Implemented
Validates and clamps against the real host, records every adjustment, and
persists to JSON. Covered by `tests/test_hardware.cpp` (configurator cases) and
`tests/test_integration.cpp`.

Every persisted field is either consumed by a module or refused with an
explanation, because a saved value that nothing reads is a lie about what the
program does. `vsync` is the one refusal: there is no window and no swapchain, so
nothing can be synchronised against, and the Configurator reports the request and
leaves it off instead of storing a flag that implies tear-free presentation. Use
`target_fps` to cap the frame rate. `telemetry_interval_ms` throttles the GUI's
progress logging: `0` logs every frame, otherwise a line is emitted at most that
often, and the closing summary always reports the measured totals.
`max_in_flight_frames` sets how many present targets the renderer rotates.

### backends — CPU Implemented, GPU backends Experimental
`BackendCreateInfo::preferred_device` is honoured, not merely stored. The rule is
`select_device()` in `backends/device_select.hpp`: an ASCII-case-insensitive
substring match over the device names, which returns the chosen index *and*
whether the preference matched. The Vulkan backend passes it only the devices that
have a compute queue, warns when nothing matches, and then uses the default
device. Covered by `tests/test_backends.cpp` (multi-device matching) and
`tests/test_gpu_backends.cpp` (a real device selected by its own name, and an
unmatched preference still yielding a working backend).
`backends/backend.hpp` defines the API-agnostic interface. It carries two
properties that the rest of the layer relies on: `unified_memory` (the device
shares the CPU's memory) and `supports_host_mapping` (the backend can hand out a
pointer to a texture). Together they express the zero-copy path.

The CPU backend is the reference implementation and is always built; it is
unified and host-mappable by construction. The **Metal backend** is real code
(`backends/metal/`), built only on Apple platforms, that allocates shared-storage
`MTLBuffer`s, exposes them as textures, compiles MSL from source, and records
compute dispatches. It is marked Experimental because it has not been exercised
on a macOS host in this repository's CI.

The **Vulkan backend** is real code (`backends/vulkan/`), opt-in with
`-DLX_ENABLE_VULKAN=ON`, and cross-platform (Windows, Linux, and a Vulkan-capable
macOS host via MoltenVK). It creates a headless `VkInstance`, a compute-capable
`VkDevice`, device-local `VkImage`s with sampled/storage/transfer usage, and
host-visible staging buffers for upload and download. A pass is a compute
dispatch: the built-in filters are GLSL compute shaders compiled to SPIR-V at
run time with **shaderc** when the build found it (`register_shader()` also
accepts pre-compiled SPIR-V, so the backend works without shaderc). It is marked
Experimental: it is built and tested against a Vulkan device in this repository's
CI, but it is new in 0.0.2 and opt-in, so it is not yet exercised across the range
of hardware drivers. When Vulkan is not found, configuration continues and
reports that the backend is skipped; no GPU backend is claimed to work until it
is built and tested on the host that will use it.

The **OpenGL backend** is real code (`backends/opengl/`) built by default on
every platform, and the **Direct3D 11 backend** (`backends/d3d11/`) is built by
default on Windows. Both run a pass as a full-surface fragment shader and are
marked Experimental because, on the hosts available here, the only device either
one can reach is a software rasterizer.

**Hardware versus software is reported, not assumed.** Every probe returns
`is_hardware` alongside availability, and `BackendCapabilities::is_hardware`
carries the same fact once a device exists. A software rasterizer is recognised
by what it actually reports: `deviceType == CPU` for Vulkan, the `GL_RENDERER`
string for OpenGL (`llvmpipe`, `softpipe`, `SwiftShader`), and the
`DXGI_ADAPTER_FLAG_SOFTWARE` flag plus the adapter description for Direct3D 11
(WARP, Basic Render Driver). `probe_available_backends()` sorts in three tiers:
real hardware first, then the CPU reference path, then software rasterizers.
The CPU outranks a software rasterizer because both do the same work on the same
cores and the rasterizer adds driver overhead, so picking one as the "best"
backend would present a slowdown as acceleration. The CLI states this plainly:
on a host with no GPU, Vulkan and OpenGL are listed as *software rasterizer: no
GPU acceleration*, and the benchmark labels the same rows, so a timing from
lavapipe is never presented as a GPU result.

`lx_cli render` takes `--backend NAME` and defaults to the first probed backend,
so the same command renders through the real GPU on a Windows host and through
the CPU path where no GPU exists. It prints the backend and device it actually
used, and an unknown name is an error rather than a silent fallback.

The Direct3D 11 constant blocks mirror the C++ push structs field for field,
including explicit padding, because HLSL packs a `float2` at the next 4-byte
boundary while the C++/std430 layout places it at offset 8. An unpadded block
feeds the shader a shifted `image_size`; that bug was found by the end-to-end
chain test and is now pinned by an isolated per-pass test for every filter.

### shaders — Implemented (library and GPU execution)
`shaders/shader_library.cpp` loads `shaders/manifest.json` and reads whatever
bytecode is present, reporting which modules have bytecode and which do not. The
GLSL sources are real. Compiling them to SPIR-V is a build step that runs only
when `glslangValidator` is available; the CPU backend does not need it.

The GPU backends execute the built-in passes as their own shaders rather than
interpreting the GLSL in `shaders/`: Direct3D 11 uses HLSL compiled at run time
with `D3DCompile`, OpenGL compiles GLSL with `glShaderSource`/`glCompileShader`,
and Vulkan compiles generated GLSL to SPIR-V with shaderc. A pass is a
full-surface draw with a pixel/fragment shader (a compute dispatch on Vulkan),
and each one is checked against the CPU reference output in
`tests/test_gpu_backends.cpp`. Loading a third-party shader pack into this
pipeline is still not implemented; see `GAME_COMPATIBILITY.md`.

### filters — Implemented
Five filters with real pixel math: tone (exposure/contrast/saturation), separable
Gaussian blur, unsharp-mask sharpen, threshold-and-add bloom, and vignette.
Every filter validates its inputs and rejects size mismatches.

Each filter implements `apply_rows`, so the chain can split it into contiguous
row bands and run them on the thread pool. Neighbour-reading filters (blur,
sharpen, bloom) recompute their own halo rows locally, which keeps every band
independent; the chain copies a non-in-place result back only after all bands
finish. The parallel output is asserted byte-identical to the serial output for
every filter in `tests/test_filters.cpp`.

Filters carry a `FilterScope` (`Unknown`/`Game`/`System`) and the chain takes the
frame's own scope, skipping any filter whose scope does not match. This is how a
filter tuned for game content leaves the desktop UI alone. `Unknown` is a
wildcard on both sides, so an unclassified frame still runs every filter and a
default configuration is unchanged. The Configurator exposes a per-filter
`scope`, validated and persisted, and the GUI can toggle each filter on or off
and set its scope and intensity.

### renderer — Implemented (CPU path; GPU filter chain on hardware backends)
Upload, filter, present and readback work end to end on the CPU backend.
`RendererDesc::cpu_worker_count` sizes an owned filter pool, so the cores the
Configurator reserves actually reach the frame; `set_thread_pool` overrides it
with a caller-owned pool. `StatsWindow` reports measured FPS and percentiles.

On a backend without host mapping (a discrete GPU), the renderer runs the filter
chain on the backend itself: each filter's `gpu_plan` names its passes and the
backend's built-in shaders execute them, ping-ponging between the present target
and an auxiliary texture. The path is chosen by `can_run_filters_on_gpu` and
falls back to the host path with an explicit error when a filter has no GPU plan,
so a filter is never silently skipped. `FrameInput::scope` selects which filters
run. Covered by `tests/test_renderer.cpp` and the end-to-end chain test in
`tests/test_gpu_backends.cpp`.

### manager — Implemented
Priority queue, admission control, cost-model-driven placement, budget
enforcement and telemetry. Covered by `tests/test_manager.cpp` and
`tests/test_integration.cpp`.

Two utilization numbers are reported and they are not interchangeable.
`cpu_queue_pressure` is queue backlog (waiting work over worker count), which
reads 0.0 while every worker is busy with one long pass. `cpu_worker_utilization`
is measured occupancy: task execution time summed over workers divided by
worker-time elapsed, taken from `ThreadPool` timing, so a long task counts while
it runs rather than when it returns. It describes the reserved threads only, not
the whole machine. `gpu_utilization` remains unmeasured and is gated by
`gpu_utilization_available`.

`ThreadPool::pending()` counts everything in flight, including running tasks;
`ThreadPool::queued()` is the queue depth. The manager's pressure figure uses
`queued()`. Before this split, `pending()` was documented as queue depth and the
pressure figure therefore read 1.0 for a pool with an empty queue.

### driver — Implemented
Detects the host, applies the reservation, selects and creates a backend,
connects the platform integration, and reports the integration level honestly.
Covered by `tests/test_driver.cpp` (lifecycle, double-initialize refusal, clamp
reporting, CPU fallback both allowed and disallowed, `auto` selection, integration
level ceiling, budget/plan agreement) and end-to-end in `tests/test_integration.cpp`.

`shutdown()` discards the whole `DriverStatus`, not just the initialized flag.
Before that, a caller that reinitialized after `shutdown()` saw every warning from
the previous session again -- a probe showed 2 warnings growing to 4 -- and
`status()` still described a backend that had been released.

### GUI — Implemented
`lx_gui` is a native control panel for the virtual GPU. All logic lives in the
display-free `GuiModel` (host detection, safe RAM/core limits, config
validation, frame rendering, measured stats), so it is fully covered by
`tests/test_gui.cpp` without a display. The Configurator offers a per-filter
checkbox plus scope and intensity, driven by `set_filter_enabled`,
`set_filter_scope` and `set_filter_intensity`; enabling a filter creates its
config entry and disabling it keeps the tuning for a later re-enable. `gui/main.cpp`
dispatches between a headless text report (`--headless`), a smoke test
(`--self-test`) and a native window. The native front ends are Win32
(`gui/platform/gui_win32.cpp`) and X11 (`gui/platform/gui_x11.cpp`, built only
when the headers are present). The Win32 front end is built and run under Wine;
the X11 front end is built and launched under Xvfb. Neither is claimed to be
verified on the corresponding real desktop session beyond that.

`preferred_backend = "auto"` resolves through `probe_available_backends()`, the
same rule the CLI uses, so the GUI renders on the detected Direct3D 11 adapter
on Windows and on the CPU path where no real GPU exists. An explicit name
(`vulkan`, `d3d11`, `opengl`, `metal`, `cpu`) is honoured, and an unusable one
logs the reason and falls back to CPU rather than reporting a device it did not
use.

### Virtual adapter (Vulkan ICD) — Experimental

`-DLX_ENABLE_VULKAN=ON -DLX_BUILD_ICD=ON` builds `liblxc_icd.so`, a real Vulkan
Installable Client Driver. A JSON manifest (`backends/vulkan/icd/icd_manifest.*`)
registers it with the loader. This is the same mechanism Mesa's lavapipe and
MoltenVK use: a user-space library that the Vulkan loader `dlopen()`s, with no
kernel-mode driver.

What works, verified against the system loader in `tests/test_icd.cpp`:

* `vkEnumeratePhysicalDevices` returns one device named
  `LoucuraXXXX GPU (virtual, CPU-backed)`, reported as
  `VK_PHYSICAL_DEVICE_TYPE_CPU` — not as discrete hardware.
* One host-visible, host-coherent memory heap of 1 GiB.
* Buffers, images, memory allocation, mapping, shader modules, fences,
  semaphores, command pools and command buffers.
* `vkMapMemory` returns a genuinely writable host pointer; writes through it are
  what later reads observe.
* `vkCmdCopyBuffer`, `vkCmdFillBuffer` and `vkCmdUpdateBuffer` execute at submit
  time against that memory. The tests copy 256 bytes and verify every byte.

What does **not** work, and fails loudly instead of silently:

* Shader execution. `vkCmdDispatch` is recorded, and `vkQueueSubmit` returns
  `VK_ERROR_FEATURE_NOT_PRESENT` without applying anything. A silent no-op would
  let an application read memory the driver never wrote.
* No swapchain, surface or presentation. The device cannot display a frame, so it
  is not usable for games.
* No device extensions, and no graphics or transfer queue bits are advertised.

**Windows Device Manager.** This does *not* make the adapter appear there. Device
Manager lists devices backed by a signed WDDM kernel-mode driver (`*.sys` plus a
user-mode driver, installed with administrator rights). A user-space process
cannot create one, and shipping an unsigned one would be exactly the fake GPU
identification this project forbids. What this ICD does is make the adapter
enumerable by Vulkan applications (`vulkaninfo`, compute programs that accept a
CPU device). Appearing in Device Manager remains unimplemented and is listed as
planned work, not as a capability.

### OS integrations — Experimental
Each platform's integration is a **user-space probe**. It reports whether a DRM
render node, a WDDM adapter, or a Metal device is present and whether surfaces
can be shared, and it refuses to claim it can create a display adapter, because
that requires a kernel module. The Linux path is exercised natively here and the
Windows path is exercised under Wine (the full suite passes and the DXGI probe
runs), but neither has been run on the corresponding real host, so both remain
Experimental. The macOS path is deferred: it compiles only on a Mac and no macOS
host is available, so it is written but unexercised.

## What is explicitly *not* claimed

* The Metal backend is not claimed to be verified on Apple hardware. It is real
  code that compiles and follows the documented API, but this repository's CI is
  Linux, so it has not been run on a Mac. It is Experimental until it has.
  macOS support is **deferred**: the goal for 0.0.2 is Windows and Linux; the
  macOS sources are kept compiling and correct but are not the active target.
* Windows is claimed to build and pass the full test suite, and its CLI and
  benchmark are exercised, under Wine. It is **not** claimed to be verified on a
  real Windows host until that run is done.
* The Vulkan backend is real and tested against a Vulkan device (a software ICD
  in CI), but it is opt-in and off by default, and it is not yet exercised on a
  range of hardware drivers.
* No GPU acceleration is claimed on a host whose only device is a software
  rasterizer. The probes report `is_hardware == false` there, and the CLI and
  benchmark label those rows as such. On such a host the CPU backend is the
  fastest path measured, and the benchmark says so rather than implying a GPU
  helped.
* The OpenGL and Direct3D 11 backends are not claimed to be verified on real GPU
  hardware. They compile and their full test suite passes, but the Direct3D 11
  run is under Wine and the OpenGL run is on llvmpipe, so neither has been
  exercised against a vendor driver here. Wine is detected explicitly for this
  reason: it reports an adapter from its own card database (here an "NVIDIA
  GeForce GTX 470") while executing on the host's software rasterizer, and it
  sets neither the DXGI software flag nor a vendor id of 0, so the name and the
  descriptor both look genuine. `running_under_wine()` is what stops that
  adapter from being advertised as hardware, and
  `gpu_backends.wine_environment_is_detected` asserts no API backend is reported
  as hardware under Wine.
* No kernel-mode driver is claimed. Privileged integrations are out of scope for
  a user-space process and are reported as such.
* No game compatibility is claimed. Minecraft with Continuum RT or any other
  shader pack is a **test goal**, not a supported feature — see
  [`GAME_COMPATIBILITY.md`](GAME_COMPATIBILITY.md).
* No performance improvement is claimed without a reproducible benchmark.
