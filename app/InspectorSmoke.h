#pragma once

#include <filesystem>
#include <string>

namespace rae::renderer { class SceneRenderer; }

// Tests the initialized renderer's actual backend with isolated CPU assets and
// public capture APIs. Does not reinterpret native texture IDs or save a project.
bool runInspectorSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& captureDirectory, std::string& error);
