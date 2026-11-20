# API reference

Interfaces are grouped by module. All types live in namespace `lx` or a
sub-namespace. Errors are returned as `Status` / `Result<T>`; no module throws
across its boundary.

## core

```cpp
// core/status.hpp
enum class ErrorCode { Ok, Unknown, InvalidArgument, OutOfRange, NotSupported,
                       NotImplemented, OutOfMemory, IoError, ParseError,
                       DeviceLost, ResourceBusy, Timeout, Cancelled,
                       InitializationFailed, BackendUnavailable };

class Status {
  static Status success();
  static Status error(ErrorCode code, std::string message);
  bool ok() const;
  ErrorCode code() const;
  const std::string& message() const;
};

template <typename T> class Result {
  bool ok() const;
  T& value();  const T& value() const;
  const Status& status() const;
};
```

`LX_TRY(expr)` returns early when `expr` is a failed `Status` or `Result<T>`,
propagating the error.

```cpp
// core/json.hpp
class Json {                       // parse / build / dump, strict
  static Result<Json> parse(const std::string& text);
  std::string dump(int indent = 2) const;
  bool has(const std::string& key) const;
  const Json& at(const std::string& key) const;   // Null when absent
};
```

## hardware

```cpp
Result<HardwareInfo> detect_hardware();
Result<std::vector<GpuDeviceInfo>> enumerate_system_gpus();
u32 logical_processor_count();
```

`HardwareInfo` carries `CpuInfo`, `MemoryInfo`, `OperatingSystemInfo` and a
`std::vector<GpuDeviceInfo>`. `primary_gpu()` returns the first device that is
real hardware (never a software rasterizer).

```cpp
enum class MemoryTopology { Unknown, Unified, Discrete };

// CpuInfo: ... performance_cores, efficiency_cores ...
// GpuDeviceInfo: ... unified_memory, dedicated_vram_bytes ...

MemoryTopology HardwareInfo::memory_topology() const;  // from the primary device
bool HardwareInfo::has_unified_memory() const;         // == Unified
bool HardwareInfo::is_hybrid_cpu() const;              // P-cores and E-cores both present
```

## memory

```cpp
class MemoryPolicy {
  ReservationPlan plan(const hardware::MemoryInfo& memory, u64 requested_bytes) const;
};

class MemoryBudget {
  void set_limit(u64 bytes);
  Status commit(u64 bytes, const std::string& owner);  // OutOfMemory when over
  void release(u64 bytes);
  u64 limit() const; u64 used() const; u64 peak() const; u64 available() const;
};

class BufferPool {
  Result<Buffer> acquire(u64 bytes);
  void release(Buffer&& buffer);
  Stats stats() const;
};
```

## scheduler

```cpp
enum class CoreClass { Performance, Efficiency };

struct ThreadPoolConfig {
  u32 thread_count;          // 0 => hardware_concurrency() - 1
  bool allow_stealing;
  std::vector<u32> pin_to_cpus;
  u32 performance_threads;   // leading workers that are performance-class
};

// Splits `total` workers across a hybrid CPU's core classes.
u32 recommend_performance_threads(u32 total, u32 performance_cores, u32 efficiency_cores);

class ThreadPool {
  Status start();
  Status submit(Task task, u32 lane = 0);   // lane preserves order within it
  Status submit_on(Task task, CoreClass c); // prefers a core class; never drops
  Status wait_idle();
  void stop();
  std::size_t pending() const;  // in flight: queued + running
  std::size_t queued() const;   // queue depth: waiting for a worker
  double utilization() const;   // measured busy worker-time / worker-time
  u64 busy_nanoseconds() const; // task time, including tasks still running
  u32 thread_count() const;
  u32 performance_worker_count() const;
  u32 efficiency_worker_count() const;
  CoreClass worker_class(u32 index) const;
};

class CostModel {
  DispatchDecision decide(const DispatchRequest& request) const;
  void set_constants(const CostConstants& c);
};
// CostConstants::unified_memory removes transfer cost entirely.
// DispatchDecision: target, estimated_{cpu,gpu,split}_seconds, rationale.

class TaskGraph {
  NodeId add_node(std::string name, ThreadPool::Task task);
  Status add_dependency(NodeId node, NodeId dependency);  // rejects cycles
  u32 submit_ready();
  Status run_to_completion();
};
```

## configurator

```cpp
class Configurator {
  void attach_host(const hardware::HardwareInfo& hardware);
  ValidationReport validate(const Config& requested) const;
  Result<ValidationReport> apply(const Config& requested);
  Result<ValidationReport> load(const std::string& path);
  Status save(const std::string& path) const;
  static Result<std::string> default_config_path();
};
// ValidationReport: effective, adjustments[], errors[].
```

## backends

```cpp
class Backend {
  const BackendInfo& info() const;
  const BackendCapabilities& capabilities() const;
  Result<TextureHandle> create_texture(const TextureDesc&);
  Status upload_texture(TextureHandle, const void* data, u64 bytes, u64 row_pitch = 0);
  Result<u64> download_texture(TextureHandle, void* out, u64 out_bytes, u64 row_pitch = 0);
  // Zero-copy path: valid only when capabilities().supports_host_mapping.
  Result<MappedTexture> map_texture(TextureHandle);   // NotSupported otherwise
  Status unmap_texture(TextureHandle);
  Status register_shader(const std::string& name, const std::vector<u32>& spirv);
  Status register_shader_source(const std::string& name, const std::string& source);
  Status create_pipeline(const std::string& name, const std::string& kernel);
  Status register_cpu_kernel(const std::string& name, CpuKernel kernel);
  Result<std::unique_ptr<CommandEncoder>> begin_commands();
  Status submit(CommandEncoder*);
  Status wait_idle();
  u64 allocated_bytes() const;
};

struct BackendCapabilities {
  // ... compute/storage/timestamps ...
  bool unified_memory;         // device shares the CPU's physical memory
  bool supports_host_mapping;  // map_texture() is supported
  bool is_hardware;            // backed by a real GPU, not a software rasterizer
};

struct MappedTexture { void* data; u32 width, height; u64 row_pitch; };

struct BackendProbeResult {
  bool available;           // the API can be used on this host
  std::string reason;       // why not, when unavailable
  std::string device_name;  // best device found, when available
  bool is_hardware;         // false for lavapipe / llvmpipe / WARP
};

struct AvailableBackend {
  BackendKind kind;
  std::string name;
  std::string device_name;
  std::string reason;
  Maturity maturity;
  bool is_hardware;         // false for the CPU path and software rasterizers
};

Result<std::unique_ptr<Backend>> create_backend(const BackendCreateInfo&);
const std::vector<BackendRegistration>& registered_backends();
std::vector<AvailableBackend> probe_available_backends();
```

`BackendCreateInfo::preferred` selects the implementation. When it is not
compiled in, `create_backend` returns `BackendUnavailable` — it never silently
substitutes a different backend.

`BackendCreateInfo::preferred_device` is a case-insensitive substring of the
device name, so `"4090"` selects an `NVIDIA GeForce RTX 4090`. The matching rule
lives in `backends/device_select.hpp` (`select_device`), which returns both the
chosen index and whether the preference actually matched. The Vulkan backend
offers it only the devices that have a compute queue, and when nothing matches it
logs a warning and uses the default device: a stale preference must not take the
GPU away, and it must not look honoured when it was not. Backends with a single
device, or that do not enumerate, ignore the field.

`probe_available_backends()` returns only backends usable on this host, ordered
hardware first: real GPUs before software rasterizers, and within each group the
preference ladder (Vulkan, Metal, Direct3D 11, OpenGL) before the CPU fallback.
A host with no Vulkan device reports no Vulkan entry at all, and a host whose
only Vulkan device is Mesa lavapipe reports it with `is_hardware == false`.

`map_texture()` is the zero-copy contract. On a unified-memory backend the
returned pointer *is* the texture's storage, so writing through it is the
upload. Backends without host mapping (a discrete GPU whose textures live only
in device memory) return `NotSupported`, and callers fall back to
`upload_texture`/`download_texture`. `register_shader_source` is refused by
backends that only accept pre-compiled bytecode (the CPU reference backend).

The **Vulkan backend** (opt-in) accepts both: `register_shader(name, bytecode)`
takes pre-compiled SPIR-V and `register_shader_source(name, glsl)` takes GLSL
compute source, which it compiles to SPIR-V at run time with shaderc when the
build found it. Its `builtin_shader_source()` returns GLSL compute source, the
same way the OpenGL backend returns GLSL fragment source and Direct3D 11 returns
HLSL.

## filters

```cpp
class IFilter {
  virtual const FilterInfo& info() const = 0;
  virtual Status apply(const ConstImage& source, Image destination,
                       const FilterParams& params) const = 0;
  // Applies only rows [row_begin, row_end). Reads all of `source` (so a filter
  // with a halo can reach outside its band) and writes only its own rows.
  virtual Status apply_rows(const ConstImage& source, Image destination,
                            const FilterParams& params, u32 row_begin,
                            u32 row_end) const;   // default delegates to apply()
  virtual bool supports_row_split() const;         // default false
  virtual bool supports_in_place() const;          // default false
  virtual std::string gpu_kernel_name() const;     // empty when no GPU kernel
};

Result<FilterPtr> create_filter(const std::string& name);
const std::vector<FilterRegistration>& registered_filters();
std::vector<std::string> filter_names();

class FilterChain {
  Status add(FilterPtr filter, FilterParams params);
  Status run(Image target, memory::BufferPool& pool,
             scheduler::ThreadPool* pool_threads, ChainStats* stats,
             ContentScope frame_scope = ContentScope::Unknown);
};

enum class ContentScope { Unknown, Game, System };
ContentScope content_scope_from_string(const std::string&);
bool scope_matches(ContentScope filter_scope, ContentScope frame_scope);
```

`FilterParams{ intensity, passes, quality, scope }`. `intensity` 0 is always an
exact no-op.

`passes` is how many times the filter runs, clamped to the range the filter
declares in `FilterInfo::min_passes`/`max_passes`; 0 means "the filter's minimum".
It applies on both paths -- `FilterChain::run` iterates the filter, and the GPU
path repeats the filter's whole `gpu_plan` -- so a frame filtered on either path
looks the same. More passes of a smoothing filter blur further, which is what the
parameter is for. It previously travelled from the config through the GUI and the
JSON round-trip into `FilterParams` and was then read by nothing, so setting it
changed nothing; two tests now fail if that regresses.

`scope` classifies what a filter is for. `run` is given the frame's own
`frame_scope` and skips any filter whose scope does not match, which is how a
filter tuned for game content avoids touching the desktop UI (and vice versa).
`Unknown` on either side is a wildcard: an unclassified frame matches every
filter, and a filter with no declared scope applies to every frame. So a default
configuration behaves exactly as before, and scoping only ever removes work.

When `pool_threads` is non-null and running, `run` splits a filter that reports
`supports_row_split()` into one contiguous band per worker and executes the bands
in parallel. Each band reads only `source` and writes only its own rows, so the
result is byte-identical to the serial path; this is asserted for every built-in
in `tests/test_filters.cpp`. Filters that do not split (or images shorter than
`2 * 16` rows) run serially.

## renderer

`RendererDesc::target_fps` paces `render_frame`. It is the only field in the
config that changes the rate rather than the image. The wait happens after the
frame has been timed, so `FrameStats::cpu_time_nanos` is the work and never the
deliberate sleep, and a capped renderer reports the same per-frame cost as an
uncapped one. The CLI therefore prints two measured numbers: the achieved wall
clock rate (which the cap lowers) and the render work rate (which it does not).

```cpp
struct RendererDesc {
  u32 width, height;
  // ... backend / format / shader_asset_root / enable_filters ...
  u32 cpu_worker_count = 0;   // cores for filter work; 0 => serial
  u32 performance_cores = 0;  // host hybrid topology, for the pool split
  u32 efficiency_cores = 0;
  u32 target_fps = 0;         // 0 => uncapped; N => pace to N frames/second
};

class Renderer {
  Status initialize(const RendererDesc& desc);
  void shutdown();
  Status configure_filters(const std::vector<std::pair<std::string, filters::FilterParams>>&);
  Status render_frame(const FrameInput& input, FrameStats* stats = nullptr);
  Result<u64> readback(void* out, u64 out_bytes, u64 row_pitch = 0);
  u8* cpu_target_pixels(u32* width = nullptr, u32* height = nullptr);
  const StatsWindow& stats() const;
  void set_thread_pool(scheduler::ThreadPool*);   // caller-owned; overrides desc
  scheduler::ThreadPool* thread_pool() const;
};
```

`RendererDesc::cpu_worker_count` is where the Configurator's reserved CPU cores
reach the render path: the renderer owns a pool of that size and the filter chain
runs across it. `set_thread_pool` lets a caller supply its own pool instead (the
benchmark and the manager do this).

`FrameInput` carries a `filters::ContentScope scope` (default `Unknown`). The
renderer passes it to the filter chain, so a game-classified frame runs game
filters and a system-classified frame runs system filters. The classification is
the caller's: the renderer does not guess what a frame is.

The GUI model exposes the same knobs without a window:

```cpp
Status GuiModel::set_filter_enabled(const std::string& name, bool enabled);
Status GuiModel::set_filter_scope(const std::string& name, const std::string& scope);
Status GuiModel::set_filter_intensity(const std::string& name, f32 intensity);
void   GuiModel::sync_filter_choices();   // mirrors config.filters into state().filters
```

`set_filter_enabled` creates the config entry on first enable and keeps it (with
its intensity and scope) on disable, so toggling off and on preserves tuning;
the chain already skips disabled entries. `state().filters` carries a per-filter
`enabled` and `scope` so a front end can draw the toggles directly.

## manager

```cpp
class Manager {
  Status initialize(backends::Backend* backend, memory::MemoryBudget* budget,
                    const ManagerConfig& config);
  void shutdown();
  Result<DispatchOutcome> plan(const WorkItem& item);
  std::optional<DispatchOutcome> next_cpu_task();
  bool has_gpu() const;
  void refresh_utilization();
  const Telemetry& telemetry() const;
};
// DispatchOutcome: id, target, estimated_seconds, bytes_transferred, rationale.
// Telemetry::snapshot() -> TelemetrySnapshot (counters, queue depth, bytes,
//   budget, and the utilization fields described below).
//
// Two fields in the snapshot are deliberately named for what they measure rather
// than what they resemble:
//   cpu_queue_pressure       queued work / worker count, clamped to 0..1. This is
//                            queue backlog, not CPU utilization: it reads 0.0 when
//                            one long pass occupies every worker with an empty
//                            queue.
//   cpu_worker_utilization   measured occupancy of the reserved workers, 0..1:
//                            task execution time summed over workers divided by
//                            worker-time elapsed. Taken from ThreadPool timing, so
//                            it is high while workers run tasks whatever the queue
//                            depth. Covers the reserved threads only, not the
//                            whole machine.
//   gpu_utilization          only meaningful when gpu_utilization_available is
//                            true. No backend exposes a utilization counter yet, so
//                            the flag is false and the value stays 0.0 -- an
//                            unmeasured GPU, not an idle one.
```

## driver

```cpp
class Driver {
  Status initialize(const configurator::Config& config);
  void shutdown();
  backends::Backend* backend();
  memory::MemoryBudget& memory_budget();
  const DriverStatus& status() const;
  static std::vector<backends::BackendRegistration> probe_backends();
};
// DriverStatus::integration is an IntegrationStatus with an IntegrationLevel of
// Unavailable, UserSpaceOnly or KernelAssisted.
```

## gui

```cpp
namespace lx::gui {

struct GuiState {
    configurator::Config config;      // editing copy (clamped by refresh/validate)
    u64 total_memory_bytes = 0;
    u64 max_ram_bytes = 0;            // safe ceiling after the OS reserve
    u32 logical_cores = 0;
    u32 max_cpu_cores = 0;
    std::string os_summary;
    std::vector<std::string> backends;
    std::vector<std::string> filters;
    bool validation_ok = false;
    std::vector<std::string> log;     // user-visible message log
};

class GuiModel {
  Status refresh_host();               // detect host, clamp config to safe limits
  Status validate();                   // adopt the effective (clamped) config
  Status render_frames(u32 width, u32 height, u32 frames);
  backends::BackendKind requested_backend_kind() const;
  GuiState& state();
  const GuiState& state() const;
  void clear_log();
};

}  // namespace lx::gui
```

`GuiModel` is display-free and is the only place GUI behaviour should live; the
Win32 and X11 front ends in `gui/platform/` are thin renderers over it. See
`tests/test_gui.cpp`.

## Adding a backend

1. Implement `Backend` in `backends/<name>/`.
2. Provide a `probe()` returning `BackendProbeResult` and a `create()` factory.
3. Add a `register_<name>_backend()` function and call it from
   `ensure_builtin_backends_registered()` in `backends/backend_registry.cpp`.

Registration is an explicit call, not a file-scope initialiser, because a
static-library object that nothing references is dropped by the linker and its
initialiser never runs.

## Adding a filter

1. Implement `IFilter` in `filters/builtin/`.
2. Expose `register_<name>_filter()` and call it from
   `ensure_builtin_filters_registered()` in `filters/filter_registry.cpp`.
3. Add a test in `tests/test_filters.cpp`.
