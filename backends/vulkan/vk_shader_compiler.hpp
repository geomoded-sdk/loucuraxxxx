// LoucuraXXXX GPU - GLSL to SPIR-V compilation for the Vulkan backend.
//
// Vulkan takes only SPIR-V, so the backend needs a front end to turn the GLSL
// its built-in passes are written in into a module the driver can consume. That
// front end is shaderc when the build found it; without shaderc the backend
// still works, but only with pre-compiled bytecode passed to register_shader().
//
// This is a thin wrapper: it never throws, it reports the compiler's own error
// text on failure, and it is honest about being unavailable.
#pragma once

#include <string>
#include <vector>

#include "core/types.hpp"

namespace lx::backends::vk {

// Which shader stage `compile` is being asked for.
enum class ShaderStage { Vertex, Fragment, Compute };

// True when this build can compile GLSL to SPIR-V.
bool compiler_available();

// Compiles `source` to SPIR-V for `stage`, with `name` used in diagnostics.
// On success fills `spirv` (32-bit words) and returns true. On failure fills
// `error` with the compiler log and returns false.
bool compile_glsl(const std::string& source, ShaderStage stage, const std::string& name,
                  std::vector<u32>& spirv, std::string& error);

}  // namespace lx::backends::vk
