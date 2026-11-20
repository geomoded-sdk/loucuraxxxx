#include "shaders/shader_library.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/files.hpp"
#include "core/json.hpp"
#include "core/log.hpp"

namespace lx::shaders {

namespace {

ShaderStage parse_stage(const std::string& text) {
    if (text == "vertex") return ShaderStage::Vertex;
    if (text == "fragment") return ShaderStage::Fragment;
    return ShaderStage::Compute;
}

Maturity parse_maturity(const std::string& text) {
    if (text == "experimental") return Maturity::Experimental;
    if (text == "planned") return Maturity::Planned;
    return Maturity::Implemented;
}

// Reads a raw .spv file into 32-bit words.
Result<std::vector<u32>> read_spirv(const std::string& path) {
    auto bytes = read_file(path);
    if (!bytes.ok()) return bytes.status();
    const std::string& data = bytes.value();
    if (data.size() % 4 != 0) {
        return Status::error(ErrorCode::ParseError, "SPIR-V size is not a multiple of 4");
    }
    std::vector<u32> words(data.size() / 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<u32>(static_cast<unsigned char>(data[i * 4])) |
                   (static_cast<u32>(static_cast<unsigned char>(data[i * 4 + 1])) << 8) |
                   (static_cast<u32>(static_cast<unsigned char>(data[i * 4 + 2])) << 16) |
                   (static_cast<u32>(static_cast<unsigned char>(data[i * 4 + 3])) << 24);
    }
    // Validate the SPIR-V magic number (0x07230203, little endian).
    if (words.empty() || words[0] != 0x07230203u) {
        return Status::error(ErrorCode::ParseError, "not a SPIR-V module: " + path);
    }
    return words;
}

bool tool_on_path(const std::string& tool) {
    const std::string command = "command -v " + tool + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

}  // namespace

Result<std::vector<u32>> compile_glsl_to_spirv(const std::string& glsl_path,
                                               ShaderStage stage,
                                               const std::string& entry_point) {
    if (!tool_on_path("glslangValidator")) {
        return Status::error(ErrorCode::NotImplemented,
                             "glslangValidator is not available on PATH");
    }
    if (!file_exists(glsl_path)) {
        return Status::error(ErrorCode::IoError, "shader source not found: " + glsl_path);
    }

    // Compile to a temporary .spv next to the source, then read it back.
    const std::string out_path = glsl_path + ".spv";
    std::string stage_flag;
    switch (stage) {
        case ShaderStage::Vertex: stage_flag = "-V"; break;
        case ShaderStage::Fragment: stage_flag = "-V"; break;
        case ShaderStage::Compute: stage_flag = "-V"; break;
    }
    const std::string command = "glslangValidator " + stage_flag + " -o " + out_path +
                                " " + glsl_path + " >/dev/null 2>&1";
    if (std::system(command.c_str()) != 0) {
        return Status::error(ErrorCode::ParseError,
                             "glslangValidator failed for " + glsl_path);
    }
    (void)entry_point;  // glslang emits all entry points in the module
    auto words = read_spirv(out_path);
    std::remove(out_path.c_str());
    return words;
}

Status ShaderLibrary::load(const std::string& asset_root) {
    const std::string manifest_path = asset_root + "/manifest.json";
    auto text = read_file(manifest_path);
    if (!text.ok()) return text.status();

    auto parsed = Json::parse(text.value());
    if (!parsed.ok()) return parsed.status();
    const Json& root = parsed.value();
    if (!root.is_object() || !root.has("shaders")) {
        return Status::error(ErrorCode::ParseError,
                             "shader manifest must contain a 'shaders' array");
    }
    const Json& shaders = root.at("shaders");
    if (!shaders.is_array()) {
        return Status::error(ErrorCode::ParseError, "'shaders' must be an array");
    }

    for (const Json& item : shaders.items()) {
        ShaderModule module;
        module.name = item.at("name").as_string();
        if (module.name.empty()) {
            return Status::error(ErrorCode::ParseError, "shader entry missing 'name'");
        }
        module.stage = parse_stage(item.at("stage").as_string("compute"));
        module.entry_point = item.at("entry_point").as_string("main");
        module.source_file = item.at("file").as_string();
        module.cpu_kernel = item.at("cpu_kernel").as_string();
        // A negative or oversized manifest value must not wrap into a huge
        // push-constant block; clamp it into the u32 range.
        const long long push_bytes = item.at("push_constant_bytes").as_int(0);
        module.push_constant_bytes =
            push_bytes < 0 ? 0u
                           : (push_bytes > 0xFFFFFFFFll ? 0xFFFFFFFFu
                                                        : static_cast<u32>(push_bytes));
        module.maturity = parse_maturity(item.at("maturity").as_string("implemented"));

        // Prefer precompiled bytecode under <asset_root>/compiled/<name>.spv.
        const std::string spv_path = asset_root + "/compiled/" + module.name + ".spv";
        if (file_exists(spv_path)) {
            auto words = read_spirv(spv_path);
            if (words.ok()) {
                module.spirv = std::move(words.value());
                module.has_bytecode = true;
            } else {
                LX_WARN << "failed to read compiled shader " << spv_path << ": "
                        << words.status().message();
            }
        }
        add_module(std::move(module));
    }
    LX_INFO << "shader library loaded " << modules_.size() << " modules ("
            << compiled_module_names().size() << " with bytecode)";
    return Status::success();
}

void ShaderLibrary::add_module(ShaderModule module) {
    auto it = index_.find(module.name);
    if (it != index_.end()) {
        modules_[it->second] = std::move(module);
        return;
    }
    index_[module.name] = modules_.size();
    modules_.push_back(std::move(module));
}

const ShaderModule* ShaderLibrary::find(const std::string& name) const {
    auto it = index_.find(name);
    return it == index_.end() ? nullptr : &modules_[it->second];
}

std::vector<std::string> ShaderLibrary::compiled_module_names() const {
    std::vector<std::string> names;
    for (const auto& module : modules_) {
        if (module.has_bytecode) names.push_back(module.name);
    }
    return names;
}

}  // namespace lx::shaders
