// LoucuraXXXX GPU - renderer.
//
// Owns a backend, a render target, and the filter chain. It turns a frame of
// input pixels into a presented frame by running the filter pipeline over the
// target and (when the backend supports it) presenting the result. The renderer
// never fabricates a frame: every stage is either executed or reported as
// unavailable.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backends/backend.hpp"
#include "core/status.hpp"
#include "filters/filter_chain.hpp"
#include "memory/buffer_pool.hpp"
#include "renderer/frame_stats.hpp"
#include "scheduler/thread_pool.hpp"
#include "shaders/shader_library.hpp"

namespace lx::renderer {

struct RendererDesc {
    u32 width = 0;
    u32 height = 0;
    backends::PixelFormat format = backends::PixelFormat::RGBA8Unorm;
    backends::BackendCreateInfo backend;
    std::string shader_asset_root;  // directory containing manifest.json
    bool enable_filters = true;

    // How many present targets the renderer rotates through (double/triple
    // buffering). Frame N writes into target N % max_in_flight_frames, so the
    // storage of the previous frames is not overwritten until that many frames
    // have gone by. A consumer that still holds the last frame's pixels can keep
    // reading them while the next one is drawn. Costs max_in_flight_frames
    // textures of width x height x 4 bytes. 0 is treated as 1.
    u32 max_in_flight_frames = 2;

    // Frame pacing. 0 means uncapped: render_frame returns as soon as the work
    // is done. A non-zero value makes render_frame wait until that many frames
    // per second would have elapsed, which is what the config's target_fps is
    // for. The wait happens after the frame is timed, so the measured frame cost
    // is the real work and never includes the deliberate sleep.
    u32 target_fps = 0;

    // Cores the virtual GPU may use for filter work. When > 0 the renderer owns
    // a thread pool of this size and runs row-splittable filters across it; 0
    // keeps the filter chain serial. `performance_cores`/`efficiency_cores` are
    // the host's hybrid topology (0/0 on a uniform CPU) so the pool can steer
    // latency-sensitive work to the performance cores.
    u32 cpu_worker_count = 0;
    u32 performance_cores = 0;
    u32 efficiency_cores = 0;
};

// A single input frame handed to the renderer.
struct FrameInput {
    const void* pixels = nullptr;
    u64 bytes = 0;
    u64 row_pitch = 0;  // 0 => tightly packed
    // What this frame is. Filters whose scope does not match are skipped, so a
    // game-only filter never touches the desktop and vice versa. Unknown applies
    // every enabled filter.
    filters::ContentScope scope = filters::ContentScope::Unknown;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Creates the backend and the render target. Fails cleanly when the
    // requested backend is unavailable.
    Status initialize(const RendererDesc& desc);

    // Releases every resource. Idempotent.
    void shutdown();

    bool initialized() const { return backend_ != nullptr; }
    const backends::BackendInfo& backend_info() const;
    const RendererDesc& desc() const { return desc_; }

    // The filter chain applied before presentation. Callers configure it before
    // the first render_frame call.
    filters::FilterChain& filter_chain() { return filter_chain_; }
    const filters::FilterChain& filter_chain() const { return filter_chain_; }

    // Loads filters from a list of (name, params) pairs, clearing the chain
    // first. Unknown filter names are reported rather than skipped silently.
    Status configure_filters(
        const std::vector<std::pair<std::string, filters::FilterParams>>& filters);

    // Uploads a frame, runs the filter chain, and presents it. `stats` receives
    // the timings for this frame when provided.
    Status render_frame(const FrameInput& input, FrameStats* stats = nullptr);

    // Copies the current target back to host memory.
    Result<u64> readback(void* out, u64 out_bytes, u64 row_pitch = 0);

    // Direct access to the render target's pixels for the CPU backend. Returns
    // nullptr for GPU backends, which must use readback().
    u8* cpu_target_pixels(u32* width = nullptr, u32* height = nullptr);

    const StatsWindow& stats() const { return stats_; }
    memory::BufferPool& buffer_pool() { return buffer_pool_; }

    // Gives the renderer a thread pool for parallel filter execution. The
    // renderer does not own the pool; the caller must keep it alive and start
    // it. Overrides any pool the renderer created from desc.cpu_worker_count.
    void set_thread_pool(scheduler::ThreadPool* pool) {
        thread_pool_ = pool;
        owns_pool_ = false;
    }

    // The thread pool the filter chain runs on, or nullptr when filters run
    // serially.
    scheduler::ThreadPool* thread_pool() const { return thread_pool_; }

    // Runs the enabled filter chain on the GPU backend, ping-ponging between the
    // present target and an auxiliary texture. Filters whose scope does not
    // match `frame_scope` are skipped. Returns NotImplemented when the backend
    // has no built-in for a filter's pass or a filter has no GPU plan, so the
    // caller can fall back to the host path. `filter_micros` receives the
    // wall-clock cost of the passes.
    Status run_filter_chain_on_gpu(filters::ContentScope frame_scope, u64* filter_micros);

    // True when every enabled, in-scope filter advertises a GPU plan and the
    // backend ships a built-in shader for it, i.e. run_filter_chain_on_gpu can
    // succeed.
    bool can_run_filters_on_gpu(filters::ContentScope frame_scope) const;

private:
    // Allocates the auxiliary textures the GPU filter chain needs. Idempotent.
    Status ensure_gpu_chain_resources();
    // Compiles and caches the pipeline for a built-in pass shader, once.
    Status ensure_gpu_pipeline(const std::string& shader_name);

    RendererDesc desc_{};
    std::unique_ptr<backends::Backend> backend_;
    // Present targets, rotated one per frame. `target_` is the one the current
    // frame writes into; readback()/cpu_target_pixels() refer to it.
    std::vector<backends::ResourceHandle> targets_;
    backends::ResourceHandle target_{};
    filters::FilterChain filter_chain_;
    memory::BufferPool buffer_pool_;
    std::unique_ptr<scheduler::ThreadPool> owned_pool_;
    scheduler::ThreadPool* thread_pool_ = nullptr;
    bool owns_pool_ = false;
    shaders::ShaderLibrary shaders_;
    StatsWindow stats_{};
    u64 frame_index_ = 0;
    // When the previous paced frame's slot ends. Pacing is measured from this
    // deadline rather than from the previous frame's start, so a frame that
    // overruns its budget does not make the next one wait too.
    u64 next_frame_deadline_nanos_ = 0;
    bool initialized_ = false;

    // GPU filter-chain resources. Two ping-pong textures carry the image from
    // filter to filter, and two scratch textures serve a filter's own
    // intermediate passes (bloom needs both).
    backends::ResourceHandle gpu_ping_a_{};
    backends::ResourceHandle gpu_ping_b_{};
    backends::ResourceHandle gpu_scratch_a_{};
    backends::ResourceHandle gpu_scratch_b_{};
    // Pipelines compiled for built-in passes, keyed by shader name, so a chain
    // compiles each pass once rather than per frame.
    std::vector<std::string> gpu_pipelines_;
    bool gpu_chain_ready_ = false;
};

}  // namespace lx::renderer
