#pragma once

#include <array>
#include <cstdint>

namespace rae::scene { class World; }
namespace rae::renderer {

// Shared CPU / shader contract. Sky lights are accumulated separately and do
// not consume this budget. Additional visible direct lights remain authored.
inline constexpr std::uint32_t maxSceneLights = 32;
struct alignas(16) GpuLight {
    std::array<float, 4> positionType{};
    std::array<float, 4> directionRange{};
    std::array<float, 4> colorIntensity{};
    std::array<float, 4> axisUWidth{};
    std::array<float, 4> axisVHeight{};
    std::array<float, 4> spotShadow{};
    bool operator==(const GpuLight&) const = default;
};
struct alignas(16) LightingConstants {
    // direct light count, authored/override mode, shadow-map light index,
    // visible direct lights excluded by the fixed GPU budget.
    std::array<float, 4> info{};
    // Accumulated linear sky tint * intensity; w distinguishes actor-authored
    // lighting from the legacy scene override (whose rectangle keeps PT MIS).
    std::array<float, 4> sky{};
    std::array<GpuLight, maxSceneLights> lights{};
    bool operator==(const LightingConstants&) const = default;
};
static_assert(sizeof(GpuLight) == 96);
static_assert(sizeof(LightingConstants) == 3104);

// Stable actor order determines the fixed GPU budget and first shadow-map sun.
// A hidden light still selects authored lighting, preventing legacy light from
// reappearing when the last light is hidden. Transform scale is ignored.
LightingConstants gatherLighting(const scene::World& world, float legacyIntensity);

} // namespace rae::renderer
