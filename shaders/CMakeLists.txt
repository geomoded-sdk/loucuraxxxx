# Shader library + optional GLSL -> SPIR-V compilation.
#
# The library always builds. When glslangValidator is on PATH, shaders are
# compiled to SPIR-V at build time into <build>/shaders/compiled/; otherwise the
# build succeeds and the library simply reports that no bytecode is available,
# leaving the CPU kernels to cover every pass.

add_library(lx_shaders STATIC
    shader_library.cpp
)
target_include_directories(lx_shaders PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lx_shaders PUBLIC lx_core)

# Stage the shader assets (manifest + sources) next to the build for runtime use.
set(LX_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
add_custom_command(
    OUTPUT ${LX_SHADER_OUTPUT_DIR}/manifest.json
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LX_SHADER_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
            ${LX_SHADER_OUTPUT_DIR}/manifest.json
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    COMMENT "Staging shader manifest"
)
add_custom_target(lx_shader_assets ALL
    DEPENDS ${LX_SHADER_OUTPUT_DIR}/manifest.json)

find_program(GLSLANG_VALIDATOR glslangValidator)
if(GLSLANG_VALIDATOR)
    set(LX_SPV_DIR ${LX_SHADER_OUTPUT_DIR}/compiled)
    set(LX_SPV_OUTPUTS "")
    file(GLOB LX_GLSL_FILES ${CMAKE_CURRENT_SOURCE_DIR}/glsl/*.comp
                            ${CMAKE_CURRENT_SOURCE_DIR}/glsl/*.vert
                            ${CMAKE_CURRENT_SOURCE_DIR}/glsl/*.frag)
    foreach(glsl ${LX_GLSL_FILES})
        get_filename_component(base ${glsl} NAME_WE)
        set(spv ${LX_SPV_DIR}/${base}.spv)
        add_custom_command(
            OUTPUT ${spv}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${LX_SPV_DIR}
            COMMAND ${GLSLANG_VALIDATOR} -V -o ${spv} ${glsl}
            DEPENDS ${glsl}
            COMMENT "Compiling shader ${base} to SPIR-V"
        )
        list(APPEND LX_SPV_OUTPUTS ${spv})
    endforeach()
    add_custom_target(lx_shaders_spirv ALL DEPENDS ${LX_SPV_OUTPUTS})
    add_dependencies(lx_shader_assets lx_shaders_spirv)
    message(STATUS "  Shaders .......... GLSL -> SPIR-V enabled")
else()
    message(STATUS "  Shaders .......... glslangValidator not found; bytecode disabled")
endif()
