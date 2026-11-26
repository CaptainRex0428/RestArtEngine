#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }

// Progressive convergence and realtime raster-consumer evidence use actual GPU
// images. All geometry/materials live in memory; source and size are restored.
void runGlobalIlluminationSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory);


namespace rae::renderer { struct SceneSettings; }
namespace rae::scene { class World; }
namespace rae::assets { class AssetLibrary; }

void runRasterGiSmoke(rae::renderer::SceneRenderer& renderer, rae::renderer::SceneSettings settings,
    const rae::scene::World& world, const rae::assets::AssetLibrary& assets,
    const std::filesystem::path& output);

void runSoftwareGiSmoke(rae::renderer::SceneRenderer& renderer, rae::renderer::SceneSettings settings,
    const rae::scene::World& world, const rae::assets::AssetLibrary& assets,
    const std::filesystem::path& output);
