// LoucuraXXXX GPU - command line front end.
//
// Exposes the pieces a user needs to drive the virtual GPU without writing
// code: detect the host, validate and persist a configuration, list the
// backends and filters that are actually available, and run a short render
// through the pipeline to confirm it works.
#include <cstring>
#include <exception>
#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "backends/backend_registry.hpp"
#include "configurator/configurator.hpp"
#include "core/version.hpp"
#include "filters/filter_registry.hpp"
#include "renderer/renderer.hpp"

namespace {

// Parses a non-negative integer argument, rejecting anything else. Using
// std::stoul directly would throw on malformed input and terminate the process
// with an unhandled exception.
bool parse_u64(const char* text, lx::u64& out) {
    if (text == nullptr || *text == '\0') return false;
    std::size_t consumed = 0;
    try {
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (consumed != std::strlen(text)) return false;
        out = static_cast<lx::u64>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_u32(const char* text, lx::u32& out) {
    lx::u64 value = 0;
    if (!parse_u64(text, value)) return false;
    if (value > std::numeric_limits<lx::u32>::max()) return false;
    out = static_cast<lx::u32>(value);
    return true;
}

// Converts a MiB count to bytes, rejecting a value whose scaling would wrap
// u64 (which would otherwise pass a small, unsafe reservation downstream).
bool mib_to_bytes(const char* text, lx::u64& out) {
    lx::u64 mib = 0;
    if (!parse_u64(text, mib)) return false;
    if (mib > std::numeric_limits<lx::u64>::max() / lx::kMiB) return false;
    out = mib * lx::kMiB;
    return true;
}

void print_usage() {
    std::cout
        << lx::version_string() << " ("
        << lx::to_string(lx::kMaturity) << ")\n\n"
        << "Usage: lx_cli <command> [options]\n\n"
        << "Commands:\n"
        << "  info                 Detect hardware and list usable backends\n"
        << "  filters              List registered filters\n"
        << "  validate [--ram-mib N] [--cores N] [--backend NAME]\n"
        << "                       Validate a configuration against this host\n"
        << "  render [--width N] [--height N] [--frames N] [--filter NAME[,NAME...]]\n"
        << "         [--cores N] [--backend NAME] [--target-fps N]\n"
        << "                       Run frames through the render pipeline\n"
        << "                       --filter takes one name or a comma-separated chain\n"
        << "                       (default backend: best probed on this host)\n"
        << "  bench --help         See lx_bench for reproducible benchmarks\n";
}

int cmd_info() {
    auto hardware = lx::hardware::detect_hardware();
    if (!hardware.ok()) {
        std::cerr << "hardware detection failed: " << hardware.status().message() << "\n";
        return 1;
    }
    const auto& hw = hardware.value();
    std::cout << "CPU: " << hw.cpu.model << "\n";
    std::cout << "  logical cores: " << hw.cpu.logical_cores
              << ", physical cores: " << hw.cpu.physical_cores << "\n";
    if (hw.is_hybrid_cpu()) {
        std::cout << "  core classes: " << hw.cpu.performance_cores << " performance, "
                  << hw.cpu.efficiency_cores << " efficiency (hybrid)\n";
    }
    std::cout << "  AVX2: " << (hw.cpu.supports_avx2 ? "yes" : "no")
              << ", AVX512: " << (hw.cpu.supports_avx512 ? "yes" : "no")
              << ", NEON: " << (hw.cpu.supports_neon ? "yes" : "no") << "\n";
    std::cout << "Memory: " << (hw.memory.total_bytes / lx::kMiB) << " MiB total, "
              << (hw.memory.available_bytes / lx::kMiB) << " MiB available\n";
    std::cout << "OS: " << hw.os.name << " " << hw.os.version << " (" << hw.os.architecture
              << ")\n";
    std::cout << "Memory topology: " << lx::hardware::to_string(hw.memory_topology());
    if (hw.has_unified_memory()) {
        std::cout << " (CPU and GPU share one pool: no host<->device copies needed)";
    }
    std::cout << "\n";

    std::cout << "\nGPUs detected by the OS:\n";
    if (hw.gpus.empty()) {
        std::cout << "  (none)\n";
    }
    for (const auto& gpu : hw.gpus) {
        std::cout << "  - " << gpu.name << " [" << lx::hardware::to_string(gpu.kind) << ", "
                  << lx::hardware::to_string(gpu.api)
                  << (gpu.unified_memory ? ", unified memory" : "") << "]\n";
    }

    std::cout << "\nUsable backends (probed):\n";
    for (const auto& reg : lx::backends::probe_available_backends()) {
        std::cout << "  - " << reg.name << " (" << lx::to_string(reg.maturity) << ")";
        if (!reg.is_hardware && reg.kind != lx::backends::BackendKind::Cpu) {
            std::cout << " [software rasterizer: no GPU acceleration]";
        }
        std::cout << "\n      device: " << reg.device_name << "\n";
        if (!reg.reason.empty()) std::cout << "      " << reg.reason << "\n";
    }
    return 0;
}

int cmd_filters() {
    std::cout << "Registered filters:\n";
    for (const auto& reg : lx::filters::registered_filters()) {
        std::cout << "  - " << reg.info.name << ": " << reg.info.description << "\n";
    }
    return 0;
}

int cmd_validate(int argc, char** argv) {
    lx::configurator::Config config;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ram-mib" && i + 1 < argc) {
            if (!mib_to_bytes(argv[++i], config.reserved_ram_bytes)) {
                std::cerr << "invalid --ram-mib value\n";
                return 2;
            }
        } else if (arg == "--cores" && i + 1 < argc) {
            if (!parse_u32(argv[++i], config.reserved_cpu_cores)) {
                std::cerr << "invalid --cores value\n";
                return 2;
            }
        } else if (arg == "--backend" && i + 1 < argc) {
            config.preferred_backend = argv[++i];
        }
    }

    auto hardware = lx::hardware::detect_hardware();
    if (!hardware.ok()) {
        std::cerr << "hardware detection failed\n";
        return 1;
    }
    lx::configurator::Configurator configurator;
    configurator.attach_host(hardware.value());
    auto report = configurator.validate(config);
    std::cout << "Validation " << (report.ok() ? "OK" : "FAILED") << "\n";
    for (const auto& error : report.errors) std::cout << "  error: " << error << "\n";
    for (const auto& adjustment : report.adjustments) {
        std::cout << "  adjustment: " << adjustment << "\n";
    }
    std::cout << "Effective: RAM=" << (report.effective.reserved_ram_bytes / lx::kMiB)
              << " MiB, cores=" << report.effective.reserved_cpu_cores
              << ", backend=" << report.effective.preferred_backend << "\n";
    return report.ok() ? 0 : 1;
}

int cmd_render(int argc, char** argv) {
    lx::u32 width = 320, height = 240, frames = 3;
    lx::u32 cores = 0;  // 0 => let the renderer run the chain serially
    lx::u32 target_fps = 0;  // 0 => uncapped
    std::string filter_list = "tone";
    std::string backend_name;  // empty => best probed backend
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--width" && i + 1 < argc) {
            if (!parse_u32(argv[++i], width) || width == 0) {
                std::cerr << "invalid --width value\n";
                return 2;
            }
        } else if (arg == "--height" && i + 1 < argc) {
            if (!parse_u32(argv[++i], height) || height == 0) {
                std::cerr << "invalid --height value\n";
                return 2;
            }
        } else if (arg == "--frames" && i + 1 < argc) {
            if (!parse_u32(argv[++i], frames)) {
                std::cerr << "invalid --frames value\n";
                return 2;
            }
        } else if (arg == "--cores" && i + 1 < argc) {
            if (!parse_u32(argv[++i], cores)) {
                std::cerr << "invalid --cores value\n";
                return 2;
            }
        } else if (arg == "--target-fps" && i + 1 < argc) {
            if (!parse_u32(argv[++i], target_fps)) {
                std::cerr << "invalid --target-fps value\n";
                return 2;
            }
        } else if (arg == "--filter" && i + 1 < argc) {
            filter_list = argv[++i];
        } else if (arg == "--backend" && i + 1 < argc) {
            backend_name = argv[++i];
        } else {
            // Silently ignoring a mistyped flag would run a different workload
            // than the one asked for, so every unknown argument is an error.
            std::cerr << "unknown option '" << arg << "' for render\n";
            return 2;
        }
    }

    // Guard the pixel-count multiply before allocating the source frame: for
    // absurd dimensions width*height*4 would wrap the size_t and the fill loop
    // would then write far past the (small) buffer.
    if (static_cast<lx::u64>(width) * height >
        std::numeric_limits<std::size_t>::max() / 4) {
        std::cerr << "invalid render dimensions\n";
        return 2;
    }

    // Resolve the backend the same way the rest of the tooling does: an explicit
    // name must exist, otherwise take the first probed backend, which the probe
    // orders hardware before software and before the CPU fallback.
    lx::backends::BackendKind kind = lx::backends::BackendKind::Cpu;
    if (!backend_name.empty()) {
        bool matched = false;
        for (const auto& reg : lx::backends::probe_available_backends()) {
            if (backend_name == lx::backends::to_string(reg.kind)) {
                kind = reg.kind;
                matched = true;
                break;
            }
        }
        if (!matched) {
            std::cerr << "backend '" << backend_name << "' is not usable on this host; run 'lx_cli info'\n";
            return 1;
        }
    } else {
        const auto available = lx::backends::probe_available_backends();
        if (!available.empty()) kind = available.front().kind;
    }

    lx::renderer::RendererDesc desc;
    desc.width = width;
    desc.height = height;
    desc.backend.preferred = kind;
    desc.shader_asset_root = "shaders";
    desc.cpu_worker_count = cores;
    desc.target_fps = target_fps;

    lx::renderer::Renderer renderer;
    lx::Status status = renderer.initialize(desc);
    if (!status.ok()) {
        std::cerr << "renderer init failed: " << status.message() << "\n";
        return 1;
    }
    std::cout << "Renderer backend: " << renderer.backend_info().device_name << "\n";
    if (renderer.thread_pool() != nullptr) {
        std::cout << "Filter workers: " << renderer.thread_pool()->thread_count() << "\n";
    }

    std::vector<std::pair<std::string, lx::filters::FilterParams>> filters;
    lx::filters::FilterParams params;
    std::size_t start = 0;
    while (start <= filter_list.size()) {
        const std::size_t comma = filter_list.find(',', start);
        const std::string name = filter_list.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (name.empty()) {
            std::cerr << "empty filter name in '--filter " << filter_list << "'\n";
            return 2;
        }
        filters.emplace_back(name, params);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    status = renderer.configure_filters(filters);
    if (!status.ok()) {
        std::cerr << "filter setup failed: " << status.message() << "\n";
        return 1;
    }

    // A simple gradient source so the output is deterministic and inspectable.
    std::vector<lx::u8> pixels(static_cast<std::size_t>(width) * height * 4);
    for (lx::u32 y = 0; y < height; ++y) {
        for (lx::u32 x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[o + 0] = static_cast<lx::u8>(x * 255 / std::max(1u, width - 1));
            pixels[o + 1] = static_cast<lx::u8>(y * 255 / std::max(1u, height - 1));
            pixels[o + 2] = 128;
            pixels[o + 3] = 255;
        }
    }

    lx::renderer::FrameInput input;
    input.pixels = pixels.data();
    input.bytes = pixels.size();

    const auto wall_start = std::chrono::steady_clock::now();
    for (lx::u32 frame = 0; frame < frames; ++frame) {
        lx::renderer::FrameStats stats;
        status = renderer.render_frame(input, &stats);
        if (!status.ok()) {
            std::cerr << "render failed: " << status.message() << "\n";
            return 1;
        }
        std::cout << "frame " << frame << ": cpu=" << stats.cpu_time_micros()
                  << "us filter=" << stats.filter_time_micros() << "us\n";
    }
    const double wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start)
            .count();
    // Two different numbers, and both are measured. The frame rate is what the
    // loop actually achieved, which a target-fps cap deliberately lowers. The
    // work rate is how fast the renderer could go without the cap; reporting only
    // that under a cap would overstate the achieved rate, and reporting only the
    // achieved rate would hide the headroom.
    std::cout << "frames per second (measured, wall clock): "
              << (wall_seconds > 0.0 ? static_cast<double>(frames) / wall_seconds : 0.0)
              << "\n";
    std::cout << "render work rate (measured, excludes any frame pacing): "
              << renderer.stats().average_fps() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    const std::string command = argv[1];
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }
    if (command == "info") return cmd_info();
    if (command == "filters") return cmd_filters();
    if (command == "validate") return cmd_validate(argc, argv);
    if (command == "render") return cmd_render(argc, argv);
    std::cerr << "unknown command '" << command << "'\n\n";
    print_usage();
    return 1;
}
