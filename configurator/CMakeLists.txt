add_library(lx_configurator STATIC
    configurator.cpp
)
target_include_directories(lx_configurator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_configurator PUBLIC lx_core lx_hardware lx_memory lx_backends lx_filters)
