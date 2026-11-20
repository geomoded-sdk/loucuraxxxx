set(LX_DRIVER_SOURCES driver.cpp)

if(WIN32)
    if(LX_ENABLE_WINDOWS)
        list(APPEND LX_DRIVER_SOURCES windows/windows_integration.cpp)
        set(LX_DRIVER_LIBS dxgi)
    endif()
elseif(APPLE)
    # This TU uses Objective-C messaging and must be compiled as Objective-C++.
    list(APPEND LX_DRIVER_SOURCES macos/macos_integration.mm)
    enable_language(OBJCXX)
    set_source_files_properties(macos/macos_integration.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    set(LX_DRIVER_LIBS "-framework Metal" "-framework Foundation")
else()
    if(LX_ENABLE_LINUX)
        list(APPEND LX_DRIVER_SOURCES linux/linux_integration.cpp)
    endif()
    set(LX_DRIVER_LIBS "")
endif()

add_library(lx_driver STATIC ${LX_DRIVER_SOURCES})
target_include_directories(lx_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_driver PUBLIC
    lx_core lx_hardware lx_memory lx_backends lx_configurator ${LX_DRIVER_LIBS})
