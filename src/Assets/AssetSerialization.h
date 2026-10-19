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
inline void validateMaterial(const MaterialAsset& material) {
    readId(Json(material.id));
    if (material.name.empty()) throw std::runtime_error("Material name cannot be empty.");
    for (float value : material.baseColor)
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::runtime_error("Base color must be finite and between zero and one.");
    if (!std::isfinite(material.roughness) || material.roughness < 0 || material.roughness > 1 ||
        !std::isfinite(material.metallic) || material.metallic < 0 || material.metallic > 1)
        throw std::runtime_error("Roughness and metallic must be finite and between zero and one.");
}
inline Json materialJson(const MaterialAsset& material) {
    validateMaterial(material);
    return {{"id",material.id}, {"name",material.name},
        {"baseColor",material.baseColor}, {"roughness",material.roughness}, {"metallic",material.metallic},
        {"grid",material.grid}};
}
inline MaterialAsset readMaterial(const Json& value) {
    MaterialAsset result;
    result.id = readId(value.at("id"));
    result.name = value.at("name").get<std::string>();
    readVector(value.at("baseColor"), result.baseColor);
    result.roughness = value.at("roughness").get<float>();
    result.metallic = value.at("metallic").get<float>();
    result.grid = value.value("grid", false);
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
