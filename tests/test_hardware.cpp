// Tests for host detection and the configurator.
#include <cstdio>

#include "configurator/configurator.hpp"
#include "core/files.hpp"
#include "hardware/hardware_info.hpp"
#include "test_framework.hpp"

using namespace lx;

LX_TEST(hardware, detection_reports_real_values) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    const auto& info = hardware.value();
    // These come from the OS; a real machine always reports them.
    LX_CHECK(info.cpu.logical_cores > 0);
    LX_CHECK(info.memory.total_bytes > 0);
    LX_CHECK(info.memory.page_size_bytes > 0);
    LX_CHECK(!info.os.name.empty());
    LX_CHECK(!info.os.architecture.empty());
}

LX_TEST(hardware, logical_processor_count_matches) {
    LX_CHECK(hardware::logical_processor_count() > 0);
}

LX_TEST(hardware, gpu_enumeration_does_not_crash) {
    auto gpus = hardware::enumerate_system_gpus();
    LX_CHECK_OK(gpus.status());
    // The count may legitimately be zero (headless container).
    for (const auto& gpu : gpus.value()) {
        LX_CHECK(!gpu.name.empty());
    }
}

LX_TEST(hardware, memory_topology_is_consistent_with_devices) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    const auto& info = hardware.value();

    const auto topology = info.memory_topology();
    // The derived topology must agree with the primary device's own flag, so a
    // caller can rely on either.
    const auto* gpu = info.primary_gpu();
    if (gpu == nullptr) {
        LX_CHECK(topology == hardware::MemoryTopology::Unknown);
        LX_CHECK(!info.has_unified_memory());
    } else if (gpu->unified_memory) {
        LX_CHECK(topology == hardware::MemoryTopology::Unified);
        LX_CHECK(info.has_unified_memory());
    } else if (gpu->dedicated_vram_bytes > 0) {
        LX_CHECK(topology == hardware::MemoryTopology::Discrete);
    }
}

LX_TEST(hardware, hybrid_cpu_flag_matches_core_counts) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    const auto& cpu = hardware.value().cpu;
    // A hybrid CPU reports both classes; a uniform one reports neither.
    const bool both = cpu.performance_cores > 0 && cpu.efficiency_cores > 0;
    LX_CHECK_EQ(hardware.value().is_hybrid_cpu(), both);
    if (both) {
        // The classes cannot together exceed the physical cores.
        LX_CHECK(cpu.performance_cores + cpu.efficiency_cores <= cpu.physical_cores);
    }
}

LX_TEST(configurator, clamps_cores_to_host) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());

    configurator::Config request;
    request.reserved_cpu_cores = 9999;  // absurd
    request.reserved_ram_bytes = 1 * kGiB;
    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK(report.effective.reserved_cpu_cores < 9999);
    LX_CHECK(report.effective.reserved_cpu_cores >= 1);
    LX_CHECK(!report.adjustments.empty());
}

LX_TEST(configurator, clamps_ram_to_safe_limit) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());

    configurator::Config request;
    request.reserved_cpu_cores = 1;
    request.reserved_ram_bytes = hardware.value().memory.total_bytes;  // everything
    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK(report.effective.reserved_ram_bytes < hardware.value().memory.total_bytes);
}

LX_TEST(configurator, save_and_load_roundtrip) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());

    configurator::Config request;
    request.reserved_cpu_cores = 2;
    request.reserved_ram_bytes = 512 * kMiB;
    request.preferred_backend = "cpu";
    request.filters_enabled = true;
    configurator::FilterConfig filter;
    filter.name = "tone";
    filter.intensity = 0.75f;
    filter.scope = "game";
    request.filters.push_back(filter);

    auto applied = configurator.apply(request);
    LX_CHECK_OK(applied.status());

    const std::string path = "/tmp/lx_test_config.json";
    LX_CHECK_OK(configurator.save(path));

    configurator::Configurator reloaded;
    reloaded.attach_host(hardware.value());
    auto loaded = reloaded.load(path);
    LX_CHECK_OK(loaded.status());
    LX_CHECK_EQ(reloaded.active().preferred_backend, std::string("cpu"));
    LX_CHECK_EQ(reloaded.active().reserved_cpu_cores, applied.value().effective.reserved_cpu_cores);
    LX_CHECK_EQ(reloaded.active().filters.size(), static_cast<std::size_t>(1));
    // The content scope survives a save/load round trip.
    LX_CHECK_EQ(reloaded.active().filters[0].scope, std::string("game"));
    std::remove(path.c_str());
}

LX_TEST(configurator, unknown_filter_scope_is_clamped_to_any) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());

    configurator::Config request;
    request.preferred_backend = "cpu";
    configurator::FilterConfig filter;
    filter.name = "tone";
    filter.scope = "nonsense";
    request.filters.push_back(filter);

    auto applied = configurator.apply(request);
    LX_CHECK_OK(applied.status());
    LX_CHECK_EQ(applied.value().effective.filters[0].scope, std::string("any"));
    // The clamp is reported, not silent.
    bool reported = false;
    for (const auto& adjustment : applied.value().adjustments) {
        if (adjustment.find("scope") != std::string::npos) reported = true;
    }
    LX_CHECK(reported);
}

LX_TEST(configurator, rejects_absurd_numeric_values) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());

    auto load_text = [&](const std::string& text, const std::string& path) {
        LX_CHECK_OK(lx::write_file(path, text));
        configurator::Configurator configurator;
        configurator.attach_host(hardware.value());
        auto report = configurator.load(path);
        std::remove(path.c_str());
        return report;
    };

    // A RAM figure whose MiB-to-bytes scaling would overflow u64 must be
    // reported as an error, not wrapped into a tiny reservation.
    const auto huge_ram = load_text(
        R"({"reserved_ram_mib": 99999999999999, "reserved_cpu_cores": 1})",
        "/tmp/lx_test_huge_ram.json");
    LX_CHECK(!huge_ram.ok());

    // A core count beyond u32 is likewise rejected.
    const auto huge_cores = load_text(R"({"reserved_cpu_cores": 99999999999})",
                                      "/tmp/lx_test_huge_cores.json");
    LX_CHECK(!huge_cores.ok());

    // A sane document loads without error.
    const auto sane = load_text(R"({"reserved_ram_mib": 512, "reserved_cpu_cores": 2})",
                                "/tmp/lx_test_sane.json");
    LX_CHECK_OK(sane.status());
}

LX_TEST_MAIN("hardware")
