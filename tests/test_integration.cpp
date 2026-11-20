// End-to-end integration tests: configurator -> driver -> renderer, and the
// manager coordinating real work across the CPU pool and a backend.
#include <numeric>
#include <vector>

#include "configurator/configurator.hpp"
#include "driver/driver.hpp"
#include "manager/manager.hpp"
#include "renderer/renderer.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {

std::vector<u8> gradient(u32 width, u32 height) {
    std::vector<u8> pixels(static_cast<std::size_t>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[o + 0] = static_cast<u8>(x * 3);
            pixels[o + 1] = static_cast<u8>(y * 3);
            pixels[o + 2] = 128;
            pixels[o + 3] = 255;
        }
    }
    return pixels;
}

}  // namespace

LX_TEST(integration, configurator_to_driver_pipeline) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());

    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());

    configurator::Config request;
    request.reserved_cpu_cores = 1;
    request.reserved_ram_bytes = 256 * kMiB;
    request.preferred_backend = "cpu";  // deterministic for this test
    auto report = configurator.validate(request);
    LX_CHECK(report.ok());

    driver::Driver driver;
    LX_CHECK_OK(driver.initialize(report.effective));
    LX_CHECK(driver.initialized());
    LX_CHECK(driver.backend() != nullptr);
    LX_CHECK(driver.backend()->info().kind == backends::BackendKind::Cpu);
    LX_CHECK(driver.memory_budget().limit() > 0);
    // The integration probe must report something, even if only "unavailable".
    LX_CHECK(!driver.status().integration.platform.empty());
    driver.shutdown();
}

LX_TEST(integration, driver_reports_backend_probe) {
    const auto available = driver::Driver::probe_backends();
    LX_CHECK(!available.empty());
    // The CPU backend is always available in any build.
    bool has_cpu = false;
    for (const auto& reg : available) {
        if (reg.kind == backends::BackendKind::Cpu) has_cpu = true;
    }
    LX_CHECK(has_cpu);
}

LX_TEST(integration, full_frame_render_with_filters) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());

    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());
    configurator::Config request;
    request.reserved_cpu_cores = 2;
    request.reserved_ram_bytes = 256 * kMiB;
    request.preferred_backend = "cpu";
    configurator::FilterConfig tone;
    tone.name = "tone";
    tone.intensity = 0.6f;
    request.filters.push_back(tone);
    auto report = configurator.validate(request);
    LX_CHECK(report.ok());

    driver::Driver driver;
    LX_CHECK_OK(driver.initialize(report.effective));

    renderer::Renderer renderer;
    renderer::RendererDesc desc;
    desc.width = 128;
    desc.height = 128;
    desc.backend.preferred = backends::BackendKind::Cpu;
    LX_CHECK_OK(renderer.initialize(desc));
    LX_CHECK_OK(renderer.configure_filters({{"tone", {}}, {"bloom", {}}}));

    auto frame = gradient(128, 128);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();

    // Render several frames; the pool must be reused without leaking.
    for (int i = 0; i < 5; ++i) {
        renderer::FrameStats stats;
        LX_CHECK_OK(renderer.render_frame(input, &stats));
    }
    LX_CHECK_EQ(renderer.stats().total_frames(), static_cast<u64>(5));
    LX_CHECK(renderer.stats().average_fps() > 0.0);

    // Readback must succeed and produce a full frame.
    std::vector<u8> out(frame.size(), 0);
    auto read = renderer.readback(out.data(), out.size());
    LX_CHECK_OK(read.status());
    LX_CHECK_EQ(read.value(), static_cast<u64>(frame.size()));

    renderer.shutdown();
    driver.shutdown();
}

LX_TEST(integration, manager_and_renderer_share_budget) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());
    configurator::Config request;
    request.reserved_cpu_cores = 2;
    request.reserved_ram_bytes = 128 * kMiB;
    request.preferred_backend = "cpu";
    auto report = configurator.validate(request);
    LX_CHECK(report.ok());

    driver::Driver driver;
    LX_CHECK_OK(driver.initialize(report.effective));

    manager::Manager manager;
    manager::ManagerConfig config;
    LX_CHECK_OK(manager.initialize(driver.backend(), &driver.memory_budget(), config));

    // Submit a batch of CPU-bound passes and drain them through the manager.
    constexpr int kPasses = 20;
    for (int i = 0; i < kPasses; ++i) {
        manager::WorkItem item;
        item.working_set_bytes = 64 * kKiB;
        item.output_bytes = 64 * kKiB;
        item.independent_units = 1;  // small => CPU
        item.name = "pass";
        auto outcome = manager.plan(item);
        LX_CHECK_OK(outcome.status());
    }
    int drained = 0;
    while (manager.next_cpu_task().has_value()) ++drained;
    LX_CHECK_EQ(drained, kPasses);
    LX_CHECK_EQ(driver.memory_budget().used(), static_cast<u64>(0));

    manager.shutdown();
    driver.shutdown();
}

LX_TEST(integration, memory_budget_is_enforced_end_to_end) {
    auto hardware = hardware::detect_hardware();
    LX_CHECK_OK(hardware.status());
    configurator::Configurator configurator;
    configurator.attach_host(hardware.value());
    configurator::Config request;
    request.reserved_cpu_cores = 1;
    request.reserved_ram_bytes = 32 * kMiB;  // deliberately small
    request.preferred_backend = "cpu";
    auto report = configurator.validate(request);
    LX_CHECK(report.ok());

    driver::Driver driver;
    LX_CHECK_OK(driver.initialize(report.effective));
    const u64 limit = driver.memory_budget().limit();

    manager::Manager manager;
    LX_CHECK_OK(manager.initialize(driver.backend(), &driver.memory_budget(), {}));

    // One request larger than the whole budget must be refused.
    manager::WorkItem huge;
    huge.working_set_bytes = limit + (16 * kMiB);
    huge.output_bytes = 0;
    huge.independent_units = 1;
    huge.name = "oversized";
    auto outcome = manager.plan(huge);
    LX_CHECK(!outcome.ok());
    LX_CHECK(outcome.status().code() == ErrorCode::OutOfMemory);

    manager.shutdown();
    driver.shutdown();
}

LX_TEST_MAIN("integration")
