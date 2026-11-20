// LoucuraXXXX GPU - Direct3D 11 backend implementation.
//
// See d3d11_backend.hpp for the contract. Implementation notes:
//  * The device is created with D3D11CreateDevice, hardware first and WARP as
//    the software fallback, so the backend works even on a machine with no GPU
//    driver (and under Wine, which exposes D3D11 through its own stack).
//  * Upload and download go through a staging texture, honouring both the
//    caller's row pitch and the driver's, which is the standard D3D11 idiom.
//  * A pass is a full-screen triangle (SV_VertexID) drawn with a pixel shader
//    into the target's render-target view.
//  * The constant buffer is the emulated push-constant block (register b0).
#include "backends/d3d11/d3d11_backend.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <limits>
#include <string>

#include "core/log.hpp"

#if defined(_WIN32)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>
#endif

namespace lx::backends {

#if defined(_WIN32)

namespace {

// Full-screen triangle from the vertex id, no vertex buffer. Same shape as the
// GL/GLSL vertex stage, so every backend draws the identical geometry.
const char* kFullscreenVertex = R"(
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 p = float2(float((id << 1) & 2), float(id & 2));
    VSOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
)";

// Common preamble: the input texture, the emulated push-constant block and the
// luma weights. The pass bodies mirror the CPU filters exactly, the same maths
// the GL backend uses, so the three executors agree.
const char* kPixelHeader = R"(
Texture2D u_image : register(t0);
cbuffer Push : register(b0) {
)";

const char* kPixelFooter = R"(};
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
)";

const char* kToneBody = R"(
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);
    float4 src = u_image.Load(int3(coord, 0));
    float3 rgb = src.rgb * 255.0;
    rgb *= pow(2.0, exposure_stops);
    rgb = (rgb - 128.0) * (1.0 + contrast) + 128.0;
    float luma = dot(clamp(rgb, 0.0, 255.0), kLuma);
    rgb = float3(luma, luma, luma) + (rgb - float3(luma, luma, luma)) * saturation;
    float3 blended = lerp(src.rgb * 255.0, rgb, intensity);
    return float4(clamp(blended, 0.0, 255.0) / 255.0, src.a);
}
)";

const char* kVignetteBody = R"(
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);
    float4 src = u_image.Load(int3(coord, 0));
    float2 p = pos.xy - 0.5;
    float2 center = image_size * 0.5;
    float max_radius = length(center);
    float radius = length(p - center) / max_radius;
    float t = clamp((radius - inner) / max(outer - inner, 1e-3), 0.0, 1.0);
    float falloff = t * t * (3.0 - 2.0 * t);
    float factor = 1.0 - falloff * intensity;
    return float4(src.rgb * factor, src.a);
}
)";

const char* kSharpenBody = R"(
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);
    int2 hi = int2(image_size) - 1;
    float3 avg = float3(0.0, 0.0, 0.0);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int2 s = clamp(coord + int2(dx, dy), int2(0, 0), hi);
            avg += u_image.Load(int3(s, 0)).rgb;
        }
    avg /= 9.0;
    float4 src = u_image.Load(int3(coord, 0));
    float3 sharpened = src.rgb + strength * (src.rgb - avg);
    return float4(clamp(sharpened, 0.0, 1.0), src.a);
}
)";

const char* kBlurBody = R"(
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);
    int2 hi = int2(image_size) - 1;
    float denom = 2.0 * sigma * sigma;
    float3 accum = float3(0.0, 0.0, 0.0);
    float weight_sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        int2 s = coord;
        if (axis == 0) s.x = clamp(coord.x + k, 0, hi.x);
        else           s.y = clamp(coord.y + k, 0, hi.y);
        float w = exp(-float(k * k) / denom);
        accum += u_image.Load(int3(s, 0)).rgb * w;
        weight_sum += w;
    }
    float3 blurred = accum / weight_sum;
    float4 src = u_image.Load(int3(coord, 0));
    return float4(lerp(src.rgb, blurred, intensity), src.a);
}
)";

// copy: straight passthrough, used to blit a filter chain's result into the
// present target when the chain ran on scratch textures.
const char* kCopyBody = R"(
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    return u_image.Load(int3(int2(pos.xy), 0));
}
)";

const char* kBloomBrightBody = R"(
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);
    float4 src = u_image.Load(int3(coord, 0));
    float luma = dot(src.rgb * 255.0, kLuma);
    return (luma > threshold) ? src : float4(0.0, 0.0, 0.0, src.a);
}
)";

const char* kBloomBlurBody = R"(
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);
    int2 hi = int2(image_size) - 1;
    float3 accum = float3(0.0, 0.0, 0.0);
    float count = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        int2 s = coord;
        if (axis == 0) s.x = clamp(coord.x + k, 0, hi.x);
        else           s.y = clamp(coord.y + k, 0, hi.y);
        accum += u_image.Load(int3(s, 0)).rgb;
        count += 1.0;
    }
    float4 src = u_image.Load(int3(coord, 0));
    return float4(accum / count, src.a);
}
)";

const char* kBloomCombineBody = R"(
Texture2D u_glow : register(t1);
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 coord = int2(pos.xy);
    float4 base = u_image.Load(int3(coord, 0));
    float4 glow = u_glow.Load(int3(coord, 0));
    return float4(clamp(base.rgb + glow.rgb * intensity, 0.0, 1.0), base.a);
}
)";

// Constant-block field declarations, matching the GL std140 layouts field for
// field so one C++ push struct fills both backends.
const char* kToneUniforms = R"(
    float exposure_stops; float contrast; float saturation; float intensity;
)";
const char* kVignetteUniforms = R"(
    float inner; float outer; float intensity; float _pad0;
    float2 image_size; float2 _pad1;
)";
const char* kSharpenUniforms = R"(
    float strength; float _pad0; float2 image_size; float2 _pad1;
)";
const char* kBlurUniforms = R"(
    float sigma; float intensity; float2 image_size;
    int axis; int radius; int _pad0; int _pad1;
)";
const char* kBloomBrightUniforms = R"(
    float threshold; float _pad0; float2 _pad1;
)";
const char* kBloomBlurUniforms = R"(
    int radius; int axis; float2 image_size;
)";
const char* kBloomCombineUniforms = R"(
    float intensity; float _pad0; float2 _pad1;
)";
const char* kCopyUniforms = R"(
    float _pad0; float _pad1; float2 _pad2;
)";

std::string build_pixel_source(const char* uniforms, const char* body) {
    std::string source = kPixelHeader;
    source += uniforms;
    source += kPixelFooter;
    source += body;
    return source;
}

}  // namespace

const char* D3D11Backend::builtin_pixel_shader_source(const std::string& shader_name) {
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(shader_name);
    if (it != cache.end()) return it->second.c_str();

    std::string source;
    if (shader_name == "tone") {
        source = build_pixel_source(kToneUniforms, kToneBody);
    } else if (shader_name == "vignette") {
        source = build_pixel_source(kVignetteUniforms, kVignetteBody);
    } else if (shader_name == "sharpen") {
        source = build_pixel_source(kSharpenUniforms, kSharpenBody);
    } else if (shader_name == "blur_horizontal" || shader_name == "blur_vertical") {
        source = build_pixel_source(kBlurUniforms, kBlurBody);
    } else if (shader_name == "bloom_bright") {
        source = build_pixel_source(kBloomBrightUniforms, kBloomBrightBody);
    } else if (shader_name == "bloom_blur_horizontal" ||
               shader_name == "bloom_blur_vertical") {
        source = build_pixel_source(kBloomBlurUniforms, kBloomBlurBody);
    } else if (shader_name == "bloom_combine") {
        source = build_pixel_source(kBloomCombineUniforms, kBloomCombineBody);
    } else if (shader_name == "copy") {
        source = build_pixel_source(kCopyUniforms, kCopyBody);
    } else {
        return nullptr;
    }
    auto inserted = cache.emplace(shader_name, std::move(source));
    return inserted.first->second.c_str();
}

// Records one full-screen pass and executes it at submit() time, matching the
// interface's record-then-submit contract.
class D3D11Backend::Encoder : public CommandEncoder {
public:
    explicit Encoder(D3D11Backend& backend) : backend_(backend) {}

    Status bind_pipeline(const std::string& pipeline_name) override {
        auto it = backend_.pipelines_.find(pipeline_name);
        if (it == backend_.pipelines_.end()) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "unknown pipeline '" + pipeline_name + "'");
        }
        pipeline_ = it->second;
        have_pipeline_ = true;
        return Status::success();
    }
    Status bind_texture(u32 slot, ResourceHandle texture) override {
        if (slot >= kMaxInputs) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "Direct3D 11 backend binds at most " +
                                     std::to_string(kMaxInputs) + " inputs");
        }
        inputs_[slot] = texture;
        input_count_ = std::max(input_count_, slot + 1);
        return Status::success();
    }
    Status bind_storage_texture(u32 slot, ResourceHandle texture) override {
        if (slot != 0) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "Direct3D 11 backend binds the output at slot 0");
        }
        output_ = texture;
        return Status::success();
    }
    Status set_push_constants(const void* data, u32 bytes) override {
        push_.assign(static_cast<const u8*>(data),
                     static_cast<const u8*>(data) + bytes);
        return Status::success();
    }
    Status dispatch(const DispatchDesc&) override { return Status::success(); }

    Status execute() {
        if (!have_pipeline_ || input_count_ == 0 || !output_.valid()) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "pass needs a pipeline, an input and an output");
        }
        auto* dst = backend_.find_texture(output_);
        if (!dst) {
            return Status::error(ErrorCode::InvalidArgument, "pass references a dead texture");
        }
        for (u32 i = 0; i < input_count_; ++i) {
            auto* src = backend_.find_texture(inputs_[i]);
            if (!src) {
                return Status::error(ErrorCode::InvalidArgument,
                                     "pass references a dead input texture");
            }
            if (src->desc.width != dst->desc.width ||
                src->desc.height != dst->desc.height) {
                return Status::error(ErrorCode::InvalidArgument,
                                     "input and output textures differ in size");
            }
        }
        if (pipeline_.vs == nullptr || pipeline_.ps == nullptr) {
            return Status::error(ErrorCode::NotImplemented,
                                 "pipeline has no shaders; register its source first");
        }

        ID3D11DeviceContext* ctx = backend_.context_;
        ID3D11ShaderResourceView* nullsrv[kMaxInputs] = {nullptr, nullptr, nullptr, nullptr};
        ctx->PSSetShaderResources(0, kMaxInputs, nullsrv);

        if (!push_.empty()) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(backend_.constant_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                   &mapped))) {
                std::memcpy(mapped.pData, push_.data(), push_.size());
                ctx->Unmap(backend_.constant_buffer_, 0);
            }
        }
        ID3D11Buffer* cbufs[1] = {backend_.constant_buffer_};
        ctx->VSSetConstantBuffers(0, 1, cbufs);
        ctx->PSSetConstantBuffers(0, 1, cbufs);

        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->RSSetState(backend_.rasterizer_state_);
        ctx->VSSetShader(pipeline_.vs, nullptr, 0);
        ctx->PSSetShader(pipeline_.ps, nullptr, 0);

        // Inputs go to consecutive shader-resource slots: u_image at t0, the
        // bloom glow at t1.
        ID3D11ShaderResourceView* srvs[kMaxInputs] = {nullptr, nullptr, nullptr, nullptr};
        for (u32 i = 0; i < input_count_; ++i) {
            srvs[i] = backend_.find_texture(inputs_[i])->srv;
        }
        ctx->PSSetShaderResources(0, input_count_, srvs);

        ID3D11RenderTargetView* rtvs[1] = {dst->rtv};
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(dst->desc.width);
        vp.Height = static_cast<float>(dst->desc.height);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);

        // Unbind so the texture is not left bound as both input and output.
        ctx->PSSetShaderResources(0, kMaxInputs, nullsrv);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return Status::success();
    }

private:
    static constexpr u32 kMaxInputs = 4;
    D3D11Backend& backend_;
    PipelineRecord pipeline_;
    bool have_pipeline_ = false;
    std::array<ResourceHandle, kMaxInputs> inputs_{};
    u32 input_count_ = 0;
    ResourceHandle output_{};
    std::vector<u8> push_;
};

D3D11Backend::D3D11Backend(const BackendCreateInfo& info) {
    info_.kind = BackendKind::Direct3D11;
    info_.driver_name = "loucuraxxxx-d3d11";
    info_.api_version = "11.0";
    info_.maturity = Maturity::Planned;
    info_.device_name = "Direct3D 11 unavailable";

    UINT flags = 0;
    if (info.allow_validation) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, 4, D3D11_SDK_VERSION, &device_, &got, &context_);
    bool hardware_device = SUCCEEDED(hr) && device_ != nullptr;
    if (!hardware_device) {
        // No hardware device: fall back to the software rasterizer so the
        // backend is still real (WARP is a full D3D11 implementation). This is
        // recorded so capabilities() does not report it as acceleration.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 4,
                               D3D11_SDK_VERSION, &device_, &got, &context_);
    }
    if (FAILED(hr) || device_ == nullptr) {
        LX_WARN << "Direct3D 11 device creation failed (hr=0x" << std::hex << hr << ")";
        return;
    }

    info_.maturity = Maturity::Implemented;
    info_.device_name = "Direct3D 11 device";
    // The adapter description is the only way to tell a real GPU from WARP or
    // the Basic Render Driver, so it is read rather than assumed.
    {
        IDXGIDevice* dxgi_device = nullptr;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice),
                                              reinterpret_cast<void**>(&dxgi_device))) &&
            dxgi_device != nullptr) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr) {
                // DXGI_ADAPTER_DESC1 carries the Flags field that marks a
                // software adapter; the base desc does not.
                DXGI_ADAPTER_DESC1 adapter_desc{};
                IDXGIAdapter1* adapter1 = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(
                        __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter1))) &&
                    adapter1 != nullptr) {
                    if (SUCCEEDED(adapter1->GetDesc1(&adapter_desc))) {
                        char narrow[256] = {};
                        const int written = WideCharToMultiByte(
                            CP_UTF8, 0, adapter_desc.Description, -1, narrow,
                            static_cast<int>(sizeof(narrow)), nullptr, nullptr);
                        if (written > 0) info_.device_name = narrow;
                        if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                            hardware_device = false;
                        }
                        // Wine reports a card from its built-in database and runs the
                        // calls on the host's software rasterizer, without setting the
                        // SOFTWARE flag, so the environment is the only honest signal.
                        if (running_under_wine()) hardware_device = false;
                    }
                    adapter1->Release();
                }
                adapter->Release();
            }
            dxgi_device->Release();
        }
    }
    switch (got) {
        case D3D_FEATURE_LEVEL_11_1: info_.api_version = "11.1"; break;
        case D3D_FEATURE_LEVEL_11_0: info_.api_version = "11.0"; break;
        case D3D_FEATURE_LEVEL_10_1: info_.api_version = "10.1"; break;
        default: info_.api_version = "10.0"; break;
    }

    info_.capabilities.is_hardware =
        hardware_device && !looks_like_software_rasterizer(info_.device_name);
    info_.capabilities.supports_compute = false;  // built-in passes are pixel shaders
    info_.capabilities.supports_storage_textures = true;
    info_.capabilities.supports_float_textures = true;
    info_.capabilities.supports_parallel_dispatch = false;
    info_.capabilities.supports_timestamps = false;
    info_.capabilities.max_texture_dimension = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    info_.capabilities.max_workgroup_size = 0;
    info_.capabilities.max_memory_bytes = info.memory_budget_bytes;
    info_.capabilities.unified_memory = false;
    info_.capabilities.supports_host_mapping = false;

    // The emulated push-constant block. 256 bytes covers every built-in pass and
    // is a multiple of 16 as D3D11 requires.
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 256;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cb, nullptr, &constant_buffer_))) {
        LX_WARN << "Direct3D 11 constant buffer creation failed";
    }

    // The full-screen triangle has a fixed winding; D3D11 culls back faces by
    // default, which would drop it depending on the coordinate convention. A
    // pass is a full-surface fill, so culling is disabled outright.
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rs, &rasterizer_state_))) {
        LX_WARN << "Direct3D 11 rasterizer state creation failed";
    }

    LX_INFO << "Direct3D 11 backend ready: " << info_.device_name << " (" << info_.api_version
            << ")";
}

D3D11Backend::~D3D11Backend() {
    if (device_ == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, record] : textures_) {
        if (record.srv) record.srv->Release();
        if (record.rtv) record.rtv->Release();
        if (record.texture) record.texture->Release();
    }
    textures_.clear();
    for (auto& [name, pipeline] : pipelines_) {
        if (pipeline.vs) pipeline.vs->Release();
        if (pipeline.ps) pipeline.ps->Release();
    }
    pipelines_.clear();
    if (idle_event_) idle_event_->Release();
    if (rasterizer_state_) rasterizer_state_->Release();
    if (constant_buffer_) constant_buffer_->Release();
    if (context_) context_->Release();
    if (device_) device_->Release();
}

D3D11Backend::TextureRecord* D3D11Backend::find_texture(ResourceHandle handle) {
    auto it = textures_.find(handle.id);
    return it == textures_.end() ? nullptr : &it->second;
}

Result<ResourceHandle> D3D11Backend::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return Status::error(ErrorCode::InvalidArgument,
                             "texture dimensions must be non-zero");
    }
    if (desc.width > info_.capabilities.max_texture_dimension ||
        desc.height > info_.capabilities.max_texture_dimension) {
        return Status::error(ErrorCode::OutOfRange, "texture exceeds maximum dimension");
    }
    const u64 bytes = texture_byte_size(desc.width, desc.height, desc.format);
    if (info_.capabilities.max_memory_bytes != 0 &&
        bytes > info_.capabilities.max_memory_bytes - allocated_bytes_) {
        return Status::error(ErrorCode::OutOfMemory, "texture exceeds the memory budget");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    TextureRecord record;
    record.desc = desc;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc.width;
    td.Height = desc.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &record.texture))) {
        return Status::error(ErrorCode::OutOfMemory, "CreateTexture2D failed");
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(record.texture, &sd, &record.srv))) {
        record.texture->Release();
        return Status::error(ErrorCode::InitializationFailed, "CreateShaderResourceView failed");
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipSlice = 0;
    if (FAILED(device_->CreateRenderTargetView(record.texture, &rd, &record.rtv))) {
        record.srv->Release();
        record.texture->Release();
        return Status::error(ErrorCode::InitializationFailed, "CreateRenderTargetView failed");
    }

    const u64 id = next_id_++;
    allocated_bytes_ += bytes;
    textures_.emplace(id, record);
    return ResourceHandle{id};
}

Status D3D11Backend::destroy_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    const u64 bytes = texture_byte_size(it->second.desc.width, it->second.desc.height,
                                        it->second.desc.format);
    if (it->second.srv) it->second.srv->Release();
    if (it->second.rtv) it->second.rtv->Release();
    if (it->second.texture) it->second.texture->Release();
    allocated_bytes_ -= bytes;
    textures_.erase(it);
    return Status::success();
}

Status D3D11Backend::upload_texture(ResourceHandle handle, const void* data, u64 bytes,
                                    u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* record = find_texture(handle);
    if (!record) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    const u64 tight_row = static_cast<u64>(record->desc.width) * 4;
    const u64 pitch = row_pitch == 0 ? tight_row : row_pitch;
    if (pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    if (record->desc.height > 1 &&
        pitch > (std::numeric_limits<u64>::max() - tight_row) / (record->desc.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch is absurd");
    }
    const u64 required = pitch * (record->desc.height - 1) + tight_row;
    if (bytes < required) {
        return Status::error(ErrorCode::InvalidArgument, "source buffer too small");
    }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = record->desc.width;
    sd.Height = record->desc.height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) {
        return Status::error(ErrorCode::OutOfMemory, "staging texture creation failed");
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped))) {
        staging->Release();
        return Status::error(ErrorCode::InitializationFailed, "Map for upload failed");
    }
    const auto* src = static_cast<const u8*>(data);
    for (u32 y = 0; y < record->desc.height; ++y) {
        std::memcpy(static_cast<u8*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    src + static_cast<std::size_t>(y) * pitch,
                    static_cast<std::size_t>(tight_row));
    }
    context_->Unmap(staging, 0);
    context_->CopyResource(record->texture, staging);
    staging->Release();
    return Status::success();
}

Result<u64> D3D11Backend::download_texture(ResourceHandle handle, void* out, u64 out_bytes,
                                           u64 row_pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* record = find_texture(handle);
    if (!record) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    const u64 tight_row = static_cast<u64>(record->desc.width) * 4;
    const u64 pitch = row_pitch == 0 ? tight_row : row_pitch;
    if (pitch < tight_row) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch smaller than a row");
    }
    if (record->desc.height > 1 &&
        pitch > (std::numeric_limits<u64>::max() - tight_row) / (record->desc.height - 1)) {
        return Status::error(ErrorCode::InvalidArgument, "row pitch is absurd");
    }
    const u64 required = pitch * (record->desc.height - 1) + tight_row;
    if (out_bytes < required) {
        return Status::error(ErrorCode::InvalidArgument, "destination buffer too small");
    }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = record->desc.width;
    sd.Height = record->desc.height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging))) {
        return Status::error(ErrorCode::OutOfMemory, "staging texture creation failed");
    }
    context_->CopyResource(staging, record->texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return Status::error(ErrorCode::InitializationFailed, "Map for download failed");
    }
    auto* dst = static_cast<u8*>(out);
    for (u32 y = 0; y < record->desc.height; ++y) {
        std::memcpy(dst + static_cast<std::size_t>(y) * pitch,
                    static_cast<const u8*>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch,
                    static_cast<std::size_t>(tight_row));
    }
    context_->Unmap(staging, 0);
    staging->Release();
    return required;
}

Status D3D11Backend::register_shader(const std::string& name,
                                     const std::vector<u32>& bytecode) {
    (void)name;
    (void)bytecode;
    return Status::error(ErrorCode::NotSupported,
                         "Direct3D 11 backend compiles HLSL source, not SPIR-V bytecode");
}

Status D3D11Backend::register_shader_source(const std::string& name,
                                            const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    shader_sources_[name] = source;
    return Status::success();
}

Status D3D11Backend::register_cpu_kernel(const std::string& name, CpuKernel kernel) {
    std::lock_guard<std::mutex> lock(mutex_);
    kernels_[name] = kernel;
    return Status::success();
}

Status D3D11Backend::compile_pipeline(PipelineRecord& pipeline, const std::string& source) {
    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(NDEBUG)
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(kFullscreenVertex, std::strlen(kFullscreenVertex),
                            "lx_fullscreen_vs", nullptr, nullptr, "VSMain", "vs_4_0", flags, 0,
                            &vs_blob, &error_blob);
    if (FAILED(hr)) {
        std::string message = "vertex shader compile failed";
        if (error_blob) {
            message += ": ";
            message += static_cast<const char*>(error_blob->GetBufferPointer());
            error_blob->Release();
        }
        return Status::error(ErrorCode::InitializationFailed, message);
    }

    hr = D3DCompile(source.c_str(), source.size(), "lx_pass_ps", nullptr, nullptr, "PSMain",
                    "ps_4_0", flags, 0, &ps_blob, &error_blob);
    if (FAILED(hr)) {
        std::string message = "pixel shader compile failed";
        if (error_blob) {
            message += ": ";
            message += static_cast<const char*>(error_blob->GetBufferPointer());
            error_blob->Release();
        }
        vs_blob->Release();
        return Status::error(ErrorCode::InitializationFailed, message);
    }

    if (pipeline.vs) pipeline.vs->Release();
    if (pipeline.ps) pipeline.ps->Release();
    hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                     nullptr, &pipeline.vs);
    if (SUCCEEDED(hr)) {
        hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                        nullptr, &pipeline.ps);
    }
    vs_blob->Release();
    ps_blob->Release();
    if (FAILED(hr)) {
        return Status::error(ErrorCode::InitializationFailed, "shader object creation failed");
    }
    return Status::success();
}

Status D3D11Backend::create_pipeline(const std::string& name,
                                     const std::string& shader_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    PipelineRecord pipeline;
    pipeline.shader_name = shader_name;

    auto it = shader_sources_.find(shader_name);
    if (it != shader_sources_.end()) {
        LX_TRY(compile_pipeline(pipeline, it->second));
    } else if (const char* builtin = builtin_pixel_shader_source(shader_name)) {
        LX_TRY(compile_pipeline(pipeline, builtin));
    }
    pipelines_[name] = pipeline;
    return Status::success();
}

Result<std::unique_ptr<CommandEncoder>> D3D11Backend::begin_commands() {
    return std::unique_ptr<CommandEncoder>(new Encoder(*this));
}

Status D3D11Backend::submit(CommandEncoder* encoder) {
    if (encoder == nullptr) return sync();
    auto* d3d_encoder = dynamic_cast<Encoder*>(encoder);
    if (d3d_encoder == nullptr) {
        return Status::error(ErrorCode::InvalidArgument,
                             "encoder was not created by this backend");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return d3d_encoder->execute();
}

Status D3D11Backend::wait_idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_) return Status::success();
    if (idle_event_ == nullptr) {
        D3D11_QUERY_DESC qdesc{};
        qdesc.Query = D3D11_QUERY_EVENT;
        const HRESULT hr = device_->CreateQuery(&qdesc, &idle_event_);
        if (FAILED(hr) || idle_event_ == nullptr) {
            // Without an event query the honest thing is to say the wait is not
            // available rather than to return success on a Flush that does not
            // wait. Flush still submits the queued work.
            context_->Flush();
            return Status::error(ErrorCode::NotImplemented,
                                 "Direct3D 11 idle event query could not be created");
        }
    }
    context_->End(idle_event_);
    context_->Flush();
    // Spin briefly, then back off to a short sleep. D3D11 has no blocking wait
    // here, so a pure spin is the alternative, and it holds a core for the whole
    // duration of the GPU work. On this host the two strategies measured
    // identically for both fps and total CPU time (320 fps, ~1.5 s user for 400
    // frames), so this is a design choice about not monopolising a core that the
    // filter pool may want, not a measured speedup.
    BOOL done = FALSE;
    u32 spins = 0;
    while (context_->GetData(idle_event_, &done, sizeof(done), 0) != S_OK || !done) {
        done = FALSE;
        if (++spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    return Status::success();
}

Status D3D11Backend::sync() {
    return wait_idle();
}

u64 D3D11Backend::allocated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocated_bytes_;
}

BackendProbeResult D3D11Backend::probe() {
    BackendProbeResult result;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                   D3D11_SDK_VERSION, &device, &got, &context);
    bool got_hardware = SUCCEEDED(hr) && device != nullptr;
    if (!got_hardware) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                               D3D11_SDK_VERSION, &device, &got, &context);
    }
    if (FAILED(hr) || device == nullptr) {
        result.reason = "no Direct3D 11 device (hardware or WARP)";
        return result;
    }
    result.available = true;
    result.device_name = "Direct3D 11 device";
    // The adapter name is what distinguishes a GPU from WARP, so read it here
    // too instead of only inside the full device constructor.
    bool hardware = SUCCEEDED(hr) && got_hardware;
    {
        IDXGIDevice* dxgi_device = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                             reinterpret_cast<void**>(&dxgi_device))) &&
            dxgi_device != nullptr) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr) {
                DXGI_ADAPTER_DESC1 adapter_desc{};
                IDXGIAdapter1* adapter1 = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(
                        __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter1))) &&
                    adapter1 != nullptr) {
                    if (SUCCEEDED(adapter1->GetDesc1(&adapter_desc))) {
                        char narrow[256] = {};
                        if (WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1,
                                                narrow, static_cast<int>(sizeof(narrow)),
                                                nullptr, nullptr) > 0) {
                            result.device_name = narrow;
                        }
                        if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) hardware = false;
                        // See the constructor: Wine's synthetic adapter must not be
                        // reported as hardware.
                        if (running_under_wine()) hardware = false;
                    }
                    adapter1->Release();
                }
                adapter->Release();
            }
            dxgi_device->Release();
        }
    }
    result.is_hardware = hardware && !looks_like_software_rasterizer(result.device_name);
    result.reason = result.is_hardware ? "Direct3D 11 hardware device available"
                                       : "Direct3D 11 device available (software rasterizer)";
    context->Release();
    device->Release();
    return result;
}

Result<std::unique_ptr<Backend>> D3D11Backend::create(const BackendCreateInfo& info) {
    std::unique_ptr<D3D11Backend> backend(new D3D11Backend(info));
    if (backend->device_ == nullptr) {
        return Status::error(ErrorCode::BackendUnavailable, "no Direct3D 11 device");
    }
    return std::unique_ptr<Backend>(backend.release());
}

#endif  // _WIN32

}  // namespace lx::backends
