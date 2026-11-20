// LoucuraXXXX GPU - benchmark harness.
//
// Compares the three cases the project cares about, using only measured data:
//   1. the pipeline with the virtual GPU layer disabled (baseline),
//   2. the pipeline with the virtual GPU layer enabled,
//   3. different RAM and CPU-core reservations.
//
// Every number printed comes from measure() over real work. Nothing is
// extrapolated or assumed.
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "backends/backend_registry.hpp"
#include "bench_util.hpp"
#include "configurator/configurator.hpp"
#include "core/version.hpp"
#include "filters/filter_chain.hpp"
#include "filters/filter_registry.hpp"
#include "hardware/hardware_info.hpp"
#include "manager/manager.hpp"
#include "memory/buffer_pool.hpp"
#include "memory/memory_budget.hpp"
#include "renderer/renderer.hpp"
#include "scheduler/cost_model.hpp"
#include "scheduler/thread_pool.hpp"

using namespace lx;
using namespace lx::benchmarks;

namespace {

std::vector<u8> make_gradient(u32 width, u32 height) {
    std::vector<u8> pixels(static_cast<std::size_t>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[o + 0] = static_cast<u8>(x * 255 / std::max(1u, width - 1));
            pixels[o + 1] = static_cast<u8>(y * 255 / std::max(1u, height - 1));
            pixels[o + 2] = static_cast<u8>((x + y) % 256);
            pixels[o + 3] = 255;
        }
    }
    return pixels;
}

void print_environment() {
    auto hardware = hardware::detect_hardware();
    if (!hardware.ok()) {
        std::printf("hardware detection failed: %s\n", hardware.status().message().c_str());
        return;
    }
    const auto& hw = hardware.value();
    std::printf("== %s benchmark ==\n", lx::version_string().c_str());
    std::printf("host: %s | %u logical cores | %llu MiB RAM | %s %s\n\n",
                hw.cpu.model.c_str(), hw.cpu.logical_cores,
                static_cast<unsigned long long>(hw.memory.total_bytes / kMiB),
                hw.os.name.c_str(), hw.os.architecture.c_str());
}

// Baseline: a filter chain run directly on host memory, with no virtual GPU
// layer, no manager, and no backend indirection. This is what an application
// would do today without LoucuraXXXX GPU.
//
// `worker_count` must match the reservation used for the virtual GPU case.
// Running the baseline serially against a 2-worker virtual GPU measured the
// extra core, not the layer: 29.2 ms against 15.3 ms on this host, which reads
// as a 2x win for the layer when the only difference was parallelism the
// baseline was denied. Pass the same thread count to both, so the difference is
// the layer itself.
void bench_baseline(u32 width, u32 height, int iterations, u32 worker_count) {
    auto image = make_gradient(width, height);
    const u64 pixels = static_cast<u64>(width) * height;

    filters::FilterChain chain;
    memory::BufferPool pool;
    for (const char* name : {"tone", "blur", "vignette"}) {
        auto filter = filters::create_filter(name);
        if (!filter.ok()) continue;
        filters::FilterParams params;
        params.intensity = 0.8f;
        chain.add(std::move(filter.value()), params);
    }

    // Same thread count as the virtual GPU case, so the comparison isolates the
    // layer rather than the core count. worker_count == 0 runs serially.
    std::unique_ptr<scheduler::ThreadPool> threads;
    if (worker_count > 0) {
        scheduler::ThreadPoolConfig config;
        config.thread_count = worker_count;
        threads = std::make_unique<scheduler::ThreadPool>(config);
        threads->start();
    }

    filters::Image view{image.data(), width, height};
    const auto samples = measure(iterations, 3, [&] {
        chain.run(view, pool, threads.get(), nullptr);
    });
    print_summary("baseline: direct filter chain (" + std::to_string(worker_count) +
                      " cores)",
                  summarize(samples), pixels);
    if (threads) threads->stop();
}

// With the virtual GPU layer: the same filters, but driven through the renderer
// and the CPU backend, including upload and readback of the target texture. The
// renderer owns a filter pool of `reserved_cores` workers, so the reserved cores
// genuinely affect the frame.
void bench_virtual_gpu(u32 width, u32 height, int iterations,
                       backends::BackendKind backend_kind, u32 reserved_cores,
                       bool is_hardware = true) {
    auto image = make_gradient(width, height);

    renderer::RendererDesc desc;
    desc.width = width;
    desc.height = height;
    desc.backend.preferred = backend_kind;
    desc.shader_asset_root = "shaders";
    desc.cpu_worker_count = reserved_cores;

    renderer::Renderer renderer;
    if (!renderer.initialize(desc).ok()) {
        std::printf("virtual gpu: backend '%s' unavailable, skipping\n",
                    backends::to_string(backend_kind));
        return;
    }
    renderer.configure_filters({{"tone", {}}, {"blur", {}}, {"vignette", {}}});

    renderer::FrameInput input;
    input.pixels = image.data();
    input.bytes = image.size();

    const auto samples = measure(iterations, 3, [&] {
        renderer.render_frame(input, nullptr);
    });

    const std::string label = std::string("virtual gpu: ") +
                              backends::to_string(backend_kind) + " (" +
                              std::to_string(reserved_cores) + " cores)" +
                              (is_hardware ? "" : " [software rasterizer]");
    print_summary(label, summarize(samples), static_cast<u64>(width) * height);
}

// Reservation sweep: measures a filter pass at several thread counts to show
// how the CPU contribution scales with the cores actually granted. Runs the
// same chain twice, once with no pool (serial) and once with the pool, so the
// parallel speedup is visible rather than assumed.
void bench_core_sweep(u32 width, u32 height, int iterations) {
    auto image = make_gradient(width, height);
    const u32 max_cores = hardware::logical_processor_count();

    filters::FilterChain serial_chain;
    memory::BufferPool serial_pool;
    for (const char* name : {"tone", "blur", "vignette"}) {
        auto filter = filters::create_filter(name);
        if (filter.ok()) serial_chain.add(std::move(filter.value()), {});
    }
    filters::Image serial_view{image.data(), width, height};
    const auto serial_samples = measure(iterations, 2, [&] {
        serial_chain.run(serial_view, serial_pool, nullptr, nullptr);
    });
    print_summary("reservation: 0 core(s) (serial)", summarize(serial_samples),
                  static_cast<u64>(width) * height);

    for (u32 cores = 1; cores <= max_cores; cores *= 2) {
        scheduler::ThreadPoolConfig config;
        config.thread_count = cores;
        scheduler::ThreadPool pool(config);
        pool.start();

        filters::FilterChain chain;
        memory::BufferPool pool_buffers;
        for (const char* name : {"tone", "blur", "vignette"}) {
            auto filter = filters::create_filter(name);
            if (filter.ok()) chain.add(std::move(filter.value()), {});
        }
        filters::Image view{image.data(), width, height};

        const auto samples = measure(iterations, 2, [&] {
            chain.run(view, pool_buffers, &pool, nullptr);
        });
        const std::string label = "reservation: " + std::to_string(cores) + " core(s)";
        print_summary(label, summarize(samples), static_cast<u64>(width) * height);
        pool.stop();
    }
}

// Cost-model behaviour across working-set sizes, so the placement decisions can
// be inspected rather than trusted.
void bench_cost_model() {
    scheduler::CostModel model;
    std::printf("\ncost model placement (default constants):\n");
    for (u64 bytes : {static_cast<u64>(4 * kKiB), static_cast<u64>(1 * kMiB),
                      static_cast<u64>(64 * kMiB), static_cast<u64>(512 * kMiB)}) {
        scheduler::DispatchRequest request;
        request.working_set_bytes = bytes;
        request.output_bytes = bytes;
        request.independent_units = 64;
        const auto decision = model.decide(request);
        std::printf("  %8llu KiB working set -> %-5s (cpu=%.3f ms, gpu=%.3f ms)\n",
                    static_cast<unsigned long long>(bytes / 1024),
                    scheduler::to_string(decision.target),
                    decision.estimated_cpu_seconds * 1e3,
                    decision.estimated_gpu_seconds * 1e3);
    }
}

}  // namespace

namespace {

// Rejects malformed numeric arguments instead of letting std::stoi/stoul throw
// and terminate the process.
bool parse_u32_arg(const char* text, u32& out) {
    if (text == nullptr || *text == '\0') return false;
    std::size_t consumed = 0;
    try {
        const unsigned long value = std::stoul(text, &consumed, 10);
        if (consumed != std::strlen(text) || value > std::numeric_limits<u32>::max()) {
            return false;
        }
        out = static_cast<u32>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    int iterations = 30;
    u32 width = 640, height = 480;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc) {
            u32 value = 0;
            if (!parse_u32_arg(argv[++i], value) || value == 0) {
                std::fprintf(stderr, "invalid --iterations value\n");
                return 2;
            }
            iterations = static_cast<int>(value);
        } else if (arg == "--width" && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], width) || width == 0) {
                std::fprintf(stderr, "invalid --width value\n");
                return 2;
            }
        } else if (arg == "--height" && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], height) || height == 0) {
                std::fprintf(stderr, "invalid --height value\n");
                return 2;
            }
        } else if (arg == "--help") {
            std::printf("usage: lx_bench [--width N] [--height N] [--iterations N]\n");
            return 0;
        }
    }

    print_environment();
    std::printf("workload: %ux%u RGBA, %d iterations (3 warmup)\n\n", width, height,
                iterations);

    // Both cases get the same reservation so the comparison isolates the layer.
    constexpr u32 kCompareCores = 2;
    bench_baseline(width, height, iterations, kCompareCores);
    bench_virtual_gpu(width, height, iterations, backends::BackendKind::Cpu, kCompareCores);
    // The same workload through every GPU backend the host offers, so the GPU
    // path is compared against the CPU path on measured frames rather than
    // described. A backend that is not compiled in or not usable is skipped, and
    // one that is present only as a software rasterizer is labelled as such: its
    // timings say nothing about GPU acceleration on this host.
    for (const auto& reg : backends::probe_available_backends()) {
        if (reg.kind == backends::BackendKind::Cpu) continue;
        bench_virtual_gpu(width, height, iterations, reg.kind, kCompareCores, reg.is_hardware);
    }
    bench_core_sweep(width, height, iterations);
    bench_cost_model();

    std::printf("\nnote: a GPU backend appears above only when one is compiled in and "
                "usable on this host.\n");
    return 0;
}
