#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }

// In-memory fixtures and actual GPU readback exercise depth/cull states and
// camera submission culling. The caller owns serialized GPU test scheduling.
void runMaterialRenderStateSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory);
