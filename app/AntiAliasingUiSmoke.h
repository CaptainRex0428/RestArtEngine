#pragma once
#include <filesystem>
#include <string>

namespace rae::renderer { class SceneRenderer; }
namespace rae::imgui { class ImGuiLayer; }
namespace rae::editor { class SceneEditor; }
namespace rae::project { struct ProjectPaths; }

// The caller supplies the editor's isolated project and independent UI layout.
bool runAntiAliasingUiSmoke(rae::renderer::SceneRenderer&, rae::imgui::ImGuiLayer&,
    rae::editor::SceneEditor&, const rae::project::ProjectPaths&,
    const std::filesystem::path& outputDirectory, std::string& error);
