add_executable(lx_bench lx_bench.cpp)
target_link_libraries(lx_bench PRIVATE
    lx_core lx_hardware lx_configurator lx_renderer lx_manager lx_filters
    lx_backends lx_memory lx_scheduler)
# Benchmarks read the shader assets from the build root.
set_target_properties(lx_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
