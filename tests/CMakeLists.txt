# One executable per module keeps failures attributable. Each is registered
# with CTest so `ctest` runs the whole suite.
set(LX_TEST_TARGETS
    test_core
    test_configurator
    test_memory
    test_scheduler
    test_hardware
    test_filters
    test_backends
    test_driver
    test_gpu_backends
    test_manager
    test_renderer
    test_integration
    test_gui
    test_icd
)

set(test_core_LIBS lx_core)
set(test_configurator_LIBS lx_core lx_configurator lx_hardware lx_memory)
set(test_memory_LIBS lx_core lx_memory lx_hardware)
set(test_scheduler_LIBS lx_core lx_scheduler)
set(test_hardware_LIBS lx_core lx_hardware lx_configurator lx_memory)
set(test_filters_LIBS lx_core lx_filters lx_memory lx_scheduler)
set(test_backends_LIBS lx_core lx_backends lx_hardware)
set(test_driver_LIBS lx_core lx_driver lx_backends lx_hardware lx_memory lx_configurator)
set(test_gpu_backends_LIBS
    lx_core lx_backends lx_filters lx_memory lx_scheduler lx_hardware lx_renderer lx_shaders)
set(test_manager_LIBS lx_core lx_manager lx_backends lx_memory lx_scheduler)
set(test_renderer_LIBS lx_core lx_renderer lx_filters lx_backends)
set(test_integration_LIBS
    lx_core lx_hardware lx_configurator lx_driver lx_manager lx_renderer lx_filters)
set(test_gui_LIBS lx_core lx_gui_model)
set(test_icd_LIBS lx_core lx_backends)

foreach(target ${LX_TEST_TARGETS})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${target}.cpp)
        add_executable(${target} ${target}.cpp)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(${target} PRIVATE ${${target}_LIBS})
        add_test(NAME ${target} COMMAND ${target})
        # Tests run from the build root so the shader assets are discoverable.
        set_tests_properties(${target} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        # Under ASan, apply the LeakSanitizer suppressions (see cmake/lsan.supp).
        if(LX_LSAN_OPTIONS)
            set_tests_properties(${target} PROPERTIES ENVIRONMENT "LSAN_OPTIONS=${LX_LSAN_OPTIONS}")
        endif()
    endif()
endforeach()

# --- ICD tests ------------------------------------------------------------
#
# The manifest tests are pure and always run. The loader round trip needs both
# the ICD shared library and the Vulkan loader, so it is enabled only when both
# exist; otherwise the suite still builds and the integration cases are absent
# rather than silently passing.
if(TARGET lxc_icd AND LX_VULKAN_LOADER)
    # Same header resolution as the backend: system SDK first, vendored fallback.
    if(EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.h")
        target_include_directories(test_icd PRIVATE ${Vulkan_INCLUDE_DIR})
    else()
        target_include_directories(test_icd PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/vulkan/include)
    endif()
    target_link_libraries(test_icd PRIVATE lxc_icd ${LX_VULKAN_LOADER})
    target_compile_definitions(test_icd PRIVATE
        LX_TEST_HAVE_ICD=1
        LX_TEST_ICD_MANIFEST="${CMAKE_BINARY_DIR}/tests/lxc_icd_test.json")

    # Generated at configure time from the real target path, so the manifest
    # always points at this build tree's ICD.
    set(LX_ICD_TEST_MANIFEST "${CMAKE_BINARY_DIR}/tests/lxc_icd_test.json")
    file(GENERATE OUTPUT "${LX_ICD_TEST_MANIFEST}" CONTENT
"{
  \"file_format_version\": \"1.0.0\",
  \"ICD\": {
    \"library_path\": \"$<TARGET_FILE:lxc_icd>\",
    \"api_version\": \"1.1.0\"
  }
}
")
else()
    message(STATUS "  ICD tests ....... manifest only (no ICD library or loader)")
endif()
