#pragma once

#include <optional>
#include <string_view>

namespace rae::scene {

enum class LightType { Directional, Sky, Rect, Spot, Point };

// API-independent, linear-color light data. Actor rotation aims local -Z;
// a rectangle lies in local X/Y and emits toward -Z. Transform scale does not
// change intensity, range, cone angles or the authored rectangle dimensions.
struct LightComponent {
    LightType type = LightType::Point;
    float color[3] = {1, 1, 1};
    float intensity = 10;
    float range = 10;
    float innerConeAngle = 20; // Half-angle in degrees.
    float outerConeAngle = 35; // Half-angle in degrees; equal angles form a hard edge.
    float width = 2;
    float height = 2;
    bool castShadows = true;
};

constexpr const char* lightTypeName(LightType type) {
    switch (type) {
    case LightType::Directional: return "Directional";
    case LightType::Sky: return "Sky";
    case LightType::Rect: return "Rect";
    case LightType::Spot: return "Spot";
    case LightType::Point: return "Point";
    }
    return "Unknown";
}

constexpr std::optional<LightType> parseLightType(std::string_view name) {
    for (auto type : {LightType::Directional, LightType::Sky, LightType::Rect, LightType::Spot, LightType::Point})
        if (name == lightTypeName(type)) return type;
    return std::nullopt;
}

} // namespace rae::scene
