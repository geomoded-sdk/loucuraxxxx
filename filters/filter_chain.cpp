#include "filters/filter_chain.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "core/clock.hpp"
#include "core/log.hpp"

namespace lx::filters {

namespace {

// Below this many rows, splitting costs more in dispatch than it saves.
constexpr u32 kMinRowsPerBand = 16;

// Splits [0, height) into contiguous bands and runs `fn(begin, end)` on the
// pool, returning the first failure. When there is no usable pool, or the image
// is too short to split usefully, it runs one band over the whole range so the
// result is identical to the serial path.
//
// Each band is submitted to a fixed lane so a band's rows stay on one worker;
// the bands are independent by contract (a filter reads only `source` and
// writes only its own rows), so no ordering between them is required.
Status run_bands(u32 height, scheduler::ThreadPool* pool,
                 const std::function<Status(u32, u32)>& fn) {
    const u32 workers = pool != nullptr && pool->running() ? pool->thread_count() : 0;
    if (workers <= 1 || height < kMinRowsPerBand * 2) {
        return fn(0, height);
    }

    u32 bands = std::min(workers, height / kMinRowsPerBand);
    if (bands <= 1) return fn(0, height);

    std::vector<Status> errors(bands);
    std::atomic<bool> failed{false};

    const u32 base = height / bands;
    const u32 extra = height % bands;
    u32 begin = 0;
    for (u32 i = 0; i < bands; ++i) {
        const u32 rows = base + (i < extra ? 1 : 0);
        const u32 end = begin + rows;
        const u32 band_begin = begin;
        const u32 band_end = end;
        const Status submitted = pool->submit(
            [&fn, &errors, &failed, i, band_begin, band_end] {
                if (failed.load(std::memory_order_relaxed)) return;
                const Status status = fn(band_begin, band_end);
                if (!status.ok()) {
                    errors[i] = status;
                    failed.store(true, std::memory_order_relaxed);
                }
            },
            i);
        if (!submitted.ok()) {
            // The pool refused a band (it stopped mid-run). Fall back to running
            // the remaining range serially so the frame is still produced.
            const Status direct = fn(band_begin, band_end);
            if (!direct.ok()) return direct;
        }
        begin = end;
    }

    LX_TRY(pool->wait_idle());
    for (const Status& error : errors) {
        if (!error.ok()) return error;
    }
    return Status::success();
}
}  // namespace

Status FilterChain::add(FilterPtr filter, const FilterParams& params) {
    if (!filter) {
        return Status::error(ErrorCode::InvalidArgument, "cannot add a null filter");
    }
    ChainEntry entry;
    entry.filter = std::move(filter);
    entry.params = params;
    entries_.push_back(std::move(entry));
    return Status::success();
}

void FilterChain::clear() { entries_.clear(); }

u64 FilterChain::scratch_bytes_for(const Image& image) {
    return image.byte_size();
}

Status FilterChain::run(Image image, memory::BufferPool& pool,
                        scheduler::ThreadPool* thread_pool, ChainStats* stats,
                        ContentScope frame_scope) {
    if (!image.valid()) {
        return Status::error(ErrorCode::InvalidArgument, "filter chain: invalid image");
    }
    // byte_size() is width*height*4; for absurd dimensions that product wraps
    // size_t and every filter would then index out of the real buffer. Reject
    // such an image before it reaches a filter.
    if (static_cast<u64>(image.width) * image.height >
        std::numeric_limits<std::size_t>::max() / 4) {
        return Status::error(ErrorCode::InvalidArgument,
                             "filter chain: image dimensions are absurd");
    }

    ChainStats local;
    const u64 chain_start = now_nanos();

    // The scratch buffer is only acquired when a filter needs it.
    memory::Buffer scratch;
    bool scratch_valid = false;

    for (auto& entry : entries_) {
        if (!entry.enabled) continue;
        if (!scope_matches(entry.params.scope, frame_scope)) continue;
        const FilterInfo& info = entry.filter->info();

        // `passes` is the number of times this filter is applied, clamped to the
        // range the filter declares. It used to be carried from the config, the
        // GUI and the JSON round-trip and then never read, so setting it did
        // nothing. Iterating the same filter is what the field always meant.
        const u32 iterations =
            std::min(std::max(entry.params.passes == 0 ? info.min_passes
                                                       : entry.params.passes,
                              info.min_passes),
                     info.max_passes);

        const u64 filter_start = now_nanos();
        Status status;
        for (u32 pass = 0; pass < iterations; ++pass) {
            if (entry.filter->supports_in_place()) {
                // In place: source and destination are the same buffer. A
                // row-splittable filter writes only the rows of its band and reads
                // no other row, so bands are independent and may run in parallel.
                const ConstImage source{image.pixels, image.width, image.height};
                if (entry.filter->supports_row_split()) {
                    status = run_bands(image.height, thread_pool,
                                       [&](u32 begin, u32 end) {
                                           return entry.filter->apply_rows(source, image,
                                                                           entry.params, begin,
                                                                           end);
                                       });
                } else {
                    status = entry.filter->apply(source, image, entry.params);
                }
            } else {
                if (!scratch_valid) {
                    auto acquired = pool.acquire(image.byte_size());
                    if (!acquired.ok()) return acquired.status();
                    scratch = std::move(acquired.value());
                    scratch_valid = true;
                }
                // The filter reads the whole source image and writes only its band's
                // rows of scratch. Every band reads the same, untouched image, so the
                // bands are independent. The copy back into the image must wait until
                // *all* bands have finished: copying band-by-band would let one band
                // overwrite image rows another band still has to read as its halo.
                Image scratch_view{scratch.data(), image.width, image.height};
                const ConstImage source{image.pixels, image.width, image.height};
                auto filter_band = [&](u32 begin, u32 end) -> Status {
                    return entry.filter->apply_rows(source, scratch_view, entry.params, begin,
                                                    end);
                };
                if (entry.filter->supports_row_split()) {
                    status = run_bands(image.height, thread_pool, filter_band);
                } else {
                    status = filter_band(0, image.height);
                }
                if (status.ok()) {
                    std::memcpy(image.pixels, scratch.data(),
                                static_cast<std::size_t>(image.byte_size()));
                }
            }
            if (!status.ok()) {
                if (scratch_valid) pool.release(std::move(scratch));
                return Status::error(status.code(),
                                     "filter '" + info.name + "' failed: " + status.message());
            }
        }  // pass loop

        const u64 elapsed = now_nanos() - filter_start;
        local.per_filter_nanoseconds.emplace_back(info.name, elapsed);
        ++local.filters_applied;
    }

    local.total_nanoseconds = now_nanos() - chain_start;
    if (scratch_valid) pool.release(std::move(scratch));
    if (stats) *stats = std::move(local);
    return Status::success();
}

}  // namespace lx::filters
