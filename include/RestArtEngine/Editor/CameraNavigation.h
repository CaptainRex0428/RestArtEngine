#pragma once
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <algorithm>
#include <cmath>

namespace rae::editor {
struct NavigationInput {
    float orbitX = 0, orbitY = 0, panX = 0, panY = 0, dolly = 0;
    // Local travel in world units; forward is local -Z.
    float right = 0, up = 0, forward = 0;
    bool fly = false;
};
inline void applyNavigation(renderer::SceneSettings& settings, const NavigationInput& input, float viewportHeight) {
    const bool retainPose = input.fly || (settings.cameraPose.has_value() && input.orbitX == 0 && input.orbitY == 0);
    constexpr float radians = .017453292519943295f;
    auto current = renderer::resolveCamera(settings);
    auto evaluated = camera::evaluate(current, 1);
    const float distance = std::clamp(settings.cameraDistance, .01f, 1000000.0f);
    if (input.fly) {
        if (input.orbitY != 0) current.pose.rotation[0] = std::clamp(current.pose.rotation[0] - input.orbitY * .45836624f, -89.9f, 89.9f);
        if (input.orbitX != 0) current.pose.rotation[1] -= input.orbitX * .45836624f;
        evaluated = camera::evaluate(current, 1);
    } else if (input.orbitX != 0 || input.orbitY != 0) {
        if (settings.cameraPose) {
            settings.cameraYaw = current.pose.rotation[1] * radians;
            settings.cameraPitch = -current.pose.rotation[0] * radians;
            for (int i = 0; i < 3; ++i) settings.cameraTarget[i] = current.pose.position[i] + evaluated.forward[i] * distance;
        }
        settings.cameraYaw -= input.orbitX * .008f;
        settings.cameraPitch = std::clamp(settings.cameraPitch + input.orbitY * .008f,
            renderer::SceneSettings::minPitch, renderer::SceneSettings::maxPitch);
        current = camera::orbitCamera(settings.cameraYaw, settings.cameraPitch, distance, settings.cameraTarget, current.lens);
        evaluated = camera::evaluate(current, 1);
    }
    const float height = current.lens.projection == camera::Projection::Orthographic ? current.lens.orthographicHeight :
        2 * distance * std::tan(current.lens.verticalFovDegrees * radians * .5f);
    const float perPixel = height / std::max(1.0f, viewportHeight);
    for (int i = 0; i < 3; ++i) {
        const float delta = evaluated.right[i] * (input.right - input.panX * perPixel) +
            evaluated.up[i] * (input.up + input.panY * perPixel) + evaluated.forward[i] * input.forward;
        current.pose.position[i] += delta;
        settings.cameraTarget[i] += delta;
    }
    if (input.dolly != 0) {
        if (current.lens.projection == camera::Projection::Orthographic)
            current.lens.orthographicHeight = std::clamp(current.lens.orthographicHeight * std::pow(.88f, input.dolly), .001f, 1000000.0f);
        else {
            const float next = std::clamp(distance * std::pow(.88f, input.dolly), .01f, 1000000.0f);
            for (int i = 0; i < 3; ++i) current.pose.position[i] += evaluated.forward[i] * (distance - next);
            settings.cameraDistance = next;
        }
    }
    settings.cameraLens = current.lens;
    if (retainPose) settings.cameraPose = current.pose;
    else settings.cameraPose.reset();
}
} // namespace rae::editor
