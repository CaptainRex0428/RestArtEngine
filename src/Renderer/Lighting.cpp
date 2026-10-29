#include <RestArtEngine/Renderer/Lighting.h>
#include <RestArtEngine/Scene/World.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace rae::renderer {
namespace {
using namespace DirectX;
float nonnegative(float value) { return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f; }
void store(std::array<float, 4>& output, FXMVECTOR vector, float w) {
    XMFLOAT3 value; XMStoreFloat3(&value, vector);
    output = {value.x, value.y, value.z, w};
}
}

LightingConstants gatherLighting(const scene::World& world, float legacyIntensity) {
    LightingConstants result{};
    result.info[2] = -1;
    const bool authored = std::any_of(world.actors.begin(), world.actors.end(),
        [](const auto& actor) { return actor.light.has_value(); });
    result.info[1] = authored || world.environment.overrideLighting ? 1.0f : 0.0f;
    result.sky[3] = authored ? 1.0f : 0.0f;
    const auto append = [&](const GpuLight& light) {
        if (result.info[0] >= maxSceneLights) { result.info[3] += 1; return; }
        const auto index = static_cast<std::size_t>(result.info[0]++);
        result.lights[index] = light;
        if (result.info[2] < 0 && light.positionType[3] == 0 && light.spotShadow[2] > 0 && light.colorIntensity[3] > 0)
            result.info[2] = static_cast<float>(index);
    };
    if (!authored) {
        result.sky = {1, 1, 1, 0};
        if (world.environment.overrideLighting)
            for (int i = 0; i < 3; ++i) result.sky[i] = nonnegative(world.environment.environmentIntensity);
        GpuLight sun{};
        store(sun.directionRange, XMVector3Normalize(XMVectorSet(3, -6, 2, 0)), 0);
        sun.colorIntensity = {1, .87f, .72f, nonnegative(world.environment.overrideLighting
            ? world.environment.directionalIntensity : legacyIntensity)};
        sun.spotShadow[2] = 1;
        append(sun);
        const auto& rect = world.environment.areaLight;
        if (world.environment.overrideLighting && rect.enabled) {
            GpuLight light{};
            light.positionType = {rect.center[0], rect.center[1], rect.center[2], 2};
            const auto u = XMVectorSet(rect.halfAxisU[0], rect.halfAxisU[1], rect.halfAxisU[2], 0);
            const auto v = XMVectorSet(rect.halfAxisV[0], rect.halfAxisV[1], rect.halfAxisV[2], 0);
            store(light.directionRange, XMVector3Normalize(XMVector3Cross(u, v)), 0);
            store(light.axisUWidth, XMVector3Normalize(u), XMVectorGetX(XMVector3Length(u)) * 2);
            store(light.axisVHeight, XMVector3Normalize(v), XMVectorGetX(XMVector3Length(v)) * 2);
            light.colorIntensity = {nonnegative(rect.radiance[0]), nonnegative(rect.radiance[1]), nonnegative(rect.radiance[2]), 1};
            light.spotShadow[2] = 1;
            append(light);
        }
        return result;
    }
    for (const auto& actor : world.actors) {
        if (!actor.light || !actor.visible) continue;
        const auto& source = *actor.light;
        if (source.type == scene::LightType::Sky) {
            for (int i = 0; i < 3; ++i) result.sky[i] += nonnegative(source.color[i]) * nonnegative(source.intensity);
            continue;
        }
        bool finite = true;
        for (int i = 0; i < 3; ++i) finite &= std::isfinite(actor.transform.position[i]) && std::isfinite(actor.transform.rotation[i]);
        if (!finite) continue;
        const auto rotation = XMMatrixRotationRollPitchYaw(XMConvertToRadians(actor.transform.rotation[0]),
            XMConvertToRadians(actor.transform.rotation[1]), XMConvertToRadians(actor.transform.rotation[2]));
        GpuLight light{};
        light.positionType = {actor.transform.position[0], actor.transform.position[1], actor.transform.position[2], static_cast<float>(source.type)};
        store(light.directionRange, XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), rotation), nonnegative(source.range));
        light.colorIntensity = {nonnegative(source.color[0]), nonnegative(source.color[1]), nonnegative(source.color[2]), nonnegative(source.intensity)};
        store(light.axisUWidth, XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), rotation), nonnegative(source.width));
        store(light.axisVHeight, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rotation), nonnegative(source.height));
        const float outer = std::clamp(nonnegative(source.outerConeAngle), 0.0f, 89.99f);
        const float inner = std::clamp(nonnegative(source.innerConeAngle), 0.0f, outer);
        light.spotShadow = {std::cos(XMConvertToRadians(inner)), std::cos(XMConvertToRadians(outer)), source.castShadows ? 1.0f : 0.0f, 0};
        append(light);
    }
    return result;
}
} // namespace rae::renderer
