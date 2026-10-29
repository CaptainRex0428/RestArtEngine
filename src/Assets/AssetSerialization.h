#pragma once
#include "RestArtEngine/Assets/AssetLibrary.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace rae::assets::detail {
using Json = nlohmann::json;
inline std::string pathUtf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
inline std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline AssetId readId(const Json& value) {
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() <= 0))
        throw std::runtime_error("Asset and actor IDs must be positive integers.");
    const auto id = value.get<AssetId>();
    if (id == 0 || id == std::numeric_limits<AssetId>::max())
        throw std::runtime_error("ID is zero or exceeds the supported range.");
    return id;
}
inline void readVector(const Json& value, float (&result)[3]) {
    if (!value.is_array() || value.size() != 3) throw std::runtime_error("Expected a three-component vector.");
    for (int axis = 0; axis != 3; ++axis) {
        if (!value[axis].is_number()) throw std::runtime_error("Vector component is not a number.");
        result[axis] = value[axis].get<float>();
        if (!std::isfinite(result[axis])) throw std::runtime_error("Vector contains a non-finite component.");
    }
}
inline const char* materialCullModeName(MaterialCullMode mode) {
    switch (mode) {
    case MaterialCullMode::Off: return "off";
    case MaterialCullMode::Front: return "front";
    case MaterialCullMode::Back: return "back";
    }
    throw std::runtime_error("Invalid material cull mode.");
}
inline const char* materialDepthTestName(MaterialDepthTest test) {
    switch (test) {
    case MaterialDepthTest::Never: return "never";
    case MaterialDepthTest::Less: return "less";
    case MaterialDepthTest::Equal: return "equal";
    case MaterialDepthTest::LessEqual: return "lessEqual";
    case MaterialDepthTest::Greater: return "greater";
    case MaterialDepthTest::NotEqual: return "notEqual";
    case MaterialDepthTest::GreaterEqual: return "greaterEqual";
    case MaterialDepthTest::Always: return "always";
    case MaterialDepthTest::Disabled: return "disabled";
    }
    throw std::runtime_error("Invalid material depth test.");
}
inline void readMaterialRenderState(const Json& value, MaterialAsset& material, bool legacyTwoSided) {
    // Missing fields retain the historical defaults of each file format.
    const bool twoSided = value.value("twoSided", legacyTwoSided);
    auto cull = twoSided ? MaterialCullMode::Off : MaterialCullMode::Back;
    if (value.contains("cullMode")) {
        const auto mode = value.at("cullMode").get<std::string>();
        if (mode == "off") cull = MaterialCullMode::Off;
        else if (mode == "front") cull = MaterialCullMode::Front;
        else if (mode == "back") cull = MaterialCullMode::Back;
        else throw std::runtime_error("Material cullMode must be off, front or back.");
        if (value.contains("twoSided") && twoSided != (cull == MaterialCullMode::Off))
            throw std::runtime_error("Material cullMode conflicts with twoSided.");
    }
    material.setCullMode(cull);
    const auto depth = value.value("depthTest", std::string("less"));
    if (depth == "never") material.depthTest = MaterialDepthTest::Never;
    else if (depth == "less") material.depthTest = MaterialDepthTest::Less;
    else if (depth == "equal") material.depthTest = MaterialDepthTest::Equal;
    else if (depth == "lessEqual") material.depthTest = MaterialDepthTest::LessEqual;
    else if (depth == "greater") material.depthTest = MaterialDepthTest::Greater;
    else if (depth == "notEqual") material.depthTest = MaterialDepthTest::NotEqual;
    else if (depth == "greaterEqual") material.depthTest = MaterialDepthTest::GreaterEqual;
    else if (depth == "always") material.depthTest = MaterialDepthTest::Always;
    else if (depth == "disabled") material.depthTest = MaterialDepthTest::Disabled;
    else throw std::runtime_error("Invalid material depthTest name.");
    material.depthWrite = value.value("depthWrite", true);
}
inline void validateMaterial(const MaterialAsset& material) {
    readId(Json(material.id));
    if (material.name.empty()) throw std::runtime_error("Material name cannot be empty.");
    for (float value : material.baseColor)
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::runtime_error("Base color must be finite and between zero and one.");
    if (!std::isfinite(material.roughness) || material.roughness < 0 || material.roughness > 1 ||
        !std::isfinite(material.metallic) || material.metallic < 0 || material.metallic > 1)
        throw std::runtime_error("Roughness and metallic must be finite and between zero and one.");
    (void)materialDepthTestName(material.depthTest);
}
inline Json materialJson(const MaterialAsset& material) {
    validateMaterial(material);
    return {{"id",material.id}, {"name",material.name},
        {"baseColor",material.baseColor}, {"roughness",material.roughness}, {"metallic",material.metallic},
        {"grid",material.grid}, {"twoSided",material.twoSided},
        {"cullMode",materialCullModeName(material.effectiveCullMode())},
        {"depthTest",materialDepthTestName(material.depthTest)}, {"depthWrite",material.depthWrite}};
}
inline MaterialAsset readMaterial(const Json& value) {
    MaterialAsset result;
    result.id = readId(value.at("id"));
    result.name = value.at("name").get<std::string>();
    readVector(value.at("baseColor"), result.baseColor);
    result.roughness = value.at("roughness").get<float>();
    result.metallic = value.at("metallic").get<float>();
    result.grid = value.value("grid", false);
    // Before this option existed the renderer disabled back-face culling.
    // value<bool> also rejects non-boolean fields instead of coercing them.
    readMaterialRenderState(value, result, true);
    validateMaterial(result);
    return result;
}
inline Json readJson(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Cannot open " + pathUtf8(path));
    return Json::parse(stream);
}
inline void writeJson(const std::filesystem::path& path, const Json& value) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Cannot write " + pathUtf8(temporary));
        stream << value.dump(2) << '\n';
        stream.flush();
        if (!stream) throw std::runtime_error("Failed writing " + pathUtf8(temporary));
        stream.close();
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace " + pathUtf8(path) + ": OS error " + std::to_string(GetLastError()));
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
template<class Item>
inline AssetId nextId(const std::vector<Item>& items, AssetId& next) {
    for (const auto& item : items) {
        if (item.id >= std::numeric_limits<AssetId>::max() - 1) throw std::runtime_error("ID space exhausted.");
        if (item.id >= next) next = item.id + 1;
    }
    if (next >= std::numeric_limits<AssetId>::max() - 1) throw std::runtime_error("ID space exhausted.");
    return next++;
}
} // namespace rae::assets::detail
