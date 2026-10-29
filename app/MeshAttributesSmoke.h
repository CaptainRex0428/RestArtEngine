#pragma once
#include <filesystem>
namespace rae::renderer { class SceneRenderer; }
void runMeshAttributesSmoke(rae::renderer::SceneRenderer& renderer, const std::filesystem::path& outputDirectory);
