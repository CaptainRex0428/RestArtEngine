#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }

// In-memory raster edges, GPU supersampling reference and temporal lifecycle.
// The caller serializes execution and chooses the actual graphics backend.
void runAntiAliasingSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory);
