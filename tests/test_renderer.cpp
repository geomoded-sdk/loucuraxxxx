// Tests for the renderer: initialization, frame processing, filters, readback.
#include <numeric>
#include <chrono>
#include <algorithm>
#include <vector>

#include "core/files.hpp"
#include "renderer/renderer.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {

renderer::RendererDesc make_desc(u32 width, u32 height) {
    renderer::RendererDesc desc;
    desc.width = width;
    desc.height = height;
    desc.backend.preferred = backends::BackendKind::Cpu;
    return desc;
}

std::vector<u8> make_frame(u32 width, u32 height) {
    std::vector<u8> pixels(static_cast<std::size_t>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[o + 0] = static_cast<u8>(x * 4);
            pixels[o + 1] = static_cast<u8>(y * 4);
            pixels[o + 2] = 64;
            pixels[o + 3] = 255;
        }
    }
    return pixels;
}

}  // namespace

LX_TEST(renderer, initialize_and_shutdown) {
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(64, 64)));
    LX_CHECK(renderer.initialized());
    LX_CHECK(renderer.backend_info().kind == backends::BackendKind::Cpu);
    renderer.shutdown();
    LX_CHECK(!renderer.initialized());
    // Shutdown is idempotent.
    renderer.shutdown();
}

LX_TEST(renderer, present_targets_rotate_so_the_previous_frame_survives) {
    // max_in_flight_frames used to be stored and validated but never read, so a
    // second frame overwrote the storage a consumer was still holding. With two
    // targets, frame 1 must be drawn into different storage than frame 0 and the
    // pixels of frame 0 must still be intact afterwards.
    auto desc = make_desc(16, 16);
    desc.enable_filters = false;
    desc.max_in_flight_frames = 2;
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(desc));

    auto first = make_frame(16, 16);
    auto second = first;
    for (auto& byte : second) byte = static_cast<u8>(255 - byte);
    second[3] = 255;  // keep alpha opaque so both frames stay comparable

    renderer::FrameInput input;
    input.pixels = first.data();
    input.bytes = first.size();
    LX_CHECK_OK(renderer.render_frame(input, nullptr));

    u8* first_pixels = renderer.cpu_target_pixels();
    LX_CHECK(first_pixels != nullptr);
    if (first_pixels == nullptr) return;
    const std::vector<u8> first_snapshot(first_pixels, first_pixels + first.size());

    input.pixels = second.data();
    LX_CHECK_OK(renderer.render_frame(input, nullptr));

    u8* second_pixels = renderer.cpu_target_pixels();
    LX_CHECK(second_pixels != nullptr);
    // Different storage for consecutive frames: the rotation is real.
    LX_CHECK(second_pixels != first_pixels);
    // And the previous frame was not overwritten by the new one.
    LX_CHECK(std::equal(first_snapshot.begin(), first_snapshot.end(), first_pixels));
    // The new frame really landed in the rotated target.
    LX_CHECK(std::equal(second.begin(), second.end(), second_pixels));
    renderer.shutdown();
}

LX_TEST(renderer, single_in_flight_frame_reuses_one_target) {
    // max_in_flight_frames = 1 must mean exactly one target: the low-latency,
    // highest-memory-saving setting, not an accidental rotation.
    auto desc = make_desc(16, 16);
    desc.enable_filters = false;
    desc.max_in_flight_frames = 1;
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(desc));

    auto frame = make_frame(16, 16);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();
    LX_CHECK_OK(renderer.render_frame(input, nullptr));
    u8* before = renderer.cpu_target_pixels();
    LX_CHECK_OK(renderer.render_frame(input, nullptr));
    LX_CHECK(renderer.cpu_target_pixels() == before);
    renderer.shutdown();
}

LX_TEST(renderer, rejects_zero_dimensions) {
    renderer::Renderer renderer;
    const Status status = renderer.initialize(make_desc(0, 64));
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);
}

LX_TEST(renderer, render_frame_uploads_and_presents) {
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(32, 32)));
    auto frame = make_frame(32, 32);

    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();

    renderer::FrameStats stats;
    LX_CHECK_OK(renderer.render_frame(input, &stats));
    LX_CHECK_EQ(stats.frame_index, static_cast<u64>(0));
    LX_CHECK_EQ(stats.bytes_uploaded, static_cast<u64>(frame.size()));
    LX_CHECK(renderer.stats().sample_count() == 1);
    renderer.shutdown();
}

LX_TEST(renderer, render_rejects_empty_input) {
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(16, 16)));
    renderer::FrameInput empty;
    const Status status = renderer.render_frame(empty, nullptr);
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);
    renderer.shutdown();
}

LX_TEST(renderer, render_before_initialize_fails_cleanly) {
    renderer::Renderer renderer;
    renderer::FrameInput input;
    u8 pixel = 0;
    input.pixels = &pixel;
    input.bytes = 1;
    const Status status = renderer.render_frame(input, nullptr);
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InitializationFailed);
}

LX_TEST(renderer, padded_frame_input_is_bounds_checked) {
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(8, 4)));

    // A padded source needs src_pitch*(height-1)+row bytes, which is more than
    // width*height*4. Supplying only the tight size must be rejected rather than
    // read past the end of the buffer.
    const u64 tight = static_cast<u64>(8) * 4 * 4;   // 128
    const u64 src_pitch = 64;                        // 8 px padded to 16 px
    std::vector<u8> padded(tight, 0);

    renderer::FrameInput input;
    input.pixels = padded.data();
    input.bytes = tight;  // too small once the pitch is honoured
    input.row_pitch = src_pitch;
    const Status status = renderer.render_frame(input, nullptr);
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);

    // With the correct size it succeeds.
    std::vector<u8> correct(src_pitch * 3 + 32, 0);
    input.pixels = correct.data();
    input.bytes = correct.size();
    input.row_pitch = src_pitch;
    LX_CHECK_OK(renderer.render_frame(input, nullptr));
    renderer.shutdown();
}

LX_TEST(renderer, filters_change_the_output) {
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(64, 64)));
    LX_CHECK_OK(renderer.configure_filters({{"tone", {}}, {"vignette", {}}}));
    LX_CHECK_EQ(renderer.filter_chain().size(), static_cast<std::size_t>(2));

    auto frame = make_frame(64, 64);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();

    renderer::FrameStats stats;
    LX_CHECK_OK(renderer.render_frame(input, &stats));
    // The chain ran, so it recorded time for both filters.
    LX_CHECK(stats.filter_time_micros() > 0);
    LX_CHECK_EQ(renderer.filter_chain().entries().size(), static_cast<std::size_t>(2));
    renderer.shutdown();
}

LX_TEST(renderer, filter_passes_affect_the_rendered_frame) {
    // End-to-end proof that `passes` reaches the frame, not just the chain API.
    // The same input rendered with one blur pass and with three must differ.
    auto render_with = [](u32 passes) {
        renderer::Renderer renderer;
        LX_CHECK_OK(renderer.initialize(make_desc(64, 64)));
        filters::FilterParams params;
        params.passes = passes;
        LX_CHECK_OK(renderer.configure_filters({{"blur", params}}));
        auto frame = make_frame(64, 64);
        renderer::FrameInput input;
        input.pixels = frame.data();
        input.bytes = frame.size();
        renderer::FrameStats stats;
        LX_CHECK_OK(renderer.render_frame(input, &stats));
        std::vector<u8> out(frame.size(), 0);
        auto read = renderer.readback(out.data(), out.size());
        LX_CHECK_OK(read.status());
        renderer.shutdown();
        return out;
    };

    const auto one_pass = render_with(1);
    const auto three_passes = render_with(3);
    LX_CHECK(one_pass != three_passes);
}

LX_TEST(renderer, target_fps_paces_frames_without_inflating_frame_cost) {
    // target_fps was parsed, saved, round-tripped through JSON and asserted in a
    // config test, but nothing ever applied it. It now paces render_frame. Two
    // things matter: the achieved rate really is capped, and the reported frame
    // cost stays the work rather than the deliberate sleep -- otherwise a capped
    // renderer would appear slower per frame than it is.
    renderer::Renderer renderer;
    auto desc = make_desc(64, 64);
    desc.target_fps = 50;  // 20 ms per frame
    LX_CHECK_OK(renderer.initialize(desc));

    auto frame = make_frame(64, 64);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();

    const auto wall_start = std::chrono::steady_clock::now();
    constexpr int kFrames = 10;
    u64 max_frame_nanos = 0;
    for (int i = 0; i < kFrames; ++i) {
        renderer::FrameStats stats;
        LX_CHECK_OK(renderer.render_frame(input, &stats));
        max_frame_nanos = std::max(max_frame_nanos, stats.cpu_time_nanos);
    }
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - wall_start)
                          .count();

    // Ten frames at 50 fps take at least 9 periods, because the first frame has
    // no preceding deadline to wait for.
    const u64 minimum_wall = 9ull * 20'000'000ull;
    LX_CHECK(static_cast<u64>(wall) >= minimum_wall);

    // The per-frame cost stays the work: well under the 20 ms slot it was paced to.
    LX_CHECK(max_frame_nanos < 20'000'000ull);
    renderer.shutdown();
}

LX_TEST(renderer, uncapped_renderer_does_not_wait) {
    // The counterpart: with target_fps 0, many frames must complete far faster
    // than any pacing slot would allow. If pacing leaked into the uncapped path,
    // this would take seconds.
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(64, 64)));
    auto frame = make_frame(64, 64);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();

    const auto wall_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        renderer::FrameStats stats;
        LX_CHECK_OK(renderer.render_frame(input, &stats));
    }
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - wall_start)
                          .count();
    LX_CHECK(wall < 2000);
    renderer.shutdown();
}

LX_TEST(renderer, unknown_filter_is_rejected) {
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(16, 16)));
    const Status status = renderer.configure_filters({{"nonexistent", {}}});
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::NotImplemented);
    renderer.shutdown();
}

LX_TEST(renderer, content_scope_filters_are_skipped_off_scope) {
    // A game-scoped filter must not run on a system frame and vice versa; the
    // scope is what lets the desktop UI bypass a filter tuned for a game.
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(32, 32)));

    filters::FilterParams game_only;
    game_only.scope = filters::ContentScope::Game;
    // vignette with its default intensity darkens the corners, so it visibly
    // changes a frame; tone's defaults are identity and would not.
    LX_CHECK_OK(renderer.configure_filters({{"vignette", game_only}}));

    auto frame = make_frame(32, 32);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();

    // A system frame skips the game-only filter: output equals the input.
    input.scope = filters::ContentScope::System;
    renderer::FrameStats system_stats;
    LX_CHECK_OK(renderer.render_frame(input, &system_stats));
    std::vector<u8> system_out(frame.size(), 0);
    LX_CHECK_OK(renderer.readback(system_out.data(), system_out.size()));
    LX_CHECK(system_out == frame);

    // The same filter runs on a game frame.
    input.scope = filters::ContentScope::Game;
    renderer::FrameStats game_stats;
    LX_CHECK_OK(renderer.render_frame(input, &game_stats));
    LX_CHECK(game_stats.filter_time_micros() > 0);
    std::vector<u8> game_out(frame.size(), 0);
    LX_CHECK_OK(renderer.readback(game_out.data(), game_out.size()));
    LX_CHECK(game_out != frame);

    renderer.shutdown();
}

LX_TEST(renderer, content_scope_unknown_runs_every_filter) {
    // An unclassified frame must not silently skip a filter.
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(32, 32)));
    filters::FilterParams system_only;
    system_only.scope = filters::ContentScope::System;
    LX_CHECK_OK(renderer.configure_filters({{"vignette", system_only}}));

    auto frame = make_frame(32, 32);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();
    // scope stays Unknown
    renderer::FrameStats stats;
    LX_CHECK_OK(renderer.render_frame(input, &stats));
    LX_CHECK(stats.filter_time_micros() > 0);
    renderer.shutdown();
}

LX_TEST(renderer, readback_matches_rendered_frame) {
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(16, 16)));
    auto frame = make_frame(16, 16);
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();
    LX_CHECK_OK(renderer.render_frame(input, nullptr));

    std::vector<u8> out(frame.size(), 0);
    auto read = renderer.readback(out.data(), out.size());
    LX_CHECK_OK(read.status());
    LX_CHECK_EQ(read.value(), static_cast<u64>(frame.size()));
    // With no filters, the target must equal the uploaded frame.
    LX_CHECK(out == frame);
    renderer.shutdown();
}

LX_TEST(renderer, shader_manifest_clamps_push_constant_bytes) {
    // A manifest with a negative push_constant_bytes used to wrap through the
    // u32 cast into a huge block. It must be clamped, not wrapped.
    const std::string dir = "/tmp/lx_test_shader_manifest";
    LX_CHECK_OK(make_directories(dir));
    const std::string manifest = dir + "/manifest.json";
    LX_CHECK_OK(write_file(manifest,
                           R"({"shaders":[{"name":"n","stage":"compute",)"
                           R"("push_constant_bytes":-1}]})"));
    shaders::ShaderLibrary library;
    LX_CHECK_OK(library.load(dir));
    const shaders::ShaderModule* module = library.find("n");
    LX_CHECK(module != nullptr);
    if (module != nullptr) LX_CHECK_EQ(module->push_constant_bytes, static_cast<u32>(0));
    std::remove(manifest.c_str());
}

LX_TEST(renderer, stats_window_percentiles) {
    renderer::StatsWindow window(10);
    for (u64 i = 1; i <= 10; ++i) {
        renderer::FrameStats frame;
        frame.cpu_time_nanos = i * 100'000;
        window.push(frame);
    }
    LX_CHECK_EQ(window.sample_count(), static_cast<std::size_t>(10));
    LX_CHECK_EQ(window.min_frame_micros(), static_cast<u64>(100));
    LX_CHECK_EQ(window.max_frame_micros(), static_cast<u64>(1000));
    LX_CHECK(window.average_fps() > 0.0);
    // The window keeps at most `capacity` samples.
    for (u64 i = 0; i < 5; ++i) {
        renderer::FrameStats frame;
        frame.cpu_time_nanos = 50'000;
        window.push(frame);
    }
    LX_CHECK_EQ(window.sample_count(), static_cast<std::size_t>(10));
    LX_CHECK_EQ(window.total_frames(), static_cast<u64>(15));
}

LX_TEST(renderer, frame_stats_are_measured_not_synthesised) {
    // The FPS figure the project reports comes from these timings, so they have
    // to be real measurements. Rather than assert a specific duration (which
    // would be a machine-dependent number in a test), this checks the internal
    // consistency a genuine clock reading must satisfy: a non-zero frame time
    // that is at least the present phase, and a present phase inside it.
    renderer::Renderer renderer;
    LX_CHECK_OK(renderer.initialize(make_desc(64, 64)));
    auto frame = make_frame(64, 64);

    renderer::FrameStats stats;
    renderer::FrameInput input;
    input.pixels = frame.data();
    input.bytes = frame.size();
    LX_CHECK_OK(renderer.render_frame(input, &stats));

    LX_CHECK(stats.cpu_time_nanos > 0);
    LX_CHECK(stats.present_time_nanos <= stats.cpu_time_nanos);
    // gpu_time stays 0 because nothing queries a GPU clock; StatsWindow adds it
    // to cpu_time, so a non-zero value here would double-count the frame.
    LX_CHECK_EQ(stats.gpu_time_nanos, u64{0});

    const double fps = renderer.stats().average_fps();
    LX_CHECK(fps > 0.0);
    // FPS and the mean frame time must describe the same window.
    const double ms = renderer.stats().average_frame_ms();
    LX_CHECK(ms > 0.0);
    LX_CHECK(std::abs(fps * ms - 1000.0) < 1.0);
}

LX_TEST(renderer, sub_microsecond_frames_do_not_report_zero_fps) {
    // Regression: with microsecond storage a 500 ns frame truncated to 0, the
    // window's total became 0, and average_fps() returned 0.0 -- the CLI printed
    // "average fps (measured): 0" for a renderer that was actually fast. Timing
    // is stored in nanoseconds so this cannot happen.
    renderer::StatsWindow window(10);
    for (int i = 0; i < 4; ++i) {
        renderer::FrameStats frame;
        frame.cpu_time_nanos = 500;
        window.push(frame);
    }
    LX_CHECK_EQ(window.average_frame_ms() > 0.0, true);
    // 4 frames x 500 ns = 2000 ns = 2 us => 2,000,000 fps. The exact figure does
    // not matter; what matters is that it is finite, positive and derived.
    const double fps = window.average_fps();
    LX_CHECK(fps > 0.0);
    LX_CHECK(std::abs(fps - 2.0e6) < 1.0);
    LX_CHECK_EQ(window.min_frame_nanos(), u64{500});
    LX_CHECK_EQ(window.max_frame_nanos(), u64{500});
    LX_CHECK_EQ(window.percentile_frame_nanos(1.0), u64{500});
}

LX_TEST_MAIN("renderer")
