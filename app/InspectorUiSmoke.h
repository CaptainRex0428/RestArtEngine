#pragma once
#include <filesystem>
#include <string>

namespace rae::renderer { class SceneRenderer; }
namespace rae::imgui { class ImGuiLayer; }
namespace rae::editor { class SceneEditor; }

// Runs real ImGui widgets on an initialized editor in an isolated test project
// and layout. Uses public screenshots on either backend, never a native SRV cast.
bool runInspectorUiSmoke(rae::renderer::SceneRenderer&, rae::imgui::ImGuiLayer&,
    rae::editor::SceneEditor&, const std::filesystem::path& outputDirectory, std::string& error);
