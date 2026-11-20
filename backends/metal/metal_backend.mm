// LoucuraXXXX GPU - Metal backend implementation (Objective-C++).
//
// Compiled only on Apple platforms. See metal_backend.hpp for the design and
// the honest status of this code.
#include "backends/metal/metal_backend.hpp"

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>

#include "core/log.hpp"

namespace lx::backends {

namespace {

// Maps the API-agnostic pixel format onto the Metal one. Every format the
// interface defines has a direct Metal equivalent, so a texture's storage is
// laid out exactly as the host expects.
MTLPixelFormat to_metal_format(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
        case PixelFormat::RGBA8Srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
        case PixelFormat::R32Float: return MTLPixelFormatR32Float;
    }
    return MTLPixelFormatRGBA8Unorm;
}

}  // namespace

// ---------------------------------------------------------------------------
// Device / queue state, hidden from the C++ header.
// ---------------------------------------------------------------------------
struct MetalBackend::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
};

// ---------------------------------------------------------------------------
// Command encoder: records compute dispatches into one MTLCommandBuffer.
// ---------------------------------------------------------------------------
class MetalBackend::Encoder : public CommandEncoder {
public:
    explicit Encoder(MetalBackend& backend) : backend_(backend) {}

    ~Encoder() override {
        // Dropping an unsubmitted encoder releases its command buffer, which
        // Metal discards without executing. That matches submit semantics.
    }

    Status bind_pipeline(const std::string& pipeline_name) override {
        auto it = backend_.pipelines_.find(pipeline_name);
        if (it == backend_.pipelines_.end()) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "unknown pipeline '" + pipeline_name + "'");
        }
        pipeline_ = (__bridge id<MTLComputePipelineState>)it->second.pipeline_state;
        if (command_buffer_ == nil) {
            command_buffer_ = [backend_.impl_->queue commandBuffer];
        }
        encoder_ = [command_buffer_ computeCommandEncoder];
        [encoder_ setComputePipelineState:pipeline_];
        return Status::success();
    }

    Status bind_texture(u32 slot, ResourceHandle texture) override {
        if (!ensure_encoder()) return not_encoding();
        auto it = backend_.textures_.find(texture.id);
        if (it == backend_.textures_.end()) {
            return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
        }
        // The texture is a view over the same buffer the host maps, so binding
        // it costs nothing and shares memory with the CPU.
        id<MTLTexture> tex = (__bridge id<MTLTexture>)it->second.texture;
        if (tex != nil) [encoder_ setTexture:tex atIndex:slot];
        return Status::success();
    }

    Status bind_storage_texture(u32 slot, ResourceHandle texture) override {
        return bind_texture(slot, texture);
    }

    Status set_push_constants(const void* data, u32 bytes) override {
        if (!ensure_encoder()) return not_encoding();
        [encoder_ setBytes:data length:bytes atIndex:0];
        return Status::success();
    }

    Status dispatch(const DispatchDesc& desc) override {
        if (pipeline_ == nil) {
            return Status::error(ErrorCode::InvalidArgument, "no pipeline bound");
        }
        if (!ensure_encoder()) return not_encoding();

        const NSUInteger max_threads = [pipeline_ maxTotalThreadsPerThreadgroup];
        const NSUInteger threads = std::max<NSUInteger>(1, std::min<NSUInteger>(max_threads, 64));
        const MTLSize threadgroup = MTLSizeMake(threads, 1, 1);
        const MTLSize grid = MTLSizeMake(desc.groups_x * threads, desc.groups_y, desc.groups_z);
        [encoder_ dispatchThreads:grid threadsPerThreadgroup:threadgroup];
        return Status::success();
    }

    // Called by the backend on submit(): finish the encoder and commit.
    Status commit_and_wait() {
        if (encoder_ != nil) {
            [encoder_ endEncoding];
            encoder_ = nil;
        }
        if (command_buffer_ == nil) return Status::success();  // nothing recorded
        [command_buffer_ commit];
        [command_buffer_ waitUntilCompleted];
        command_buffer_ = nil;
        return Status::success();
    }

private:
    bool ensure_encoder() {
        if (encoder_ != nil) return true;
        if (command_buffer_ == nil) command_buffer_ = [backend_.impl_->queue commandBuffer];
        encoder_ = [command_buffer_ computeCommandEncoder];
        return encoder_ != nil;
    }
    Status not_encoding() const {
        return Status::error(ErrorCode::ResourceBusy, "could not create a command encoder");
    }

    MetalBackend& backend_;
    id<MTLComputePipelineState> pipeline_ = nil;
    id<MTLCommandBuffer> command_buffer_ = nil;
    id<MTLComputeCommandEncoder> encoder_ = nil;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
MetalBackend::MetalBackend(const BackendCreateInfo& info) {
    impl_ = std::make_unique<Impl>();

    id<MTLDevice> device = nil;
    if (!info.preferred_device.empty()) {
        NSString* wanted = [NSString stringWithUTF8String:info.preferred_device.c_str()];
        for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
            if ([candidate.name isEqualToString:wanted]) {
                device = candidate;
                break;
            }
        }
    }
    if (device == nil) {
        // On an Intel Mac with a discrete GPU, MTLCreateSystemDefaultDevice()
        // returns the integrated device, which is the slower one for graphics
        // work. Prefer an explicitly discrete device when one exists and fall
        // back to the system default otherwise, so both kinds of Mac work.
        for (id<MTLDevice> candidate in MTLCopyAllDevices()) {
            if (![candidate isLowPower]) {
                device = candidate;
                break;
            }
        }
    }
    if (device == nil) device = MTLCreateSystemDefaultDevice();
    impl_->device = device;
    if (device != nil) impl_->queue = [device newCommandQueue];

    info_.kind = BackendKind::Metal;
    info_.driver_name = "Metal";
    info_.maturity = Maturity::Experimental;
    info_.api_version = "Metal";
    // hasUnifiedMemory is true on Apple silicon and false on an Intel Mac with a
    // discrete GPU, where the device has its own memory. Querying it is what
    // keeps the zero-copy path honest across both kinds of Mac.
    const bool unified = device != nil && [device hasUnifiedMemory];
    info_.capabilities.is_hardware = device != nil;
    info_.capabilities.supports_compute = device != nil;
    info_.capabilities.supports_storage_textures = device != nil;
    info_.capabilities.supports_float_textures = device != nil;
    info_.capabilities.supports_parallel_dispatch = device != nil;
    info_.capabilities.supports_timestamps = device != nil;
    info_.capabilities.unified_memory = unified;
    // Shared storage is always host-visible on macOS, so mapping is available
    // even when the device has its own memory (the driver handles coherence).
    info_.capabilities.supports_host_mapping = device != nil;
    info_.capabilities.max_texture_dimension =
        device != nil ? static_cast<u32>([device maxTextureDimension2D]) : 0;
    info_.capabilities.max_workgroup_size =
        device != nil ? static_cast<u32>([device maxThreadsPerThreadgroup].width) : 0;
    info_.capabilities.max_memory_bytes =
        device != nil ? static_cast<u64>([device recommendedMaxWorkingSetSize]) : 0;

    if (device != nil) {
        info_.device_name = [device.name UTF8String];
    } else {
        info_.device_name = "Metal (no device)";
        info_.capabilities.is_hardware = false;
    }
}

MetalBackend::~MetalBackend() {
    for (auto& entry : textures_) {
        if (entry.second.buffer != nullptr) {
            CFRelease(entry.second.buffer);
        }
    }
    for (auto& entry : pipelines_) {
        if (entry.second.pipeline_state != nullptr) {
            CFRelease(entry.second.pipeline_state);
        }
    }
    for (auto& entry : shader_functions_) {
        if (entry.second != nullptr) CFRelease(entry.second);
    }
    textures_.clear();
    pipelines_.clear();
    shader_functions_.clear();
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
Result<ResourceHandle> MetalBackend::create_texture(const TextureDesc& desc) {
    if (impl_->device == nil) {
        return Status::error(ErrorCode::BackendUnavailable, "no Metal device");
    }
    if (desc.width == 0 || desc.height == 0) {
        return Status::error(ErrorCode::InvalidArgument, "texture dimensions must be non-zero");
    }
    if (desc.width > info_.capabilities.max_texture_dimension ||
        desc.height > info_.capabilities.max_texture_dimension) {
        return Status::error(ErrorCode::OutOfRange, "texture exceeds maximum dimension");
    }
    const u64 bytes = static_cast<u64>(desc.width) * desc.height * bytes_per_pixel(desc.format);
    const u64 row_bytes = static_cast<u64>(desc.width) * bytes_per_pixel(desc.format);

    // Storage is shared, so on Apple silicon this buffer is the same memory the
    // CPU sees and no copy is needed to hand it to a shader or back to the host.
    id<MTLBuffer> buffer = [impl_->device newBufferWithLength:bytes
                                                      options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        return Status::error(ErrorCode::OutOfMemory, "Metal buffer allocation failed");
    }

    // A texture view over the buffer lets compute shaders read/write it while
    // keeping the host mapping intact. The pixel format must match the storage
    // layout the host writes, or the shader would read the wrong bytes.
    MTLTextureDescriptor* tex_desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:to_metal_format(desc.format)
                                     width:desc.width
                                    height:desc.height
                                 mipmapped:NO];
    tex_desc.storageMode = MTLStorageModeShared;
    tex_desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:tex_desc
                                                       offset:0
                                                  bytesPerRow:row_bytes];
    if (texture == nil) {
        // Under ARC `buffer` is released automatically when it goes out of
        // scope; releasing it here as well would over-release and crash.
        return Status::error(ErrorCode::InitializationFailed, "Metal texture view creation failed");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    TextureRecord record;
    record.desc = desc;
    record.bytes = bytes;
    record.buffer = (void*)CFBridgingRetain(buffer);
    record.texture = (void*)CFBridgingRetain(texture);
    const u64 id = next_id_++;
    allocated_bytes_ += bytes;
    textures_.emplace(id, std::move(record));
    return ResourceHandle{id};
}

Status MetalBackend::destroy_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    if (it->second.texture != nullptr) CFRelease(it->second.texture);
    if (it->second.buffer != nullptr) CFRelease(it->second.buffer);
    allocated_bytes_ -= it->second.bytes;
    textures_.erase(it);
    return Status::success();
}

Status MetalBackend::upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                                    u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    TextureRecord& record = it->second;
    const u64 tight_row =
        static_cast<u64>(record.desc.width) * bytes_per_pixel(record.desc.format);
    const u64 src_pitch = row_pitch == 0 ? tight_row : row_pitch;
    if (src_pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    if (bytes < tight_row * record.desc.height) {
        return Status::error(ErrorCode::InvalidArgument, "source smaller than the texture");
    }
    auto* buffer = (__bridge id<MTLBuffer>)record.buffer;
    auto* dst = static_cast<u8*>([buffer contents]);
    const auto* src = static_cast<const u8*>(data);
    for (u32 y = 0; y < record.desc.height; ++y) {
        std::memcpy(dst + static_cast<u64>(y) * tight_row,
                    src + static_cast<u64>(y) * src_pitch, tight_row);
    }
    return Status::success();
}

Result<u64> MetalBackend::download_texture(ResourceHandle handle, void* out, u64 out_bytes,
                                           u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    TextureRecord& record = it->second;
    const u64 tight_row =
        static_cast<u64>(record.desc.width) * bytes_per_pixel(record.desc.format);
    const u64 dst_pitch = row_pitch == 0 ? tight_row : row_pitch;
    const u64 needed = tight_row * record.desc.height;
    if (out_bytes < needed) {
        return Status::error(ErrorCode::InvalidArgument, "destination smaller than the texture");
    }
    if (dst_pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    auto* buffer = (__bridge id<MTLBuffer>)record.buffer;
    const auto* src = static_cast<const u8*>([buffer contents]);
    auto* dst = static_cast<u8*>(out);
    for (u32 y = 0; y < record.desc.height; ++y) {
        std::memcpy(dst + static_cast<u64>(y) * dst_pitch,
                    src + static_cast<u64>(y) * tight_row, tight_row);
    }
    return needed;
}

Result<MappedTexture> MetalBackend::map_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    TextureRecord& record = it->second;
    auto* buffer = (__bridge id<MTLBuffer>)record.buffer;
    MappedTexture mapped;
    mapped.data = [buffer contents];
    mapped.width = record.desc.width;
    mapped.height = record.desc.height;
    mapped.row_pitch =
        static_cast<u64>(record.desc.width) * bytes_per_pixel(record.desc.format);
    return mapped;
}

Status MetalBackend::unmap_texture(ResourceHandle handle) {
    // Shared storage is coherent with the CPU; nothing to flush. We only check
    // the handle so a caller mapping an unknown texture gets an error.
    std::lock_guard<std::mutex> lock(mutex_);
    if (textures_.find(handle.id) == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// Shaders / pipelines
// ---------------------------------------------------------------------------
Status MetalBackend::register_shader(const std::string& name,
                                     const std::vector<u32>& bytecode) {
    (void)name;
    (void)bytecode;
    // Metal consumes Metal Shading Language source, not SPIR-V. Pretending to
    // accept bytecode here would be dishonest, so it is refused.
    return Status::error(ErrorCode::NotSupported,
                         "Metal consumes MSL source; use register_shader_source");
}

Status MetalBackend::register_shader_source(const std::string& name,
                                            const std::string& source) {
    if (impl_->device == nil) {
        return Status::error(ErrorCode::BackendUnavailable, "no Metal device");
    }
    NSError* error = nil;
    NSString* src = [NSString stringWithUTF8String:source.c_str()];
    id<MTLLibrary> library = [impl_->device newLibraryWithSource:src options:nil error:&error];
    if (library == nil) {
        const char* what = error != nil ? [[error localizedDescription] UTF8String]
                                        : "unknown compile error";
        return Status::error(ErrorCode::NotImplemented,
                             "MSL compilation failed: " + std::string(what));
    }
    // Compile every function the library exposes under "<name>.<function>".
    for (NSString* fn_name in [library functionNames]) {
        id<MTLFunction> fn = [library newFunctionWithName:fn_name];
        if (fn == nil) continue;
        const std::string key = name + "." + [fn_name UTF8String];
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = shader_functions_.find(key);
        if (existing != shader_functions_.end() && existing->second != nullptr) {
            CFRelease(existing->second);
        }
        shader_functions_[key] = (void*)CFBridgingRetain(fn);
    }
    return Status::success();
}

Status MetalBackend::register_cpu_kernel(const std::string& name, CpuKernel kernel) {
    (void)name;
    (void)kernel;
    // A CPU kernel has no meaning on a GPU backend.
    return Status::error(ErrorCode::NotSupported, "Metal backend does not run host kernels");
}

Status MetalBackend::create_pipeline(const std::string& name,
                                     const std::string& shader_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = shader_functions_.find(shader_name);
    if (it == shader_functions_.end()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "no shader function registered as '" + shader_name + "'");
    }
    id<MTLFunction> fn = (__bridge id<MTLFunction>)it->second;
    NSError* error = nil;
    id<MTLComputePipelineState> state = [impl_->device newComputePipelineStateWithFunction:fn
                                                                                     error:&error];
    if (state == nil) {
        const char* what = error != nil ? [[error localizedDescription] UTF8String]
                                        : "unknown pipeline error";
        return Status::error(ErrorCode::InitializationFailed,
                             "pipeline creation failed: " + std::string(what));
    }
    PipelineRecord record;
    record.shader_name = shader_name;
    record.pipeline_state = (void*)CFBridgingRetain(state);
    pipelines_[name] = std::move(record);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
Result<std::unique_ptr<CommandEncoder>> MetalBackend::begin_commands() {
    return std::unique_ptr<CommandEncoder>(new Encoder(*this));
}

Status MetalBackend::submit(CommandEncoder* encoder) {
    if (encoder == nullptr) {
        // Submitting nothing is a valid no-op: the renderer uses it as a fence.
        return wait_idle();
    }
    auto* metal_encoder = dynamic_cast<Encoder*>(encoder);
    if (metal_encoder == nullptr) {
        return Status::error(ErrorCode::InvalidArgument, "encoder is not a Metal encoder");
    }
    return metal_encoder->commit_and_wait();
}

Status MetalBackend::wait_idle() {
    if (impl_->queue == nil) return Status::success();
    id<MTLCommandBuffer> fence = [impl_->queue commandBuffer];
    if (fence != nil) {
        [fence commit];
        [fence waitUntilCompleted];
    }
    return Status::success();
}

Status MetalBackend::sync() { return wait_idle(); }

u64 MetalBackend::allocated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocated_bytes_;
}

// ---------------------------------------------------------------------------
// Probe / factory
// ---------------------------------------------------------------------------
BackendProbeResult MetalBackend::probe() {
    BackendProbeResult result;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        result.available = false;
        result.reason = "no Metal device available";
        return result;
    }
    result.available = true;
    result.device_name = [device.name UTF8String];
    // Metal has no software rasterizer, so a device means real hardware. The
    // flag is still set explicitly so the CLI never has to guess.
    result.is_hardware = true;
    result.reason = [device hasUnifiedMemory] ? "Metal device (unified memory)"
                                              : "Metal device (discrete memory)";
    return result;
}

Result<std::unique_ptr<Backend>> MetalBackend::create(const BackendCreateInfo& info) {
    auto backend = std::unique_ptr<MetalBackend>(new MetalBackend(info));
    if (!backend->capabilities().is_hardware) {
        return Status::error(ErrorCode::BackendUnavailable, "no Metal device available");
    }
    return std::unique_ptr<Backend>(std::move(backend));
}

}  // namespace lx::backends

#endif  // __APPLE__
