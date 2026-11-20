cmake_minimum_required(VERSION 3.16)

project(LoucuraXXXX_GPU
    VERSION 0.0.2
    DESCRIPTION "LoucuraXXXX GPU - hybrid virtual GPU layer"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

add_compile_options(
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Threads are required everywhere.
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Options: each backend / integration can be disabled.
# ---------------------------------------------------------------------------
# The Vulkan backend is OPT-IN and OFF by default: it only works on machines
# whose GPU and driver expose Vulkan. The CPU backend is the working default.
#
# OpenGL 3.3 core is the opposite: it is the universal real-GPU fallback and is
# ON by default. It is runtime-loaded, so it needs no GL SDK and adds no link
# dependency; when no GL driver is present the backend reports BackendUnavailable
# and the ladder falls through to the CPU. Direct3D 11 is the Windows
# counterpart, also ON by default and built only where its headers exist.
option(LX_ENABLE_VULKAN  "Build the optional Vulkan backend" OFF)
# The ICD is the virtual adapter the Vulkan loader can enumerate. It is a separate
# shared library, off by default, and implies the Vulkan headers.
option(LX_BUILD_ICD      "Build the Vulkan ICD (virtual adapter) shared library" OFF)
option(LX_ENABLE_OPENGL  "Build the OpenGL 3.3 core backend"  ON)
option(LX_ENABLE_D3D11   "Build the Direct3D 11 backend (Windows)" ON)
option(LX_ENABLE_LINUX   "Build the Linux integration"       ON)
option(LX_ENABLE_WINDOWS "Build the Windows integration"     ON)
option(LX_ENABLE_METAL   "Build the Metal (macOS) backend"   ON)
option(LX_BUILD_TESTS    "Build unit and integration tests"  ON)
option(LX_BUILD_BENCHMARKS "Build benchmarks"                ON)
option(LX_ENABLE_INSTALL "Generate install rules and packages" ON)

# Sanitizers as first-class options rather than hand-passed flags. The test
# matrix (default, Vulkan opt-in, ASan/UBSan, TSan) is part of how this project
# claims correctness, so it has to be reproducible from a configure line. TSan and
# ASan are mutually exclusive: they cannot share a binary.
set(LX_SANITIZER "" CACHE STRING "Sanitizer to build with: '' , 'address' or 'thread'")
set_property(CACHE LX_SANITIZER PROPERTY STRINGS "" address thread)

if(LX_SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
elseif(LX_SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
elseif(NOT LX_SANITIZER STREQUAL "")
    message(FATAL_ERROR
        "LX_SANITIZER='${LX_SANITIZER}' is not supported; use 'address' or 'thread'")
endif()

include(LxHelpers)

# ---------------------------------------------------------------------------
# Core library: error handling, logging, JSON, filesystem helpers.
# ---------------------------------------------------------------------------
add_library(lx_core STATIC
    core/status.cpp
    core/clock.cpp
    core/log.cpp
    core/json.cpp
    core/files.cpp
)
target_include_directories(lx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lx_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Module libraries. Each is independent and links only what it needs.
# ---------------------------------------------------------------------------
add_subdirectory(hardware)
add_subdirectory(memory)
add_subdirectory(scheduler)
add_subdirectory(configurator)
add_subdirectory(backends)
add_subdirectory(shaders)
add_subdirectory(filters)
add_subdirectory(renderer)
add_subdirectory(driver)
add_subdirectory(manager)
add_subdirectory(gui)

# ---------------------------------------------------------------------------
# Tools, tests and benchmarks.
# ---------------------------------------------------------------------------
add_subdirectory(tools)

if(LX_BUILD_TESTS)
    enable_testing()
    # When the build uses AddressSanitizer, point LeakSanitizer at the project
    # suppression file so the known third-party Mesa/llvmpipe allocation from the
    # OpenGL probe (see cmake/lsan.supp) does not fail an otherwise clean run.
    if(LX_SANITIZER STREQUAL "address")
        set(LX_LSAN_OPTIONS "suppressions=${CMAKE_SOURCE_DIR}/cmake/lsan.supp:fast_unwind_on_malloc=0")
    endif()
    add_subdirectory(tests)
endif()

if(LX_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install rules and packaging.
#
# The runtime pieces are the three front ends (lx_cli, lx_gui, lx_bench) plus
# the shader assets the renderer loads at runtime. The shader assets are copied
# next to the executables under `shaders/` so the default working directory
# finds them, exactly as the build tree does.
# ---------------------------------------------------------------------------
if(LX_ENABLE_INSTALL)
    include(GNUInstallDirs)

    install(TARGETS lx_cli lx_gui lx_bench
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders/
        DESTINATION ${CMAKE_INSTALL_BINDIR}/shaders
        PATTERN "CMakeFiles" EXCLUDE
        PATTERN "Makefile" EXCLUDE
        PATTERN "cmake_install.cmake" EXCLUDE)

    install(FILES
        README.md
        documentation/API.md
        documentation/ARCHITECTURE.md
        documentation/BUILDING.md
        documentation/MODULE_STATUS.md
        documentation/PERFORMANCE.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

    set(CPACK_PACKAGE_NAME "LoucuraXXXX GPU")
    set(CPACK_PACKAGE_VENDOR "LoucuraXXXX")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "LoucuraXXXX GPU - hybrid virtual GPU layer (${PROJECT_VERSION})")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "LoucuraXXXX GPU")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

    # Archive fallback that works on every host, including a cross-build.
    set(CPACK_GENERATOR "ZIP")

    if(WIN32)
        # A real Windows installer (NSIS). The Start Menu entry launches the GUI;
        # the CLI and benchmark are installed beside it. The uninstaller is
        # registered by NSIS under Add/Remove Programs.
        list(APPEND CPACK_GENERATOR "NSIS")
        set(CPACK_NSIS_DISPLAY_NAME "LoucuraXXXX GPU ${PROJECT_VERSION}")
        set(CPACK_NSIS_PACKAGE_NAME "LoucuraXXXX GPU ${PROJECT_VERSION}")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        set(CPACK_NSIS_MODIFY_PATH OFF)
        set(CPACK_NSIS_MENU_LINKS
            "bin/lx_gui.exe" "LoucuraXXXX GPU"
            "bin/lx_cli.exe" "LoucuraXXXX GPU CLI")
        set(CPACK_PACKAGE_EXECUTABLES "lx_gui" "LoucuraXXXX GPU")
    endif()

    include(CPack)
endif()

message(STATUS "LoucuraXXXX GPU ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type ........ ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vulkan backend .... ${LX_ENABLE_VULKAN}")
message(STATUS "  Vulkan ICD ........ ${LX_BUILD_ICD}")
message(STATUS "  Linux integration . ${LX_ENABLE_LINUX}")
message(STATUS "  Windows integration ${LX_ENABLE_WINDOWS}")
message(STATUS "  Metal backend ..... ${LX_ENABLE_METAL}")
message(STATUS "  Tests ............. ${LX_BUILD_TESTS}")
message(STATUS "  Benchmarks ........ ${LX_BUILD_BENCHMARKS}")
