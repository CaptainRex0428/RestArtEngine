#pragma once

#include <filesystem>

namespace rae::renderer { class SceneRenderer; }
namespace rae::imgui { class ImGuiLayer; }
namespace rae::editor { class SceneEditor; }
namespace rae::project { struct ProjectPaths; }

// Requires a throwaway project copy. Exercises real DXR dispatch, GPU readback,
// transactional shader replacement, and the project settings UI/save workflow.
void runHardwareRayTracingSmoke(rae::renderer::SceneRenderer& renderer,
    rae::imgui::ImGuiLayer& ui, rae::editor::SceneEditor& editor,
    const rae::project::ProjectPaths& project,
    const std::filesystem::path& outputDirectory);
