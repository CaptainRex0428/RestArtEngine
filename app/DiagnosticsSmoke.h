#pragma once

#include <filesystem>
#include <string>

namespace rae::renderer { class SceneRenderer; }

// GPU evidence uses local assets only; restores renderer size and active source.
void runDiagnosticsSmoke(rae::renderer::SceneRenderer& renderer,
                         const std::filesystem::path& output,
                         const std::string& standardMaterialSource);
