// Tests for the backend interface using the CPU reference backend.
#include <cstring>
#include <vector>

#include "backends/backend_registry.hpp"
#include "backends/cpu/cpu_backend.hpp"
#include "backends/device_select.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {
backends::BackendCreateInfo cpu_info() {
    backends::BackendCreateInfo info;
    info.preferred = backends::BackendKind::Cpu;
    return info;
}
}  // namespace

LX_TEST(backends, cpu_backend_is_registered) {
    const auto* registration = backends::find_backend(backends::BackendKind::Cpu);
    LX_CHECK(registration != nullptr);
    if (registration) {
        const auto probe = registration->probe();
        LX_CHECK(probe.available);
    }
}

LX_TEST(backends, create_texture_and_query_size) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());

    backends::TextureDesc desc;
    desc.width = 64;
    desc.height = 48;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    auto* cpu = static_cast<backends::CpuBackend*>(backend.value().get());
    u32 width = 0, height = 0;
    u8* pixels = cpu->texture_data(texture.value(), &width, &height);
    LX_CHECK(pixels != nullptr);
    LX_CHECK_EQ(width, 64u);
    LX_CHECK_EQ(height, 48u);
    LX_CHECK_EQ(backend.value()->allocated_bytes(), static_cast<u64>(64 * 48 * 4));
}

LX_TEST(backends, rejects_zero_size_texture) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    backends::TextureDesc desc;
    desc.width = 0;
    desc.height = 10;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK(!texture.ok());
    LX_CHECK(texture.status().code() == ErrorCode::InvalidArgument);
}

LX_TEST(backends, upload_and_download_roundtrip) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    backends::TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    std::vector<u8> source(16 * 16 * 4);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<u8>(i & 0xFF);
    LX_CHECK_OK(backend.value()->upload_texture(texture.value(), source.data(), source.size()));

    std::vector<u8> downloaded(source.size(), 0);
    auto written = backend.value()->download_texture(texture.value(), downloaded.data(),
                                                     downloaded.size());
    LX_CHECK_OK(written.status());
    LX_CHECK_EQ(written.value(), static_cast<u64>(source.size()));
    LX_CHECK(source == downloaded);
}

LX_TEST(backends, upload_rejects_undersized_buffer) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    backends::TextureDesc desc;
    desc.width = 32;
    desc.height = 32;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    std::vector<u8> too_small(100, 0);
    const Status status =
        backend.value()->upload_texture(texture.value(), too_small.data(), too_small.size());
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);
}

LX_TEST(backends, upload_and_download_reject_absurd_pitch) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    backends::TextureDesc desc;
    desc.width = 8;
    desc.height = 4;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    // A pitch so large that pitch*(height-1) would wrap must be rejected before
    // it can turn a small buffer into an apparently sufficient one.
    const u64 absurd = ~0ull;
    std::vector<u8> buffer(8 * 4 * 4, 0);
    const Status up = backend.value()->upload_texture(texture.value(), buffer.data(),
                                                      buffer.size(), absurd);
    LX_CHECK(!up.ok());
    LX_CHECK(up.code() == ErrorCode::InvalidArgument);
    const auto down = backend.value()->download_texture(texture.value(), buffer.data(),
                                                        buffer.size(), absurd);
    LX_CHECK(!down.ok());
    LX_CHECK(down.status().code() == ErrorCode::InvalidArgument);
}

LX_TEST(backends, upload_honours_row_pitch) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    backends::TextureDesc desc;
    desc.width = 4;
    desc.height = 2;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    // Rows padded to 32 bytes; the extra bytes must be ignored.
    const u64 pitch = 32;
    std::vector<u8> padded(pitch * 2, 0xEE);
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            padded[y * pitch + x * 4] = static_cast<u8>(10 + x + y * 10);
        }
    }
    LX_CHECK_OK(backend.value()->upload_texture(texture.value(), padded.data(), padded.size(),
                                                pitch));
    auto* cpu = static_cast<backends::CpuBackend*>(backend.value().get());
    u8* pixels = cpu->texture_data(texture.value());
    LX_CHECK(pixels != nullptr);
    LX_CHECK_EQ(pixels[0], static_cast<u8>(10));
    LX_CHECK_EQ(pixels[4], static_cast<u8>(11));
    LX_CHECK_EQ(pixels[16], static_cast<u8>(20));  // second row, first pixel
}

LX_TEST(backends, cpu_kernel_dispatch) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());

    // Register a kernel that fills every byte of the first bound texture.
    struct Context {
        int calls = 0;
    };
    static Context context;
    auto kernel = [](const void*, u32, const std::vector<backends::ResourceHandle>& textures,
                     void* user_data) -> Status {
        auto* ctx = static_cast<Context*>(user_data);
        if (textures.empty()) {
            return Status::error(ErrorCode::InvalidArgument, "no texture bound");
        }
        ctx->calls++;
        return Status::success();
    };
    LX_CHECK_OK(backend.value()->register_cpu_kernel(
        "fill", backends::CpuKernel(kernel, &context)));
    LX_CHECK_OK(backend.value()->create_pipeline("fill_pipeline", "fill"));

    backends::TextureDesc desc;
    desc.width = 8;
    desc.height = 8;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    auto encoder = backend.value()->begin_commands();
    LX_CHECK_OK(encoder.status());
    LX_CHECK_OK(encoder.value()->bind_pipeline("fill_pipeline"));
    LX_CHECK_OK(encoder.value()->bind_texture(0, texture.value()));
    backends::DispatchDesc dispatch;
    dispatch.kernel_name = "fill";
    LX_CHECK_OK(encoder.value()->dispatch(dispatch));
    LX_CHECK_EQ(context.calls, 1);
}

LX_TEST(backends, unknown_pipeline_reports_error) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    auto encoder = backend.value()->begin_commands();
    LX_CHECK_OK(encoder.status());
    const Status status = encoder.value()->bind_pipeline("no_such_pipeline");
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);
}

LX_TEST(backends, cpu_backend_reports_unified_memory_and_host_mapping) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    // The CPU backend allocates in host memory, so it is unified by definition
    // and can hand out a pointer to a texture.
    LX_CHECK(backend.value()->capabilities().unified_memory);
    LX_CHECK(backend.value()->capabilities().supports_host_mapping);
}

LX_TEST(backends, mapped_texture_is_the_texture_storage) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());

    backends::TextureDesc desc;
    desc.width = 8;
    desc.height = 4;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    auto mapped = backend.value()->map_texture(texture.value());
    LX_CHECK_OK(mapped.status());
    LX_CHECK(mapped.value().valid());
    LX_CHECK_EQ(mapped.value().width, 8u);
    LX_CHECK_EQ(mapped.value().height, 4u);
    LX_CHECK_EQ(mapped.value().row_pitch, static_cast<u64>(8 * 4));

    // Writing through the mapping must be visible to a download, proving the
    // pointer really is the texture's storage and not a copy.
    auto* pixels = static_cast<u8*>(mapped.value().data);
    std::memset(pixels, 0xAB, mapped.value().row_pitch * mapped.value().height);
    LX_CHECK_OK(backend.value()->unmap_texture(texture.value()));

    std::vector<u8> out(8 * 4 * 4, 0);
    auto downloaded = backend.value()->download_texture(texture.value(), out.data(), out.size());
    LX_CHECK_OK(downloaded.status());
    LX_CHECK_EQ(out[0], static_cast<u8>(0xAB));
    LX_CHECK_EQ(out[out.size() - 1], static_cast<u8>(0xAB));
}

LX_TEST(backends, mapping_unknown_texture_reports_error) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    auto mapped = backend.value()->map_texture(backends::ResourceHandle{9999});
    LX_CHECK(!mapped.ok());
    LX_CHECK(mapped.status().code() == ErrorCode::InvalidArgument);
}

LX_TEST(backends, shader_source_is_refused_by_cpu_backend) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    // The CPU backend runs host kernels, not shader source; it must say so
    // rather than pretend to compile.
    auto status = backend.value()->register_shader_source("noop", "void main() {}");
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::NotSupported);
}

LX_TEST(backends, unavailable_backend_reports_cleanly) {
    // Pick a GPU backend kind that this build did not compile in; creating it
    // must fail cleanly with BackendUnavailable rather than crash or silently
    // succeed. Which kinds are absent depends on the build configuration, so the
    // test finds one instead of hard-coding it.
    const backends::BackendKind candidates[] = {
        backends::BackendKind::Vulkan,
        backends::BackendKind::Metal,
        backends::BackendKind::Direct3D11,
    };
    bool found = false;
    backends::BackendKind absent = backends::BackendKind::Cpu;
    for (backends::BackendKind kind : candidates) {
        if (backends::find_backend(kind) == nullptr) {
            absent = kind;
            found = true;
            break;
        }
    }
    if (!found) {
        // Every GPU backend is compiled in; there is nothing absent to check.
        LX_CHECK(true);
        return;
    }
    backends::BackendCreateInfo info;
    info.preferred = absent;
    auto backend = backends::create_backend(info);
    LX_CHECK(!backend.ok());
    LX_CHECK(backend.status().code() == ErrorCode::BackendUnavailable);
}

LX_TEST(backends, download_rejects_pitch_smaller_than_a_row) {
    auto backend = backends::create_backend(cpu_info());
    LX_CHECK_OK(backend.status());
    backends::TextureDesc desc;
    desc.width = 8;
    desc.height = 4;
    auto texture = backend.value()->create_texture(desc);
    LX_CHECK_OK(texture.status());

    // A 4-pixel pitch on an 8-pixel-wide texture would make the per-row writes
    // overlap; download must reject it, just as upload does.
    std::vector<u8> out(8 * 4 * 4, 0);
    auto written = backend.value()->download_texture(texture.value(), out.data(), out.size(),
                                                     /*row_pitch=*/16);
    LX_CHECK(!written.ok());

    // A correctly sized pitch is accepted.
    written = backend.value()->download_texture(texture.value(), out.data(), out.size(),
                                                /*row_pitch=*/32);
    LX_CHECK_OK(written.status());
    backend.value()->destroy_texture(texture.value());
}

LX_TEST(backends, texture_byte_size_does_not_wrap_32_bits) {
    // Regression: the byte count used to be computed as width*height*bpp in
    // 32-bit arithmetic, which wraps for large frames. Compute it in 64 bits
    // and verify against the exact expected value (no allocation involved).
    const u64 expected = static_cast<u64>(65535) * 65535 * 4;
    LX_CHECK_EQ(backends::texture_byte_size(65535, 65535, backends::PixelFormat::RGBA8Unorm),
                expected);
    // The wrapped 32-bit result would be much smaller; make sure we are not
    // returning it.
    const u64 wrapped = static_cast<u64>(
        static_cast<u32>(65535u * 65535u * 4u));
    LX_CHECK(backends::texture_byte_size(65535, 65535,
                                         backends::PixelFormat::RGBA8Unorm) != wrapped);
    // Small sizes are unaffected.
    LX_CHECK_EQ(backends::texture_byte_size(64, 48, backends::PixelFormat::RGBA8Unorm),
                static_cast<u64>(64 * 48 * 4));
}

LX_TEST(backends, preferred_device_selects_by_case_insensitive_substring) {
    // A multi-device machine is where a preference has to do work. This is the
    // exact rule the Vulkan backend applies to its compute-capable devices.
    const std::vector<std::string> devices = {
        "llvmpipe (LLVM 15.0.7, 256 bits)",
        "NVIDIA GeForce RTX 4090",
        "Intel(R) UHD Graphics 770",
    };
    bool matched = false;

    // Substring, any case.
    LX_CHECK_EQ(backends::select_device(devices, "4090", &matched), std::size_t{1});
    LX_CHECK(matched);
    LX_CHECK_EQ(backends::select_device(devices, "geforce", &matched), std::size_t{1});
    LX_CHECK(matched);
    LX_CHECK_EQ(backends::select_device(devices, "INTEL", &matched), std::size_t{2});
    LX_CHECK(matched);

    // No preference: the first device, and that is not a failed match.
    LX_CHECK_EQ(backends::select_device(devices, "", &matched), std::size_t{0});
    LX_CHECK(matched);

    // Nothing matches: fall back to the first device and report the miss, so the
    // caller can warn instead of claiming the preference was honoured.
    LX_CHECK_EQ(backends::select_device(devices, "RTX 5090", &matched), std::size_t{0});
    LX_CHECK(!matched);

    // An empty enumeration has nothing to select; index 0 with no match.
    LX_CHECK_EQ(backends::select_device({}, "4090", &matched), std::size_t{0});
    LX_CHECK(!matched);
}

LX_TEST_MAIN("backends")
