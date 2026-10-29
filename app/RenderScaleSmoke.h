#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }

// An explicit, heavyweight acceptance run: ten million unique nondegenerate
// triangles inside the camera frustum, DXR and realtime GI at 1920x1080.
// Writes evidence without modifying project assets. Unavailable hardware is
// reported as a failed/unexercised requirement, never a successful skip.
void runRenderScaleSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory);
