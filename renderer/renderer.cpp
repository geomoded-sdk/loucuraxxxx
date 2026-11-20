#include "renderer/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <cstring>
#include <limits>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "filters/filter_registry.hpp"
#include "filters/gpu_plan.hpp"

namespace lx::renderer {

namespace {

// Copies frame input into mapped texture storage, honouring the texture's row
// pitch (which may be padded) and the input's own pitch. This is the zero-copy
// upload: the destination is the texture itself.
Status copy_into_mapped(const FrameInput& input, const backends::MappedTexture& mapped) {
    if (!mapped.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "mapped texture has no storage");
    }
    if (mapped.height == 0) return Status::success();
    const u64 tight_row = static_cast<u64>(mapped.width) * 4;  // RGBA8
    const u64 dst_pitch = mapped.row_pitch == 0 ? tight_row : mapped.row_pitch;
    const u64 src_pitch = input.row_pitch == 0 ? tight_row : input.row_pitch;
    if (dst_pitch < tight_row || src_pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    // The last row needs only `tight_row` bytes, so the true requirement is
    // src_pitch*(height-1) + tight_row. Checking tight_row*height instead would
    // under-count a padded source and let the final memcpy read past the end of
    // the caller's buffer. A caller-supplied pitch large enough to overflow that
    // product must be rejected, not wrapped into a small requirement.
    if (mapped.height > 1 &&
        src_pitch > (std::numeric_limits<u64>::max() - tight_row) / (mapped.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "frame input row pitch is absurd");
    }
    // The destination pitch comes from the backend; guard it the same way so a
    // bad backend cannot wrap the row offset below.
    if (mapped.height > 1 &&
        dst_pitch > (std::numeric_limits<u64>::max() - tight_row) / (mapped.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "mapped texture row pitch is absurd");
    }
    const u64 required = src_pitch * (mapped.height - 1) + tight_row;
    if (input.bytes < required) {
        return Status::error(ErrorCode::InvalidArgument, "frame input is smaller than the target");
    }
    const auto* src = static_cast<const u8*>(input.pixels);
    auto* dst = static_cast<u8*>(mapped.data);
    for (u32 y = 0; y < mapped.height; ++y) {
        std::memcpy(dst + y * dst_pitch, src + y * src_pitch, tight_row);
    }
    return Status::success();
}
}  // namespace

Renderer::~Renderer() { shutdown(); }

Status Renderer::initialize(const RendererDesc& desc) {
    if (initialized_) {
        return Status::error(ErrorCode::ResourceBusy, "renderer already initialized");
    }
    if (desc.width == 0 || desc.height == 0) {
        return Status::error(ErrorCode::InvalidArgument,
                             "renderer dimensions must be non-zero");
    }
    desc_ = desc;
    desc_.max_in_flight_frames = desc.max_in_flight_frames == 0 ? 1 : desc.max_in_flight_frames;

    auto backend = backends::create_backend(desc.backend);
    if (!backend.ok()) return backend.status();
    backend_ = std::move(backend.value());

    backends::TextureDesc target_desc;
    target_desc.width = desc.width;
    target_desc.height = desc.height;
    target_desc.format = desc.format;
    target_desc.sampled = true;
    target_desc.storage = true;
    target_desc.render_target = true;
    target_desc.debug_name = "present_target";

    // One target per in-flight frame. Rotating them means a consumer holding the
    // last presented frame keeps reading valid pixels while the next frame is
    // drawn into a different texture, which is what max_in_flight_frames means.
    const u32 target_count = desc_.max_in_flight_frames;
    targets_.reserve(target_count);
    for (u32 i = 0; i < target_count; ++i) {
        auto target = backend_->create_texture(target_desc);
        if (!target.ok()) {
            for (const auto& created : targets_) backend_->destroy_texture(created);
            targets_.clear();
            backend_.reset();
            return target.status();
        }
        targets_.push_back(target.value());
    }
    target_ = targets_.front();

    if (!desc.shader_asset_root.empty()) {
        Status shader_status = shaders_.load(desc.shader_asset_root);
        if (!shader_status.ok()) {
            // Shader bytecode is optional; the CPU kernels cover every pass.
            LX_WARN << "shader library not loaded: " << shader_status.message();
        }
    }

    // Own a filter thread pool sized to the reserved cores when the caller did
    // not hand us one. This is what makes reserved_cpu_cores affect the frame:
    // the filter chain splits row-splittable filters across these workers.
    if (desc.cpu_worker_count > 0) {
        scheduler::ThreadPoolConfig pool_config = scheduler::thread_pool_config_for(
            desc.cpu_worker_count, desc.performance_cores, desc.efficiency_cores,
            "lx-render");
        owned_pool_ = std::make_unique<scheduler::ThreadPool>(pool_config);
        Status pool_status = owned_pool_->start();
        if (!pool_status.ok()) {
            owned_pool_.reset();
            LX_WARN << "filter thread pool not started: " << pool_status.message();
        } else {
            thread_pool_ = owned_pool_.get();
            owns_pool_ = true;
        }
    }

    initialized_ = true;
    LX_INFO << "renderer initialized on backend '" << backends::to_string(backend_->info().kind)
            << "' (" << backend_->info().device_name << ") at " << desc.width << "x"
            << desc.height;
    return Status::success();
}

void Renderer::shutdown() {
    if (owned_pool_) {
        owned_pool_->stop();
        owned_pool_.reset();
    }
    if (owns_pool_) {
        thread_pool_ = nullptr;
        owns_pool_ = false;
    }
    if (backend_) {
        for (const auto& target : targets_) {
            if (target.valid()) backend_->destroy_texture(target);
        }
    }
    targets_.clear();
    target_ = {};
    if (backend_) {
        for (auto handle : {gpu_ping_a_, gpu_ping_b_, gpu_scratch_a_, gpu_scratch_b_}) {
            if (handle.valid()) backend_->destroy_texture(handle);
        }
    }
    gpu_ping_a_ = {};
    gpu_ping_b_ = {};
    gpu_scratch_a_ = {};
    gpu_scratch_b_ = {};
    gpu_pipelines_.clear();
    gpu_chain_ready_ = false;
    backend_.reset();
    buffer_pool_.clear();
    initialized_ = false;
}

const backends::BackendInfo& Renderer::backend_info() const {
    static const backends::BackendInfo kEmpty{};
    return backend_ ? backend_->info() : kEmpty;
}

Status Renderer::configure_filters(
    const std::vector<std::pair<std::string, filters::FilterParams>>& filters) {
    filter_chain_.clear();
    // A change to the chain invalidates compiled GPU pipelines for it.
    gpu_pipelines_.clear();
    for (const auto& [name, params] : filters) {
        auto filter = filters::create_filter(name);
        if (!filter.ok()) return filter.status();
        LX_TRY(filter_chain_.add(std::move(filter.value()), params));
    }
    return Status::success();
}

Status Renderer::ensure_gpu_pipeline(const std::string& shader_name) {
    for (const auto& name : gpu_pipelines_) {
        if (name == shader_name) return Status::success();
    }
    const char* source = backend_->builtin_shader_source(shader_name);
    if (source == nullptr) {
        return Status::error(ErrorCode::NotImplemented,
                             "backend has no built-in shader for '" + shader_name + "'");
    }
    LX_TRY(backend_->register_shader_source(shader_name, source));
    LX_TRY(backend_->create_pipeline(shader_name, shader_name));
    gpu_pipelines_.push_back(shader_name);
    return Status::success();
}

Status Renderer::ensure_gpu_chain_resources() {
    if (gpu_chain_ready_) return Status::success();
    backends::TextureDesc desc;
    desc.width = desc_.width;
    desc.height = desc_.height;
    desc.format = desc_.format;
    desc.sampled = true;
    desc.storage = true;
    desc.render_target = true;

    auto create = [&](const char* name, backends::ResourceHandle* out) -> Status {
        desc.debug_name = name;
        auto tex = backend_->create_texture(desc);
        LX_TRY(tex.status());
        *out = tex.value();
        return Status::success();
    };
    LX_TRY(create("gpu_chain_ping_a", &gpu_ping_a_));
    LX_TRY(create("gpu_chain_ping_b", &gpu_ping_b_));
    LX_TRY(create("gpu_chain_scratch_a", &gpu_scratch_a_));
    LX_TRY(create("gpu_chain_scratch_b", &gpu_scratch_b_));
    gpu_chain_ready_ = true;
    return Status::success();
}

bool Renderer::can_run_filters_on_gpu(filters::ContentScope frame_scope) const {
    if (!initialized_ || !desc_.enable_filters || filter_chain_.size() == 0) return false;
    if (backend_->capabilities().supports_host_mapping) return false;  // host path is cheaper
    bool any = false;
    for (const auto& entry : filter_chain_.entries()) {
        if (!entry.enabled) continue;
        if (!filters::scope_matches(entry.params.scope, frame_scope)) continue;
        any = true;
        const auto plan = entry.filter->gpu_plan(desc_.width, desc_.height, entry.params);
        if (!plan.supported) return false;
        for (const auto& pass : plan.passes) {
            if (backend_->builtin_shader_source(pass.shader_name) == nullptr) return false;
        }
    }
    return any;
}

Status Renderer::run_filter_chain_on_gpu(filters::ContentScope frame_scope,
                                         u64* filter_nanos) {
    const u64 start = now_nanos();
    LX_TRY(ensure_gpu_chain_resources());

    // Physical textures the logical slots resolve to. `current` starts as the
    // present target, where the frame was just uploaded.
    backends::ResourceHandle current = target_;
    backends::ResourceHandle ping[2] = {gpu_ping_a_, gpu_ping_b_};
    backends::ResourceHandle scratch[2] = {gpu_scratch_a_, gpu_scratch_b_};

    u32 ping_index = 0;
    for (const auto& entry : filter_chain_.entries()) {
        if (!entry.enabled) continue;
        if (!filters::scope_matches(entry.params.scope, frame_scope)) continue;
        const auto plan = entry.filter->gpu_plan(desc_.width, desc_.height, entry.params);
        if (!plan.supported) {
            return Status::error(ErrorCode::NotImplemented,
                                 "filter has no GPU plan; cannot run on this backend");
        }
        // `passes` repeats the whole filter. Each repetition needs a fresh
        // output texture, so the ping index advances per repetition rather than
        // per filter. The CPU chain applies the same rule, so a frame filtered on
        // either path looks the same.
        const filters::FilterInfo& info = entry.filter->info();
        const u32 iterations =
            std::min(std::max(entry.params.passes == 0 ? info.min_passes
                                                       : entry.params.passes,
                              info.min_passes),
                     info.max_passes);

        for (u32 iteration = 0; iteration < iterations; ++iteration) {
            const backends::ResourceHandle next = ping[ping_index];
            ping_index ^= 1u;

            for (const auto& pass : plan.passes) {
                LX_TRY(ensure_gpu_pipeline(pass.shader_name));
                auto encoder = backend_->begin_commands();
                if (!encoder.ok()) return encoder.status();
                LX_TRY(encoder.value()->bind_pipeline(pass.shader_name));
                for (u32 i = 0; i < pass.inputs.size(); ++i) {
                    backends::ResourceHandle physical;
                    switch (pass.inputs[i]) {
                        case 0: physical = current; break;
                        case 1: physical = next; break;
                        case 2: physical = scratch[0]; break;
                        case 3: physical = scratch[1]; break;
                        default:
                            return Status::error(ErrorCode::InvalidArgument,
                                                 "GPU plan references an unknown texture slot");
                    }
                    LX_TRY(encoder.value()->bind_texture(i, physical));
                }
                backends::ResourceHandle out = (pass.output == 0)   ? current
                                               : (pass.output == 1) ? next
                                               : (pass.output == 2) ? scratch[0]
                                                                    : scratch[1];
                LX_TRY(encoder.value()->bind_storage_texture(0, out));
                if (!pass.push_constants.empty()) {
                    LX_TRY(encoder.value()->set_push_constants(
                        pass.push_constants.data(),
                        static_cast<u32>(pass.push_constants.size())));
                }
                backends::DispatchDesc dispatch;
                dispatch.kernel_name = pass.shader_name;
                LX_TRY(encoder.value()->dispatch(dispatch));
                LX_TRY(backend_->submit(encoder.value().get()));
            }
            current = next;
        }  // passes loop
    }

    // The last filter wrote to a scratch texture; blit the result into the
    // present target so readback() and present() see it.
    if (current.id != target_.id) {
        LX_TRY(ensure_gpu_pipeline("copy"));
        auto encoder = backend_->begin_commands();
        if (!encoder.ok()) return encoder.status();
        LX_TRY(encoder.value()->bind_pipeline("copy"));
        LX_TRY(encoder.value()->bind_texture(0, current));
        LX_TRY(encoder.value()->bind_storage_texture(0, target_));
        backends::DispatchDesc dispatch;
        dispatch.kernel_name = "copy";
        LX_TRY(encoder.value()->dispatch(dispatch));
        LX_TRY(backend_->submit(encoder.value().get()));
    }

    // One sync for the whole chain instead of one per pass. Per-pass ordering
    // does not need it: the GL path is command-ordered on a single context, the
    // D3D11 path rewrites its constant buffer with WRITE_DISCARD (rename, not
    // in-place), and Vulkan passes real push constants while its encoder already
    // waits on its own fence. Waiting between passes would serialise the queue
    // for no ordering benefit. No figure is quoted here: the cost of an extra
    // sync is adapter- and driver-dependent, and this project only states numbers
    // that benchmarks/ can reproduce on the machine in front of you.
    // The wait is kept here, before the timestamp, so filter_time_nanos still
    // covers the GPU work rather than just the submissions.
    LX_TRY(backend_->wait_idle());

    if (filter_nanos) *filter_nanos = now_nanos() - start;
    return Status::success();
}

Status Renderer::render_frame(const FrameInput& input, FrameStats* stats) {
    if (!initialized_) {
        return Status::error(ErrorCode::InitializationFailed, "renderer not initialized");
    }
    if (input.pixels == nullptr || input.bytes == 0) {
        return Status::error(ErrorCode::InvalidArgument, "frame input is empty");
    }

    // Rotate to this frame's target before any work touches it. With N > 1 the
    // texture drawn two frames ago is reused only after N frames, so a consumer
    // holding the previous frame's pixels is not reading storage being overwritten.
    if (targets_.size() > 1) {
        target_ = targets_[static_cast<std::size_t>(frame_index_) % targets_.size()];
    }
    FrameStats frame;
    frame.frame_index = frame_index_++;
    const u64 start = now_nanos();

    // On a unified-memory backend the texture *is* host memory, so we map it and
    // write the frame straight in: no staging copy, no upload. On a discrete
    // backend map_texture is unsupported and we fall back to upload_texture.
    const bool want_host_access =
        desc_.enable_filters && filter_chain_.size() > 0 &&
        backend_->capabilities().supports_host_mapping;

    if (want_host_access) {
        auto mapped = backend_->map_texture(target_);
        if (!mapped.ok()) {
            return Status::error(ErrorCode::InitializationFailed,
                                 "backend claims host mapping but map_texture failed");
        }
        filters::Image image{static_cast<u8*>(mapped.value().data), mapped.value().width,
                             mapped.value().height};
        // Writing into the mapped storage is the upload.
        const Status copy_status = copy_into_mapped(input, mapped.value());
        frame.bytes_uploaded = copy_status.ok() ? input.bytes : 0;
        // The texture must be unmapped on every exit path, including failure,
        // or the backend is left holding a host mapping.
        Status filter_status = copy_status;
        filters::ChainStats chain_stats;
        if (copy_status.ok()) {
            filter_status = filter_chain_.run(image, buffer_pool_, thread_pool_, &chain_stats,
                                              input.scope);
            frame.filter_time_nanos = chain_stats.total_nanoseconds;
        }
        const Status unmap_status = backend_->unmap_texture(target_);
        if (!filter_status.ok()) return filter_status;
        LX_TRY(unmap_status);
    } else {
        LX_TRY(backend_->upload_texture(target_, input.pixels, input.bytes, input.row_pitch));
        frame.bytes_uploaded = input.bytes;

        // No host mapping: run the filter chain on the GPU, pass by pass. This is
        // the path that makes the filters real on a discrete/GPU backend instead
        // of silently skipping them.
        if (can_run_filters_on_gpu(input.scope)) {
            u64 filter_nanos = 0;
            const Status filter_status = run_filter_chain_on_gpu(input.scope, &filter_nanos);
            if (!filter_status.ok()) return filter_status;
            frame.filter_time_nanos = filter_nanos;
        } else if (desc_.enable_filters && filter_chain_.size() > 0) {
            // Some filter has no GPU plan on this backend. The caller sees the
            // chain was not applied rather than a silently unfiltered frame.
            LX_DEBUG << "filters requested but not runnable on this backend; "
                        "frame passed through unfiltered";
        }
    }
    const u64 after_filters = now_nanos();

    LX_TRY(backend_->submit(nullptr));
    LX_TRY(backend_->wait_idle());
    const u64 after_present = now_nanos();

    // cpu_time_nanos is the whole frame's CPU cost: upload, filters, submit and
    // present. Upload and present are broken out separately for inspection.
    // Because it includes wait_idle(), it already covers the GPU's work for this
    // frame, which is why StatsWindow can use it as wall-clock frame time.
    frame.cpu_time_nanos = after_present - start;
    frame.present_time_nanos = after_present - after_filters;
    // No backend reports a GPU-side duration yet: supports_timestamps is set by
    // the CPU and Vulkan backends, but nothing queries it. Leaving this at 0 is
    // deliberate rather than an oversight, and it must stay 0 until a real query
    // exists, because StatsWindow adds it to cpu_time_nanos and a guess there
    // would double-count the frame.
    frame.gpu_time_nanos = 0;

    if (stats) *stats = frame;
    stats_.push(frame);

    // Frame pacing. The sleep is deliberately outside the timestamps above, so a
    // capped renderer reports the same frame cost as an uncapped one and only
    // the wall-clock rate differs. Deadline-based rather than sleep-per-frame:
    // if this frame overran its slot, the next deadline is already in the past
    // and no extra wait is added.
    if (desc_.target_fps > 0) {
        const u64 frame_period = 1'000'000'000ull / desc_.target_fps;
        const u64 now = now_nanos();
        if (next_frame_deadline_nanos_ == 0) {
            next_frame_deadline_nanos_ = now + frame_period;
        } else {
            if (now < next_frame_deadline_nanos_) {
                const u64 remaining = next_frame_deadline_nanos_ - now;
                std::this_thread::sleep_for(std::chrono::nanoseconds(remaining));
            }
            // Advance by one period from the previous deadline, and resynchronise
            // if the renderer fell more than a whole period behind, so a stall
            // does not produce a burst of catch-up frames.
            next_frame_deadline_nanos_ += frame_period;
            const u64 after = now_nanos();
            if (next_frame_deadline_nanos_ + frame_period < after) {
                next_frame_deadline_nanos_ = after + frame_period;
            }
        }
    }

    return Status::success();
}

Result<u64> Renderer::readback(void* out, u64 out_bytes, u64 row_pitch) {
    if (!initialized_) {
        return Status::error(ErrorCode::InitializationFailed, "renderer not initialized");
    }
    return backend_->download_texture(target_, out, out_bytes, row_pitch);
}

u8* Renderer::cpu_target_pixels(u32* width, u32* height) {
    // Prefer the backend's own mapping so this works for any backend that
    // exposes host-visible storage, not just the CPU backend.
    if (!backend_ || !backend_->capabilities().supports_host_mapping) return nullptr;
    auto mapped = backend_->map_texture(target_);
    if (!mapped.ok()) return nullptr;
    if (width) *width = mapped.value().width;
    if (height) *height = mapped.value().height;
    return static_cast<u8*>(mapped.value().data);
}

}  // namespace lx::renderer
