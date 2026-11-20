// LoucuraXXXX GPU - shader library.
//
// Holds shader modules and their metadata. Bytecode is loaded from the build
// output (compiled from GLSL by glslangValidator) when present. The library is
// explicit about whether a module has real bytecode: a shader whose SPIR-V was
// not compiled is reported as such, never silently substituted.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "core/types.hpp"

namespace lx::shaders {

enum class ShaderStage { Vertex, Fragment, Compute };

inline const char* to_string(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex: return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Compute: return "compute";
    }
    return "unknown";
}

struct ShaderModule {
    std::string name;
    ShaderStage stage = ShaderStage::Compute;
    std::string entry_point = "main";
    std::string source_file;             // GLSL path relative to the repo
    std::string cpu_kernel;              // matching CPU kernel name, if any
    u32 push_constant_bytes = 0;
    Maturity maturity = Maturity::Implemented;
    std::vector<u32> spirv;              // empty when not compiled
    bool has_bytecode = false;
};

// Loads the manifest that ships with the build. `asset_root` is the directory
// containing shaders/manifest.json and the compiled .spv files.
class ShaderLibrary {
public:
    // Parses the manifest and loads any compiled bytecode found under
    // `asset_root`. Missing bytecode is not an error: the CPU kernels still
    // cover the pass, and GPU backends will report the module as unavailable.
    Status load(const std::string& asset_root);

    // Registers a module directly, bypassing the manifest. Used by tests.
    void add_module(ShaderModule module);

    const ShaderModule* find(const std::string& name) const;
    const std::vector<ShaderModule>& modules() const { return modules_; }

    // Names of modules that have compiled bytecode available.
    std::vector<std::string> compiled_module_names() const;

    std::size_t size() const { return modules_.size(); }

private:
    std::vector<ShaderModule> modules_;
    std::map<std::string, std::size_t> index_;
};

// Compiles a single GLSL file to SPIR-V using glslangValidator, when it is on
// PATH. Returns NotImplemented when the tool is absent, so callers can decide
// how to proceed rather than failing hard.
Result<std::vector<u32>> compile_glsl_to_spirv(const std::string& glsl_path,
                                               ShaderStage stage,
                                               const std::string& entry_point);

}  // namespace lx::shaders
