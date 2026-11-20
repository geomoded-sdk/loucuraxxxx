// LoucuraXXXX GPU - ordered filter chain.
//
// Composes filters into a pipeline and runs them with correct buffer handling:
// in-place filters write directly; filters that need neighbours receive a
// scratch buffer that is ping-ponged, so the caller never has to reason about
// aliasing. The chain is the unit the renderer and manager schedule.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "filters/filter.hpp"
#include "filters/image.hpp"
#include "memory/buffer_pool.hpp"
#include "scheduler/thread_pool.hpp"

namespace lx::filters {

struct ChainEntry {
    FilterPtr filter;
    FilterParams params;
    bool enabled = true;
};

// Timing and counters from a chain execution, for telemetry.
// Timings are nanoseconds: a cheap filter on a small image can take less than a
// microsecond, and storing microseconds turned those into 0.
struct ChainStats {
    u32 filters_applied = 0;
    u64 total_nanoseconds = 0;
    std::vector<std::pair<std::string, u64>> per_filter_nanoseconds;
};

class FilterChain {
public:
    FilterChain() = default;

    // Adds a filter to the end of the chain.
    Status add(FilterPtr filter, const FilterParams& params);

    // Removes every filter. Does not free pooled buffers.
    void clear();

    std::size_t size() const { return entries_.size(); }
    const std::vector<ChainEntry>& entries() const { return entries_; }

    // Runs the chain over `image` in place, allocating scratch buffers from
    // `pool` for filters that cannot run in place. The image is the final
    // result on success.
    //
    // `pool` (thread pool) is optional: when provided, filters that advertise
    // parallel support may be split across rows. It is used when non-null and
    // the image is large enough to be worth it.
    //
    // `frame_scope` classifies the frame. A filter whose own scope does not
    // match is skipped, which is how a game-only filter avoids touching the
    // system UI. The default (Unknown) applies every enabled filter.
    Status run(Image image, memory::BufferPool& pool,
               scheduler::ThreadPool* thread_pool = nullptr,
               ChainStats* stats = nullptr,
               ContentScope frame_scope = ContentScope::Unknown);

    // Number of bytes the chain will need for scratch buffers at this size.
    static u64 scratch_bytes_for(const Image& image);

private:
    std::vector<ChainEntry> entries_;
};

}  // namespace lx::filters
