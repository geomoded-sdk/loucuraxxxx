add_library(lx_scheduler STATIC
    thread_pool.cpp
    task_graph.cpp
    cost_model.cpp
)
target_include_directories(lx_scheduler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_scheduler PUBLIC lx_core lx_hardware)
