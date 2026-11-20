#include "filters/filter_registry.hpp"

#include <mutex>

namespace lx::filters {

namespace {
std::mutex g_mutex;
std::vector<FilterRegistration>& registry() {
    static std::vector<FilterRegistration> instance;
    return instance;
}
}  // namespace

void register_filter(const FilterRegistration& registration) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& reg = registry();
    for (auto& existing : reg) {
        if (existing.info.name == registration.info.name) {
            existing = registration;
            return;
        }
    }
    reg.push_back(registration);
}

void ensure_builtin_filters_registered() {
    static bool done = false;
    if (done) return;
    done = true;
    register_tone_filter();
    register_blur_filter();
    register_sharpen_filter();
    register_bloom_filter();
    register_vignette_filter();
}

const std::vector<FilterRegistration>& registered_filters() {
    ensure_builtin_filters_registered();
    return registry();
}

const FilterRegistration* find_filter(const std::string& name) {
    for (const auto& reg : registered_filters()) {
        if (reg.info.name == name) return &reg;
    }
    return nullptr;
}

Result<FilterPtr> create_filter(const std::string& name) {
    const FilterRegistration* reg = find_filter(name);
    if (reg == nullptr || !reg->factory) {
        return Status::error(ErrorCode::NotImplemented, "unknown filter '" + name + "'");
    }
    FilterPtr filter = reg->factory();
    if (!filter) {
        return Status::error(ErrorCode::InitializationFailed,
                             "filter factory for '" + name + "' returned null");
    }
    return filter;
}

std::vector<std::string> filter_names() {
    std::vector<std::string> names;
    for (const auto& reg : registered_filters()) names.push_back(reg.info.name);
    return names;
}

}  // namespace lx::filters
