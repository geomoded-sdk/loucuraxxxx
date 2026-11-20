# LoucuraXXXX GPU

**Version 0.0.2 INDEV** — complete rebuild of the 0.0.1 prototype.

LoucuraXXXX GPU is a **hybrid virtual GPU layer**. It is not a fake device and
it does not pretend to be one. It is a software layer that sits beside the
physical GPU and coordinates three things that a machine already has:

* the **physical GPU**, used through a real graphics API when one is available,
* the **CPU**, used as a genuine additional processing unit, and
* a **bounded slice of system RAM**, reserved for graphics work.

On top of that layer the project provides its own **renderer** and a modular
**filter system** that runs just before presentation.

The design takes inspiration from the tight hardware/software integration of
Apple silicon, but it never claims hardware capabilities the machine does not
have. When something is not available, the code says so.

> **Status.** This is development software. The core pipeline is real and
> tested end to end on the CPU backend, and the filter chain also runs on real
> GPU backends — Direct3D 11 on Windows, OpenGL everywhere, and opt-in Vulkan —
> with each pass checked against the CPU reference output. What does not exist
> is a device driver a game's own graphics API can enumerate; see
> [`documentation/MODULE_STATUS.md`](documentation/MODULE_STATUS.md) for the
> exact, per-module status and [`documentation/GAME_COMPATIBILITY.md`](documentation/GAME_COMPATIBILITY.md)
> for what that means for running games. No performance claim in this project
> is made without a reproducible benchmark to back it.

## Why there is no Vulkan-only story

The project's primary target machine **does not support Vulkan**, so Vulkan is
**opt-in and off by default** (`-DLX_ENABLE_VULKAN=ON`). The default build uses
the CPU backend, which is always present and always works. This is a deliberate
choice, not a limitation of the design: the backend interface is API-agnostic,
so a Vulkan, Metal or Direct3D backend plugs into the same pipeline.

## Apple silicon

The layer is designed around the two properties that make an M-series chip
different: **unified memory** (the CPU and GPU share one pool, so there is
nothing to copy between them) and **performance/efficiency cores** (latency-
bound work belongs on P-cores, background work on E-cores). On macOS the real
backend is **Metal** — user space only, no kernel extension. See
[`documentation/APPLE_SILICON.md`](documentation/APPLE_SILICON.md) for what is
implemented, what is experimental, and what is not yet done.

## Layout

```
LoucuraXXXX-GPU/
├── core/          error handling, logging, JSON, filesystem helpers
├── hardware/      real CPU / memory / OS / GPU detection
├── memory/        reservation policy, budget accounting, buffer pool
├── scheduler/     thread pool, cost model, dependency task graph
├── configurator/  validation, clamping, save / load
├── backends/      backend interface + cpu (default), d3d11 (Windows), opengl, metal (macOS), vulkan (opt-in)
├── shaders/       shader library and GLSL sources
├── filters/       filter interface, registry, built-in filters
├── renderer/      frame pipeline and presentation
├── manager/       the central coordinator (placement + telemetry)
├── driver/        virtual GPU lifecycle + OS integrations
├── tools/         lx_cli
├── gui/           graphical front end (Win32 / X11 / headless)
├── benchmarks/    lx_bench (reproducible measurements)
├── tests/         unit + integration tests
└── documentation/ architecture, status, API, build and game notes
```

Each directory has one responsibility. See
[`documentation/ARCHITECTURE.md`](documentation/ARCHITECTURE.md) for how the
pieces fit together and [`documentation/API.md`](documentation/API.md) for the
interfaces.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements: a C++17 compiler and CMake 3.16+. No Python, no mandatory
third-party libraries. See [`documentation/BUILDING.md`](documentation/BUILDING.md)
for options, platform notes and packaging.

## Install

```sh
cmake --install build --prefix /tmp/lx-stage
```

On Windows the same build produces a real NSIS installer and a ZIP:

```sh
cd build-win && cpack -G NSIS && cpack -G ZIP
# -> "LoucuraXXXX GPU-0.0.2-win64.exe", "LoucuraXXXX GPU-0.0.2-win64.zip"
```

## Try it

```sh
build/bin/lx_cli info                 # detect this host and list usable backends
build/bin/lx_cli filters              # list the registered filters
build/bin/lx_cli validate --ram-mib 4096 --cores 4
build/bin/lx_cli render --width 640 --height 480 --frames 5 --filter bloom
build/bin/lx_cli render --backend d3d11 --width 640 --height 480   # force a backend
build/bin/lx_bench --width 640 --height 480 --iterations 30
```

`render` defaults to the first backend the probe finds, which is real GPU
hardware when present, then the CPU path, then a software rasterizer. It prints
the backend and device it actually used.

## The one rule

Nothing in this project is allowed to lie. A backend that is not compiled in
reports `BackendUnavailable`. An OS integration that needs a kernel module
reports `UserSpaceOnly` and explains why. A filter that cannot run in place
says so. The configuration that a user requests is clamped to the real machine
and every clamp is reported back.
