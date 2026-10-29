#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }

// In-memory light fixtures, actual GPU readback and explicit hardware skips.
// The caller serializes GPU test execution; no project assets are loaded/saved.
void runLightingSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory);
