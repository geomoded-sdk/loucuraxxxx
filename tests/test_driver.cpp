// Tests for the driver: initialization, backend selection and fallback, the
// memory budget it owns, platform integration reporting, and error handling.
//
// These run against the real host: the driver's whole job is to inspect it, so
// substituting a fake host would test nothing. Where a value is host-dependent
// the assertion is on an invariant (granted <= available, budget matches the
// plan) rather than on a specific number.
#include <string>
#include <vector>

#include "configurator/config.hpp"
#include "driver/driver.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {

// The CPU backend always exists, so these tests can run on any host.
configurator::Config cpu_config() {
    configurator::Config config;
    config.preferred_backend = "cpu";
    config.reserved_cpu_cores = 1;
    config.reserved_ram_bytes = 64 * kMiB;
    config.allow_cpu_fallback = true;
    return config;
}

bool has_warning_containing(const driver::DriverStatus& status, const std::string& needle) {
    for (const auto& warning : status.warnings) {
        if (warning.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

LX_TEST(driver, initialize_on_cpu_backend_succeeds) {
    driver::Driver drv;
    LX_CHECK(!drv.initialized());
    LX_CHECK_OK(drv.initialize(cpu_config()));
    LX_CHECK(drv.initialized());
    LX_CHECK(drv.status().initialized);
    LX_CHECK(drv.backend() != nullptr);
    LX_CHECK_EQ(drv.status().backend.kind, backends::BackendKind::Cpu);
}

LX_TEST(driver, initialize_twice_is_refused_without_disturbing_the_first) {
    // A second initialize() must fail loudly rather than leaking or replacing the
    // live backend, which would leave the first caller holding a dangling view.
    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(cpu_config()));
    const auto* first = drv.backend();

    const auto second = drv.initialize(cpu_config());
    LX_CHECK(!second.ok());
    LX_CHECK_EQ(second.code(), ErrorCode::ResourceBusy);
    LX_CHECK_EQ(drv.backend(), first);
    LX_CHECK(drv.initialized());
}

LX_TEST(driver, shutdown_is_idempotent_and_releases_the_backend) {
    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(cpu_config()));
    drv.shutdown();
    LX_CHECK(!drv.initialized());
    LX_CHECK(drv.backend() == nullptr);
    // Calling it again must be harmless: callers release in destructors and error
    // paths, so a second call is normal rather than a bug.
    drv.shutdown();
    LX_CHECK(!drv.initialized());
    LX_CHECK(drv.backend() == nullptr);
}

LX_TEST(driver, reinitialize_does_not_accumulate_stale_status) {
    // Regression: shutdown() used to clear only the initialized flag, so warnings
    // from the previous session survived. A caller that reinitialized saw each
    // warning twice -- 2 then 4 in a probe -- and status() still described a
    // device that had been released.
    configurator::Config config = cpu_config();
    config.reserved_ram_bytes = 1ull << 50;  // guarantees a clamp warning

    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(config));
    const std::size_t first_cycle = drv.status().warnings.size();
    LX_CHECK(first_cycle > 0);

    drv.shutdown();
    LX_CHECK(drv.status().warnings.empty());
    LX_CHECK(drv.status().reservation.granted_bytes == 0);
    LX_CHECK(!drv.status().initialized);

    LX_CHECK_OK(drv.initialize(config));
    LX_CHECK_EQ(drv.status().warnings.size(), first_cycle);
}

LX_TEST(driver, reinitialize_after_shutdown_works) {
    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(cpu_config()));
    drv.shutdown();
    LX_CHECK_OK(drv.initialize(cpu_config()));
    LX_CHECK(drv.initialized());
    LX_CHECK(drv.backend() != nullptr);
}

LX_TEST(driver, reservation_is_clamped_to_what_the_host_can_give) {
    // Asking for more RAM than exists must not be granted. The driver reports the
    // clamp and records why, and the budget reflects the grant rather than the
    // request.
    configurator::Config config = cpu_config();
    config.reserved_ram_bytes = 1ull << 50;  // 1 PiB, larger than any host

    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(config));
    const auto& reservation = drv.status().reservation;
    LX_CHECK(reservation.clamped);
    LX_CHECK(reservation.granted_bytes < reservation.requested_bytes);
    LX_CHECK(reservation.granted_bytes <= reservation.system_available_bytes);
    LX_CHECK(!reservation.reason.empty());
    LX_CHECK(has_warning_containing(drv.status(), "clamped"));
    LX_CHECK_EQ(drv.memory_budget().limit(), reservation.granted_bytes);
}

LX_TEST(driver, small_reservation_is_granted_as_requested) {
    configurator::Config config = cpu_config();
    config.reserved_ram_bytes = 16 * kMiB;

    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(config));
    const auto& reservation = drv.status().reservation;
    // 16 MiB is small enough that no sane policy clamps it on a test host. If a
    // host is so constrained that it does, the invariant below still holds.
    LX_CHECK(reservation.granted_bytes > 0);
    LX_CHECK(reservation.granted_bytes <= reservation.system_available_bytes);
    LX_CHECK_EQ(drv.memory_budget().limit(), reservation.granted_bytes);
}

LX_TEST(driver, zero_cores_is_reported_as_a_warning_not_a_failure) {
    // A configuration with no reserved cores is legal: the layer then contributes
    // no CPU parallelism. It must be visible in the warnings, not silent.
    configurator::Config config = cpu_config();
    config.reserved_cpu_cores = 0;

    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(config));
    LX_CHECK(has_warning_containing(drv.status(), "no CPU cores"));
}

LX_TEST(driver, unavailable_backend_falls_back_to_cpu_when_allowed) {
    // "vulkan" may or may not exist on the host running this test. Either way the
    // outcome must be well defined: a working backend, and a warning naming the
    // fallback when one happened.
    configurator::Config config = cpu_config();
    config.preferred_backend = "vulkan";
    config.allow_cpu_fallback = true;

    driver::Driver drv;
    const auto status = drv.initialize(config);
    LX_CHECK_OK(status);
    LX_CHECK(drv.backend() != nullptr);
    if (drv.status().backend.kind == backends::BackendKind::Cpu) {
        LX_CHECK(has_warning_containing(drv.status(), "falling back to CPU"));
    }
}

LX_TEST(driver, unavailable_backend_fails_when_fallback_is_disallowed) {
    configurator::Config config = cpu_config();
    config.preferred_backend = "vulkan";
    config.allow_cpu_fallback = false;

    driver::Driver drv;
    const auto status = drv.initialize(config);
    if (status.ok()) {
        // A Vulkan device (possibly a software rasterizer) is present, so the
        // failure path cannot be exercised here. Assert the invariant that
        // matters instead of pretending: the backend really is Vulkan, and
        // nothing fell back to the CPU.
        LX_CHECK_EQ(drv.status().backend.kind, backends::BackendKind::Vulkan);
        LX_CHECK(!has_warning_containing(drv.status(), "falling back to CPU"));
    } else {
        LX_CHECK(!drv.initialized());
        LX_CHECK(drv.backend() == nullptr);
    }
}

LX_TEST(driver, auto_selects_some_backend_on_any_host) {
    configurator::Config config = cpu_config();
    config.preferred_backend = "auto";

    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(config));
    LX_CHECK(drv.backend() != nullptr);
    // The driver must never report an "initialized" state with no device behind
    // it, which is the fake-GPU failure mode this project exists to avoid.
    LX_CHECK(drv.status().initialized);
}

LX_TEST(driver, platform_integration_never_claims_more_than_user_space) {
    // The driver may not install kernel drivers, so anything above user-space
    // would be a false claim. On Linux without /dev/dri it must report
    // Unavailable rather than pretending to have a DRM connection.
    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(cpu_config()));
    const auto& integration = drv.status().integration;
    LX_CHECK(integration.level == driver::IntegrationLevel::Unavailable ||
             integration.level == driver::IntegrationLevel::UserSpaceOnly);
    // Whatever it reports, it must be self-consistent: exposing an adapter
    // requires at least a user-space connection.
    if (integration.can_expose_adapter) {
        LX_CHECK(integration.level != driver::IntegrationLevel::Unavailable);
    }
    if (integration.level != driver::IntegrationLevel::Unavailable) {
        LX_CHECK(!integration.platform.empty());
    }
}

LX_TEST(driver, integration_does_not_claim_unimplemented_capabilities) {
    // Both flags describe what the integration achieves. No code path in this
    // repository advertises a virtual adapter or shares a surface without a copy,
    // so both must be false. They were previously set from a platform check (a
    // DRM render node existed, so "zero-copy sharing is possible"), which told
    // callers a feature was available that nothing implements.
    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(cpu_config()));
    const auto& integration = drv.status().integration;
    LX_CHECK_EQ(integration.can_expose_adapter, false);
    LX_CHECK_EQ(integration.can_share_surfaces, false);
}

LX_TEST(driver, probe_backends_reports_the_cpu_backend) {
    // probe_backends() must not create a device, and the CPU backend is always
    // available, so it is always in the list.
    const auto available = driver::Driver::probe_backends();
    LX_CHECK(!available.empty());
    bool found_cpu = false;
    for (const auto& entry : available) {
        if (entry.kind == backends::BackendKind::Cpu) found_cpu = true;
    }
    LX_CHECK(found_cpu);
}

LX_TEST(driver, config_is_retained_for_callers) {
    configurator::Config config = cpu_config();
    config.reserved_cpu_cores = 3;
    configurator::FilterConfig filter;
    filter.name = "sharpen";
    filter.intensity = 0.5f;
    config.filters.push_back(filter);

    driver::Driver drv;
    LX_CHECK_OK(drv.initialize(config));
    LX_CHECK_EQ(drv.config().reserved_cpu_cores, u32{3});
    LX_CHECK_EQ(drv.config().filters.size(), static_cast<std::size_t>(1));
}

LX_TEST_MAIN("driver")
