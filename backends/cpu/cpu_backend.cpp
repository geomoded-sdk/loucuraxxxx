#include "backends/cpu/cpu_backend.hpp"

#include <cstring>
#include <exception>
#include <limits>
#include <string>

#include "backends/backend_registry.hpp"
#include "core/log.hpp"
#include "hardware/hardware_info.hpp"

namespace lx::backends {

// Executes bound pipelines over bound textures. The CPU backend has no real
// command buffer: a "dispatch" invokes the registered kernel once per texture
// set, which is exactly what a compute pass does.
class CpuBackend::Encoder : public CommandEncoder {
public:
    explicit Encoder(CpuBackend& backend) : backend_(backend) {}

    Status bind_pipeline(const std::string& pipeline_name) override {
        auto it = backend_.pipelines_.find(pipeline_name);
        if (it == backend_.pipelines_.end()) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "unknown pipeline '" + pipeline_name + "'");
        }
        current_shader_ = it->second.shader_name;
        return Status::success();
    }

    Status bind_texture(u32 slot, ResourceHandle texture) override {
        if (!texture.valid()) {
            return Status::error(ErrorCode::InvalidArgument, "invalid texture handle");
        }
        if (slot >= textures_.size()) textures_.resize(slot + 1);
        textures_[slot] = texture;
        return Status::success();
    }

    Status bind_storage_texture(u32 slot, ResourceHandle texture) override {
        return bind_texture(slot, texture);
    }

    Status set_push_constants(const void* data, u32 bytes) override {
        push_.assign(static_cast<const u8*>(data),
                     static_cast<const u8*>(data) + bytes);
        return Status::success();
    }

    Status dispatch(const DispatchDesc& desc) override {
        const std::string& kernel_name =
            desc.kernel_name.empty() ? current_shader_ : desc.kernel_name;
        auto it = backend_.kernels_.find(kernel_name);
        if (it == backend_.kernels_.end()) {
            return Status::error(ErrorCode::NotImplemented,
                                 "no CPU kernel registered for '" + kernel_name + "'");
        }
        return it->second.invoke(push_.empty() ? nullptr : push_.data(),
                                 static_cast<u32>(push_.size()), textures_);
    }

private:
    CpuBackend& backend_;
    std::string current_shader_;
    std::vector<ResourceHandle> textures_;
    std::vector<u8> push_;
};

CpuBackend::CpuBackend(const BackendCreateInfo&) {
    info_.kind = BackendKind::Cpu;
    info_.device_name = "CPU reference backend";
    info_.driver_name = "loucuraxxxx-cpu";
    info_.api_version = "1.0";
    info_.maturity = Maturity::Implemented;
    info_.capabilities.is_hardware = false;
    info_.capabilities.supports_compute = true;
    info_.capabilities.supports_storage_textures = true;
    info_.capabilities.supports_float_textures = true;
    info_.capabilities.supports_parallel_dispatch = true;
    info_.capabilities.supports_timestamps = true;  // host clock is available
    info_.capabilities.max_texture_dimension = 65535;
    info_.capabilities.max_workgroup_size = hardware::logical_processor_count();
    info_.capabilities.max_memory_bytes = 0;  // governed by the memory budget
    // The CPU backend allocates in ordinary host memory, so it is unified with
    // the CPU by construction and can always hand out a host pointer.
    info_.capabilities.unified_memory = true;
    info_.capabilities.supports_host_mapping = true;
}

CpuBackend::~CpuBackend() = default;

Result<ResourceHandle> CpuBackend::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return Status::error(ErrorCode::InvalidArgument,
                             "texture dimensions must be non-zero");
    }
    if (desc.width > info_.capabilities.max_texture_dimension ||
        desc.height > info_.capabilities.max_texture_dimension) {
        return Status::error(ErrorCode::OutOfRange, "texture exceeds maximum dimension");
    }
    const u64 bytes = texture_byte_size(desc.width, desc.height, desc.format);
    std::lock_guard<std::mutex> lock(mutex_);
    TextureRecord record;
    record.desc = desc;
    // A huge frame can exceed what the host can allocate; report it as an error
    // instead of letting std::bad_alloc escape across the API boundary.
    try {
        record.pixels.assign(static_cast<std::size_t>(bytes), 0);
    } catch (const std::exception& e) {
        return Status::error(ErrorCode::OutOfMemory,
                             "failed to allocate " + std::to_string(bytes) +
                                 " bytes for texture: " + e.what());
    }
    const u64 id = next_id_++;
    allocated_bytes_ += bytes;
    textures_.emplace(id, std::move(record));
    return ResourceHandle{id};
}

Status CpuBackend::destroy_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    allocated_bytes_ -= it->second.pixels.size();
    textures_.erase(it);
    return Status::success();
}

Status CpuBackend::upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                                  u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    TextureRecord& record = it->second;
    const u32 bpp = bytes_per_pixel(record.desc.format);
    const u64 tight_row = static_cast<u64>(record.desc.width) * bpp;
    const u64 effective_pitch = row_pitch == 0 ? tight_row : row_pitch;

    if (effective_pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    // Reject a pitch that would overflow the requirement computation.
    if (record.desc.height > 1 &&
        effective_pitch >
            (std::numeric_limits<u64>::max() - tight_row) / (record.desc.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch is absurd");
    }
    const u64 required = effective_pitch * (record.desc.height - 1) + tight_row;
    if (bytes < required) {
        return Status::error(ErrorCode::InvalidArgument,
                             "source buffer too small for texture");
    }

    const auto* src = static_cast<const u8*>(data);
    if (effective_pitch == tight_row) {
        std::memcpy(record.pixels.data(), src, static_cast<std::size_t>(required));
    } else {
        for (u32 y = 0; y < record.desc.height; ++y) {
            std::memcpy(record.pixels.data() + static_cast<std::size_t>(y) * tight_row,
                        src + static_cast<std::size_t>(y) * effective_pitch,
                        static_cast<std::size_t>(tight_row));
        }
    }
    return Status::success();
}

Result<u64> CpuBackend::download_texture(ResourceHandle handle, void* out,
                                         u64 out_bytes, u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    TextureRecord& record = it->second;
    const u32 bpp = bytes_per_pixel(record.desc.format);
    const u64 tight_row = static_cast<u64>(record.desc.width) * bpp;
    const u64 effective_pitch = row_pitch == 0 ? tight_row : row_pitch;
    // Without this check a caller-supplied pitch smaller than a row would make
    // the per-row writes overlap. upload_texture already rejects it; download
    // must be symmetric.
    if (effective_pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    if (record.desc.height > 1 &&
        effective_pitch >
            (std::numeric_limits<u64>::max() - tight_row) / (record.desc.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch is absurd");
    }
    const u64 required = effective_pitch * (record.desc.height - 1) + tight_row;
    if (out_bytes < required) {
        return Status::error(ErrorCode::InvalidArgument, "destination buffer too small");
    }

    auto* dst = static_cast<u8*>(out);
    if (effective_pitch == tight_row) {
        std::memcpy(dst, record.pixels.data(), static_cast<std::size_t>(required));
    } else {
        for (u32 y = 0; y < record.desc.height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * effective_pitch,
                        record.pixels.data() + static_cast<std::size_t>(y) * tight_row,
                        static_cast<std::size_t>(tight_row));
        }
    }
    return required;
}

Status CpuBackend::register_shader(const std::string& name,
                                   const std::vector<u32>& bytecode) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The CPU backend does not execute SPIR-V; it records the module so the
    // pipeline can still be validated for consistency across backends.
    shaders_[name] = bytecode;
    return Status::success();
}

Status CpuBackend::register_cpu_kernel(const std::string& name, CpuKernel kernel) {
    if (!kernel.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "null CPU kernel");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    kernels_[name] = kernel;
    return Status::success();
}

Status CpuBackend::create_pipeline(const std::string& name,
                                   const std::string& shader_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (kernels_.find(shader_name) == kernels_.end() &&
        shaders_.find(shader_name) == shaders_.end()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "shader '" + shader_name + "' is not registered");
    }
    pipelines_[name] = PipelineRecord{shader_name};
    return Status::success();
}

Result<std::unique_ptr<CommandEncoder>> CpuBackend::begin_commands() {
    return std::unique_ptr<CommandEncoder>(new Encoder(*this));
}

Status CpuBackend::submit(CommandEncoder*) {
    // The CPU encoder runs work eagerly on dispatch(), so submission is a no-op
    // beyond validating that the encoder belonged to this backend.
    return Status::success();
}

Status CpuBackend::wait_idle() { return Status::success(); }
Status CpuBackend::sync() { return Status::success(); }

u64 CpuBackend::allocated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocated_bytes_;
}

Result<MappedTexture> CpuBackend::map_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    TextureRecord& record = it->second;
    MappedTexture mapped;
    mapped.data = record.pixels.data();
    mapped.width = record.desc.width;
    mapped.height = record.desc.height;
    mapped.row_pitch = static_cast<u64>(record.desc.width) * bytes_per_pixel(record.desc.format);
    return mapped;
}

Status CpuBackend::unmap_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (textures_.find(handle.id) == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    // Host memory needs no flush.
    return Status::success();
}

u8* CpuBackend::texture_data(ResourceHandle handle, u32* width, u32* height) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) return nullptr;
    if (width) *width = it->second.desc.width;
    if (height) *height = it->second.desc.height;
    return it->second.pixels.data();
}

BackendProbeResult CpuBackend::probe() {
    BackendProbeResult result;
    result.available = true;
    result.device_name = "CPU reference backend";
    result.is_hardware = false;  // the CPU path is never a GPU
    result.reason = "CPU reference backend (always available)";
    return result;
}

Result<std::unique_ptr<Backend>> CpuBackend::create(const BackendCreateInfo& info) {
    return std::unique_ptr<Backend>(new CpuBackend(info));
}

}  // namespace lx::backends
