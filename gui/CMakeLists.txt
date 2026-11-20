# LoucuraXXXX GPU - graphical front end.
#
# The model is a normal static library (testable headlessly). The front ends are
# platform specific and thin: Win32 on Windows, X11 on Linux. A headless mode
# runs the model without any window, which is what CI and the test suite use.

add_library(lx_gui_model STATIC
    gui_model.cpp
)
target_link_libraries(lx_gui_model PUBLIC
    lx_core lx_hardware lx_configurator lx_renderer lx_filters lx_backends)
target_include_directories(lx_gui_model PUBLIC ${CMAKE_SOURCE_DIR})

# The front end executable. It always builds: when no native windowing toolkit
# is available it degrades to a headless run (--headless), never to a stub that
# pretends to show a window.
add_executable(lx_gui
    main.cpp
    headless.cpp
)

set(LX_GUI_HAS_WINDOWING OFF)

if(WIN32)
    target_sources(lx_gui PRIVATE platform/gui_win32.cpp)
    # The Win32 front end uses the wide-character API exclusively; the macros
    # IDC_ARROW and friends must resolve to their LPCWSTR forms.
    target_compile_definitions(lx_gui PRIVATE UNICODE _UNICODE)
    target_link_libraries(lx_gui PRIVATE user32 gdi32 comctl32)
    set(LX_GUI_HAS_WINDOWING ON)
elseif(UNIX AND NOT APPLE)
    # X11 is optional: build the X11 front end only when the headers are present.
    find_package(X11)
    if(X11_FOUND)
        target_sources(lx_gui PRIVATE platform/gui_x11.cpp)
        target_link_libraries(lx_gui PRIVATE X11::X11)
        set(LX_GUI_HAS_WINDOWING ON)
    else()
        message(STATUS "GUI: X11 not found; lx_gui will build in headless-only mode")
    endif()
else()
    message(STATUS "GUI: no native windowing backend for this platform; headless only")
endif()

target_link_libraries(lx_gui PRIVATE lx_gui_model)

if(LX_GUI_HAS_WINDOWING)
    target_compile_definitions(lx_gui PRIVATE LX_GUI_HAS_WINDOWING=1)
else()
    target_compile_definitions(lx_gui PRIVATE LX_GUI_HAS_WINDOWING=0)
endif()
