// LoucuraXXXX GPU - OpenGL 3.3 core backend implementation.
//
// Design summary (see gl_backend.hpp for the contract):
//  * A headless context is created through EGL. The surfaceless platform is
//    preferred (no display server needed); eglGetDisplay is the fallback.
//  * A "texture" is a real GL texture plus an FBO so it can be a render target.
//  * OpenGL 3.3 core has no compute shaders, so a pass is a full-screen
//    fragment shader over the target. dispatch() is therefore honest: it runs
//    the bound pipeline as a fragment pass, not a compute dispatch, and the
//    capabilities report supports_compute = false.
//  * Push constants are emulated with a std140 uniform buffer.
//
// GL is single-threaded, so every call is serialised by the backend mutex. The
// context is current on the thread that created the backend.
#include "backends/opengl/gl_backend.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "core/log.hpp"
#include "hardware/hardware_info.hpp"

namespace lx::backends {

namespace {

// Vertex stage: one oversized triangle from gl_VertexID, no vertex buffer. This
// is valid in GLSL 3.30 and is what every full-screen pass uses.
const char* kFullscreenVertex = R"(#version 330 core
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Emulated push-constant block. The layout matches the std140 rules so the C++
// side can fill it with a plain struct.
const char* kCommonHeader = R"(#version 330 core
in vec2 v_uv;
out vec4 out_color;
uniform sampler2D u_image;
layout(std140) uniform Push {
)";

const char* kCommonFooter = R"(} pc;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
)";

// tone: mirrors filters/builtin/tone_filter.cpp exactly (working in 0..255).
const char* kToneBody = R"(
void main() {
    vec4 src = texelFetch(u_image, ivec2(gl_FragCoord.xy), 0);
    vec3 rgb = src.rgb * 255.0;
    float gain = pow(2.0, pc.exposure_stops);
    rgb *= gain;
    rgb = (rgb - 128.0) * (1.0 + pc.contrast) + 128.0;
    float luma = dot(clamp(rgb, 0.0, 255.0), kLuma);
    rgb = vec3(luma) + (rgb - vec3(luma)) * pc.saturation;
    vec3 blended = mix(src.rgb * 255.0, rgb, pc.intensity);
    out_color = vec4(clamp(blended, 0.0, 255.0) / 255.0, src.a);
}
)";

// vignette: mirrors filters/builtin/vignette_filter.cpp. gl_FragCoord is at the
// pixel centre (x+0.5), while the CPU filter uses the integer pixel index, so
// the fragment coordinate is shifted back by 0.5 to match exactly.
const char* kVignetteBody = R"(
void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec4 src = texelFetch(u_image, coord, 0);
    vec2 p = gl_FragCoord.xy - vec2(0.5);
    vec2 center = pc.image_size * 0.5;
    float max_radius = length(center);
    float radius = length(p - center) / max_radius;
    float t = clamp((radius - pc.inner) / max(pc.outer - pc.inner, 1e-3), 0.0, 1.0);
    float falloff = t * t * (3.0 - 2.0 * t);
    float factor = 1.0 - falloff * pc.intensity;
    out_color = vec4(src.rgb * factor, src.a);
}
)";

// sharpen: mirrors filters/builtin/sharpen_filter.cpp (3x3 box unsharp mask).
const char* kSharpenBody = R"(
void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    ivec2 hi = ivec2(pc.image_size) - 1;
    vec3 avg = vec3(0.0);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 s = clamp(coord + ivec2(dx, dy), ivec2(0), hi);
            avg += texelFetch(u_image, s, 0).rgb;
        }
    avg /= 9.0;
    vec4 src = texelFetch(u_image, coord, 0);
    vec3 sharpened = src.rgb + pc.strength * (src.rgb - avg);
    out_color = vec4(clamp(sharpened, 0.0, 1.0), src.a);
}
)";

// blur: one axis per pass; mirrors blur_filter.cpp (Gaussian, border clamp).
const char* kBlurBody = R"(
void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    ivec2 hi = ivec2(pc.image_size) - 1;
    int radius = pc.radius;
    float denom = 2.0 * pc.sigma * pc.sigma;
    vec3 accum = vec3(0.0);
    float weight_sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        ivec2 s = coord;
        if (pc.axis == 0) s.x = clamp(coord.x + k, 0, hi.x);
        else              s.y = clamp(coord.y + k, 0, hi.y);
        float w = exp(-float(k * k) / denom);
        accum += texelFetch(u_image, s, 0).rgb * w;
        weight_sum += w;
    }
    vec3 blurred = accum / weight_sum;
    vec4 src = texelFetch(u_image, coord, 0);
    out_color = vec4(mix(src.rgb, blurred, pc.intensity), src.a);
}
)";

// copy: straight passthrough, used to blit a filter chain's result into the
// present target when the chain ran on scratch textures.
const char* kCopyBody = R"(
void main() {
    out_color = texelFetch(u_image, ivec2(gl_FragCoord.xy), 0);
}
)";

// bloom bright pass: mirrors the bright_pass() step of bloom_filter.cpp.
const char* kBloomBrightBody = R"(
void main() {
    vec4 src = texelFetch(u_image, ivec2(gl_FragCoord.xy), 0);
    float luma = dot(src.rgb * 255.0, kLuma);
    out_color = (luma > pc.threshold) ? src : vec4(0.0, 0.0, 0.0, src.a);
}
)";

// bloom box blur: mirrors box_blur() in bloom_filter.cpp (unweighted average).
const char* kBloomBlurBody = R"(
void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    ivec2 hi = ivec2(pc.image_size) - 1;
    int radius = pc.radius;
    vec3 accum = vec3(0.0);
    float count = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        ivec2 s = coord;
        if (pc.axis == 0) s.x = clamp(coord.x + k, 0, hi.x);
        else              s.y = clamp(coord.y + k, 0, hi.y);
        accum += texelFetch(u_image, s, 0).rgb;
        count += 1.0;
    }
    vec4 src = texelFetch(u_image, coord, 0);
    out_color = vec4(accum / count, src.a);
}
)";

// bloom combine: add the glow back, weighted by intensity.
const char* kBloomCombineBody = R"(
void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec4 base = texelFetch(u_image, coord, 0);
    vec4 glow = texelFetch(u_glow, coord, 0);
    out_color = vec4(clamp(base.rgb + glow.rgb * pc.intensity, 0.0, 1.0), base.a);
}
)";

// Uniform block field declarations, per shader. These are textually spliced
// between the header and the body so each pass sees exactly its own layout.
const char* kToneUniforms = R"(
    float exposure_stops;
    float contrast;
    float saturation;
    float intensity;
)";

const char* kVignetteUniforms = R"(
    float inner;
    float outer;
    float intensity;
    float _pad0;
    vec2  image_size;
    vec2  _pad1;
)";

const char* kSharpenUniforms = R"(
    float strength;
    vec2  image_size;
    vec2  _pad;
)";

const char* kBlurUniforms = R"(
    float sigma;
    float intensity;
    vec2  image_size;
    int   axis;
    int   radius;
    int   _pad0;
    int   _pad1;
)";

const char* kBloomBrightUniforms = R"(
    float threshold;
    float _pad0;
    vec2  _pad1;
)";

const char* kBloomBlurUniforms = R"(
    int   radius;
    int   axis;
    vec2  image_size;
)";

const char* kBloomCombineUniforms = R"(
    float intensity;
    float _pad0;
    vec2  _pad1;
)";

const char* kCopyUniforms = R"(
    float _pad0;
    float _pad1;
    vec2  _pad2;
)";

std::string build_fragment_source(const char* uniforms, const char* body,
                                  bool needs_glow) {
    std::string source = kCommonHeader;
    source += uniforms;
    source += kCommonFooter;
    if (needs_glow) source += "uniform sampler2D u_glow;\n";
    source += body;
    return source;
}

}  // namespace

const char* GlBackend::builtin_fragment_source(const std::string& shader_name) {
    // Returned strings are built once and kept alive for the process; the map is
    // keyed by shader name.
    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(shader_name);
    if (it != cache.end()) return it->second.c_str();

    std::string source;
    if (shader_name == "tone") {
        source = build_fragment_source(kToneUniforms, kToneBody, false);
    } else if (shader_name == "vignette") {
        source = build_fragment_source(kVignetteUniforms, kVignetteBody, false);
    } else if (shader_name == "sharpen") {
        source = build_fragment_source(kSharpenUniforms, kSharpenBody, false);
    } else if (shader_name == "blur_horizontal" || shader_name == "blur_vertical") {
        source = build_fragment_source(kBlurUniforms, kBlurBody, false);
    } else if (shader_name == "bloom_bright") {
        source = build_fragment_source(kBloomBrightUniforms, kBloomBrightBody, false);
    } else if (shader_name == "bloom_blur_horizontal" ||
               shader_name == "bloom_blur_vertical") {
        source = build_fragment_source(kBloomBlurUniforms, kBloomBlurBody, false);
    } else if (shader_name == "bloom_combine") {
        source = build_fragment_source(kBloomCombineUniforms, kBloomCombineBody, true);
    } else if (shader_name == "copy") {
        source = build_fragment_source(kCopyUniforms, kCopyBody, false);
    } else {
        return nullptr;
    }
    auto inserted = cache.emplace(shader_name, std::move(source));
    return inserted.first->second.c_str();
}

// The encoder records one full-screen pass: it binds the pipeline's program,
// binds the input texture as a sampler and the target as an FBO, uploads the
// push-constant block to the UBO, and draws. Because GL draws immediately, the
// pass is executed at submit() time so the encoder matches the interface's
// record-then-submit contract.
class GlBackend::Encoder : public CommandEncoder {
public:
    explicit Encoder(GlBackend& backend) : backend_(backend) {}

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
                                 "OpenGL backend binds at most " +
                                     std::to_string(kMaxInputs) + " input textures");
        }
        inputs_[slot] = texture;
        input_count_ = std::max(input_count_, slot + 1);
        return Status::success();
    }

    Status bind_storage_texture(u32 slot, ResourceHandle texture) override {
        if (slot != 0) {
            return Status::error(ErrorCode::InvalidArgument,
                                 "OpenGL backend binds only one storage texture");
        }
        output_ = texture;
        return Status::success();
    }

    Status set_push_constants(const void* data, u32 bytes) override {
        push_.assign(static_cast<const u8*>(data),
                     static_cast<const u8*>(data) + bytes);
        return Status::success();
    }

    // A dispatch is a fragment pass; the group counts are ignored because the
    // pass covers the whole target exactly once.
    Status dispatch(const DispatchDesc&) override { return Status::success(); }

    Status execute() {
        auto& gl = backend_.gl_;
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
        if (pipeline_.program == 0) {
            return Status::error(ErrorCode::NotImplemented,
                                 "pipeline has no program; register its shader source first");
        }

        gl.BindFramebuffer(gl::GL_FRAMEBUFFER, dst->fbo);
        gl.Viewport(0, 0, static_cast<gl::GLsizei>(dst->desc.width),
                    static_cast<gl::GLsizei>(dst->desc.height));
        gl.UseProgram(pipeline_.program);

        // Bind the inputs to consecutive texture units and point the shader's
        // samplers at them. The first input is always u_image; a second input
        // (bloom's glow) is u_glow. The uniform locations are cached per program
        // so this costs nothing after the first pass.
        for (u32 i = 0; i < input_count_; ++i) {
            auto* src = backend_.find_texture(inputs_[i]);
            gl.ActiveTexture(gl::GL_TEXTURE0 + i);
            gl.BindTexture(gl::GL_TEXTURE_2D, src->texture);
        }
        backend_.bind_input_samplers(pipeline_.program, input_count_);

        gl.BindBuffer(gl::GL_UNIFORM_BUFFER, backend_.ubo_);
        if (!push_.empty()) {
            gl.BufferSubData(gl::GL_UNIFORM_BUFFER, 0,
                             static_cast<long long>(push_.size()), push_.data());
        }
        gl.BindBufferBase(gl::GL_UNIFORM_BUFFER, 0, backend_.ubo_);

        gl.BindVertexArray(backend_.vao_);
        gl.DrawArrays(gl::GL_TRIANGLES, 0, 3);
        gl.BindVertexArray(0);
        gl.BindFramebuffer(gl::GL_FRAMEBUFFER, 0);

        const gl::GLenum err = gl.GetError();
        if (err != gl::GL_NO_ERROR) {
            return Status::error(ErrorCode::Unknown,
                                 "OpenGL error 0x" + std::to_string(err) + " during pass");
        }
        return Status::success();
    }

private:
    static constexpr u32 kMaxInputs = 4;
    GlBackend& backend_;
    PipelineRecord pipeline_;
    bool have_pipeline_ = false;
    std::array<ResourceHandle, kMaxInputs> inputs_{};
    u32 input_count_ = 0;
    ResourceHandle output_{};
    std::vector<u8> push_;
};

GlBackend::GlBackend(const BackendCreateInfo& info) {
    info_.kind = BackendKind::OpenGL;
    info_.driver_name = "loucuraxxxx-gl";
    info_.api_version = "3.3 core";

    gl::GlApi gl{};
    gl::EglApi egl{};
    std::string error;
    if (!gl::load(gl, egl, error)) {
        // Probe already reported availability; reaching here means the library
        // went away or a symbol is missing.
        info_.maturity = Maturity::Planned;
        info_.device_name = "OpenGL unavailable: " + error;
        return;
    }
    gl_ = gl;
    egl_ = egl;

    // Prefer the surfaceless platform so no display server is required.
    if (egl_.GetPlatformDisplayEXT) {
        display_ = egl_.GetPlatformDisplayEXT(gl::EGL_PLATFORM_SURFACELESS_MESA, nullptr,
                                              nullptr);
    }
    if (display_ == nullptr) display_ = egl_.GetDisplay(nullptr);
    if (display_ == nullptr) {
        info_.maturity = Maturity::Planned;
        info_.device_name = "OpenGL unavailable: no EGL display";
        return;
    }

    gl::EGLint major = 0, minor = 0;
    if (!egl_.Initialize(display_, &major, &minor)) {
        info_.maturity = Maturity::Planned;
        info_.device_name = "OpenGL unavailable: eglInitialize failed";
        display_ = nullptr;
        return;
    }
    egl_.BindAPI(gl::EGL_OPENGL_API);

    const gl::EGLint config_attribs[] = {
        gl::EGL_SURFACE_TYPE, gl::EGL_PBUFFER_BIT,
        gl::EGL_RENDERABLE_TYPE, gl::EGL_OPENGL_BIT,
        gl::EGL_RED_SIZE, 8,
        gl::EGL_GREEN_SIZE, 8,
        gl::EGL_BLUE_SIZE, 8,
        gl::EGL_ALPHA_SIZE, 8,
        gl::EGL_NONE,
    };
    gl::EGLConfig config = nullptr;
    gl::EGLint num_configs = 0;
    if (!egl_.ChooseConfig(display_, config_attribs, &config, 1, &num_configs) ||
        num_configs < 1) {
        info_.maturity = Maturity::Planned;
        info_.device_name = "OpenGL unavailable: no matching EGL config";
        egl_.Terminate(display_);
        display_ = nullptr;
        return;
    }

    const gl::EGLint context_attribs[] = {
        gl::EGL_CONTEXT_MAJOR_VERSION, 3,
        gl::EGL_CONTEXT_MINOR_VERSION, 3,
        gl::EGL_CONTEXT_OPENGL_PROFILE_MASK, gl::EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        gl::EGL_NONE,
    };
    context_ = egl_.CreateContext(display_, config, nullptr, context_attribs);
    if (context_ == nullptr) {
        info_.maturity = Maturity::Planned;
        info_.device_name = "OpenGL unavailable: no 3.3 core context";
        egl_.Terminate(display_);
        display_ = nullptr;
        return;
    }
    if (!egl_.MakeCurrent(display_, nullptr, nullptr, context_)) {
        info_.maturity = Maturity::Planned;
        info_.device_name = "OpenGL unavailable: eglMakeCurrent failed";
        egl_.DestroyContext(display_, context_);
        egl_.Terminate(display_);
        display_ = nullptr;
        context_ = nullptr;
        return;
    }

    const auto* renderer = reinterpret_cast<const char*>(gl_.GetString(gl::GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(gl_.GetString(gl::GL_VERSION));
    info_.device_name = renderer ? renderer : "OpenGL device";
    info_.api_version = version ? version : "3.3 core";
    info_.maturity = Maturity::Implemented;

    gl::GLint max_dim = 0;
    gl_.GetIntegerv(gl::GL_MAX_TEXTURE_SIZE, &max_dim);
    // GL_RENDERER reports the real implementation, so it is what tells a GPU
    // apart from a software rasterizer such as llvmpipe or softpipe. Claiming
    // hardware unconditionally would make the cost model treat a software path
    // as an accelerator.
    info_.capabilities.is_hardware =
        !looks_like_software_rasterizer(info_.device_name) && !running_under_wine();
    // GL 3.3 core has no compute shaders; passes run as fragment passes. Saying
    // otherwise would be a lie the cost model would act on.
    info_.capabilities.supports_compute = false;
    info_.capabilities.supports_storage_textures = false;
    info_.capabilities.supports_float_textures = true;
    info_.capabilities.supports_parallel_dispatch = false;
    info_.capabilities.supports_timestamps = false;
    info_.capabilities.max_texture_dimension = static_cast<u32>(max_dim > 0 ? max_dim : 0);
    info_.capabilities.max_workgroup_size = 0;
    info_.capabilities.max_memory_bytes = info.memory_budget_bytes;
    // A GL texture lives in driver memory, not host memory, so there is no
    // zero-copy mapping; uploads and downloads go through the GL calls.
    info_.capabilities.unified_memory = false;
    info_.capabilities.supports_host_mapping = false;

    // The full-screen triangle needs a bound VAO in the core profile even with
    // no attributes.
    gl_.GenVertexArrays(1, &vao_);
    gl_.GenBuffers(1, &ubo_);
    gl_.BindBuffer(gl::GL_UNIFORM_BUFFER, ubo_);
    // Room for the largest push block any built-in pass uses.
    gl_.BufferData(gl::GL_UNIFORM_BUFFER, 256, nullptr, gl::GL_DYNAMIC_DRAW);
    gl_.BindBuffer(gl::GL_UNIFORM_BUFFER, 0);

    LX_INFO << "OpenGL backend ready: " << info_.device_name << " (" << info_.api_version
            << "), max texture " << info_.capabilities.max_texture_dimension;
}

GlBackend::~GlBackend() {
    if (context_ == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, record] : textures_) {
        if (record.fbo) gl_.DeleteFramebuffers(1, &record.fbo);
        if (record.texture) gl_.DeleteTextures(1, &record.texture);
    }
    textures_.clear();
    for (auto& [name, pipeline] : pipelines_) {
        if (pipeline.program) gl_.DeleteProgram(pipeline.program);
    }
    pipelines_.clear();
    if (vao_) gl_.DeleteVertexArrays(1, &vao_);
    if (ubo_) gl_.DeleteBuffers(1, &ubo_);
    egl_.MakeCurrent(display_, nullptr, nullptr, nullptr);
    egl_.DestroyContext(display_, context_);
    egl_.Terminate(display_);
}

GlBackend::TextureRecord* GlBackend::find_texture(ResourceHandle handle) {
    auto it = textures_.find(handle.id);
    return it == textures_.end() ? nullptr : &it->second;
}

void GlBackend::bind_input_samplers(gl::GLuint program, u32 input_count) {
    if (input_count == 0) return;
    auto image_it = image_sampler_locations_.find(program);
    if (image_it == image_sampler_locations_.end()) {
        const gl::GLint loc = gl_.GetUniformLocation(program, "u_image");
        image_it = image_sampler_locations_.emplace(program, loc).first;
    }
    if (image_it->second >= 0) gl_.Uniform1i(image_it->second, 0);
    if (input_count < 2) return;
    auto glow_it = glow_sampler_locations_.find(program);
    if (glow_it == glow_sampler_locations_.end()) {
        const gl::GLint loc = gl_.GetUniformLocation(program, "u_glow");
        glow_it = glow_sampler_locations_.emplace(program, loc).first;
    }
    if (glow_it->second >= 0) gl_.Uniform1i(glow_it->second, 1);
}

Status GlBackend::ensure_vao() {
    if (vao_ == 0) gl_.GenVertexArrays(1, &vao_);
    return Status::success();
}

Result<ResourceHandle> GlBackend::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return Status::error(ErrorCode::InvalidArgument,
                             "texture dimensions must be non-zero");
    }
    if (info_.capabilities.max_texture_dimension != 0 &&
        (desc.width > info_.capabilities.max_texture_dimension ||
         desc.height > info_.capabilities.max_texture_dimension)) {
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

    gl_.GenTextures(1, &record.texture);
    gl_.BindTexture(gl::GL_TEXTURE_2D, record.texture);
    gl_.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    gl_.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
    gl_.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_S, gl::GL_CLAMP_TO_EDGE);
    gl_.TexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_T, gl::GL_CLAMP_TO_EDGE);
    gl_.TexImage2D(gl::GL_TEXTURE_2D, 0, gl::GL_RGBA8, static_cast<gl::GLsizei>(desc.width),
                   static_cast<gl::GLsizei>(desc.height), 0, gl::GL_RGBA,
                   gl::GL_UNSIGNED_BYTE, nullptr);
    record.allocated = true;

    // Every texture gets an FBO so it can be a pass target.
    gl_.GenFramebuffers(1, &record.fbo);
    gl_.BindFramebuffer(gl::GL_FRAMEBUFFER, record.fbo);
    gl_.FramebufferTexture2D(gl::GL_FRAMEBUFFER, gl::GL_COLOR_ATTACHMENT0, gl::GL_TEXTURE_2D,
                             record.texture, 0);
    const gl::GLenum complete = gl_.CheckFramebufferStatus(gl::GL_FRAMEBUFFER);
    gl_.BindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    gl_.BindTexture(gl::GL_TEXTURE_2D, 0);
    if (complete != gl::GL_FRAMEBUFFER_COMPLETE) {
        gl_.DeleteFramebuffers(1, &record.fbo);
        gl_.DeleteTextures(1, &record.texture);
        return Status::error(ErrorCode::InitializationFailed,
                             "framebuffer for texture is incomplete");
    }

    const u64 id = next_id_++;
    allocated_bytes_ += bytes;
    textures_.emplace(id, record);
    return ResourceHandle{id};
}

Status GlBackend::destroy_texture(ResourceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(handle.id);
    if (it == textures_.end()) {
        return Status::error(ErrorCode::InvalidArgument, "unknown texture handle");
    }
    const u64 bytes = texture_byte_size(it->second.desc.width, it->second.desc.height,
                                        it->second.desc.format);
    if (it->second.fbo) gl_.DeleteFramebuffers(1, &it->second.fbo);
    if (it->second.texture) gl_.DeleteTextures(1, &it->second.texture);
    allocated_bytes_ -= bytes;
    textures_.erase(it);
    return Status::success();
}

Status GlBackend::upload_texture(ResourceHandle handle, const void* data, u64 bytes,
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

    gl_.BindTexture(gl::GL_TEXTURE_2D, record->texture);
    gl_.PixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);
    if (pitch == tight_row) {
        if (record->allocated) {
            gl_.TexSubImage2D(gl::GL_TEXTURE_2D, 0, 0, 0,
                              static_cast<gl::GLsizei>(record->desc.width),
                              static_cast<gl::GLsizei>(record->desc.height), gl::GL_RGBA,
                              gl::GL_UNSIGNED_BYTE, data);
        } else {
            gl_.TexImage2D(gl::GL_TEXTURE_2D, 0, gl::GL_RGBA8,
                           static_cast<gl::GLsizei>(record->desc.width),
                           static_cast<gl::GLsizei>(record->desc.height), 0, gl::GL_RGBA,
                           gl::GL_UNSIGNED_BYTE, data);
            record->allocated = true;
        }
    } else {
        // A padded source is repacked into a tight buffer first; GL 3.3 core has
        // no per-row stride for TexImage2D without pixel-unpack state we would
        // have to set and restore.
        std::vector<u8> tight(static_cast<std::size_t>(tight_row) * record->desc.height);
        const auto* src = static_cast<const u8*>(data);
        for (u32 y = 0; y < record->desc.height; ++y) {
            std::memcpy(tight.data() + static_cast<std::size_t>(y) * tight_row,
                        src + static_cast<std::size_t>(y) * pitch,
                        static_cast<std::size_t>(tight_row));
        }
        if (record->allocated) {
            gl_.TexSubImage2D(gl::GL_TEXTURE_2D, 0, 0, 0,
                              static_cast<gl::GLsizei>(record->desc.width),
                              static_cast<gl::GLsizei>(record->desc.height), gl::GL_RGBA,
                              gl::GL_UNSIGNED_BYTE, tight.data());
        } else {
            gl_.TexImage2D(gl::GL_TEXTURE_2D, 0, gl::GL_RGBA8,
                           static_cast<gl::GLsizei>(record->desc.width),
                           static_cast<gl::GLsizei>(record->desc.height), 0, gl::GL_RGBA,
                           gl::GL_UNSIGNED_BYTE, tight.data());
            record->allocated = true;
        }
    }
    gl_.BindTexture(gl::GL_TEXTURE_2D, 0);
    return Status::success();
}

Result<u64> GlBackend::download_texture(ResourceHandle handle, void* out, u64 out_bytes,
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

    gl_.BindTexture(gl::GL_TEXTURE_2D, record->texture);
    gl_.PixelStorei(gl::GL_PACK_ALIGNMENT, 1);
    if (pitch == tight_row) {
        gl_.GetTexImage(gl::GL_TEXTURE_2D, 0, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, out);
    } else {
        std::vector<u8> tight(static_cast<std::size_t>(tight_row) * record->desc.height);
        gl_.GetTexImage(gl::GL_TEXTURE_2D, 0, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, tight.data());
        auto* dst = static_cast<u8*>(out);
        for (u32 y = 0; y < record->desc.height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * pitch,
                        tight.data() + static_cast<std::size_t>(y) * tight_row,
                        static_cast<std::size_t>(tight_row));
        }
    }
    gl_.BindTexture(gl::GL_TEXTURE_2D, 0);
    return required;
}

Status GlBackend::register_shader(const std::string& name,
                                  const std::vector<u32>& bytecode) {
    (void)name;
    (void)bytecode;
    // GL takes GLSL source, not SPIR-V. Refuse rather than pretend to consume it.
    return Status::error(ErrorCode::NotSupported,
                         "OpenGL backend accepts GLSL source, not SPIR-V bytecode");
}

Status GlBackend::register_shader_source(const std::string& name,
                                         const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    shader_sources_[name] = source;
    return Status::success();
}

Status GlBackend::register_cpu_kernel(const std::string& name, CpuKernel kernel) {
    std::lock_guard<std::mutex> lock(mutex_);
    kernels_[name] = kernel;
    return Status::success();
}

Status GlBackend::compile_pipeline(PipelineRecord& pipeline, const std::string& source) {
    const char* src = source.c_str();
    gl::GLuint vs = gl_.CreateShader(gl::GL_VERTEX_SHADER);
    gl_.ShaderSource(vs, 1, &kFullscreenVertex, nullptr);
    gl_.CompileShader(vs);
    gl::GLint ok = 0;
    gl_.GetShaderiv(vs, gl::GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl_.GetShaderInfoLog(vs, sizeof(log) - 1, nullptr, log);
        gl_.DeleteShader(vs);
        return Status::error(ErrorCode::InitializationFailed,
                             std::string("vertex shader compile failed: ") + log);
    }

    gl::GLuint fs = gl_.CreateShader(gl::GL_FRAGMENT_SHADER);
    gl_.ShaderSource(fs, 1, &src, nullptr);
    gl_.CompileShader(fs);
    ok = 0;
    gl_.GetShaderiv(fs, gl::GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl_.GetShaderInfoLog(fs, sizeof(log) - 1, nullptr, log);
        gl_.DeleteShader(vs);
        gl_.DeleteShader(fs);
        return Status::error(ErrorCode::InitializationFailed,
                             std::string("fragment shader compile failed: ") + log);
    }

    gl::GLuint program = gl_.CreateProgram();
    gl_.AttachShader(program, vs);
    gl_.AttachShader(program, fs);
    gl_.LinkProgram(program);
    ok = 0;
    gl_.GetProgramiv(program, gl::GL_LINK_STATUS, &ok);
    gl_.DeleteShader(vs);
    gl_.DeleteShader(fs);
    if (!ok) {
        char log[1024] = {0};
        gl_.GetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        gl_.DeleteProgram(program);
        return Status::error(ErrorCode::InitializationFailed,
                             std::string("program link failed: ") + log);
    }

    if (pipeline.program) gl_.DeleteProgram(pipeline.program);
    pipeline.program = program;
    // Bind the emulated push-constant block to slot 0 for this program.
    gl_.UseProgram(program);
    gl_.BindBufferBase(gl::GL_UNIFORM_BUFFER, 0, ubo_);
    gl_.UseProgram(0);
    return Status::success();
}

Status GlBackend::create_pipeline(const std::string& name,
                                  const std::string& shader_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    PipelineRecord pipeline;
    pipeline.shader_name = shader_name;

    auto it = shader_sources_.find(shader_name);
    if (it != shader_sources_.end()) {
        LX_TRY(compile_pipeline(pipeline, it->second));
    } else if (const char* builtin = builtin_fragment_source(shader_name)) {
        LX_TRY(compile_pipeline(pipeline, builtin));
    } else {
        // The pipeline is registered without a program; a pass that uses it
        // fails with NotImplemented rather than drawing nothing.
        pipeline.program = 0;
    }
    pipelines_[name] = pipeline;
    return Status::success();
}

Result<std::unique_ptr<CommandEncoder>> GlBackend::begin_commands() {
    return std::unique_ptr<CommandEncoder>(new Encoder(*this));
}

Status GlBackend::submit(CommandEncoder* encoder) {
    if (encoder == nullptr) {
        // No recorded work: just ensure prior work is complete.
        return sync();
    }
    auto* gl_encoder = dynamic_cast<Encoder*>(encoder);
    if (gl_encoder == nullptr) {
        return Status::error(ErrorCode::InvalidArgument,
                             "encoder was not created by this backend");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return gl_encoder->execute();
}

Status GlBackend::wait_idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    gl_.Finish();
    return Status::success();
}

Status GlBackend::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    gl_.Finish();
    return Status::success();
}

u64 GlBackend::allocated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocated_bytes_;
}

BackendProbeResult GlBackend::probe() {
    BackendProbeResult result;
    gl::GlApi gl{};
    gl::EglApi egl{};
    std::string error;
    if (!gl::load(gl, egl, error)) {
        result.reason = error;
        return result;
    }
    // Confirm a real 3.3 core context can be made; probing must not assume.
    gl::EGLDisplay display = nullptr;
    if (egl.GetPlatformDisplayEXT) {
        display = egl.GetPlatformDisplayEXT(gl::EGL_PLATFORM_SURFACELESS_MESA, nullptr,
                                            nullptr);
    }
    if (display == nullptr) display = egl.GetDisplay(nullptr);
    if (display == nullptr) {
        result.reason = "no EGL display";
        return result;
    }
    gl::EGLint major = 0, minor = 0;
    if (!egl.Initialize(display, &major, &minor)) {
        result.reason = "eglInitialize failed";
        return result;
    }
    egl.BindAPI(gl::EGL_OPENGL_API);
    const gl::EGLint config_attribs[] = {
        gl::EGL_SURFACE_TYPE, gl::EGL_PBUFFER_BIT,
        gl::EGL_RENDERABLE_TYPE, gl::EGL_OPENGL_BIT,
        gl::EGL_RED_SIZE, 8, gl::EGL_GREEN_SIZE, 8, gl::EGL_BLUE_SIZE, 8,
        gl::EGL_ALPHA_SIZE, 8, gl::EGL_NONE,
    };
    gl::EGLConfig config = nullptr;
    gl::EGLint num = 0;
    // Once eglInitialize succeeds the driver has per-thread state to release on
    // every exit path, not just the success one.
    auto release_thread = [&]() {
        if (egl.ReleaseThread) egl.ReleaseThread();
    };
    if (!egl.ChooseConfig(display, config_attribs, &config, 1, &num) || num < 1) {
        result.reason = "no matching EGL config";
        egl.Terminate(display);
        release_thread();
        return result;
    }
    const gl::EGLint context_attribs[] = {
        gl::EGL_CONTEXT_MAJOR_VERSION, 3,
        gl::EGL_CONTEXT_MINOR_VERSION, 3,
        gl::EGL_CONTEXT_OPENGL_PROFILE_MASK, gl::EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        gl::EGL_NONE,
    };
    gl::EGLContext context = egl.CreateContext(display, config, nullptr, context_attribs);
    if (context == nullptr) {
        result.reason = "no OpenGL 3.3 core context";
        egl.Terminate(display);
        release_thread();
        return result;
    }
    if (!egl.MakeCurrent(display, nullptr, nullptr, context)) {
        result.reason = "eglMakeCurrent failed";
        egl.DestroyContext(display, context);
        egl.Terminate(display);
        release_thread();
        return result;
    }
    const auto* renderer = reinterpret_cast<const char*>(gl.GetString(gl::GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(gl.GetString(gl::GL_VERSION));
    result.available = true;
    result.device_name = renderer ? renderer : "OpenGL device";
    result.is_hardware =
        renderer != nullptr && !looks_like_software_rasterizer(result.device_name) &&
        !running_under_wine();
    if (version) result.reason = std::string("OpenGL ") + version;

    egl.MakeCurrent(display, nullptr, nullptr, nullptr);
    egl.DestroyContext(display, context);
    egl.Terminate(display);
    // Drop the driver's per-thread EGL state. Mesa allocates it on the first
    // eglInitialize and holds it for the life of the thread, so without this a
    // probe that runs once leaves a small allocation live at exit.
    release_thread();
    return result;
}

Result<std::unique_ptr<Backend>> GlBackend::create(const BackendCreateInfo& info) {
    std::unique_ptr<GlBackend> backend(new GlBackend(info));
    if (backend->info_.maturity == Maturity::Planned) {
        return Status::error(ErrorCode::BackendUnavailable, backend->info_.device_name);
    }
    return std::unique_ptr<Backend>(backend.release());
}

}  // namespace lx::backends
