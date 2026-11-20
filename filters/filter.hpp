// LoucuraXXXX GPU - modular graphics filter interface.
//
// A filter is a self-contained image transformation with its own parameters.
// Filters are composed into an ordered chain applied just before presentation.
// Each filter declares whether it can run on the CPU, on the GPU, or both, so
// the manager can place it using the same cost model as any other pass.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"
#include "filters/image.hpp"

namespace lx::filters {

// Which executor a filter supports.
enum class FilterTarget { CpuOnly, GpuOnly, Both };

// What a frame is, so filters can be applied selectively. A game frame and the
// operating system's own desktop are different content: a filter tuned for a
// game (a strong tone map, bloom) usually should not be applied to the system UI,
// where it would just hurt legibility. The renderer tags each frame with a scope
// and only runs the filters whose own scope matches. `Unknown` means the caller
// did not classify the frame; it matches any filter scope so nothing is skipped
// by accident.
enum class ContentScope { Unknown, Game, System };

inline const char* to_string(ContentScope scope) {
    switch (scope) {
        case ContentScope::Unknown: return "unknown";
        case ContentScope::Game: return "game";
        case ContentScope::System: return "system";
    }
    return "unknown";
}

// Parses a scope name; unrecognised names yield Unknown.
ContentScope content_scope_from_string(const std::string& name);

// Whether a filter with scope `filter_scope` applies to a frame with scope
// `frame_scope`. A filter scoped to one kind runs only on that kind; a frame
// that was not classified matches every filter.
bool scope_matches(ContentScope filter_scope, ContentScope frame_scope);

// Parameters shared by every filter. A filter reads intensity and passes and
// may expose additional typed parameters through its own setters.
struct FilterParams {
    f32 intensity = 1.0f;  // 0 => no-op, 1 => full strength
    u32 passes = 1;        // iterations; filters may clamp this
    std::string quality = "balanced";
    // Which content this instance applies to. Unknown (the default) applies to
    // every frame, which keeps existing configurations behaving as before.
    ContentScope scope = ContentScope::Unknown;
};

// Static description of a filter, used for discovery and documentation.
struct FilterInfo {
    std::string name;
    std::string description;
    FilterTarget target = FilterTarget::CpuOnly;
    Maturity maturity = Maturity::Implemented;
    u32 min_passes = 1;
    u32 max_passes = 4;
    f32 default_intensity = 1.0f;
};

// A single GPU pass: a shader, its push-constant block, and the logical texture
// slots it reads and writes. Texture slots are resolved by the caller:
//   0 = the filter's input, 1 = the filter's output, 2/3 = scratch textures.
struct GpuPass {
    std::string shader_name;         // built-in shader the backend compiles
    std::vector<u8> push_constants;  // raw bytes matching the shader's block
    std::vector<u32> inputs;         // logical texture slots this pass reads
    u32 output = 1;                  // logical texture slot this pass writes
};

// A filter's GPU execution plan: the ordered passes that reproduce apply() on a
// GPU backend. `supported == false` means the filter has no GPU implementation
// and must run on the CPU.
struct GpuFilterPlan {
    std::vector<GpuPass> passes;
    u32 scratch_textures = 0;  // 0, 1 or 2 extra textures the plan needs
    bool supported = false;
};

// The filter interface. apply() must be pure with respect to its inputs: it may
// read `source` and must write only to `destination`. A filter that works in
// place is expressed by passing the same image as both, which the framework
// permits only when supports_in_place() is true.
class IFilter {
public:
    virtual ~IFilter() = default;

    virtual const FilterInfo& info() const = 0;

    // Applies the filter. Implementations must validate their inputs and return
    // InvalidArgument rather than reading out of bounds.
    virtual Status apply(const ConstImage& source, Image destination,
                         const FilterParams& params) const = 0;

    // Applies the filter to a horizontal band of rows [row_begin, row_end).
    //
    // Contract: the whole of `source` is readable (so a filter that reads
    // neighbours can reach the halo it needs), and only rows [row_begin,
    // row_end) of `destination` may be written. Distinct bands must be
    // independent: no band may depend on a row another band writes. That holds
    // when every band reads only `source`, which the chain guarantees by
    // handing a non-in-place filter a scratch destination separate from the
    // source image.
    //
    // A filter that can be split overrides supports_row_split() to return true
    // and implements this; the chain then runs the bands on its thread pool.
    // The default delegates to apply() and is therefore only correct for the
    // full range, which is exactly how the chain uses it when a filter does not
    // support splitting.
    virtual Status apply_rows(const ConstImage& source, Image destination,
                              const FilterParams& params, u32 row_begin,
                              u32 row_end) const {
        (void)row_begin;
        (void)row_end;
        return apply(source, destination, params);
    }

    // True when apply_rows() may be called with a strict sub-range of rows.
    virtual bool supports_row_split() const { return false; }

    // True when apply() may be called with source and destination aliasing the
    // same buffer. Filters that need a neighbour read must return false so the
    // framework can allocate a scratch buffer.
    virtual bool supports_in_place() const { return false; }

    // Optional: a stable identifier for the GPU kernel that implements this
    // filter, or empty when no GPU implementation exists.
    virtual std::string gpu_kernel_name() const { return {}; }

    // Optional: describes this filter as an ordered list of GPU passes, so the
    // renderer can run it on a GPU backend instead of the host. Returns an
    // unsupported plan by default. A filter that returns a supported plan must
    // produce the same result on the GPU as apply() does on the CPU, within
    // float rounding; the GPU backend tests enforce that.
    virtual GpuFilterPlan gpu_plan(u32 width, u32 height,
                                   const FilterParams& params) const {
        (void)width;
        (void)height;
        (void)params;
        return GpuFilterPlan{};
    }
};

using FilterPtr = std::unique_ptr<IFilter>;

}  // namespace lx::filters
