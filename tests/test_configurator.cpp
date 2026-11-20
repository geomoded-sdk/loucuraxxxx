// Tests for the Configurator: validation, clamping, filter handling and
// persistence. The host is synthesised so the clamps are deterministic and do
// not depend on the machine running the suite.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "configurator/configurator.hpp"
#include "hardware/hardware_info.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {

// A host with known numbers, so "clamped to the safe limit" can be asserted
// exactly instead of compared against whatever this machine happens to report.
hardware::HardwareInfo make_host(u64 total, u64 available, u32 logical_cores,
                                 bool with_gpu) {
    hardware::HardwareInfo info;
    info.cpu.model = "Synthetic CPU";
    info.cpu.vendor = "test";
    info.cpu.logical_cores = logical_cores;
    info.cpu.physical_cores = logical_cores;
    info.memory.total_bytes = total;
    info.memory.available_bytes = available;
    info.memory.page_size_bytes = 4096;
    info.os.name = "Linux";
    info.os.architecture = "x86_64";
    if (with_gpu) {
        hardware::GpuDeviceInfo gpu;
        gpu.name = "Synthetic GPU";
        gpu.kind = hardware::GpuKind::Discrete;
        gpu.dedicated_vram_bytes = 8 * kGiB;
        info.gpus.push_back(gpu);
    }
    return info;
}

configurator::Configurator make_configurator(const hardware::HardwareInfo& host) {
    configurator::Configurator configurator;
    configurator.attach_host(host);
    return configurator;
}

bool report_mentions(const configurator::ValidationReport& report,
                     const std::string& needle) {
    for (const auto& adjustment : report.adjustments) {
        if (adjustment.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

LX_TEST(configurator, valid_request_is_not_altered) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 16, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 2 * kGiB;
    request.reserved_cpu_cores = 4;
    request.priority = configurator::Priority::High;

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK(report.adjustments.empty());
    LX_CHECK_EQ(report.effective.reserved_ram_bytes, u64{2 * kGiB});
    LX_CHECK_EQ(report.effective.reserved_cpu_cores, u32{4});
    LX_CHECK(report.effective.priority == configurator::Priority::High);
}

LX_TEST(configurator, ram_request_is_clamped_to_available_memory) {
    const auto host = make_host(16 * kGiB, 12 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 64 * kGiB;  // more than the machine has
    request.reserved_cpu_cores = 2;

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK(report.effective.reserved_ram_bytes < request.reserved_ram_bytes);
    LX_CHECK(report.effective.reserved_ram_bytes <= host.memory.total_bytes);
    // The OS must keep its headroom, so the grant cannot be the whole machine.
    LX_CHECK(report.effective.reserved_ram_bytes < host.memory.total_bytes);
    LX_CHECK(!report.adjustments.empty());
}

LX_TEST(configurator, zero_ram_is_raised_to_a_usable_minimum) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 0;
    request.reserved_cpu_cores = 1;

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK(report.effective.reserved_ram_bytes > 0);
    LX_CHECK(report_mentions(report, "minimum"));
}

LX_TEST(configurator, cores_are_clamped_and_one_core_is_left_for_the_os) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 2 * kGiB;
    request.reserved_cpu_cores = 999;

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK(report.effective.reserved_cpu_cores <= host.cpu.logical_cores);
    LX_CHECK(report.effective.reserved_cpu_cores < host.cpu.logical_cores);
    LX_CHECK(report.effective.reserved_cpu_cores >= 1);
    LX_CHECK(!report.adjustments.empty());
}

LX_TEST(configurator, single_core_host_still_yields_a_usable_config) {
    // A one-core machine cannot give a core away and keep one for the OS. The
    // Configurator must not deadlock the user or return an impossible value.
    const auto host = make_host(8 * kGiB, 6 * kGiB, 1, false);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 1 * kGiB;
    request.reserved_cpu_cores = 4;

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK_EQ(report.effective.reserved_cpu_cores, u32{1});
    LX_CHECK(report.effective.reserved_ram_bytes > 0);
}

LX_TEST(configurator, unknown_backend_falls_back_and_says_so) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 2 * kGiB;
    request.reserved_cpu_cores = 2;
    request.preferred_backend = "not_a_real_backend";

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK(report.effective.preferred_backend != "not_a_real_backend");
    LX_CHECK(!report.adjustments.empty());
}

LX_TEST(configurator, host_without_gpu_can_configure_cpu_only) {
    const auto host = make_host(16 * kGiB, 12 * kGiB, 8, false);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 2 * kGiB;
    request.reserved_cpu_cores = 2;
    request.preferred_backend = "auto";

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    // No GPU means the resolved backend cannot promise one.
    LX_CHECK(report.effective.preferred_backend != "vulkan");
    LX_CHECK(report.effective.preferred_backend != "metal");
}

LX_TEST(configurator, unknown_host_memory_is_refused_not_guessed) {
    // total_bytes == 0 means detection failed. Guessing a budget here would be
    // the one way this module could starve the machine, so it must refuse.
    auto host = make_host(0, 0, 8, false);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 2 * kGiB;
    request.reserved_cpu_cores = 2;

    const auto report = configurator.validate(request);
    LX_CHECK_EQ(report.effective.reserved_ram_bytes, u64{0});
    LX_CHECK(!report.adjustments.empty());
}

LX_TEST(configurator, filter_intensity_and_quality_are_clamped) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 2 * kGiB;
    request.reserved_cpu_cores = 2;
    configurator::FilterConfig high;
    high.name = "tone";
    high.intensity = 5.0f;      // out of range
    high.quality = "ultra";     // unknown
    high.scope = "everything";  // unknown
    request.filters.push_back(high);

    configurator::FilterConfig negative;
    negative.name = "blur";
    negative.intensity = -2.0f;
    request.filters.push_back(negative);

    const auto report = configurator.validate(request);
    LX_CHECK(report.ok());
    LX_CHECK_EQ(report.effective.filters.size(), std::size_t{2});
    LX_CHECK_EQ(report.effective.filters[0].intensity, 1.0f);
    LX_CHECK_EQ(report.effective.filters[0].quality, std::string("balanced"));
    LX_CHECK_EQ(report.effective.filters[0].scope, std::string("any"));
    LX_CHECK_EQ(report.effective.filters[1].intensity, 0.0f);
    LX_CHECK(report.adjustments.size() >= 3);
}

LX_TEST(configurator, unknown_filter_name_is_reported) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 2 * kGiB;
    request.reserved_cpu_cores = 2;
    configurator::FilterConfig unknown;
    unknown.name = "no_such_filter";
    request.filters.push_back(unknown);

    const auto report = configurator.validate(request);
    // Silently dropping a filter the user asked for would be a lie of omission,
    // so it must surface as an error naming it.
    LX_CHECK(!report.ok());
    const bool named = std::any_of(report.errors.begin(), report.errors.end(),
                                   [](const std::string& e) {
                                       return e.find("no_such_filter") != std::string::npos;
                                   });
    LX_CHECK(named);
}

LX_TEST(configurator, vsync_is_reported_instead_of_silently_stored) {
    // There is no swapchain, so vsync cannot be honoured. Persisting the flag
    // would tell a caller frames were tear-free when nothing synchronised them.
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.vsync = true;
    const auto report = configurator.validate(request);

    LX_CHECK(report.ok());
    LX_CHECK(!report.effective.vsync);
    bool explained = false;
    for (const auto& adjustment : report.adjustments) {
        if (adjustment.find("vsync") != std::string::npos) explained = true;
    }
    LX_CHECK(explained);

    // Leaving it off is silent: no spurious adjustment for the default.
    configurator::Config plain;
    const auto quiet = configurator.validate(plain);
    for (const auto& adjustment : quiet.adjustments) {
        LX_CHECK(adjustment.find("vsync") == std::string::npos);
    }
}

LX_TEST(configurator, filters_round_trip_through_json) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    configurator::Config request;
    request.reserved_ram_bytes = 3 * kGiB;
    request.reserved_cpu_cores = 3;
    request.priority = configurator::Priority::Low;
    request.filters_enabled = true;
    request.target_fps = 60;
    request.max_in_flight_frames = 3;

    configurator::FilterConfig tone;
    tone.name = "tone";
    tone.enabled = true;
    tone.intensity = 0.75f;
    tone.passes = 2;
    tone.quality = "quality";
    tone.scope = "game";
    request.filters.push_back(tone);

    configurator::FilterConfig blur;
    blur.name = "blur";
    blur.enabled = false;
    blur.intensity = 0.25f;
    blur.passes = 3;
    blur.quality = "fast";
    blur.scope = "system";
    request.filters.push_back(blur);

    auto applied = configurator.apply(request);
    LX_CHECK_OK(applied.status());
    if (!applied.ok()) return;

    const std::string path = "test_configurator_roundtrip.json";
    std::remove(path.c_str());
    LX_CHECK_OK(configurator.save(path));

    configurator::Configurator reloaded = make_configurator(host);
    auto loaded = reloaded.load(path);
    LX_CHECK_OK(loaded.status());
    if (!loaded.ok()) {
        std::remove(path.c_str());
        return;
    }

    const auto& config = loaded.value().effective;
    LX_CHECK_EQ(config.reserved_ram_bytes, u64{3 * kGiB});
    LX_CHECK_EQ(config.reserved_cpu_cores, u32{3});
    LX_CHECK(config.priority == configurator::Priority::Low);
    LX_CHECK_EQ(config.target_fps, u32{60});
    LX_CHECK_EQ(config.max_in_flight_frames, u32{3});
    LX_CHECK_EQ(config.filters.size(), std::size_t{2});
    if (config.filters.size() == 2) {
        LX_CHECK_EQ(config.filters[0].name, std::string("tone"));
        LX_CHECK_EQ(config.filters[0].intensity, 0.75f);
        LX_CHECK_EQ(config.filters[0].passes, u32{2});
        LX_CHECK_EQ(config.filters[0].quality, std::string("quality"));
        LX_CHECK_EQ(config.filters[0].scope, std::string("game"));
        LX_CHECK_EQ(config.filters[1].name, std::string("blur"));
        LX_CHECK(!config.filters[1].enabled);
        LX_CHECK_EQ(config.filters[1].passes, u32{3});
        LX_CHECK_EQ(config.filters[1].scope, std::string("system"));
    }

    std::remove(path.c_str());
}

LX_TEST(configurator, malformed_json_is_rejected_with_a_message) {
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);

    const std::string path = "test_configurator_malformed.json";
    {
        std::ofstream out(path);
        out << "{ this is not json ";
    }
    auto loaded = configurator.load(path);
    LX_CHECK(!loaded.ok());
    if (!loaded.ok()) LX_CHECK(!loaded.status().message().empty());
    std::remove(path.c_str());
}

LX_TEST(configurator, missing_file_is_an_error_not_a_default) {
    // Silently substituting defaults for a missing file would hide a mistyped
    // path and apply settings the user never asked for.
    const auto host = make_host(32 * kGiB, 24 * kGiB, 8, true);
    auto configurator = make_configurator(host);
    auto loaded = configurator.load("definitely_not_a_real_config_file.json");
    LX_CHECK(!loaded.ok());
}

LX_TEST(configurator, load_clamps_values_from_disk) {
    // A config file can be edited by hand or written on another machine, so a
    // value that is fine there may be unsafe here. Loading must clamp too.
    const auto host = make_host(8 * kGiB, 6 * kGiB, 4, true);
    auto configurator = make_configurator(host);

    const std::string path = "test_configurator_oversized.json";
    {
        std::ofstream out(path);
        out << R"({"reserved_ram_bytes": 68719476736, "reserved_cpu_cores": 64})";
    }
    auto loaded = configurator.load(path);
    LX_CHECK_OK(loaded.status());
    if (loaded.ok()) {
        LX_CHECK(loaded.value().effective.reserved_ram_bytes < u64{64 * kGiB});
        LX_CHECK(loaded.value().effective.reserved_ram_bytes <= host.memory.total_bytes);
        LX_CHECK(loaded.value().effective.reserved_cpu_cores < host.cpu.logical_cores);
        LX_CHECK(!loaded.value().adjustments.empty());
    }
    std::remove(path.c_str());
}

LX_TEST(configurator, validate_before_attach_is_an_error_not_a_crash) {
    // Calling validate() without attach_host() is a programming error. It must be
    // reported rather than clamped: clamping against limits that were never
    // computed would invent a budget, which is the one thing this module must
    // never do.
    configurator::Configurator configurator;
    configurator::Config request;
    request.reserved_ram_bytes = 1 * kGiB;
    request.reserved_cpu_cores = 2;
    const auto report = configurator.validate(request);
    LX_CHECK(!report.ok());
    LX_CHECK(!report.errors.empty());
}

LX_TEST_MAIN("configurator")
