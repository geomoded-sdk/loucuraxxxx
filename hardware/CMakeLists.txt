set(LX_HARDWARE_SOURCES
    hardware_info.cpp
)

if(WIN32)
    list(APPEND LX_HARDWARE_SOURCES platform_windows.cpp)
    set(LX_HARDWARE_LIBS dxgi)
elseif(APPLE)
    # This TU uses Objective-C messaging and must be compiled as Objective-C++.
    list(APPEND LX_HARDWARE_SOURCES platform_macos.mm)
    enable_language(OBJCXX)
    set_source_files_properties(platform_macos.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set(LX_HARDWARE_LIBS "-framework Foundation" "-framework Metal")
else()
    list(APPEND LX_HARDWARE_SOURCES platform_linux.cpp)
    set(LX_HARDWARE_LIBS "")
endif()

add_library(lx_hardware STATIC ${LX_HARDWARE_SOURCES})
target_include_directories(lx_hardware PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_hardware PUBLIC lx_core ${LX_HARDWARE_LIBS})
