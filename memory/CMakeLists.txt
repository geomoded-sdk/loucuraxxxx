add_library(lx_memory STATIC
    arena.cpp
    memory_budget.cpp
    buffer_pool.cpp
)
target_include_directories(lx_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_memory PUBLIC lx_core lx_hardware)
