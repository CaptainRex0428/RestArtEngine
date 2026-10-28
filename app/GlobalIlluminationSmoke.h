#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }

// All geometry/materials live in memory. Reads the actual GPU scene image and
// restores the renderer's source and dimensions before returning.
void runGlobalIlluminationSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory);
