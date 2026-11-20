add_executable(lx_cli lx_cli.cpp)
target_link_libraries(lx_cli PRIVATE
    lx_core lx_hardware lx_configurator lx_driver lx_renderer lx_manager lx_filters)
