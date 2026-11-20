# AGENTS.md — repository guide

LoucuraXXXX GPU: a hybrid virtual GPU layer (C++17, CMake). Version 0.0.2
INDEV. This file is the fast path to working in the repo.

## Hard constraints (do not violate)

* **C++ only.** No Python anywhere in the project.
* **No Vulkan by default.** The primary target machine has no Vulkan support.
  Vulkan is opt-in (`-DLX_ENABLE_VULKAN=ON`) and is real code now
  (`backends/vulkan/`, compute pipelines), but never make the default build
  depend on it. Without shaderc the Vulkan backend only accepts SPIR-V bytecode.
* **Never fabricate.** No invented performance numbers, no fake device IDs, no
  claiming support for something not built and tested. Unavailable things must
  report `BackendUnavailable` / `NotSupported` / `UserSpaceOnly` and explain why.
* **No privileged installs.** Integrations are user-space. Do not install kernel
  drivers, especially not on macOS.

## Build / test / run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure

build/bin/lx_cli info
build/bin/lx_cli render --width 320 --height 240 --frames 3 --filter bloom
build/bin/lx_bench --width 640 --height 480 --iterations 30
```

Windows cross-build + test under Wine (how the Windows target is exercised
here). The toolchain links the C++ runtime statically, so the `.exe` files need
no DLLs alongside them:

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
cmake --build build-win -j
cd build-win/bin
export WINEPREFIX=$HOME/.wine-lx XDG_RUNTIME_DIR=/tmp/xdg WINEDEBUG=-all
for t in test_*; do wine "./$t.exe"; done
```

Sanitizer builds (Linux) — run these before calling a change done:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLX_SANITIZER=address
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

# The ASan configure above also wires `cmake/lsan.supp` into CTest, so
# `ctest --test-dir build-asan` passes. Running the binaries by hand needs the
# same options, because Mesa's llvmpipe keeps a small process-global cache that
# LeakSanitizer reports (see cmake/lsan.supp):
#   LSAN_OPTIONS=suppressions=$PWD/cmake/lsan.supp:fast_unwind_on_malloc=0 ./test_gui

# ThreadSanitizer needs ASLR disabled in this container, otherwise it aborts
# with "unexpected memory mapping" before running anything:
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLX_SANITIZER=thread
cmake --build build-tsan -j
setarch $(uname -m) -R ctest --test-dir build-tsan --output-on-failure
```

Platform priority: **Windows and Linux are the active targets for 0.0.2. macOS
is deferred** — its sources must keep compiling (see the `.mm` note below) but
are not the focus and have never been run on a Mac here.

Packaging (CPack, on by default via `LX_ENABLE_INSTALL`):

```sh
cmake --install build --prefix /tmp/lx-stage   # stage a tree
cd build-win && cpack -G NSIS && cpack -G ZIP  # Windows installer + zip
```

The installer is a complete NSIS package. Only its own bootstrap (the small
self-extractor that unpacks the payload) is a 32-bit executable — the NSIS
default, which runs on every x64 Windows through WoW64; the payload it installs
is 64-bit. Verifying the bootstrap under Wine therefore needs `wine32:i386`,
while the 64-bit payload runs under plain `wine64`. Install silently with
`"<installer>.exe" /S /D=C:\LXTest` and uninstall with
`"C:\LXTest\Uninstall.exe" /S`.

## Layout and responsibilities

| Dir | Owns |
|---|---|
| `core/` | `Status`/`Result<T>`, JSON, logging, file helpers |
| `hardware/` | real OS hardware detection |
| `memory/` | reservation policy, `MemoryBudget`, `BufferPool` |
| `scheduler/` | thread pool, cost model, task graph |
| `configurator/` | validate/clamp/persist config |
| `backends/` | backend interface; `cpu/` is the reference backend; `opengl/`, `d3d11/`, `vulkan/`, `metal/` are real GPU paths |
| `shaders/` | shader library + GLSL sources |
| `filters/` | filter interface, registry, built-ins |
| `renderer/` | frame pipeline + stats |
| `manager/` | placement, queue, telemetry |
| `driver/` | lifecycle + OS integrations |
| `gui/` | `lx_gui`: platform-independent `gui_model` + native front ends |
| `tools/` | `lx_cli` |
| `benchmarks/` | `lx_bench` |
| `tests/` | one executable per module |

### GUI

`gui/gui_model.{hpp,cpp}` holds all the display-free logic (host detection,
safe limits, validation, frame rendering, stats) and is covered by
`tests/test_gui.cpp`. `gui/main.cpp` dispatches: `--headless` prints a text
report, `--self-test` runs a smoke test, and with no flag it opens a native
window. The native front ends are `gui/platform/gui_win32.cpp` (Win32, built
with `UNICODE`/`_UNICODE` so `IDC_ARROW` resolves to its wide form) and
`gui/platform/gui_x11.cpp` (X11, built only when the headers are found). Keep
new behaviour in `GuiModel` so it stays testable without a display.

## Conventions

* Namespace `lx` (sub-namespaces `lx::backends`, `lx::filters`, `lx::renderer`,
  `lx::manager`, `lx::driver`, `lx::memory`, `lx::scheduler`, `lx::hardware`,
  `lx::configurator`).
* Errors cross boundaries as `Status`/`Result<T>`, never as exceptions. Use
  `LX_TRY(expr)` to propagate.
* Comments explain *why*, not *what*. Do not narrate diffs or history.

## Gotchas learned the hard way

* **HLSL packs a `float2` at a 4-byte boundary; C++/std430 put it at offset 8.**
  A D3D11 constant block declared as `float strength; float2 image_size;` puts
  `image_size` at byte 4, while the C++ push struct places it at byte 8. The
  shader then reads a shifted (or zeroed) `image_size` and clamps the
  neighbourhood to the wrong extent. Every D3D11 block must spell out its pad
  words so its offsets match the C++ struct field for field. This bug passed
  every isolated pass test because there was no isolated sharpen test; the
  end-to-end chain caught it. Each pass now has its own CPU-vs-GPU test.
* **`wait_idle()` must actually wait, or every GPU benchmark is a lie.**
  D3D11's `ID3D11DeviceContext::Flush` only submits queued work; it does not
  block. Waiting on it means the harness times the enqueue and the GPU work
  spills into later iterations, which shows up as a min far below the median.
  The correct primitives are `vkQueueWaitIdle`, `glFinish`, and a
  `D3D11_QUERY_EVENT` polled with `GetData` (D3D11 has no blocking wait). If a
  backend cannot build its wait primitive it must return an error, not success.
* **Static-library self-registration is dropped by the linker.** A translation
  unit whose only job is a file-scope registrar has no referenced symbol, so it
  is discarded and registration never runs. Backends and filters therefore use
  **explicit registration functions** called from `ensure_builtin_*_registered()`
  in their registry `.cpp`. If you add a backend or filter, add its
  `register_*` call there.
* **`/proc/cpuinfo` keys are padded before the colon.** Parse by finding the
  first `:` and trimming the key; do not compare against `"key:"` at offset 0.
* **`FrameStats::cpu_time_micros` is the whole frame**, not just upload. Timing
  FPS from upload alone produces absurd values.
* **The build root is the working directory for tests and tools** so
  `shaders/manifest.json` is found relative to `shaders/`.
* A `Maturity` enum has only three values: `Implemented`, `Experimental`,
  `Planned`.
* **Metal is Objective-C++.** `backends/metal/metal_backend.mm` is compiled with
  `-fobjc-arc` and is gated on `APPLE`. The header must stay plain C++ (it is
  includable from C++ TUs), so all `id<MTL...>` types live behind a pimpl.
  `register_shader` (SPIR-V) is refused by Metal; MSL goes through
  `register_shader_source`.
* **Every macOS TU that messages Objective-C objects must be `.mm`.** That
  includes `hardware/platform_macos.mm` and `driver/macos/macos_integration.mm`
  as well as the Metal backend; their CMake entries call `enable_language(OBJCXX)`
  and set `-fobjc-arc`. A `.cpp` extension makes CMake compile them as plain C++,
  which cannot parse `[device name]` and fails to build.
* **macOS core-class steering uses QoS, not affinity.** `pthread_set_qos_class_self_np`
  is the supported mechanism; CPU affinity masks are not used on macOS. The
  Linux path still uses `sched_setaffinity`.
* **One condition variable per mutex.** The thread pool gives each worker its
  own `cv`; sharing one CV across per-worker mutexes is UB and loses wakeups.
  Tasks submitted to an explicit lane are marked `lane_bound` and are never
  stolen, so lane ordering holds under stealing pressure.
* **`Manager::complete_dispatch(id)` must be called for GPU/split work.** The
  manager releases the CPU path itself in `next_cpu_task()`, but it does not
  execute GPU work, so the caller owns that release. `shutdown()` drains any
  reservation a caller forgot, so the shared budget is never left holding it.
* **`max_in_flight_bytes == 0` means "no separate ceiling".** The memory budget
  is then the only admission limit; a rejection there is `OutOfMemory`, not
  `ResourceBusy`.
* **`BufferPool` owns its pooled blocks, so `~BufferPool()` must `clear()`.**
  A `= default` destructor leaks every returned block (ASan caught 16 KiB in the
  pool test). `clear()` is also idempotent and safe to call from the destructor.
* **Buffer-size checks must use `pitch*(height-1)+row`, not `row*height`.** The
  last row is only `row` bytes, so a padded source needs less than a full pitched
  frame. Checking the wrong bound lets the final `memcpy` read past the caller's
  buffer. `upload_texture`, `download_texture` and `Renderer`'s mapped-copy all
  use the correct form, and `download_texture` also rejects a pitch smaller than
  a row (asymmetric with upload previously let rows overlap).
* **`wait_idle()`'s predicate load must be `acquire`.** The final task decrements
  `pending_` to zero *before* taking `idle_mutex_`, so a waiter can acquire the
  mutex and observe zero without any synchronisation. A relaxed load there means
  data written by finished tasks is not guaranteed visible to the caller (TSan
  flagged exactly this). The acquire load pairs with the completion release.
* **`GuiModel::validate()` re-detects the host and must refresh the stored
  limits.** It re-runs detection (available memory can move between calls), so
  it overwrites `max_ram_bytes`/`max_cpu_cores` before clamping. Otherwise the
  effective config can exceed the ceiling the front end is displaying, which
  showed up as a flaky test.
* **The JSON parser caps nesting depth at 64.** Without the limit, deeply nested
  `[[[[...` recurses until the stack overflows. `Parser::kMaxDepth` + a
  `DepthGuard` reject it with `ParseError` on every exit path.
* **Size and pitch arithmetic must be overflow-safe.** `a + b > ceiling` and
  `pitch*(height-1)+row` both wrap for a large enough operand, turning an absurd
  request into one that passes a naive check. Compare against the headroom
  (`bytes > limit - current`) or guard the multiply
  (`pitch > (max - row) / (height - 1)`). Applied in `MemoryBudget::commit`,
  `Arena::allocate`, `Manager::admit`, `Renderer`'s mapped copy, and
  `CpuBackend::upload_texture`/`download_texture`.
* **`WorkQueue` counters are read under the mutex.** `total_enqueued()` and
  friends were inline unlocked reads of fields written under `mutex_`; they are
  now out-of-line and lock.
* **`Json::as_int()` saturates before converting.** A JSON number like `1e300`
  is a `double`; casting one outside the `long long` range is UB, so `as_int`
  clamps (and maps NaN to the fallback). Config parsing then bounds each value
  (`mib * kMiB` and `cores` are range-checked) so a huge number is an error
  rather than a wrapped, unsafe reservation. `as_u32_clamped` covers the
  remaining u32 fields (`target_fps`, `max_in_flight_frames`,
  `telemetry_interval_ms`, filter `passes`).
* **`Arena::allocate_array<T>` guards `sizeof(T) * count`.** The product wraps
  for a huge count and hands back an under-sized block; it returns nullptr
  instead.
* **`MemoryPolicy::plan` clamps the fractional reserve.** `total * fraction` can
  exceed u64 (or be NaN for a nonsensical fraction); the double-to-u64 cast is
  UB there, so it is bounded by the total before converting.
* **The CLI scales MiB with `mib_to_bytes()`.** `--ram-mib` used to multiply by
  `kMiB` unchecked, so a huge argument wrapped to a small reservation.
* **Frame and image sizes are checked before allocation.** `width*height*4`
  wraps `size_t` for absurd dimensions, after which the fill loop or a filter
  writes past the small buffer. The CLI, `GuiModel::render_frames` and
  `FilterChain::run` all reject such dimensions up front.
* **The shader manifest's `push_constant_bytes` is clamped.** A negative or
  oversized value wrapped through the u32 cast into a huge block.
* **Filter row bands must not race with the copy-back.** A non-in-place filter
  reads the whole source image and writes its band into scratch. Copying scratch
  back to the image band-by-band inside each band lets one band overwrite rows
  another band still reads as its halo, which produced nondeterministic output
  (only blur and bloom, only at some heights). The chain now copies back once,
  after every band has finished. Any new parallel stage must keep the read set
  (whole source) and the write set (own rows) disjoint until all bands join.
* **Reserved CPU cores reach the frame through `RendererDesc::cpu_worker_count`.**
  The Configurator's `reserved_cpu_cores` sizes the renderer's owned filter pool
  (`scheduler::thread_pool_config_for`), and `FilterChain::run` splits
  row-splittable filters across it. `lx_cli render --cores N` and the GUI both
  set this; `set_thread_pool` overrides it with a caller-owned pool. Do not add a
  path that reserves cores but never uses them — that was the 0.0.1 defect.
* **A row-splittable filter recomputes its own halo.** `blur`, `sharpen` and
  `bloom` read `radius` rows outside the rows they write, so `apply_rows` widens
  its own working range to `[row_begin - radius, row_end + radius)` locally
  rather than depending on a neighbouring band. `supports_row_split()` promises
  exactly this independence; a filter that cannot honour it must not report it.
* **`blur_pass`/`box_blur`/`bright_pass` take explicit source and destination
  base rows.** A band's scratch buffer starts at global row `halo_begin`, not
  row 0, so indexing a band by its global row offset without subtracting the
  base row reads the wrong memory (or past the buffer).

## Gotchas (older, still true)

* **Unmap a mapped texture on every exit path.** `Renderer::render_frame` maps
  the target for the filter chain; it unmaps before returning on both the
  success and the failure path, so a filter error does not leak the mapping.

## Adding a module-level feature

1. Put it in the module that owns the concern; keep the directory's single
   responsibility.
2. Return `Status`/`Result<T>`; document the interface in
   `documentation/API.md`.
3. Add a test in the matching `tests/test_<module>.cpp` and register the target
   in `tests/CMakeLists.txt` if it is a new executable.
4. Update `documentation/MODULE_STATUS.md` if maturity changes.
