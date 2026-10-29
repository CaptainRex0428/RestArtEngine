#pragma once

namespace rae::scene {

// A single-sided rectangle; cross(halfAxisU, halfAxisV) points toward its
// emitting side. Axes are nonzero, perpendicular half-extents in world space.
struct RectAreaLight {
    bool enabled = false;
    float center[3] = {0, 3, 0};
    float halfAxisU[3] = {.5f, 0, 0};
    float halfAxisV[3] = {0, 0, .5f};
    float radiance[3] = {12, 12, 12};
};

struct SceneEnvironment {
    // False retains legacy application/editor lighting settings.
    bool overrideLighting = false;
    float directionalIntensity = 5;
    float environmentIntensity = 1;
    RectAreaLight areaLight;
};

// An authored opening view, not the continuously changing editor camera.
struct SceneCamera {
    float yaw = 0, pitch = 0, distance = 6;
    float target[3] = {0, 1, 0};
    float exposure = 1;
};

} // namespace rae::scene
