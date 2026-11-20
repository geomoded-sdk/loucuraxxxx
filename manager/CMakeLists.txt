add_library(lx_manager STATIC
    work_queue.cpp
    telemetry.cpp
    manager.cpp
)
target_include_directories(lx_manager PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_manager PUBLIC
    lx_core lx_backends lx_memory lx_scheduler lx_configurator)
