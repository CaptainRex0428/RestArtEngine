#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }

// Progressive convergence and realtime raster-consumer evidence use actual GPU
// images. All geometry/materials live in memory; source and size are restored.
void runGlobalIlluminationSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory);
