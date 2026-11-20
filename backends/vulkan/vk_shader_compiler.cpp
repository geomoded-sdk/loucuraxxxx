// LoucuraXXXX GPU - GLSL to SPIR-V compilation, shaderc-backed.
#include "backends/vulkan/vk_shader_compiler.hpp"

#if defined(LX_HAS_SHADERC)
#include <shaderc/shaderc.h>
#endif

namespace lx::backends::vk {

#if defined(LX_HAS_SHADERC)

namespace {
shaderc_shader_kind to_kind(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return shaderc_vertex_shader;
        case ShaderStage::Fragment: return shaderc_fragment_shader;
        case ShaderStage::Compute: return shaderc_compute_shader;
    }
    return shaderc_fragment_shader;
}
}  // namespace

bool compiler_available() { return true; }

bool compile_glsl(const std::string& source, ShaderStage stage, const std::string& name,
                  std::vector<u32>& spirv, std::string& error) {
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    if (compiler == nullptr) {
        error = "shaderc compiler initialization failed";
        return false;
    }
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    // The built-in passes use Vulkan GLSL (set/binding layouts, GLSL 450).
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_0);
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);

    shaderc_compilation_result_t result = shaderc_compile_into_spv(
        compiler, source.c_str(), source.size(), to_kind(stage), name.c_str(), "main", options);

    bool ok = false;
    if (result != nullptr) {
        if (shaderc_result_get_compilation_status(result) == shaderc_compilation_status_success) {
            const auto* bytes = reinterpret_cast<const u8*>(shaderc_result_get_bytes(result));
            const std::size_t size = shaderc_result_get_length(result);
            spirv.assign(reinterpret_cast<const u32*>(bytes),
                         reinterpret_cast<const u32*>(bytes + size));
            ok = true;
        } else {
            error = shaderc_result_get_error_message(result);
        }
        shaderc_result_release(result);
    } else {
        error = "shaderc returned no result";
    }

    shaderc_compile_options_release(options);
    shaderc_compiler_release(compiler);
    return ok;
}

#else  // no shaderc

bool compiler_available() { return false; }

bool compile_glsl(const std::string&, ShaderStage, const std::string&, std::vector<u32>&,
                  std::string& error) {
    error = "built without shaderc: GLSL cannot be compiled to SPIR-V at runtime";
    return false;
}

#endif

}  // namespace lx::backends::vk
