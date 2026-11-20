#include "filters/filter.hpp"

namespace lx::filters {

ContentScope content_scope_from_string(const std::string& name) {
    if (name == "game") return ContentScope::Game;
    if (name == "system" || name == "os" || name == "desktop") return ContentScope::System;
    return ContentScope::Unknown;
}

bool scope_matches(ContentScope filter_scope, ContentScope frame_scope) {
    // An unclassified frame matches everything: the caller did not say what the
    // frame is, so skipping a filter would be a silent surprise.
    if (frame_scope == ContentScope::Unknown) return true;
    // A filter with no declared scope applies to every kind of frame.
    if (filter_scope == ContentScope::Unknown) return true;
    return filter_scope == frame_scope;
}

}  // namespace lx::filters
