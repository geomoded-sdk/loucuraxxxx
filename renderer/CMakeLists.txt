add_library(lx_renderer STATIC
    frame_stats.cpp
    renderer.cpp
)
target_include_directories(lx_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_renderer PUBLIC
    lx_core lx_backends lx_filters lx_memory lx_scheduler lx_shaders)
