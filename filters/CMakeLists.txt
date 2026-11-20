add_library(lx_filters STATIC
    filter_registry.cpp
    filter_chain.cpp
    content_scope.cpp
    builtin/tone_filter.cpp
    builtin/blur_filter.cpp
    builtin/sharpen_filter.cpp
    builtin/bloom_filter.cpp
    builtin/vignette_filter.cpp
)
target_include_directories(lx_filters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_filters PUBLIC lx_core lx_memory lx_scheduler)
