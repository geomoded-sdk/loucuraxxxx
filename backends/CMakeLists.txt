# Backend abstraction + the concrete backends compiled into this build.
#
# The CPU backend is always built and is the universal fallback. The Vulkan
# backend is optional and disabled by default: it is only useful on a machine
# whose GPU and driver expose Vulkan, and the build must stay usable without it.

set(LX_BACKEND_SOURCES
    backend.cpp
    backend_registry.cpp
    cpu/cpu_backend.cpp
    cpu/cpu_registration.cpp
    # Manifest handling for the virtual adapter. Pure data and file I/O, no
    # Vulkan dependency, so it builds and is testable on any host.
    vulkan/icd/icd_manifest.cpp
)

set(LX_BACKEND_LIBS "")
set(LX_BACKEND_DEFINES "")

# --- OpenGL 3.3 core backend ---
#
# The GL backend is the universal real-GPU fallback: it runs on any OpenGL 3.3
# core driver, which covers essentially every GPU from the last decade,
# including machines with no Vulkan. It resolves EGL and GL at runtime
# (backends/opengl/gl_loader.cpp), so it needs no GL development headers and
# links against nothing: if the libraries are missing at run time the backend
# reports BackendUnavailable. That is why this is ON by default and builds on
# every platform without a GL SDK.
if(LX_ENABLE_OPENGL)
    list(APPEND LX_BACKEND_SOURCES
        opengl/gl_loader.cpp
        opengl/gl_backend.cpp
        opengl/gl_registration.cpp
    )
    list(APPEND LX_BACKEND_DEFINES LX_HAS_OPENGL=1)
    if(UNIX AND NOT APPLE)
        # dlopen/dlsym for the runtime GL loader.
        list(APPEND LX_BACKEND_LIBS ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "  OpenGL backend ... enabled (runtime-loaded, GL 3.3 core)")
else()
    message(STATUS "  OpenGL backend ... disabled")
endif()

# --- Direct3D 11 backend (Windows) ---
#
# D3D11 is the graphics API the Windows target prefers when Vulkan is absent.
# It is built only when the D3D11 headers and import libraries are available,
# which is the case for both MSVC and the mingw-w64 cross toolchain used here.
if(LX_ENABLE_D3D11 AND WIN32)
    find_path(LX_D3D11_INCLUDE_DIR d3d11.h)
    find_library(LX_D3D11_LIBRARY d3d11)
    find_library(LX_DXGI_LIBRARY dxgi)
    find_library(LX_D3DCOMPILER_LIBRARY d3dcompiler)
    if(LX_D3D11_INCLUDE_DIR AND LX_D3D11_LIBRARY AND LX_DXGI_LIBRARY)
        list(APPEND LX_BACKEND_SOURCES
            d3d11/d3d11_backend.cpp
            d3d11/d3d11_registration.cpp
        )
        list(APPEND LX_BACKEND_LIBS ${LX_D3D11_LIBRARY} ${LX_DXGI_LIBRARY})
        if(LX_D3DCOMPILER_LIBRARY)
            list(APPEND LX_BACKEND_LIBS ${LX_D3DCOMPILER_LIBRARY})
        endif()
        list(APPEND LX_BACKEND_DEFINES LX_HAS_D3D11=1)
        message(STATUS "  Direct3D 11 backend ... enabled (${LX_D3D11_LIBRARY})")
    else()
        message(STATUS "  Direct3D 11 backend ... headers or libraries not found; skipped")
    endif()
elseif(LX_ENABLE_D3D11)
    message(STATUS "  Direct3D 11 backend ... requested but this is not Windows; skipped")
endif()

# --- Metal backend (macOS / Apple silicon) ---
#
# Metal is the only low-level graphics API on modern macOS, so this is the real
# backend for Apple hardware. It is user-space only: no kernel extension and no
# privileged component, which is what the platform permits. It is built only on
# Apple platforms and is marked Experimental (see the header).
if(LX_ENABLE_METAL AND APPLE)
    enable_language(OBJCXX)
    list(APPEND LX_BACKEND_SOURCES
        metal/metal_backend.mm
        metal/metal_registration.cpp
    )
    set_source_files_properties(metal/metal_backend.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    list(APPEND LX_BACKEND_LIBS "-framework Foundation" "-framework Metal")
    list(APPEND LX_BACKEND_DEFINES LX_HAS_METAL=1)
    message(STATUS "  Metal backend .... enabled (Metal.framework)")
elseif(LX_ENABLE_METAL)
    message(STATUS "  Metal backend .... requested but this is not an Apple platform; skipped")
endif()

# --- Vulkan backend (optional) ---
#
# Vulkan is the project's preferred cross-platform GPU API, but it is opt-in:
# the primary target machine has no Vulkan device, and the build must stay
# usable without it. Enabling it needs the Vulkan headers and loader, and it
# uses shaderc to compile GLSL to SPIR-V at runtime when shaderc is present.
# Without shaderc the backend still builds and works, but only with pre-compiled
# SPIR-V passed to register_shader().
if(LX_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    # With a real SDK, the loader path comes from FindVulkan. Without one it is
    # discovered below. Both paths must end up setting LX_VULKAN_LOADER, because
    # the ICD tests use it to decide whether to link their own loader probe.
    if(Vulkan_FOUND AND Vulkan_LIBRARY)
        set(LX_VULKAN_LOADER "${Vulkan_LIBRARY}")
    endif()
    # A machine can have the loader but no -dev package (no libvulkan.so symlink,
    # no headers). The vendored headers plus a runtime-loaded loader make the
    # backend buildable there, so treat "headers available" as sufficient and
    # remember that linking must go through the loader at runtime instead.
    if(NOT Vulkan_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/vulkan/include/vulkan/vulkan.h")
        set(Vulkan_FOUND TRUE)
        set(Vulkan_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/vulkan/include)
        set(LX_VULKAN_LOAD_AT_RUNTIME ON)
        # Names first, then the versioned soname, which is all a runtime-only
        # install (no -dev package) provides on Linux.
        find_library(LX_VULKAN_LOADER NAMES vulkan vulkan-1)
        if(NOT LX_VULKAN_LOADER)
            # The loader may only be present under its versioned soname, which is
            # all a runtime-only install provides on Linux.
            foreach(_dir /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /usr/local/lib)
                if(NOT LX_VULKAN_LOADER AND EXISTS "${_dir}/libvulkan.so.1")
                    set(LX_VULKAN_LOADER "${_dir}/libvulkan.so.1")
                endif()
            endforeach()
        endif()
        if(LX_VULKAN_LOADER)
            set(Vulkan_LIBRARIES ${LX_VULKAN_LOADER})
            message(STATUS "  Vulkan ........... loader found at ${LX_VULKAN_LOADER}, headers vendored")
        else()
            message(STATUS "  Vulkan ........... headers vendored, loader resolved at runtime")
        endif()
    endif()
    if(Vulkan_FOUND)
        list(APPEND LX_BACKEND_SOURCES
            vulkan/vulkan_backend.cpp
            vulkan/vulkan_registration.cpp
            vulkan/vk_shader_compiler.cpp
        )
        if(Vulkan_LIBRARIES)
            list(APPEND LX_BACKEND_LIBS ${Vulkan_LIBRARIES})
        endif()
        # Tests need the loader path to link their own probe against the ICD.
        set(LX_VULKAN_LOADER "${LX_VULKAN_LOADER}" CACHE INTERNAL "Vulkan loader library path")
        list(APPEND LX_BACKEND_DEFINES LX_HAS_VULKAN=1)

        # Vulkan headers: prefer the system SDK, fall back to the vendored copy so
        # the build works on a machine without a Vulkan SDK installed. Only the C
        # headers are vendored (they are the stable ABI); the C++ ones are not.
        if(NOT Vulkan_INCLUDE_DIRS)
            set(Vulkan_INCLUDE_DIRS ${Vulkan_INCLUDE_DIR})
        endif()
        if(NOT EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.h")
            set(Vulkan_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/vulkan/include)
            message(STATUS "  Vulkan headers ... using vendored copy in third_party/vulkan")
        endif()
        list(APPEND LX_BACKEND_INCLUDES ${Vulkan_INCLUDE_DIRS})

        find_path(LX_SHADERC_INCLUDE_DIR shaderc/shaderc.h)
        find_library(LX_SHADERC_LIBRARY shaderc)
        if(LX_SHADERC_INCLUDE_DIR AND LX_SHADERC_LIBRARY)
            list(APPEND LX_BACKEND_LIBS ${LX_SHADERC_LIBRARY})
            list(APPEND LX_BACKEND_DEFINES LX_HAS_SHADERC=1)
            message(STATUS "  Vulkan backend ... enabled (${Vulkan_LIBRARIES}); shaderc GLSL->SPIR-V on")
        else()
            message(STATUS "  Vulkan backend ... enabled (${Vulkan_LIBRARIES}); no shaderc, SPIR-V bytecode only")
        endif()
    else()
        message(STATUS "  Vulkan backend ... requested but Vulkan not found; skipped")
        set(LX_ENABLE_VULKAN OFF)
    endif()
else()
    message(STATUS "  Vulkan backend ... disabled")
endif()

add_library(lx_backends STATIC ${LX_BACKEND_SOURCES})
target_include_directories(lx_backends PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
                                          PRIVATE ${LX_BACKEND_INCLUDES})
target_link_libraries(lx_backends PUBLIC lx_core lx_hardware ${LX_BACKEND_LIBS})

if(LX_BACKEND_DEFINES)
    target_compile_definitions(lx_backends PUBLIC ${LX_BACKEND_DEFINES})
endif()

# --- Vulkan ICD: the virtual adapter itself (opt-in, needs Vulkan headers) ---
#
# A separate shared library, not part of lx_backends: the Vulkan loader dlopen()s
# it and calls its exported vk_icd* entry points directly. It has no link-time
# dependency on the rest of the project, which is what keeps it loadable by any
# Vulkan application.
if(LX_ENABLE_VULKAN AND LX_BUILD_ICD)
    add_library(lxc_icd SHARED vulkan/icd/lxc_icd.cpp)
    target_include_directories(lxc_icd PRIVATE ${Vulkan_INCLUDE_DIRS})
    set_target_properties(lxc_icd PROPERTIES
        OUTPUT_NAME "lxc_icd"
        POSITION_INDEPENDENT_CODE ON)
    if(WIN32)
        set_target_properties(lxc_icd PROPERTIES SUFFIX ".dll")
    endif()
    message(STATUS "  Vulkan ICD ...... enabled (liblxc_icd${CMAKE_SHARED_LIBRARY_SUFFIX})")
else()
    message(STATUS "  Vulkan ICD ...... disabled")
endif()
