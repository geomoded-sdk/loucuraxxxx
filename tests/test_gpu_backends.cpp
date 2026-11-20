// Tests for the real GPU backends (OpenGL 3.3 core and Direct3D 11).
//
// These tests are deliberately written to run on whatever the host actually
// offers. On Linux the OpenGL backend is exercised against Mesa (llvmpipe in
// CI); on Windows/Wine the Direct3D 11 backend is exercised. When neither API is
// present the tests skip, but they assert the *contract* either way:
//   * a registered backend must probe honestly,
//   * an unavailable backend must fail creation with BackendUnavailable,
//   * an available backend must produce, for every built-in pass, output that
//     matches the CPU filter within a small float-rounding tolerance.
//
// The last point is the one that matters: it is what proves the GPU path is real
// rather than a stub that happens to allocate a texture.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "backends/backend_registry.hpp"
#include "filters/builtin/bloom_filter.hpp"
#include "filters/builtin/tone_filter.hpp"
#include "filters/filter_registry.hpp"
#include "filters/filter_chain.hpp"
#include "memory/buffer_pool.hpp"
#include "renderer/renderer.hpp"
#include "test_framework.hpp"

#if defined(LX_HAS_OPENGL)
#include "backends/opengl/gl_backend.hpp"
#endif
#if defined(LX_HAS_D3D11)
#include "backends/d3d11/d3d11_backend.hpp"
#endif
#if defined(LX_HAS_VULKAN)
#include "backends/vulkan/vulkan_backend.hpp"
#endif

using namespace lx;
using namespace lx::backends;

namespace {

// A deterministic test frame: a horizontal gradient in red, a vertical one in
// green, and a diagonal pattern in blue, so every pass has structure to act on.
std::vector<u8> make_frame(u32 width, u32 height) {
    std::vector<u8> pixels(static_cast<std::size_t>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[o + 0] = static_cast<u8>(width > 1 ? x * 255 / (width - 1) : 0);
            pixels[o + 1] = static_cast<u8>(height > 1 ? y * 255 / (height - 1) : 0);
            pixels[o + 2] = static_cast<u8>((x * 7 + y * 3) % 256);
            pixels[o + 3] = 255;
        }
    }
    return pixels;
}

// Returns the backend kind to exercise on this host, or Cpu when none is usable.
// The order matches the production selection ladder: Vulkan first when it was
// built in, then Direct3D 11 on Windows, then OpenGL 3.3.
BackendKind host_gpu_backend() {
#if defined(LX_HAS_VULKAN)
    if (VulkanBackend::probe().available) return BackendKind::Vulkan;
#endif
#if defined(LX_HAS_D3D11)
    if (D3D11Backend::probe().available) return BackendKind::Direct3D11;
#endif
#if defined(LX_HAS_OPENGL)
    if (GlBackend::probe().available) return BackendKind::OpenGL;
#endif
    return BackendKind::Cpu;
}

// The built-in shader source for `shader_name` on the host's GPU backend.
const char* builtin_source_for(BackendKind kind, const std::string& shader_name) {
#if defined(LX_HAS_VULKAN)
    if (kind == BackendKind::Vulkan) {
        return VulkanBackend::builtin_compute_source(shader_name);
    }
#endif
#if defined(LX_HAS_D3D11)
    if (kind == BackendKind::Direct3D11) {
        return D3D11Backend::builtin_pixel_shader_source(shader_name);
    }
#endif
#if defined(LX_HAS_OPENGL)
    if (kind == BackendKind::OpenGL) {
        return GlBackend::builtin_fragment_source(shader_name);
    }
#endif
    (void)kind;
    (void)shader_name;
    return nullptr;
}

// Runs one built-in pass on the backend from `in` to `out`, using the shader
// source the backend itself ships.
Status run_pass(Backend* backend, BackendKind kind, ResourceHandle in,
                ResourceHandle out, const std::string& shader_name, const void* push,
                u32 push_bytes) {
    const char* source = builtin_source_for(kind, shader_name);
    if (source == nullptr) {
        return Status::error(ErrorCode::NotImplemented, "no built-in for " + shader_name);
    }
    LX_TRY(backend->register_shader_source(shader_name, source));
    const std::string pipeline_name = shader_name + "_test";
    LX_TRY(backend->create_pipeline(pipeline_name, shader_name));

    auto encoder = backend->begin_commands();
    if (!encoder.ok()) return encoder.status();
    LX_TRY(encoder.value()->bind_pipeline(pipeline_name));
    LX_TRY(encoder.value()->bind_texture(0, in));
    LX_TRY(encoder.value()->bind_storage_texture(0, out));
    if (push != nullptr && push_bytes > 0) {
        LX_TRY(encoder.value()->set_push_constants(push, push_bytes));
    }
    DispatchDesc dispatch;
    dispatch.kernel_name = shader_name;
    LX_TRY(encoder.value()->dispatch(dispatch));
    LX_TRY(backend->submit(encoder.value().get()));
    return backend->wait_idle();
}

int max_abs_diff(const std::vector<u8>& a, const std::vector<u8>& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}

}  // namespace

LX_TEST(gpu_backends, registry_contains_compiled_backends) {
    const auto kinds = available_backend_kinds();
    LX_CHECK(!kinds.empty());
    // The CPU reference backend is always present.
    LX_CHECK(find_backend(BackendKind::Cpu) != nullptr);
#if defined(LX_HAS_OPENGL)
    LX_CHECK(find_backend(BackendKind::OpenGL) != nullptr);
#endif
#if defined(LX_HAS_D3D11)
    LX_CHECK(find_backend(BackendKind::Direct3D11) != nullptr);
#endif
#if defined(LX_HAS_VULKAN)
    LX_CHECK(find_backend(BackendKind::Vulkan) != nullptr);
#endif
}

LX_TEST(gpu_backends, unavailable_backend_reports_backend_unavailable) {
    // Whichever of Vulkan/Metal is not compiled here must fail creation with
    // BackendUnavailable rather than crash or silently succeed.
    for (BackendKind kind : {BackendKind::Vulkan, BackendKind::Metal}) {
        if (find_backend(kind) != nullptr) continue;  // compiled: skip this one
        BackendCreateInfo info;
        info.preferred = kind;
        auto backend = create_backend(info);
        LX_CHECK(!backend.ok());
        LX_CHECK(backend.status().code() == ErrorCode::BackendUnavailable);
    }
}

LX_TEST(gpu_backends, validation_request_never_breaks_backend_creation) {
    // `allow_validation` used to be honoured only by the D3D11 backend; Vulkan
    // and OpenGL ignored it without a word, so asking for validation got you
    // nothing. Vulkan now enables VK_LAYER_KHRONOS_validation when it is
    // installed. The contract that matters for every backend is that asking for
    // validation must never turn a working backend into a failed one: a missing
    // layer is a warning, not an error.
    for (const auto kind : {BackendKind::Cpu, BackendKind::Vulkan, BackendKind::OpenGL,
                            BackendKind::Direct3D11}) {
        if (find_backend(kind) == nullptr) continue;
        BackendCreateInfo info;
        info.preferred = kind;
        info.allow_validation = true;
        auto backend = create_backend(info);
        if (!backend.ok()) {
            // Unavailable hardware is fine; a validation-induced failure is not.
            LX_CHECK(backend.status().code() == ErrorCode::BackendUnavailable);
            continue;
        }
        LX_CHECK(backend.value() != nullptr);
    }
}

LX_TEST(gpu_backends, ladder_prefers_hardware_over_cpu) {
    const auto available = probe_available_backends();
    LX_CHECK(!available.empty());
    // The CPU backend, if available, must be last: hardware always wins.
    const auto& last = available.back();
    if (last.kind == BackendKind::Cpu) {
        for (std::size_t i = 0; i + 1 < available.size(); ++i) {
            LX_CHECK(available[i].kind != BackendKind::Cpu);
        }
    }
}

LX_TEST(gpu_backends, ladder_prefers_hardware_over_software_rasterizer) {
    // A host can expose a software ICD (Mesa lavapipe, Windows WARP) alongside a
    // real GPU. The ladder must never rank the software path above hardware,
    // whatever API each one uses, or placement decisions would be made on a
    // false premise.
    const auto available = probe_available_backends();
    bool seen_software = false;
    for (const auto& entry : available) {
        if (entry.is_hardware) {
            LX_CHECK(!seen_software);
        } else if (entry.kind != BackendKind::Cpu) {
            seen_software = true;
        }
    }
    // The CPU path is never reported as hardware.
    for (const auto& entry : available) {
        if (entry.kind == BackendKind::Cpu) LX_CHECK(!entry.is_hardware);
    }
}

LX_TEST(gpu_backends, ladder_ranks_software_rasterizer_below_cpu) {
    // A software rasterizer runs the same work on the same cores as the CPU
    // reference path, plus driver overhead. Selecting it as the default is
    // therefore strictly worse and must never happen, so the CPU path always
    // ranks ahead of a non-hardware GPU API. Without this the "best backend"
    // default on a host with no real GPU picked llvmpipe and ran ~30 fps where
    // the plain CPU chain ran several times faster.
    const auto available = probe_available_backends();
    bool seen_cpu = false;
    for (const auto& entry : available) {
        const bool software_rasterizer = !entry.is_hardware && entry.kind != BackendKind::Cpu;
        if (entry.kind == BackendKind::Cpu) {
            seen_cpu = true;
        } else if (software_rasterizer) {
            LX_CHECK(seen_cpu);
        }
    }
}

LX_TEST(gpu_backends, software_rasterizer_names_are_recognised) {
    // The heuristic only has to catch the names these implementations really
    // report; a real driver name must not be caught by accident.
    for (const char* name : {"llvmpipe (LLVM 17.0.6, 256 bits)", "Mesa softpipe",
                             "lavapipe", "SwiftShader Device", "Microsoft Basic Render Driver",
                             "Microsoft WARP"}) {
        LX_CHECK(looks_like_software_rasterizer(name));
    }
    for (const char* name : {"NVIDIA GeForce RTX 4070", "AMD Radeon RX 7900 XTX",
                             "Intel(R) UHD Graphics 630", "Apple M4", "Mesa Intel(R) Xe"}) {
        LX_CHECK(!looks_like_software_rasterizer(name));
    }
}

LX_TEST(gpu_backends, wine_environment_is_detected) {
    // Wine fabricates the adapter it reports, including a plausible card name and
    // vendor id, so the name heuristic cannot catch it. This asserts the
    // environment probe agrees with the process it is running in: under Wine it
    // must say so, and on a native host it must not, otherwise a real GPU would
    // be demoted to software.
    const bool under_wine = running_under_wine();
#if defined(_WIN32)
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const bool wine_loader_present =
        ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    LX_CHECK(under_wine == wine_loader_present);
#else
    LX_CHECK(!under_wine);
#endif
    // The point of the probe is that it reaches the placement code, so assert the
    // consequence: under Wine no API backend may be advertised as hardware.
    if (under_wine) {
        for (BackendKind kind : {BackendKind::Direct3D11, BackendKind::OpenGL,
                                 BackendKind::Vulkan}) {
            const BackendRegistration* reg = find_backend(kind);
            if (reg == nullptr || reg->probe == nullptr) continue;
            const BackendProbeResult probed = reg->probe();
            if (probed.available) LX_CHECK(!probed.is_hardware);
        }
    }
}

LX_TEST(gpu_backends, upload_download_roundtrip_matches_input) {
    const BackendKind kind = host_gpu_backend();
    if (kind == BackendKind::Cpu) {
        LX_CHECK(true);  // no GPU API on this host; nothing to exercise
        return;
    }
    BackendCreateInfo info;
    info.preferred = kind;
    auto backend = create_backend(info);
    LX_CHECK_OK(backend.status());
    if (!backend.ok()) return;

    const u32 w = 64, h = 48;
    const auto source = make_frame(w, h);
    TextureDesc desc;
    desc.width = w;
    desc.height = h;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());
    LX_CHECK_OK(backend.value()->upload_texture(texture.value(), source.data(), source.size()));

    std::vector<u8> readback(source.size(), 0);
    auto written = backend.value()->download_texture(texture.value(), readback.data(),
                                                     readback.size());
    LX_CHECK_OK(written.status());
    LX_CHECK_EQ(written.value(), static_cast<u64>(source.size()));
    // A real GPU upload/download must be byte-exact for an untouched texture.
    LX_CHECK(readback == source);
    backend.value()->destroy_texture(texture.value());
}

LX_TEST(gpu_backends, tone_pass_matches_cpu_filter) {
    const BackendKind kind = host_gpu_backend();
    if (kind == BackendKind::Cpu) {
        LX_CHECK(true);
        return;
    }
    BackendCreateInfo info;
    info.preferred = kind;
    auto backend = create_backend(info);
    LX_CHECK_OK(backend.status());
    if (!backend.ok()) return;

    const u32 w = 64, h = 48;
    const auto source = make_frame(w, h);
    TextureDesc desc;
    desc.width = w;
    desc.height = h;
    auto in = backend.value()->create_texture(desc);
    auto out = backend.value()->create_texture(desc);
    LX_CHECK_OK(in.status());
    LX_CHECK_OK(out.status());
    LX_CHECK_OK(backend.value()->upload_texture(in.value(), source.data(), source.size()));

    // Push constants: exposure, contrast, saturation, intensity. Non-trivial
    // values so the pass does real work; the field order matches the shader's
    // uniform block on both backends.
    struct TonePush {
        float exposure_stops;
        float contrast;
        float saturation;
        float intensity;
    } push{0.5f, 0.2f, 1.3f, 1.0f};
    const Status ran = run_pass(backend.value().get(), kind, in.value(), out.value(), "tone",
                                &push, sizeof(push));
    LX_CHECK_OK(ran);
    if (!ran.ok()) return;

    std::vector<u8> gpu(source.size(), 0);
    LX_CHECK_OK(backend.value()->download_texture(out.value(), gpu.data(), gpu.size()));

    auto filter = filters::create_filter("tone");
    LX_CHECK_OK(filter.status());
    if (filter.ok()) {
        // Configure the CPU filter to the same parameters the shader used.
        auto* tone = dynamic_cast<filters::ToneFilter*>(filter.value().get());
        if (tone != nullptr) {
            tone->set_exposure_stops(push.exposure_stops);
            tone->set_contrast(push.contrast);
            tone->set_saturation(push.saturation);
        }
        std::vector<u8> cpu = source;
        filters::Image dst{cpu.data(), w, h};
        filters::ConstImage src{source.data(), w, h};
        filters::FilterParams params;
        params.intensity = push.intensity;
        LX_CHECK_OK(filter.value()->apply(src, dst, params));
        // Float rounding differs between the GPU and the CPU path; 2/255 is a
        // tight bound that still tolerates a single LSB of error.
        const int diff = max_abs_diff(gpu, cpu);
        LX_CHECK(diff <= 2);
    }
}

LX_TEST(gpu_backends, vignette_pass_matches_cpu_filter) {
    const BackendKind kind = host_gpu_backend();
    if (kind == BackendKind::Cpu) {
        LX_CHECK(true);
        return;
    }
    BackendCreateInfo info;
    info.preferred = kind;
    auto backend = create_backend(info);
    LX_CHECK_OK(backend.status());
    if (!backend.ok()) return;

    const u32 w = 64, h = 48;
    const auto source = make_frame(w, h);
    TextureDesc desc;
    desc.width = w;
    desc.height = h;
    auto in = backend.value()->create_texture(desc);
    auto out = backend.value()->create_texture(desc);
    LX_CHECK_OK(in.status());
    LX_CHECK_OK(out.status());
    LX_CHECK_OK(backend.value()->upload_texture(in.value(), source.data(), source.size()));

    struct VignettePush {
        float inner;
        float outer;
        float intensity;
        float pad0;
        float width;
        float height;
        float pad1;
        float pad2;
    } push{0.45f, 1.0f, 1.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 0.0f};
    const Status ran = run_pass(backend.value().get(), kind, in.value(), out.value(),
                                "vignette", &push, sizeof(push));
    LX_CHECK_OK(ran);
    if (!ran.ok()) return;

    std::vector<u8> gpu(source.size(), 0);
    LX_CHECK_OK(backend.value()->download_texture(out.value(), gpu.data(), gpu.size()));

    auto filter = filters::create_filter("vignette");
    LX_CHECK_OK(filter.status());
    if (filter.ok()) {
        std::vector<u8> cpu = source;
        filters::Image dst{cpu.data(), w, h};
        filters::ConstImage src{source.data(), w, h};
        filters::FilterParams params;
        params.intensity = push.intensity;
        LX_CHECK_OK(filter.value()->apply(src, dst, params));
        LX_CHECK(max_abs_diff(gpu, cpu) <= 2);
    }
}

LX_TEST(gpu_backends, blur_pass_matches_cpu_filter) {
    const BackendKind kind = host_gpu_backend();
    if (kind == BackendKind::Cpu) {
        LX_CHECK(true);
        return;
    }
    BackendCreateInfo info;
    info.preferred = kind;
    auto backend = create_backend(info);
    LX_CHECK_OK(backend.status());
    if (!backend.ok()) return;

    const u32 w = 64, h = 48;
    const auto source = make_frame(w, h);
    TextureDesc desc;
    desc.width = w;
    desc.height = h;
    auto in = backend.value()->create_texture(desc);
    auto mid = backend.value()->create_texture(desc);
    auto out = backend.value()->create_texture(desc);
    LX_CHECK_OK(in.status());
    LX_CHECK_OK(mid.status());
    LX_CHECK_OK(out.status());
    LX_CHECK_OK(backend.value()->upload_texture(in.value(), source.data(), source.size()));

    // The blur is separable: a horizontal pass then a vertical one, exactly as
    // the CPU filter does internally. sigma 2 => radius ceil(3*sigma) = 6.
    const float sigma = 2.0f;
    const int radius = static_cast<int>(std::ceil(3.0f * sigma));
    struct BlurPush {
        float sigma;
        float intensity;
        float width;
        float height;
        int axis;
        int radius;
        int pad0;
        int pad1;
    };
    BlurPush hpush{sigma, 1.0f, static_cast<float>(w), static_cast<float>(h), 0, radius, 0, 0};
    BlurPush vpush{sigma, 1.0f, static_cast<float>(w), static_cast<float>(h), 1, radius, 0, 0};
    LX_CHECK_OK(run_pass(backend.value().get(), kind, in.value(), mid.value(),
                         "blur_horizontal", &hpush, sizeof(hpush)));
    LX_CHECK_OK(run_pass(backend.value().get(), kind, mid.value(), out.value(),
                         "blur_vertical", &vpush, sizeof(vpush)));

    std::vector<u8> gpu(source.size(), 0);
    LX_CHECK_OK(backend.value()->download_texture(out.value(), gpu.data(), gpu.size()));

    auto filter = filters::create_filter("blur");
    LX_CHECK_OK(filter.status());
    if (filter.ok()) {
        std::vector<u8> cpu = source;
        filters::Image dst{cpu.data(), w, h};
        filters::ConstImage src{source.data(), w, h};
        filters::FilterParams params;
        params.intensity = 1.0f;
        LX_CHECK_OK(filter.value()->apply(src, dst, params));
        LX_CHECK(max_abs_diff(gpu, cpu) <= 2);
    }
}

LX_TEST(gpu_backends, sharpen_pass_matches_cpu_filter) {
    // The sharpen push block is the one whose fields are not naturally aligned
    // in HLSL: a float followed by a float2. HLSL packs a float2 at the next
    // 4-byte boundary while the C++/std430 struct puts it at offset 8, so an
    // un-padded declaration silently feeds the shader a shifted image_size. This
    // test pins the layout by comparing against the CPU filter.
    const BackendKind kind = host_gpu_backend();
    if (kind == BackendKind::Cpu) {
        LX_CHECK(true);
        return;
    }
    BackendCreateInfo info;
    info.preferred = kind;
    auto backend = create_backend(info);
    LX_CHECK_OK(backend.status());
    if (!backend.ok()) return;

    const u32 w = 64, h = 48;
    const auto source = make_frame(w, h);
    TextureDesc desc;
    desc.width = w;
    desc.height = h;
    auto in = backend.value()->create_texture(desc);
    auto out = backend.value()->create_texture(desc);
    LX_CHECK_OK(in.status());
    LX_CHECK_OK(out.status());
    LX_CHECK_OK(backend.value()->upload_texture(in.value(), source.data(), source.size()));

    struct SharpenPush {
        float strength;
        float pad0;
        float width;
        float height;
        float pad1;
        float pad2;
    } push{0.8f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 0.0f};
    const Status ran = run_pass(backend.value().get(), kind, in.value(), out.value(),
                                "sharpen", &push, sizeof(push));
    LX_CHECK_OK(ran);
    if (!ran.ok()) return;

    std::vector<u8> gpu(source.size(), 0);
    LX_CHECK_OK(backend.value()->download_texture(out.value(), gpu.data(), gpu.size()));

    auto filter = filters::create_filter("sharpen");
    LX_CHECK_OK(filter.status());
    if (filter.ok()) {
        std::vector<u8> cpu = source;
        filters::Image dst{cpu.data(), w, h};
        filters::ConstImage src{source.data(), w, h};
        filters::FilterParams params;
        params.intensity = 1.0f;
        LX_CHECK_OK(filter.value()->apply(src, dst, params));
        // A shifted image_size clamps the neighbourhood to the wrong extent, so
        // a layout error shows up as a large diff, not a rounding difference.
        LX_CHECK(max_abs_diff(gpu, cpu) <= 2);
    }
}

LX_TEST(gpu_backends, bloom_pass_matches_cpu_filter) {
    // Bloom is the one built-in whose GPU plan has four passes and two scratch
    // textures, so it exercises multi-pass ping-ponging as well as the bright
    // threshold. The threshold makes an exact CPU/GPU comparison fragile near
    // the cut-off: a single LSB of rounding can flip a pixel across it. The
    // frame below uses coarse luminance levels and the threshold sits in the
    // middle of a gap between them, so no pixel is close enough to flip.
    const BackendKind kind = host_gpu_backend();
    if (kind == BackendKind::Cpu) {
        LX_CHECK(true);
        return;
    }
    BackendCreateInfo info;
    info.preferred = kind;
    auto backend = create_backend(info);
    LX_CHECK_OK(backend.status());
    if (!backend.ok()) return;

    const u32 w = 64, h = 48;
    const float threshold = 200.0f;
    std::vector<u8> source(static_cast<std::size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * w + x) * 4;
            // Quarter-frame bright blocks at 255, the rest at 64. Luma is either
            // ~255 or ~64, both far from the 200 threshold.
            const bool bright = (x / 8 + y / 8) % 2 == 0;
            const u8 level = bright ? 255 : 64;
            source[o + 0] = level;
            source[o + 1] = level;
            source[o + 2] = level;
            source[o + 3] = 255;
        }
    }

    TextureDesc desc;
    desc.width = w;
    desc.height = h;
    auto in = backend.value()->create_texture(desc);
    auto out = backend.value()->create_texture(desc);
    auto glow_a = backend.value()->create_texture(desc);
    auto glow_b = backend.value()->create_texture(desc);
    LX_CHECK_OK(in.status());
    LX_CHECK_OK(out.status());
    LX_CHECK_OK(glow_a.status());
    LX_CHECK_OK(glow_b.status());
    if (!in.ok() || !out.ok() || !glow_a.ok() || !glow_b.ok()) return;
    LX_CHECK_OK(backend.value()->upload_texture(in.value(), source.data(), source.size()));

    // radius_ default 4.0f, quality "balanced" => radius 4.
    const int radius = 4;
    struct BloomBrightPush {
        float threshold;
        float pad0;
        float pad1;
        float pad2;
    } bright{threshold, 0.0f, 0.0f, 0.0f};
    struct BloomBlurPush {
        int radius;
        int axis;
        float width;
        float height;
    } blur_h{radius, 0, static_cast<float>(w), static_cast<float>(h)};
    BloomBlurPush blur_v = blur_h;
    blur_v.axis = 1;
    struct BloomCombinePush {
        float intensity;
        float pad0;
        float pad1;
        float pad2;
    } combine{1.0f, 0.0f, 0.0f, 0.0f};

    // bright(in -> glow_a), blur_h(glow_a -> glow_b), blur_v(glow_b -> glow_a),
    // combine(in + glow_a -> out).
    LX_CHECK_OK(run_pass(backend.value().get(), kind, in.value(), glow_a.value(),
                         "bloom_bright", &bright, sizeof(bright)));
    LX_CHECK_OK(run_pass(backend.value().get(), kind, glow_a.value(), glow_b.value(),
                         "bloom_blur_horizontal", &blur_h, sizeof(blur_h)));
    LX_CHECK_OK(run_pass(backend.value().get(), kind, glow_b.value(), glow_a.value(),
                         "bloom_blur_vertical", &blur_v, sizeof(blur_v)));

    // The combine pass reads two textures; run_pass only binds one, so bind the
    // glow explicitly through an encoder here.
    {
        const char* source_text = builtin_source_for(kind, "bloom_combine");
        LX_CHECK(source_text != nullptr);
        if (source_text == nullptr) return;
        LX_CHECK_OK(backend.value()->register_shader_source("bloom_combine", source_text));
        LX_CHECK_OK(backend.value()->create_pipeline("bloom_combine_test", "bloom_combine"));
        auto encoder = backend.value()->begin_commands();
        LX_CHECK_OK(encoder.status());
        if (!encoder.ok()) return;
        LX_CHECK_OK(encoder.value()->bind_pipeline("bloom_combine_test"));
        LX_CHECK_OK(encoder.value()->bind_texture(0, in.value()));
        LX_CHECK_OK(encoder.value()->bind_texture(1, glow_a.value()));
        LX_CHECK_OK(encoder.value()->bind_storage_texture(0, out.value()));
        LX_CHECK_OK(encoder.value()->set_push_constants(&combine, sizeof(combine)));
        DispatchDesc dispatch;
        dispatch.kernel_name = "bloom_combine";
        LX_CHECK_OK(encoder.value()->dispatch(dispatch));
        LX_CHECK_OK(backend.value()->submit(encoder.value().get()));
        LX_CHECK_OK(backend.value()->wait_idle());
    }

    std::vector<u8> gpu(source.size(), 0);
    LX_CHECK_OK(backend.value()->download_texture(out.value(), gpu.data(), gpu.size()));

    auto filter = filters::create_filter("bloom");
    LX_CHECK_OK(filter.status());
    if (filter.ok()) {
        auto* bloom = dynamic_cast<filters::BloomFilter*>(filter.value().get());
        LX_CHECK(bloom != nullptr);
        if (bloom != nullptr) bloom->set_threshold(threshold);
        std::vector<u8> cpu = source;
        filters::Image dst{cpu.data(), w, h};
        filters::ConstImage src{source.data(), w, h};
        filters::FilterParams params;
        params.intensity = combine.intensity;
        LX_CHECK_OK(filter.value()->apply(src, dst, params));
        const int diff = max_abs_diff(gpu, cpu);
        // Box blur and the bright pass are integer-weighted on both paths, so
        // the only room for error is float rounding in the combine.
        LX_CHECK(diff <= 2);
    }
}

LX_TEST(gpu_backends, full_renderer_filter_chain_runs_on_gpu) {
    // End-to-end: a Renderer on the host GPU backend must apply a real filter
    // chain and produce output that matches the CPU chain. This is the test that
    // proves the GPU filter path is wired, not just the individual passes; each
    // pass is pinned separately by its own test above, including bloom.
    const BackendKind kind = host_gpu_backend();
    if (kind == BackendKind::Cpu) {
        LX_CHECK(true);  // no GPU API on this host
        return;
    }
    const u32 w = 64, h = 48;
    const auto source = make_frame(w, h);

    renderer::Renderer renderer;
    renderer::RendererDesc desc;
    desc.width = w;
    desc.height = h;
    desc.backend.preferred = kind;
    desc.enable_filters = true;
    LX_CHECK_OK(renderer.initialize(desc));
    if (!renderer.initialized()) return;

    // tone + vignette + blur + sharpen all have exact GPU plans; bloom is left
    // out because its threshold makes an exact comparison against the CPU
    // sensitive to a single LSB upstream (a rounding difference flips a pixel
    // across the threshold). The individual bloom passes are covered above.
    std::vector<std::pair<std::string, filters::FilterParams>> chain;
    { filters::FilterParams p; p.intensity = 1.0f; chain.push_back({"tone", p}); }
    { filters::FilterParams p; p.intensity = 0.7f; chain.push_back({"vignette", p}); }
    { filters::FilterParams p; p.intensity = 1.0f; chain.push_back({"blur", p}); }
    { filters::FilterParams p; p.intensity = 1.0f; chain.push_back({"sharpen", p}); }
    LX_CHECK_OK(renderer.configure_filters(chain));
    LX_CHECK(renderer.can_run_filters_on_gpu(filters::ContentScope::Unknown));

    renderer::FrameInput input;
    input.pixels = source.data();
    input.bytes = source.size();
    renderer::FrameStats stats;
    LX_CHECK_OK(renderer.render_frame(input, &stats));
    LX_CHECK(stats.filter_time_micros() > 0);

    std::vector<u8> gpu(source.size(), 0);
    LX_CHECK_OK(renderer.readback(gpu.data(), gpu.size()));
    // The chain changed the frame.
    LX_CHECK(gpu != source);

    // Compare against the same chain on the CPU.
    filters::FilterChain cpu_chain;
    for (const auto& entry : chain) {
        auto filter = filters::create_filter(entry.first);
        LX_CHECK_OK(filter.status());
        if (!filter.ok()) return;
        LX_CHECK_OK(cpu_chain.add(std::move(filter.value()), entry.second));
    }
    std::vector<u8> cpu = source;
    filters::Image image{cpu.data(), w, h};
    memory::BufferPool pool;
    LX_CHECK_OK(cpu_chain.run(image, pool, nullptr));
    LX_CHECK(max_abs_diff(gpu, cpu) <= 5);

    renderer.shutdown();
}

LX_TEST(gpu_backends, preferred_device_selects_or_falls_back_honestly) {
    // preferred_device used to be copied into BackendCreateInfo and never read,
    // so a config asking for one card silently got whichever was enumerated
    // first. The Vulkan backend now matches it against the device name and, when
    // nothing matches, keeps working on the default device after saying so.
#if defined(LX_HAS_VULKAN)
    backends::BackendCreateInfo matching;
    matching.preferred = backends::BackendKind::Vulkan;
    // Use the name of a device that actually exists here: query the default
    // first, then ask for part of its own name.
    std::string actual;
    {
        backends::BackendCreateInfo any;
        any.preferred = backends::BackendKind::Vulkan;
        auto default_backend = backends::create_backend(any);
        if (!default_backend.ok()) {
            LX_CHECK(true);  // no Vulkan device on this machine; nothing to select
            return;
        }
        actual = default_backend.value()->info().device_name;
    }  // release the device before creating another
    matching.preferred_device = actual;
    auto chosen = backends::create_backend(matching);
    LX_CHECK(chosen.ok());
    if (chosen.ok()) {
        LX_CHECK_EQ(chosen.value()->info().device_name, actual);
    }

    // A preference nothing matches must not remove the GPU: it is reported and
    // the default device is used.
    backends::BackendCreateInfo unmatched;
    unmatched.preferred = backends::BackendKind::Vulkan;
    unmatched.preferred_device = "definitely not a real gpu 99999";
    auto fallback = backends::create_backend(unmatched);
    LX_CHECK(fallback.ok());
    if (fallback.ok()) {
        LX_CHECK_EQ(fallback.value()->info().device_name, actual);
    }
#endif
}

LX_TEST(gpu_backends, compute_only_device_is_not_selected_for_rendering) {
    // Regression: with this project's own ICD installed system-wide, the loader
    // enumerated a compute-only device whose limits are all zero. The backend
    // picked it (it had a compute queue) and then crashed building a pipeline
    // layout the device rejects. Device selection must skip it instead.
#if defined(LX_HAS_VULKAN)
    backends::BackendCreateInfo info;
    info.preferred = backends::BackendKind::Vulkan;
    auto backend = backends::create_backend(info);
    if (!backend.ok()) {
        LX_CHECK(true);  // nothing usable on this host; the skip path is untested
        return;
    }
    const std::string chosen = backend.value()->info().device_name;
    LX_CHECK(chosen.find("LoucuraXXXX GPU (virtual") == std::string::npos);
#endif
}

LX_TEST_MAIN("gpu_backends")
