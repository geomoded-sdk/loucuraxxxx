// Tests for individual filters and the filter chain.
#include <algorithm>
#include <numeric>
#include <vector>

#include "filters/builtin/tone_filter.hpp"
#include "filters/filter_chain.hpp"
#include "filters/filter_registry.hpp"
#include "memory/buffer_pool.hpp"
#include "scheduler/thread_pool.hpp"
#include "test_framework.hpp"

using namespace lx;

namespace {

// A deterministic test image: a horizontal gradient with a bright block.
std::vector<u8> make_image(u32 width, u32 height) {
    std::vector<u8> pixels(static_cast<std::size_t>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4;
            u8 value = static_cast<u8>(x * 255 / std::max(1u, width - 1));
            if (x > width / 2 && y > height / 2) value = 255;  // bright quadrant
            pixels[o + 0] = value;
            pixels[o + 1] = static_cast<u8>(value / 2);
            pixels[o + 2] = static_cast<u8>(value / 4);
            pixels[o + 3] = 255;
        }
    }
    return pixels;
}

filters::Image view(std::vector<u8>& pixels, u32 width, u32 height) {
    return filters::Image{pixels.data(), width, height};
}

// A filter written entirely outside filters/builtin/, to check that the
// extension point works for callers and is not just an internal convention.
// It inverts RGB and leaves alpha alone, which is easy to verify exactly.
class InvertFilter final : public filters::IFilter {
public:
    const filters::FilterInfo& info() const override {
        static const filters::FilterInfo kInfo = [] {
            filters::FilterInfo info;
            info.name = "test_invert";
            info.description = "Inverts RGB; defined in the test binary, not the library";
            info.target = filters::FilterTarget::CpuOnly;
            info.min_passes = 1;
            info.max_passes = 1;
            return info;
        }();
        return kInfo;
    }

    lx::Status apply(const filters::ConstImage& source, filters::Image destination,
                     const filters::FilterParams& params) const override {
        if (!source.valid() || !destination.valid()) {
            return lx::Status::error(lx::ErrorCode::InvalidArgument, "invalid image");
        }
        if (source.width != destination.width || source.height != destination.height) {
            return lx::Status::error(lx::ErrorCode::InvalidArgument, "size mismatch");
        }
        if (params.intensity <= 0.0f) return lx::Status::success();
        for (u32 y = 0; y < source.height; ++y) {
            for (u32 x = 0; x < source.width; ++x) {
                const std::size_t o = (static_cast<std::size_t>(y) * source.width + x) * 4;
                destination.pixels[o + 0] = static_cast<u8>(255 - source.pixels[o + 0]);
                destination.pixels[o + 1] = static_cast<u8>(255 - source.pixels[o + 1]);
                destination.pixels[o + 2] = static_cast<u8>(255 - source.pixels[o + 2]);
                destination.pixels[o + 3] = source.pixels[o + 3];
            }
        }
        return lx::Status::success();
    }

    bool supports_in_place() const override { return true; }
};

}  // namespace

LX_TEST(filters, external_filter_can_register_and_run_in_chain) {
    // The claim being tested is that a new filter does not require editing the
    // pipeline. Register it, look it up by name, build it, and run it through the
    // real chain exactly as a built-in would be run.
    const std::string name = "test_invert";
    if (filters::find_filter(name) == nullptr) {
        filters::FilterRegistration registration;
        registration.info = InvertFilter{}.info();
        registration.factory = []() -> filters::FilterPtr {
            return std::make_unique<InvertFilter>();
        };
        filters::register_filter(registration);
    }

    const filters::FilterRegistration* found = filters::find_filter(name);
    LX_CHECK(found != nullptr);
    if (found == nullptr) return;

    auto created = filters::create_filter(name);
    LX_CHECK(created.ok());
    if (!created.ok()) return;
    LX_CHECK_EQ(created.value()->info().name, name);

    const auto names = filters::filter_names();
    LX_CHECK(std::find(names.begin(), names.end(), name) != names.end());

    // Run it through the chain and verify the exact result.
    constexpr u32 kW = 8;
    constexpr u32 kH = 4;
    std::vector<u8> pixels = make_image(kW, kH);
    const std::vector<u8> original = pixels;

    filters::FilterChain chain;
    filters::FilterParams params;
    params.intensity = 1.0f;
    LX_CHECK(chain.add(std::move(created.value()), params).ok());
    LX_CHECK_EQ(chain.size(), std::size_t{1});

    memory::BufferPool pool;
    filters::Image image = view(pixels, kW, kH);
    LX_CHECK(chain.run(image, pool).ok());

    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        LX_CHECK_EQ(static_cast<int>(pixels[i + 0]), 255 - static_cast<int>(original[i + 0]));
        LX_CHECK_EQ(static_cast<int>(pixels[i + 1]), 255 - static_cast<int>(original[i + 1]));
        LX_CHECK_EQ(static_cast<int>(pixels[i + 2]), 255 - static_cast<int>(original[i + 2]));
        LX_CHECK_EQ(static_cast<int>(pixels[i + 3]), static_cast<int>(original[i + 3]));
    }
}

LX_TEST(filters, external_filter_honours_zero_intensity) {
    // Intensity 0 is documented as a no-op for every filter, so an external one
    // must respect it too rather than relying on the chain to skip it.
    std::vector<u8> pixels = make_image(8, 4);
    const std::vector<u8> original = pixels;
    InvertFilter filter;
    filters::FilterParams params;
    params.intensity = 0.0f;
    filters::Image image = view(pixels, 8, 4);
    LX_CHECK(filter.apply(filters::ConstImage{image.pixels, image.width, image.height}, image,
                         params).ok());
    LX_CHECK(pixels == original);
}

LX_TEST(filters, external_filter_rejects_mismatched_images) {
    // The interface documents InvalidArgument for bad input rather than reading
    // out of bounds; check an external implementation gets the same treatment
    // the framework relies on.
    std::vector<u8> source = make_image(8, 4);
    std::vector<u8> dest = make_image(4, 4);
    InvertFilter filter;
    filters::FilterParams params;
    filters::Image src_view = view(source, 8, 4);
    filters::Image dst_view = view(dest, 4, 4);
    const auto status =
        filter.apply(filters::ConstImage{src_view.pixels, src_view.width, src_view.height},
                     dst_view, params);
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);
}

LX_TEST(filters, registry_contains_builtins) {
    const auto names = filters::filter_names();
    for (const char* expected : {"tone", "blur", "sharpen", "bloom", "vignette"}) {
        LX_CHECK(std::find(names.begin(), names.end(), expected) != names.end());
    }
    auto created = filters::create_filter("tone");
    LX_CHECK_OK(created.status());
    auto unknown = filters::create_filter("does_not_exist");
    LX_CHECK(!unknown.ok());
    LX_CHECK(unknown.status().code() == ErrorCode::NotImplemented);
}

LX_TEST(filters, tone_intensity_zero_is_noop) {
    const u32 w = 32, h = 32;
    auto source = make_image(w, h);
    auto original = source;
    std::vector<u8> destination(source.size(), 0);

    auto filter = filters::create_filter("tone");
    LX_CHECK_OK(filter.status());
    filters::FilterParams params;
    params.intensity = 0.0f;
    LX_CHECK_OK(filter.value()->apply(
        filters::ConstImage{source.data(), w, h}, view(destination, w, h), params));
    LX_CHECK(source == original);
    LX_CHECK(destination == original);
}

LX_TEST(filters, tone_neutral_params_are_noop) {
    const u32 w = 16, h = 16;
    auto source = make_image(w, h);
    std::vector<u8> destination(source.size(), 0);

    auto tone = filters::create_filter("tone");
    LX_CHECK_OK(tone.status());
    filters::FilterParams params;
    params.intensity = 1.0f;  // full strength, but the grade itself is neutral
    LX_CHECK_OK(tone.value()->apply(filters::ConstImage{source.data(), w, h},
                                    view(destination, w, h), params));

    // Default exposure 0, contrast 0, saturation 1 => identity transform.
    LX_CHECK(source == destination);
}

LX_TEST(filters, tone_exposure_brightens) {
    const u32 w = 16, h = 16;
    auto source = make_image(w, h);
    std::vector<u8> destination(source.size(), 0);

    auto tone = filters::create_filter("tone");
    LX_CHECK_OK(tone.status());
    auto* typed = static_cast<filters::ToneFilter*>(tone.value().get());
    typed->set_exposure_stops(1.0f);  // one stop brighter

    filters::FilterParams params;
    params.intensity = 1.0f;
    LX_CHECK_OK(tone.value()->apply(filters::ConstImage{source.data(), w, h},
                                    view(destination, w, h), params));

    const u64 before = std::accumulate(source.begin(), source.end(), u64{0});
    const u64 after = std::accumulate(destination.begin(), destination.end(), u64{0});
    LX_CHECK(after > before);  // +1 stop raises every channel
}

LX_TEST(filters, blur_smooths_high_frequency_edge) {
    const u32 w = 32, h = 32;
    auto source = make_image(w, h);
    std::vector<u8> destination(source.size(), 0);

    auto blur = filters::create_filter("blur");
    LX_CHECK_OK(blur.status());
    LX_CHECK(!blur.value()->supports_in_place());  // must not run in place

    filters::FilterParams params;
    params.intensity = 1.0f;
    LX_CHECK_OK(blur.value()->apply(filters::ConstImage{source.data(), w, h},
                                    view(destination, w, h), params));

    // Total energy (sum) is roughly preserved by a normalised blur.
    const u64 before = std::accumulate(source.begin(), source.end(), u64{0});
    const u64 after = std::accumulate(destination.begin(), destination.end(), u64{0});
    const double ratio = static_cast<double>(after) / static_cast<double>(before);
    LX_CHECK(ratio > 0.9 && ratio < 1.1);
}

LX_TEST(filters, vignette_darkens_corners) {
    const u32 w = 64, h = 64;
    std::vector<u8> source(static_cast<std::size_t>(w) * h * 4, 200);
    for (std::size_t i = 3; i < source.size(); i += 4) source[i] = 255;
    std::vector<u8> destination(source.size(), 0);

    auto vignette = filters::create_filter("vignette");
    LX_CHECK_OK(vignette.status());
    filters::FilterParams params;
    params.intensity = 1.0f;
    LX_CHECK_OK(vignette.value()->apply(filters::ConstImage{source.data(), w, h},
                                        view(destination, w, h), params));

    // Corner pixel must be darker than the centre pixel.
    const std::size_t corner = 0;
    const std::size_t centre = (static_cast<std::size_t>(h / 2) * w + w / 2) * 4;
    LX_CHECK(destination[corner] < destination[centre]);
}

LX_TEST(filters, filters_reject_size_mismatch) {
    const u32 w = 16, h = 16;
    auto source = make_image(w, h);
    std::vector<u8> wrong(8 * 8 * 4, 0);

    auto blur = filters::create_filter("blur");
    LX_CHECK_OK(blur.status());
    filters::FilterParams params;
    const Status status = blur.value()->apply(
        filters::ConstImage{source.data(), w, h}, view(wrong, 8, 8), params);
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);
}

LX_TEST(filters, chain_applies_in_order) {
    const u32 w = 32, h = 32;
    auto image = make_image(w, h);
    auto original = image;

    filters::FilterChain chain;
    memory::BufferPool pool;

    auto tone = filters::create_filter("tone");
    LX_CHECK_OK(tone.status());
    filters::FilterParams tone_params;
    tone_params.intensity = 0.5f;
    LX_CHECK_OK(chain.add(std::move(tone.value()), tone_params));

    auto vignette = filters::create_filter("vignette");
    LX_CHECK_OK(vignette.status());
    filters::FilterParams vignette_params;
    vignette_params.intensity = 0.5f;
    LX_CHECK_OK(chain.add(std::move(vignette.value()), vignette_params));

    LX_CHECK_EQ(chain.size(), static_cast<std::size_t>(2));
    filters::ChainStats stats;
    LX_CHECK_OK(chain.run(view(image, w, h), pool, nullptr, &stats));
    LX_CHECK_EQ(stats.filters_applied, static_cast<u32>(2));
    LX_CHECK_EQ(stats.per_filter_nanoseconds.size(), static_cast<std::size_t>(2));
    LX_CHECK(image != original);  // the chain changed the image
}

LX_TEST(filters, chain_applies_passes_the_requested_number_of_times) {
    // `passes` was plumbed from the config, through the GUI and the JSON
    // round-trip, and then never read by any code path, so changing it had no
    // effect at all. It now iterates the filter. Blurring twice must differ from
    // blurring once -- otherwise the parameter is still decorative.
    auto run_with_passes = [](u32 passes) {
        std::vector<u8> pixels = make_image(64, 64);
        memory::BufferPool pool;
        filters::FilterChain chain;
        filters::FilterParams params;
        params.passes = passes;
        auto blur = filters::create_filter("blur");
        LX_CHECK(blur.ok());
        LX_CHECK_OK(chain.add(std::move(blur.value()), params));
        LX_CHECK_OK(chain.run(view(pixels, 64, 64), pool));
        return pixels;
    };

    const auto once = run_with_passes(1);
    const auto thrice = run_with_passes(3);
    LX_CHECK(once != thrice);

    // More passes blur more: total variation (sum of absolute neighbour
    // differences) falls as the gradient softens.
    auto variation = [](const std::vector<u8>& px) {
        u64 total = 0;
        for (std::size_t i = 4; i < px.size(); i += 4) {
            total += static_cast<u64>(std::abs(static_cast<int>(px[i]) -
                                               static_cast<int>(px[i - 4])));
        }
        return total;
    };
    LX_CHECK(variation(thrice) < variation(once));
}

LX_TEST(filters, chain_clamps_passes_to_the_filters_declared_range) {
    // Asking for more passes than the filter supports must not silently run more
    // of them, and must not fail either: the filter's declared range wins.
    std::vector<u8> many = make_image(32, 32);
    std::vector<u8> capped = make_image(32, 32);
    memory::BufferPool pool;

    auto blur = filters::create_filter("blur");
    LX_CHECK(blur.ok());
    const filters::FilterInfo info = blur.value()->info();

    filters::FilterChain chain_many;
    filters::FilterParams p_many;
    p_many.passes = info.max_passes + 100;
    LX_CHECK_OK(chain_many.add(std::move(blur.value()), p_many));
    LX_CHECK_OK(chain_many.run(view(many, 32, 32), pool));

    filters::FilterChain chain_capped;
    filters::FilterParams p_capped;
    p_capped.passes = info.max_passes;
    auto blur_capped = filters::create_filter("blur");
    LX_CHECK(blur_capped.ok());
    LX_CHECK_OK(chain_capped.add(std::move(blur_capped.value()), p_capped));
    LX_CHECK_OK(chain_capped.run(view(capped, 32, 32), pool));

    LX_CHECK(many == capped);
}

LX_TEST(filters, chain_skips_filters_out_of_scope) {
    // A game-scoped filter must be skipped on a system frame, leaving the image
    // untouched, and applied on a game frame.
    const u32 w = 32, h = 32;
    auto image = make_image(w, h);
    auto original = image;

    filters::FilterChain chain;
    memory::BufferPool pool;
    auto vignette = filters::create_filter("vignette");
    LX_CHECK_OK(vignette.status());
    filters::FilterParams params;
    params.intensity = 1.0f;
    params.scope = filters::ContentScope::Game;
    LX_CHECK_OK(chain.add(std::move(vignette.value()), params));

    filters::ChainStats system_stats;
    LX_CHECK_OK(chain.run(view(image, w, h), pool, nullptr, &system_stats,
                          filters::ContentScope::System));
    LX_CHECK_EQ(system_stats.filters_applied, static_cast<u32>(0));
    LX_CHECK(image == original);

    filters::ChainStats game_stats;
    LX_CHECK_OK(chain.run(view(image, w, h), pool, nullptr, &game_stats,
                          filters::ContentScope::Game));
    LX_CHECK_EQ(game_stats.filters_applied, static_cast<u32>(1));
    LX_CHECK(image != original);
}

LX_TEST(filters, content_scope_helpers) {
    LX_CHECK(filters::content_scope_from_string("game") == filters::ContentScope::Game);
    LX_CHECK(filters::content_scope_from_string("system") == filters::ContentScope::System);
    LX_CHECK(filters::content_scope_from_string("any") == filters::ContentScope::Unknown);
    LX_CHECK(filters::content_scope_from_string("garbage") == filters::ContentScope::Unknown);

    // An unknown frame matches every filter; a scoped filter only matches its
    // own content; a scope-less filter matches everything.
    LX_CHECK(filters::scope_matches(filters::ContentScope::Game,
                                    filters::ContentScope::Unknown));
    LX_CHECK(filters::scope_matches(filters::ContentScope::Game,
                                    filters::ContentScope::Game));
    LX_CHECK(!filters::scope_matches(filters::ContentScope::Game,
                                     filters::ContentScope::System));
    LX_CHECK(filters::scope_matches(filters::ContentScope::Unknown,
                                    filters::ContentScope::System));
}

LX_TEST(filters, chain_with_neighbour_filter_uses_scratch) {
    const u32 w = 32, h = 32;
    auto image = make_image(w, h);

    filters::FilterChain chain;
    memory::BufferPool pool;
    auto blur = filters::create_filter("blur");
    LX_CHECK_OK(blur.status());
    filters::FilterParams params;
    params.intensity = 1.0f;
    LX_CHECK_OK(chain.add(std::move(blur.value()), params));

    LX_CHECK_OK(chain.run(view(image, w, h), pool, nullptr, nullptr));
    // The pool must have taken the scratch buffer back.
    LX_CHECK(pool.stats().pooled_blocks >= 1);
}

LX_TEST(filters, chain_rejects_absurd_image_dimensions) {
    // width*height*4 would wrap size_t; the chain must refuse the image before
    // any filter indexes into it.
    filters::FilterChain chain;
    memory::BufferPool pool;
    auto tone = filters::create_filter("tone");
    LX_CHECK_OK(tone.status());
    filters::FilterParams params;
    LX_CHECK_OK(chain.add(std::move(tone.value()), params));

    u8 dummy = 0;
    filters::Image huge{&dummy, 0xFFFFFFFFu, 0xFFFFFFFFu};
    const Status status = chain.run(huge, pool, nullptr, nullptr);
    LX_CHECK(!status.ok());
    LX_CHECK(status.code() == ErrorCode::InvalidArgument);
}

// Runs one filter through the chain twice, once with no thread pool (serial)
// and once with a pool (parallel bands), and asserts the two outputs are
// byte-identical. This is the correctness contract for the row-splitting path:
// parallelism must not change a single pixel.
void check_parallel_matches_serial(const char* name, u32 w, u32 h, f32 intensity) {
    auto serial_image = make_image(w, h);
    auto parallel_image = serial_image;

    filters::FilterParams params;
    params.intensity = intensity;

    {
        filters::FilterChain chain;
        memory::BufferPool pool;
        auto filter = filters::create_filter(name);
        LX_CHECK_OK(filter.status());
        LX_CHECK_OK(chain.add(std::move(filter.value()), params));
        LX_CHECK_OK(chain.run(view(serial_image, w, h), pool, nullptr, nullptr));
    }
    {
        scheduler::ThreadPoolConfig config;
        config.thread_count = 4;
        scheduler::ThreadPool thread_pool(config);
        LX_CHECK_OK(thread_pool.start());

        filters::FilterChain chain;
        memory::BufferPool pool;
        auto filter = filters::create_filter(name);
        LX_CHECK_OK(filter.status());
        LX_CHECK_OK(chain.add(std::move(filter.value()), params));
        LX_CHECK_OK(chain.run(view(parallel_image, w, h), pool, &thread_pool, nullptr));
        thread_pool.stop();
    }

    LX_CHECK(serial_image == parallel_image);
}

LX_TEST(filters, parallel_bands_match_serial) {
    // Heights chosen so bands do not divide evenly, exercising the remainder
    // path, and well above the minimum band size so splitting actually happens.
    for (u32 h : {33u, 64u, 100u, 257u}) {
        for (const char* name : {"tone", "blur", "sharpen", "bloom", "vignette"}) {
            check_parallel_matches_serial(name, 48, h, 0.75f);
        }
    }
}

LX_TEST(filters, parallel_matches_serial_full_intensity) {
    // Intensity 1.0 and the no-op path (0.0) must also agree between the serial
    // and parallel code paths.
    for (const char* name : {"tone", "blur", "sharpen", "bloom", "vignette"}) {
        check_parallel_matches_serial(name, 64, 80, 1.0f);
        check_parallel_matches_serial(name, 64, 80, 0.0f);
    }
}

LX_TEST(filters, chain_parallel_matches_serial) {
    // A multi-filter chain (in-place + neighbour filters) must also produce the
    // same bytes whether it runs serially or across bands.
    const u32 w = 96, h = 128;
    auto serial_image = make_image(w, h);
    auto parallel_image = serial_image;

    auto build = [&](filters::FilterChain& chain) {
        for (const char* name : {"tone", "blur", "vignette", "sharpen"}) {
            auto filter = filters::create_filter(name);
            if (filter.ok()) chain.add(std::move(filter.value()), {});
        }
    };

    {
        filters::FilterChain chain;
        memory::BufferPool pool;
        build(chain);
        LX_CHECK_OK(chain.run(view(serial_image, w, h), pool, nullptr, nullptr));
    }
    {
        scheduler::ThreadPoolConfig config;
        config.thread_count = 3;
        scheduler::ThreadPool thread_pool(config);
        LX_CHECK_OK(thread_pool.start());

        filters::FilterChain chain;
        memory::BufferPool pool;
        build(chain);
        LX_CHECK_OK(chain.run(view(parallel_image, w, h), pool, &thread_pool, nullptr));
        thread_pool.stop();
    }

    LX_CHECK(serial_image == parallel_image);
}

LX_TEST_MAIN("filters")
