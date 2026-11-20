// LoucuraXXXX GPU - filter discovery and construction.
//
// Filters self-register a factory so new filters can be added without touching
// the pipeline. The registry also reports what is available, which is what the
// Configurator validates user filter lists against.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "filters/filter.hpp"

namespace lx::filters {

struct FilterRegistration {
    FilterInfo info;
    std::function<FilterPtr()> factory;
};

// Registers a filter. Called by ensure_builtin_filters_registered().
void register_filter(const FilterRegistration& registration);

// Registers the built-in filters compiled into this build, exactly once. Called
// lazily on first registry access. Explicit calls are used instead of file-scope
// initialisers because a static-library object that is never referenced would be
// dropped by the linker, silently losing its registration.
void ensure_builtin_filters_registered();

// Per-filter registration entry points (one translation unit each).
void register_tone_filter();
void register_blur_filter();
void register_sharpen_filter();
void register_bloom_filter();
void register_vignette_filter();

// All registered filters, in registration order.
const std::vector<FilterRegistration>& registered_filters();

// Looks up a registration by name. Returns nullptr when unknown.
const FilterRegistration* find_filter(const std::string& name);

// Constructs a filter by name. Returns NotImplemented when unknown.
Result<FilterPtr> create_filter(const std::string& name);

// Names of every registered filter.
std::vector<std::string> filter_names();

}  // namespace lx::filters
